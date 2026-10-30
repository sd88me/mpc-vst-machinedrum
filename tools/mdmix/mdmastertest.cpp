// md-mastertest: engine/MasterFx (DSP1's master section $342-$9A8 on its own DSP, fed by the native mixer) against the whole
// mixer DSP (MixerRef: its own mix code, then the same section), block after block with random inputs and random master knobs.
// The main outputs must be word for word the same.
// usage: md-mastertest OS.syx [BLOCKS]
#include "MixerRef.h"
#include "../../engine/MasterFx.h"
#include "../../engine/Mixer.h"
#include "../../engine/Dsp56.h"
#include <cstdio>
#include <cstdlib>

using namespace md;
using engine::d56::u24;

int main(int argc, char** argv)
{
	if(argc < 2) { std::fprintf(stderr, "usage: md-mastertest OS.syx [BLOCKS]\n"); return 2; }
	const auto fwv = fw::loadFirmware(argv[1]);
	const int blocks = argc > 2 ? std::atoi(argv[2]) : 400;
	auto c = fw::parseContainer(fw::parseSysex(fw::readFile(argv[1])));
	engine::MachineRunner os(std::move(c.sections.at(0).data));
	mixref::MixerRef ref(fwv);
	engine::TrackFx::Tables tables(fwv);
	engine::Mixer mixer(tables);
	engine::MasterFx master(fwv);

	uint32_t seed = 4242;
	auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
	std::array<std::array<uint8_t, 8>, 4> knobs{};
	const uint8_t defaults[4][8] = {{0, 0, 64, 64, 0, 127, 127, 127}, {24, 0, 0, 32, 0, 127, 0, 127}, {64, 64, 64, 64, 64, 64, 64, 64}, {0, 64, 127, 0, 0, 0, 64, 0}};
	auto apply = [&](int fx)
	{
		std::array<uint32_t, 16> w{};
		if(!engine::convertMasterFx(os, static_cast<engine::MasterFxId>(fx), knobs[static_cast<size_t>(fx)].data(), w)) { std::printf("conversion fault: %s\n", os.faultReason().c_str()); std::exit(1); }
		const auto& s = engine::masterFxSection(static_cast<engine::MasterFxId>(fx));
		for(int k = 0; k < s.words; ++k) ref.pokeY(s.dspAddr + static_cast<uint32_t>(k), w[static_cast<size_t>(k)]);
		master.setParams(static_cast<engine::MasterFxId>(fx), w);
	};
	for(int fx = 0; fx < 4; ++fx) { for(int k = 0; k < 8; ++k) knobs[static_cast<size_t>(fx)][static_cast<size_t>(k)] = defaults[fx][k]; apply(fx); }

	long bad = 0, total = 0, nz = 0, maxAbs = 0;
	uint64_t instr = 0;
	for(int b = 0; b < blocks; ++b)
	{
		if(b % 20 == 0)	// a parameter change: one effect's knobs go to random values (or back to the defaults)
		{
			const int fx = static_cast<int>(rnd() % 4);
			for(int k = 0; k < 8; ++k) knobs[static_cast<size_t>(fx)][static_cast<size_t>(k)] = (rnd() % 3 == 0) ? defaults[fx][k] : static_cast<uint8_t>(rnd() % 128);
			apply(fx);
		}
		mixref::MixerRef::Block tr[16];
		uint32_t mixw[16][5];
		std::array<uint32_t, 5> mixa[16];
		const int32_t* ptr[16];
		const bool silent = (b % 50) >= 40;	// tails: some silent blocks, the delay and reverb ringing on
		for(int k = 0; k < 16; ++k)
		{
			const int kind = static_cast<int>(rnd() % 3);
			for(auto& s : tr[k]) s = silent || kind == 0 ? 0 : engine::d56::sx24(rnd()) >> (kind == 1 ? 3 : 7);
			const uint32_t vol = ((rnd() % 128) << 16) & 0xffffff, pan = (rnd() % 128) << 16, rev = ((rnd() % 128) << 11) & 0xffffff, del = ((rnd() % 128) << 11) & 0xffffff;
			const uint32_t route = rnd() % 8 == 0 ? rnd() % 6 : 6;
			const uint32_t w[5] = {route, vol, pan, rev, del};
			for(int i = 0; i < 5; ++i) { mixw[k][i] = w[i]; mixa[k][i] = w[i]; }
			ptr[k] = tr[k].data();
		}
		if(!ref.runMix(tr, mixw) || !ref.runMaster(0x9a8)) { std::printf("ref fault: %s\n", ref.fault().c_str()); return 1; }
		engine::Mixer::Output out;
		mixer.process(ptr, mixa, out);
		engine::MasterFx::Stereo res;
		if(!master.process(out, res)) { std::printf("master fault: %s\n", master.faultReason().c_str()); return 1; }
		instr += master.instructionsLastBlock();
		int diff = 0;
		for(int i = 0; i < 32; ++i)
			for(int ch = 0; ch < 2; ++ch)
			{
				const uint32_t want = ref.peekX(0x400 + 6 * i + (ch ? 5 : 2));
				diff += u24(res[static_cast<size_t>(i)][static_cast<size_t>(ch)]) != want;
				nz += want != 0;
				maxAbs = std::max<long>(maxAbs, std::abs(res[static_cast<size_t>(i)][static_cast<size_t>(ch)]));
			}
		total += 64;
		if(diff && bad < 5)
			std::printf("block %d: %d words differ (frame0 L mine %06x dsp %06x)\n", b, diff, u24(res[0][0]), ref.peekX(0x400 + 2));
		bad += diff;
	}
	std::printf("master: %ld words compared, %ld mismatches (%ld non-zero, peak %ld, %.0f DSP instructions a block)\n", total, bad, nz, maxAbs, double(instr) / blocks);
	return bad ? 1 : 0;
}
