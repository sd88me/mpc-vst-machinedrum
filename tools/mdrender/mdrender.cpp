// mdrender: Machinedrum Module's engine on a small demo pattern, written as a 24-bit stereo WAV (the dry main
// mix; no master effects).
// usage: mdrender OS.syx OUT.wav [SECONDS] [BPM]
#include "../../engine/MdEngine.h"
#include "../mdfw/Firmware.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cmath>
#include <algorithm>

namespace
{
	void put32(std::FILE* _f, uint32_t _v) { uint8_t b[4] = {uint8_t(_v), uint8_t(_v >> 8), uint8_t(_v >> 16), uint8_t(_v >> 24)}; std::fwrite(b, 1, 4, _f); }
	void put16(std::FILE* _f, uint16_t _v) { uint8_t b[2] = {uint8_t(_v), uint8_t(_v >> 8)}; std::fwrite(b, 1, 2, _f); }
}

int main(int argc, char** argv)
{
	if(argc < 3) { std::fprintf(stderr, "usage: mdrender OS.syx OUT.wav [SECONDS] [BPM]\n"); return 2; }
	const double seconds = argc > 3 ? std::atof(argv[3]) : 8.0;
	const double bpm = argc > 4 ? std::atof(argv[4]) : 120.0;
	try
	{
		const auto fwv = md::fw::loadFirmware(argv[1]);
		auto c = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(argv[1])));
		md::engine::Engine eng(fwv, std::move(c.sections.at(0).data));
		auto& h = eng.host();
		h.setTempo(bpm);

		auto idOf = [&](const char* _name)
		{
			for(const auto& m : eng.os().machines()) if(m.name == _name) return m.id;
			throw std::runtime_error(std::string("no machine ") + _name);
		};
		// track, machine, 16-step pattern, velocity, then a few settings
		struct Tr { int track; const char* machine; const char* steps; int vel; };
		const Tr kit[] = {
			{0, "TRXBD", "x...x...x...x..x", 120},
			{1, "TRXSD", "....x.......x...", 110},
			{2, "TRXCH", "x.x.x.x.x.x.xxx.", 90},
			{3, "TRXOH", "..............x.", 100},
			{4, "EFMCB", "...x.....x....x.", 80},
			{5, "P-IMT", "x.....x...x.....", 100},
		};
		for(const auto& k : kit)
		{
			h.setMachine(k.track, idOf(k.machine));
			h.setParam(k.track, 17, 100);	// VOL
			h.setParam(k.track, 18, 64);	// PAN centre
			h.setParam(k.track, 12, 0);		// FLTF
			h.setParam(k.track, 13, 127);	// FLTW: filter fully open
			h.setParam(k.track, 10, 64);	// EQF
			h.setParam(k.track, 11, 64);	// EQG flat
		}
		h.setParam(2, 18, 40); h.setParam(3, 18, 40);		// hats left
		h.setParam(4, 18, 90);								// cowbell right
		h.setParam(5, 18, 80);
		h.setParam(5, 8, 50); h.setParam(5, 9, 70);			// tom: some AMD
		h.setParam(1, 16, 40);								// snare: a little distortion
		h.setParam(0, 15, 10);								// kick: a touch of SRR
		h.setLfo(2, 2, 12, 1, 1, 0);						// hats: LFO on FLTF (a filter sweep)
		h.setParam(2, 21, 30); h.setParam(2, 22, 90);

		std::FILE* f = std::fopen(argv[2], "wb");
		if(!f) throw std::runtime_error("cannot write output");
		const uint32_t frames = static_cast<uint32_t>(seconds * 44100.0) / 32 * 32;
		std::fwrite("RIFF", 1, 4, f); put32(f, 36 + frames * 6); std::fwrite("WAVEfmt ", 1, 8, f);
		put32(f, 16); put16(f, 1); put16(f, 2); put32(f, 44100); put32(f, 44100 * 6); put16(f, 6); put16(f, 24);
		std::fwrite("data", 1, 4, f); put32(f, frames * 6);

		const double samplesPerStep = 44100.0 * 60.0 / bpm / 4.0;
		double nextStep = 0;
		int step = 0;
		int32_t peak = 0;
		std::array<int32_t, 16> trackPeak{}, voicePeak{};
		md::engine::Engine::Output out;
		std::FILE* dbgTracks = std::getenv("MDRENDER_DUMP_TRACKS") ? std::fopen(std::getenv("MDRENDER_DUMP_TRACKS"), "wb") : nullptr;
		const auto t0 = std::chrono::steady_clock::now();
		for(uint32_t pos = 0; pos < frames; pos += 32)
		{
			while(nextStep < pos + 32)
			{
				for(const auto& k : kit)
					if(k.steps[step % 16] == 'x')
						h.trigger(k.track, k.vel);
				++step;
				nextStep += samplesPerStep;
			}
			if(!eng.render(out)) throw std::runtime_error("render fault: " + eng.fault());
			if(dbgTracks) for(int t = 0; t < 6; ++t) std::fwrite(out.tracks[t].data(), 4, 32, dbgTracks);
			for(int t = 0; t < 16; ++t)
				for(const int32_t v : out.tracks[t]) trackPeak[t] = std::max(trackPeak[t], std::abs(v));
			for(const auto& fr : out.mix.main)
				for(const int32_t s : fr)
				{
					peak = std::max(peak, std::abs(s));
					const uint32_t u = static_cast<uint32_t>(s);
					const uint8_t b[3] = {uint8_t(u), uint8_t(u >> 8), uint8_t(u >> 16)};
					std::fwrite(b, 1, 3, f);
				}
		}
		std::fclose(f);
		const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		auto db = [](int32_t _p) { return 20.0 * std::log10(std::max(1, _p) / 8388608.0); };
		for(const auto& k : kit)
			std::printf("track %2d %-6s peak after effects %6.1f dBFS\n", k.track + 1, k.machine, db(trackPeak[k.track]));
		std::printf("%.1f s rendered in %.1f s (%.0f%% of real time on this machine), peak %.1f dBFS\n", seconds, cpu,
			cpu / seconds * 100.0, 20.0 * std::log10(std::max(1, peak) / 8388608.0));
	}
	catch(const std::exception& e) { std::fprintf(stderr, "mdrender: %s\n", e.what()); return 1; }
	return 0;
}
