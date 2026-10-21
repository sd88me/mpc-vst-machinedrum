// Machinemodule for Schwung (sound generator): the plugin half, loaded into MoveOriginal.
//
// Every entry point here runs on the SPI audio callback (Schwung docs/MODULES.md, "there is no control thread"), so nothing here
// allocates, locks, logs or does file IO after create_instance. The engine runs in a separate process (md-engine, src/child) that a
// per-instance supervisor thread spawns, watches and respawns; the callback copies audio from shared memory (src/common/md_shm.h),
// queues commands and keeps the kit table, which is the authority: a respawned engine is replayed from it. The hosting design
// follows legsmechanical's Schwung port of Monomodule; the pages follow DR32's (one template level per page, the editor follows
// the pad that was hit).
//
// Keys: per track "tNN_<key>" (the desktop plugin's names: machine, syn1..syn8, amd .. lfom, level, mute, lfo_track .. lfo_type);
// kit-wide mfx01..mfx32, master_on; settings depth, vel_mode, acc_thr, acc_amt, kit; editor ui_track, ui_machine (derived: the
// focused track's machine, the Synth pages' gate), ui_live_press; read-only load, peak, status.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define _Static_assert static_assert	// the host header is C11; the same check in C++
#include "host_api/plugin_api_v1.h"
#undef _Static_assert
#include "KitModel.h"
#include "md_shm.h"

extern char** environ;

namespace
{
	using namespace mm;
	namespace shm = mm::shm;

	const host_api_v1_t* g_host = nullptr;

	constexpr int kDefaultDepth = 1;	// 2.9 ms; LTNCY raises it (the user's choice, 2026-10-21)
	constexpr int kDefaultFifo = 10;
	constexpr int kNoteBase = 36;		// tracks 1-16 = notes 36-51 = Move's 16 drum pads
	constexpr int kLiveMatchBlocks = 20;	// a pad press and its note meet within 58 ms (DR32's measured window)
	constexpr int kHierCap = 60000;
	constexpr const char* kTrackNames[kTracks] = {"01-BD", "02-SD", "03-HT", "04-MT", "05-LT", "06-CP", "07-RS", "08-CB", "09-CH", "10-OH", "11-RC", "12-CC", "13-M1", "14-M2", "15-M3", "16-M4"};
	constexpr const char* kRawNames[kRaw] = {"SYN1", "SYN2", "SYN3", "SYN4", "SYN5", "SYN6", "SYN7", "SYN8", "AMD", "AMF", "EQF", "EQG", "FLTF", "FLTW", "FLTQ", "SRR",
		"DIST", "VOL", "PAN", "DEL", "REV", "LFOS", "LFOD", "LFOM"};
	constexpr bool isBipolar(int raw) { return raw == 11 || raw == 18; }	// EQG, PAN: shown -64..63 as on the unit

	// ---- kits: the factory kits imported from the user's flash image (factory/FACTORY.syx), read by the supervisor thread
	struct Catalog
	{
		std::vector<Kit> kits;
		uint64_t signature = 0;
	};

	struct Settings
	{
		int depth = kDefaultDepth;
		int velMode = 0;	// 0 MD: accent above the threshold; 1 VEL: velocity is the trigger velocity
		int accThr = 100;	// MD mode: a hit at or above this is accented
		int accAmt = 32;	// MD mode: how much softer (in velocity steps) an unaccented hit is than an accented one
		int kit = -1;		// the last kit loaded (0 = Init, 1.. = catalog kits), for the Kit page
		int voices = 16;	// VOICES: how many tracks may sound at once (the oldest is cut past it; a ROM machine counts 2); 16 = no limit
		int fxMode = 0;		// master effects, when on: 0 the whole section, 1 reverb and echo only (EQ and dynamix skipped: ~25% less CPU)
	};

	struct Instance
	{
		char moduleDir[512];
		pthread_t supervisor{};
		bool supervisorStarted = false;
		std::atomic<shm::Segment*> seg{nullptr};
		std::atomic<uint32_t> segGen{0};	// bumped by the supervisor after each (re)spawn
		uint32_t replayedGen = 0;			// callback: the last generation the table was replayed into
		std::atomic<uint32_t> stop{0};		// callback -> supervisor; also the supervisor's futex word
		std::atomic<uint32_t> respawns{0};
		std::atomic<Catalog*> catalog{nullptr};
		char error[200];

		KitValues table;					// the authority: the engine is fed from it (callback only)
		ControlRules rules;
		Settings set;
		uint32_t seenDefaults[kTracks] = {};
		uint32_t trackOuts = 0;

		// editor
		int focus = 0;						// the focused track (0-15)
		int64_t block = 0;					// callback blocks since create
		int lastHitTrack = -1;				// the last note's track, for a press arriving after it
		int64_t lastHitBlock = 0;
		bool pressArmed = false;			// a press arrived first: the next note within the window is a hand's
		int64_t pressBlock = 0;
		int64_t loadingUntilMs = 0;			// is_loading answers 1 until then: the host re-plans the pages on the falling edge
		uint64_t servedMachines = 0;		// what the served hierarchy was built from (machines in use, the lists): a hash
		float lastBpm = 0.f;
		uint32_t bpmTick = 0;
		char hier[kHierCap];
	};

	// ------------------------------------------------------------------------------------------------- helpers

	void futexWakeShared(std::atomic<uint32_t>* a) { syscall(SYS_futex, reinterpret_cast<uint32_t*>(a), FUTEX_WAKE, 1, nullptr, nullptr, 0); }
	void futexWakePrivate(std::atomic<uint32_t>* a) { syscall(SYS_futex, reinterpret_cast<uint32_t*>(a), FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, nullptr, nullptr, 0); }
	void futexWaitPrivateMs(std::atomic<uint32_t>* a, uint32_t expected, int ms)
	{
		timespec ts{ms / 1000, (ms % 1000) * 1000000L};
		syscall(SYS_futex, reinterpret_cast<uint32_t*>(a), FUTEX_WAIT | FUTEX_PRIVATE_FLAG, expected, &ts, nullptr, 0);
	}

