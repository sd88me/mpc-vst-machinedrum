// The LCD artwork of the UI: six bitmap fonts, the dial ring and its 128 pointer frames, the plain ring and the group tie.
// None of it is part of this source tree or of the binaries: it is read out of the user's own Machinedrum OS file when that
// file is selected (ensureRomArt), from the places where UW OS 1.63 keeps it (the same addresses tools/mdskin/mdartdump.cpp
// reads for the MPC skin; the Machinedrum and Monomachine OS share the Elektron LCD font family). Until an OS file is present,
// and for an image that does not validate, the UI draws with a plain built-in stand-in face.
//
// Adapted from shnolk's Monomodule (https://github.com/shnolk/monomodule, src/plugin/one/RomArt.*), AGPL-3.0: the Bitmap/Font/
// Art types and the stand-in face are kept; the addresses and the set of artwork are the Machinedrum's.
//
// Threading: the artwork is read while painting and written by an install, both without locks. Install only from the thread
// that paints (the message thread). Storage of an earlier install is never freed, so a Bitmap pointer taken before an install
// stays valid.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mm::uispec {

// 1-bit image; row r, pixel x is lit when bit (63 - x) of rows[r] is set (row 0 = top).
struct Bitmap {
	uint8_t w = 0, h = 0;
	const uint64_t* rows = nullptr;
	bool lit(int x, int y) const { return (rows[y] >> (63 - x)) & 1u; }
};

// Proportional bitmap font: glyph(c) is null for a code without a glyph (advance by adv then).
struct Font {
	uint8_t h = 0, adv = 0;
	const Bitmap* bitmaps = nullptr;
	const int16_t* index = nullptr;	// 128 entries, -1 = no glyph
	const Bitmap* glyph(int c) const { return c >= 0 && c < 128 && index[c] >= 0 ? &bitmaps[index[c]] : nullptr; }
};

struct Art
{
	Font bold8, small4x5, tiny3x5, square5x5, digitsTop, digitsBottom;
	Bitmap dialRing;	// 11x13
	Bitmap groupTie;	// 7x3
	Bitmap ringPlain;	// 11x11
	const Bitmap* dialDot[128] = {};	// 7x7 pointer frame per raw value, composite at (2, 4); never null
	Bitmap toggle[2];	// the two-state icon (OFF / ON), drawn for this project (not in the OS file)
	// The master effects' 32 parameter labels in the kit's byte order (reverb, echo, EQ, dynamix, 8 each), from the OS's own table;
	// null until an OS file is read.
	const char* masterLabel[32] = {};
	bool fromRom = false;
};

// The artwork in use: the stand-in until an install succeeds. The object itself never moves.
Art& art();

// Reads the artwork out of a decompressed main OS image (section 0 of the OS file, load base 0x200000) and makes it the
// artwork in use. Every descriptor is bounds- and shape-checked first; on failure nothing changes and `error` says what did not fit.
bool installRomArt(const std::vector<uint8_t>& mainOs, std::string* error = nullptr);

// installRomArt for an OS file on disk. Does nothing (and returns true) when the artwork in use already came from this file
// (same path, size and modification time), so it is cheap to call on every editor open.
bool ensureRomArt(const std::filesystem::path& osFile, std::string* error = nullptr);

inline const Font& kFontBold8() { return art().bold8; }
inline const Font& kFontSmall4x5() { return art().small4x5; }
inline const Font& kFontTiny3x5() { return art().tiny3x5; }
inline const Font& kFontSquare5x5() { return art().square5x5; }
inline const Font& kFontDigitsTop() { return art().digitsTop; }
inline const Font& kFontDigitsBottom() { return art().digitsBottom; }
inline const Bitmap& kDialRing() { return art().dialRing; }
inline const Bitmap& kGroupTie() { return art().groupTie; }
inline const Bitmap& kRingPlain() { return art().ringPlain; }
inline const Bitmap& kDialDot(int raw) { return *art().dialDot[raw < 0 ? 0 : raw > 127 ? 127 : raw]; }
inline const char* masterLabel(int i) { return i >= 0 && i < 32 ? art().masterLabel[i] : nullptr; }
inline const Bitmap& kToggle(bool on) { return art().toggle[on ? 1 : 0]; }

}	// namespace mm::uispec
