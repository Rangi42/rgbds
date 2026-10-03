// SPDX-License-Identifier: MIT

#ifndef RGBDS_GFX_PALETTE_HPP
#define RGBDS_GFX_PALETTE_HPP

#include <array>
#include <stddef.h>
#include <stdint.h>

#include "gfx/main.hpp" // MAX_COLORS_PER_PAL

struct Palette {
	// An array of `MAX_COLORS_PER_PAL` GBC-native (RGB555) colors; how many are actually used
	// depends on the bit depth (`-d`), the rest being left as UINT16_MAX
	std::array<uint16_t, MAX_COLORS_PER_PAL> colors{
	    UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX,
	    UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX,
	    UINT16_MAX, UINT16_MAX
	};

	void addColor(uint16_t color);
	uint8_t indexOf(uint16_t color) const;
	uint16_t &operator[](size_t index) { return colors[index]; }
	uint16_t const &operator[](size_t index) const { return colors[index]; }

	decltype(colors)::iterator begin();
	decltype(colors)::iterator end();
	decltype(colors)::const_iterator begin() const;
	decltype(colors)::const_iterator end() const;

	uint8_t size() const;
};

#endif // RGBDS_GFX_PALETTE_HPP
