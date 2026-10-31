// mdmulti: four engines rendering at once on four threads (as a DAW with several plugin instances does) must give the
// same audio as the same engines rendered one after the other: no state shared between instances (Musashi, dsp56300).
// usage: md-multi <OS.syx>   prints MULTI-INSTANCE OK, exit 0
#include <cstdio>
#include <thread>
#include <vector>
#include "MdEngine.h"
#include "Firmware.h"
static uint64_t render(const md::fw::Firmware& fwv, const std::vector<uint8_t>& os, int seed)
{
	md::engine::Engine e(fwv, os);
	auto& h = e.host();
	for(int t = 0; t < 16; ++t) { h.setMachine(t, uint8_t(1 + (t * 7 + seed) % 40)); h.setParam(t, 13, 127); h.setParam(t, 17, 100); }
	md::engine::Engine::Output o; uint64_t hash = 1469598103934665603ull;
	for(int b = 0; b < 1500; ++b)
	{
		if(b % 150 == 0) for(int t = 0; t < 16; ++t) h.trigger(t, 100);
		e.render(o);
		for(int f = 0; f < 32; ++f) { hash ^= uint32_t(o.mix.main[f][0]); hash *= 1099511628211ull; }
	}
	return hash;
}
int main(int, char** argv)
{
	const auto fwv = md::fw::loadFirmware(argv[1]);
	auto c = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(argv[1])));
	const auto os = c.sections.at(0).data;
	uint64_t solo[4], par[4];
	for(int i = 0; i < 4; ++i) solo[i] = render(fwv, os, i);
	std::vector<std::thread> th;
	for(int i = 0; i < 4; ++i) th.emplace_back([&, i] { par[i] = render(fwv, os, i); });
	for(auto& t : th) t.join();
	int bad = 0;
	for(int i = 0; i < 4; ++i) { printf("inst %d solo %016llx parallel %016llx\n", i, (unsigned long long)solo[i], (unsigned long long)par[i]); bad += solo[i] != par[i]; }
	printf(bad ? "MISMATCH\n" : "MULTI-INSTANCE OK\n");
	return bad;
}
