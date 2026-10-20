// mdinput: the audio input through the INP machines. usage: md-input <OS.syx>
// A sine on the left channel only goes through INP-GA (left) on track 1 and INP-GB (right) on track 2: track 1 must carry it
// (at the same pitch), track 2 must stay silent; then the sine on the right channel only: the other way round; with no input
// at all both are silent. Prints "input OK" and exits 0 when all of that holds.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "MdEngine.h"
#include "Firmware.h"

namespace
{
	struct Result { double peak[2]; double zeroCrossHz; };

	// _mode: 0 = silence in, 1 = sine left, 2 = sine right; -1 = setInput never called
	Result run(const md::fw::Firmware& _fw, const std::vector<uint8_t>& _os, const int _mode)
	{
		md::engine::Engine eng(_fw, _os);
		auto& h = eng.host();
		h.setMachine(0, 80);	// INP-GA
		h.setMachine(1, 81);	// INP-GB
		for(int t = 0; t < 2; ++t) { h.setParam(t, 0, 127); h.setParam(t, 1, 0); h.setParam(t, 10, 64); h.setParam(t, 11, 64); h.setParam(t, 13, 127); h.setParam(t, 17, 127); h.setParam(t, 18, 64); h.setLevel(t, 127); }	// VOL, GATE open; the rest as the plugin's defaults (EQ flat, filter open)
		md::engine::Engine::Output out;
		Result r{{0, 0}, 0};
		int crossings = 0, last = 0, samples = 0;
		for(int b = 0; b < 400; ++b)
		{
			int32_t in[64];
			for(int f = 0; f < 32; ++f)
			{
				const double s = std::sin(2.0 * M_PI * 441.0 * double(b * 32 + f) / 44100.0) * 0.5 * 8388607.0;
				in[2 * f] = _mode == 1 ? int32_t(s) : 0;
				in[2 * f + 1] = _mode == 2 ? int32_t(s) : 0;
			}
			if(_mode >= 0) eng.setInput(in);
			if(b == 10) { h.trigger(0, 100); h.trigger(1, 100); }
			if(!eng.render(out)) { std::fprintf(stderr, "render fault: %s\n", eng.fault().c_str()); std::exit(2); }
			if(b < 20) continue;
			for(int t = 0; t < 2; ++t)
				for(int f = 0; f < 32; ++f)
					r.peak[t] = std::max(r.peak[t], std::abs(double(out.tracks[t][f])) / 8388608.0);
			for(int f = 0; f < 32; ++f)
			{
				const int s = out.tracks[0][f] > 0 ? 1 : out.tracks[0][f] < 0 ? -1 : 0;
				if(s && last && s != last) ++crossings;
				if(s) last = s;
				++samples;
			}
		}
		r.zeroCrossHz = crossings / 2.0 / (samples / 44100.0);
		return r;
	}
}

int main(int argc, char** argv)
{
	if(argc < 2) { std::fprintf(stderr, "usage: md-input <OS.syx>\n"); return 2; }
	const auto fw = md::fw::loadFirmware(argv[1]);
	auto c = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(argv[1])));
	const std::vector<uint8_t> os(c.sections.at(0).data);
	bool ok = true;
	const char* names[] = {"silence in", "sine left", "sine right"};
	for(int mode = 0; mode < 3; ++mode)
	{
		const auto r = run(fw, os, mode);
		std::printf("%-10s: INP-GA peak %.4f, INP-GB peak %.4f, GA pitch %.0f Hz\n", names[mode], r.peak[0], r.peak[1], r.zeroCrossHz);
		const bool wantA = mode == 1, wantB = mode == 2;
		ok &= wantA ? r.peak[0] > 0.01 && std::abs(r.zeroCrossHz - 441.0) < 20.0 : r.peak[0] < 0.001;
		ok &= wantB ? r.peak[1] > 0.01 : r.peak[1] < 0.001;
	}
	const auto none = run(fw, os, -1);
	std::printf("no input call: peaks %.4f %.4f\n", none.peak[0], none.peak[1]);
	ok &= none.peak[0] < 0.001 && none.peak[1] < 0.001;
	std::puts(ok ? "input OK" : "input FAILED");
	return ok ? 0 : 1;
}
