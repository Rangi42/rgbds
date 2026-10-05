// SPDX-License-Identifier: MIT

#ifndef RGBDS_GFX_MAIN_HPP
#define RGBDS_GFX_MAIN_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

#include "helpers.hpp" // assume

#include "gfx/rgba.hpp"

// A contiguous range of tile IDs, all within a single VRAM bank
struct TileRegion {
	uint8_t bank;    // Which VRAM bank this region's tiles are in
	uint8_t first;   // The tile ID of this region's first tile
	uint32_t size;   // How many tiles this region contains
	uint32_t offset; // The global index of this region's first tile

	// The tile ID of this region's `ofs`th tile.
	// Like a nonzero `-b` base tile ID, this wraps around past 255.
	uint8_t tileID(uint32_t ofs) const { return first + ofs; }
	// The tile ID of this region's last tile, wrapping around past 255 as above
	uint8_t lastTileID() const { return first + (size - 1); }
	// This region's offset of a tile ID, if that tile ID falls within this region
	std::optional<uint32_t> offsetOf(uint8_t tileID) const {
		uint32_t ofs = static_cast<uint8_t>(tileID - first);
		return ofs < size ? std::optional<uint32_t>{ofs} : std::nullopt;
	}
};

// A contiguous range of palette IDs
struct PalRegion {
	uint8_t first;   // The palette ID of this region's first palette
	uint32_t size;   // How many palettes this region contains
	uint32_t offset; // The global index of this region's first palette

	// The palette ID of this region's `ofs`th palette.
	// Like a nonzero `-l` base palette ID, this wraps around past 255.
	uint8_t palID(uint32_t ofs) const { return first + ofs; }
	// The palette ID of this region's last palette, wrapping around past 255 as above
	uint8_t lastPalID() const { return first + (size - 1); }
	// This region's offset of a palette ID, if that palette ID falls within this region
	std::optional<uint32_t> offsetOf(uint8_t palID) const {
		uint32_t ofs = static_cast<uint8_t>(palID - first);
		return ofs < size ? std::optional<uint32_t>{ofs} : std::nullopt;
	}
};

// Where an ID ended up, or would end up, within a region
struct TilePlacement {
	TileRegion const *region; // The region containing (or which would contain) the tile ID
	uint32_t ofs;             // The tile's offset within its region
	uint32_t index() const { return region->offset + ofs; } // The tile's global index
};

struct PalPlacement {
	PalRegion const *region; // The region containing (or which would contain) the palette ID
	uint32_t ofs;            // The palette's offset within its region
	uint32_t index() const { return region->offset + ofs; } // The palette's global index
};

struct Options {
	bool useColorCurve = false;   // -C
	bool allowDedup = false;      // -u
	bool allowMirroringX = false; // -X, -m
	bool allowMirroringY = false; // -Y, -m
	bool columnMajor = false;     // -Z

	std::string attrmap{};         // -a, -A
	std::optional<Rgba> bgColor{}; // -B
	enum {
		NO_SPEC,
		INLINE,
		EXTERNAL,
		EMBEDDED,
		EMBEDDED_MULTIPLE,
		DMG,
	} palSpecType = NO_SPEC; // -c
	std::vector<std::array<std::optional<Rgba>, 4>> palSpec{};
	uint8_t palSpecDmg = 0;
	uint8_t bitDepth = 2;       // -d
	std::string inputTileset{}; // -i
	struct {
		uint16_t left;
		uint16_t top;
		uint16_t width;
		uint16_t height;
		uint32_t right() const { return left + width * 8; }
		uint32_t bottom() const { return top + height * 8; }
	} inputSlice{0, 0, 0, 0};   // -L (margins in clockwise order, like CSS)
	std::string output{};       // -o
	std::string palettes{};     // -p, -P
	std::string palmap{};       // -q, -Q
	uint16_t reversedWidth = 0; // -r, in tiles
	uint8_t nbColorsPerPal = 0; // -s; 0 means "auto" = 1 << bitDepth;
	std::string tilemap{};      // -t, -T
	uint64_t trim = 0;          // -x

	// The regions that tiles and palettes are output into, in the order that they are filled.
	// These are given by `-R` and `-S`; otherwise, they are computed from `-b`/`-N` and
	// `-l`/`-n`, respectively, once all the options have been parsed.
	std::vector<TileRegion> tileRegions{};
	std::vector<PalRegion> palRegions{};

	std::string input{}; // positional arg

