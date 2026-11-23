// SPDX-License-Identifier: MIT

#include "link/patch.hpp"

#include <array>
#include <deque>
#include <iterator>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <variant>
#include <vector>

#include "diagnostics.hpp"
#include "helpers.hpp" // assume
#include "linkdefs.hpp"
#include "opmath.hpp"
#include "verbosity.hpp"

#include "link/section.hpp"
#include "link/symbol.hpp"
#include "link/warning.hpp"

static std::deque<Assertion> assertions;

namespace {

struct RPNStackEntry {
	int32_t value;
	bool errorFlag; // Whether the value is a placeholder inserted for error recovery
};

// Per-expression evaluation state. This used to be a set of file-scope variables, which made
// `computeRPNExpr` non-reentrant and forced every diagnostic helper to be a macro reaching into
// that global state. Keeping the state together makes its lifetime and meaning explicit.
struct RPNContext {
	Expression const &expr;
	std::vector<Symbol> const &fileSymbols;
	std::deque<RPNStackEntry> stack;
	bool isError = false; // Did an op pop a value with its error flag set?

	RPNContext(Expression const &expr, std::vector<Symbol> const &fileSymbols)
	    : expr(expr), fileSymbols(fileSymbols) {}

	void push(int32_t value) { stack.push_front({.value = value, .errorFlag = isError}); }

	int32_t pop() {
		if (stack.empty()) {
			fatalAt(expr, "Internal error, RPN stack empty");
		}

		RPNStackEntry entry = stack.front();
		stack.pop_front();
		isError |= entry.errorFlag;
		return entry.value;
	}

	uint32_t getByte(uint8_t const *&rpnBuffer, int32_t &size) {
		if (!size--) {
			fatalAt(expr, "Internal error, RPN expression overread");
		}

		return *rpnBuffer++;
	}

	// Report a warning, or an error if it is configured as one. Either way, an error diagnostic
	// marks the current operation as erroneous, so later ops can suppress redundant errors.
	template<typename... Args>
	void diagnosticAt(WarningID id, char const *fmt, Args... args) {
		bool errorDiag = warnings.getWarningBehavior(id) == WarningBehavior::ERROR;
		if (!isError || !errorDiag) {
			warningAt(expr, id, fmt, args...);
		}
		if (errorDiag) {
			isError = true;
		}
	}

	template<typename... Args>
	void rpnErrorAt(char const *fmt, Args... args) {
		errorAt(expr, fmt, args...);
		isError = true;
	}

