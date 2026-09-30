// mdhash: a hash of 12 s of a fixed 16-track pattern (random machines incl. ROM, routing, triggers) through the whole engine:
// equal hashes = identical audio. Compare builds (recompiled vs plain interpreter, ARM vs x86, skip paths on/off).
// usage: md-hash <OS.syx> [ROM_SAMPLES.bin]   (with the ROM samples, the pattern plays two ROM machines too)
//   MD_GROUPS=n   voices split over n DSP2 instances (must give the same hash)
//   MD_SWEEP=1    plays every machine on a fresh engine and prints "SWEEP id name peak" (release gate: none of the offered ones silent)
//   MD_BUDGET=n   voice budget n; prints the max/mean number of voices that rendered
//   MD_RELEASE=n  silence release after n quiet blocks (HostModel::setSilenceRelease)
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstdint>
#include <vector>
#include "MdEngine.h"
#include "Firmware.h"
template<class E>
static void run(E& eng, int argc, char** argv)
{
	if(argc > 2)
		if(FILE* rf = fopen(argv[2], "rb"))
		{
			char magic[4]; std::vector<uint32_t> words; uint32_t head[2];
			if(fread(magic, 1, 4, rf) == 4)
				while(fread(head, 4, 2, rf) == 2 && head[1] > 0 && head[1] < 0x800000)
				{
					words.resize(head[1]);
					if(fread(words.data(), 4, head[1], rf) != head[1]) break;
					eng.voices().writeP(head[0], words.data(), words.size());
				}
			fclose(rf);
		}
	auto& h = eng.host();
	typename E::Output out;
	uint64_t hash = 1469598103934665603ull;
	auto mix = [&](int64_t v){ hash ^= uint64_t(v); hash *= 1099511628211ull; };
	unsigned rs = 99; auto rnd = [&]{ rs = rs * 1664525u + 1013904223u; return rs >> 8; };
	for(int t = 0; t < 16; ++t){ h.setMachine(t, uint8_t(16 + (t * 7) % 60)); h.setParam(t, 13, 127); h.setParam(t, 17, 100); h.setParam(t, 18, uint8_t(rnd() % 128)); h.setParam(t, 19, 40); h.setParam(t, 20, 40); h.setParam(t, 21, 40); }
	for(int t = 0; t < 6; ++t) h.setRouting(t, 1 + t % 5);
	h.setMachine(5, 130); h.setMachine(9, 140);
	if(getenv("MD_T1"))
	{
		h.setMaxActiveVoices(1);
		auto run = [&](const char* tag, int n){ for(int i = 0; i < n; ++i) eng.render(out); printf("%s: active %d\n", tag, eng.voices().activeVoicesLastBlock()); };
		run("boot", 10);
		h.trigger(0, 100); run("track0 triggered", 20);
		h.trigger(1, 100); run("track1 triggered (budget 1 cuts track0)", 20);
		run("later", 100);
		return;
	}
	int maxActive = 0; long sumActive = 0, nb = 0;
	if(getenv("MD_BUDGET")) h.setMaxActiveVoices(atoi(getenv("MD_BUDGET")));
	if(getenv("MD_RELEASE")) h.setSilenceRelease(128, atoi(getenv("MD_RELEASE")));	// silence release, hold in blocks
	FILE* raw = getenv("MD_RAW") ? fopen(getenv("MD_RAW"), "wb") : nullptr;	// main L/R as int32, for comparing two runs
	for(int b = 0; b < 44100 * 12 / 32; ++b)
	{
		if(b % 300 == 0) for(int k = 0; k < 4; ++k) h.trigger(rnd() % 16, 100);
		eng.render(out);
		if(b > 2000) { const int a = eng.voices().activeVoicesLastBlock(); maxActive = a > maxActive ? a : maxActive; sumActive += a; ++nb; }
		if(raw) fwrite(out.mix.main.data(), sizeof(int32_t), 64, raw);
		for(int f = 0; f < 32; ++f) for(int ch = 0; ch < 2; ++ch){ mix(out.mix.main[f][ch]); for(int q = 0; q < 6; ++q) mix(out.mix.frame[f][q]); mix(out.mix.rev[f][ch]); mix(out.mix.del[f][ch]); }
	}
	if(raw) fclose(raw);
	printf("hash %016llx\n", (unsigned long long)hash);
	if(getenv("MD_BUDGET") || getenv("MD_RELEASE")) printf("budget %s release %s: max rendered voices %d, mean %.2f, released %u\n", getenv("MD_BUDGET") ? getenv("MD_BUDGET") : "-", getenv("MD_RELEASE") ? getenv("MD_RELEASE") : "-", maxActive, double(sumActive) / (nb ? nb : 1), h.silenceReleases());
}
int main(int argc, char** argv)
{
	const auto fwv = md::fw::loadFirmware(argv[1]);
	auto c = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(argv[1])));
	if(getenv("MD_SWEEP"))	// every machine on a fresh engine: does it make sound? (MD_SWEEP=1)
	{
		std::vector<int> ids;
		{
			md::engine::Engine e0(fwv, std::vector<uint8_t>(c.sections.at(0).data));
			for(const auto& mc : e0.os().machines()) ids.push_back(mc.id);
		}
		for(const int id : ids)
		{
			md::engine::Engine e(fwv, std::vector<uint8_t>(c.sections.at(0).data));
			if(argc > 2)
				if(FILE* rf = fopen(argv[2], "rb"))
				{
					char magic[4]; std::vector<uint32_t> words; uint32_t head[2];
					if(fread(magic, 1, 4, rf) == 4)
						while(fread(head, 4, 2, rf) == 2 && head[1] > 0 && head[1] < 0x800000)
						{
							words.resize(head[1]);
							if(fread(words.data(), 4, head[1], rf) != head[1]) break;
							e.voices().writeP(head[0], words.data(), words.size());
						}
					fclose(rf);
				}
			auto& hh = e.host();
			const auto* mc = e.os().machine(uint8_t(id));
			hh.setMachine(2, uint8_t(id));
			for(int q = 0; q < 8; ++q) hh.setParam(2, q, mc->defaults[q]);
			hh.setParam(2, 13, 127); hh.setParam(2, 17, 100);
			md::engine::Engine::Output o;
			for(int b = 0; b < 40; ++b) e.render(o);
			hh.trigger(2, 100);
			int peak = 0;
			for(int b = 0; b < 700; ++b) { e.render(o); for(int f = 0; f < 32; ++f) peak = std::max(peak, std::abs(int(o.tracks[2][f] >> 8))); }
			printf("SWEEP %3d %-8s peak %d\n", id, mc->name.c_str(), peak);
			fflush(stdout);
		}
		return 0;
	}
	// MD_GROUPS=n (n>1): the voices split across n DSP2 instances on persistent threads; must give the same hash
	const int groups = getenv("MD_GROUPS") ? atoi(getenv("MD_GROUPS")) : 1;
	if(groups > 1)
	{
		md::engine::ParallelEngine eng(fwv, std::move(c.sections.at(0).data), groups);
		run(eng, argc, argv);
	}
	else
	{
		md::engine::Engine eng(fwv, std::move(c.sections.at(0).data));
		run(eng, argc, argv);
	}
}
