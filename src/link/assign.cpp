// SPDX-License-Identifier: MIT

#include "platform.hpp"

#include "link/assign.hpp"

#include <algorithm>
#include <deque>
#include <inttypes.h>
#include <optional>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "helpers.hpp"
#include "itertools.hpp"
#include "linkdefs.hpp"
#include "verbosity.hpp"

#include "link/main.hpp"
#include "link/output.hpp"
#include "link/section.hpp"
#include "link/symbol.hpp"
#include "link/warning.hpp"

struct FreeSpace {
	uint16_t address;
	uint16_t size; // Never zero.

	uint16_t addrOnePast() const { return address + size; }
};

// Table of free space for each bank
static std::vector<std::deque<FreeSpace>> memory[SECTTYPE_INVALID];
using FreeSpaceIter = std::deque<FreeSpace>::iterator;

static std::deque<FreeSpace> &freeSpaceOfBank(Section const &section, uint32_t bank) {
	assume(bank >= section.typeInfo().firstBank);
	assume(bank <= section.typeInfo().lastBank);
	return memory[section.type][bank - section.typeInfo().firstBank];
}

struct Scrambling {
	uint16_t romxOfs = 0;
	uint16_t sramOfs = 0;
	uint16_t wramxOfs = 0;

	// Helper for the next function, to give names to its two returned values.
	struct ScramblingInfo {
		uint16_t &curOfs;
		uint16_t maxOfs;
	};
	std::optional<ScramblingInfo> getInfoFor(SectionType type) {
		switch (type) {
		case SECTTYPE_ROMX:
			return {
			    {romxOfs, options.scrambleROMX}
			};
		case SECTTYPE_SRAM:
			return {
			    {sramOfs, options.scrambleSRAM}
			};
		case SECTTYPE_WRAMX:
			return {
			    {wramxOfs, options.scrambleWRAMX}
			};

		// Non-banked sections don't need scrambling support...
		case SECTTYPE_ROM0:
		case SECTTYPE_WRAM0:
		case SECTTYPE_OAM:
		case SECTTYPE_HRAM:
			assume(!sectionTypeInfo[type].isBanked());
			return std::nullopt;
		// ...but VRAM doesn't either, regardless of whether it's banked or not.
		case SECTTYPE_VRAM:
			return std::nullopt;

		case SECTTYPE_INVALID:
			unreachable_();
		}
		return std::nullopt; // Dead code, but some compilers don't recognize that.
	}
};
static Scrambling scrambling;

struct MemoryLocation {
	uint16_t address;
	uint32_t bank;

	static MemoryLocation initFor(Section const &section) {
		MemoryLocation location;

		if (section.isAddressFixed) { // This will never change.
			location.address = section.org;
		}

		if (section.isBankFixed) {
			location.bank = section.bank;
		} else {
			location.bank = section.typeInfo().firstBank;

			if (auto info = scrambling.getInfoFor(section.type);
			    info.has_value() && info->maxOfs != 0) { // If scrambling is enabled...
				// ...then we will begin the search from a different offset for each section.
				// Go to the next offset (backwards), wrapping around. (Thus, no underflow!)
				info->curOfs = (info->curOfs != 0 ? info->curOfs : info->maxOfs) - 1;
				location.bank += info->curOfs;
			}
		}

		return location;
	}

	// Try again in the next bank, if one is available.
	[[nodiscard("This returns whether iteration can be continued")]]
	bool goToNextApplicableBankFor(Section const &section) {
		assume(bank >= section.typeInfo().firstBank);
		assume(bank <= section.typeInfo().lastBank);

		assume(!section.isBankFixed);

		// Try scrambled banks in descending order until no bank in the scrambled range is
		// available.
		if (auto info = scrambling.getInfoFor(section.type);
		    info.has_value() && info->maxOfs != 0) {
			// All floating sections within a scrambled region should be
			// within the scrambled bank pool.
			assume(bank < info->maxOfs + section.typeInfo().firstBank);

			uint16_t ofsWithinPool = bank - section.typeInfo().firstBank;
			// Go to the next bank (backwards), wrapping around. (Thus, no overflow!)
			ofsWithinPool = (ofsWithinPool != 0 ? ofsWithinPool : info->maxOfs) - 1;

			bank = ofsWithinPool + section.typeInfo().firstBank;
			// Keep iterating unless we have wrapped back around to the start offset.
			return ofsWithinPool != info->curOfs;
		}

		// Otherwise, try in ascending order.
		if (bank == section.typeInfo().lastBank) {
			return false;
		}
		++bank;
		return true;
	}