	template<typename... Args>
	void firstErrorAt(char const *fmt, Args... args) {
		if (!isError) {
			rpnErrorAt(fmt, args...);
		}
	}
};

using RPNHandler = int32_t (*)(RPNContext &, uint8_t const *&, int32_t &);

// Read a NUL-terminated section name directly from the RPN byte stream.
static char const *readSectionName(RPNContext &ctx, uint8_t const *&rpnBuffer, int32_t &size) {
	// `rpnBuffer` is not guaranteed to be '\0'-terminated. If it is not,
	// `getByte` will have a fatal internal error.
	char const *name = reinterpret_cast<char const *>(rpnBuffer);
	while (ctx.getByte(rpnBuffer, size)) {}
	return name;
}

static uint32_t readDWord(RPNContext &ctx, uint8_t const *&rpnBuffer, int32_t &size) {
	uint32_t value = 0;
	for (uint8_t shift = 0; shift < 32; shift += 8) {
		value |= ctx.getByte(rpnBuffer, size) << shift;
	}
	return value;
}

static Symbol const *getSymbol(std::vector<Symbol> const &symbolList, uint32_t index) {
	assume(index != UINT32_MAX);       // PC needs to be handled specially, not here
	assume(index < symbolList.size()); // This needs to be checked before calling
	Symbol const &symbol = symbolList[index];

	// If the symbol is defined elsewhere...
	if (symbol.type == SYMTYPE_IMPORT) {
		return sym_GetSymbol(symbol.name);
	}

	return &symbol;
}

static int32_t getBankSym(RPNContext &ctx, uint8_t const *&rpnBuffer, int32_t &size) {
	uint32_t symID = readDWord(ctx, rpnBuffer, size);

	if (symID >= ctx.fileSymbols.size()) {
		fatalAt(ctx.expr, "Requested `BANK()` of invalid symbol ID #%" PRIu32, symID);
	}
	Symbol const *symbol = getSymbol(ctx.fileSymbols, symID);
	if (!symbol) {
		ctx.rpnErrorAt(
		    "Requested `BANK()` of undefined symbol `%s`", ctx.fileSymbols[symID].name.c_str()
		);
		return 1;
	}
	if (std::holds_alternative<Label>(symbol->data)) {
		Label const &label = std::get<Label>(symbol->data);
		if (!label.section) {
			ctx.rpnErrorAt(
			    "Requested `BANK()` of label `%s` outside of a section",
			    ctx.fileSymbols[symID].name.c_str()
			);
			return 0;
		}
		return label.section->bank;
	}
	ctx.rpnErrorAt(
	    "Requested `BANK()` of non-label symbol `%s`", ctx.fileSymbols[symID].name.c_str()
	);
	return 1;
}

static int32_t getBankSect(RPNContext &ctx, uint8_t const *&rpnBuffer, int32_t &size) {
	char const *name = readSectionName(ctx, rpnBuffer, size);
	Section const *sect = sect_GetSection(name);
	if (!sect) {
		ctx.rpnErrorAt("Requested `BANK()` of undefined section \"%s\"", name);
		return 1;
	}
	return sect->bank;
}

static int32_t getBankSelf(RPNContext &ctx, uint8_t const *&, int32_t &) {
	if (!ctx.expr.pcSection) {
		ctx.rpnErrorAt("PC has no bank outside of a section");
		return 1;
	}
	return ctx.expr.pcSection->bank;
}

static int32_t getSizeofSect(RPNContext &ctx, uint8_t const *&rpnBuffer, int32_t &size) {
	char const *name = readSectionName(ctx, rpnBuffer, size);
	Section const *sect = sect_GetSection(name);
	if (!sect) {
		ctx.rpnErrorAt("Requested `SIZEOF()` of undefined section \"%s\"", name);
		return 1;
	}
	return sect->size;
}

static int32_t getStartofSect(RPNContext &ctx, uint8_t const *&rpnBuffer, int32_t &size) {
	char const *name = readSectionName(ctx, rpnBuffer, size);
	Section const *sect = sect_GetSection(name);
	if (!sect) {
		ctx.rpnErrorAt("Requested `STARTOF()` of undefined section \"%s\"", name);
		return 1;
	}
	assume(sect->offset == 0);
	return sect->org;
}

static int32_t getSizeofSectType(RPNContext &ctx, uint8_t const *&rpnBuffer, int32_t &size) {
	int32_t type = ctx.getByte(rpnBuffer, size);
	if (type >= SECTTYPE_INVALID) {
		ctx.rpnErrorAt("Requested `SIZEOF()` of an invalid section type");
		return 0;
	}
	return sectionTypeInfo[type].size;
}

static int32_t getStartofSectType(RPNContext &ctx, uint8_t const *&rpnBuffer, int32_t &size) {
	int32_t type = ctx.getByte(rpnBuffer, size);
	if (type >= SECTTYPE_INVALID) {
		ctx.rpnErrorAt("Requested `STARTOF()` of an invalid section type");
		return 0;
	}
	return sectionTypeInfo[type].startAddr;
}

static int32_t getHRAM(RPNContext &ctx, uint8_t const *&, int32_t &) {
	int32_t value = ctx.pop();
	if (value < 0xFF00 || value > 0xFFFF) {
		ctx.firstErrorAt(
		    "Address $%" PRIx32 " for `LDH` is not in HRAM range; use `LD` instead", value
		);
		value = 0;
	}
	return value & 0xFF;
}

static int32_t getRST(RPNContext &ctx, uint8_t const *&, int32_t &) {
	int32_t value = ctx.pop();
	// Acceptable values are 0x00, 0x08, 0x10, ..., 0x38
	if (value & ~0x38) {
		ctx.firstErrorAt("Value $%" PRIx32 " is not a `RST` vector; use `CALL` instead", value);
		value = 0;
	}
	return value | 0xC7;
}

static int32_t getBitIndex(RPNContext &ctx, uint8_t const *&rpnBuffer, int32_t &size) {
	int32_t value = ctx.pop();
	int32_t mask = ctx.getByte(rpnBuffer, size);
	// Acceptable values are 0 to 7
	if (value & ~0x07) {
		ctx.firstErrorAt("Value $%" PRIx32 " is not a bit index", value);
		value = 0;
	}
	return mask | (value << 3);
}

static int32_t getSymbolValue(RPNContext &ctx, uint8_t const *&rpnBuffer, int32_t &size) {
	uint32_t symID = readDWord(ctx, rpnBuffer, size);

	if (symID == UINT32_MAX) { // PC
		if (ctx.expr.pcSection) {
			return ctx.expr.pcOffset + ctx.expr.pcSection->org;
		}
		ctx.rpnErrorAt("PC has no value outside of a section");
		return 0;
	}
	if (symID >= ctx.fileSymbols.size()) {
		fatalAt(ctx.expr, "Invalid symbol ID #%" PRIu32, symID);
	}
	Symbol const *symbol = getSymbol(ctx.fileSymbols, symID);
	if (!symbol) {
		ctx.rpnErrorAt("Undefined symbol `%s`", ctx.fileSymbols[symID].name.c_str());
		sym_TraceLocalAliasedSymbols(ctx.fileSymbols[symID].name);
		return 0;
	}
	if (std::holds_alternative<Label>(symbol->data)) {
		Label const &label = std::get<Label>(symbol->data);
		if (!label.section) {
			ctx.rpnErrorAt(
			    "Requested value of label `%s` outside of a section",
			    ctx.fileSymbols[symID].name.c_str()
			);
			return 0;
		}
		return label.section->org + label.offset;
	}
	return std::get<int32_t>(symbol->data);
}

struct RPNHandlers {
	using Unary = int32_t (*)(int32_t);

