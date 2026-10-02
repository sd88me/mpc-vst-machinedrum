// mdsamples: the ROM machines' sample memory, for Machinemodule. The UW's ROM slots play samples the MD copies
// from its sample flash into the voice DSP's (DSP2's) external memory at boot - memory this port's engine, which only
// loads the OS's own DSP program, never gets. This tool takes a dump of the emulated MD's DSP2 P memory after boot
// (tools/mdtrace/mdProbe dspdump:1:0:800000:FILE) and keeps the words that differ from what VoiceEngine loads itself,
// above the OS program's external code, as records the engine loads at start (engine.cpp, "ROM samples").
// The output is Elektron's sample data from the user's own flash image: per-user build output, never committed.
//
//   mdsamples <MD OS .syx> <dsp2 P dump (32-bit LE words)> <out samples.bin>
// samples.bin: "MDS1", then records [u32 address][u32 count][count x u32 word], little-endian, ending with count 0.
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

#include "Firmware.h"
#include "VoiceEngine.h"
#include "dsp56kEmu/dsp.h"

int main(int argc, char** argv)
{
	if(argc != 4) { std::fprintf(stderr, "usage: mdsamples <MD OS .syx> <dsp2 P dump> <out samples.bin>\n"); return 2; }
	const auto fwv = md::fw::loadFirmware(argv[1]);
	md::engine::VoiceEngine voices(fwv);
	auto& mem = voices.dsp().memory();
	std::FILE* f = std::fopen(argv[2], "rb");
	if(!f) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
	std::vector<uint32_t> dump(0x800000);
	const size_t n = std::fread(dump.data(), 4, dump.size(), f);
	std::fclose(f);
	// From $140000: the ROM sample directory the MD writes into the program's own table area at boot
	// ($147e00-$147f08, 4 words per slot: start, length, loop, flags) and the sample data after it ($150000-).
	// Lower differences are the program's runtime state, not sample data.
	constexpr uint32_t progEnd = 0x140000;
	std::FILE* o = std::fopen(argv[3], "wb");
	std::fwrite("MDS1", 1, 4, o);
	uint32_t total = 0, runs = 0;
	for(uint32_t a = progEnd; a < n && a < mem.sizeP();)
	{
		if((dump[a] & 0xffffff) == (mem.get(dsp56k::MemArea_P, a) & 0xffffff)) { ++a; continue; }
		uint32_t e = a;
		while(e < n && e < mem.sizeP() && (dump[e] & 0xffffff) != (mem.get(dsp56k::MemArea_P, e) & 0xffffff)) ++e;
		const uint32_t count = e - a;
		std::fwrite(&a, 4, 1, o);
		std::fwrite(&count, 4, 1, o);
		std::fwrite(&dump[a], 4, count, o);
		total += count; ++runs;
		a = e;
	}
	const uint32_t zero[2] = {0, 0};
	std::fwrite(zero, 4, 2, o);
	std::fclose(o);
	std::fprintf(stderr, "%s: %u words in %u runs, above $%06x\n", argv[3], total, runs, progEnd);
	return total ? 0 : 1;
}
