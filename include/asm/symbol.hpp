// SPDX-License-Identifier: MIT

#ifndef RGBDS_ASM_SYMBOL_HPP
#define RGBDS_ASM_SYMBOL_HPP

#include <memory>
#include <stdint.h>
#include <string.h>
#include <string>
#include <string_view>
#include <time.h>
#include <utility>
#include <variant>

#include "asm/lexer.hpp"
#include "asm/section.hpp"

enum SymbolType {
	SYM_LABEL,
	SYM_EQU,
	SYM_VAR,
	SYM_MACRO,
	SYM_EQUS,
	SYM_REF // Forward reference to a label
};

struct Symbol;                    // Forward declaration for `sym_IsPC`
bool sym_IsPC(Symbol const *sym); // Forward declaration for `getSection`

struct Symbol {
	std::string name;
	SymbolType type;
	bool isExported; // Whether the symbol is to be exported
	bool isBuiltin;  // Whether the symbol is a built-in
	Section *section;
	std::shared_ptr<FileStackNode> src; // Where the symbol was defined
	uint32_t fileLine;                  // Line where the symbol was defined

	std::variant<
	    int32_t,                           // If isNumeric()
	    int32_t (*)(),                     // If isNumeric() via a callback
	    ContentSpan,                       // For SYM_MACRO
	    std::shared_ptr<std::string>,      // For SYM_EQUS
	    std::shared_ptr<std::string> (*)() // For SYM_EQUS via a callback
	    >
	    data;

	uint32_t ID;       // ID of the symbol in the object file (`UINT32_MAX` if none)
	uint32_t defIndex; // Ordering of the symbol in the state file

	bool isDefined() const { return type != SYM_REF; }
	bool isNumeric() const { return type == SYM_LABEL || type == SYM_EQU || type == SYM_VAR; }
	bool isLabel() const { return type == SYM_LABEL || type == SYM_REF; }

	bool isConstant() const {
		if (type == SYM_LABEL) {
			Section const *sect = getSection();
			return sect && sect->org != UINT32_MAX;
		}
		return type == SYM_EQU || type == SYM_VAR;
	}

	Section *getSection() const { return sym_IsPC(this) ? sect_GetSymbolSection() : section; }

	int32_t getValue() const;
	int32_t getOutputValue() const;
	ContentSpan const &getMacro() const;
	std::shared_ptr<std::string> getEqus() const;
	uint32_t getConstantValue() const;
};

void sym_ForEach(void (*callback)(Symbol &));

void sym_SetExportAll(bool set);
Symbol *sym_AddLocalLabel(std::string const &symName);
Symbol *sym_AddLabel(std::string const &symName);
Symbol *sym_AddAnonLabel();
std::string sym_MakeAnonLabelName(uint32_t ofs, bool neg);
void sym_Export(std::string const &symName);
Symbol *sym_AddEqu(std::string const &symName, int32_t value);
Symbol *sym_RedefEqu(std::string const &symName, int32_t value);
Symbol *sym_AddVar(std::string const &symName, int32_t value);
int32_t sym_GetRSValue();
void sym_SetRSValue(int32_t value);
// Find a symbol by exact name, bypassing expansion checks
Symbol *sym_FindExactSymbol(std::string const &symName);
// Find a symbol, possibly scoped, by name
Symbol *sym_FindScopedSymbol(std::string const &symName);
// Find a scoped symbol by name; do not return `@` or `_NARG` when they have no value
Symbol *sym_FindScopedValidSymbol(std::string const &symName);
Symbol const *sym_GetPC();
Symbol *sym_AddMacro(std::string const &symName, int32_t defLineNo, ContentSpan const &span);
Symbol *sym_Ref(std::string const &symName);
Symbol *sym_AddString(std::string const &symName, std::shared_ptr<std::string> value);
Symbol *sym_RedefString(std::string const &symName, std::shared_ptr<std::string> value);
void sym_Purge(std::string const &symName);
bool sym_IsPurgedExact(std::string const &symName);
bool sym_IsPurgedScoped(std::string const &symName);
void sym_Init(time_t now);

// Functions to save and restore the current label scopes.
std::pair<Symbol const *, Symbol const *> sym_GetCurrentLabelScopes();
void sym_SetCurrentLabelScopes(std::pair<Symbol const *, Symbol const *> newScopes);
void sym_ResetCurrentLabelScopes();

#endif // RGBDS_ASM_SYMBOL_HPP
