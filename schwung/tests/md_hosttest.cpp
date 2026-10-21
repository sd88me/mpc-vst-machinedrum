// Loads the Machinemodule plugin (dsp.so) the way Schwung's chain host does and drives it from a simulated audio callback (128 frames
// every 2.9 ms, real time), outside Move's own audio. Run it on the Move (it spawns md-engine on cores 0-2 like the real module):
//
//   md-hosttest <module dir> [out.wav]
//
// The module dir holds dsp.so, md-engine and os/<OS 1.63 .syx> (factory/FACTORY.syx and factory/ROM_SAMPLES.bin when imported).
// Phases: boot; the served pages (ui_hierarchy parses, its size); a machine pick (its SYN defaults come back from the engine); notes on
// four tracks with MD accents; a kit (when imported); the engine killed (respawn, the table replayed); a busy pattern on all 16 tracks
// for 10 s (underruns, load); destroy (the engine exits). Exit code 0 = every check passed.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>

#define _Static_assert static_assert
#include "host_api/plugin_api_v1.h"
#undef _Static_assert

using Clock = std::chrono::steady_clock;

namespace
{
	float bpm() { return 120.f; }
	int clockStatus() { return MOVE_CLOCK_STATUS_STOPPED; }

	plugin_api_v2_t* api = nullptr;
	void* inst = nullptr;
	std::vector<int16_t> recording;
	bool record = false;
	int failures = 0;
	Clock::time_point nextBlock;
	double maxCallbackUs = 0;

	void check(bool ok, const char* what)
	{
		std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
		if(!ok) ++failures;
	}

	std::string get(const char* key)
	{
		static char buf[65536];
		const int n = api->get_param(inst, key, buf, sizeof buf);
		return n < 0 ? std::string("<none>") : std::string(buf, size_t(n));
	}

	void set(const char* key, const char* val) { api->set_param(inst, key, val); }

	int16_t lastPeak = 0;
	// Plays `ms` of callbacks at the real rate; returns the output peak.
	int16_t run(double ms)
	{
		int16_t peak = 0;
		const int blocks = int(ms / (1000.0 * 128 / 44100.0));
		for(int b = 0; b < blocks; ++b)
		{
			std::this_thread::sleep_until(nextBlock);
			nextBlock += std::chrono::microseconds(2902);
			int16_t out[256];
			const auto t0 = Clock::now();
			api->render_block(inst, out, 128);
			maxCallbackUs = std::max(maxCallbackUs, std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
			for(int16_t s : out) peak = std::max<int16_t>(peak, int16_t(std::abs(int(s))));
			if(record) recording.insert(recording.end(), out, out + 256);
		}
		lastPeak = peak;
		return peak;
	}

	void note(int n, int vel)
	{
		const uint8_t m[3] = {0x90, uint8_t(n), uint8_t(vel)};
		api->on_midi(inst, m, 3, MOVE_MIDI_SOURCE_EXTERNAL);
	}

	int statusInt(const char* field)
	{
		const std::string s = get("status");
		const auto p = s.find(std::string("\"") + field + "\":");
		return p == std::string::npos ? -1 : std::atoi(s.c_str() + p + std::strlen(field) + 3);
	}

	void writeWav(const char* path)
	{
		std::FILE* f = std::fopen(path, "wb");
		if(!f) return;
		const uint32_t data = uint32_t(recording.size() * 2), riff = 36 + data, rate = 44100, byteRate = rate * 4, fmtLen = 16;
		const uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
		std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f); std::fwrite(&fmtLen, 4, 1, f);
		std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f); std::fwrite(&rate, 4, 1, f); std::fwrite(&byteRate, 4, 1, f);
		std::fwrite(&align, 2, 1, f); std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
		std::fwrite(recording.data(), 2, recording.size(), f);
		std::fclose(f);
	}
}

