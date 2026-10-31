// The LCD artwork of the user's own Machinedrum OS file (UW OS 1.63 addresses): the six bitmap fonts, the dial ring, its plain ring,
// the group tie and the 128 dial dots, plus the two-state toggle icon this project draws itself. Used by tools/mdskin/mdartdump (the
// art.json the skin generators draw from) and by the plugin (vst/skin_recipe.h: the skin filled in on the device). Elektron's artwork:
// read from the user's file, never committed or shipped.
#pragma once
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace md::art
{
	struct Bmp { int w = 0, h = 0; std::vector<uint64_t> rows; };	// row r, column c: bit 63 - c of rows[r]

	struct Font
	{
		int h = 0, adv = 0;
		std::map<int, Bmp> glyphs;	// by character code
	};

	struct Art
	{
		std::map<std::string, Font> fonts;
		std::map<std::string, std::vector<Bmp>> bitmaps;	// one, or a list (dialDot: 128, toggle: 2)
	};

	namespace detail
	{
		constexpr uint32_t kBase = 0x200000;	// address of byte 0 of the main-processor section

		struct Image
		{
			const std::vector<uint8_t>& d;
			uint32_t u32(uint32_t a) const
			{
				if(a < kBase || size_t(a - kBase) + 4 > d.size()) throw std::runtime_error("address outside the OS image");
				const uint8_t* p = &d[a - kBase];
				return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
			}
			const uint8_t* at(uint32_t a, size_t n) const
			{
				if(a < kBase || size_t(a - kBase) + n > d.size()) throw std::runtime_error("address outside the OS image");
				return &d[a - kBase];
			}
		};

		// icon descriptor: w, h, n, pixel columns pointer, mask pointer; one u32 column per x, top row in the bit (32 - h)
		inline Bmp icon(const Image& im, uint32_t a, int wantW, int wantH)
		{
			const uint32_t w = im.u32(a), h = im.u32(a + 4), n = im.u32(a + 8), px = im.u32(a + 12), mask = im.u32(a + 16);
			if(int(w) != wantW || int(h) != wantH || n < 1 || n > 4 || mask != px + 4 * w) throw std::runtime_error("not the expected icon");
			Bmp b{int(w), int(h), std::vector<uint64_t>(h, 0)};
			for(uint32_t c = 0; c < w; ++c)
			{
				const uint32_t col = im.u32(px + 4 * c);
				for(uint32_t r = 0; r < h; ++r)
					if((col >> (32 - h + r)) & 1) b.rows[r] |= uint64_t(1) << (63 - c);
			}
			return b;
		}

		// font descriptor: advance, height, widths[256], offsets[128 x i16], one byte per glyph column (top row in bit 8-h)
		inline Font font(const Image& im, uint32_t a, int wantH)
		{
			const uint32_t adv = im.u32(a), h = im.u32(a + 4), widthsAt = im.u32(a + 8), offsetsAt = im.u32(a + 12), colsAt = im.u32(a + 16);
			if(int(h) != wantH || adv < 1 || adv > 16) throw std::runtime_error("not the expected font");
			const uint8_t* widths = im.at(widthsAt, 256);
			const uint8_t* offsets = im.at(offsetsAt, 512);
			Font f;
			f.h = int(h);
			f.adv = int(adv);
			for(int c = 0; c < 128; ++c)
			{
				const int off = int16_t((offsets[2 * c] << 8) | offsets[2 * c + 1]);
				if(off < 0) continue;
				const uint32_t w = widths[c] ? widths[c] : adv;
				if(w > 16) throw std::runtime_error("font glyph too wide");
				const uint8_t* cols = im.at(colsAt + uint32_t(off), w);
				Bmp b{int(w), int(h), std::vector<uint64_t>(h, 0)};
				for(uint32_t x = 0; x < w; ++x)
					for(uint32_t r = 0; r < h; ++r)
						if((cols[x] >> (8 - h + r)) & 1) b.rows[r] |= uint64_t(1) << (63 - x);
				f.glyphs[c] = b;
			}
			return f;
		}

		// The machine family badges: five boxed 23x13 icons (E-12, EFM, P-I, TRX, UW), found as five icon descriptors back to back
		// (the way ModuleDrum's MdMachineText.h readRomBadges finds them). Returned bare: the box cropped, 17x9.
		inline std::vector<Bmp> familyBadges(const Image& im)
		{
			const uint32_t end = kBase + uint32_t(im.d.size());
			auto badge = [&](uint32_t a) {
				if(a + 20 > end) return false;
				const uint32_t w = im.u32(a), h = im.u32(a + 4), px = im.u32(a + 12), mask = im.u32(a + 16);
				return w == 23 && h == 13 && px >= kBase && uint64_t(mask) == uint64_t(px) + 4 * w && uint64_t(mask) + 4 * w <= end;
			};
			for(uint32_t a = kBase; a + 100 <= end; a += 2)
			{
				bool all = true;
				for(uint32_t i = 0; i < 5 && all; ++i) all = badge(a + 20 * i);
				if(!all) continue;
				std::vector<Bmp> out;
				for(uint32_t i = 0; i < 5; ++i)
				{
					const Bmp full = icon(im, a + 20 * i, 23, 13);
					Bmp b{17, 9, std::vector<uint64_t>(9, 0)};
					for(int r = 0; r < 9; ++r) b.rows[size_t(r)] = (full.rows[size_t(r + 2)] << 3) & ~((uint64_t(1) << (64 - 17)) - 1);
					out.push_back(b);
				}
				return out;
			}
			throw std::runtime_error("family badges not found");
		}

		// The OFF/ON icon of the randomise cells: a ring with a pointer (up-left = off, up-right = on), drawn for this project.
		inline Bmp toggle(bool on)
		{
			Bmp b{17, 13, std::vector<uint64_t>(13, 0)};
			auto set = [&](int x, int y) { b.rows[size_t(y)] |= uint64_t(1) << (63 - x); };
			const char* ring[11] = {"...#####...", "..#.....#..", ".#.......#.", "#.........#", "#.........#", "#.........#",
			                        "#.........#", "#.........#", ".#.......#.", "..#.....#..", "...#####..."};
			for(int y = 0; y < 11; ++y)
				for(int x = 0; x < 11; ++x)
					if(ring[y][x] == '#') set(3 + x, 1 + y);
			for(int i = 0; i < 4; ++i) set(on ? 8 + 1 + i : 8 - 1 - i, 6 - i);
			return b;
		}
	}

	// From the OS's main-processor section (md::fw container section 0). Throws if it is not Machinedrum UW OS 1.63.
	inline Art read(const std::vector<uint8_t>& section0)
	{
		const detail::Image im{section0};
		Art a;
		a.fonts["tiny3x5"] = detail::font(im, 0x259954, 5);
		a.fonts["small4x5"] = detail::font(im, 0x259968, 5);
		a.fonts["square5x5"] = detail::font(im, 0x25997c, 5);
		a.fonts["bold8"] = detail::font(im, 0x259990, 8);
		a.fonts["digitsTop"] = detail::font(im, 0x2599a4, 5);
		a.fonts["digitsBottom"] = detail::font(im, 0x2599b8, 5);
		a.bitmaps["dialRing"] = {detail::icon(im, 0x2599e0, 11, 13)};
		a.bitmaps["ringPlain"] = {detail::icon(im, 0x2599cc, 11, 11)};
		a.bitmaps["groupTie"] = {detail::icon(im, 0x259af8, 7, 3)};
		for(int i = 0; i < 128; ++i) a.bitmaps["dialDot"].push_back(detail::icon(im, 0x259b0c + 20 * uint32_t(i), 7, 7));
		a.bitmaps["toggle"] = {detail::toggle(false), detail::toggle(true)};
		a.bitmaps["familyBadge"] = detail::familyBadges(im);	// E12 EFM P-I TRX UW
		return a;
	}

	// Every bit of the art in one fixed order, as tools/mdskin/skin_recipe.py's art_bits() walks art.json: fonts by name, their glyphs
	// by code, then bitmaps by name (lists in order); in each bitmap row by row, left to right.
	inline std::vector<uint8_t> bits(const Art& a)
	{
		std::vector<uint8_t> out;
		auto add = [&](const Bmp& b)
		{
			for(int y = 0; y < b.h; ++y)
				for(int x = 0; x < b.w; ++x) out.push_back(uint8_t((b.rows[size_t(y)] >> (63 - x)) & 1));
		};
		for(const auto& [name, f] : a.fonts)	// std::map: sorted by name, as Python's sorted()
			for(const auto& [code, g] : f.glyphs) add(g);
		for(const auto& [name, list] : a.bitmaps)
			for(const auto& b : list) add(b);
		return out;
	}
}
