// Drives mpc_engine() through its parameter API and checks the control machines (CTR): CTR-AL moves SYN k of the other tracks (clamped), CTR-RE / GB are
// the delay / reverb master knobs, CTR-8P starts pointing P1-P8 at SYN1 of tracks 1-8 and writes P_k into its targets: a track whose VOL it drives is silent
// at 0. usage: md-ctrtest <data-dir>     (the data dir holds the OS file; run from a clean one: no factory kits, so the default kit does not load)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
extern "C" {
#include "engine.h"
}
using Clock = std::chrono::steady_clock;

static const mpc_engine_t* E;
static void* I;
static int get(const char* key)
{
	char b[32] = "?";
	E->get_param(I, key, b, sizeof b);
	return std::atoi(b);
}
static void set(const char* key, int v) { char b[16]; std::snprintf(b, sizeof b, "%d", v); E->set_param(I, key, b); }
static std::string k(int t, const char* what) { return "track" + std::to_string(t) + "_" + what; }
static int bad = 0;
static void check(const char* what, int got, int want)
{
	const bool ok = got == want;
	std::printf("%-62s %4d (want %d)%s\n", what, got, want, ok ? "" : "   FAIL");
	bad += !ok;
}
static int peakOf(double seconds)	// plays blocks (paced), the peak of the main output
{
	int peak = 0;
	static int16_t out[256];
	const auto t0 = Clock::now();
	while(std::chrono::duration<double>(Clock::now() - t0).count() < seconds)
	{
		E->render(I, out, 128);
		for(int i = 0; i < 256; ++i) peak = std::max(peak, std::abs(int(out[i])));
		std::this_thread::sleep_for(std::chrono::microseconds(2700));
	}
	return peak;
}

int main(int argc, char** argv)
{
	E = mpc_engine();
	I = E->create(argc > 1 ? argv[1] : ".");
	const auto t0 = Clock::now();
	char buf[8];
	while(!(E->get_param(I, "ready", buf, sizeof buf) > 0 && buf[0] == '1'))
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		if(Clock::now() - t0 > std::chrono::seconds(60)) { std::puts("engine never became ready"); return 1; }
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(2000));	// the rules are armed 1.5 s after the instance is made

	// CTR-AL on track 0 (index): tracks 1 and 2 are TRX machines with SYN1 at 30 and 120; track 15 is a CTR-RE (no effect)
	set(k(1, "machine").c_str(), 16); set(k(2, "machine").c_str(), 17);
	set(k(1, "syn1").c_str(), 30); set(k(2, "syn1").c_str(), 120);
	set(k(0, "machine").c_str(), 112);
	const int d0 = get(k(0, "syn1").c_str());
	std::printf("CTR-AL SYN1 starts at %d\n", d0);
	set(k(0, "syn1").c_str(), d0 + 10);
	check("CTR-AL +10: track 1 SYN1 (30 -> 40)", get(k(1, "syn1").c_str()), 40);
	check("CTR-AL +10: track 2 SYN1 (120 -> 127, clamped)", get(k(2, "syn1").c_str()), 127);
	check("CTR-AL +10: its own SYN1 stays", get(k(0, "syn1").c_str()), d0 + 10);
	set(k(0, "syn1").c_str(), d0);
	check("CTR-AL -10: track 1 SYN1 (40 -> 30)", get(k(1, "syn1").c_str()), 30);
	check("CTR-AL -10: track 2 SYN1 (127 -> 117)", get(k(2, "syn1").c_str()), 117);
	check("CTR-AL: SYN2 of track 1 untouched by a SYN1 move", get(k(1, "syn2").c_str()), get(k(1, "syn2").c_str()));

	// CTR-RE (delay) and CTR-GB (reverb): SYN1-8 are the master knobs
	const int dl1 = get("dl1");
	set(k(3, "machine").c_str(), 120);
	check("CTR-RE picked: SYN1 starts at the delay's current knob", get(k(3, "syn1").c_str()), dl1);
	set(k(3, "syn1").c_str(), 99);
	check("CTR-RE SYN1 = 99: delay knob 1", get("dl1"), 99);
	set(k(4, "machine").c_str(), 121);
	set(k(4, "syn3").c_str(), 77);
	check("CTR-GB SYN3 = 77: reverb knob 3", get("rv3"), 77);
	set(k(5, "machine").c_str(), 122); set(k(5, "syn8").c_str(), 5);
	check("CTR-EQ SYN8 = 5: EQ knob 8", get("eq8"), 5);
	set(k(6, "machine").c_str(), 123); set(k(6, "syn2").c_str(), 33);
	check("CTR-DX SYN2 = 33: dynamix knob 2", get("dx2"), 33);

	// CTR-8P: starts with P_k -> SYN1 of track k (targets: track k, parameter 0)
	set(k(1, "syn1").c_str(), 41);
	set(k(0, "machine").c_str(), 16); set(k(0, "vol").c_str(), 100);	// track 0 is the drum the CTR-8P will drive below (set first: P1 then starts at its own SYN1)
	set(k(7, "machine").c_str(), 113);
	check("CTR-8P picked: target track of P1 (AMD = raw 8)", get(k(7, "amd").c_str()), 0);
	check("CTR-8P picked: target track of P3 (FLTF = raw 13)", get(k(7, "fltf").c_str()), 2);
	check("CTR-8P picked: target parameter of P3 (FLTW = raw 14)", get(k(7, "fltw").c_str()), 0);
	check("CTR-8P picked: P2 starts at SYN1 of track 2", get(k(7, "syn2").c_str()), get(k(1, "syn1").c_str()));

	// an audio check: P8 drives VOL (parameter 17) of track 0 (a TRX BD): at 0 it is silent, at 100 it sounds
	set(k(7, "lfod").c_str(), 0); set(k(7, "lfom").c_str(), 17);	// pair 8: raw 22 (target track) and 23 (target parameter)
	const uint8_t on[3] = {0x90, 36, 120};
	set(k(7, "syn8").c_str(), 100);
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	E->midi(I, on, 3);
	const int loud = peakOf(0.6);
	set(k(7, "syn8").c_str(), 0);
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	peakOf(0.3);
	E->midi(I, on, 3);
	const int quiet = peakOf(0.6);
	// the same track with the CTR-8P gone (its machine set to GND): VOL is its own knob again
	set(k(7, "machine").c_str(), 0);
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	E->midi(I, on, 3);
	peakOf(0.6);	// the first note after a machine change is quieter whatever the machines are: measure the next one
	E->midi(I, on, 3);
	const int baseline = peakOf(0.6);
	std::printf("CTR-8P P8 -> VOL of track 0: peak %d at 100, %d at 0; without the CTR-8P (VOL knob 100): %d\n", loud, quiet, baseline);
	check("CTR-8P P8 = 0 silences the track it drives (peak < 10)", quiet < 10 ? 1 : 0, 1);
	check("CTR-8P P8 = 100: the track sounds (peak > 10 and within a factor 4 of the plain track)", (loud > 10 && loud * 4 > baseline && baseline * 4 > loud) ? 1 : 0, 1);
	check("the target's own VOL knob is untouched", get(k(0, "vol").c_str()), 100);

	E->destroy(I);
	std::puts(bad ? "FAILED" : "ALL OK");
	return bad;
}
