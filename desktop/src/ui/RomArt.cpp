// Adapted from shnolk's Monomodule (https://github.com/shnolk/monomodule, src/plugin/one/RomArt.cpp), AGPL-3.0: the stand-in
// face and the reader are Monomodule's; the addresses are those of Machinedrum UW OS 1.63 (tools/mdskin/mdartdump.cpp).
#include "RomArt.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <deque>
#include <memory>
#include <stdexcept>
#include <system_error>
#include "Firmware.h"

namespace mm::uispec {

namespace {

// Backing storage of one artwork set. Deques and per-font vectors sized once: addresses stay put.
struct Store {
    std::deque<std::vector<uint64_t>> rows;
    std::deque<Bitmap> bitmaps;
    std::deque<std::string> strings;
    struct FontData { std::vector<Bitmap> glyphs; std::array<int16_t, 128> index; };
    std::deque<FontData> fonts;

    const uint64_t* addRows(std::vector<uint64_t> r) { rows.push_back(std::move(r)); return rows.back().data(); }
    const Bitmap* addBitmap(int w, int h, std::vector<uint64_t> r)
    {
        bitmaps.push_back(Bitmap{uint8_t(w), uint8_t(h), addRows(std::move(r))});
        return &bitmaps.back();
    }
};

// Every set ever made, kept for the life of the process (see the threading note in RomArt.h).
std::vector<std::unique_ptr<Store>>& stores() { static std::vector<std::unique_ptr<Store>> s; return s; }

// ---------------------------------------------------------------------------------------------------
// Stand-in artwork, used while no OS file is present. Drawn for this project: a plain 3x5 caps face, one
// digit per row of the glyph (bit 2 = left column), and generated dials. The other faces are made from it
// by repeating rows and columns, so layouts keep the row heights they have with the hardware's faces.

struct Glyph35 { char code; const char* rows; };
constexpr Glyph35 kStandIn[] = {
    {'A', "25755"}, {'B', "65656"}, {'C', "34443"}, {'D', "65556"}, {'E', "74647"}, {'F', "74644"}, {'G', "34553"},
    {'H', "55755"}, {'I', "72227"}, {'J', "11152"}, {'K', "55655"}, {'L', "44447"}, {'M', "57755"}, {'N', "65555"},
    {'O', "25552"}, {'P', "65644"}, {'Q', "25573"}, {'R', "65655"}, {'S', "34216"}, {'T', "72222"}, {'U', "55557"},
    {'V', "55552"}, {'W', "55775"}, {'X', "55255"}, {'Y', "55222"}, {'Z', "71247"},
    {'0', "75557"}, {'1', "26227"}, {'2', "61247"}, {'3', "61216"}, {'4', "55711"}, {'5', "74616"}, {'6', "34757"},
    {'7', "71222"}, {'8', "75757"}, {'9', "75716"},
    {'+', "02720"}, {'-', "00700"}, {'.', "00002"}, {',', "00024"}, {'/', "11244"}, {':', "02020"}, {'(', "12221"},
    {')', "42224"}, {'%', "51245"}, {'!', "22202"}, {'?', "61202"}, {'\'', "22000"}, {'<', "12421"}, {'>', "42124"},
    {'=', "07070"}, {'_', "00007"}, {'#', "57575"}, {'&', "25253"}, {'*', "52725"}, {'[', "32223"}, {']', "62226"},
};

// Three columns cannot hold a diagonal: the 5-column faces take these letters from their own drawings (bit 4 = left).
struct Glyph55 { char code; uint8_t rows[5]; };
constexpr Glyph55 kStandInWide[] = {
    {'M', {0b10001, 0b11011, 0b10101, 0b10001, 0b10001}},
    {'N', {0b10001, 0b11001, 0b10101, 0b10011, 0b10001}},
    {'W', {0b10001, 0b10001, 0b10101, 0b11011, 0b10001}},
    {'V', {0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
    {'X', {0b10001, 0b01010, 0b00100, 0b01010, 0b10001}},
    {'Y', {0b10001, 0b01010, 0b00100, 0b00100, 0b00100}},
    {'K', {0b10010, 0b10100, 0b11000, 0b10100, 0b10010}},
};

// rowMap/colMap: source row/column of every output row/column.
Font standInFont(Store& st, std::vector<int> rowMap, std::vector<int> colMap, const char* only = nullptr)
{
    st.fonts.emplace_back();
    auto& fd = st.fonts.back();
    fd.index.fill(-1);
    const int w = int(colMap.size()), h = int(rowMap.size());
    for (const auto& g : kStandIn) {
        if (only && !std::char_traits<char>::find(only, std::char_traits<char>::length(only), g.code)) continue;
        std::vector<uint64_t> rows(size_t(h), 0);
        const Glyph55* wide = nullptr;
        if (w == 5) for (const auto& k : kStandInWide) if (k.code == g.code) wide = &k;
        for (int r = 0; r < h; ++r) {
            const int bits = wide ? wide->rows[rowMap[size_t(r)]] : g.rows[rowMap[size_t(r)]] - '0';
            for (int c = 0; c < w; ++c)
                if ((bits >> (wide ? 4 - c : 2 - colMap[size_t(c)])) & 1) rows[size_t(r)] |= uint64_t(1) << (63 - c);
        }
        fd.index[size_t(g.code)] = int16_t(fd.glyphs.size());
        fd.glyphs.push_back(Bitmap{uint8_t(w), uint8_t(h), st.addRows(std::move(rows))});
    }
    return Font{uint8_t(h), uint8_t(w), fd.glyphs.data(), fd.index.data()};
}

const Bitmap* fromText(Store& st, std::initializer_list<const char*> lines)
{
    std::vector<uint64_t> rows;
    int w = 0;
    for (const char* l : lines) {
        uint64_t v = 0; int x = 0;
        for (; l[x]; ++x) if (l[x] == '#') v |= uint64_t(1) << (63 - x);
        rows.push_back(v); w = x;
    }
    const int h = int(rows.size());
    return st.addBitmap(w, h, std::move(rows));
}

const Bitmap* ring(Store& st, int size, int topPad)   // a circle of `size` pixels across under topPad blank rows
{
    std::vector<uint64_t> rows(size_t(size + topPad), 0);
    const double c = (size - 1) / 2.0;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            if (std::abs(std::hypot(x - c, y - c) - c) < 0.5) rows[size_t(y + topPad)] |= uint64_t(1) << (63 - x);
    return st.addBitmap(size, size + topPad, std::move(rows));
}

void buildToggles(Store& st, Bitmap out[2]);

void buildStandIn(Art& a)
{
    stores().push_back(std::make_unique<Store>());
    auto& st = *stores().back();
    a = Art{};
    a.tiny3x5 = standInFont(st, {0, 1, 2, 3, 4}, {0, 1, 2});
    a.small4x5 = a.tiny3x5;
    a.square5x5 = standInFont(st, {0, 1, 2, 3, 4}, {0, 0, 1, 2, 2});
    // the 8-row face: outer columns doubled for a heavy stem (smearing a 3-column glyph would close its counters)
    a.bold8 = standInFont(st, {0, 0, 1, 1, 2, 3, 3, 4}, {0, 0, 1, 2, 2});
    a.digitsTop = standInFont(st, {0, 0, 1, 1, 2}, {0, 0, 1, 2, 2}, "%./0123456789");
    a.digitsBottom = standInFont(st, {2, 3, 3, 4, 4}, {0, 0, 1, 2, 2}, "%./0123456789");

    const Bitmap* dial = ring(st, 11, 2);   // 11x13 like the hardware's, with a tick at 12 o'clock
    const_cast<uint64_t*>(dial->rows)[0] |= uint64_t(1) << (63 - 5);
    a.dialRing = *dial;
    a.ringPlain = *ring(st, 11, 0);
    a.groupTie = *fromText(st, {".#####.", "#.....#", "#.....#"});
    for (int v = 0; v < 128; ++v) {   // pointer: two pixels on the spoke, swept -150..+150 degrees
        std::vector<uint64_t> rows(7, 0);
        const double ang = (-150.0 + 300.0 * v / 127.0) * 3.14159265358979323846 / 180.0;
        for (double rad : {3.0, 2.0}) {
            const int x = int(std::lround(3 + rad * std::sin(ang))), y = int(std::lround(3 - rad * std::cos(ang)));
            if (x >= 0 && x < 7 && y >= 0 && y < 7) rows[size_t(y)] |= uint64_t(1) << (63 - x);
        }
        a.dialDot[v] = st.addBitmap(7, 7, std::move(rows));
    }
    buildToggles(st, a.toggle);
}

// ---------------------------------------------------------------------------------------------------
// Where Machinedrum UW OS 1.63 keeps the artwork (addresses in the main OS image, load base 0x200000). Facts about the file's
// layout; the pixels themselves are only ever read from the user's copy.
//
// Icon descriptor, 20 bytes of big-endian u32: w, h, n, ->pixels, ->mask. pixels = w columns of u32, the glyph in the high h
// bits, row r (0 = top) = bit 32-h+r; mask = pixels + 4w.
// Font descriptor, 5 x u32: default advance, rows (<= 8), ->widths[256] u8, ->offsets[256] i16 (negative = no glyph),
// ->columns, one byte per column, row r = bit 8-h+r. Codes >= 128 are symbol slots, not used.

constexpr uint32_t kBase = 0x200000;
constexpr uint32_t kDialRingAt = 0x2599E0, kRingPlainAt = 0x2599CC, kGroupTieAt = 0x259AF8;
constexpr uint32_t kDialDotAt = 0x259B0C;	// 128 descriptors back to back
constexpr uint32_t kDescSize = 20;
// The master effects' parameter names: 32 pointers to C strings in the order echo, reverb, EQ, dynamix (8 each).
constexpr uint32_t kMasterLabelsAt = 0x247CCA;
constexpr uint32_t kFontTiny3x5At = 0x259954, kFontSmall4x5At = 0x259968, kFontSquare5x5At = 0x25997C, kFontBold8At = 0x259990,
                   kFontDigitsTopAt = 0x2599A4, kFontDigitsBottomAt = 0x2599B8;

struct Reader {
	const std::vector<uint8_t>& d;
	Store& st;

	[[noreturn]] static void bad(const char* what, uint32_t at)
	{
		char buf[96];
		std::snprintf(buf, sizeof buf, "%s at 0x%06X", what, unsigned(at));
		throw std::runtime_error(buf);
	}
	const uint8_t* at(uint32_t addr, size_t n) const
	{
		if (addr < kBase || size_t(addr - kBase) + n > d.size()) bad("address outside the image", addr);
		return &d[addr - kBase];
	}
	uint32_t u32(uint32_t addr) const { const auto* p = at(addr, 4); return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

	const Bitmap* icon(uint32_t addr, int wantW, int wantH)
	{
		const uint32_t w = u32(addr), h = u32(addr + 4), n = u32(addr + 8), px = u32(addr + 12), mask = u32(addr + 16);
		if (w < 1 || w > 64 || h < 1 || h > 32 || n < 1 || n > 4 || mask != px + 4 * w) bad("not an icon descriptor", addr);
		if (int(w) != wantW || int(h) != wantH) bad("icon of unexpected size", addr);
		std::vector<uint64_t> rows(h, 0);
		for (uint32_t c = 0; c < w; ++c) {
			const uint32_t col = u32(px + 4 * c);
			for (uint32_t r = 0; r < h; ++r)
				if ((col >> (32 - h + r)) & 1) rows[r] |= uint64_t(1) << (63 - c);
		}
		return st.addBitmap(int(w), int(h), std::move(rows));
	}

	// A short upper-case label the pointer at `ptrAddr` refers to.
	const char* label(uint32_t ptrAddr)
	{
		const uint32_t s = u32(ptrAddr);
		std::string out;
		for(int i = 0; i < 6; ++i)
		{
			const char ch = char(*at(s + uint32_t(i), 1));
			if(ch == 0) break;
			if(!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) bad("not a label", s);
			out += ch;
		}
		if(out.empty() || out.size() > 5) bad("not a label", s);
		st.strings.push_back(out);
		return st.strings.back().c_str();
	}

	Font font(uint32_t addr, int wantH)
	{
		const uint32_t adv = u32(addr), h = u32(addr + 4), widthsAt = u32(addr + 8), offsetsAt = u32(addr + 12), colsAt = u32(addr + 16);
		if (int(h) != wantH || adv < 1 || adv > 16) bad("not the expected font", addr);
		const uint8_t* widths = at(widthsAt, 256);
		const uint8_t* offsets = at(offsetsAt, 512);
		st.fonts.emplace_back();
		auto& fd = st.fonts.back();
		fd.index.fill(-1);
		int have = 0;
		for (int c = 0; c < 128; ++c) {
			const int off = int16_t((offsets[2 * c] << 8) | offsets[2 * c + 1]);
			if (off < 0) continue;
			const uint32_t w = widths[c] ? widths[c] : adv;
			if (w > 16) bad("font glyph too wide", addr);
			const uint8_t* cols = at(colsAt + uint32_t(off), w);
			std::vector<uint64_t> rows(h, 0);
			for (uint32_t x = 0; x < w; ++x)
				for (uint32_t r = 0; r < h; ++r)
					if ((cols[x] >> (8 - h + r)) & 1) rows[r] |= uint64_t(1) << (63 - x);
			fd.index[size_t(c)] = int16_t(fd.glyphs.size());
			fd.glyphs.push_back(Bitmap{uint8_t(w), uint8_t(h), st.addRows(std::move(rows))});
			++have;
		}
		if (have < 10) bad("font without glyphs", addr);
		return Font{uint8_t(h), uint8_t(adv), fd.glyphs.data(), fd.index.data()};
	}
};

// The OFF / ON icon of a two-state cell: a ring with a pointer (up-left = off, up-right = on), drawn for this project.
void buildToggles(Store& st, Bitmap out[2])
{
	static const char* const ringRows[11] = {"...#####...", "..#.....#..", ".#.......#.", "#.........#", "#.........#", "#.........#",
	                                         "#.........#", "#.........#", ".#.......#.", "..#.....#..", "...#####..."};
	for (int on = 0; on < 2; ++on) {
		std::vector<uint64_t> rows(13, 0);
		auto set = [&](int x, int y) { rows[size_t(y)] |= uint64_t(1) << (63 - x); };
		for (int y = 0; y < 11; ++y)
			for (int x = 0; x < 11; ++x)
				if (ringRows[y][x] == '#') set(3 + x, 1 + y);
		for (int i = 0; i < 4; ++i) set(on ? 8 + 1 + i : 8 - 1 - i, 6 - i);
		out[on] = Bitmap{17, 13, st.addRows(std::move(rows))};
	}
}

struct Installed { std::string path; std::uintmax_t size = 0; std::filesystem::file_time_type mtime{}; bool valid = false; };
Installed& installed() { static Installed i; return i; }

} // namespace

Art& art()
{
	static Art a = [] { Art x; buildStandIn(x); return x; }();
	return a;
}

bool installRomArt(const std::vector<uint8_t>& mainOs, std::string* error)
{
	auto store = std::make_unique<Store>();
	Art a;
	try {
		Reader rd{mainOs, *store};
		a.bold8 = rd.font(kFontBold8At, 8);
		a.small4x5 = rd.font(kFontSmall4x5At, 5);
		a.tiny3x5 = rd.font(kFontTiny3x5At, 5);
		a.square5x5 = rd.font(kFontSquare5x5At, 5);
		a.digitsTop = rd.font(kFontDigitsTopAt, 5);
		a.digitsBottom = rd.font(kFontDigitsBottomAt, 5);
		a.dialRing = *rd.icon(kDialRingAt, 11, 13);
		a.ringPlain = *rd.icon(kRingPlainAt, 11, 11);
		a.groupTie = *rd.icon(kGroupTieAt, 7, 3);
		for (int i = 0; i < 128; ++i) a.dialDot[i] = rd.icon(kDialDotAt + uint32_t(i) * kDescSize, 7, 7);
		buildToggles(*store, a.toggle);
		// decoration: without the table the UI keeps its own generic names
		try {
			const int tableIndex[4] = {8, 0, 16, 24};	// kit order reverb, echo, EQ, dynamix -> position in the OS's table
			for(int g = 0; g < 4; ++g)
				for(int k = 0; k < 8; ++k) a.masterLabel[g * 8 + k] = rd.label(kMasterLabelsAt + 4 * uint32_t(tableIndex[g] + k));
		} catch(const std::exception&) { for(auto& l : a.masterLabel) l = nullptr; }
	} catch (const std::exception& e) {
		if (error) *error = std::string("LCD artwork not found in this OS file (") + e.what() + "); Machinedrum UW OS 1.63 is the supported version";
		return false;
	}
	a.fromRom = true;
	stores().push_back(std::move(store));
	art() = a;
	return true;
}

bool ensureRomArt(const std::filesystem::path& osFile, std::string* error)
{
	std::error_code ec;
	const auto size = std::filesystem::file_size(osFile, ec);
	if (ec) { if (error) *error = "cannot read " + osFile.string(); return false; }
	const auto mtime = std::filesystem::last_write_time(osFile, ec);
	auto& in = installed();
	if (in.valid && in.path == osFile.string() && in.size == size && in.mtime == mtime) return true;
	try {
		const auto ct = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(osFile.string())));
		if (ct.sections.empty()) throw std::runtime_error("no main-processor section");
		if (!installRomArt(ct.sections[0].data, error)) return false;
	} catch (const std::exception& e) {
		if (error) *error = e.what();
		return false;
	}
	in = Installed{osFile.string(), size, mtime, true};
	return true;
}

} // namespace mm::uispec
