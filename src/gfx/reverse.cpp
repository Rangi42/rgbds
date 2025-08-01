// SPDX-License-Identifier: MIT

#include "gfx/reverse.hpp"

#include <algorithm>
#include <array>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <optional>
#include <png.h>
#include <string.h>
#include <vector>

#include "diagnostics.hpp"
#include "file.hpp"
#include "helpers.hpp" // assume

#include "gfx/main.hpp"
#include "gfx/warning.hpp"

static std::vector<uint8_t> readInto(std::string const &path) {
	File file;
	if (!file.open(path, std::ios::in | std::ios::binary)) {
		fatal("Failed to open \"%s\": %s", file.c_str(path), strerror(errno));
	}
	std::vector<uint8_t> data(128 * 16); // Begin with some room pre-allocated

	size_t curSize = 0;
	for (;;) {
		size_t oldSize = curSize;
		curSize = data.size();

		// Fill the new area ([oldSize; curSize[) with bytes
		size_t nbRead =
		    file->sgetn(reinterpret_cast<char *>(&data.data()[oldSize]), curSize - oldSize);
		if (nbRead != curSize - oldSize) {
			// Shrink the vector to discard bytes that weren't read
			data.resize(oldSize + nbRead);
			break;
		}
		// If the vector has some capacity left, use it; otherwise, double the current size

		// Arbitrary, but if you got a better idea...
		size_t newSize = oldSize != data.capacity() ? data.capacity() : oldSize * 2;
		assume(oldSize != newSize);
		data.resize(newSize);
	}

	return data;
}

[[noreturn]]
static void pngError(png_structp png, char const *msg) {
	fatal(
	    "Error writing reversed image (\"%s\"): %s",
	    static_cast<char const *>(png_get_error_ptr(png)),
	    msg
	);
}

static void pngWarning(png_structp png, char const *msg) {
	warnx(
	    "While writing reversed image (\"%s\"): %s",
	    static_cast<char const *>(png_get_error_ptr(png)),
	    msg
	);
}

static void writePng(png_structp png, png_bytep data, size_t length) {
	File &pngFile = *static_cast<File *>(png_get_io_ptr(png));
	pngFile->sputn(reinterpret_cast<char *>(data), length);
}

static void flushPng(png_structp png) {
	File &pngFile = *static_cast<File *>(png_get_io_ptr(png));
	pngFile->pubsync();
}

static void printColor(std::optional<Rgba> const &color) {
	if (color) {
		fprintf(stderr, "#%08x", color->toCSS());
	} else {
		fputs("<none>   ", stderr);
	}
}

static void printPalette(std::array<std::optional<Rgba>, 4> const &palette) {
	putc('[', stderr);
	printColor(palette[0]);
	fputs(", ", stderr);
	printColor(palette[1]);
	fputs(", ", stderr);
	printColor(palette[2]);
	fputs(", ", stderr);
	printColor(palette[3]);
	putc(']', stderr);
}

