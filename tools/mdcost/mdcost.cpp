// mdcost: DSP instructions and wall-clock time per playing voice for every machine (idle baseline first). Run on the
// Force to see what one voice really costs there.  usage: md-cost <OS.syx> <ROM_SAMPLES.bin>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <string>
#include <chrono>
#include "MdEngine.h"
#include "Firmware.h"
#include "dsp56kEmu/dsp.h"
static void loadRom(md::engine::VoiceEngine& v, const char* path)
{
	FILE* f = fopen(path, "rb"); if(!f) return; char m[4]; if(fread(m,1,4,f)!=4){fclose(f);return;}
	std::vector<uint32_t> w; uint32_t h[2];
	while(fread(h,4,2,f)==2 && h[1]>0 && h[1]<0x800000){ w.resize(h[1]); if(fread(w.data(),4,h[1],f)!=h[1])break; v.writeP(h[0],w.data(),w.size()); }
	fclose(f);
}
int main(int argc, char** argv)
{
	const auto fwv = md::fw::loadFirmware(argv[1]);
	auto c = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(argv[1])));
	md::engine::Engine eng(fwv, std::move(c.sections.at(0).data));
	loadRom(eng.voices(), argv[2]);
	auto& h = eng.host();
	md::engine::Engine::Output out;
	auto run = [&](int blocks){ for(int b=0;b<blocks;++b) eng.render(out); };
	auto now = []{ return std::chrono::steady_clock::now(); };
	auto us = [&](std::chrono::steady_clock::time_point a){ return std::chrono::duration<double, std::micro>(now() - a).count(); };
	run(200);
	auto instr = [&]{ return eng.voices().dsp().getInstructionCounter(); };
	uint64_t i0 = instr(); auto t0 = now(); run(400); const double base = double(instr()-i0)/400; const double baseUs = us(t0) / 400 * 4;
	// idle stage breakdown, per 128-frame host block (4 engine blocks)
	{
		eng.timingOn = true; h.tickUs = h.dspUs = h.voiceLoopUs = h.osCallsUs = eng.fxUs = eng.mixUs = 0; h.timingOn = true;
		const int nb = 800; run(nb);
		const double k = 4.0 / nb;
		printf("  of the tick: machine functions (16 voices) %.0f us, OS smoothing/LFO calls %.0f us\n", h.voiceLoopUs * k, h.osCallsUs * k);
		printf("idle stages per host block: OS tick %.0f us, voice DSP %.0f us, track FX %.0f us, mixer %.0f us\n", h.tickUs * k, h.dspUs * k, eng.fxUs * k, eng.mixUs * k);
		eng.timingOn = false; h.timingOn = false;
	}
	printf("idle: %.0f DSP instr per 32-sample block, %.0f us per 128-frame host block (budget 2902 us)\n", base, baseUs);
	struct R{ std::string n; int id; double c; double us; };
	std::vector<R> rs;
	for(const auto& m : eng.os().machines())
	{
		if(m.id < 16 || (m.id > 159)) continue;
		h.setMachine(0, uint8_t(m.id)); h.setParam(0, 13, 127); h.setParam(0, 17, 127);
		h.trigger(0, 110); run(60);
		uint64_t a = instr(); int n = 0; auto ta = now();
		for(int b = 0; b < 600; ++b){ if(b % 80 == 0) h.trigger(0, 110); eng.render(out); ++n; }
		rs.push_back({m.name, m.id, double(instr()-a)/n - base, us(ta) / n * 4 - baseUs});
	}
	std::sort(rs.begin(), rs.end(), [](const R&a,const R&b){return a.c>b.c;});
	printf("extra DSP instr per block for ONE playing voice (retriggered every 80 blocks), costliest first:\n");
	for(size_t i=0;i<rs.size();++i) if(i<14 || i>rs.size()-4 || std::getenv("MD_COST_ALL")) printf("%3d %-7s %7.0f instr  %6.0f us/host block extra  (%.1fx TRX-BD)\n", rs[i].id, rs[i].n.c_str(), rs[i].c, rs[i].us, rs[i].c/std::max(1.0, [&]{for(auto&r:rs) if(r.id==16) return r.c; return 1.0;}()));
	double rom=0, romUs=0, othUs=0; int nr=0, oth=0; double othc=0;
	for(auto&r:rs){ if(r.id>=128){rom+=r.c;romUs+=r.us;++nr;} else {othc+=r.c;othUs+=r.us;++oth;} }
	printf("mean extra per voice: ROM %.0f instr = %.0f us (%d machines), others %.0f instr = %.0f us (%d)\n", rom/nr, romUs/nr, nr, othc/oth, othUs/oth, oth);
}
