// SPDX-License-Identifier: MIT

#include "platform.hpp"

#include "link/object.hpp"

#include <algorithm>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <memory>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "helpers.hpp"
#include "linkdefs.hpp"
#include "util.hpp" // UniqueFile
#include "verbosity.hpp"
#include "version.hpp"

#include "link/fstack.hpp"
#include "link/patch.hpp"
#include "link/sdas_obj.hpp"
#include "link/section.hpp"
#include "link/symbol.hpp"
#include "link/warning.hpp"

// State owned by each object file. The outer vector is sized once by `obj_Setup`, so the
// per-file containers are never moved and pointers into them stay valid once populated.
struct ObjectFile {
	std::vector<Symbol> symbols;
	std::vector<FileStackNode> nodes;
};

static std::vector<ObjectFile> objectFiles;

// Helper functions for reading object files

// Reads an unsigned long (32-bit) value from a file, or `INT64_MAX` on failure.
static int64_t readLong(FILE *file) {
	uint32_t value = 0;

	// Read the little-endian value byte by byte
	for (uint8_t shift = 0; shift < sizeof(value) * CHAR_BIT; shift += 8) {
		int byte = getc(file);

		if (byte == EOF) {
			return INT64_MAX;
		}
		// Cast to `uint32_t` to avoid UB when shifting a byte >= 128 by a count >= 24.
		value |= static_cast<uint32_t>(byte) << shift;
	}
	return value;
}

static std::string formatMessage(char const *fmt, va_list args) {
	va_list copy;
	va_copy(copy, args);
	int length = vsnprintf(nullptr, 0, fmt, copy);
	va_end(copy);
	if (length < 0) {
		fatal("Failed to format object file error message");
	}
	std::string message(length + 1, '\0');
	vsnprintf(message.data(), message.size(), fmt, args);
	message.pop_back();
	return message;
}

[[noreturn]]
static void fatalRead(FILE *file, char const *fmt, va_list args) {
	fatal(
	    "%s: %s",
	    formatMessage(fmt, args).c_str(),
	    feof(file) ? "Unexpected end of file" : strerror(errno)
	);
}

// Reads a long from a file to a var, or errors out if it fails to.
template<typename T>
[[gnu::format(printf, 3, 4)]]
static void tryReadLong(T &var, FILE *file, char const *fmt, ...) {
	int64_t value = readLong(file);
	if (value == INT64_MAX) {
		va_list args;
		va_start(args, fmt);
		fatalRead(file, fmt, args);
	}
	var = static_cast<long>(value);
}

// Reads a byte from a file to a var, or errors out if it fails to.
[[gnu::format(printf, 3, 4)]]
static void tryGetc(uint8_t &var, FILE *file, char const *fmt, ...) {
	int value = getc(file);
	if (value == EOF) {
		va_list args;
		va_start(args, fmt);
		fatalRead(file, fmt, args);
	}
	var = static_cast<uint8_t>(value);
}

// Reads a '\0'-terminated string from a file to a var, or errors out if it fails to.
[[gnu::format(printf, 3, 4)]]
static void tryReadString(std::string &var, FILE *file, char const *fmt, ...) {
	for (int byte; (byte = getc(file)) != '\0';) {
		if (byte == EOF) {
			va_list args;
			va_start(args, fmt);
			fatalRead(file, fmt, args);
		}
		var.push_back(byte);
	}
}

// Functions to parse object files

