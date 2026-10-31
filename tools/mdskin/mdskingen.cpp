// md-skin-gen: fills in a skin recipe (tools/mdskin/skin_recipe.py) from an OS file, as the Gen2 plugin does on the device
// (vst/skin_recipe.h), writing every image to <out dir>/<its path in the recipe>. The test that the device writes the generators' skin:
// compare its images with mk_skin.py's (tools/mdskin/check_skin_gen.py).
//   md-skin-gen <MD OS .syx> <art.bin> <out dir> [bits.txt]
#include <cstdio>
#include <stdexcept>

#include "Firmware.h"
#include "md_art.h"
#include "skin_recipe.h"

int main(int argc, char** argv)
{
	if(argc < 4) { std::fprintf(stderr, "usage: md-skin-gen <MD OS .syx> <art.bin> <out dir> [bits.txt]\n"); return 2; }
	try
	{
		const auto ct = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(argv[1])));
		const auto bits = md::art::bits(md::art::read(ct.sections.at(0).data));
		if(argc > 4)
			if(std::FILE* f = std::fopen(argv[4], "w")) { for(uint8_t b : bits) std::fputc('0' + b, f); std::fclose(f); }
		std::string err;
		const std::string out = argv[3];
		const int n = md::skin::fill(argv[2], bits, [&](const std::string& skin, const std::string& folder) { return out + "/" + skin + "/" + folder; }, err);
		if(n < 0) { std::fprintf(stderr, "md-skin-gen: %s\n", err.c_str()); return 1; }
		std::printf("%d images from %zu art bits\n", n, bits.size());
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "md-skin-gen: %s\n", e.what());
		return 1;
	}
	return 0;
}