	template<Unary op>
	static int32_t unary(RPNContext &ctx, uint8_t const *&, int32_t &) {
		return op(ctx.pop());
	}

	static int32_t sub(RPNContext &ctx, uint8_t const *&, int32_t &) {
		int32_t rhs = ctx.pop();
		return op_sub(ctx.pop(), rhs);
	}

	static int32_t divide(RPNContext &ctx, uint8_t const *&, int32_t &) {
		int32_t rhs = ctx.pop();
		if (rhs == 0) {
			ctx.firstErrorAt("Division by 0");
			ctx.pop();
			return 0;
		}
		int32_t lhs = ctx.pop();
		if (lhs == INT32_MIN && rhs == -1) {
			ctx.diagnosticAt(
			    WARNING_DIV,
			    "Division of %" PRId32 " by -1 yields %" PRId32,
			    INT32_MIN,
			    INT32_MIN
			);
			return INT32_MIN;
		}
		return op_divide(lhs, rhs);
	}

	static int32_t modulo(RPNContext &ctx, uint8_t const *&, int32_t &) {
		int32_t rhs = ctx.pop();
		if (rhs == 0) {
			ctx.firstErrorAt("Modulo by 0");
			ctx.pop();
			return 0;
		}
		int32_t lhs = ctx.pop();
		if (lhs == INT32_MIN && rhs == -1) {
			ctx.diagnosticAt(WARNING_DIV, "Modulo of %" PRId32 " by -1 yields 0", INT32_MIN);
			return 0;
		}
		return op_modulo(lhs, rhs);
	}

	static int32_t exponent(RPNContext &ctx, uint8_t const *&, int32_t &) {
		int32_t rhs = ctx.pop();
		if (rhs < 0) {
			ctx.firstErrorAt("Exponent by negative value %" PRId32, rhs);
			ctx.pop();
			return 0;
		}
		return op_exponent(ctx.pop(), rhs);
	}

	static int32_t logAnd(RPNContext &ctx, uint8_t const *&, int32_t &) {
		int32_t rhs = ctx.pop();
		return ctx.pop() && rhs;
	}

	static int32_t logOr(RPNContext &ctx, uint8_t const *&, int32_t &) {
		int32_t rhs = ctx.pop();
		return ctx.pop() || rhs;
	}

	static int32_t logGreater(RPNContext &ctx, uint8_t const *&, int32_t &) {
		int32_t rhs = ctx.pop();
		return ctx.pop() > rhs;
	}

	static int32_t logLess(RPNContext &ctx, uint8_t const *&, int32_t &) {
		int32_t rhs = ctx.pop();
		return ctx.pop() < rhs;
	}

	static int32_t logGreaterEq(RPNContext &ctx, uint8_t const *&, int32_t &) {
		int32_t rhs = ctx.pop();
		return ctx.pop() >= rhs;
	}

	static int32_t logLessEq(RPNContext &ctx, uint8_t const *&, int32_t &) {
		int32_t rhs = ctx.pop();
		return ctx.pop() <= rhs;
	}