	void makeAddressAligned(uint16_t alignMask, uint16_t alignOfs) {
		// By how much the address is past the target offset within the current alignment "page".
		uint16_t offset = (address - alignOfs) & alignMask;
		// Move by one page *minus* that "overshoot" offset.
		// If it's 0, then this would move by a whole page, but `& alignMask` resets it back to 0.
		address += ((alignMask + 1) - offset) & alignMask;
	}
};

static FreeSpaceIter tryPlacingInBank(Section const &section, MemoryLocation &location) {
	std::deque<FreeSpace> &bankMem = freeSpaceOfBank(section, location.bank);

	if (section.isAddressFixed) {
		// There is only one candidate location in this bank: the address at which the section is
		// fixed.
		assume(location.address == section.org);

		FreeSpaceIter iter = std::find_if(RANGE(bankMem), [&location](FreeSpace const &freeSpace) {
			// If they both exactly match, that means the next block will begin past the requested
			// addr, so the function would fail anyway.
			return freeSpace.addrOnePast() >= location.address;
		});
		if (iter != bankMem.end()) {
			// We have the first block ending after the section's address, so all that's left is
			// checking that the address does fall into the block, and then that the section fits.
			if (location.address < iter->address
			    || location.address + section.size > iter->addrOnePast()) {
				return bankMem.end(); // Failed! Better luck next bank?
			}
		}
		return iter;

	} else {
		// There are many possible locations within the bank, so we are going to iterate on free
		// blocks. If it is impossible to fit at the earliest (constraint-satisfying) address, then
		// no other address in the block will do; thus, we make only one attempt per block.
		return std::find_if(RANGE(bankMem), [&location, &section](FreeSpace const &freeSpace) {
			location.address = freeSpace.address;
			if (section.isAlignFixed) {
				location.makeAddressAligned(section.alignMask, section.alignOfs);
				// Did it advance past the block? Or, rarely, overflowed?
				if (location.address >= freeSpace.addrOnePast()
				    || location.address < freeSpace.address) {
					return false;
				}
			}

			// Since `location.address` lies within the block,
			// we only need to check that its end address also does.
			return location.address + section.size <= freeSpace.addrOnePast();
		});
	}
}

// Place section using first-fit decreasing algorithm
// <https://en.wikipedia.org/wiki/Bin_packing_problem#First-fit_algorithm>
// Returns an iterator to within `freeSpaceOfBank(section, location.bank)`
// (guaranteeing that `location.bank` remains valid) pointing at the free block that
// the section can go into (at `location.address`).
// The iterator is an end iterator if and only if no suitable location was found.
// `location` is updated accordingly.
static FreeSpaceIter tryPlacing(Section const &section, MemoryLocation &location) {
	if (section.isBankFixed) {
		assume(location.bank == section.bank);
		return tryPlacingInBank(section, location);
	}

	do {
		if (FreeSpaceIter iter = tryPlacingInBank(section, location);
		    iter != freeSpaceOfBank(section, location.bank).end()) {
			return iter; // Found one!
		}
	} while (location.goToNextApplicableBankFor(section));
	// Return a deque's end iterator to signal failure.
	// The exact deque doesn't matter, but the caller will use `freeSpaceOfBank` also.
	return freeSpaceOfBank(section, location.bank).end();
}

static void
    updateFreeSpace(FreeSpaceIter iter, std::deque<FreeSpace> &bankMem, Section const &section) {
	assume(section.org + section.size <= UINT16_MAX);
	uint16_t sectionEnd = section.org + section.size;
	assume(section.org >= iter->address);
	assume(sectionEnd <= iter->addrOnePast());

	bool noLeftSpace = iter->address == section.org;
	bool noRightSpace = iter->address + iter->size == sectionEnd;
	if (noLeftSpace && noRightSpace) {
		// The free space is entirely deleted
		bankMem.erase(iter);
	} else if (!noLeftSpace && !noRightSpace) {
		// The free space is split in two
		uint16_t size = static_cast<uint16_t>(iter->address + iter->size - sectionEnd);
		// Resize the original space (address is unmodified)
		iter->size = section.org - iter->address;
		// Append the new space after the original one
		bankMem.insert(iter + 1, {.address = sectionEnd, .size = size});
		// `iter` cannot be reused from this point on, because `bankMem.insert`
		// invalidates iterators to itself!
	} else {
		// The amount of free spaces doesn't change: resize!
		iter->size -= section.size;
		if (noLeftSpace) {
			// The free space is moved *and* resized
			iter->address += section.size;
		}
	}
}