// Reads a file stack node from a file.
static void readFileStackNode(
    FILE *file, std::vector<FileStackNode> &fileNodes, uint32_t nodeID, char const *fileName
) {
	FileStackNode &node = fileNodes[nodeID];

	uint32_t parentID;
	tryReadLong(
	    parentID, file, "%s: Cannot read node #%" PRIu32 "'s parent ID", fileName, nodeID
	);
	if (parentID == UINT32_MAX) {
		node.parent = nullptr;
	} else if (parentID >= fileNodes.size()) {
		fatal("%s: Node #%" PRIu32 " has invalid parent ID #%" PRIu32, fileName, nodeID, parentID);
	} else {
		node.parent = &fileNodes[parentID];
	}

	tryReadLong(
	    node.lineNo, file, "%s: Cannot read node #%" PRIu32 "'s line number", fileName, nodeID
	);

	uint8_t typeAndQuiet;
	tryGetc(typeAndQuiet, file, "%s: Cannot read node #%" PRIu32 "'s type", fileName, nodeID);
	switch (uint8_t type = typeAndQuiet & ~(1 << FSTACKNODE_QUIET_BIT); type) {
	case NODE_FILE:
	case NODE_MACRO:
		node.type = FileStackNodeType(type);
		node.data = "";
		tryReadString(
		    node.name(), file, "%s: Cannot read node #%" PRIu32 "'s file name", fileName, nodeID
		);
		break;
	case NODE_REPT: {
		node.type = NODE_REPT;
		uint32_t depth;
		tryReadLong(
		    depth, file, "%s: Cannot read node #%" PRIu32 "'s REPT depth", fileName, nodeID
		);
		node.data = std::vector<uint32_t>(depth);
		for (uint32_t i = 0; i < depth; ++i) {
			tryReadLong(
			    node.iters()[i],
			    file,
			    "%s: Cannot read node #%" PRIu32 "'s iter #%" PRIu32,
			    fileName,
			    nodeID,
			    i
			);
		}
		// Object files store iteration counts deepest-first; keep the same in-memory order as
		// RGBASM, so both programs can share the backtrace-printing logic.
		std::reverse(RANGE(node.iters()));
		if (!node.parent) {
			fatal(
			    "%s: Invalid object file: root node (#%" PRIu32 ") may not be REPT",
			    fileName,
			    nodeID
			);
		}
		break;
	}
	default:
		fatal("%s: Node #%" PRIu32 " has unknown type 0x%02x", fileName, nodeID, type);
	}

	node.isQuiet = (typeAndQuiet & (1 << FSTACKNODE_QUIET_BIT)) != 0;
}

// Reads a symbol from a file.
static void readSymbol(
    FILE *file, Symbol &symbol, char const *fileName, std::vector<FileStackNode> const &fileNodes
) {
	tryReadString(symbol.name, file, "%s: Cannot read symbol name", fileName);

	uint8_t type;
	tryGetc(type, file, "%s: Cannot read `%s`'s type", fileName, symbol.name.c_str());
	if (type >= SYMTYPE_INVALID) {
		fatal("%s: `%s` has unknown type 0x%02x", fileName, symbol.name.c_str(), type);
	} else {
		symbol.type = ExportLevel(type);
	}

	// If the symbol is defined in this file, read its definition
	if (symbol.type != SYMTYPE_IMPORT) {
		uint32_t nodeID;
		tryReadLong(
		    nodeID, file, "%s: Cannot read `%s`'s node ID", fileName, symbol.name.c_str()
		);
		if (nodeID >= fileNodes.size()) {
			fatal("%s: `%s` has invalid node ID #%" PRIu32, fileName, symbol.name.c_str(), nodeID);
		}

		symbol.src = &fileNodes[nodeID];
		tryReadLong(
		    symbol.lineNo,
		    file,
		    "%s: Cannot read `%s`'s line number",
		    fileName,
		    symbol.name.c_str()
		);
		int32_t sectionID, value;
		tryReadLong(
		    sectionID, file, "%s: Cannot read `%s`'s section ID", fileName, symbol.name.c_str()
		);
		tryReadLong(value, file, "%s: Cannot read `%s`'s value", fileName, symbol.name.c_str());
		if (sectionID == -1) {
			symbol.data = value;
		} else {
			symbol.data = Label{
			    .sectionID = sectionID,
			    .offset = value,
			    // Set the `.section` later based on the `.sectionID`
			    .section = nullptr,
			};
		}
	} else {
		symbol.data = -1;
	}
}