	int64_t nowMs()
	{
		timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);	// vDSO: no syscall, safe on the audio thread
		return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
	}

	// The served pages change (a machine set, the machine list arriving): the grid re-reads them on an is_loading 1 -> 0 edge, which it
	// polls every ~133 ms, so "1" is held for 350 ms (as Monomodule's port does).
	void armReplan(Instance& in) { in.loadingUntilMs = nowMs() + 350; }

	void push(Instance& in, shm::Op op, int track = 0, int b = 0, int c = 0)
	{
		auto* s = in.seg.load(std::memory_order_acquire);
		if(!s) return;	// not booted yet: the table is replayed when it is
		shm::Cmd cmd{};
		cmd.op = uint8_t(op);
		cmd.track = uint8_t(track);
		cmd.b = uint8_t(b);
		cmd.c = c;
		shm::pushCmd(*s, cmd);
	}

	void pushTrack(Instance& in, int t, bool userMachine)
	{
		const auto& tr = in.table.track[size_t(t)];
		push(in, shm::Op::SetMachine, t, userMachine ? 1 : 0, tr.machine);
		for(int i = 0; i < kRaw; ++i) push(in, shm::Op::SetRaw, t, i, tr.raw[size_t(i)]);
		push(in, shm::Op::SetLevel, t, 0, tr.level);
		push(in, shm::Op::SetMute, t, 0, tr.mute ? 1 : 0);
		for(int i = 0; i < kNumLfo; ++i) push(in, shm::Op::SetLfo, t, i, tr.lfo[size_t(i)]);
	}

	void pushMaster(Instance& in)
	{
		for(int i = 0; i < kMasterBytes; ++i) push(in, shm::Op::SetMaster, 0, i, in.table.master[size_t(i)]);
		push(in, shm::Op::SetMasterOn, 0, 0, in.table.masterOn ? 1 : 0);
		push(in, shm::Op::SetMasterMode, 0, 0, in.set.fxMode);
		push(in, shm::Op::SetVoices, 0, 0, in.set.voices >= kTracks ? 0 : in.set.voices);
	}

	void replay(Instance& in)
	{
		for(int t = 0; t < kTracks; ++t) pushTrack(in, t, false);
		pushMaster(in);
		push(in, shm::Op::SetAccent, 0, 0, 0);
		push(in, shm::Op::SetTrackOuts, 0, 0, int(in.trackOuts));
		if(in.lastBpm > 0.f) push(in, shm::Op::SetBpm, 0, 0, int(std::lround(in.lastBpm * 100.f)));
	}

	// Sends what a table edit changed (the control machines move other values).
	void pushDiff(Instance& in, const KitValues& was)
	{
		for(int t = 0; t < kTracks; ++t)
			for(int i = 0; i < kRaw; ++i)
				if(in.table.track[size_t(t)].raw[size_t(i)] != was.track[size_t(t)].raw[size_t(i)]) push(in, shm::Op::SetRaw, t, i, in.table.track[size_t(t)].raw[size_t(i)]);
		for(int i = 0; i < kMasterBytes; ++i)
			if(in.table.master[size_t(i)] != was.master[size_t(i)]) push(in, shm::Op::SetMaster, 0, i, in.table.master[size_t(i)]);
	}

	// ---- machines: the OS's own table, published by the engine at boot

	const shm::Machine* machineOf(const Instance& in, int id)
	{
		auto* s = in.seg.load(std::memory_order_acquire);
		if(!s || s->phase.load(std::memory_order_acquire) != uint32_t(shm::Phase::Ready) || id < 0 || id >= shm::kMachineIds) return nullptr;
		return s->machines[id].valid ? &s->machines[id] : nullptr;
	}

	// Offered on Move: all but MID (no MIDI out here), RAM (no sampling), INP (a sound generator's slot has no audio input) and the
	// empty ROM slots above 32. The ROM machines only when the samples were imported.
	bool offered(const Instance& in, const shm::Machine& m)
	{
		if(!m.name[0]) return false;
		if(!std::strncmp(m.name, "MID", 3) || !std::strncmp(m.name, "RAM", 3) || !std::strncmp(m.name, "INP", 3)) return false;
		if(!std::strncmp(m.name, "ROM", 3))
		{
			auto* s = in.seg.load(std::memory_order_acquire);
			if(!s || !s->romSamples.load()) return false;
			if(std::atoi(m.name + 3) > 32) return false;
		}
		return true;
	}

	// "TRXBD" -> "TRX-BD": the group, then the rest, as the desktop picker splits them
	int displayName(const shm::Machine& m, char* buf, int len)
	{
		return std::snprintf(buf, size_t(len), "%.3s-%s", m.name, std::strlen(m.name) > 3 ? m.name + 3 : "");
	}

	int machineByName(const Instance& in, const char* val)
	{
		char n[24];
		for(int id = 0; id < shm::kMachineIds; ++id)
			if(const auto* m = machineOf(in, id))
				if(offered(in, *m) && (displayName(*m, n, sizeof n), !std::strcmp(n, val))) return id;
		return -1;
	}

	uint64_t machineSet(const Instance& in)
	{
		uint64_t h = 1469598103934665603ull;
		bool used[shm::kMachineIds] = {};
		for(const auto& tr : in.table.track) used[std::clamp(tr.machine, 0, shm::kMachineIds - 1)] = true;
		for(int id = 0; id < shm::kMachineIds; ++id) if(used[id]) { h ^= uint64_t(id + 1); h *= 1099511628211ull; }
		auto* s = in.seg.load(std::memory_order_acquire);
		h ^= s ? s->phase.load() * 7 + s->romSamples.load() : 0;	// the list arriving changes the pages too
		h ^= reinterpret_cast<uintptr_t>(in.catalog.load(std::memory_order_acquire)) * 31;	// so does a new kit list
		return h;
	}

	void noteMachineChange(Instance& in)
	{
		if(machineSet(in) != in.servedMachines) armReplan(in);
	}

	// ------------------------------------------------------------------------------------------------- the pages (ui_hierarchy)

	struct Out
	{
		char* buf;
		int len, n = 0;
		bool ok = true;
		void put(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
		{
			if(!ok) return;
			va_list ap;
			va_start(ap, fmt);
			const int w = std::vsnprintf(buf + n, size_t(len - n), fmt, ap);
			va_end(ap);
			if(w < 0 || n + w >= len) { ok = false; return; }
			n += w;
		}
	};

	void putCopyKeys(Out& o)
	{
		o.put("\"child_copy_keys\":[\"machine\"");
		for(int i = 0; i < kRaw; ++i) o.put(",\"%s\"", kRawKeys[i]);
		o.put(",\"level\"");
		for(int i = 0; i < kNumLfo; ++i) o.put(",\"%s\"", kLfoKeys[i]);
		o.put("]");
	}

	// A per-track page: one template level over the 16 tracks, focus owned by ui_track, presses vouched through ui_live_press.
	void openTrackLevel(Out& o, const char* id, const char* name)
	{
		o.put(",\"%s\":{\"name\":\"%s\",\"child_prefix\":\"t\",\"child_count\":16,\"child_index_base\":1,\"child_index_digits\":2,"
			"\"child_label\":\"Track\",\"child_index_param\":\"ui_track\",\"child_press_param\":\"ui_live_press\",\"child_note_base\":%d,",
			id, name, kNoteBase);
		putCopyKeys(o);
	}

	void putInt(Out& o, const char* key, const char* name, int lo, int hi, int def)
	{
		o.put("{\"key\":\"%s\",\"name\":\"%s\",\"short_name\":\"%s\",\"type\":\"int\",\"min\":%d,\"max\":%d,\"default\":%d}", key, name, name, lo, hi, def);
	}

	void putEnum(Out& o, const char* key, const char* name, const char* const* opts, int count, int def)
	{
		o.put("{\"key\":\"%s\",\"name\":\"%s\",\"short_name\":\"%s\",\"type\":\"enum\",\"options\":[", key, name, name);
		for(int i = 0; i < count; ++i) o.put("%s\"%s\"", i ? "," : "", opts[i]);
		o.put("],\"default\":%d}", def);
	}

	void putRawParam(Out& o, int i, const char* label)
	{
		if(isBipolar(i)) putInt(o, kRawKeys[i], label, -64, 63, kRawDefault[i] - 64);
		else putInt(o, kRawKeys[i], label, 0, 127, kRawDefault[i]);
	}

	void putKnobs(Out& o, const char* const* keys, int n)
	{
		o.put(",\"knobs\":[");
		for(int i = 0; i < n; ++i) o.put("%s\"%s\"", i ? "," : "", keys[i]);
		o.put("]}");
	}

	int buildHierarchy(Instance& in, char* buf, int len)
	{
		Out o{buf, len};
		bool used[shm::kMachineIds] = {};
		for(const auto& tr : in.table.track) used[std::clamp(tr.machine, 0, shm::kMachineIds - 1)] = true;
		const bool ready = machineOf(in, 0) != nullptr;

		o.put("{\"pad_layout\":\"drums\",\"levels\":{\"root\":{\"name\":\"Machinemodule\",\"params\":[");
		o.put("{\"level\":\"kits\",\"label\":\"Kit\"},{\"level\":\"track\",\"label\":\"Track\"}");
		for(int id = 0; id < shm::kMachineIds; ++id)
			if(used[id] && ready) o.put(",{\"level\":\"syn_%d\",\"label\":\"Synth\"}", id);
		if(used[kCtr8P]) o.put(",{\"level\":\"ctr8_a\",\"label\":\"Targets 1-4\"},{\"level\":\"ctr8_b\",\"label\":\"Targets 5-8\"}");
		o.put(",{\"level\":\"amp\",\"label\":\"Amp/Filter\"},{\"level\":\"mix\",\"label\":\"Mix\"},{\"level\":\"lfo\",\"label\":\"LFO\"}"
			",{\"level\":\"fx_echo\",\"label\":\"Echo\"},{\"level\":\"fx_rev\",\"label\":\"Reverb\"},{\"level\":\"fx_eq\",\"label\":\"EQ\"}"
			",{\"level\":\"fx_dyn\",\"label\":\"Dynamix\"},{\"level\":\"master\",\"label\":\"Master\"}],\"knobs\":[]}");

		// Kit
		{
			const Catalog* c = in.catalog.load(std::memory_order_acquire);
			o.put(",\"kits\":{\"name\":\"Kit\",\"params\":[{\"key\":\"kit\",\"name\":\"Kit\",\"short_name\":\"KIT\",\"type\":\"enum\",\"options\":[\"Init\"");
			if(c)
				for(const auto& k : c->kits)
				{
					char nm[24];
					std::snprintf(nm, sizeof nm, "%s", k.name);
					for(char* p = nm + std::strlen(nm); p > nm && p[-1] == ' '; --p) p[-1] = 0;
					for(char* p = nm; *p; ++p) if(*p == '"' || *p == '\\') *p = ' ';
					o.put(",\"%s\"", nm);
				}
			o.put("],\"default\":0}],\"knobs\":[\"kit\"]}");
		}

		// Track: TRK suppresses the host's "Selected Pad" page (DR32's PAD knob does the same)
		openTrackLevel(o, "track", "Track");
		o.put(",\"params\":[");
		putInt(o, "ui_track", "TRK", 1, 16, 1);
		o.put(",{\"key\":\"machine\",\"name\":\"Machine\",\"short_name\":\"MACH\",\"type\":\"enum\",\"options_as_string\":true,\"options\":[");
		{
			bool first = true;
			char n[24];
			if(ready)
				for(int id = 0; id < shm::kMachineIds; ++id)
					if(const auto* m = machineOf(in, id); m && offered(in, *m)) { displayName(*m, n, sizeof n); o.put("%s\"%s\"", first ? "" : ",", n); first = false; }
			if(first) o.put("\"GND-EM\"");
		}
		o.put("]},");
		putInt(o, "level", "LEVEL", 0, 127, 100);
		o.put(",");
		{
			static const char* const kOffOn[2] = {"Off", "On"};
			putEnum(o, "mute", "MUTE", kOffOn, 2, 0);
		}
		o.put("]");
		{
			static const char* const k[] = {"ui_track", "machine", "level", "mute"};
			putKnobs(o, k, 4);
		}

		// Synth: one page per machine in use, gated on the focused track's machine, with the OS's own SYN labels
		for(int id = 0; id < shm::kMachineIds && ready; ++id)
		{
			if(!used[id]) continue;
			const auto* m = machineOf(in, id);
			char lvl[16];
			std::snprintf(lvl, sizeof lvl, "syn_%d", id);
			openTrackLevel(o, lvl, "Synth");
			o.put(",\"visible_if\":{\"param\":\"ui_machine\",\"equals\":%d},\"params\":[", id);
			const char* keys[8];
			int nk = 0;
			for(int k = 0; k < 8; ++k)
			{
				char lab[8];
				if(m && m->syn[k][0]) std::snprintf(lab, sizeof lab, "%s", m->syn[k]);
				else std::snprintf(lab, sizeof lab, "SYN%d", k + 1);
				for(char* p = lab; *p; ++p) if(*p == '"' || *p == '\\') *p = ' ';
				if(nk) o.put(",");
				putRawParam(o, k, lab);
				keys[nk++] = kRawKeys[k];
			}
			o.put("]");
			putKnobs(o, keys, nk);
		}

		// CTR-8P: its parameters 9-24 are eight (track, parameter) targets
		if(used[kCtr8P])
			for(int page = 0; page < 2; ++page)
			{
				openTrackLevel(o, page ? "ctr8_b" : "ctr8_a", page ? "Targets 5-8" : "Targets 1-4");
				o.put(",\"visible_if\":{\"param\":\"ui_machine\",\"equals\":%d},\"params\":[", kCtr8P);
				const char* keys[8];
				for(int j = 0; j < 8; ++j)
				{
					const int raw = 8 + page * 8 + j;
					char nm[8];
					std::snprintf(nm, sizeof nm, "%s%d", (j & 1) ? "PRM" : "TRK", page * 4 + j / 2 + 1);
					if(j) o.put(",");
					if(j & 1) putEnum(o, kRawKeys[raw], nm, kRawNames, kRaw, 0);
					else putEnum(o, kRawKeys[raw], nm, kTrackNames, kTracks, 0);
					keys[j] = kRawKeys[raw];
				}
				o.put("]");
				putKnobs(o, keys, 8);
			}

		// Amp/Filter, Mix (hidden on CTR-8P, whose values 9-24 are its targets)
		for(int page = 0; page < 2; ++page)
		{
			openTrackLevel(o, page ? "mix" : "amp", page ? "Mix" : "Amp/Filter");
			o.put(",\"visible_if\":{\"param\":\"ui_machine\",\"not_equals\":%d},\"params\":[", kCtr8P);
			const int first = page ? 16 : 8, count = page ? 5 : 8;
			const char* keys[8];
			for(int j = 0; j < count; ++j) { if(j) o.put(","); putRawParam(o, first + j, kRawNames[first + j]); keys[j] = kRawKeys[first + j]; }
			o.put("]");
			putKnobs(o, keys, count);
		}

		// LFO
		{
			openTrackLevel(o, "lfo", "LFO");
			o.put(",\"visible_if\":{\"param\":\"ui_machine\",\"not_equals\":%d},\"params\":[", kCtr8P);
			static const char* const kUpdate[3] = {"FREE", "TRIG", "HOLD"};
			putEnum(o, "lfo_track", "TRACK", kTrackNames, kTracks, 0); o.put(",");
			putEnum(o, "lfo_param", "PARAM", kRawNames, kRaw, 0); o.put(",");
			putInt(o, "lfo_shp1", "SHP1", 0, 5, 0); o.put(",");
			putInt(o, "lfo_shp2", "SHP2", 0, 5, 0); o.put(",");
			putEnum(o, "lfo_type", "UPDTE", kUpdate, 3, 0); o.put(",");
			putInt(o, "lfos", "SPEED", 0, 127, 0); o.put(",");
			putInt(o, "lfod", "DEPTH", 0, 127, 0); o.put(",");
			putInt(o, "lfom", "SHMIX", 0, 127, 0);
			o.put("]");
			static const char* const k[] = {"lfo_track", "lfo_param", "lfo_shp1", "lfo_shp2", "lfo_type", "lfos", "lfod", "lfom"};
			putKnobs(o, k, 8);
		}

		// The master effects: kit-wide, the kit's 32 bytes (reverb 0-7, echo 8-15, EQ 16-23, dynamix 24-31)
		{
			static const char* const kIds[4] = {"fx_rev", "fx_echo", "fx_eq", "fx_dyn"};
			static const char* const kNames[4] = {"Reverb", "Echo", "EQ", "Dynamix"};
			static const char* const kShort[4] = {"REV", "ECH", "EQ", "DYN"};
			for(int b = 0; b < 4; ++b)
			{
				o.put(",\"%s\":{\"name\":\"%s\",\"params\":[", kIds[b], kNames[b]);
				char keys[8][8];
				const char* kp[8];
				for(int j = 0; j < 8; ++j)
				{
					char nm[8];
					std::snprintf(keys[j], sizeof keys[j], "mfx%02d", b * 8 + j + 1);
					std::snprintf(nm, sizeof nm, "%s%d", kShort[b], j + 1);
					if(j) o.put(",");
					putInt(o, keys[j], nm, 0, 127, kMasterDefault[b * 8 + j]);
					kp[j] = keys[j];
				}
				o.put("]");
				putKnobs(o, kp, 8);
			}
		}

		// Master: the module's own settings
		{
			static const char* const kFx[3] = {"Off", "Full", "Rev+Echo"};
			static const char* const kVel[2] = {"MD", "VEL"};
			o.put(",\"master\":{\"name\":\"Master\",\"params\":[");
			putEnum(o, "master_on", "FX", kFx, 3, 1); o.put(",");
			putInt(o, "depth", "LTNCY", 1, shm::kMaxDepth, kDefaultDepth); o.put(",");
			putEnum(o, "vel_mode", "VELMD", kVel, 2, 0); o.put(",");
			putInt(o, "acc_thr", "ACTHR", 1, 127, 100); o.put(",");
			putInt(o, "acc_amt", "ACAMT", 0, 127, 32); o.put(",");
			putInt(o, "voices", "VOICS", 4, 16, 16); o.put(",");
			o.put("{\"key\":\"load\",\"name\":\"Engine Load\",\"short_name\":\"LOAD\",\"type\":\"int\",\"min\":0,\"max\":200,\"unit\":\"%%\",\"access\":\"read\",\"live\":true},");
			o.put("{\"key\":\"peak\",\"name\":\"Peak Load\",\"short_name\":\"PEAK\",\"type\":\"int\",\"min\":0,\"max\":200,\"unit\":\"%%\",\"access\":\"read\",\"live\":true}");
			o.put("]");
			static const char* const k[] = {"master_on", "depth", "vel_mode", "acc_thr", "acc_amt", "voices", "load", "peak"};
			putKnobs(o, k, 8);
		}
		o.put("}}");
		if(!o.ok) return -1;
		in.servedMachines = machineSet(in);
		return o.n;
	}

	int buildChainParams(char* buf, int len)
	{
		Out o{buf, len};
		o.put("[{\"key\":\"ui_machine\",\"name\":\"Machine\",\"type\":\"int\",\"min\":0,\"max\":%d,\"default\":0}", shm::kMachineIds - 1);
		for(int i = 0; i < kMasterBytes; ++i) o.put(",{\"key\":\"mfx%02d\",\"name\":\"FX %d\",\"type\":\"int\",\"min\":0,\"max\":127,\"default\":%d}", i + 1, i + 1, kMasterDefault[i]);
		o.put("]");
		return o.ok ? o.n : -1;
	}

	// ------------------------------------------------------------------------------------------------- kits

	uint64_t fileSignature(const char* path)
	{
		struct stat st{};
		if(stat(path, &st) != 0) return 0;
		return uint64_t(st.st_size) * 1000003u ^ uint64_t(st.st_mtime);
	}

	std::vector<uint8_t> readFile(const std::string& path)
	{
		std::vector<uint8_t> data;
		std::FILE* f = std::fopen(path.c_str(), "rb");
		if(!f) return data;
		uint8_t b[65536];
		size_t n;
		while((n = std::fread(b, 1, sizeof b, f)) > 0 && data.size() < (8u << 20)) data.insert(data.end(), b, b + n);
		std::fclose(f);
		return data;
	}

	// The kit files: the factory kits (factory/FACTORY.syx, from the flash import), then the user's own kit dumps (kits/*.syx, by
	// name). Their signature: names, sizes, times; a change rebuilds the list.
	uint64_t kitFiles(const Instance& in, std::vector<std::string>* files)
	{
		uint64_t h = 1469598103934665603ull;
		auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
		const std::string factory = std::string(in.moduleDir) + "/factory/FACTORY.syx";
		if(const uint64_t s = fileSignature(factory.c_str())) { mix(s); if(files) files->push_back(factory); }
		const std::string dir = std::string(in.moduleDir) + "/kits";
		std::vector<std::string> user;
		if(DIR* d = opendir(dir.c_str()))
		{
			while(dirent* e = readdir(d))
			{
				const size_t n = std::strlen(e->d_name);
				if(n > 4 && e->d_name[0] != '.' && !strcasecmp(e->d_name + n - 4, ".syx")) user.push_back(dir + "/" + e->d_name);
			}
			closedir(d);
		}
		std::sort(user.begin(), user.end());
		for(const auto& u : user) { for(char ch : u) mix(uint8_t(ch)); mix(fileSignature(u.c_str())); if(files) files->push_back(u); }
		return h;
	}

	Catalog* buildCatalog(const std::vector<std::string>& files, uint64_t sig)
	{
		auto* c = new Catalog();
		for(const auto& f : files)
			for(auto& k : parseKits(readFile(f)))
				if(c->kits.size() < 120) c->kits.push_back(k);	// an enum knob holds 128 options
		c->signature = sig;
		return c;
	}

	void loadKit(Instance& in, int index)
	{
		const Catalog* c = in.catalog.load(std::memory_order_acquire);
		if(index == 0) in.table = KitValues::defaults();
		else if(c && index >= 1 && index <= int(c->kits.size())) in.table.load(c->kits[size_t(index - 1)]);
		else return;
		in.set.kit = index;
		in.rules.resync(in.table);
		for(int t = 0; t < kTracks; ++t) pushTrack(in, t, false);	// a kit brings its own values with its machines
		pushMaster(in);
		noteMachineChange(in);
	}

	// ------------------------------------------------------------------------------------------------- state

	// "state": the whole kit table and the settings, self-contained: {"md":"1;<16 x 32 track values>;<32 master>;<settings>"}
	int writeState(const Instance& in, char* buf, int len)
	{
		Out o{buf, len};
		o.put("{\"md\":\"1");
		for(const auto& tr : in.table.track)
		{
			o.put(";%d", tr.machine);
			for(int v : tr.raw) o.put(",%d", v);
			o.put(",%d,%d", tr.level, tr.mute ? 1 : 0);
			for(int v : tr.lfo) o.put(",%d", v);
		}
		o.put(";");
		for(int i = 0; i < kMasterBytes; ++i) o.put("%s%d", i ? "," : "", in.table.master[size_t(i)]);
		o.put(";%d,%d,%d,%d,%d,%d,%d,%d,%d\"}", in.table.masterOn ? 1 : 0, in.set.depth, in.set.velMode, in.set.accThr, in.set.accAmt, in.set.kit, in.focus + 1, in.set.fxMode, in.set.voices);
		return o.ok ? o.n : -1;
	}

	bool readInts(const char*& p, int* out, int count)
	{
		for(int i = 0; i < count; ++i)
		{
			char* end = nullptr;
			const long x = std::strtol(p, &end, 10);
			if(end == p) return false;
			out[i] = int(x);
			p = end;
			if(*p == ',' || *p == ';') ++p;
		}
		return true;
	}

	void readState(Instance& in, const char* val)
	{
		const char* p = std::strstr(val, "\"md\":\"1;");
		if(!p) return;
		p += 8;
		KitValues v = in.table;
		for(auto& tr : v.track)
		{
			int x[32];
			if(!readInts(p, x, 32)) return;	// malformed: keep the current table
			tr.machine = std::clamp(x[0], 0, shm::kMachineIds - 1);
			for(int i = 0; i < kRaw; ++i) tr.raw[size_t(i)] = std::clamp(x[1 + i], 0, 127);
			tr.level = std::clamp(x[25], 0, 127);
			tr.mute = x[26] != 0;
			for(int i = 0; i < kNumLfo; ++i) tr.lfo[size_t(i)] = std::clamp(x[27 + i], 0, kLfoMax[i]);
		}
		int m[kMasterBytes], s[9] = {};
		if(!readInts(p, m, kMasterBytes) || !readInts(p, s, 8)) return;
		if(!readInts(p, s + 8, 1)) s[8] = 16;	// a state from before VOICES
		for(int i = 0; i < kMasterBytes; ++i) v.master[size_t(i)] = std::clamp(m[i], 0, 127);
		v.masterOn = s[0] != 0;
		in.table = v;
		in.set.depth = std::clamp(s[1], 1, shm::kMaxDepth);
		in.set.velMode = s[2] ? 1 : 0;
		in.set.accThr = std::clamp(s[3], 1, 127);
		in.set.accAmt = std::clamp(s[4], 0, 127);
		in.set.kit = s[5];
		in.focus = std::clamp(s[6] - 1, 0, kTracks - 1);
		in.set.fxMode = s[7] == 1 ? 1 : 0;
		in.set.voices = std::clamp(s[8], 4, kTracks);
		if(auto* sg = in.seg.load()) sg->depth.store(uint32_t(in.set.depth));
		in.rules.resync(in.table);
		replay(in);
		noteMachineChange(in);
	}

	// ------------------------------------------------------------------------------------------------- supervisor

	bool findFile(const char* dir, const char* ext, char* out, size_t len)
	{
		DIR* d = opendir(dir);
		if(!d) return false;
		bool found = false;
		while(dirent* e = readdir(d))
		{
			const size_t n = std::strlen(e->d_name);
			if(n > 4 && !strcasecmp(e->d_name + n - 4, ext) && e->d_name[0] != '.')
			{
				std::snprintf(out, len, "%s/%s", dir, e->d_name);
				struct stat st{};
				if(stat(out, &st) == 0 && st.st_size > 0) { found = true; break; }
			}
		}
		closedir(d);
		return found;
	}

	bool sleepOrStop(Instance& in, int ms);

	// The factory kits and ROM samples from the user's flash image (the "flash" upload): bin/md-import boots the emulated Machinedrum
	// once (about 6 minutes on a Move) and writes factory/FACTORY.syx and factory/ROM_SAMPLES.bin. Run before the engine starts (both at
	// once could run the Move out of memory), and only for an image not imported yet (factory/.flash-signature). Returns false when
	// destroy asked the supervisor to stop meanwhile.
	bool importFlash(Instance& in, const char* os)
	{
		char flashDir[600], flash[700], factory[600], sigPath[700], exe[600], log[700];
		std::snprintf(flashDir, sizeof flashDir, "%s/flash", in.moduleDir);
		if(!findFile(flashDir, ".bin", flash, sizeof flash)) return true;
		std::snprintf(factory, sizeof factory, "%s/factory", in.moduleDir);
		std::snprintf(sigPath, sizeof sigPath, "%s/.flash-signature", factory);
		char want[64];
		std::snprintf(want, sizeof want, "%llu", static_cast<unsigned long long>(fileSignature(flash)));
		if(std::FILE* f = std::fopen(sigPath, "r"))
		{
			char have[64] = {};
			const bool same = std::fgets(have, sizeof have, f) && !std::strcmp(have, want);
			std::fclose(f);
			if(same) return true;
		}
		mkdir(factory, 0755);
		std::snprintf(exe, sizeof exe, "%s/bin/md-import", in.moduleDir);
		std::snprintf(log, sizeof log, "%s/import.log", factory);
		std::snprintf(in.error, sizeof in.error, "Importing your flash image (factory kits, ROM samples): about 6 minutes, first time only");
		posix_spawn_file_actions_t fa;
		posix_spawn_file_actions_init(&fa);
		posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
		posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);	// md-import keeps its own log (factory/import.log)
		char* argv[] = {exe, flash, const_cast<char*>(os), factory, nullptr};
		pid_t pid = -1;
		const int rc = posix_spawn(&pid, exe, &fa, nullptr, argv, environ);
		posix_spawn_file_actions_destroy(&fa);
		if(rc != 0) { std::snprintf(in.error, sizeof in.error, "cannot start the flash import: %s", std::strerror(rc)); return true; }
		int status = 0;
		while(waitpid(pid, &status, WNOHANG) != pid)
			if(sleepOrStop(in, 1000)) { kill(pid, SIGKILL); waitpid(pid, &status, 0); return false; }
		if(WIFEXITED(status) && WEXITSTATUS(status) == 0)
		{
			if(std::FILE* f = std::fopen(sigPath, "w")) { std::fputs(want, f); std::fclose(f); }
			in.error[0] = 0;
		}
		else std::snprintf(in.error, sizeof in.error, "The flash image import failed: see factory/import.log (is it a Machinedrum UW flash image?)");
		return true;
	}

	bool spawnChild(Instance& in, int fd, const char* os, const char* rom, pid_t& pid)
	{
		char exe[600], log[600], prio[8], ppid[16];
		std::snprintf(exe, sizeof exe, "%s/md-engine", in.moduleDir);
		std::snprintf(log, sizeof log, "%s/md-engine.log", in.moduleDir);
		std::snprintf(prio, sizeof prio, "%d", kDefaultFifo);
		std::snprintf(ppid, sizeof ppid, "%d", int(getpid()));	// as this process sees itself (Move's host runs in a pid namespace)
		posix_spawn_file_actions_t fa;
		posix_spawn_file_actions_init(&fa);
		posix_spawn_file_actions_adddup2(&fa, fd, 3);
		posix_spawn_file_actions_addopen(&fa, 1, log, O_WRONLY | O_CREAT | O_APPEND, 0644);	// the emulator prints; cleared per instance (supervise)
		posix_spawn_file_actions_adddup2(&fa, 1, 2);
		char* argv[] = {exe, const_cast<char*>(os), const_cast<char*>(rom), log, ppid, prio, nullptr};
		const int rc = posix_spawn(&pid, exe, &fa, nullptr, argv, environ);
		posix_spawn_file_actions_destroy(&fa);
		if(rc != 0) { std::snprintf(in.error, sizeof in.error, "cannot start %s: %s", exe, std::strerror(rc)); return false; }
		return true;
	}

	void resetSegment(shm::Segment& s, int depth)
	{
		// Only what the child owns or reads at boot. host_block, cmd_write and underruns belong to the callback and keep running.
		s.produced.store(s.host_block.load());
		s.phase.store(uint32_t(shm::Phase::Spawning));
		s.child_pid.store(0);
		s.shutdown.store(0);
		s.error[0] = 0;
		s.depth.store(uint32_t(depth));
		s.cmd_read.store(s.cmd_write.load());	// a respawned engine starts from the table replay, not stale commands
		for(auto& t : s.tids) t.store(0);
		s.rt.store(0);
	}

	// destroy_instance returns without waiting, so the supervisor frees the instance itself once destroy has handed it over.
	void freeWhenStopped(Instance& in)
	{
		while(!in.stop.load(std::memory_order_acquire)) futexWaitPrivateMs(&in.stop, 0, 1000);
		delete in.catalog.exchange(nullptr);
		in.~Instance();
		std::free(&in);
	}

	bool sleepOrStop(Instance& in, int ms)
	{
		if(!in.stop.load(std::memory_order_acquire)) futexWaitPrivateMs(&in.stop, 0, ms);
		return in.stop.load(std::memory_order_acquire) != 0;
	}

	void describeExit(Instance& in, int status)
	{
		if(WIFSIGNALED(status)) std::snprintf(in.error, sizeof in.error, "engine process crashed (signal %d)", WTERMSIG(status));
		else if(WIFEXITED(status) && WEXITSTATUS(status) == 6) std::snprintf(in.error, sizeof in.error, "engine fault (restarted)");
		else if(WIFEXITED(status)) std::snprintf(in.error, sizeof in.error, "engine process exited (%d)", WEXITSTATUS(status));
	}

	void* supervise(void* arg)
	{
		auto& in = *static_cast<Instance*>(arg);
		// Threads inherit the callback's SCHED_FIFO 70: demote first, and keep off Move's audio core.
		sched_param sp{};
		sp.sched_priority = 0;
		sched_setscheduler(0, SCHED_OTHER, &sp);
		cpu_set_t cpus;
		CPU_ZERO(&cpus);
		CPU_SET(0, &cpus); CPU_SET(1, &cpus); CPU_SET(2, &cpus);
		sched_setaffinity(0, sizeof cpus, &cpus);

		{
			char log[600];
			std::snprintf(log, sizeof log, "%s/md-engine.log", in.moduleDir);
			if(const int lf = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644); lf >= 0) close(lf);	// this instance's engines append
		}
		char dir[600], os[700], rom[700];
		std::snprintf(dir, sizeof dir, "%s/os", in.moduleDir);
		while(!findFile(dir, ".syx", os, sizeof os))	// the user may upload it after adding the module
		{
			std::snprintf(in.error, sizeof in.error, "Upload the Machinedrum OS 1.63 file (.syx) on the module's page");
			if(sleepOrStop(in, 2000)) { freeWhenStopped(in); return nullptr; }
		}
		in.error[0] = 0;
		if(!importFlash(in, os)) { freeWhenStopped(in); return nullptr; }
		std::snprintf(rom, sizeof rom, "%s/factory/ROM_SAMPLES.bin", in.moduleDir);
		if(fileSignature(rom) == 0) std::snprintf(rom, sizeof rom, "-");

		const int fd = int(syscall(SYS_memfd_create, "md-shm", 1u /* MFD_CLOEXEC: other children must not inherit it */));
		if(fd < 0 || ftruncate(fd, sizeof(shm::Segment)) != 0)
		{
			std::snprintf(in.error, sizeof in.error, "shared memory: %s", std::strerror(errno));
			if(fd >= 0) close(fd);
			freeWhenStopped(in);
			return nullptr;
		}
		auto* seg = static_cast<shm::Segment*>(mmap(nullptr, sizeof(shm::Segment), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
		if(seg == MAP_FAILED)
		{
			std::snprintf(in.error, sizeof in.error, "shared memory map: %s", std::strerror(errno));
			close(fd);
			freeWhenStopped(in);
			return nullptr;
		}
		new(seg) shm::Segment();
		seg->magic = shm::kMagic;
		seg->version = shm::kVersion;

		Catalog* retired = nullptr;
		int retiredPolls = 0, kitPolls = 0;
		auto refreshKits = [&]
		{
			if(retired && ++retiredPolls >= 10) { delete retired; retired = nullptr; }	// ~1 s after it was replaced
			if(kitPolls++ % 30 != 0) return;	// every ~3 s
			std::vector<std::string> files;
			const uint64_t sig = kitFiles(in, &files);
			Catalog* cur = in.catalog.load();
			if(cur ? cur->signature == sig : files.empty()) return;
			if(retired) { delete retired; retired = nullptr; }
			in.catalog.store(buildCatalog(files, sig), std::memory_order_release);
			retired = cur;
			retiredPolls = 0;
		};

		constexpr int kMaxQuickFailures = 5;
		constexpr int kBootTimeoutPolls = 600;	// 60 s to reach Ready
		pid_t pid = -1;
		uint32_t lastHeartbeat = 0, lastHostBlock = 0;
		int stalledPolls = 0, bootPolls = 0, failures = 0;
		bool published = false, gaveUp = false;
		while(!in.stop.load(std::memory_order_acquire))
		{
			if(pid <= 0)
			{
				if(gaveUp || failures >= kMaxQuickFailures) { gaveUp = true; if(sleepOrStop(in, 1000)) break; continue; }
				if(failures > 1 && sleepOrStop(in, 1000 * (failures - 1))) break;	// first restart at once, then back off
				resetSegment(*seg, in.set.depth);
				if(!spawnChild(in, fd, os, rom, pid)) { pid = -1; ++failures; continue; }
				if(!published) { in.seg.store(seg, std::memory_order_release); published = true; }
				in.segGen.fetch_add(1, std::memory_order_release);	// the callback replays the table
				stalledPolls = 0;
				bootPolls = 0;
			}
			if(sleepOrStop(in, 100)) break;
			refreshKits();

			int status = 0;
			if(waitpid(pid, &status, WNOHANG) == pid)
			{
				const bool bootFailed = seg->phase.load() == uint32_t(shm::Phase::Failed);
				seg->phase.store(uint32_t(shm::Phase::Spawning), std::memory_order_release);	// callback: silence, not underruns
				if(bootFailed) { std::snprintf(in.error, sizeof in.error, "%s", seg->error); gaveUp = true; }	// a bad OS file will not fix itself
				else { describeExit(in, status); ++failures; }
				pid = -1;
				in.respawns.fetch_add(1);
				continue;
			}
			const bool ready = seg->phase.load() == uint32_t(shm::Phase::Ready);
			const uint32_t hb = seg->heartbeat.load(), hostBlock = seg->host_block.load();
			bool hung = false;
			if(!ready)
			{
				hung = ++bootPolls >= kBootTimeoutPolls;
				if(hung) std::snprintf(in.error, sizeof in.error, "engine did not start within 60 s");
			}
			else if(hb == lastHeartbeat && hostBlock != lastHostBlock)
			{
				hung = ++stalledPolls >= 10;	// ~1 s: the callback advances, the child's loop does not
				if(hung) std::snprintf(in.error, sizeof in.error, "engine stopped responding (restarted)");
			}
			else
			{
				stalledPolls = 0;
				if(failures && hostBlock - lastHostBlock > 0) failures = 0;
			}
			if(hung)
			{
				seg->phase.store(uint32_t(shm::Phase::Spawning), std::memory_order_release);
				kill(pid, SIGKILL);
				waitpid(pid, &status, 0);
				pid = -1;
				++failures;
				in.respawns.fetch_add(1);
				continue;
			}
			if(ready && !seg->rt.load())	// the engine's threads to SCHED_FIFO, below Move's own audio (35, 70): this process has CAP_SYS_NICE
			{
				bool ok = true;
				for(auto& t : seg->tids)
					if(const uint32_t tid = t.load())
					{
						sched_param rp{};
						rp.sched_priority = kDefaultFifo;
						ok &= sched_setscheduler(pid_t(tid), SCHED_FIFO, &rp) == 0;
					}
				seg->rt.store(ok ? 1 : 2);
			}
			if(ready && !failures && !std::strncmp(in.error, "engine", 6)) in.error[0] = 0;
			lastHeartbeat = hb;
			lastHostBlock = hostBlock;
		}

		if(pid > 0)
		{
			seg->shutdown.store(1, std::memory_order_release);
			futexWakeShared(&seg->host_block);
			int status = 0;
			for(int i = 0; i < 20 && waitpid(pid, &status, WNOHANG) != pid; ++i) usleep(5000);
			if(waitpid(pid, &status, WNOHANG) == 0) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
		}
		in.seg.store(nullptr, std::memory_order_release);
		munmap(seg, sizeof(shm::Segment));
		close(fd);
		delete retired;
		freeWhenStopped(in);
		return nullptr;
	}

	// ------------------------------------------------------------------------------------------------- entry points

	// Keeps this .so mapped for the life of the process: supervisor threads outlive destroy_instance, and the host may dlclose
	// the module right after it.
	void pinSelf()
	{
		static std::atomic<bool> pinned{false};
		if(pinned.exchange(true)) return;
		Dl_info info{};
		if(dladdr(reinterpret_cast<void*>(&pinSelf), &info) && info.dli_fname) dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE | RTLD_NOLOAD);
	}

	void* createInstance(const char* moduleDir, const char*)
	{
		auto* in = static_cast<Instance*>(std::calloc(1, sizeof(Instance)));
		if(!in) return nullptr;
		new(in) Instance();
		std::snprintf(in->moduleDir, sizeof in->moduleDir, "%s", moduleDir ? moduleDir : ".");
		in->table = KitValues::defaults();
		in->rules.resync(in->table);
		pinSelf();
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		in->supervisorStarted = pthread_create(&in->supervisor, &attr, supervise, in) == 0;
		pthread_attr_destroy(&attr);
		return in;
	}

	void destroyInstance(void* p)
	{
		auto* in = static_cast<Instance*>(p);
		if(!in) return;
		if(!in->supervisorStarted) { in->~Instance(); std::free(in); return; }
		// Hand the instance to the supervisor: it stops the child, unmaps and frees. Nothing here waits, and nothing touches the
		// instance after the store (a private FUTEX_WAKE does not dereference the address).
		in->stop.store(1, std::memory_order_release);
		futexWakePrivate(&in->stop);
	}

	bool transportRunning()
	{
		return g_host && g_host->get_clock_status && g_host->get_clock_status() == MOVE_CLOCK_STATUS_RUNNING;
	}

	void setFocus(Instance& in, int t) { in.focus = std::clamp(t, 0, kTracks - 1); }

	void onMidi(void* p, const uint8_t* msg, int len, int)
	{
		auto& in = *static_cast<Instance*>(p);
		if(len < 3 || (msg[0] & 0xF0) != 0x90 || msg[2] == 0) return;	// the Machinedrum's triggers are one-shot: note-offs mean nothing
		const int t = msg[1] - kNoteBase;
		if(t < 0 || t >= kTracks) return;
		const int vel = msg[2];
		if(in.set.velMode == 1) push(in, shm::Op::Trigger, t, vel, 0);
		else if(vel >= in.set.accThr) push(in, shm::Op::Trigger, t, 127, 1);	// accented: as the front panel's trigger (measured: = velocity 128)
		else push(in, shm::Op::Trigger, t, std::max(1, 127 - in.set.accAmt), 0);
		// The editor follows the pad: outright while the transport is stopped; while it runs only a note a finger played, which the
		// host vouches for through ui_live_press (either order, within the window), so a running pattern does not move the pages.
		if(!transportRunning()) setFocus(in, t);
		else if(in.pressArmed && in.block - in.pressBlock <= kLiveMatchBlocks) { setFocus(in, t); in.pressArmed = false; }
		else { in.lastHitTrack = t; in.lastHitBlock = in.block; }
	}

	// tNN_<key> -> track 0-15 and the key, else -1
	int splitTrackKey(const char* key, const char** sub)
	{
		if(key[0] != 't' || key[1] < '0' || key[1] > '9' || key[2] < '0' || key[2] > '9' || key[3] != '_') return -1;
		const int t = (key[1] - '0') * 10 + (key[2] - '0') - 1;
		if(t < 0 || t >= kTracks) return -1;
		*sub = key + 4;
		return t;
	}

	int rawIndex(const char* sub)
	{
		for(int i = 0; i < kRaw; ++i) if(!std::strcmp(sub, kRawKeys[i])) return i;
		return -1;
	}

	int lfoIndex(const char* sub)
	{
		for(int i = 0; i < kNumLfo; ++i) if(!std::strcmp(sub, kLfoKeys[i])) return i;
		return -1;
	}

	// An enum value arrives as its option name or its index.
	int enumValue(const char* val, const char* const* opts, int count)
	{
		for(int i = 0; i < count; ++i) if(!std::strcmp(val, opts[i])) return i;
		return std::clamp(std::atoi(val), 0, count - 1);
	}

	void setTrackValue(Instance& in, int t, const char* sub, const char* val)
	{
		auto& tr = in.table.track[size_t(t)];
		if(!std::strcmp(sub, "machine"))
		{
			int id = machineByName(in, val);
			if(id < 0) { char* end = nullptr; const long x = std::strtol(val, &end, 10); if(end != val && !*end) id = int(x); }	// a raw id (tests)
			if(id < 0 || id >= shm::kMachineIds || id == tr.machine) return;
			tr.machine = id;
			push(in, shm::Op::SetMachine, t, 1, id);	// the user's pick: it starts from its own values (back through Defaults)
			noteMachineChange(in);
			return;
		}
		if(!std::strcmp(sub, "level")) { tr.level = std::clamp(std::atoi(val), 0, 127); push(in, shm::Op::SetLevel, t, 0, tr.level); return; }
		if(!std::strcmp(sub, "mute")) { tr.mute = !std::strcmp(val, "On") || std::atoi(val) == 1; push(in, shm::Op::SetMute, t, 0, tr.mute ? 1 : 0); return; }
		if(const int i = lfoIndex(sub); i >= 0)
		{
			static const char* const kUpdate[3] = {"FREE", "TRIG", "HOLD"};
			int v = i == 0 ? enumValue(val, kTrackNames, kTracks) : i == 1 ? enumValue(val, kRawNames, kRaw) : i == 4 ? enumValue(val, kUpdate, 3) : std::atoi(val);
			tr.lfo[size_t(i)] = std::clamp(v, 0, kLfoMax[i]);
			push(in, shm::Op::SetLfo, t, i, tr.lfo[size_t(i)]);
			return;
		}
		const int i = rawIndex(sub);
		if(i < 0) return;
		int v;
		if(tr.machine == kCtr8P && i >= 8) v = (i & 1) ? enumValue(val, kRawNames, kRaw) : enumValue(val, kTrackNames, kTracks);	// a target
		else v = std::atoi(val) + (isBipolar(i) ? 64 : 0);
		v = std::clamp(v, 0, 127);
		if(v == tr.raw[size_t(i)]) return;
		tr.raw[size_t(i)] = v;
		push(in, shm::Op::SetRaw, t, i, v);
		if(i < 8)	// a control machine's knob: its rule moves other values
		{
			const KitValues was = in.table;
			if(in.rules.synMoved(in.table, t, i)) pushDiff(in, was);
		}
	}

	int getTrackValue(const Instance& in, int t, const char* sub, char* buf, int len)
	{
		const auto& tr = in.table.track[size_t(t)];
		if(!std::strcmp(sub, "machine"))
		{
			if(const auto* m = machineOf(in, tr.machine)) return displayName(*m, buf, len);
			return std::snprintf(buf, size_t(len), "%d", tr.machine);
		}
		if(!std::strcmp(sub, "level")) return std::snprintf(buf, size_t(len), "%d", tr.level);
		if(!std::strcmp(sub, "mute")) return std::snprintf(buf, size_t(len), "%s", tr.mute ? "On" : "Off");
		if(const int i = lfoIndex(sub); i >= 0) return std::snprintf(buf, size_t(len), "%d", tr.lfo[size_t(i)]);
		const int i = rawIndex(sub);
		if(i < 0) return -1;
		const int v = tr.raw[size_t(i)];
		return std::snprintf(buf, size_t(len), "%d", isBipolar(i) && !(tr.machine == kCtr8P) ? v - 64 : v);
	}

	void setParam(void* ptr, const char* key, const char* val)
	{
		auto& in = *static_cast<Instance*>(ptr);
		if(!key || !val) return;
		const char* k = std::strrchr(key, ':');
		k = k ? k + 1 : key;
		const char* sub = nullptr;
		if(const int t = splitTrackKey(k, &sub); t >= 0) { setTrackValue(in, t, sub, val); return; }
		if(!std::strcmp(k, "state")) { readState(in, val); return; }
		if(!std::strcmp(k, "ui_track")) { setFocus(in, std::atoi(val) - 1); return; }
		if(!std::strcmp(k, "ui_live_press"))
		{
			// a finger hit a pad: the note may already be here (then it is the hand's), or arrive within the window
			if(in.lastHitTrack >= 0 && in.block - in.lastHitBlock <= kLiveMatchBlocks) { setFocus(in, in.lastHitTrack); in.lastHitTrack = -1; in.pressArmed = false; }
			else { in.pressArmed = true; in.pressBlock = in.block; }
			return;
		}
		if(!std::strcmp(k, "ui_machine")) return;	// derived
		if(!std::strcmp(k, "kit"))
		{
			const Catalog* c = in.catalog.load(std::memory_order_acquire);
			int idx = -1;
			char* end = nullptr;
			const long x = std::strtol(val, &end, 10);
			if(end != val && !*end) idx = int(x);
			else if(!std::strcmp(val, "Init")) idx = 0;
			else if(c)
				for(size_t i = 0; i < c->kits.size() && idx < 0; ++i)
				{
					char nm[24];
					std::snprintf(nm, sizeof nm, "%s", c->kits[i].name);
					for(char* p = nm + std::strlen(nm); p > nm && p[-1] == ' '; --p) p[-1] = 0;
					if(!std::strcmp(nm, val)) idx = int(i) + 1;
				}
			loadKit(in, idx);
			return;
		}
		if(!std::strncmp(k, "mfx", 3))
		{
			const int i = std::atoi(k + 3) - 1;
			if(i < 0 || i >= kMasterBytes) return;
			in.table.master[size_t(i)] = std::clamp(std::atoi(val), 0, 127);
			push(in, shm::Op::SetMaster, 0, i, in.table.master[size_t(i)]);
			return;
		}
		if(!std::strcmp(k, "master_on"))	// Off / Full / Rev+Echo (or the index; "On" = Full)
		{
			static const char* const kFx[3] = {"Off", "Full", "Rev+Echo"};
			const int v = !std::strcmp(val, "On") ? 1 : enumValue(val, kFx, 3);
			in.table.masterOn = v != 0;
			if(v) in.set.fxMode = v - 1;
			push(in, shm::Op::SetMasterOn, 0, 0, in.table.masterOn ? 1 : 0);
			push(in, shm::Op::SetMasterMode, 0, 0, in.set.fxMode);
			return;
		}
		if(!std::strcmp(k, "depth"))
		{
			in.set.depth = std::clamp(std::atoi(val), 1, shm::kMaxDepth);
			if(auto* s = in.seg.load()) s->depth.store(uint32_t(in.set.depth));
			return;
		}
		if(!std::strcmp(k, "vel_mode")) { in.set.velMode = (!std::strcmp(val, "VEL") || std::atoi(val) == 1) ? 1 : 0; return; }
		if(!std::strcmp(k, "acc_thr")) { in.set.accThr = std::clamp(std::atoi(val), 1, 127); return; }
		if(!std::strcmp(k, "acc_amt")) { in.set.accAmt = std::clamp(std::atoi(val), 0, 127); return; }
		if(!std::strcmp(k, "voices")) { in.set.voices = std::clamp(std::atoi(val), 4, kTracks); push(in, shm::Op::SetVoices, 0, 0, in.set.voices >= kTracks ? 0 : in.set.voices); return; }
		if(!std::strcmp(k, "peak")) { if(auto* s = in.seg.load()) s->stats.maxUs.store(0); return; }	// a write resets it
	}

	int loadPercent(const Instance& in, bool peak)
	{
		auto* s = in.seg.load(std::memory_order_acquire);
		if(!s || s->phase.load() != uint32_t(shm::Phase::Ready)) return 0;
		const uint32_t us = peak ? s->stats.maxUs.load() : s->stats.lastUs.load();
		return std::min(200, int(std::lround(100.0 * us / (1e6 * shm::kFrames / 44100.0))));
	}

	int getParam(void* ptr, const char* key, char* buf, int len)
	{
		auto& in = *static_cast<Instance*>(ptr);
		if(!key || !buf || len <= 0) return -1;
		const char* k = std::strrchr(key, ':');
		k = k ? k + 1 : key;
		const char* sub = nullptr;
		if(const int t = splitTrackKey(k, &sub); t >= 0) return getTrackValue(in, t, sub, buf, len);
		if(!std::strcmp(k, "ui_hierarchy"))
		{
			const int n = buildHierarchy(in, in.hier, kHierCap);
			if(n < 0 || n >= len) return -1;
			std::memcpy(buf, in.hier, size_t(n) + 1);
			return n;
		}
		if(!std::strcmp(k, "chain_params")) return buildChainParams(buf, len);
		if(!std::strcmp(k, "split_voices"))	// the 16 tracks; ids are permanent (a bus stores them), labels follow the machines
		{
			Out o{buf, len};
			o.put("[");
			for(int t = 0; t < kTracks; ++t)
			{
				char name[24] = "";
				if(const auto* m = machineOf(in, in.table.track[size_t(t)].machine)) displayName(*m, name, sizeof name);
				o.put("%s{\"id\":\"t%02d\",\"label\":\"T%02d %s\"}", t ? "," : "", t + 1, t + 1, name);
			}
			o.put("]");
			return o.ok ? o.n : -1;
		}
		if(!std::strcmp(k, "state")) return writeState(in, buf, len);
		if(!std::strcmp(k, "is_loading"))
		{
			if(machineSet(in) != in.servedMachines && in.loadingUntilMs < nowMs()) armReplan(in);
			return std::snprintf(buf, size_t(len), "%s", nowMs() < in.loadingUntilMs ? "1" : "0");
		}
		if(!std::strcmp(k, "ui_track")) return std::snprintf(buf, size_t(len), "%d", in.focus + 1);
		if(!std::strcmp(k, "ui_machine")) return std::snprintf(buf, size_t(len), "%d", in.table.track[size_t(in.focus)].machine);
		if(!std::strcmp(k, "kit")) return std::snprintf(buf, size_t(len), "%d", std::max(0, in.set.kit));
		if(!std::strncmp(k, "mfx", 3))
		{
			const int i = std::atoi(k + 3) - 1;
			return i >= 0 && i < kMasterBytes ? std::snprintf(buf, size_t(len), "%d", in.table.master[size_t(i)]) : -1;
		}
		if(!std::strcmp(k, "master_on")) return std::snprintf(buf, size_t(len), "%s", !in.table.masterOn ? "Off" : in.set.fxMode ? "Rev+Echo" : "Full");
		if(!std::strcmp(k, "depth")) return std::snprintf(buf, size_t(len), "%d", in.set.depth);
		if(!std::strcmp(k, "vel_mode")) return std::snprintf(buf, size_t(len), "%s", in.set.velMode ? "VEL" : "MD");
		if(!std::strcmp(k, "acc_thr")) return std::snprintf(buf, size_t(len), "%d", in.set.accThr);
		if(!std::strcmp(k, "acc_amt")) return std::snprintf(buf, size_t(len), "%d", in.set.accAmt);
		if(!std::strcmp(k, "voices")) return std::snprintf(buf, size_t(len), "%d", in.set.voices);
		if(!std::strcmp(k, "load")) return std::snprintf(buf, size_t(len), "%d", loadPercent(in, false));
		if(!std::strcmp(k, "peak")) return std::snprintf(buf, size_t(len), "%d", loadPercent(in, true));
		if(!std::strcmp(k, "status"))	// diagnostics
		{
			auto* s = in.seg.load();
			if(!s) return std::snprintf(buf, size_t(len), "{\"phase\":\"none\",\"error\":\"%s\"}", in.error);
			return std::snprintf(buf, size_t(len),
				"{\"phase\":%u,\"rt\":%u,\"pid\":%u,\"underruns\":%u,\"skips\":%u,\"respawns\":%u,\"last_us\":%u,\"max_us\":%u,\"releases\":%u,\"rom\":%u,\"depth\":%d,\"focus\":%d,\"error\":\"%s\"}",
				s->phase.load(), s->rt.load(), s->child_pid.load(), s->underruns.load(), s->skips.load(), in.respawns.load(), s->stats.lastUs.load(),
				s->stats.maxUs.load(), s->stats.releases.load(), s->romSamples.load(), in.set.depth, in.focus + 1, in.error);
		}
		return -1;
	}

	int getError(void* ptr, char* buf, int len)
	{
		auto& in = *static_cast<Instance*>(ptr);
		if(!in.error[0]) return 0;
		return std::snprintf(buf, size_t(len), "%s", in.error);
	}

	// The callback's half of the clock, and the engine's answers. Returns the segment when the engine is live, nullptr otherwise.
	shm::Segment* tick(Instance& in)
	{
		++in.block;
		auto* s = in.seg.load(std::memory_order_acquire);
		if(!s) return nullptr;
		const bool ready = s->phase.load(std::memory_order_acquire) == uint32_t(shm::Phase::Ready);
		const uint32_t gen = in.segGen.load(std::memory_order_acquire);
		if(gen != in.replayedGen && ready)
		{
			in.replayedGen = gen;
			for(int t = 0; t < kTracks; ++t) in.seenDefaults[t] = s->defaults[t].seq.load(std::memory_order_acquire);
			replay(in);
			armReplan(in);	// the machine list has arrived
		}
		if(!ready) return nullptr;
		for(int t = 0; t < kTracks; ++t)	// a machine the user picked has chosen its starting values: they are the table's now
		{
			auto& d = s->defaults[t];
			const uint32_t seq = d.seq.load(std::memory_order_acquire);
			if(seq == in.seenDefaults[t]) continue;
			in.seenDefaults[t] = seq;
			auto& tr = in.table.track[size_t(t)];
			if(d.machine != tr.machine) continue;	// overtaken by another change
			for(int i = 0; i < d.count && i < kRaw; ++i) tr.raw[size_t(i)] = d.values[i];
			int syn[8];
			for(int i = 0; i < 8; ++i) syn[i] = tr.raw[size_t(i)];
			in.rules.machineDefaults(t, syn);
		}
		if(g_host && g_host->get_bpm && (++in.bpmTick & 63) == 0)	// ~6 times a second
		{
			const float bpm = g_host->get_bpm();
			if(bpm > 0.f && std::fabs(bpm - in.lastBpm) > 0.01f) { in.lastBpm = bpm; push(in, shm::Op::SetBpm, 0, 0, int(std::lround(bpm * 100.f))); }
		}
		if(in.pressArmed && in.block - in.pressBlock > kLiveMatchBlocks) in.pressArmed = false;
		return s;
	}

	inline int16_t toS16(int32_t v24)
	{
		return int16_t(std::clamp<int32_t>((v24 + 128) >> 8, -32768, 32767));
	}

	inline void addS16(int16_t& d, int32_t v24)
	{
		d = int16_t(std::clamp<int32_t>(int32_t(d) + ((v24 + 128) >> 8), -32768, 32767));
	}

	// Which tracks are rendered apart (their own Schwung bus): they leave the dry main mix in the engine. The engine hears a change
	// from its next block, so for one block a track moving onto (or off) a bus is in both places (or neither).
	void setTrackOuts(Instance& in, uint32_t mask)
	{
		if(mask == in.trackOuts) return;
		in.trackOuts = mask;
		push(in, shm::Op::SetTrackOuts, 0, 0, int(mask));
	}

	// Plays block host_block: calls _play(slot) when the engine has produced it, else counts an underrun; then advances the clock.
	template<class F> void playBlock(shm::Segment& s, F _play)
	{
		const uint32_t hb = s.host_block.load(std::memory_order_relaxed);
		const uint32_t produced = s.produced.load(std::memory_order_acquire);
		if(int32_t(produced - hb) <= 0 || int32_t(produced - hb) > shm::kSlots) s.underruns.fetch_add(1, std::memory_order_relaxed);
		else _play(int(hb & (shm::kSlots - 1)));
		s.host_block.store(hb + 1, std::memory_order_release);
		futexWakeShared(&s.host_block);
	}

	void renderBlock(void* ptr, int16_t* out, int frames)
	{
		auto& in = *static_cast<Instance*>(ptr);
		std::memset(out, 0, size_t(frames) * 2 * sizeof(int16_t));
		auto* s = tick(in);
		if(!s || frames != shm::kFrames) return;
		setTrackOuts(in, 0);	// no track on a bus: all in the main mix
		playBlock(*s, [&](int slot) { const int32_t* src = s->main[slot]; for(int i = 0; i < frames * 2; ++i) out[i] = toS16(src[i]); });
	}

	// Schwung's per-voice render (split_voices): a track on a bus is handed its own buffer and gets its share of the dry main mix
	// (its effects, VOL and pan: the level it had in the mix; the master effects are the main mix's); a track on no bus is handed
	// main_out and stays in the main mix. Accumulates,
	// never clears (the buffers alias). The same clock and engine as renderBlock: the host may switch between the two at any block.
	void renderSplit(void* ptr, int16_t* const* voiceOut, int nVoices, int16_t* mainOut, int frames)
	{
		auto& in = *static_cast<Instance*>(ptr);
		auto* s = tick(in);
		if(!s || frames != shm::kFrames) return;
		uint32_t mask = 0;
		for(int v = 0; v < std::min(nVoices, kTracks); ++v)
			if(voiceOut[v] && voiceOut[v] != mainOut) mask |= 1u << v;
		setTrackOuts(in, mask);
		playBlock(*s, [&](int slot)
		{
			const int32_t* src = s->main[slot];
			for(int i = 0; i < frames * 2; ++i) addS16(mainOut[i], src[i]);
			for(int v = 0; v < kTracks; ++v)
			{
				if(!((mask >> v) & 1)) continue;
				const int32_t* t = s->track[slot][v];
				int16_t* d = voiceOut[v];
				for(int i = 0; i < frames * 2; ++i) addS16(d[i], t[i]);
			}
		});
	}

	plugin_api_v2_t g_api = {
		MOVE_PLUGIN_API_VERSION_2, createInstance, destroyInstance, onMidi, setParam, getParam, getError, renderBlock,
	};
}

extern "C" __attribute__((visibility("default"))) plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t* host)
{
	g_host = host;
	return &g_api;
}

extern "C" __attribute__((visibility("default"))) void move_plugin_render_split(void* instance, int16_t* const* voiceOut, int nVoices, int16_t* mainOut, int frames)
{
	renderSplit(instance, voiceOut, nVoices, mainOut, frames);
}
