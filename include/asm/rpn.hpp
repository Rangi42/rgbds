// SPDX-License-Identifier: MIT

#ifndef RGBDS_ASM_RPN_HPP
#define RGBDS_ASM_RPN_HPP

#include <stdint.h>
#include <string>
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "linkdefs.hpp"

#include "asm/intern.hpp"

struct Symbol;

struct RPNValue {
	RPNCommand command;                                                // The RPN_* command ID
	std::variant<std::monostate, uint8_t, uint32_t, InternedStr> data; // Data after the ID, if any

	RPNValue(RPNCommand cmd);
	RPNValue(RPNCommand cmd, uint8_t val);
	RPNValue(RPNCommand cmd, uint32_t val);
	RPNValue(RPNCommand cmd, InternedStr name);

	void appendEncoded(std::vector<uint8_t> &buffer) const;
};

struct Expression {
	std::variant<
	    int32_t,    // If the expression's value is known, it's here
	    std::string // Why the expression is not known, if it isn't
	    >
	    data = 0;
	std::vector<RPNValue> rpn{}; // Values to be serialized into the RPN expression

	bool isKnown() const { return std::holds_alternative<int32_t>(data); }
	int32_t value() const { return std::get<int32_t>(data); }

	int32_t getConstVal() const;
	Symbol const *symbolOf() const;
	bool isDiffConstant(Symbol const *symName) const;

	void makeNumber(uint32_t value);
	void makeSymbol(InternedStr symName);
	void makeBankSymbol(InternedStr symName);
	void makeBankSection(std::string const &sectName);
	void makeSizeOfSection(std::string const &sectName);
	void makeStartOfSection(std::string const &sectName);
	void makeSizeOfSectionType(SectionType type);
	void makeStartOfSectionType(SectionType type);
	void makeUnaryOp(RPNCommand op, Expression &&src);
	void makeBinaryOp(RPNCommand op, Expression &&src1, Expression const &src2);

	void addCheckHRAM();
	void addCheckRST();
	void addCheckBitIndex(uint8_t mask);

	void checkNBit(uint8_t n) const;
	void checkSignedNBit(uint8_t n) const;

	void encode(std::vector<uint8_t> &buffer) const;
};


struct NumExpr {
    std::shared_ptr<std::function<Expression()>> fn;
    mutable std::shared_ptr<std::optional<Expression>> cache;

    NumExpr() : fn(std::make_shared<std::function<Expression()>>([]() { Expression e; e.makeNumber(0); return e; })), cache(std::make_shared<std::optional<Expression>>()) {}
    NumExpr(std::function<Expression()> f) : fn(std::make_shared<std::function<Expression()>>(std::move(f))), cache(std::make_shared<std::optional<Expression>>()) {}
    NumExpr(Expression e) : NumExpr([e = std::move(e)]() mutable { return std::move(e); }) {}

    Expression eval() const {
        if (!(*cache)) {
            *cache = (*fn)();
        }
        return **cache;
    }

    bool isKnown() const { return eval().isKnown(); }
    int32_t value() const { return eval().value(); }
    int32_t getConstVal() const { return eval().getConstVal(); }
    Symbol const *symbolOf() const { return eval().symbolOf(); }
    bool isDiffConstant(Symbol const *sym) const { return eval().isDiffConstant(sym); }

    void makeNumber(uint32_t value);
    void makeSymbol(InternedStr symName);
    void makeBankSymbol(InternedStr symName);
    void makeBankSection(std::string const &sectName);
    void makeSizeOfSection(std::string const &sectName);
    void makeStartOfSection(std::string const &sectName);
    void makeSizeOfSectionType(SectionType type);
    void makeStartOfSectionType(SectionType type);
    void makeUnaryOp(RPNCommand op, NumExpr &&src);
    void makeBinaryOp(RPNCommand op, NumExpr &&src1, NumExpr const &src2);

    void addCheckHRAM();
    void addCheckRST();
    void addCheckBitIndex(uint8_t mask);

    void checkNBit(uint8_t n) const;
    void checkSignedNBit(uint8_t n) const;

    void encode(std::vector<uint8_t> &buffer) const { eval().encode(buffer); }
    operator Expression() const { return eval(); }
};

struct StrExpr {
    std::shared_ptr<std::function<std::string()>> fn;
    mutable std::shared_ptr<std::optional<std::string>> cache;

    StrExpr() : fn(std::make_shared<std::function<std::string()>>([]() { return std::string(); })), cache(std::make_shared<std::optional<std::string>>()) {}
    StrExpr(std::function<std::string()> f) : fn(std::make_shared<std::function<std::string()>>(std::move(f))), cache(std::make_shared<std::optional<std::string>>()) {}

    std::string eval() const {
        if (!(*cache)) {
            *cache = (*fn)();
        }
        return **cache;
    }
    operator std::string() const { return eval(); }
};

bool checkNBit(int32_t v, uint8_t n, char const *name);
bool checkSignedNBit(int32_t v, uint8_t n, char const *name);

#endif // RGBDS_ASM_RPN_HPP