// Reads a section from a file.
static void readSection(
    FILE *file, Section &section, char const *fileName, std::vector<FileStackNode> const &fileNodes
) {
	int32_t tmp;
	uint8_t byte;

	tryReadString(section.name, file, "%s: Cannot read section name", fileName);

	uint32_t nodeID;
	tryReadLong(
	    nodeID, file, "%s: Cannot read \"%s\"'s node ID", fileName, section.name.c_str()
	);
	if (nodeID >= fileNodes.size()) {
		fatal("%s: \"%s\" has invalid node ID #%" PRIu32, fileName, section.name.c_str(), nodeID);
	}
	section.src = &fileNodes[nodeID];

	tryReadLong(
	    section.lineNo,
	    file,
	    "%s: Cannot read \"%s\"'s line number",
	    fileName,
	    section.name.c_str()
	);
	tryReadLong(tmp, file, "%s: Cannot read \"%s\"'s' size", fileName, section.name.c_str());
	if (tmp < 0 || tmp > UINT16_MAX) {
		fatal(
		    "%s: \"%s\"'s section size ($%" PRIx32 ") is invalid",
		    fileName,
		    section.name.c_str(),
		    tmp
		);
	}
	section.size = tmp;
	section.offset = 0;

	tryGetc(byte, file, "%s: Cannot read \"%s\"'s type", fileName, section.name.c_str());
	if (uint8_t type = byte & SECTTYPE_TYPE_MASK; type >= SECTTYPE_INVALID) {
		fatal("%s: \"%s\" has unknown section type 0x%02x", fileName, section.name.c_str(), type);
	} else {
		section.type = SectionType(type);
	}

	if (byte & (1 << SECTTYPE_UNION_BIT)) {
		section.modifier = SECTION_UNION;
	} else if (byte & (1 << SECTTYPE_FRAGMENT_BIT)) {
		section.modifier = SECTION_FRAGMENT;
	} else {
		section.modifier = SECTION_NORMAL;
	}
	tryReadLong(tmp, file, "%s: Cannot read \"%s\"'s org", fileName, section.name.c_str());
	if (tmp < 0) {
		section.org.reset();
	} else {
		if (tmp > UINT16_MAX) {
			error("\"%s\"'s org is too large ($%" PRIx32 ")", section.name.c_str(), tmp);
			tmp = UINT16_MAX;
		}
		section.org = static_cast<uint16_t>(tmp);
	}
	tryReadLong(tmp, file, "%s: Cannot read \"%s\"'s bank", fileName, section.name.c_str());
	if (tmp < 0) {
		section.bank.reset();
	} else {
		section.bank = static_cast<uint32_t>(tmp);
	}
	tryGetc(byte, file, "%s: Cannot read \"%s\"'s alignment", fileName, section.name.c_str());
	if (byte > 16) {
		byte = 16;
	}
	tryReadLong(
	    tmp, file, "%s: Cannot read \"%s\"'s alignment offset", fileName, section.name.c_str()
	);
	if (tmp > UINT16_MAX) {
		error("\"%s\"'s alignment offset is too large ($%" PRIx32 ")", section.name.c_str(), tmp);
		tmp = UINT16_MAX;
	}
	if (byte == 0) {
		section.align.reset();
	} else {
		section.align = SectionAlignment{
		    .mask = static_cast<uint16_t>((1 << byte) - 1),
		    .offset = static_cast<uint16_t>(tmp),
		};
	}

	if (!sectTypeHasData(section.type)) {
		return;
	}

	if (section.size) {
		section.data.resize(section.size);
		if (fread(section.data.data(), 1, section.size, file) != section.size) {
			fatal(
			    "%s: Cannot read \"%s\"'s data: %s",
			    fileName,
			    section.name.c_str(),
			    feof(file) ? "Unexpected end of file" : strerror(errno)
			);
		}
	}

	uint32_t nbPatches;
	tryReadLong(
	    nbPatches,
	    file,
	    "%s: Cannot read \"%s\"'s number of patches",
	    fileName,
	    section.name.c_str()
	);

	section.patches.resize(nbPatches);
	for (uint32_t patchID = 0; patchID < nbPatches; ++patchID) {
		Patch &patch = section.patches[patchID];

		tryReadLong(
		    nodeID,
		    file,
		    "%s: Cannot read \"%s\"'s patch #%" PRIu32 "'s node ID",
		    fileName,
		    section.name.c_str(),
		    patchID
		);
		if (nodeID >= fileNodes.size()) {
			fatal(
			    "%s: \"%s\"'s patch #%" PRIu32 " has invalid node ID #%" PRIu32,
			    fileName,
			    section.name.c_str(),
			    patchID,
			    nodeID
			);
		}
		patch.rpn.src = &fileNodes[nodeID];

		tryReadLong(
		    patch.rpn.lineNo,
		    file,
		    "%s: Cannot read \"%s\"'s patch #%" PRIu32 "'s line number",
		    fileName,
		    section.name.c_str(),
		    patchID
		);
		tryReadLong(
		    patch.offset,
		    file,
		    "%s: Cannot read \"%s\"'s patch #%" PRIu32 "'s offset",
		    fileName,
		    section.name.c_str(),
		    patchID
		);
		tryReadLong(
		    patch.rpn.pcSectionID,
		    file,
		    "%s: Cannot read \"%s\"'s patch #%" PRIu32 "'s PC offset",
		    fileName,
		    section.name.c_str(),
		    patchID
		);
		tryReadLong(
		    patch.rpn.pcOffset,
		    file,
		    "%s: Cannot read \"%s\"'s patch #%" PRIu32 "'s PC offset",
		    fileName,
		    section.name.c_str(),
		    patchID
		);

		uint8_t type;
		tryGetc(
		    type,
		    file,
		    "%s: Cannot read \"%s\"'s patch #%" PRIu32 "'s type",
		    fileName,
		    section.name.c_str(),
		    patchID
		);
		if (type >= PATCHTYPE_INVALID) {
			fatal(
			    "%s: \"%s\"'s patch #%" PRIu32 " has unknown type 0x%02x",
			    fileName,
			    section.name.c_str(),
			    patchID,
			    type
			);
		} else {
			patch.type = PatchType(type);
		}

		if (patch.offset > section.size || patch.offset + patch.typeSize() > section.size) {
			fatal(
			    "%s: \"%s\"'s patch #%" PRIu32 " overflows the section",
			    fileName,
			    section.name.c_str(),
			    patchID
			);
		}

		uint32_t rpnSize;
		tryReadLong(
		    rpnSize,
		    file,
		    "%s: Cannot read \"%s\"'s patch #%" PRIu32 "'s RPN size",
		    fileName,
		    section.name.c_str(),
		    patchID
		);

		patch.rpn.rpn.resize(rpnSize);
		if (fread(patch.rpn.rpn.data(), 1, rpnSize, file) != rpnSize) {
			fatal(
			    "%s: Cannot read \"%s\"'s patch #%" PRIu32 "'s RPN expression: %s",
			    fileName,
			    section.name.c_str(),
			    patchID,
			    feof(file) ? "Unexpected end of file" : strerror(errno)
			);
		}
	}
}