// Assigns a section to a given memory location
static void assignSection(Section &section, MemoryLocation const &location) {
	assume(location.address >= section.typeInfo().startAddr);
	// Zero-sized sections can start one past the end of their region.
	assume(location.address <= section.typeInfo().endAddr() + 1);
	// This one is not redundant, it guards against overflow!
	assume(location.address + section.size >= section.typeInfo().startAddr);
	assume(location.address + section.size <= section.typeInfo().endAddr() + 1);

	if (section.isAddressFixed) {
		assume(location.address == section.org);
	} else if (section.isAlignFixed) {
		assume((location.address & section.alignMask) == section.alignOfs);
	}
	if (section.isBankFixed) {
		assume(location.bank == section.bank);
	}

	// Propagate the assigned location to all UNIONs/FRAGMENTs
	// so `jr` patches in them will have the correct offset
	for (Section &piece : section.pieces()) {
		piece.org = location.address;
		piece.bank = location.bank;
		piece.isAssigned = true;
	}
	out_AddSection(section);
}

static std::string describeConstraintsOf(Section const &section) {
	std::string description = "\"" + section.name + "\" (" + section.typeInfo().name + " section) ";
	if (section.isBankFixed && section.typeInfo().isBanked()) {
		char bank[9];
		sprintf_to_array(bank, "%02" PRIx32, section.bank);
		if (section.isAddressFixed) {
			char addr[5];
			sprintf_to_array(addr, "%04" PRIx16, section.org);
			description = description + "at $" + bank + ":" + addr;
		} else if (section.isAlignFixed) {
			char mask[5];
			sprintf_to_array(mask, "%" PRIx16, static_cast<uint16_t>(~section.alignMask));
			description = description + "in bank $" + bank + " with align mask $" + mask;
		} else {
			description = description + "in bank $" + bank;
		}
	} else {
		if (section.isAddressFixed) {
			char addr[5];
			sprintf_to_array(addr, "%04" PRIx16, section.org);
			description = description + "at address $" + addr;
		} else if (section.isAlignFixed) {
			char mask[5], offset[5];
			sprintf_to_array(mask, "%" PRIx16, static_cast<uint16_t>(~section.alignMask));
			sprintf_to_array(offset, "%" PRIx16, section.alignOfs);
			description = description + "with align mask $" + mask + " and offset $" + offset;
		} else {
			description = description + "anywhere";
		}

		if (auto info = scrambling.getInfoFor(section.type);
		    info.has_value() && info->maxOfs != 0) { // Only mention scrambling if it is enabled.
			char size[6];
			sprintf_to_array(size, "%" PRIu16, info->maxOfs);
			description = description + " within the " + size + " scrambled banks";
		}
	}

	return description;
}

// Places a section in a suitable location, or error out if it fails to.
// Due to the implemented algorithm, this should be called with sections of decreasing size!
static void placeSection(Section &section) {
	MemoryLocation location = MemoryLocation::initFor(section);

	// Specially handle 0-byte SECTIONs, as they ignore free space entirely.
	if (section.size == 0) {
		if (!section.isAddressFixed) {
			location.address = section.typeInfo().startAddr;
			if (section.isAlignFixed) {
				location.makeAddressAligned(section.alignMask, section.alignOfs);
			}
		}

		// This check safely handles sections with impossible alignment and no fixed address.
		// The above `location.address = section.typeInfo().startAddr` would be valid on its own,
		// but `location.makeAddressAligned(...)` can increase `location.address` above the valid
		// range for its `section`, which would violate an assumption in `assignSection`.
		// Note that sections with fixed addresses are handled earlier by `sect_DoSanityChecks`,
		// but this check would safely handle them too if they ever reached it.
		if (location.address <= section.typeInfo().endAddr() + 1) {
			assignSection(section, location);
			return;
		}
	}

	FreeSpaceIter iter = tryPlacing(section, location);
	if (std::deque<FreeSpace> &bankMem = freeSpaceOfBank(section, location.bank);
	    iter != bankMem.end()) {
		assignSection(section, location);

		updateFreeSpace(iter, bankMem, section);
		return;
	}

	if (!section.isBankFixed || !section.isAddressFixed) {
		// If a section failed to go to several places, nothing we can report
		fatal("Unable to place %s", describeConstraintsOf(section).c_str());
	} else if (uint16_t onePastEnd = section.typeInfo().endAddr() + 1;
	           section.org + section.size > onePastEnd) {
		// If the section just can't fit the bank, report that
		fatal(
		    "Unable to place %s: section runs past end of region ($%04x > $%04x)",
		    describeConstraintsOf(section).c_str(),
		    section.org + section.size,
		    onePastEnd
		);
	} else {
		// Otherwise there is overlap with another section
		Section const *overlap = out_OverlappingSection(section);
		assume(overlap != nullptr);
		fatal(
		    "Unable to place %s: section overlaps with \"%s\"",
		    describeConstraintsOf(section).c_str(),
		    overlap->name.c_str()
		);
	}
}