	static int32_t shiftLeft(RPNContext &ctx, uint8_t const *&, int32_t &) {
		int32_t amount = ctx.pop();
		if (amount < 0) {
			ctx.diagnosticAt(
			    WARNING_SHIFT_AMOUNT, "Shifting left by negative amount %" PRId32, amount
			);
		}
		if (amount >= 32) {
			ctx.diagnosticAt(
			    WARNING_SHIFT_AMOUNT, "Shifting left by large amount %" PRId32, amount
			);
		}
		return op_shift_left(ctx.pop(), amount);
	}

	static int32_t shiftRight(RPNContext &ctx, uint8_t const *&, int32_t &) {
		int32_t amount = ctx.pop();
		int32_t lhs = ctx.pop();
		if (lhs < 0) {
			ctx.diagnosticAt(WARNING_SHIFT, "Shifting right negative value %" PRId32, lhs);
		}
		if (amount < 0) {
			ctx.diagnosticAt(
			    WARNING_SHIFT_AMOUNT, "Shifting right by negative amount %" PRId32, amount
			);
		}
		if (amount >= 32) {
			ctx.diagnosticAt(WARNING_SHIFT_AMOUNT, "Shifting right by large amount %" PRId32, amount);
		}
		return op_shift_right(lhs, amount);
	}

	static int32_t shiftRightUnsigned(RPNContext &ctx, uint8_t const *&, int32_t &) {
		int32_t amount = ctx.pop();
		if (amount < 0) {
			ctx.diagnosticAt(
			    WARNING_SHIFT_AMOUNT, "Shifting right by negative amount %" PRId32, amount
			);
		}
		if (amount >= 32) {
			ctx.diagnosticAt(
			    WARNING_SHIFT_AMOUNT, "Shifting right by large amount %" PRId32, amount
			);
		}
		return op_shift_right_unsigned(ctx.pop(), amount);
	}

