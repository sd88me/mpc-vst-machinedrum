// md-busybench: the cost of a busy kit on this machine, per 128-frame Move block, against the 2902 us budget. The same pattern as
// md-hosttest's busy phase (16 tracks, dense 16ths, accents), rendered offline through the module's own path (KitValues +
// EngineFeeder + the engine, silence release on), for each configuration:
//   voice groups 1/2/3 (ParallelVoiceEngine threads) x master effects off/on
//   md-busybench <OS.syx> [ROM_SAMPLES.bin] [seconds=10]
// Prints the mean and the worst block after a 2 s warm-up (the JIT compiles each machine's code on its first blocks).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "EngineFeeder.h"
#include "Firmware.h"
#include "MdEngine.h"
#include "master_worker.h"

using Clock = std::chrono::steady_clock;

namespace
{
	struct NoHost final : mm::EngineFeeder::Host
	{
		bool takeDefaults(int) override { return true; }
		void defaultsChosen(int, const int*, int) override {}
	};

	template<class E> void loadRom(E& eng, const char* path)
	{
		std::FILE* rf = path ? std::fopen(path, "rb") : nullptr;
		if(!rf) return;
		char magic[4];
		std::vector<uint32_t> words;
		uint32_t head[2];
		if(std::fread(magic, 1, 4, rf) == 4)
			while(std::fread(head, 4, 2, rf) == 2 && head[1] > 0 && head[1] < 0x800000)
			{
				words.resize(head[1]);
				if(std::fread(words.data(), 4, head[1], rf) != head[1]) break;
				eng.voices().writeP(head[0], words.data(), words.size());
			}
		std::fclose(rf);
	}

	const bool groove = std::getenv("MD_BENCH_GROOVE") != nullptr;	// the typical beat instead of every track busy

	template<class E> void bench(E& eng, const char* label, bool master, double seconds, mm::MasterWorker* pipe = nullptr)
	{
		static const char* const kMachines[16] = {"TRXBD", "TRXSD", "EFMXT", "EFMXT", "TRXXT", "TRXCP", "TRXRS", "TRXCB", "TRXCH", "TRXOH", "TRXCY", "EFMCY", "E12BD", "P-IBD", "EFMHH", "TRXMA"};
		mm::KitValues v = mm::KitValues::defaults();
		for(int t = 0; t < 16; ++t)
			for(const auto& m : eng.os().machines())
				if(m.name == kMachines[t]) v.track[size_t(t)].machine = m.id;
		{
			typename E::Output o;
			for(int b = 0; b < 40; ++b) eng.render(o);
		}
		if(master) eng.enableMasterFx(true);
		eng.host().setSilenceRelease(128, 138);
		mm::EngineFeeder feeder;
		NoHost host;
		typename E::Output out;
		const int blocks = int(seconds * 44100 / 128), warm = int(2.0 * 44100 / 128);
		double sum = 0, worst = 0;
		uint64_t hash = 1469598103934665603ull;	// of the main output: equal = identical audio
		std::vector<double> all;
		for(int b = 0; b < blocks; ++b)
		{
			const auto t0 = Clock::now();
			if(b % 43 == 0)	// a 16th at 120 BPM ~ 43 blocks
			{
				feeder.apply(eng, v, 120.0, host);
				const int step = b / 43;
				if(groove)	// a typical beat: kick on the quarters, snare on 2 and 4, closed hat 16ths, open hat, a clap, a tom fill
				{
					const int st = step % 16;
					if(st % 4 == 0) eng.host().trigger(0, 120, false);
					if(st == 4 || st == 12) eng.host().trigger(1, 110, false);
					if(st != 14) eng.host().trigger(8, st % 2 ? 80 : 110, false);
					if(st == 14) eng.host().trigger(9, 100, false);
					if(st == 12 && (step / 16) % 2) eng.host().trigger(5, 100, false);
					if(st >= 13 && (step / 16) % 4 == 3) eng.host().trigger(3, 100, false);
				}
				else
					for(int t = 0; t < 16; ++t)
						if((step * 7 + t * 5) % 16 < (t < 2 ? 6 : 4)) eng.host().trigger(t, 100 + (step % 4 == 0) * 27, false);
			}
			if constexpr(std::is_same_v<E, md::engine::Engine>)
				if(pipe)	// the engine process's way: the master effects one 32-frame block behind, on the worker thread
				{
					for(int sb = 0; sb < mm::kSub; ++sb) { feeder.apply(eng, v, 120.0, host); eng.renderDry(pipe->outs[sb], pipe->jobs[sb]); pipe->post(); }
					mm::waitFor(pipe->done, pipe->posted.load());
					for(const auto& o : pipe->outs) for(const auto& fr : o.mix.main) for(int32_t x : fr) { hash ^= uint64_t(uint32_t(x)); hash *= 1099511628211ull; }
				}
			if(!pipe)
				for(int sb = 0; sb < 4; ++sb)
				{
					feeder.apply(eng, v, 120.0, host);
					eng.render(out);
					for(const auto& fr : out.mix.main) for(int32_t x : fr) { hash ^= uint64_t(uint32_t(x)); hash *= 1099511628211ull; }
				}
			const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
			if(b >= warm) { sum += us; worst = std::max(worst, us); all.push_back(us); }
		}
		std::sort(all.begin(), all.end());
		const double mean = sum / double(all.size());
		std::printf("%-28s master %-3s  mean %5.0f us (%3.0f%%)  p99 %5.0f  worst %6.0f  releases %u  hash %016llx\n", label, master ? "on" : "off", mean,
			100.0 * mean / 2902.0, all[all.size() * 99 / 100], worst, eng.host().silenceReleases(), static_cast<unsigned long long>(hash));
	}
}

int main(int argc, char** argv)
{
	if(argc < 2) { std::fprintf(stderr, "usage: md-busybench <OS.syx> [ROM_SAMPLES.bin] [seconds]\n"); return 2; }
	const char* rom = argc > 2 ? argv[2] : nullptr;
	const double seconds = argc > 3 ? std::atof(argv[3]) : 10.0;
	const auto fw = md::fw::loadFirmware(argv[1]);
	const auto section0 = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(argv[1]))).sections.at(0).data;
	const char* only = std::getenv("MD_BENCH_ONLY");	// e.g. "g1" to run one configuration
	for(int master = 0; master < 2; ++master)
	{
		if(!only || !std::strcmp(only, "g1"))
		{
			md::engine::Engine eng(fw, std::vector<uint8_t>(section0));
			loadRom(eng, rom);
			bench(eng, "1 voice thread", master != 0, seconds);
		}
		if(master && (!only || !std::strcmp(only, "g1") || !std::strcmp(only, "pipe")))
		{
			md::engine::Engine eng(fw, std::vector<uint8_t>(section0));
			loadRom(eng, rom);
			mm::MasterWorker w;
			w.eng = &eng;
			w.start();
			bench(eng, "1 voice thread + master thr", true, seconds, &w);
			w.stop();
		}
		for(int g = 2; g <= 3; ++g)
		{
			char id[4];
			std::snprintf(id, sizeof id, "g%d", g);
			if(only && std::strcmp(only, id)) continue;
			md::engine::ParallelEngine eng(fw, std::vector<uint8_t>(section0), g);
			loadRom(eng, rom);
			char label[32];
			std::snprintf(label, sizeof label, "%d voice threads", g);
			bench(eng, label, master != 0, seconds);
		}
	}
	return 0;
}