static std::deque<Section *> unassignedSections;

// Bank constraints of the form `BANK = BANK(other) + N` ("bank references")
//
// Such sections cannot simply be placed by the first-fit algorithm above, since their bank is
// not their own: it follows another section's. The idea is to place the section a reference
// points to (its "leader") normally, and then fix the referencing section's ("dependent")
// bank to the leader's bank plus the reference's offset. Dependents are placed immediately
// after their leader: this way, whichever sections would compete with them for space in the
// bank cannot steal it first (which would make the dependent harder to place, for they
// *cannot* look elsewhere). Sections that transitively depend on each other are processed
// recursively, so whole chains of dependent sections can follow each other. Cycles and other
// unsatisfiable references are detected and rejected before any placement is attempted.
static std::unordered_map<Section *, std::vector<Section *>> dependentsOf;

// Cycle detection state, for DFS over bank reference edges
enum BankRelVisitState { BANK_REL_NEW, BANK_REL_ON_CHAIN, BANK_REL_DONE };
static std::unordered_map<Section *, BankRelVisitState> bankRelStates;

// Resolves the section that a dependents's bank depends on, checking that the reference can
// be satisfied at least in principle; fatals otherwise.
static Section &referentOfBankRel(Section const &section) {
	if (section.bankRel->kind == BANKREL_SYMBOL) {
		if (Symbol *symbol = sym_GetSymbol(section.bankRel->name); !symbol) {
			if (sym_HasLocalAliases(section.bankRel->name)) {
				fatalAt(
				    section,
				    "Section \"%s\"'s bank depends on symbol `%s`, but it is not exported "
				    "(labels referenced through `BANK = BANK()` must be exported)",
				    section.name.c_str(),
				    section.bankRel->name.c_str()
				);
			}
			fatalAt(
			    section,
			    "Section \"%s\"'s bank depends on undefined symbol `%s`",
			    section.name.c_str(),
			    section.bankRel->name.c_str()
			);
		} else if (Label *label = std::get_if<Label>(&symbol->data); !label) {
			fatalAt(
			    section,
			    "Section \"%s\"'s bank depends on `%s`, which is not a label",
			    section.name.c_str(),
			    section.bankRel->name.c_str()
			);
		} else if (!label->section) {
			fatalAt(
			    section,
			    "Section \"%s\"'s bank depends on label `%s`, which is not defined in any "
			    "section",
			    section.name.c_str(),
			    section.bankRel->name.c_str()
			);
		} else {
			return *label->section;
		}
	}

	if (Section *referent = sect_GetSection(section.bankRel->name); !referent) {
		fatalAt(
		    section,
		    "Section \"%s\"'s bank depends on undefined section \"%s\"",
		    section.name.c_str(),
		    section.bankRel->name.c_str()
		);
	} else {
		return *referent;
	}
}

// DFS traversal over a section's whole bank reference chain, to check that it isn't cyclic.
static void visitBankRelChain(Section &section) {
	switch (bankRelStates[&section]) {
	case BANK_REL_DONE:
		return;

	case BANK_REL_ON_CHAIN: {
		// The DFS reached a node it is currently walking, so we ran into a cycle.
		// Report it by walking the chain back around, up to the section where it closes.
		std::string cycle = "\"" + section.name + "\"";
		Section *cur = &section;
		while ((cur = cur->bankRelBase) != &section) {
			cycle += " -> \"";
			cycle += cur->name;
			cycle += "\"";
		}
		cycle += " -> \"";
		cycle += section.name;
		cycle += "\"";

		fatal("Section banks form a cycle: %s", cycle.c_str());
	}

	case BANK_REL_NEW:
		bankRelStates[&section] = BANK_REL_ON_CHAIN;
		if (section.bankRelBase) {
			visitBankRelChain(*section.bankRelBase);
		}
		bankRelStates[&section] = BANK_REL_DONE;
		return;
	}
}