// Reads an assertion from a file.
static void readAssertion(
    FILE *file,
    Assertion &assert,
    char const *fileName,
    uint32_t assertID,
    std::vector<FileStackNode> const &fileNodes
) {
	uint32_t nodeID;
	tryReadLong(
	    nodeID, file, "%s: Cannot read assertion #%" PRIu32 "'s node ID", fileName, assertID
	);
	if (nodeID >= fileNodes.size()) {
		fatal(
		    "%s: assertion #%" PRIu32 " has invalid node ID #%" PRIu32, fileName, assertID, nodeID
		);
	}
	assert.rpn.src = &fileNodes[nodeID];

	tryReadLong(
	    assert.rpn.lineNo,
	    file,
	    "%s: Cannot read assertion #%" PRIu32 "'s line number",
	    fileName,
	    assertID
	);
	tryReadLong(
	    assert.offset,
	    file,
	    "%s: Cannot read assertion #%" PRIu32 "'s offset",
	    fileName,
	    assertID
	);
	tryReadLong(
	    assert.rpn.pcSectionID,
	    file,
	    "%s: Cannot read assertion #%" PRIu32 "'s PC offset",
	    fileName,
	    assertID
	);
	tryReadLong(
	    assert.rpn.pcOffset,
	    file,
	    "%s: Cannot read assertion #%" PRIu32 "'s PC offset",
	    fileName,
	    assertID
	);

	uint8_t type;
	tryGetc(type, file, "%s: Cannot read assertion #%" PRIu32 "'s type", fileName, assertID);
	if (type > ASSERT_FATAL) {
		fatal("%s: assertion #%" PRIu32 " has unknown type 0x%02x", fileName, assertID, type);
	} else {
		assert.type = AssertionType(type);
	}

	uint32_t rpnSize;
	tryReadLong(
	    rpnSize, file, "%s: Cannot read assertion #%" PRIu32 "'s RPN size", fileName, assertID
	);

	assert.rpn.rpn.resize(rpnSize);
	if (fread(assert.rpn.rpn.data(), 1, rpnSize, file) != rpnSize) {
		fatal(
		    "%s: Cannot read assertion #%" PRIu32 "'s RPN expression: %s",
		    fileName,
		    assertID,
		    feof(file) ? "Unexpected end of file" : strerror(errno)
		);
	}

	tryReadString(
	    assert.message,
	    file,
	    "%s: Cannot read assertion #%" PRIu32 "'s message",
	    fileName,
	    assertID
	);
}

