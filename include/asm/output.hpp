// SPDX-License-Identifier: MIT

#ifndef RGBDS_ASM_OUTPUT_HPP
#define RGBDS_ASM_OUTPUT_HPP

#include <memory>
#include <stdint.h>
#include <string>
#include <vector>

#include "linkdefs.hpp"

struct Expression;
struct FileStackNode;
struct Symbol;

enum StateFeature { STATE_EQU, STATE_VAR, STATE_EQUS, STATE_CHAR, STATE_MACRO, NB_STATE_FEATURES };

struct StateFeatureInfo {
	char const *name;    // Lowercase, as printed by `--help`
	char const *heading; // Comment heading in state files
};

static constexpr StateFeatureInfo stateFeatureInfo[NB_STATE_FEATURES] = {
    {"equ",   "Numeric constants"},
    {"var",   "Variables"        },
    {"equs",  "String constants" },
    {"char",  "Character maps"   },
    {"macro", "Macros"           },
};

void out_RegisterNode(std::shared_ptr<FileStackNode> node);
void out_RegisterSymbol(Symbol &sym);
void out_CreatePatch(PatchType type, Expression const &expr, uint32_t ofs, uint32_t pcShift);
void out_CreateAssert(
    AssertionType type, Expression const &expr, std::string const &message, uint32_t ofs
);
void out_WriteObject();
void out_WriteState(std::string name, std::vector<StateFeature> const &features);

#endif // RGBDS_ASM_OUTPUT_HPP