// Fixes the dependent's bank to the (known) one of the section it depends on, plus the
// reference's offset.
static void fixBankRelativeTo(Section &dependent, Section const &leader) {
	SectionTypeInfo const &typeInfo = dependent.typeInfo();
	int64_t bank = static_cast<int64_t>(leader.bank) + dependent.bankRel->offset;

	// The offset was checked to be within the memory region's number of banks, but the
	// leader's bank being exactly at a region's edge can still push this section's bank out
	// of bounds; there is nothing more we can do about that than report the conflict.
	if (bank < typeInfo.firstBank || bank > typeInfo.lastBank) {
		fatalAt(
		    dependent,
		    "Section \"%s\"'s bank depends on that of \"%s\" (bank %" PRIu32 "), plus %" PRId32
		    ", which would be bank %" PRId64 ", but %s does not have one",
		    dependent.name.c_str(),
		    leader.name.c_str(),
		    leader.bank,
		    dependent.bankRel->offset,
		    bank,
		    typeInfo.name
		);
	}

	dependent.isBankFixed = true;
	dependent.bank = static_cast<uint32_t>(bank);
	dependent.bankRelBase = nullptr; // Its bank no longer needs to follow anyone's
}

// Fixes the section's bank to its chain leader's, if that one's bank is already known (or
// transitively can be made known).
static void foldBankRelIfKnown(Section &section) {
	Section *leader = section.bankRelBase;
	if (!leader) {
		return;
	}

	foldBankRelIfKnown(*leader);
	if (leader->isBankFixed) {
		fixBankRelativeTo(section, *leader);
	}
}

// Resolves all bank references: checks that they are well-formed and satisfiable, folds the
// bank of any dependent whose leader's bank is already fixed, and registers the rest so that
// their placement can follow their leader's.
static void resolveBankRels() {
	for (Section *section : unassignedSections) {
		if (section->bankRel && section->bankRel->kind != BANKREL_NONE) {
			SectionBankRel const &rel = *section->bankRel;

			section->bankRelBase = &referentOfBankRel(*section);

			// Accounts for sections in different types of memories referencing each other.
			if (section->bankRelBase->type != section->type) {
				fatalTwoAt(
				    *section,
				    *section->bankRelBase,
				    "Section \"%s\" is a %s section, so its bank cannot depend on that of "
				    "\"%s\", which is a %s section",
				    section->name.c_str(),
				    section->typeInfo().name,
				    section->bankRelBase->name.c_str(),
				    section->bankRelBase->typeInfo().name
				);
			}

			// Detects unconditionally impossible offsets, e.g. `BANK(other) + 999999`: even
			// with the leader's bank at either end of the range, the dependent's bank would
			// never land in range.
			SectionTypeInfo const &typeInfo = section->typeInfo();
			int64_t span = typeInfo.lastBank - typeInfo.firstBank;
			if (int64_t offset = rel.offset; offset > span || offset < -span) {
				fatalAt(
				    *section,
				    "Section \"%s\"'s bank depends on BANK(%s) with offset %" PRId32
				    ", but %s only has %" PRIu32 " banks",
				    section->name.c_str(),
				    describeBankRel(rel).c_str(),
				    rel.offset,
				    typeInfo.name,
				    typeInfo.nbBanks()
				);
			}
		}
	}

	for (Section *section : unassignedSections) {
		if (section->bankRelBase) {
			visitBankRelChain(*section);
		}
	}

	for (Section *section : unassignedSections) {
		foldBankRelIfKnown(*section);
	}

	// The remaining dependents are placed as their leader's bank becomes known; keep them
	// sorted as per the main list, so that the whole chain is placed well.
	for (Section *section : unassignedSections) {
		if (section->bankRelBase) {
			dependentsOf[section->bankRelBase].push_back(section);
		}
	}
}

