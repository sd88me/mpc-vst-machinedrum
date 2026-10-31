// mdartdump: the LCD artwork of the user's own Machinedrum OS file (the six bitmap fonts, the dial ring, dot, group tie and the machine family badges) as the
// JSON tools/mdskin/mk_skin.py draws the plugin's skin from. The Machinedrum and Monomachine OS share the Elektron LCD font family,
// so the skin needs no second OS file. The two-state toggle icon is drawn here, not read from the OS. Addresses are those of
// Machinedrum UW OS 1.63 (the only OS this port supports). The output is Elektron's artwork: per-user build data, never committed.
//   mdartdump <MD OS .syx> <out art.json>
#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "Firmware.h"
#include "md_art.h"

namespace
{
FILE* g_f;

void put(const md::art::Bmp& b)
{
	std::fprintf(g_f, "{\"w\":%d,\"h\":%d,\"rows\":[", b.w, b.h);
	for(int r = 0; r < b.h; ++r) std::fprintf(g_f, "%s\"%016llx\"", r ? "," : "", (unsigned long long)b.rows[size_t(r)]);
	std::fputs("]}", g_f);
}

void putFont(const md::art::Art& a, const char* name, bool last)
{
	const auto& f = a.fonts.at(name);
	std::fprintf(g_f, "\"%s\":{\"h\":%d,\"adv\":%d,\"glyphs\":{", name, f.h, f.adv);
	bool first = true;
	for(const auto& [c, g] : f.glyphs)
	{
		std::fprintf(g_f, "%s\"%d\":", first ? "" : ",", c);
		first = false;
		put(g);
	}
	std::fprintf(g_f, "}}%s", last ? "" : ",");
}
} // namespace

int main(int argc, char** argv)
{
	if(argc != 3) { std::fprintf(stderr, "usage: mdartdump <MD OS .syx> <out art.json>\n"); return 2; }
	try
	{
		const auto ct = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(argv[1])));
		if(ct.sections.empty()) throw std::runtime_error("no main-processor section");
		const auto a = md::art::read(ct.sections[0].data);
		g_f = std::fopen(argv[2], "w");
		if(!g_f) throw std::runtime_error("cannot write the output");
		std::fputs("{\"fonts\":{", g_f);
		for(const char* n : {"tiny3x5", "small4x5", "square5x5", "bold8", "digitsTop"}) putFont(a, n, false);
		putFont(a, "digitsBottom", true);
		std::fputs("},\"bitmaps\":{", g_f);
		bool first = true;
		for(const char* n : {"dialRing", "ringPlain", "groupTie", "dialDot", "toggle", "familyBadge"})
		{
			const auto& list = a.bitmaps.at(n);
			std::fprintf(g_f, "%s\"%s\":", first ? "" : ",", n);
			first = false;
			const bool many = list.size() > 1;
			if(many) std::fputc('[', g_f);
			for(size_t i = 0; i < list.size(); ++i) { if(i) std::fputc(',', g_f); put(list[i]); }
			if(many) std::fputc(']', g_f);
		}
		std::fputs("}}\n", g_f);
		std::fclose(g_f);
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "mdartdump: %s (this needs Machinedrum UW OS 1.63)\n", e.what());
		return 1;
	}
	return 0;
}