int main(int argc, char** argv)
{
	if(argc < 2) { std::fprintf(stderr, "usage: md-hosttest <module dir> [out.wav]\n"); return 2; }
	const std::string dir = argv[1];
	host_api_v1_t host{};
	host.get_bpm = bpm;
	host.get_clock_status = clockStatus;
	void* so = dlopen((dir + "/dsp.so").c_str(), RTLD_NOW | RTLD_LOCAL);
	if(!so) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }
	auto init = reinterpret_cast<move_plugin_init_v2_fn>(dlsym(so, MOVE_PLUGIN_INIT_V2_SYMBOL));
	if(!init) { std::fprintf(stderr, "no %s\n", MOVE_PLUGIN_INIT_V2_SYMBOL); return 3; }
	api = init(&host);
	const auto tCreate = Clock::now();
	inst = api->create_instance(dir.c_str(), nullptr);
	std::printf("create_instance: %.1f ms\n", std::chrono::duration<double, std::milli>(Clock::now() - tCreate).count());
	{
		// The callback as Move runs it: SCHED_FIFO 70 on core 3 (when allowed: run as root). Otherwise a late wake-up here makes two
		// callbacks back to back, and at LTNCY 1 the second finds its block not yet rendered: an underrun that Move would not have.
		sched_param sp{};
		sp.sched_priority = 70;
		const bool rt = sched_setscheduler(0, SCHED_FIFO, &sp) == 0;
		cpu_set_t c3;
		CPU_ZERO(&c3);
		CPU_SET(3, &c3);
		if(rt) sched_setaffinity(0, sizeof c3, &c3);
		std::printf("callback thread: %s\n", rt ? "SCHED_FIFO 70 on core 3, as Move's" : "normal priority (run as root for Move's): underruns at LTNCY 1 may be the test's own");
	}
	nextBlock = Clock::now();

	std::printf("boot\n");
	const auto t0 = Clock::now();
	while(statusInt("phase") != 2 && Clock::now() - t0 < std::chrono::seconds(60)) run(100);
	const double bootMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
	std::printf("  %s\n", get("status").c_str());
	check(statusInt("phase") == 2, "the engine is ready");
	std::printf("  boot %.0f ms\n", bootMs);
	run(500);	// the replay lands

	std::printf("pages\n");
	{
		const std::string h = get("ui_hierarchy");
		std::printf("  ui_hierarchy %zu bytes, chain_params %zu bytes\n", h.size(), get("chain_params").size());
		check(h.size() > 1000 && h.front() == '{' && h.back() == '}', "ui_hierarchy is served");
		check(h.find("\"syn_0\"") != std::string::npos, "a Synth page for the machine in use (GND-EM)");
		check(h.find("TRX-BD") != std::string::npos, "the machine list holds the OS's machines");
		if(argc > 3) { std::FILE* f = std::fopen(argv[3], "wb"); if(f) { std::fwrite(h.data(), 1, h.size(), f); std::fclose(f); } }
	}

	std::printf("machines\n");
	set("t01_machine", "TRX-BD");
	set("t02_machine", "TRX-SD");
	set("t03_machine", "EFM-CB");
	set("t09_machine", "TRX-CH");
	run(300);
	std::printf("  t01 %s syn1..3 %s %s %s, ui_machine (focus %s) %s\n", get("t01_machine").c_str(), get("t01_syn1").c_str(), get("t01_syn2").c_str(),
		get("t01_syn3").c_str(), get("ui_track").c_str(), get("ui_machine").c_str());
	check(get("t01_machine") == "TRX-BD", "t01 is TRX-BD");
	check(get("t01_syn2") != "0", "TRX-BD's own starting values came back from the engine (DEC is not 0)");
	check(get("is_loading") == "1", "a new machine set re-plans the pages (is_loading)");
	{
		const std::string h = get("ui_hierarchy");
		check(h.find("\"PTCH\"") != std::string::npos && h.find("\"HARM\"") != std::string::npos, "the TRX-BD Synth page has its own labels");
	}

	std::printf("notes\n");
	record = argc > 2;
	const int16_t silent = run(200);
	note(36, 127); const int16_t bd = run(400);
	note(37, 90); const int16_t sd = run(400);
	note(36, 60); const int16_t soft = run(400);
	std::printf("  peaks: before %d, BD accented %d, SD %d, BD unaccented %d\n", silent, bd, sd, soft);
	check(bd > 1000, "the kick sounds");
	check(sd > 500, "the snare sounds");
	check(soft < bd, "an unaccented hit is softer (MD mode)");
	check(get("ui_track") == "1", "the editor follows the last hit pad (track 1)");
	set("vel_mode", "VEL");
	note(36, 127); const int16_t v127 = run(400);
	note(36, 64); const int16_t v64 = run(400);
	std::printf("  VEL mode: 127 -> %d, 64 -> %d (ratio %.2f, want ~0.50)\n", v127, v64, double(v64) / std::max<int16_t>(v127, 1));
	check(v64 < v127 * 0.6 && v64 > v127 * 0.4, "VEL mode scales the level with velocity");
	set("vel_mode", "MD");

	std::printf("kits\n");
	{
		const std::string h = get("ui_hierarchy");
		const bool haveKits = h.find("\"options\":[\"Init\",") != std::string::npos;
		std::printf("  factory kits %s\n", haveKits ? "listed" : "not imported (Init only)");
		if(haveKits)
		{
			set("kit", "1");
			run(500);
			std::printf("  kit 1: t01 %s, t02 %s\n", get("t01_machine").c_str(), get("t02_machine").c_str());
			note(36, 127); const int16_t k = run(500);
			check(k > 500, "kit 1's track 1 sounds");
		}
	}

	std::printf("state\n");
	const std::string state = get("state");
	std::printf("  %zu bytes\n", state.size());
	set("t05_machine", "EFM-BD");
	run(300);
	set("state", state.c_str());
	run(300);
	check(get("state") == state, "the state round-trips");

	std::printf("respawn\n");
	{
		const int pid = statusInt("pid");
		kill(pid, SIGKILL);
		const auto tk = Clock::now();
		while((statusInt("phase") != 2 || statusInt("pid") == pid) && Clock::now() - tk < std::chrono::seconds(60)) run(100);
		std::printf("  back in %.0f ms: %s\n", std::chrono::duration<double, std::milli>(Clock::now() - tk).count(), get("status").c_str());
		run(500);
		note(36, 127);
		check(run(400) > 1000, "after a respawn the kick sounds again (the table was replayed)");
		check(get("state") == state, "and the table is unchanged");
	}

	// every track busy, with kit 1's sounds: more than one core can render in real time on a Move (md-busybench: 2.9 ms of voices
	// per 2.9 ms block). Reported as is, then with the safety valves (VOICES, LTNCY), which must hold it without a gap.
	auto busy = [&](const char* label, double seconds)
	{
		set("peak", "0");
		const int u0 = statusInt("underruns"), s0 = statusInt("skips");
		for(int step = 0; step < int(seconds * 8); ++step)	// 16ths at 120 BPM
		{
			for(int t = 0; t < 16; ++t) if((step * 7 + t * 5) % 16 < (t < 2 ? 6 : 4)) note(36 + t, 100 + (step % 4 == 0) * 27);
			run(125);
		}
		const int u = statusInt("underruns") - u0, sk = statusInt("skips") - s0;
		std::printf("  %-34s underruns %4d, skips %4d, load %s%%, peak %s%%\n", label, u, sk, get("load").c_str(), get("peak").c_str());
		return u;
	};
	// a typical beat on kit 1 (kick quarters, snare 2 and 4, hat 16ths, open hat, clap, a tom fill): the default settings (LTNCY 1,
	// master FX Full, no voice limit) must play it without a gap
	std::printf("groove (kit 1, defaults: LTNCY 1, FX Full)\n");
	{
		set("kit", "1");
		run(800);
		set("peak", "0");
		const int u0 = statusInt("underruns");
		for(int step = 0; step < 160; ++step)
		{
			const int st = step % 16;
			if(st % 4 == 0) note(36, 120);
			if(st == 4 || st == 12) note(37, 110);
			if(st != 14) note(44, st % 2 ? 80 : 110);
			if(st == 14) note(45, 100);
			if(st == 12 && (step / 16) % 2) note(41, 100);
			if(st >= 13 && (step / 16) % 4 == 3) note(39, 100);
			const int ub = statusInt("underruns");
			run(125);
			if(statusInt("underruns") != ub) std::printf("    step %d (%d in the bar): +%d underruns\n", step, st, statusInt("underruns") - ub);
		}
		const int u = statusInt("underruns") - u0;
		std::printf("  20 s: underruns %d, load %s%%, peak %s%%\n", u, get("load").c_str(), get("peak").c_str());
		check(u == 0, "a typical beat plays without a gap at the defaults");
	}
	std::printf("busy pattern (16 tracks, kit 1's sounds; reported, not checked: more than the Move can render)\n");
	{
		static const char* const kMachines[16] = {"TRX-BD", "TRX-SD", "EFM-XT", "EFM-XT", "TRX-XT", "TRX-CP", "TRX-RS", "TRX-CB", "TRX-CH", "TRX-OH", "TRX-CY", "EFM-CY", "E12-BD", "P-I-BD", "EFM-HH", "TRX-MA"};
		for(int t = 0; t < 16; ++t) { char k[16]; std::snprintf(k, sizeof k, "t%02d_machine", t + 1); set(k, kMachines[t]); }
		run(500);
		busy("no limit, LTNCY 1, FX Full", 6);
		set("voices", "6");
		set("depth", "2");
		busy("VOICES 6, LTNCY 2, FX Full", 8);
		set("master_on", "Rev+Echo");
		busy("VOICES 6, LTNCY 2, FX Rev+Echo", 6);
		check(get("master_on") == "Rev+Echo", "the master FX mode reads back");
		set("master_on", "Full");
		set("voices", "16");
		set("depth", "1");
	}
	std::printf("  the callback's worst block: %.0f us\n", maxCallbackUs);
	record = false;
	if(argc > 2) writeWav(argv[2]);

	std::printf("destroy\n");
	const int pid = statusInt("pid");
	api->destroy_instance(inst);
	std::this_thread::sleep_for(std::chrono::seconds(2));
	check(kill(pid, 0) != 0, "the engine process is gone");
	std::printf("%s (%d failures)\n", failures ? "FAILED" : "ALL OK", failures);
	return failures ? 1 : 0;
}