	static int32_t constant(RPNContext &ctx, uint8_t const *&rpnBuffer, int32_t &size) {
		// The returned value is interpreted as signed, as with all stack entries.
		return static_cast<int32_t>(readDWord(ctx, rpnBuffer, size));
	}
};

// A separate handler reports invalid commands, since the table is sized by command value.
[[noreturn]]
static void invalidRPNCommand(RPNContext &ctx, RPNCommand command) {
	// LCOV_EXCL_START
	fatalAt(ctx.expr, "Invalid RPN command $%02x", static_cast<uint32_t>(command));
	// LCOV_EXCL_STOP
}

// Keep the handler names short to make the dispatch table easier to scan.
static int32_t add(RPNContext &ctx, uint8_t const *&, int32_t &) {
	// Operand evaluation order is unspecified in C++; keep it explicit.
	int32_t rhs = ctx.pop();
	return op_add(ctx.pop(), rhs);
}
static int32_t mul(RPNContext &ctx, uint8_t const *&, int32_t &) {
	int32_t rhs = ctx.pop();
	return op_mul(ctx.pop(), rhs);
}
static int32_t bitNot(RPNContext &ctx, uint8_t const *&, int32_t &) { return ~ctx.pop(); }
static int32_t logNot(RPNContext &ctx, uint8_t const *&, int32_t &) { return !ctx.pop(); }

static int32_t logEq(RPNContext &ctx, uint8_t const *&, int32_t &) {
	int32_t rhs = ctx.pop();
	return ctx.pop() == rhs;
}
static int32_t logNe(RPNContext &ctx, uint8_t const *&, int32_t &) {
	int32_t rhs = ctx.pop();
	return ctx.pop() != rhs;
}
static int32_t bitOr(RPNContext &ctx, uint8_t const *&, int32_t &) {
	int32_t rhs = ctx.pop();
	return ctx.pop() | rhs;
}
static int32_t bitAnd(RPNContext &ctx, uint8_t const *&, int32_t &) {
	int32_t rhs = ctx.pop();
	return ctx.pop() & rhs;
}
static int32_t bitXor(RPNContext &ctx, uint8_t const *&, int32_t &) {
	int32_t rhs = ctx.pop();
	return ctx.pop() ^ rhs;
}

static constexpr auto rpnHandlers = [] {
	std::array<RPNHandler, RPN_SYM + 1> handlers{};
	handlers[RPN_ADD] = add;
	handlers[RPN_SUB] = RPNHandlers::sub;
	handlers[RPN_MUL] = mul;
	handlers[RPN_DIV] = RPNHandlers::divide;
	handlers[RPN_MOD] = RPNHandlers::modulo;
	handlers[RPN_NEG] = RPNHandlers::unary<op_neg>;
	handlers[RPN_EXP] = RPNHandlers::exponent;
	handlers[RPN_OR] = bitOr;
	handlers[RPN_AND] = bitAnd;
	handlers[RPN_XOR] = bitXor;
	handlers[RPN_NOT] = bitNot;
	handlers[RPN_LOGAND] = RPNHandlers::logAnd;
	handlers[RPN_LOGOR] = RPNHandlers::logOr;
	handlers[RPN_LOGNOT] = logNot;
	handlers[RPN_LOGEQ] = logEq;
	handlers[RPN_LOGNE] = logNe;
	handlers[RPN_LOGGT] = RPNHandlers::logGreater;
	handlers[RPN_LOGLT] = RPNHandlers::logLess;
	handlers[RPN_LOGGE] = RPNHandlers::logGreaterEq;
	handlers[RPN_LOGLE] = RPNHandlers::logLessEq;
	handlers[RPN_SHL] = RPNHandlers::shiftLeft;
	handlers[RPN_SHR] = RPNHandlers::shiftRight;
	handlers[RPN_USHR] = RPNHandlers::shiftRightUnsigned;
	handlers[RPN_BANK_SYM] = getBankSym;
	handlers[RPN_BANK_SECT] = getBankSect;
	handlers[RPN_BANK_SELF] = getBankSelf;
	handlers[RPN_SIZEOF_SECT] = getSizeofSect;
	handlers[RPN_STARTOF_SECT] = getStartofSect;
	handlers[RPN_SIZEOF_SECTTYPE] = getSizeofSectType;
	handlers[RPN_STARTOF_SECTTYPE] = getStartofSectType;
	handlers[RPN_HRAM] = getHRAM;
	handlers[RPN_RST] = getRST;
	handlers[RPN_BIT_INDEX] = getBitIndex;
	handlers[RPN_HIGH] = RPNHandlers::unary<op_high>;
	handlers[RPN_LOW] = RPNHandlers::unary<op_low>;
	handlers[RPN_BITWIDTH] = RPNHandlers::unary<op_bitwidth>;
	handlers[RPN_TZCOUNT] = RPNHandlers::unary<op_tzcount>;
	handlers[RPN_CONST] = RPNHandlers::constant;
	handlers[RPN_SYM] = getSymbolValue;
	return handlers;
}();
// Compute an Expression's value from its RPN string.
// `error` reports whether any op in the expression encountered an error.
static int32_t computeRPNExpr(
    Expression const &expr,
    std::vector<Symbol> const &fileSymbols,
    bool *error = nullptr
) {
	RPNContext ctx(expr, fileSymbols);
	uint8_t const *rpnBuffer = expr.rpn.data();
	int32_t size = static_cast<int32_t>(expr.rpn.size());

	while (size > 0) {
		RPNCommand command = static_cast<RPNCommand>(ctx.getByte(rpnBuffer, size));
		ctx.isError = false;

		if (command >= rpnHandlers.size() || !rpnHandlers[command]) {
			invalidRPNCommand(ctx, command);
		}
		ctx.push(rpnHandlers[command](ctx, rpnBuffer, size));
	}

	if (ctx.stack.size() > 1) {
		ctx.rpnErrorAt("RPN stack has %zu entries on exit, not 1", ctx.stack.size());
	}

	// Only the final stack entry's error flag is reported to the caller.
	ctx.isError = false;
	int32_t value = ctx.pop();
	if (error) {
		*error = ctx.isError;
	}
	return value;
}

} // namespace

Assertion &patch_AddAssertion() {
	return assertions.emplace_front();
}

void patch_CheckAssertions() {
	verbosePrint(VERB_NOTICE, "Checking assertions...\n");

	for (Assertion &assert : assertions) {
		bool error;
		int32_t value = computeRPNExpr(assert.rpn, *assert.fileSymbols, &error);
		if (!error && !value) {
			switch (assert.type) {
			case ASSERT_FATAL:
				fatalAt(
				    assert.rpn,
				    "%s",
				    !assert.message.empty() ? assert.message.c_str() : "assert failure"
				);
			case ASSERT_ERROR: {
				RPNContext ctx(assert.rpn, *assert.fileSymbols);
				ctx.rpnErrorAt(
				    "%s",
				    !assert.message.empty() ? assert.message.c_str() : "assert failure"
				);
				break;
			}
			case ASSERT_WARN:
				warningAt(
				    assert.rpn,
				    WARNING_ASSERT,
				    "%s",
				    !assert.message.empty() ? assert.message.c_str() : "assert failure"
				);
				break;
			}
		} else if (error && assert.type == ASSERT_FATAL) {
			fatalAt(
			    assert.rpn,
			    "Failed to evaluate assertion%s%s",
			    !assert.message.empty() ? ": " : "",
			    assert.message.c_str()
			);
		}
	}
}

