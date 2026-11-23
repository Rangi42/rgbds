// SPDX-License-Identifier: MIT

#ifndef RGBDS_FIX_MAIN_HPP
#define RGBDS_FIX_MAIN_HPP

#include <optional>
#include <stddef.h>
#include <stdint.h>
#include <string>

#include "fix/mbc.hpp" // UNSPECIFIED, MbcType

// clang-format off: vertically align values
static constexpr uint8_t FIX_LOGO         = 1 << 7;
static constexpr uint8_t TRASH_LOGO       = 1 << 6;
static constexpr uint8_t FIX_HEADER_SUM   = 1 << 5;
static constexpr uint8_t TRASH_HEADER_SUM = 1 << 4;
static constexpr uint8_t FIX_GLOBAL_SUM   = 1 << 3;
static constexpr uint8_t TRASH_GLOBAL_SUM = 1 << 2;
// clang-format on

enum Model { DMG, BOTH, CGB };

// A header string option, together with the length to actually write to the ROM header.
// `value` keeps the original argument so truncation warnings can report it in full.
template<size_t MaxLength>
struct HeaderString {
	static constexpr uint8_t maxLength = MaxLength;

	std::optional<std::string> value;
	uint8_t len = 0;

	explicit operator bool() const { return value.has_value(); }

	char const *c_str() const { return value->c_str(); }
	size_t length() const { return value->length(); }
	uint8_t size() const { return len; }

	void assign(char const *arg) {
		value = arg;
		size_t argLen = value->length();
		len = static_cast<uint8_t>(argLen < MaxLength ? argLen : MaxLength);
	}

	bool truncate(uint8_t newLen) {
		if (len <= newLen) {
			return false;
		}
		len = newLen;
		return true;
	}
};

struct Options {
	uint8_t fixSpec = 0;                // -f, -v
	Model model = DMG;                  // -C, -c
	bool japanese = true;               // -j
	uint16_t oldLicensee = UNSPECIFIED; // -l
	uint16_t romVersion = UNSPECIFIED;  // -n
	uint16_t padValue = UNSPECIFIED;    // -p
	uint16_t ramSize = UNSPECIFIED;     // -r
	bool sgb = false;                   // -s

	HeaderString<4> gameID;        // -i
	HeaderString<2> newLicensee;   // -k

	std::optional<std::string> logoFilename; // -L
	uint8_t logo[48] = {};

	MbcType cartridgeType = MBC_NONE; // -m
	uint8_t tpp1Rev[2];

	HeaderString<16> title; // -t
};

extern Options options;

#endif // RGBDS_FIX_MAIN_HPP
