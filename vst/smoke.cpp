// Drives mpc_engine() like the host does: 128-frame blocks paced to real time. Prints readiness time, peak
// and underruns. usage: md-vst-smoke <data-dir> [seconds]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>
extern "C" {
#include "engine.h"
}
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv)
{
	const mpc_engine_t* e = mpc_engine();
	const auto t0 = Clock::now();
	void* in = e->create(argc > 1 ? argv[1] : ".");
	char buf[32];
	// A project restore arrives before the engine has booted: machine first, then a saved SYN value.
	const bool fresh = std::getenv("MD_SMOKE_FRESH") != nullptr;	// a newly inserted instance: nothing restored
	if(std::getenv("MD_SMOKE_RESTORED")) e->set_param(in, "track3_machine", "28");	// with MD_SMOKE_FRESH: a project restore
	if(!fresh)
	{
		e->set_param(in, "track3_machine", "28");	// TRXB2
		e->set_param(in, "track3_syn2", "5");
	}
	while(true)
	{
		if(e->get_param(in, "ready", buf, sizeof buf) > 0 && buf[0] == '1') break;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		if(Clock::now() - t0 > std::chrono::seconds(std::getenv("MD_SMOKE_READY_S") ? std::atoi(std::getenv("MD_SMOKE_READY_S")) : 60)) { puts("engine never became ready"); return 1; }	// the first load makes the factory data from a flash image (minutes)
	}
	printf("ready after %.0f ms\n", std::chrono::duration<double, std::milli>(Clock::now() - t0).count());

	if(fresh)	// a newly inserted instance: is the first kit loaded once the catalog is up?
	{
		auto show = [&](const char* what)
		{
			char kn[64] = "?", m0[16] = "?", m5[16] = "?";
			e->get_param(in, "kit_name", kn, sizeof kn);
			e->get_param(in, "track0_machine", m0, sizeof m0);
			e->get_param(in, "track5_machine", m5, sizeof m5);
			printf("%-12s kit=%s track1 machine=%s track6 machine=%s\n", what, kn, m0, m5);
		};
		show("fresh 0 ms");
		std::this_thread::sleep_for(std::chrono::milliseconds(2000));
		show("fresh +2 s");
		return 0;
	}
	if(const char* mf = std::getenv("MD_SMOKE_MASTER")) e->set_param(in, "master_fx", mf);	// 0 = off, 1 = on (the default)
	// machine 0 (GND--) is silent by design; assign real machines to the 3 tracks this test triggers
	// (ids from the OS's own descriptor table, not guaranteed stable across OS versions - fine for a smoke
	// test, not something a real preset should hardcode).
	e->set_param(in, "track0_machine", "16");	// TRXBD
	e->set_param(in, "track1_machine", "17");	// TRXSD
	e->set_param(in, "track2_machine", "22");	// TRXCH

	// SYN1-8: untouched knobs follow the machine's defaults and labels; a touched one keeps its value.
	auto dumpSyn = [&](int t)
	{
		printf("track%d syn:", t);
		for(int p = 1; p <= 8; ++p)
		{
			char k[32], v[16] = "?", n[16] = "?";
			snprintf(k, sizeof k, "track%d_syn%d", t, p);
			e->get_param(in, k, v, sizeof v);
			snprintf(k, sizeof k, "track%d_syn%d_name", t, p);
			e->get_param(in, k, n, sizeof n);
			printf(" %s=%s", n, v);
		}
		printf("\n");
	};
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	dumpSyn(0);
	dumpSyn(3);	// expect TRXB2's defaults except DEC=5
	e->set_param(in, "track1_syn1", "99");
	e->set_param(in, "track1_machine", "28");
	dumpSyn(1);	// machine change after the touch: all defaults
	e->set_param(in, "track1_syn1", "99");
	dumpSyn(1);	// PTCH=99
	e->set_param(in, "track1_machine", "17");
	// LFO PARAM shows the destination's own label: track 4 (TRXB2) param 1 = DEC, param 9 = AMF
	for(const char* v : {"1", "9"})
	{
		char d[16] = "?";
		e->set_param(in, "track0_lfo_track", "3");
		e->set_param(in, "track0_lfo_param", v);
		e->get_param(in, "track0_lfo_param_display", d, sizeof d);
		printf("lfo dest track 4 param %s: %s\n", v, d);
	}
	e->set_param(in, "track0_lfo_track", "0");
	e->set_param(in, "track0_lfo_param", "0");
	// kits (<data dir>/factory etc.): step through the first two, show what loaded; randomise machines 9-16
	{
		char kn[32] = "?", bn[32] = "?", m0[8] = "?", m12[8] = "?";
		e->set_param(in, "kit_next", "1");
		e->get_param(in, "kit_name", kn, sizeof kn);
		e->get_param(in, "bank_name", bn, sizeof bn);
		e->get_param(in, "track0_machine", m0, sizeof m0);
		e->get_param(in, "track12_machine", m12, sizeof m12);
		printf("kit: bank %s, kit %s, track 1 machine %s, track 13 machine %s\n", bn, kn, m0, m12);
		dumpSyn(0);
		e->set_param(in, "track0_syn1", "5");
		e->get_param(in, "kit_name", kn, sizeof kn);
		printf("after an edit: %s\n", kn);
		e->set_param(in, "kit_next", "1");
		e->get_param(in, "kit_name", kn, sizeof kn);
		printf("next kit: %s\n", kn);
		e->set_param(in, "randomize_9_16", "1");
		printf("randomised 9-16:");
		for(int t = 0; t < 16; ++t) { char k[32], v[8]; snprintf(k, sizeof k, "track%d_machine", t); e->get_param(in, k, v, sizeof v); printf(" %s", v); }
		printf("\n");
	}

	// kit / bank steppers, as the skin's buttons fire them (momentary set_param 1)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(std::getenv("MD_SMOKE_WAIT") ? std::atoi(std::getenv("MD_SMOKE_WAIT")) : 1500));	// the catalog is built by its own thread
		auto show = [&](const char* what)
		{
			char kn[64] = "?", bn[64] = "?", m0[16] = "?";
			e->get_param(in, "kit_name", kn, sizeof kn);
			e->get_param(in, "bank_name", bn, sizeof bn);
			e->get_param(in, "track0_machine", m0, sizeof m0);
			printf("%-10s bank=%s kit=%s track1 machine=%s\n", what, bn, kn, m0);
		};
		show("start");

		e->set_param(in, "kit_next", "1"); show("kit_next");
		e->set_param(in, "kit_next", "1"); show("kit_next");
		e->set_param(in, "kit_prev", "1"); show("kit_prev");
		e->set_param(in, "bank_next", "1"); show("bank_next");
		e->set_param(in, "kit_next", "1"); show("kit_next");
		e->set_param(in, "bank_prev", "1"); e->set_param(in, "kit_next", "1"); show("bank_prev+");
	}
	const int secs = argc > 2 ? std::atoi(argv[2]) : 8;
	static int16_t out[128 * 2];
	int peak = 0;
	const uint8_t kick[3] = {0x90, 36, 120}, snare[3] = {0x90, 37, 100}, hat[3] = {0x90, 38, 90};
	const double cpu0 = static_cast<double>(clock());
	const auto t1 = Clock::now();
	auto next = t1;
	// MD_SMOKE_KIT="32,34,49,66": that kit on tracks 1..n instead, every track on every 16th at 120 BPM
	int kit = 0;
	if(const char* k = std::getenv("MD_SMOKE_KIT"))
		for(const char* p = k; *p; ++kit)
		{
			char key[32];
			snprintf(key, sizeof key, "track%d_machine", kit);
			e->set_param(in, key, std::to_string(std::atoi(p)).c_str());
			while(*p && *p != ',') ++p;
			if(*p) ++p;
		}
	// MD_SMOKE_CHURN=1: all 16 tracks play every 16th and a random track gets a random machine every ~0.5 s;
	// prints each second that clipped or went quiet - the long-play distortion hunt
	const bool churn = std::getenv("MD_SMOKE_CHURN") != nullptr;
	int secPeak = 0, secClip = 0, secBlocks = 0;
	uint32_t rs = 12345;
	auto rnd = [&] { rs = rs * 1664525u + 1013904223u; return rs >> 8; };
	if(std::getenv("MD_SMOKE_ROMOFF")) e->set_param(in, "rom_enabled", "0");
	if(const char* mv = std::getenv("MD_SMOKE_VOICES")) e->set_param(in, "max_voices", mv);
	if(churn)
		for(int t = 0; t < 16; ++t) e->set_param(in, ("track" + std::to_string(t) + "_machine").c_str(), std::to_string(16 + t * 6).c_str());
	// MD_SMOKE_FX="fltw=0,eqg=127,...": set those per-track FX params on tracks 1-3 first (proves the track FX act)
	if(const char* fx = std::getenv("MD_SMOKE_FX"))
		for(const char* q = fx; *q;)
		{
			char k[32], v[16];
			if(std::sscanf(q, "%31[^=]=%15[^,]", k, v) == 2)
				for(int t = 0; t < 3; ++t) e->set_param(in, ("track" + std::to_string(t) + "_" + k).c_str(), v);
			while(*q && *q != ',') ++q;
			if(*q) ++q;
		}
	const int blocks = secs * 44100 / 128;
	for(int b = 0; b < blocks; ++b)
	{
		if(churn)
		{
			if(b % 43 == 0)
				for(int t = 0; t < 16; ++t) { const uint8_t on[3] = {0x90, static_cast<uint8_t>(36 + t), 100}; e->midi(in, on, 3); }
			if(b % 172 == 0)
				e->set_param(in, ("track" + std::to_string(rnd() % 16) + "_machine").c_str(), std::to_string(16 + rnd() % 112).c_str());
		}
		if(kit)
		{
			if(b % 43 == 0)
				for(int t = 0; t < kit; ++t)
				{
					const uint8_t on[3] = {0x90, static_cast<uint8_t>(36 + t), 110};
					e->midi(in, on, 3);
				}
		}
		else
		{
			if(b % 12 == 0) e->midi(in, kick, 3);
			if(b % 24 == 12) e->midi(in, snare, 3);
			if(b % 6 == 3) e->midi(in, hat, 3);
		}
		e->render(in, out, 128);
		for(int i = 0; i < 128 * 2; ++i)
		{
			if(std::abs(out[i]) > peak) peak = std::abs(out[i]);
			if(churn) { secPeak = std::max(secPeak, std::abs(int(out[i]))); if(std::abs(int(out[i])) >= 32767) ++secClip; }
		}
		if(churn && ++secBlocks == 344)
		{
			char ur[16]; e->get_param(in, "underruns", ur, sizeof ur);
			printf("t=%3ds peak=%5d clipped=%5d underruns=%s\n", (b + 1) / 344, secPeak, secClip, ur);
			secPeak = secClip = secBlocks = 0;
		}
		next += std::chrono::microseconds(2902);
		std::this_thread::sleep_until(next);
	}
	e->get_param(in, "underruns", buf, sizeof buf);
	printf("%.1f s: peak %d/32767, underruns %s, wall cpu %.0f ms\n", double(secs), peak, buf,
		(static_cast<double>(clock()) - cpu0) / CLOCKS_PER_SEC * 1000.0);
	e->destroy(in);
	return 0;
}
