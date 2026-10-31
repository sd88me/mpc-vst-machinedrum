// mdmfxref: renders a factory kit with the master effects through the engine, for comparison with the full-system emulation
// (gearmulator-md-mm via tools/mdtrace/mdProbe). Both write the stereo output as raw float32; tools/mdmix/mfx_compare.py compares.
//   mdmfxref <OS.syx> <kits.syx> <kit index 0-15> <out.raw> [--track T] [--vel V] [--at FRAME] [--frames N] [--nofx] [--nosnap] [--accent] [--master KIT [--switch]]
// The kit's machines, parameters, levels, LFOs and master bytes are applied, then track T is triggered at FRAME.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../desktop/src/core/KitCodec.h"
#include "Firmware.h"
#include "MdEngine.h"

int main(int argc, char** argv)
{
	if(argc < 5) { std::fprintf(stderr, "usage: mdmfxref <OS.syx> <kits.syx> <kit> <out.raw> [--track T] [--vel V] [--at FRAME] [--frames N] [--nofx] [--nosnap] [--accent] [--master KIT [--switch]]\n"); return 2; }
	int track = 0, vel = 100, at = 20000, frames = 80000; bool fx = true, snap = true, accent = false, fromKit = false; int masterKit = -1;
	for(int i = 5; i < argc; ++i)
	{
		const std::string a = argv[i];
		if(a == "--track" && i + 1 < argc) track = std::atoi(argv[++i]);
		else if(a == "--vel" && i + 1 < argc) vel = std::atoi(argv[++i]);
		else if(a == "--at" && i + 1 < argc) at = std::atoi(argv[++i]);
		else if(a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
		else if(a == "--nofx") fx = false;
		else if(a == "--master" && i + 1 < argc) masterKit = std::atoi(argv[++i]);	// use this kit's master effect bytes with the kit's voices
		else if(a == "--switch") fromKit = true;	// start on the kit's own master settings (settled), then slew to --master KIT's, as a unit does after a kit change
		else if(a == "--accent") accent = true;	// an accented step: velocity 128 (the front-panel trigger)
		else if(a == "--nosnap") snap = false;	// let the OS smoother slew the master values from zero, as the unit does after a kit load
	}
	const auto fwv = md::fw::loadFirmware(argv[1]);
	auto c = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(argv[1])));
	const auto kitFile = md::fw::readFile(argv[2]);
	const auto kits = mm::parseKits(std::vector<uint8_t>(kitFile.begin(), kitFile.end()));
	const size_t ki = static_cast<size_t>(std::atoi(argv[3]));
	if(ki >= kits.size()) { std::fprintf(stderr, "kit %zu: only %zu kits\n", ki, kits.size()); return 1; }
	const auto& kit = kits[ki];
	if(std::FILE* rf = std::fopen("/tmp/claude-0/factory/ROM_SAMPLES.bin", "rb"); false && rf) std::fclose(rf);

	md::engine::Engine eng(fwv, std::vector<uint8_t>(c.sections.at(0).data));
	if(const char* rom = std::getenv("MD_ROM"))
		if(std::FILE* rf = std::fopen(rom, "rb"))
		{
			char magic[4]; std::vector<uint32_t> words; uint32_t head[2];
			if(std::fread(magic, 1, 4, rf) == 4)
				while(std::fread(head, 4, 2, rf) == 2 && head[1] > 0 && head[1] < 0x800000)
				{
					words.resize(head[1]);
					if(std::fread(words.data(), 4, head[1], rf) != head[1]) break;
					eng.voices().writeP(head[0], words.data(), words.size());
				}
			std::fclose(rf);
		}
	auto& h = eng.host();
	for(int t = 0; t < mm::kTracks; ++t)
	{
		h.setMachine(t, kit.machine[t]);
		for(int i = 0; i < 24; ++i) h.setParam(t, i, kit.params[t][i]);
		h.setLevel(t, kit.level[t]);
		h.setLfo(t, kit.lfo[t][0], kit.lfo[t][1], kit.lfo[t][2], kit.lfo[t][3], kit.lfo[t][4]);
	}
	if(fx) { eng.enableMasterFx(true); const uint8_t* mb = masterKit >= 0 && static_cast<size_t>(masterKit) < kits.size() ? kits[static_cast<size_t>(masterKit)].master : kit.master;
		if(std::getenv("MD_PRINT_MASTER")) { for(int i = 0; i < 32; ++i) std::printf("%02x%s", mb[i], i % 8 == 7 ? "\n" : " "); }
		if(fromKit) { eng.setMasterFx(kit.master, true); snap = false; }	// settled on the kit's own, the OS smoother then moves from there
		eng.setMasterFx(mb, snap); }
	h.setTempo(120.0);
	if(std::getenv("MD_DUMP_WORDS") && fx)	// the mixer DSP's parameter words, in the format of mdProbe dspmem:0:Y:150:190, for comparison
	{
		uint32_t a[9], b[29];
		h.masterWords(a, b);
		std::printf("slewed master values:"); for(int i = 0; i < 32; ++i) std::printf(" %04x", eng.os().peek16(0x1000d9c + 2 * i)); std::printf("\n");
		for(int i = 0; i < 9; ++i) std::printf("W %03x %06x\n", 0x150 + i, a[i]);
		for(int i = 0; i < 29; ++i) std::printf("W %03x %06x\n", 0x170 + i, b[i]);
	}
	std::FILE* out = std::fopen(argv[4], "wb");
	md::engine::Engine::Output o;
	std::printf("kit %zu \"%s\", track %d vel %d at %d, master fx %s\n", ki, kit.name, track, vel, at, fx ? "on" : "off");
	for(int b = 0; b * 32 < frames; ++b)
	{
		if(b * 32 <= at && at < b * 32 + 32) h.trigger(track, vel, accent);
		if(!eng.render(o)) { std::fprintf(stderr, "engine fault: %s\n", eng.fault().c_str()); return 1; }
		for(int f = 0; f < 32; ++f)
		{
			const float l = float(eng.mainOut(o, f, 0)) / 8388608.0f, r = float(eng.mainOut(o, f, 1)) / 8388608.0f;
			std::fwrite(&l, 4, 1, out); std::fwrite(&r, 4, 1, out);
		}
	}
	std::fclose(out);
	return 0;
}