void reverse() {
	options.verbosePrint(Options::VERB_CFG, "Using libpng %s\n", png_get_libpng_ver(nullptr));

	// Check for weird flag combinations

	if (options.output.empty()) {
		fatal("Tile data must be provided when reversing an image!");
	}

	if (options.allowDedup && options.tilemap.empty()) {
		warnx("Tile deduplication is enabled, but no tilemap is provided?");
	}

	if (options.useColorCurve) {
		warnx("The color curve is not yet supported in reverse mode...");
	}

	if (options.inputSlice.left != 0 || options.inputSlice.top != 0
	    || options.inputSlice.height != 0) {
		warnx("\"Sliced-off\" pixels are ignored in reverse mode");
	}
	if (options.inputSlice.width != 0 && options.inputSlice.width != options.reversedWidth * 8) {
		warnx(
		    "Specified input slice width (%" PRIu16
		    ") doesn't match provided reversing width (%" PRIu16 " * 8)",
		    options.inputSlice.width,
		    options.reversedWidth
		);
	}

	options.verbosePrint(Options::VERB_LOG_ACT, "Reading tiles...\n");
	std::vector<uint8_t> const tiles = readInto(options.output);
	uint8_t tileSize = 8 * options.bitDepth;
	if (tiles.size() % tileSize != 0) {
		fatal(
		    "Tile data size (%zu bytes) is not a multiple of %" PRIu8 " bytes",
		    tiles.size(),
		    tileSize
		);
	}

	// By default, assume tiles are not deduplicated, and add the (allegedly) trimmed tiles
	size_t const nbTiles = tiles.size() / tileSize;
	options.verbosePrint(Options::VERB_INTERM, "Read %zu tiles.\n", nbTiles);
	size_t mapSize = nbTiles + options.trim; // Image size in tiles
	std::optional<std::vector<uint8_t>> tilemap;
	if (!options.tilemap.empty()) {
		tilemap = readInto(options.tilemap);
		mapSize = tilemap->size();
		options.verbosePrint(Options::VERB_INTERM, "Read %zu tilemap entries.\n", mapSize);
	}

	if (mapSize == 0) {
		fatal("Cannot generate empty image");
	}
	if (mapSize > options.maxNbTiles[0] + options.maxNbTiles[1]) {
		warnx(
		    "Total number of tiles (%zu) is more than the limit of %" PRIu16 " + %" PRIu16,
		    mapSize,
		    options.maxNbTiles[0],
		    options.maxNbTiles[1]
		);
	}

	size_t width = options.reversedWidth, height; // In tiles
	if (width == 0) {
		// Pick the smallest width that will result in a landscape-aspect rectangular image.
		// Thus a prime number of tiles will result in a horizontal row.
		// This avoids redundancy with `-r 1` which results in a vertical column.
		width = static_cast<size_t>(ceil(sqrt(mapSize)));
		for (; width < mapSize; ++width) {
			if (mapSize % width == 0) {
				break;
			}
		}
		options.verbosePrint(Options::VERB_INTERM, "Picked reversing width of %zu tiles\n", width);
	}
	if (mapSize % width != 0) {
		if (options.trim == 0 && !tilemap) {
			fatal(
			    "Total number of tiles (%zu) cannot be divided by image width (%zu tiles)\n"
			    "(To proceed anyway with this image width, try passing `-x %zu`)",
			    mapSize,
			    width,
			    width - mapSize % width
			);
		}
		fatal(
		    "Total number of tiles (%zu) cannot be divided by image width (%zu tiles)",
		    mapSize,
		    width
		);
	}
	height = mapSize / width;

	options.verbosePrint(
	    Options::VERB_INTERM, "Reversed image dimensions: %zux%zu tiles\n", width, height
	);

	Rgba const grayColors[4] = {
	    Rgba(0xFFFFFFFF), Rgba(0xAAAAAAFF), Rgba(0x555555FF), Rgba(0x000000FF)
	};
	std::vector<std::array<std::optional<Rgba>, 4>> palettes{
	    {grayColors[0], grayColors[1], grayColors[2], grayColors[3]}
	};
	// If a palette file or palette spec is used as input, it overrides the default colors.
	if (!options.palettes.empty()) {
		File file;
		if (!file.open(options.palettes, std::ios::in | std::ios::binary)) {
			fatal("Failed to open \"%s\": %s", file.c_str(options.palettes), strerror(errno));
		}

		palettes.clear();
		std::array<uint8_t, sizeof(uint16_t) * 4> buf; // 4 colors
		for (;;) {
			if (size_t nbRead = file->sgetn(reinterpret_cast<char *>(buf.data()), buf.size());
			    nbRead == 0) {
				break;
			} else if (nbRead != buf.size()) {
				fatal(
				    "Palette data size (%zu) is not a multiple of %zu bytes!\n",
				    palettes.size() * buf.size() + nbRead,
				    buf.size()
				);
			}
			// Expand the colors
			auto &palette = palettes.emplace_back();
			std::generate(
			    palette.begin(),
			    palette.begin() + options.nbColorsPerPal,
			    [&buf, i = 0]() mutable {
				    i += 2;
				    return Rgba::fromCGBColor(buf[i - 2] + (buf[i - 1] << 8));
			    }
			);
		}

		if (palettes.size() > options.nbPalettes) {
			warnx(
			    "Read %zu palettes, more than the specified limit of %" PRIu16,
			    palettes.size(),
			    options.nbPalettes
			);
		}

		if (options.palSpecType == Options::EXPLICIT && palettes != options.palSpec) {
			warnx("Colors in the palette file do not match those specified with `-c`!");
			// This spacing aligns "...versus with `-c`" above the column of `-c` palettes
			fputs("Colors specified in the palette file:         ...versus with `-c`:\n", stderr);
			for (size_t i = 0; i < palettes.size() && i < options.palSpec.size(); ++i) {
				if (i < palettes.size()) {
					printPalette(palettes[i]);
				} else {
					fputs("                                            ", stderr);
				}
				if (i < options.palSpec.size()) {
					fputs("  ", stderr);
					printPalette(options.palSpec[i]);
				}
				putc('\n', stderr);
			}
		}
	} else if (options.palSpecType == Options::DMG) {
		for (size_t i = 0; i < palettes[0].size(); ++i) {
			palettes[0][i] = grayColors[options.dmgValue(i)];
		}
	} else if (options.palSpecType == Options::EMBEDDED) {
		warnx("An embedded palette was requested, but no palette file was specified; ignoring "
		      "request.");
	} else if (options.palSpecType == Options::EXPLICIT) {
		palettes = std::move(options.palSpec); // We won't be using it again.
	}

	std::optional<std::vector<uint8_t>> attrmap;
	uint16_t nbTilesInBank[2] = {0, 0}; // Only used if there is an attrmap.
	if (!options.attrmap.empty()) {
		attrmap = readInto(options.attrmap);
		if (attrmap->size() != mapSize) {
			fatal(
			    "Attribute map size (%zu tiles) doesn't match image's (%zu)",
			    attrmap->size(),
			    mapSize
			);
		}

		// Scan through the attributes for inconsistencies
		// We do this now for two reasons:
		// 1. Checking those during the main loop is harmful to optimization, and
		// 2. It clutters the code more, and it's not in great shape to begin with
		for (size_t index = 0; index < mapSize; ++index) {
			uint8_t attr = (*attrmap)[index];
			size_t tx = index % width, ty = index / width;

			if (uint8_t palID = (attr & 0b111) - options.basePalID; palID > palettes.size()) {
				error(
				    "Attribute map references palette #%u at (%zu, %zu), but there are only %zu!",
				    palID,
				    tx,
				    ty,
				    palettes.size()
				);
			}

			bool bank = attr & 0b1000;

			if (!tilemap) {
				if (bank) {
					warnx(
					    "Attribute map assigns tile at (%zu, %zu) to bank 1, but no tilemap "
					    "specified; "
					    "ignoring the bank bit",
					    tx,
					    ty
					);
				}
			} else {
				if (uint8_t tileOfs = (*tilemap)[index] - options.baseTileIDs[bank];
				    tileOfs >= nbTilesInBank[bank]) {
					nbTilesInBank[bank] = tileOfs + 1;
				}
			}
		}

		options.verbosePrint(
		    Options::VERB_INTERM,
		    "Number of tiles in bank {0: %" PRIu16 ", 1: %" PRIu16 "}\n",
		    nbTilesInBank[0],
		    nbTilesInBank[1]
		);

		for (int bank = 0; bank < 2; ++bank) {
			if (nbTilesInBank[bank] > options.maxNbTiles[bank]) {
				error(
				    "Bank %d contains %" PRIu16 " tiles, but the specified limit is %" PRIu16,
				    bank,
				    nbTilesInBank[bank],
				    options.maxNbTiles[bank]
				);
			}
		}

		if (nbTilesInBank[0] + nbTilesInBank[1] > nbTiles) {
			fatal(
			    "The tilemap references %" PRIu16 " tiles in bank 0 and %" PRIu16
			    " in bank 1, but only %zu have been read in total",
			    nbTilesInBank[0],
			    nbTilesInBank[1],
			    nbTiles
			);
		}

		requireZeroErrors();
	}

	if (tilemap) {
		if (attrmap) {
			for (size_t index = 0; index < mapSize; ++index) {
				size_t tx = index % width, ty = index / width;
				uint8_t tileID = (*tilemap)[index];
				uint8_t attr = (*attrmap)[index];
				bool bank = attr & 0b1000;

				if (uint8_t tileOfs = tileID - options.baseTileIDs[bank];
				    tileOfs >= options.maxNbTiles[bank]) {
					error(
					    "Tilemap references tile #%" PRIu8
					    " at (%zu, %zu), but the limit for bank %u is %" PRIu16,
					    tileID,
					    tx,
					    ty,
					    bank,
					    options.maxNbTiles[bank]
					);
				}
			}
		} else {
			size_t const limit = std::min<size_t>(nbTiles, options.maxNbTiles[0]);
			for (size_t index = 0; index < mapSize; ++index) {
				if (uint8_t tileID = (*tilemap)[index];
				    static_cast<uint8_t>(tileID - options.baseTileIDs[0]) >= limit) {
					size_t tx = index % width, ty = index / width;
					error(
					    "Tilemap references tile #%" PRIu8 " at (%zu, %zu), but the limit is %zu",
					    tileID,
					    tx,
					    ty,
					    limit
					);
				}
			}
		}

		requireZeroErrors();
	}

	std::optional<std::vector<uint8_t>> palmap;
	if (!options.palmap.empty()) {
		palmap = readInto(options.palmap);
		if (palmap->size() != mapSize) {
			fatal(
			    "Palette map size (%zu tiles) doesn't match image size (%zu)",
			    palmap->size(),
			    mapSize
			);
		}
	}

	options.verbosePrint(Options::VERB_LOG_ACT, "Writing image...\n");
	File pngFile;
	if (!pngFile.open(options.input, std::ios::out | std::ios::binary)) {
		// LCOV_EXCL_START
		fatal("Failed to create \"%s\": %s", pngFile.c_str(options.input), strerror(errno));
		// LCOV_EXCL_STOP
	}
	png_structp png = png_create_write_struct(
	    PNG_LIBPNG_VER_STRING,
	    const_cast<png_voidp>(static_cast<void const *>(pngFile.c_str(options.input))),
	    pngError,
	    pngWarning
	);
	if (!png) {
		// LCOV_EXCL_START
		fatal("Failed to create PNG write struct: %s", strerror(errno));
		// LCOV_EXCL_STOP
	}
	png_infop pngInfo = png_create_info_struct(png);
	if (!pngInfo) {
		// LCOV_EXCL_START
		fatal("Failed to create PNG info structure: %s", strerror(errno));
		// LCOV_EXCL_STOP
	}
	png_set_write_fn(png, &pngFile, writePng, flushPng);

	int pngColorType = options.palettes.empty() ? PNG_COLOR_TYPE_GRAY
	                   : palettes.size() == 1   ? PNG_COLOR_TYPE_PALETTE
	                                            : PNG_COLOR_TYPE_RGB_ALPHA;
	int pngDepth = options.palettes.empty() ? options.bitDepth : 8;

	png_set_IHDR(
	    png,
	    pngInfo,
	    width * 8,
	    height * 8,
	    pngDepth,
	    pngColorType,
	    PNG_INTERLACE_NONE,
	    PNG_COMPRESSION_TYPE_DEFAULT,
	    PNG_FILTER_TYPE_DEFAULT
	);

	if (pngColorType != PNG_COLOR_TYPE_GRAY) {
		png_color_8 sbitChunk;
		sbitChunk.red = 5;
		sbitChunk.green = 5;
		sbitChunk.blue = 5;
		if (pngColorType == PNG_COLOR_TYPE_RGB_ALPHA) {
			sbitChunk.alpha = 1;
		}
		png_set_sBIT(png, pngInfo, &sbitChunk);
	}

	if (pngColorType == PNG_COLOR_TYPE_PALETTE) {
		assume(palettes.size() == 1);
		png_color pngPalette[4] = {};
		png_byte pngTrans[4] = {};
		int nbPngColors = 0, nbPngTrans = 0;
		for (auto const &color : palettes[0]) {
			if (!color.has_value()) {
				continue;
			}
			pngPalette[nbPngColors].red = color->red;
			pngPalette[nbPngColors].green = color->green;
			pngPalette[nbPngColors].blue = color->blue;
			pngTrans[nbPngColors] = color->alpha;
			++nbPngColors;
			if (color->alpha < 255) {
				nbPngTrans = nbPngColors;
			}
		}
		png_set_PLTE(png, pngInfo, pngPalette, nbPngColors);
		if (nbPngTrans > 0) {
			png_set_tRNS(png, pngInfo, pngTrans, nbPngTrans, nullptr);
		}
	}

	png_write_info(png, pngInfo);

	// N bits/pixel * 8 pixels/tile row / 8 bits/byte = N bytes/tile row
	uint8_t const bytesPerTileRow = pngColorType == PNG_COLOR_TYPE_RGB_ALPHA ? 32 : pngDepth;
	size_t const bytesPerRow = width * bytesPerTileRow;
	std::vector<uint8_t> tileRow(8 * bytesPerRow, 0xFF); // Data for 8 rows of pixels
	uint8_t * const rowPtrs[8] = {
	    &tileRow.data()[0 * bytesPerRow],
	    &tileRow.data()[1 * bytesPerRow],
	    &tileRow.data()[2 * bytesPerRow],
	    &tileRow.data()[3 * bytesPerRow],
	    &tileRow.data()[4 * bytesPerRow],
	    &tileRow.data()[5 * bytesPerRow],
	    &tileRow.data()[6 * bytesPerRow],
	    &tileRow.data()[7 * bytesPerRow],
	};

	for (size_t ty = 0; ty < height; ++ty) {
		for (size_t tx = 0; tx < width; ++tx) {
			size_t index = options.columnMajor ? ty + tx * height : ty * width + tx;
			// By default, a tile is unflipped, in bank 0, and uses palette #0
			uint8_t attribute = attrmap ? (*attrmap)[index] : 0b0000;
			bool bank = attribute & 0b1000;
			// Get the tile ID at this location
			size_t tileOfs =
			    tilemap ? static_cast<uint8_t>((*tilemap)[index] - options.baseTileIDs[bank])
			                  + (bank ? nbTilesInBank[0] : 0)
			            : index;
			// This should have been enforced by the earlier checking.
			assume(tileOfs < nbTiles + options.trim);
			size_t palID = (palmap ? (*palmap)[index] : attribute & 0b111) - options.basePalID;
			assume(palID < palettes.size()); // Should be ensured on data read

			// We do not have data for tiles trimmed with `-x`, so assume they are "blank"
			static std::array<uint8_t, 16> const trimmedTile{0x00};
			uint8_t const *tileData =
			    tileOfs >= nbTiles ? trimmedTile.data() : &tiles[tileOfs * tileSize];
			auto const &palette = palettes[palID];
			for (uint8_t y = 0; y < 8; ++y) {
				// If vertically mirrored, fetch the bytes from the other end
				uint8_t realY = (attribute & 0x40 ? 7 - y : y) * options.bitDepth;
				uint8_t bitplane0 = tileData[realY];
				uint8_t bitplane1 = tileData[realY + 1 % options.bitDepth];
				if (attribute & 0x20) { // Handle horizontal flip
					bitplane0 = flipTable[bitplane0];
					bitplane1 = flipTable[bitplane1];
				}

				uint8_t *ptr = &rowPtrs[y][tx * bytesPerTileRow];
				uint16_t gray = 0;
				for (uint8_t x = 0; x < 8; ++x) {
					uint8_t bit0 = bitplane0 & 0x80, bit1 = bitplane1 & 0x80;
					uint8_t colorID = bit0 >> 7 | bit1 >> 6;
					Rgba const &pixel = *palette[colorID];

					if (pngColorType == PNG_COLOR_TYPE_GRAY) {
						gray = gray << pngDepth | (pixel.red & ((1 << pngDepth) - 1));
					} else if (pngColorType == PNG_COLOR_TYPE_PALETTE) {
						*ptr++ = palID * 4 + colorID;
					} else {
						*ptr++ = pixel.red;
						*ptr++ = pixel.green;
						*ptr++ = pixel.blue;
						*ptr++ = pixel.alpha;
					}

					// Shift the pixel out
					bitplane0 <<= 1;
					bitplane1 <<= 1;
				}

				if (pngDepth == 1) {
					*ptr = gray;
				} else if (pngDepth == 2) {
					*ptr++ = gray >> 8;
					*ptr = gray & 0xff;
				}
			}
		}
		// We never modify the pointers, and neither should libpng, despite the overly lax function
		// signature.
		// (AIUI, casting away const-ness is okay as long as you don't actually modify the
		// pointed-to data)
		png_write_rows(png, const_cast<png_bytepp>(rowPtrs), 8);
	}

	// Finalize the write
	png_write_end(png, pngInfo);

	png_destroy_write_struct(&png, &pngInfo);
}