static void checkPatchSize(Expression const &expr, int32_t v, uint8_t n, bool &isError) {
	static std::vector<Symbol> const noSymbols;
	RPNContext ctx(expr, noSymbols);
	ctx.isError = isError;
	assume(n != 0);                         // That doesn't make sense
	assume(n < CHAR_BIT * sizeof(int) - 1); // Otherwise `1 << n` is UB

	if (v < -(1 << n) || v >= 1 << n) {
		ctx.diagnosticAt(
		    WARNING_TRUNCATION_1,
		    "Value $%" PRIx32 "%s is not %u-bit",
		    v,
		    v < 0 ? " (may be negative?)" : "",
		    n
		);
	} else if (v < -(1 << (n - 1))) {
		ctx.diagnosticAt(
		    WARNING_TRUNCATION_2,
		    "Value $%" PRIx32 "%s is not %u-bit",
		    v,
		    v < 0 ? " (may be negative?)" : "",
		    n
		);
	}
	isError = ctx.isError;
}

// Applies all of a section's patches to a data section
static void applyFilePatches(Section &section, Section &dataSection) {
	verbosePrint(VERB_INFO, "Patching section \"%s\"...\n", section.name.c_str());
	for (Patch &patch : section.patches) {
		bool error;
		int32_t value = computeRPNExpr(patch.rpn, *section.fileSymbols, &error);
		RPNContext ctx(patch.rpn, *section.fileSymbols);
		ctx.isError = error;
		uint32_t offset = patch.offset + section.offset;
		uint8_t typeSize = patch.typeSize();
		if (dataSection.data.size() < offset + typeSize) {
			ctx.rpnErrorAt(
			    "Patch would write %zu bytes past the end of section \"%s\" (%zu bytes long)",
			    offset + typeSize - dataSection.data.size(),
			    dataSection.name.c_str(),
			    dataSection.data.size()
			);
		} else if (patch.type == PATCHTYPE_JR) {
			if (!patch.rpn.pcSection) {
				ctx.rpnErrorAt("PC has no value outside of a section");
				dataSection.data[offset] = 0;
			} else {
				// A `jr` is *encoded* in ROM as a 1-byte (8-bit) offset, so here `typeSize == 1`,
				// but the object's *value* size is a 16-bit absolute address, so we pass 16 here.
				checkPatchSize(patch.rpn, value, 16, ctx.isError);
				// Offset is relative to the byte *after* the operand
				// PC as operand to `jr` is lower than reference PC by 2
				uint16_t address = patch.rpn.pcSection->org + patch.rpn.pcOffset + 2;
				// The 16-bit truncation of `value - address` is intentional, since
				// a low ROM0 address may `jr` backwards to a high HRAM one.
				int16_t jumpOffset = static_cast<int16_t>(value - address);

				if (jumpOffset < -128 || jumpOffset > 127) {
					ctx.firstErrorAt(
					    "`JR` target must be between -128 and 127 bytes away, not %" PRId16
					    "; use `JP` instead",
					    jumpOffset
					);
				}
				dataSection.data[offset] = jumpOffset & 0xFF;
			}
		} else {
			// Patch a certain number of bytes
			if (typeSize < sizeof(int)) {
				checkPatchSize(patch.rpn, value, typeSize * 8, ctx.isError);
			}
			for (uint8_t i = 0; i < typeSize; ++i) {
				dataSection.data[offset + i] = value & 0xFF;
				value >>= 8;
			}
		}
	}
}

// Applies all of a section's patches, iterating over "pieces" of unionized sections
static void applyPatches(Section &section) {
	if (!sectTypeHasData(section.type)) {
		return;
	}

	for (Section &piece : section.pieces()) {
		applyFilePatches(piece, section);
	}
}

void patch_ApplyPatches() {
	sect_ForEach(applyPatches);
}