// Assigns all sections that depend on the given one's bank (transitively). Its bank must be
// known, i.e. the section must have been assigned.
static void placeBankDependents(Section &leader) {
	if (auto const search = dependentsOf.find(&leader); search != dependentsOf.end()) {
		for (Section *dependent : search->second) {
			// Now that the leader is assigned, the dependent's bank is simply known.
			fixBankRelativeTo(*dependent, leader);
			placeSection(*dependent);
			placeBankDependents(*dependent);
		}
	}
}

// Categorize a section depending on how constrained it is.
// This is so the most-constrained sections are placed first.
static void categorizeSection(Section &section) {
	// Insert section while keeping the list sorted by decreasing constraints, alignment, and size.
	auto const compareSections = [](Section const *prev, Section const &sect) {
		// Sort more-constrained sections first (bank > address > alignment).
		if (prev->isBankFixed != sect.isBankFixed) {
			return prev->isBankFixed > sect.isBankFixed;
		}
		if (prev->isAddressFixed != sect.isAddressFixed) {
			return prev->isAddressFixed > sect.isAddressFixed;
		}
		if (prev->isAlignFixed != sect.isAlignFixed) {
			return prev->isAlignFixed > sect.isAlignFixed;
		}
		// For equally-constrained sections, sort more-aligned ones first.
		assume(prev->isAlignFixed == sect.isAlignFixed);
		if (sect.isAlignFixed && prev->alignMask != sect.alignMask) {
			return prev->alignMask > sect.alignMask;
		}
		// For equally-aligned sections, sort larger ones first.
		return prev->size > sect.size;
	};
	unassignedSections.insert(
	    std::lower_bound(RANGE(unassignedSections), section, compareSections), &section
	);
}

static void checkOverlayCompat() {
	std::string unfixedList;
	size_t nbListed = 0;
	size_t nbUnlisted = 0;

	for (Section const *section : unassignedSections) {
		if (section->isBankFixed && section->isAddressFixed) {
			continue;
		}

		if (nbListed == 10) {
			++nbUnlisted;
			continue;
		}

		unfixedList += "\n- \"";
		unfixedList += section->name;
		unfixedList += "\" (";
		std::string bankDesc =
		    section->bankRel && section->bankRel->kind != BANKREL_NONE
		        ? "bank (it depends on another section's)"
		        : "bank";
		if (!section->isBankFixed && !section->isAddressFixed) {
			unfixedList += bankDesc;
			unfixedList += " and address";
		} else if (!section->isBankFixed) {
			unfixedList += bankDesc;
		} else {
			assume(!section->isAddressFixed);
			unfixedList += "address";
		}
		unfixedList += " not specified)";
		++nbListed;
	}

	size_t nbUnfixedSections = nbListed + nbUnlisted;
	if (nbUnfixedSections == 0) {
		return;
	}

	if (nbUnlisted > 0) {
		unfixedList += "\n- and ";
		unfixedList += std::to_string(nbUnlisted);
		unfixedList += " more";
	}

	fatal(
	    "All sections must be fixed when using an overlay file; %zu %s not:%s",
	    nbUnfixedSections,
	    nbUnfixedSections == 1 ? "is" : "are",
	    unfixedList.c_str()
	);
}

void assign_AssignSections() {
	verbosePrint(VERB_NOTICE, "Beginning assignment...\n");

	// Initialize the free space-modelling structs
	for (SectionType type : EnumSeq(SECTTYPE_INVALID)) {
		SectionTypeInfo const &typeInfo = sectionTypeInfo[type];
		memory[type].resize(typeInfo.nbBanks());
		for (std::deque<FreeSpace> &bankMem : memory[type]) {
			bankMem.push_back({
			    .address = typeInfo.startAddr,
			    .size = typeInfo.size,
			});
		}
	}

	// Generate sorted list of sections to assign
	sect_ForEach(categorizeSection);

	// Check that all bank references (`BANK = BANK(other) + N`) are satisfiable, and fold the
	// bank of those that can be resolved up front
	resolveBankRels();

	// Overlaying requires only fully-constrained sections
	if (options.overlayFileName) {
		checkOverlayCompat();
	}

	// Sections whose bank depends on another section's are not placed here: they run out of
	// space otherwise, as their placement may only follow their dependency's.
	// Assign sections in the order they were sorted
	for (Section *section : unassignedSections) {
		if (section->isAssigned || section->bankRelBase) {
			continue;
		}
		placeSection(*section);
		placeBankDependents(*section);
	}
}