	// The total capacities of the regions, in tiles and in palettes
	uint32_t maxNbTiles() const {
		return tileRegions.empty() ? 0 : tileRegions.back().offset + tileRegions.back().size;
	}
	uint32_t maxNbPalettes() const {
		return palRegions.empty() ? 0 : palRegions.back().offset + palRegions.back().size;
	}

	// The maximum number of tiles that can be output in each VRAM bank
	std::array<uint32_t, 2> maxNbTilesPerBank() const {
		std::array<uint32_t, 2> nbTiles{0, 0};
		for (TileRegion const &region : tileRegions) {
			nbTiles[region.bank] += region.size;
		}
		return nbTiles;
	}

	// The global index just past the last tile in a VRAM bank, or 0 if that bank has no regions.
	// This is only meaningful if a bank's regions all come before any other bank's, which is
	// how `-b`/`-N` always place them; with `-R`, use `findTile` instead.
	uint32_t endNbTilesInBank(uint8_t bank) const {
		uint32_t end = 0;
		for (TileRegion const &region : tileRegions) {
			if (region.bank == bank) {
				end = std::max(end, region.offset + region.size);
			}
		}
		return end;
	}

	// The region which contains the `index`th tile or palette, respectively.
	// `index` must be within the corresponding total capacity.
	TileRegion const &tileRegionAt(uint32_t index) const {
		assume(index < maxNbTiles());
		auto it = std::lower_bound(
		    tileRegions.begin(),
		    tileRegions.end(),
		    index,
		    [](TileRegion const &region, uint32_t idx) {
			    return region.offset + region.size <= idx;
		    }
		);
		assume(it != tileRegions.end());
		return *it;
	}
	PalRegion const &palRegionAt(uint32_t index) const {
		assume(index < maxNbPalettes());
		auto it = std::lower_bound(
		    palRegions.begin(),
		    palRegions.end(),
		    index,
		    [](PalRegion const &region, uint32_t idx) {
			    return region.offset + region.size <= idx;
		    }
		);
		assume(it != palRegions.end());
		return *it;
	}

	// Where the `index`th tile or palette will be placed
	std::pair<uint8_t, uint8_t> tilePlacementAt(uint32_t index) const {
		TileRegion const &region = tileRegionAt(index);
		return {region.bank, region.tileID(index - region.offset)};
	}
	uint8_t palPlacementAt(uint32_t index) const {
		PalRegion const &region = palRegionAt(index);
		return region.palID(index - region.offset);
	}

	// The tile ID that background tiles are emitted as referencing.
	// Background tiles are not part of the tile data, so they are emitted as if they used the
	// first region's tile ID in bank 0, regardless of which bank that region is really in.
	uint8_t backgroundTileID() const { return tileRegions.empty() ? 0 : tileRegions.front().first; }

	// Where a tile or palette ID is placed, if it is placed at all.
	// Tile IDs are only looked for in the specified VRAM bank.
	std::optional<TilePlacement> findTile(uint8_t bank, uint8_t tileID) const {
		for (TileRegion const &region : tileRegions) {
			if (region.bank == bank) {
				if (std::optional<uint32_t> ofs = region.offsetOf(tileID); ofs.has_value()) {
					return TilePlacement{&region, *ofs};
				}
			}
		}
		return std::nullopt;
	}
	std::optional<PalPlacement> findPalette(uint8_t palID) const {
		for (PalRegion const &region : palRegions) {
			if (std::optional<uint32_t> ofs = region.offsetOf(palID); ofs.has_value()) {
				return PalPlacement{&region, *ofs};
			}
		}
		return std::nullopt;
	}

	mutable bool hasTransparentPixels = false;
	uint8_t maxOpaqueColors() const { return nbColorsPerPal - hasTransparentPixels; }

	uint16_t maxNbColors() const { return nbColorsPerPal * maxNbPalettes(); }

	bool hasExplicitPalSpec() const { return palSpecType == INLINE || palSpecType == EXTERNAL; }
	bool hasEmbeddedPalSpec() const {
		return palSpecType == EMBEDDED || palSpecType == EMBEDDED_MULTIPLE;
	}

	uint8_t dmgColors[4] = {};
	uint8_t dmgValue(uint8_t i) const {
		assume(i < 4);
		return (palSpecDmg >> (2 * i)) & 0b11;
	}
};

extern Options options;

// Formats a list of capacities as "a + b + c", for diagnostics about total capacities
inline std::string sumString(std::vector<uint32_t> const &capacities) {
	std::string str;
	for (uint32_t capacity : capacities) {
		if (!str.empty()) {
			str += " + ";
		}
		str += std::to_string(capacity);
	}
	return str;
}

#endif // RGBDS_GFX_MAIN_HPP