void obj_ReadFile(std::string const &filePath, size_t fileID) {
	UniqueFile file;
	char const *fileName = filePath.c_str();
	if (filePath != "-") {
		file = fopen(fileName, "rb");
	} else {
		fileName = "<stdin>";
		(void)setmode(STDIN_FILENO, O_BINARY);
		file = stdin;
	}
	if (!file) {
		fatal("Failed to open file \"%s\": %s", fileName, strerror(errno));
	}
	if (fileID >= objectFiles.size()) {
		fatal("Internal error: invalid object file ID #%zu", fileID);
	}
	ObjectFile &object = objectFiles[fileID];

	// First, check if the object is a RGBDS object, a SDCC one, or neither.
	// A single `ungetc` is guaranteed to work.
	switch (ungetc(getc(file), file)) {
	case EOF:
		fatal("File \"%s\" is empty", fileName);

	case 'X':
	case 'D':
	case 'Q': {
		// This is (probably) a SDCC object file, defer the rest of detection to it.
		// Since SDCC does not provide line info, everything will be reported as coming from the
		// object file. It's better than nothing.
		object.nodes.push_back({
		    .type = NODE_FILE,
		    .data = std::variant<std::monostate, std::vector<uint32_t>, std::string>(fileName),
		    .isQuiet = false,
		    .parent = nullptr,
		    .lineNo = 0,
		});

		std::vector<Symbol> &fileSymbols = object.symbols;

		sdobj_ReadFile(object.nodes.back(), file, fileSymbols);
		return;
	}

	case 'R':
		// Check the magic byte signature for a RGB object file.
		if (char magic[literal_strlen(RGBDS_OBJECT_VERSION_STRING)];
		    fread(magic, 1, sizeof(magic), file) == sizeof(magic)
		    && !memcmp(magic, RGBDS_OBJECT_VERSION_STRING, sizeof(magic))) {
			break;
		}
		[[fallthrough]];

	default:
		fatal("%s: Not a RGBDS object file", fileName);
	}

	verbosePrint(VERB_NOTICE, "Reading object file %s\n", fileName);

	uint32_t revNum;
	tryReadLong(revNum, file, "%s: Cannot read revision number", fileName);
	if (revNum != RGBDS_OBJECT_REV) {
		fatal(
		    "%s: Unsupported object file for rgblink %s; try rebuilding \"%s\"%s"
		    " (expected revision %d, got %d)",
		    fileName,
		    get_package_version_string(),
		    fileName,
		    revNum > RGBDS_OBJECT_REV ? " or updating rgblink" : "",
		    RGBDS_OBJECT_REV,
		    revNum
		);
	}

	uint32_t nbSymbols;
	tryReadLong(nbSymbols, file, "%s: Cannot read number of symbols", fileName);

	uint32_t nbSections;
	tryReadLong(nbSections, file, "%s: Cannot read number of sections", fileName);

	uint32_t nbNodes;
	tryReadLong(nbNodes, file, "%s: Cannot read number of nodes", fileName);
	object.nodes.resize(nbNodes);
	verbosePrint(VERB_INFO, "Reading %u nodes...\n", nbNodes);
	for (uint32_t nodeID = nbNodes; nodeID--;) {
		readFileStackNode(file, object.nodes, nodeID, fileName);
	}

	// This file's symbols, kept to link sections to them
	std::vector<Symbol> &fileSymbols = object.symbols;
	fileSymbols.resize(nbSymbols);
	std::vector<uint32_t> nbSymPerSect(nbSections, 0);

	verbosePrint(VERB_INFO, "Reading %" PRIu32 " symbols...\n", nbSymbols);
	for (Symbol &sym : fileSymbols) {
		readSymbol(file, sym, fileName, object.nodes);
		sym_AddSymbol(sym);
		if (std::holds_alternative<Label>(sym.data)) {
			int32_t sectionID = std::get<Label>(sym.data).sectionID;
			if (sectionID < 0 || static_cast<size_t>(sectionID) >= nbSymPerSect.size()) {
				fatal(
				    "%s: `%s` has invalid section ID #%" PRId32,
				    fileName,
				    sym.name.c_str(),
				    sectionID
				);
			}
			++nbSymPerSect[sectionID];
		}
	}

	// This file's sections, stored in a table to link symbols to them
	std::vector<std::unique_ptr<Section>> fileSections(nbSections);

	verbosePrint(VERB_INFO, "Reading %" PRIu32 " sections...\n", nbSections);
	for (uint32_t i = 0; i < nbSections; ++i) {
		fileSections[i] = std::make_unique<Section>();
		fileSections[i]->nextPiece = nullptr;
		readSection(file, *fileSections[i], fileName, object.nodes);
		fileSections[i]->fileSymbols = &fileSymbols;
		fileSections[i]->symbols.reserve(nbSymPerSect[i]);
	}

	uint32_t nbAsserts;
	tryReadLong(nbAsserts, file, "%s: Cannot read number of assertions", fileName);
	verbosePrint(VERB_INFO, "Reading %" PRIu32 " assertions...\n", nbAsserts);
	for (uint32_t i = 0; i < nbAsserts; ++i) {
		Assertion &assertion = patch_AddAssertion();

		readAssertion(file, assertion, fileName, i, object.nodes);

		if (assertion.rpn.pcSectionID == UINT32_MAX) {
			assertion.rpn.pcSection = nullptr;
		} else if (assertion.rpn.pcSectionID >= fileSections.size()) {
			fatal(
			    "%s: Assertion #%" PRIu32 " has invalid section ID #%" PRIu32,
			    fileName,
			    i,
			    assertion.rpn.pcSectionID
			);
		} else {
			assertion.rpn.pcSection = fileSections[assertion.rpn.pcSectionID].get();
		}

		assertion.fileSymbols = &fileSymbols;
	}

	// Give patches' PC section pointers to their sections
	for (std::unique_ptr<Section> const &sect : fileSections) {
		if (!sectTypeHasData(sect->type)) {
			continue;
		}
		for (size_t i = 0; i < sect->patches.size(); ++i) {
			if (Expression &rpn = sect->patches[i].rpn; rpn.pcSectionID == UINT32_MAX) {
				rpn.pcSection = nullptr;
			} else if (rpn.pcSectionID >= fileSections.size()) {
				fatal(
				    "%s: \"%s\"'s patch #%zu has invalid section ID #%" PRIu32,
				    fileName,
				    sect->name.c_str(),
				    i,
				    rpn.pcSectionID
				);
			} else {
				rpn.pcSection = fileSections[rpn.pcSectionID].get();
			}
		}
	}

	// Give symbols' section pointers to their sections
	for (Symbol &sym : fileSymbols) {
		if (std::holds_alternative<Label>(sym.data)) {
			sym.linkToSection(*fileSections[std::get<Label>(sym.data).sectionID]);
		}
	}

	// Calling `sect_AddSection` invalidates the contents of `fileSections`!
	for (uint32_t i = 0; i < nbSections; ++i) {
		sect_AddSection(std::move(fileSections[i]));
	}

	// Fix symbols' section pointers to section "pieces"
	// This has to run **after** all the `sect_AddSection()` calls,
	// so that `sect_GetSection()` will work
	for (Symbol &sym : fileSymbols) {
		sym.fixSectionOffset();
	}
}

void obj_Setup(size_t nbFiles) {
	objectFiles.assign(nbFiles, {});
}
