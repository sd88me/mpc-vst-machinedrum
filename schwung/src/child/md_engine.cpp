// md-engine: the engine process of one Machinemodule instance. Spawned by the plugin's supervisor thread with the shared segment
// (src/common/md_shm.h) on fd 3:
//
//   md-engine <OS 1.63 .syx> <ROM_SAMPLES.bin or -> <log file> <parent pid> [fifo priority, default 10]
//
// Runs the whole Machinedrum (the repo's engine: voice DSP in the dsp56300 JIT, the OS's control code, track effects, mixer and
// master effects) on this process's main thread, pinned to cores 0-2 (core 3 is Move's audio core), SCHED_FIFO at the given
// priority when allowed, SCHED_OTHER otherwise. Renders ahead of the audio callback by `depth` blocks and sleeps on a futex
// between blocks. The kit values arrive as commands into a table (desktop/src/core/KitModel) that EngineFeeder sends to the
// engine, as the desktop plugin does.
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <linux/capability.h>
#include <linux/futex.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "EngineFeeder.h"
#include "Firmware.h"
#include "MdEngine.h"
#include "master_worker.h"
#include "md_shm.h"

using Clock = std::chrono::steady_clock;
using md::engine::Engine;

namespace
{
	FILE* g_log = nullptr;

	void logf(const char* fmt, ...)
	{
		if(!g_log) return;
		const std::time_t t = std::time(nullptr);
		char ts[32];
		std::strftime(ts, sizeof ts, "%H:%M:%S", std::localtime(&t));
		std::fprintf(g_log, "%s [%d] ", ts, int(getpid()));
		va_list ap;
		va_start(ap, fmt);
		std::vfprintf(g_log, fmt, ap);
		va_end(ap);
		std::fputc('\n', g_log);
		std::fflush(g_log);
	}

	void futexWait(std::atomic<uint32_t>* addr, uint32_t expected)
	{
		timespec ts{0, 50 * 1000 * 1000};	// bounded, so shutdown and a dead parent are noticed
		syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAIT, expected, &ts, nullptr, 0);
	}

	void fail(mm::shm::Segment& s, const char* why)
	{
		std::snprintf(s.error, sizeof s.error, "%s", why);
		s.phase.store(uint32_t(mm::shm::Phase::Failed), std::memory_order_release);
		logf("FAILED: %s", why);
	}

	int loadRomSamples(Engine& eng, const char* path)
	{
		if(!path || !std::strcmp(path, "-")) return 0;
		std::FILE* rf = std::fopen(path, "rb");
		if(!rf) return 0;
		int records = 0;
		char magic[4];
		std::vector<uint32_t> words;
		uint32_t head[2];
		if(std::fread(magic, 1, 4, rf) == 4)
			while(std::fread(head, 4, 2, rf) == 2 && head[1] > 0 && head[1] < 0x800000)
			{
				words.resize(head[1]);
				if(std::fread(words.data(), 4, head[1], rf) != head[1]) break;
				eng.voices().writeP(head[0], words.data(), words.size());
				++records;
			}
		std::fclose(rf);
		return records;
	}

	// The engine's side of a machine change: the user's picks start from the machine's own values, which go back to the plugin.
	struct Feed final : mm::EngineFeeder::Host
	{
		mm::shm::Segment* seg = nullptr;
		mm::KitValues* table = nullptr;
		uint32_t userPick = 0;	// bit t: the next machine change of track t is the user's

		bool takeDefaults(int t) override
		{
			const bool user = (userPick >> t) & 1;
			userPick &= ~(1u << t);
			return user;
		}
		void defaultsChosen(int t, const int* values, int count) override
		{
			auto& tr = table->track[size_t(t)];
			for(int i = 0; i < count; ++i) tr.raw[size_t(i)] = values[i];	// this table now holds them, so they are not overwritten
			auto& d = seg->defaults[t];
			d.machine = uint8_t(tr.machine);
			d.count = uint8_t(count);
			for(int i = 0; i < count; ++i) d.values[i] = uint8_t(values[i]);
			d.seq.fetch_add(1, std::memory_order_release);
		}
	};

	void apply(const mm::shm::Cmd& c, mm::KitValues& v, Feed& feed, Engine& eng, double& bpm, uint32_t& trackOuts)
	{
		using mm::shm::Op;
		const int t = std::min<int>(c.track, mm::kTracks - 1);
		auto& tr = v.track[size_t(t)];
		switch(Op(c.op))
		{
		case Op::Trigger: eng.host().trigger(t, std::clamp<int>(c.b, 1, 127), c.c != 0); break;
		case Op::SetMachine:
			tr.machine = std::clamp<int>(c.c, 0, mm::shm::kMachineIds - 1);
			if(c.b) feed.userPick |= 1u << t; else feed.userPick &= ~(1u << t);
			break;
		case Op::SetRaw: if(c.b < mm::kRaw) tr.raw[c.b] = std::clamp<int>(c.c, 0, 127); break;
		case Op::SetLevel: tr.level = std::clamp<int>(c.c, 0, 127); break;
		case Op::SetMute: tr.mute = c.c != 0; break;
		case Op::SetLfo: if(c.b < mm::kNumLfo) tr.lfo[c.b] = std::clamp<int>(c.c, 0, mm::kLfoMax[c.b]); break;
		case Op::SetMaster: if(c.b < mm::kMasterBytes) v.master[c.b] = std::clamp<int>(c.c, 0, 127); break;
		case Op::SetMasterOn: v.masterOn = c.c != 0; break;
		case Op::SetAccent: eng.host().setAccentAmount(c.c); break;
		case Op::SetBpm: bpm = c.c / 100.0; break;
		case Op::SetTrackOuts: trackOuts = uint32_t(c.c) & 0xffff; break;
		case Op::SetMasterMode: eng.setMasterReverbEchoOnly(c.c == 1); break;	// between blocks: the master thread is idle
		case Op::SetVoices: eng.host().setMaxActiveVoices(c.c > 0 ? c.c : 2 * mm::kTracks); break;
		case Op::None: break;
		}
	}
}

