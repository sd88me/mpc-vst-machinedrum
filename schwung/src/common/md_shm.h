// Shared memory between a Machinemodule instance (the plugin half, dsp.so, running in MoveOriginal on the audio callback) and its
// engine process (md-engine, spawned per instance). One segment per instance, created with memfd_create by the plugin's supervisor
// thread and handed to the child as fd 3. The design follows legsmechanical's Schwung port of Monomodule (schwung-monomodule).
//
// Timing: the callback owns `host_block` (the number of Move blocks it has played). The child keeps up to `depth` blocks rendered
// ahead of it: block b lives in slot b % kSlots and is published by storing `produced = b + 1` (release). The callback plays block
// `host_block` when `produced > host_block`, else silence and an underrun. A child that falls behind skips forward (the engine's
// own time stays continuous: the listener hears a gap, not a slowdown). Latency = depth x 128 frames (depth 1 = 2.9 ms).
//
// Commands: an SPSC ring written by the callback (triggers, kit values) and drained by the child before each block it renders.
// The child answers through mailboxes: a new machine's own starting values, the OS's machine table, stats.
#pragma once
#include <atomic>
#include <cstdint>

namespace mm::shm
{
	constexpr uint32_t kMagic = 0x4D444D31;	// "MDM1"
	constexpr uint32_t kVersion = 4;	// 2: SetMasterMode, 3: SetVoices, 4: tids / rt
	constexpr int kFrames = 128;	// one Move block
	constexpr int kSlots = 8;		// power of two, > kMaxDepth
	constexpr int kMaxDepth = 4;
	constexpr int kCmdSlots = 2048;	// power of two: a whole kit replay is ~600 commands
	constexpr int kTracks = 16;
	constexpr int kMachineIds = 192;

	enum class Op : uint8_t
	{
		None = 0,
		Trigger,		// track, b = velocity 1-127, c = 1: accented (the kit's accent amount)
		SetMachine,		// track, c = machine id, b = 1: the user picked it (it starts from its own values: see Defaults)
		SetRaw,			// track, b = 0-23 (HostModel raw order), c = 0-127
		SetLevel,		// track, c
		SetMute,		// track, c = 0/1
		SetLfo,			// track, b = 0-4, c
		SetMaster,		// b = 0-31, c
		SetMasterOn,	// c = 0/1
		SetAccent,		// c = the kit's accent amount 0-127
		SetBpm,			// c = bpm x 100
		SetTrackOuts,	// c = bit t: track t is rendered apart (split_voices) and leaves the dry main mix
		SetMasterMode,	// c = 0: the whole master section, 1: reverb and echo only (MasterFx::setReverbEchoOnly)
		SetVoices,		// c = the voice budget (HostModel::setMaxActiveVoices: ROM machines count 2), 0 = no limit
	};

	struct Cmd
	{
		uint8_t op, track, b, pad;
		int32_t c;
	};
	static_assert(sizeof(Cmd) == 8);

	enum class Phase : uint32_t { Spawning = 0, Loading, Ready, Failed };

	// A new machine's starting values (SYN1-8; CTR-8P: all 24), chosen in the engine as the OS does, for the plugin's table.
	struct Defaults
	{
		std::atomic<uint32_t> seq;	// child: bumped (release) after values/count/machine are written
		uint8_t machine, count, values[24];
	};

	// The OS's machine table, written once by the child at boot (before phase = Ready).
	struct Machine
	{
		uint8_t valid;
		char name[11];		// "TRX-BD" as the OS names it ("TRXBD" in the table: the UI splits it)
		char syn[8][6];		// the SYN labels, "" where unused
	};

	struct Stats
	{
		std::atomic<uint32_t> lastUs, maxUs;	// per 128-frame block; maxUs is reset by the plugin
		std::atomic<uint32_t> blocks, voices, releases, faulted;
	};

	struct Segment
	{
		// ---- header, written by the plugin before spawning
		uint32_t magic, version;
		std::atomic<uint32_t> depth;	// blocks rendered ahead, 1..kMaxDepth (live)

		// ---- clock: the callback increments host_block once per block and futex-wakes the child on it
		alignas(64) std::atomic<uint32_t> host_block;
		alignas(64) std::atomic<uint32_t> produced;		// child: blocks published (index of the next block + 1)
		alignas(64) std::atomic<uint32_t> underruns;	// callback
		std::atomic<uint32_t> skips;					// child: blocks skipped to catch up
		std::atomic<uint32_t> heartbeat;				// child: bumped every loop iteration
		std::atomic<uint32_t> shutdown;					// plugin -> child

		// ---- boot (child)
		std::atomic<uint32_t> phase;
		std::atomic<uint32_t> child_pid;
		std::atomic<uint32_t> romSamples;	// 1: the ROM machines have their samples
		// The engine's rendering threads (Linux thread ids), published before phase = Ready. The engine cannot make itself real-time
		// (a spawned child does not inherit MoveOriginal's CAP_SYS_NICE), so the plugin's supervisor does it for these threads.
		std::atomic<uint32_t> tids[4];
		std::atomic<uint32_t> rt;	// supervisor: 0 not yet, 1 SCHED_FIFO set, 2 refused
		char error[160];

		// ---- commands (callback -> child)
		alignas(64) std::atomic<uint32_t> cmd_write;
		alignas(64) std::atomic<uint32_t> cmd_read;
		Cmd cmds[kCmdSlots];

		// ---- child -> plugin
		Defaults defaults[kTracks];
		Machine machines[kMachineIds];
		Stats stats;
		std::atomic<uint32_t> peak[kTracks];	// each track's output peak (24-bit), max since the plugin last cleared it

		// ---- audio: 24-bit in int32. main: L/R interleaved (after the master effects); track: each track's mono signal after its
		// effects and VOL, no pan (Mixer::solo), for split_voices
		alignas(64) int32_t main[kSlots][kFrames * 2];
		int32_t track[kSlots][kTracks][kFrames];
	};

	// Callback side: never blocks. Returns false (the command is dropped) when the ring is full.
	inline bool pushCmd(Segment& s, const Cmd& c)
	{
		const uint32_t w = s.cmd_write.load(std::memory_order_relaxed);
		if(w - s.cmd_read.load(std::memory_order_acquire) >= uint32_t(kCmdSlots)) return false;
		s.cmds[w & (kCmdSlots - 1)] = c;
		s.cmd_write.store(w + 1, std::memory_order_release);
		return true;
	}
}