int main(int argc, char** argv)
{
	if(argc < 5) { std::fprintf(stderr, "usage: md-engine <OS.syx> <ROM_SAMPLES.bin|-> <log> <parent pid> [fifo]\n"); return 2; }
	g_log = std::fopen(argv[3], "a");
	const pid_t parent = pid_t(std::atoi(argv[4]));	// as the plugin sees itself: Move's host runs in its own pid namespace
	const int fifo = argc > 5 ? std::atoi(argv[5]) : 10;
	prctl(PR_SET_PDEATHSIG, SIGKILL);	// die with the plugin's supervisor thread
	if(getppid() != parent) { logf("parent %d is gone (ppid %d): exiting", int(parent), int(getppid())); return 1; }
	prctl(PR_SET_NAME, "md-engine");
	{
		// No locked memory: the emulator mlocks its memory maps (dsp56kBase/mmuhelper.cpp, failure ignored), which map a few pages
		// many times over: ~5 GB of address space. MoveOriginal's RLIMIT_MEMLOCK is unlimited and we inherit it; locked like that, an
		// engine took seconds to exit (respawns, unloading). The Move has no swap, so locking buys nothing. Any capabilities
		// inherited are dropped too (CAP_IPC_LOCK would bypass the limit); the supervisor makes the threads real-time.
		const rlimit rl{64 * 1024, 64 * 1024};
		setrlimit(RLIMIT_MEMLOCK, &rl);
		__user_cap_header_struct hdr{_LINUX_CAPABILITY_VERSION_3, 0};
		__user_cap_data_struct data[2] = {};
		syscall(SYS_capset, &hdr, data);
	}

	auto* seg = static_cast<mm::shm::Segment*>(mmap(nullptr, sizeof(mm::shm::Segment), PROT_READ | PROT_WRITE, MAP_SHARED, 3, 0));
	if(seg == MAP_FAILED) { logf("cannot map the shared segment"); return 3; }
	auto& s = *seg;
	if(s.magic != mm::shm::kMagic || s.version != mm::shm::kVersion) { logf("segment magic/version mismatch"); return 3; }
	s.child_pid.store(uint32_t(getpid()));

	cpu_set_t cpus;	// cores 0-2, never Move's audio core
	CPU_ZERO(&cpus);
	CPU_SET(0, &cpus); CPU_SET(1, &cpus); CPU_SET(2, &cpus);
	sched_setaffinity(0, sizeof cpus, &cpus);

	s.phase.store(uint32_t(mm::shm::Phase::Loading));
	const auto t0 = Clock::now();
	std::unique_ptr<md::fw::Firmware> fw;
	std::unique_ptr<Engine> eng;
	try
	{
		fw = std::make_unique<md::fw::Firmware>(md::fw::loadFirmware(argv[1]));
		auto container = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(argv[1])));
		eng = std::make_unique<Engine>(*fw, std::vector<uint8_t>(container.sections.at(0).data));
	}
	catch(const std::exception& e)
	{
		char why[150];
		std::snprintf(why, sizeof why, "OS file: %s", e.what());
		fail(s, why);
		return 4;
	}
	const int romRecords = loadRomSamples(*eng, argv[2]);
	s.romSamples.store(romRecords > 0 ? 1 : 0);
	{
		Engine::Output o;
		for(int b = 0; b < 40; ++b) eng->render(o);
	}
	eng->enableMasterFx(true);	// after the warm-up: the section must not run before it has its parameters (the first block sets them)
	eng->host().setSilenceRelease(128, 138);	// a voice below -96 dBFS for 100 ms stops costing DSP time (vst/engine.cpp)
	{
		// Pre-warm: play every machine once, so the JIT compiles its code now and not on its first hit (measured on the Move: 15-18 ms
		// for a first hit, a dropout at any latency). Then back to the empty machine; its tail is released before the first block plays.
		const auto w0 = Clock::now();
		auto& h = eng->host();
		Engine::Output o;
		int warmed = 0;
		for(const auto& m : eng->os().machines())
		{
			const std::string g = m.name.substr(0, 3);
			if(m.name.empty() || g == "MID" || g == "RAM" || g == "INP" || (g == "ROM" && (!romRecords || std::atoi(m.name.c_str() + 3) > 32))) continue;
			h.setMachine(0, m.id);
			h.trigger(0, 127, false);
			for(int b = 0; b < 8; ++b) eng->render(o);
			++warmed;
		}
		h.setMachine(0, 0);
		h.trigger(0, 1, false);
		for(int b = 0; b < 600; ++b) eng->render(o);	// 0.4 s: every tail released
		logf("pre-warmed %d machines in %.0f ms", warmed, std::chrono::duration<double, std::milli>(Clock::now() - w0).count());
	}
	for(const auto& m : eng->os().machines())
	{
		if(m.id >= mm::shm::kMachineIds) continue;
		auto& d = s.machines[m.id];
		std::snprintf(d.name, sizeof d.name, "%s", m.name.c_str());
		for(int k = 0; k < 8; ++k) std::snprintf(d.syn[k], sizeof d.syn[k], "%s", m.params[size_t(k)].c_str());
		d.valid = 1;
	}
	logf("ready in %.0f ms (ROM samples: %d records)", std::chrono::duration<double, std::milli>(Clock::now() - t0).count(), romRecords);

	if(fifo > 0)	// realtime only once booted: boot work must not starve Move
	{
		sched_param sp{};
		sp.sched_priority = fifo;
		if(sched_setscheduler(0, SCHED_FIFO, &sp) != 0) logf("SCHED_FIFO %d refused, staying SCHED_OTHER", fifo);
	}

	mm::KitValues table = mm::KitValues::defaults();
	mm::EngineFeeder feeder;
	Feed feed;
	feed.seg = &s;
	feed.table = &table;
	double bpm = 120.0;
	uint32_t trackOuts = 0;
	const bool diag = std::getenv("MD_DIAG") != nullptr;	// stage times in the log every ~3 s
	eng->timingOn = diag;
	double diagUs = 0, feedUs = 0;
	uint32_t diagBlocks = 0, diagMax = 0;
	if(const char* sp = std::getenv("MD_SPIN_US")) mm::g_spinUs = std::atoi(sp);
	mm::MasterWorker master;	// created after the FIFO switch above: the thread inherits it
	master.eng = eng.get();
	master.tidOut = &s.tids[1];
	master.start();
	s.tids[0].store(uint32_t(syscall(SYS_gettid)), std::memory_order_release);
	while(!s.tids[1].load(std::memory_order_acquire)) usleep(100);
	uint32_t posted = 0;

	// start in step with the callback: the first block rendered is the one it will play next
	uint32_t next = s.host_block.load(std::memory_order_acquire);
	s.produced.store(next, std::memory_order_release);
	s.phase.store(uint32_t(mm::shm::Phase::Ready), std::memory_order_release);

	while(!s.shutdown.load(std::memory_order_acquire))
	{
		if(getppid() != parent) break;	// the host process is gone
		s.heartbeat.fetch_add(1, std::memory_order_relaxed);
		const uint32_t hb = s.host_block.load(std::memory_order_acquire);
		const uint32_t depth = std::clamp<uint32_t>(s.depth.load(std::memory_order_relaxed), 1, mm::shm::kMaxDepth);
		if(int32_t(next - hb) < 0)	// fell behind: skip forward, the engine keeps its own time
		{
			s.skips.fetch_add(hb - next, std::memory_order_relaxed);
			next = hb;
			s.produced.store(next, std::memory_order_release);
		}
		if(int32_t(next - (hb + depth)) >= 0) { futexWait(&s.host_block, hb); continue; }

		const auto b0 = Clock::now();
		// commands first, so this block hears them (triggers at its start, as on the hardware's tick)
		uint32_t r = s.cmd_read.load(std::memory_order_relaxed);
		const uint32_t w = s.cmd_write.load(std::memory_order_acquire);
		bool triggered = false;
		for(; r != w; ++r)
		{
			const auto& c = s.cmds[r & (mm::shm::kCmdSlots - 1)];
			if(mm::shm::Op(c.op) == mm::shm::Op::Trigger && !triggered)
			{
				feeder.apply(*eng, table, bpm, feed);	// the values sent before a trigger are the ones it plays (a machine change lands on it)
				triggered = true;
			}
			apply(c, table, feed, *eng, bpm, trackOuts);
		}
		s.cmd_read.store(r, std::memory_order_release);

		const int slot = int(next & (mm::shm::kSlots - 1));
		int32_t* main = s.main[slot];
		eng->dryMute = trackOuts;
		for(int sb = 0; sb < mm::kSub; ++sb)
		{
			const auto f0t = Clock::now();
			feeder.apply(*eng, table, bpm, feed);
			if(diag) feedUs += std::chrono::duration<double, std::micro>(Clock::now() - f0t).count();
			const int f0 = sb * Engine::kBlock;
			auto& out = master.outs[sb];
			if(!eng->renderDry(out, master.jobs[sb]))
			{
				s.stats.faulted.store(1, std::memory_order_relaxed);
				logf("engine fault: %s", eng->fault().c_str());
				master.stop();
				return 6;	// the supervisor restarts the engine and replays the table
			}
			master.post();	// the master effects of this 32 frames run while the next 32 frames' voices render
			++posted;
			for(int t = 0; t < mm::kTracks; ++t)
			{
				int32_t* dst = s.track[slot][t] + f0;
				const uint32_t vol = eng->host().mixerInput(t).mix[1];
				uint32_t pk = 0;
				for(int f = 0; f < Engine::kBlock; ++f)
				{
					const int32_t x = md::engine::Mixer::solo(out.tracks[size_t(t)][size_t(f)], vol);
					dst[f] = x;
					pk = std::max(pk, uint32_t(x < 0 ? -x : x));
				}
				if(pk > s.peak[t].load(std::memory_order_relaxed)) s.peak[t].store(pk, std::memory_order_relaxed);
			}
		}
		mm::waitFor(master.done, posted);
		if(master.failed.load())
		{
			s.stats.faulted.store(1, std::memory_order_relaxed);
			logf("master effects fault");
			master.stop();
			return 6;
		}
		for(int sb = 0; sb < mm::kSub; ++sb)
			for(int f = 0; f < Engine::kBlock; ++f)
			{
				main[2 * (sb * Engine::kBlock + f)] = master.outs[sb].mix.main[size_t(f)][0];
				main[2 * (sb * Engine::kBlock + f) + 1] = master.outs[sb].mix.main[size_t(f)][1];
			}
		++next;
		s.produced.store(next, std::memory_order_release);

		const auto us = uint32_t(std::chrono::duration<double, std::micro>(Clock::now() - b0).count());
		s.stats.lastUs.store(us, std::memory_order_relaxed);
		if(us > s.stats.maxUs.load(std::memory_order_relaxed)) s.stats.maxUs.store(us, std::memory_order_relaxed);
		s.stats.blocks.fetch_add(1, std::memory_order_relaxed);
		s.stats.releases.store(eng->host().silenceReleases(), std::memory_order_relaxed);
		if(diag)
		{
			diagUs += us;
			diagMax = std::max(diagMax, us);
			if(++diagBlocks == 1034)	// ~3 s
			{
				auto& h = eng->host();
				const double k = 1.0 / diagBlocks;
				logf("diag: block avg %.0f max %u us | OS tick %.0f (machines %.0f, smoothing/LFO %.0f), voice DSP %.0f, track FX %.0f, mix+master %.0f, feeder %.0f | releases %u skips %u",
					diagUs * k, diagMax, h.tickUs * k, h.voiceLoopUs * k, h.osCallsUs * k, h.dspUs * k, eng->fxUs * k, eng->mixUs * k, feedUs * k,
					h.silenceReleases(), s.skips.load());
				diagUs = feedUs = 0; diagBlocks = diagMax = 0;
				h.tickUs = h.dspUs = h.voiceLoopUs = h.osCallsUs = 0; eng->fxUs = eng->mixUs = 0;
			}
		}
	}
	master.stop();
	logf("shutdown");
	return 0;
}
