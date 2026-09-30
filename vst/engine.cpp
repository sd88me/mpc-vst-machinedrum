// mpc_engine() (mpc-vst-plugins wrapper/engine.h) for Machinedrum Module: all 16 voices from one instance, a
// MIDI note-number drum map (note - kBaseNote = track, clamped 0-15; velocity -> trigger velocity). See
// HANDOFF.md, "Design goal: all voices in one plugin instance".
//
// The engine runs on its own persistent thread (created once at create(), not per-block - see HANDOFF.md's
// note on why per-block thread spawn is the wrong design), rendering 128-frame blocks ahead of the host into
// a small ring, same architecture as mpc-vst-monomodule's DSP thread. render() (the host's audio callback)
// only copies a finished block out, or outputs silence when the DSP thread is behind (counted, never
// waited for).
//
// Parameters: per track (0-15), machine (a raw OS machine id - not a named option list, since the id table
// is decoded from the user's own firmware at runtime, not something this repo can commit; see
// docs/FIRMWARE.md), the AMP/EFX and ROUTE pages' params and SYN1-8 (see the slot layout below). Plus two
// globals: tempo (for LFO/E12 timing) and max_voices (HostModel::setMaxActiveVoices - a voice cap safety
// valve, see HANDOFF.md "adjustable voice cap").
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>
#include <sched.h>
#include <unistd.h>

#include "MdEngine.h"
#include "Firmware.h"
#include "tap_shared.h"

extern "C" {
#include "engine.h"
}

namespace {

mdtap::Shared g_tap;	// see tap_shared.h; the first Inst to claim g_tap.owner publishes into it

using Engine = md::engine::ParallelEngine;	// voices split over kGroups DSP2 instances on persistent threads (1 = the plain single-thread path)

// One engine per core, never the UI core (MPC's main thread lives on cpu0): take the least busy of
// cores 1..N-1 (sampled from /proc/stat over 100 ms). Same approach as mpc-vst-monomodule's chooseCore().
int chooseCore()
{
	auto sample = [](unsigned long long* busy, unsigned long long* total, int n)
	{
		std::FILE* f = std::fopen("/proc/stat", "r");
		if(!f) return;
		char line[256];
		while(std::fgets(line, sizeof line, f))
		{
			int c; unsigned long long u, ni, s, id, io, ir, so, st;
			if(std::sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &c, &u, &ni, &s, &id, &io, &ir, &so, &st) == 9 && c >= 0 && c < n)
			{
				busy[c] = u + ni + s + ir + so + st;
				total[c] = busy[c] + id + io;
			}
		}
		std::fclose(f);
	};
	const int n = static_cast<int>(std::min<long>(sysconf(_SC_NPROCESSORS_ONLN), 8));
	if(n < 2) return -1;
	unsigned long long b0[8] = {}, t0[8] = {}, b1[8] = {}, t1[8] = {};
	sample(b0, t0, n);
	struct timespec ts{0, 100000000};
	nanosleep(&ts, nullptr);
	sample(b1, t1, n);
	int best = -1; double bestLoad = 2;
	for(int c = 1; c < n; ++c)
	{
		const double dt = static_cast<double>(t1[c] - t0[c]);
		const double load = dt > 0 ? static_cast<double>(b1[c] - b0[c]) / dt : 0;
		if(load < bestLoad) { bestLoad = load; best = c; }
	}
	return best;
}

// Cores ranked by how busy they have been since boot (least busy first, core 0 last: MPC's housekeeping runs there).
// A 100 ms sample at load time can't see MPC's steady audio work (one AudioWorker is several times busier than the
// others), so the long-term share picks better homes for the engine and voice threads.
std::vector<int> rankCores()
{
	std::vector<std::pair<double, int>> load;
	if(std::FILE* f = std::fopen("/proc/stat", "r"))
	{
		char line[256];
		while(std::fgets(line, sizeof line, f))
		{
			int c; unsigned long long u, ni, s, id, io, ir, so, st;
			if(std::sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &c, &u, &ni, &s, &id, &io, &ir, &so, &st) == 9 && c >= 0)
			{
				const double busy = static_cast<double>(u + ni + s + ir + so + st), total = busy + static_cast<double>(id + io);
				load.emplace_back((c == 0 ? 10.0 : 0.0) + (total > 0 ? busy / total : 0.0), c);
			}
		}
		std::fclose(f);
	}
	std::sort(load.begin(), load.end());
	std::vector<int> cores;
	for(const auto& l : load) cores.push_back(l.second);
	return cores;
}

constexpr int kDefaultGroups = 3;	// DSP2 instances (voice threads); 1 = single thread. 3 on the Force (4 cores) since the groups are cost-balanced: each ~40% lighter than with 2 (HANDOFF 2026-09-30)
constexpr int kFrames = 128;					// the host's block size
constexpr int kInner = kFrames / Engine::kBlock;	// 32-sample engine blocks per host block
constexpr int kRing = 5, kAheadDefault = 3;	// blocks rendered ahead: 3 (8.7 ms) rides out the 7-8 ms stalls seen on the Force with 2 voice threads (2 = 5.8 ms glitched on a busy E12 kit); /tmp/md-ahead overrides
constexpr int kTracks = Engine::kTracks;
constexpr int kBaseNote = 36;					// MPC/GM kick; note 36 = track 0, 37 = track 1, ...

// slot layout, per track: 0=machine, 1=vol (HostModel raw param 17), 2=pan (param 18), 3.. = the
// AMP/EFX page's 8 params (raw 8-15, real hardware page order - see gen_params.py's FX_PARAMS), then
// the ROUTE page's 6 not-otherwise-exposed params (DIST raw 16, DEL/REV/LFOS/LFOD/LFOM raw 19-23 -
// VOL/PAN raw 17/18 are ROUTE-page params too on real hardware, but reuse the existing vol/pan keys
// above rather than duplicating them - see HANDOFF.md, "found the third per-track page: ROUTE").
// Then SYN1-8 (raw 0-7), whose meaning and good defaults are per-machine: see "SYN1-8" below.
constexpr const char* kFxKeys[] = {"amd", "amf", "eqf", "eqg", "fltf", "fltw", "fltq", "srr"};
constexpr int kNumFx = sizeof(kFxKeys) / sizeof(kFxKeys[0]);
constexpr int kFxRawParamBase = 8;	// HostModel raw param index of kFxKeys[0] ("amd")

constexpr const char* kRouteKeys[] = {"dist", "del", "rev", "lfos", "lfod", "lfom"};
constexpr int kRouteRawParam[] = {16, 19, 20, 21, 22, 23};	// not contiguous (17/18 are vol/pan)
constexpr int kNumRoute = sizeof(kRouteKeys) / sizeof(kRouteKeys[0]);
static_assert(sizeof(kRouteRawParam) / sizeof(kRouteRawParam[0]) == kNumRoute);

constexpr int kNumSyn = 8;
constexpr int kSlotSyn = 3 + kNumFx + kNumRoute;	// within a track's slots
constexpr int kSlotLevel = kSlotSyn + kNumSyn;	// the kit's track LEV (HostModel::setLevel), 0-127
// The track's LFO page (FUNCTION + SYN/EFX/ROUTE on the MD) minus SPEED/DEPTH/SHMIX, which are ROUTE's LFOS/LFOD/LFOM:
// destination track 0-15, destination param 0-23 (HostModel raw order), shapes 0-5, update 0 FREE / 1 TRIG / 2 HOLD.
constexpr const char* kLfoKeys[] = {"lfo_track", "lfo_param", "lfo_shp1", "lfo_shp2", "lfo_type"};
constexpr int kLfoMax[] = {15, 23, 5, 5, 2};
constexpr int kNumLfo = sizeof(kLfoKeys) / sizeof(kLfoKeys[0]);
constexpr int kSlotLfo = kSlotLevel + 1;
constexpr int kSlotTrack = 0, kSlotsPerTrack = kSlotLfo + kNumLfo;	// machine, vol, pan, FX, ROUTE, SYN, LEV, LFO
constexpr int kSlotTempo = kSlotTrack + kTracks * kSlotsPerTrack;
constexpr int kSlotMaxVoices = kSlotTempo + 1;
constexpr int kSlotHostBpm = kSlotMaxVoices + 1;	// MPC's tempo x 100 (wrapper "lfo_bpm", vst.json HAS_LFO_BPM); 0 = none yet
constexpr int kSlotRomEnabled = kSlotHostBpm + 1;	// ROM machines on (1) / off (0)
constexpr int kNumSlots = kSlotRomEnabled + 1;

int slotOf(const char* key)
{
	if(!std::strcmp(key, "tempo")) return kSlotTempo;
	if(!std::strcmp(key, "max_voices")) return kSlotMaxVoices;
	if(!std::strcmp(key, "rom_enabled")) return kSlotRomEnabled;
	int t = -1, consumed = 0;
	if(std::sscanf(key, "track%d_machine%n", &t, &consumed) == 1 && key[consumed] == '\0' && t >= 0 && t < kTracks)
		return kSlotTrack + t * kSlotsPerTrack + 0;
	if(std::sscanf(key, "track%d_vol%n", &t, &consumed) == 1 && key[consumed] == '\0' && t >= 0 && t < kTracks)
		return kSlotTrack + t * kSlotsPerTrack + 1;
	if(std::sscanf(key, "track%d_pan%n", &t, &consumed) == 1 && key[consumed] == '\0' && t >= 0 && t < kTracks)
		return kSlotTrack + t * kSlotsPerTrack + 2;
	if(std::sscanf(key, "track%d_level%n", &t, &consumed) == 1 && key[consumed] == '\0' && t >= 0 && t < kTracks)
		return kSlotTrack + t * kSlotsPerTrack + kSlotLevel;
	if(std::sscanf(key, "track%d_%n", &t, &consumed) == 1 && t >= 0 && t < kTracks)
	{
		const char* fxKey = key + consumed;
		for(int i = 0; i < kNumFx; ++i)
			if(!std::strcmp(fxKey, kFxKeys[i]))
				return kSlotTrack + t * kSlotsPerTrack + 3 + i;
		for(int i = 0; i < kNumRoute; ++i)
			if(!std::strcmp(fxKey, kRouteKeys[i]))
				return kSlotTrack + t * kSlotsPerTrack + 3 + kNumFx + i;
		for(int i = 0; i < kNumLfo; ++i)
			if(!std::strcmp(fxKey, kLfoKeys[i]))
				return kSlotTrack + t * kSlotsPerTrack + kSlotLfo + i;
		int p = 0, c2 = 0;
		if(std::sscanf(fxKey, "syn%d%n", &p, &c2) == 1 && fxKey[c2] == '\0' && p >= 1 && p <= kNumSyn)
			return kSlotTrack + t * kSlotsPerTrack + kSlotSyn + p - 1;
	}
	return -1;
}

struct NoteEv { uint8_t track; uint8_t velocity; };

// SYN1-8: each machine sets its own defaults for these (HostModel::setMachine), and what they mean
// changes with the machine (their names come from the OS's machine table, served as "<key>_name" for
// the wrapper's dynamic_name). So a machine change resets every SYN value the user hasn't set since to
// the new machine's default, and the engine - not the host - is the source of truth for them
// (getParameter reads eGet), so the knobs show those defaults. "Touched" = set by the host after the
// last machine change: a project restore sends machine first (lower VST index) and then the saved SYN
// values, which then win over the defaults.
struct MachineTable
{
	std::atomic<bool> ready{false};
	bool valid[256] = {};
	uint8_t defaults[256][kNumSyn] = {};
	char names[256][kNumSyn][8] = {};
};

// Kits: the MD's own kit sysex messages ($52, 1233 bytes: header, name, 16x24 track params, 16 levels, then 7-bit
// encoded machines and LFO settings - see HANDOFF.md, "kits"), from any .syx in the watched folders. The factory kits
// are the OS's own, dumped from the emulated MD at build time (tools/mdkits) and installed as <data dir>/factory.
struct Kit
{
	char name[17];
	char bank[24];	// the .syx file it came from (basename, upper case, no extension)
	uint8_t machine[kTracks];
	uint8_t params[kTracks][24];	// HostModel raw order: SYN1-8, AMD..SRR, DIST, VOL, PAN, DEL, REV, LFOS, LFOD, LFOM
	uint8_t level[kTracks];
	uint8_t lfo[kTracks][kNumLfo];	// destination track, destination param, shape 1, shape 2, update
};
struct Catalog { std::vector<Kit> kits; std::vector<std::string> banks; uint64_t signature = 0; };

// Elektron's 7-bit packing: each group of up to 7 bytes is preceded by a byte holding their top bits
std::vector<uint8_t> decode7(const uint8_t* _p, size_t _n)
{
	std::vector<uint8_t> out;
	for(size_t i = 0; i < _n; i += 8)
		for(size_t j = 1; j < 8 && i + j < _n; ++j)
			out.push_back(static_cast<uint8_t>(_p[i + j] | ((_p[i] << j) & 0x80)));
	return out;
}

bool parseKit(const uint8_t* _m, size_t _n, Kit& _k)
{
	static constexpr uint8_t kHeader[] = {0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x52};
	if(_n < 0x4d1 || std::memcmp(_m, kHeader, sizeof kHeader)) return false;
	if(_m[0x0a] == 0x7f || _m[0x0a] == 0) return false;	// an empty slot
	std::memset(&_k, 0, sizeof _k);
	for(int i = 0; i < 16 && _m[0x0a + i] >= 0x20 && _m[0x0a + i] < 0x7f; ++i) _k.name[i] = static_cast<char>(_m[0x0a + i]);
	for(int t = 0; t < kTracks; ++t)
	{
		for(int i = 0; i < 24; ++i) _k.params[t][i] = _m[0x1a + t * 24 + i] & 0x7f;
		_k.level[t] = _m[0x19a + t] & 0x7f;
	}
	const auto mach = decode7(_m + 0x1aa, 74);
	const auto lfo = decode7(_m + 0x1f4, 664);
	if(mach.size() < 64 || lfo.size() < kTracks * 36) return false;
	for(int t = 0; t < kTracks; ++t)
	{
		_k.machine[t] = mach[t * 4 + 3];	// a 32-bit big-endian word; the machine id is its low byte
		for(int i = 0; i < kNumLfo; ++i) _k.lfo[t][i] = static_cast<uint8_t>(std::clamp<int>(lfo[t * 36 + i], 0, kLfoMax[i]));
	}
	return true;
}

// A cheap signature of every .syx in the watched folders (name, size, mtime), re-sampled every few seconds on a
// background thread: the catalog is only rebuilt when it changes (as mpc-vst-monomodule's dump scan).
uint64_t scanKits(const std::vector<std::string>& _dirs, std::vector<std::string>* _files)
{
	uint64_t h = 1469598103934665603ull;
	auto mix = [&](uint64_t _v) { h ^= _v; h *= 1099511628211ull; };
	std::vector<std::string> found;
	for(const auto& dir : _dirs)
		if(DIR* d = opendir(dir.c_str()))
		{
			while(dirent* e = readdir(d))
			{
				const size_t n = std::strlen(e->d_name);
				if(n > 4 && strcasecmp(e->d_name + n - 4, ".syx") == 0) found.push_back(dir + "/" + e->d_name);
			}
			closedir(d);
		}
	std::sort(found.begin(), found.end());
	for(const auto& path : found)
	{
		for(char c : path) mix(static_cast<uint8_t>(c));
		struct stat st{};
		if(stat(path.c_str(), &st) == 0) { mix(static_cast<uint64_t>(st.st_size)); mix(static_cast<uint64_t>(st.st_mtime)); }
	}
	if(_files) *_files = found;
	return h;
}

Catalog* buildCatalog(const std::vector<std::string>& _dirs, uint64_t _sig)
{
	auto* cat = new Catalog();
	cat->signature = _sig;
	std::vector<std::string> files;
	scanKits(_dirs, &files);
	for(const auto& path : files)
	{
		std::string bank = path.substr(path.find_last_of('/') + 1);
		bank.resize(bank.size() - 4);
		for(auto& ch : bank) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
		if(bank.size() > 23) bank.resize(23);
		std::FILE* f = std::fopen(path.c_str(), "rb");
		if(!f) continue;
		std::vector<uint8_t> data;
		uint8_t buf[65536];
		size_t n;
		while((n = std::fread(buf, 1, sizeof buf, f)) > 0 && data.size() < (16u << 20)) data.insert(data.end(), buf, buf + n);
		std::fclose(f);
		bool any = false;
		for(size_t i = 0; i < data.size(); ++i)	// every sysex message in the file; kits are the $52 ones
		{
			if(data[i] != 0xf0) continue;
			size_t e = i + 1;
			while(e < data.size() && data[e] != 0xf7) ++e;
			Kit k;
			if(e < data.size() && parseKit(&data[i], e - i + 1, k))
			{
				std::snprintf(k.bank, sizeof k.bank, "%s", bank.c_str());
				cat->kits.push_back(k);
				any = true;
			}
			i = e;
		}
		if(any && std::find(cat->banks.begin(), cat->banks.end(), bank) == cat->banks.end()) cat->banks.push_back(bank);
	}
	std::sort(cat->banks.begin(), cat->banks.end());
	return cat;
}

// Randomise: the machines that make sound in this port (TRX, EFM, E12, P-I) - not GND, the ROM machines (their
// samples aren't loaded yet), MID/CTR (no audio) or INP/RAM (no audio input, no sampling here)
bool randomPoolMachine(int _id) { return (_id >= 16 && _id < 80); }

// ROM machines: ROM01-32 are OS machine ids 128-159, ROM33-48 are 176-191; each plays sample slot 0-47.
int romSlotOf(int _id) { return _id >= 128 && _id < 160 ? _id - 128 : _id >= 176 && _id < 192 ? _id - 176 + 32 : -1; }

// The ROM machines' sample memory (tools/mdkits: the voice DSP's sample directory and sample data as the MD sets
// them up from its sample flash at boot, extracted from the user's own flash image): "MDS1", then records
// [u32 address][u32 count][count x u32 word], ending with count 0. Returns the words loaded.
template<class V>
size_t loadSamples(V& _voices, const std::string& _path)
{
	std::FILE* f = std::fopen(_path.c_str(), "rb");
	if(!f) return 0;
	char magic[4] = {};
	size_t total = 0;
	if(std::fread(magic, 1, 4, f) == 4 && !std::memcmp(magic, "MDS1", 4))
	{
		std::vector<uint32_t> words;
		uint32_t head[2];
		while(std::fread(head, 4, 2, f) == 2 && head[1] > 0 && head[1] < 0x800000)
		{
			words.resize(head[1]);
			if(std::fread(words.data(), 4, head[1], f) != head[1]) break;
			_voices.writeP(head[0], words.data(), words.size());
			total += words.size();
		}
	}
	std::fclose(f);
	return total;
}
constexpr uint32_t kRomDirectory = 0x147e00;	// 4 words per sample slot: start, length, loop, flags

struct Inst
{
	std::string osPath, dataDir;
	bool romSlot[48] = {};	// which ROM sample slots hold a sample (set once the sample memory is loaded)
	std::atomic<int> param[kNumSlots];
	std::atomic<bool> synUntouched[kTracks][kNumSyn];
	MachineTable machines;
	std::atomic<bool> stop{false}, ready{false};
	std::atomic<uint32_t> underruns{0}, blocks{0}, dutyNaps{0};
	std::atomic<int> core{-1};

	NoteEv notes[256];
	std::atomic<uint32_t> nWrite{0}, nRead{0};

	alignas(64) int16_t ring[kRing][kFrames * 2];
	std::atomic<uint32_t> rWrite{0}, rRead{0};
	std::thread th, catTh;

	// kits: the catalog (rebuilt by catTh), the selected bank ("" = ALL) and kit (-1 = none loaded yet) - host thread
	std::atomic<Catalog*> cat{nullptr};
	std::vector<Catalog*> retiredCats;	// freed at destroy: a concurrent get_param() may still hold an old pointer
	std::vector<std::string> kitDirs;
	std::string bankName;
	int kitIdx = -1;
	std::chrono::steady_clock::time_point born = std::chrono::steady_clock::now();
	bool defaultKitDone = false;
	int snap[kNumSlots] = {};	// the loaded kit's values, to show "modified"

	Inst()
	{
		for(auto& p : param) p.store(0);
		for(auto& t : synUntouched) for(auto& u : t) u.store(true);
		param[kSlotTempo].store(120);
		param[kSlotRomEnabled].store(1);
		param[kSlotMaxVoices].store(6);	// the VOICES knob's default (gen_params.py): a cost budget, ROM voices count double
		// Matches gen_params.py's declared defaults: the host normally pushes these via set_param right
		// after create(), but this is what plays if render() is called before that (or from a host that
		// doesn't restore params on creation).
		constexpr int kFxDefaults[kNumFx] = {0, 0, 64, 64, 0, 127, 0, 0};	// amd amf eqf eqg fltf fltw fltq srr
		constexpr int kRouteDefaults[kNumRoute] = {0, 0, 0, 0, 0, 0};	// dist del rev lfos lfod lfom
		for(int t = 0; t < kTracks; ++t)
		{
			param[kSlotTrack + t * kSlotsPerTrack + 1].store(100);	// vol
			param[kSlotTrack + t * kSlotsPerTrack + 2].store(64);	// pan (centre)
			param[kSlotTrack + t * kSlotsPerTrack + kSlotLevel].store(100);	// LEV (HostModel's own default)
			param[kSlotTrack + t * kSlotsPerTrack + kSlotLfo + 0].store(t);	// LFO -> its own track (HostModel's default)
			for(int i = 0; i < kNumFx; ++i)
				param[kSlotTrack + t * kSlotsPerTrack + 3 + i].store(kFxDefaults[i]);
			for(int i = 0; i < kNumRoute; ++i)
				param[kSlotTrack + t * kSlotsPerTrack + 3 + kNumFx + i].store(kRouteDefaults[i]);
		}
	}

	void run();

	// A machine change, as the host makes it: SYN1-8 go back to "untouched", taking the new machine's defaults.
	void setMachine(int _t, int _v)
	{
		const int slot = kSlotTrack + _t * kSlotsPerTrack;
		if(param[slot].exchange(_v, std::memory_order_relaxed) == _v) return;
		const bool known = machines.ready.load(std::memory_order_acquire) && _v >= 0 && _v < 256 && machines.valid[_v];
		for(int i = 0; i < kNumSyn; ++i)
		{
			if(known) param[slot + kSlotSyn + i].store(machines.defaults[_v][i], std::memory_order_relaxed);
			synUntouched[_t][i].store(true);
		}
	}

	std::vector<int> kitsInBank() const
	{
		std::vector<int> out;
		if(const Catalog* c = cat.load())
			for(size_t i = 0; i < c->kits.size(); ++i)
				if(bankName.empty() || bankName == c->kits[i].bank) out.push_back(static_cast<int>(i));
		return out;
	}
	std::vector<std::string> bankList() const
	{
		std::vector<std::string> out{""};
		if(const Catalog* c = cat.load()) for(auto& b : c->banks) out.push_back(b);
		return out;
	}
	void stepBank(int _dir)
	{
		const auto banks = bankList();
		int idx = 0;
		for(size_t i = 0; i < banks.size(); ++i) if(banks[i] == bankName) idx = static_cast<int>(i);
		const int n = static_cast<int>(banks.size());
		bankName = banks[static_cast<size_t>(((idx + _dir) % n + n) % n)];
		kitIdx = -1;	// the kit list changed: the next KIT press starts at its first/last kit
	}
	std::string bankLabel() const { return bankName.empty() ? "ALL" : bankName; }
	void stepKit(int _dir)
	{
		const auto list = kitsInBank();
		if(list.empty()) return;
		const int n = static_cast<int>(list.size());
		loadKit(kitIdx < 0 ? (_dir > 0 ? 0 : n - 1) : ((kitIdx + _dir) % n + n) % n);
	}
	void loadKit(int _idx)
	{
		const auto list = kitsInBank();
		const Catalog* c = cat.load();
		if(!c || _idx < 0 || _idx >= static_cast<int>(list.size())) return;
		const Kit& k = c->kits[static_cast<size_t>(list[static_cast<size_t>(_idx)])];
		static constexpr int kRawToSlot[24] = {
			kSlotSyn + 0, kSlotSyn + 1, kSlotSyn + 2, kSlotSyn + 3, kSlotSyn + 4, kSlotSyn + 5, kSlotSyn + 6, kSlotSyn + 7,
			3, 4, 5, 6, 7, 8, 9, 10,	// AMD..SRR
			3 + kNumFx + 0, 1, 2,	// DIST, VOL, PAN
			3 + kNumFx + 1, 3 + kNumFx + 2, 3 + kNumFx + 3, 3 + kNumFx + 4, 3 + kNumFx + 5};	// DEL REV LFOS LFOD LFOM
		for(int t = 0; t < kTracks; ++t)
		{
			const int base = kSlotTrack + t * kSlotsPerTrack;
			for(int i = 0; i < kNumSyn; ++i) synUntouched[t][i].store(false);	// the kit's SYN values, not the machine's defaults
			param[base].store(k.machine[t], std::memory_order_relaxed);
			for(int i = 0; i < 24; ++i) param[base + kRawToSlot[i]].store(k.params[t][i], std::memory_order_relaxed);
			param[base + kSlotLevel].store(k.level[t], std::memory_order_relaxed);
			for(int i = 0; i < kNumLfo; ++i) param[base + kSlotLfo + i].store(k.lfo[t][i], std::memory_order_relaxed);
		}
		kitIdx = _idx;
		for(int i = 0; i < kSlotTempo; ++i) snap[i] = param[i].load(std::memory_order_relaxed);
	}
	// A fresh instance plays the first kit instead of eight empty tracks. The host pushes every param right after
	// create (a restored project's values too), so "fresh" = the catalog is up, a moment has passed, and every track
	// is still on GND-- (a restored project or a kit the user already picked always changes that). Host thread only.
	void maybeDefaultKit()
	{
		if(defaultKitDone || !cat.load() || std::chrono::steady_clock::now() - born < std::chrono::milliseconds(1500)) return;
		defaultKitDone = true;
		if(kitIdx >= 0) return;
		for(int t = 0; t < kTracks; ++t)
			if(param[kSlotTrack + t * kSlotsPerTrack].load(std::memory_order_relaxed) != 0) return;
		loadKit(0);
	}
	std::string kitLabel() const
	{
		const auto list = kitsInBank();
		const Catalog* c = cat.load();
		if(!c || kitIdx < 0 || kitIdx >= static_cast<int>(list.size())) return list.empty() ? "NO KITS" : "-";
		std::string name = c->kits[static_cast<size_t>(list[static_cast<size_t>(kitIdx)])].name;
		for(int i = 0; i < kSlotTempo; ++i)
			if(param[i].load(std::memory_order_relaxed) != snap[i]) return name + " *";
		return name;
	}
	void randomiseMachines(int _first, int _last)
	{
		std::vector<int> pool;
		for(int id = 0; id < 256; ++id)
			if((randomPoolMachine(id) || (param[kSlotRomEnabled].load() && romSlotOf(id) >= 0 && romSlot[romSlotOf(id)])) && machines.valid[id]) pool.push_back(id);
		if(pool.empty()) return;
		for(int t = _first; t <= _last; ++t) setMachine(t, pool[static_cast<size_t>(std::rand()) % pool.size()]);
	}
};

void Inst::run()
{
	try
	{
		const auto fwv = md::fw::loadFirmware(osPath);
		auto c = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(osPath)));
		int groups = kDefaultGroups;
		if(const char* e = std::getenv("MD_GROUPS")) groups = std::atoi(e);
		if(std::FILE* gf = std::fopen("/tmp/md-groups", "r"))	// A/B on the device without env: echo 1 > /tmp/md-groups, re-insert the plugin
		{
			int g = 0;
			if(std::fscanf(gf, "%d", &g) == 1) groups = g;
			std::fclose(gf);
		}
		Engine eng(fwv, std::move(c.sections.at(0).data), std::clamp(groups, 1, 4));
		auto& h = eng.host();
		// ROM machines: their samples, if the installer put the extracted sample memory in place
		if(loadSamples(eng.voices(), dataDir + "/factory/ROM_SAMPLES.bin") > 0)
			for(int k = 0; k < 48; ++k)
				romSlot[k] = eng.voices().readP(kRomDirectory + 4 * static_cast<uint32_t>(k) + 1) != 0;	// its length
		for(const auto& m : eng.os().machines())
		{
			machines.valid[m.id] = true;
			for(int p = 0; p < kNumSyn; ++p)
			{
				machines.defaults[m.id][p] = m.defaults[p];
				std::snprintf(machines.names[m.id][p], sizeof machines.names[m.id][p], "%s", m.params[p].c_str());
			}
		}
		machines.ready.store(true, std::memory_order_release);

		// Real-time priority - only once booted, so the boot itself doesn't hog the CPU at real-time priority.
		// Without it the render thread is a plain SCHED_OTHER thread that gets starved under system load, heard
		// as choppy audio. But BELOW MPC's own audio threads (AudioWorkerN / Audio Processing: SCHED_RR 20) and
		// its MIDI out (RR 10): at FIFO 30 (above them, the first version) an overloaded engine took its core
		// from MPC's AudioWorker there and the whole Force glitched, not just this plugin (measured on the
		// device, 2026-09-28: this thread at 89% of its core with a 4-track kit). Overloaded, it now drops
		// only its own blocks (counted as underruns).
		pthread_setname_np(pthread_self(), "md-engine");
		std::vector<int> ranked;	// cores, least busy first: the engine thread takes the first, voice group g the g-th
		{
			int prio = 5;
			if(const char* e = std::getenv("MD_FIFO")) prio = std::atoi(e);
			if(prio > 0)
			{
				sched_param sp{};
				sp.sched_priority = prio;
				pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
			}
			ranked = rankCores();
			const int c = std::getenv("MD_CPU") ? std::atoi(std::getenv("MD_CPU")) : ranked.empty() ? chooseCore() : ranked[0];
			if(c >= 0)
			{
				cpu_set_t s;
				CPU_ZERO(&s);
				CPU_SET(c, &s);
				sched_setaffinity(0, sizeof s, &s);
			}
			core.store(c, std::memory_order_relaxed);
		}
		// Voice worker threads: same real-time class as the engine thread, on the cores after it.
		eng.voices().tuneWorkers([ranked](int _g)
		{
			pthread_setname_np(pthread_self(), ("md-voice" + std::to_string(_g)).c_str());
			int prio = 5;
			if(const char* e = std::getenv("MD_FIFO")) prio = std::atoi(e);
			if(prio > 0)
			{
				sched_param sp{};
				sp.sched_priority = prio;
				pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
			}
			if(static_cast<size_t>(_g) < ranked.size())
			{
				cpu_set_t s;
				CPU_ZERO(&s);
				CPU_SET(ranked[static_cast<size_t>(_g)], &s);	// group g takes the g-th least busy core
				sched_setaffinity(0, sizeof s, &s);
			}
		});

		// Silence release (HostModel::setSilenceRelease): a voice quiet below -96 dBFS for 138 blocks (100 ms) stops costing DSP time.
		// /tmp/md-release overrides the hold in blocks (0 = off), for A/B on the device.
		int releaseBlocks = 138;
		if(std::FILE* rf = std::fopen("/tmp/md-release", "r"))
		{
			int v = 0;
			if(std::fscanf(rf, "%d", &v) == 1) releaseBlocks = std::max(0, v);
			std::fclose(rf);
		}
		if(releaseBlocks > 0) h.setSilenceRelease(128, releaseBlocks);
		int ahead = kAheadDefault;	// blocks rendered ahead of the host: /tmp/md-ahead (1-3) overrides, for A/B on the device
		if(std::FILE* af = std::fopen("/tmp/md-ahead", "r"))
		{
			int a = 0;
			if(std::fscanf(af, "%d", &a) == 1) ahead = std::clamp(a, 1, kRing - 1);
			std::fclose(af);
		}
		int appliedTempo = -1, appliedMaxVoices = -1;
		const bool tapOwner = [this] { const void* none = nullptr; return g_tap.owner.compare_exchange_strong(none, this); }();
		int appliedMachine[kTracks], appliedEff[kTracks], appliedVol[kTracks], appliedPan[kTracks];
		int appliedFx[kTracks][kNumFx];
		int appliedRoute[kTracks][kNumRoute];
		int appliedSyn[kTracks][kNumSyn];
		int appliedLevel[kTracks];
		int appliedLfo[kTracks][kNumLfo];
		for(int t = 0; t < kTracks; ++t)
		{
			appliedMachine[t] = appliedEff[t] = appliedVol[t] = appliedPan[t] = appliedLevel[t] = -1;
			for(int i = 0; i < kNumFx; ++i) appliedFx[t][i] = -1;
			for(int i = 0; i < kNumRoute; ++i) appliedRoute[t][i] = -1;
			for(int i = 0; i < kNumSyn; ++i) appliedSyn[t][i] = -1;
			for(int i = 0; i < kNumLfo; ++i) appliedLfo[t][i] = -1;
		}

		// Duty cap: this thread runs at a real-time priority (else it is starved and glitches), and overloaded it never
		// sleeps - it would keep its core 100% busy and starve MPC's own normal-priority threads on it, one of which can
		// hold a lock the UI thread waits for: the whole MPC froze (measured 2026-09-29: this thread at 100% of core 2,
		// mean 4.4 ms per 2.9 ms block, MPC's main thread blocked on that lock until this thread was demoted). So over a
		// ~30 ms window it may use at most kMaxDuty of its core's CPU time; the rest of the window it sleeps. An
		// overload then costs it dropped blocks (crackle, counted as underruns), never MPC's UI.
		double kMaxDuty = std::getenv("MD_DUTY") ? std::atof(std::getenv("MD_DUTY")) : 0.95;	// MD_DUTY: test override
		if(std::FILE* df = std::fopen("/tmp/md-duty", "r"))	// same on the device without env: echo 0.95 > /tmp/md-duty, re-insert
		{
			double d = 0;
			if(std::fscanf(df, "%lf", &d) == 1 && d >= 0.3 && d <= 1.0) kMaxDuty = d;
			std::fclose(df);
		}
		auto cpuNow = [] { timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3; };
		double winCpu0 = cpuNow();
		auto winWall0 = std::chrono::steady_clock::now();
		Engine::Output out;
		const bool statsOn = std::getenv("MD_STATS") != nullptr || access("/tmp/md-stats-on", F_OK) == 0;
		auto statT = std::chrono::steady_clock::now(), lastEnd = statT;
		double worstUs = 0, sumUs = 0, worstGap = 0;
		int nUs = 0, maxActive = 0;
		double pTick = 0, pDsp = 0, pFx = 0, pMix = 0;	// stage timers at the last stats line (Engine::timingOn, only with stats on)
		eng.timingOn = statsOn;
		eng.voices().timingOn = statsOn;
		ready.store(true);

		while(!stop.load(std::memory_order_acquire))
		{
			const uint32_t w = rWrite.load(std::memory_order_relaxed);
			if(int32_t(w - rRead.load(std::memory_order_acquire)) >= ahead)
			{
				struct timespec ts{0, 400000};
				nanosleep(&ts, nullptr);
				continue;
			}

			// MPC's own tempo once the wrapper has sent it (it does every block it changes), else the tempo param
			const int hostBpm = param[kSlotHostBpm].load(std::memory_order_relaxed);
			const int tempo = hostBpm > 0 ? std::clamp(hostBpm, 3000, 30000) : std::clamp(param[kSlotTempo].load(std::memory_order_relaxed), 30, 300) * 100;
			if(tempo != appliedTempo) { appliedTempo = tempo; h.setTempo(tempo / 100.0); }
			const int maxVoices = std::clamp(param[kSlotMaxVoices].load(std::memory_order_relaxed), 1, kTracks);
			if(maxVoices != appliedMaxVoices) { appliedMaxVoices = maxVoices; h.setMaxActiveVoices(maxVoices); }
			for(int t = 0; t < kTracks; ++t)
			{
				const int m = std::clamp(param[kSlotTrack + t * kSlotsPerTrack + 0].load(std::memory_order_relaxed), 0, 191);
				if(m != appliedMachine[t])
				{
					appliedMachine[t] = m;
					appliedEff[t] = -1;	// the machine just applied is the real one: the ROM on/off swap below runs again
					h.setMachine(t, static_cast<uint8_t>(m));	// raw SYN1-8 = the machine's defaults
					for(int i = 0; i < kNumSyn; ++i)
					{
						// Untouched ones adopt the defaults (normally eSet already did this); touched ones are
						// re-applied over them below.
						if(synUntouched[t][i].exchange(false))
							param[kSlotTrack + t * kSlotsPerTrack + kSlotSyn + i].store(h.param(t, i));
						appliedSyn[t][i] = -1;
					}
				}
				{
					// ROM machines off: the track plays the empty machine instead (its own setting is kept). The SYN values are
					// re-applied over the swapped machine's defaults, so switching back finds them as they were.
					const bool romOff = param[kSlotRomEnabled].load(std::memory_order_relaxed) == 0;
					const int eff = romOff && romSlotOf(m) >= 0 ? 0 : m;
					if(eff != appliedEff[t])
					{
						appliedEff[t] = eff;
						h.setMachine(t, static_cast<uint8_t>(eff));
						for(int i = 0; i < kNumSyn; ++i) appliedSyn[t][i] = -1;
					}
				}
				const int vol = std::clamp(param[kSlotTrack + t * kSlotsPerTrack + 1].load(std::memory_order_relaxed), 0, 127);
				// VOL (param 17, read by the mixer's own gain formula) - not setLevel(), a separate kit LEV
				// knob (HostModel.h) that also gates level but isn't what mdrender.cpp's working demo kit uses.
				if(vol != appliedVol[t]) { appliedVol[t] = vol; h.setParam(t, 17, vol); }
				{
					int v[kNumLfo];
					bool changed = false;
					for(int i = 0; i < kNumLfo; ++i)
					{
						v[i] = std::clamp(param[kSlotTrack + t * kSlotsPerTrack + kSlotLfo + i].load(std::memory_order_relaxed), 0, kLfoMax[i]);
						changed |= v[i] != appliedLfo[t][i];
						appliedLfo[t][i] = v[i];
					}
					if(changed) h.setLfo(t, v[0], v[1], v[2], v[3], v[4]);
				}
				const int lev = std::clamp(param[kSlotTrack + t * kSlotsPerTrack + kSlotLevel].load(std::memory_order_relaxed), 0, 127);
				if(lev != appliedLevel[t]) { appliedLevel[t] = lev; h.setLevel(t, lev); }
				const int pan = std::clamp(param[kSlotTrack + t * kSlotsPerTrack + 2].load(std::memory_order_relaxed), 0, 127);
				if(pan != appliedPan[t]) { appliedPan[t] = pan; h.setParam(t, 18, pan); }
				for(int i = 0; i < kNumFx; ++i)
				{
					const int v = std::clamp(param[kSlotTrack + t * kSlotsPerTrack + 3 + i].load(std::memory_order_relaxed), 0, 127);
					if(v != appliedFx[t][i]) { appliedFx[t][i] = v; h.setParam(t, kFxRawParamBase + i, v); }
				}
				for(int i = 0; i < kNumRoute; ++i)
				{
					const int v = std::clamp(param[kSlotTrack + t * kSlotsPerTrack + 3 + kNumFx + i].load(std::memory_order_relaxed), 0, 127);
					if(v != appliedRoute[t][i]) { appliedRoute[t][i] = v; h.setParam(t, kRouteRawParam[i], v); }
				}
				for(int i = 0; i < kNumSyn; ++i)
				{
					const int v = std::clamp(param[kSlotTrack + t * kSlotsPerTrack + kSlotSyn + i].load(std::memory_order_relaxed), 0, 127);
					if(v != appliedSyn[t][i]) { appliedSyn[t][i] = v; h.setParam(t, i, v); }
				}
			}

			uint32_t r = nRead.load(std::memory_order_relaxed);
			const uint32_t nw = nWrite.load(std::memory_order_acquire);
			for(; r != nw; ++r)
			{
				const NoteEv e = notes[r & 255];
				h.trigger(e.track, e.velocity);
			}
			nRead.store(r, std::memory_order_release);

			if(tapOwner)
			{
				uint32_t mask = 0;
				for(int t = 0; t < kTracks; ++t) if(g_tap.tapped[t].load(std::memory_order_relaxed) > 0) mask |= 1u << t;
				eng.dryMute = mask;
			}
			int16_t* dst = ring[w % kRing];
			const auto t0 = std::chrono::steady_clock::now();
			for(int i = 0; i < kInner; ++i)
			{
				if(!eng.render(out))
				{
					std::fprintf(stderr, "[machinedrum] DSP fault: %s\n", eng.fault().c_str());
					return;
				}
				for(int f = 0; f < Engine::kBlock; ++f)
				{
					const int idx = i * Engine::kBlock + f;
					// 24-bit -> 16-bit
					dst[2 * idx + 0] = static_cast<int16_t>(std::clamp(out.mix.main[f][0] >> 8, -32768, 32767));
					dst[2 * idx + 1] = static_cast<int16_t>(std::clamp(out.mix.main[f][1] >> 8, -32768, 32767));
				}
				if(tapOwner)
				{
					auto& planes = g_tap.data[w % mdtap::kSlots];
					auto q = [](int32_t v) { return static_cast<int16_t>(std::clamp(v >> 8, -32768, 32767)); };
					for(int t = 0; t < kTracks; ++t)
					{
						const uint32_t vol = h.mixerInput(t).mix[1];
						for(int f = 0; f < Engine::kBlock; ++f)
							planes[t][i * Engine::kBlock + f] = q(md::engine::Mixer::solo(out.tracks[t][f], vol));
					}
					for(int f = 0; f < Engine::kBlock; ++f)
					{
						planes[mdtap::kPlaneRev][i * Engine::kBlock + f] = q(out.mix.rev[f][0]);
						planes[mdtap::kPlaneRev + 1][i * Engine::kBlock + f] = q(out.mix.rev[f][1]);
						planes[mdtap::kPlaneDel][i * Engine::kBlock + f] = q(out.mix.del[f][0]);
						planes[mdtap::kPlaneDel + 1][i * Engine::kBlock + f] = q(out.mix.del[f][1]);
					}
				}
			}
			if(tapOwner) g_tap.written.store(w + 1, std::memory_order_release);
			rWrite.store(w + 1, std::memory_order_release);
			blocks.fetch_add(1, std::memory_order_relaxed);
			{
				const auto wn = std::chrono::steady_clock::now();
				const double wall = std::chrono::duration<double, std::micro>(wn - winWall0).count();
				if(wall >= 30000)
				{
					const double cpu = cpuNow() - winCpu0;
					if(cpu > kMaxDuty * wall)
					{
						const double nap = std::min(cpu / kMaxDuty - wall, 20000.0);	// us
						timespec ts{0, static_cast<long>(nap * 1000)};
						nanosleep(&ts, nullptr);
						dutyNaps.fetch_add(1, std::memory_order_relaxed);
					}
					winCpu0 = cpuNow();
					winWall0 = std::chrono::steady_clock::now();
				}
			}
			if(statsOn)	// MD_STATS=1 or a /tmp/md-stats-on file: once a second, /tmp/md-stats.<pid> = underruns, worst/mean render us, worst gap between blocks
			{
				const auto t1 = std::chrono::steady_clock::now();
				const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
				const double gap = std::chrono::duration<double, std::micro>(t1 - lastEnd).count();
				lastEnd = t1;
				worstUs = std::max(worstUs, us); sumUs += us; ++nUs; worstGap = std::max(worstGap, gap);
				maxActive = std::max(maxActive, eng.voices().activeVoicesLastBlock());
				if(t1 - statT >= std::chrono::seconds(1))
				{
					statT = t1;
					if(FILE* f = std::fopen(("/tmp/md-stats." + std::to_string(getpid())).c_str(), "a"))
					{
						std::fprintf(f, "underruns=%u released=%u naps=%u worst_us=%.0f mean_us=%.0f worst_gap_us=%.0f active=%d rom=%d budget=%d tick=%.0f dsp=%.0f fx=%.0f mix=%.0f\n", underruns.load(), h.silenceReleases(), dutyNaps.load(), worstUs, sumUs / std::max(1, nUs), worstGap, maxActive, param[kSlotRomEnabled].load(), param[kSlotMaxVoices].load(), (h.tickUs - pTick) / std::max(1, nUs), (h.dspUs - pDsp) / std::max(1, nUs), (eng.fxUs - pFx) / std::max(1, nUs), (eng.mixUs - pMix) / std::max(1, nUs));
						auto& vg = eng.voices();
						std::fprintf(f, "  groups:");
						for(int g = 0; g < vg.groupCount(); ++g)
						{
							std::fprintf(f, " g%d_us=%.0f g%d_voices=%.2f", g, vg.groupUs[g] / std::max(1, nUs), g, vg.groupVoices[g] / std::max(1, nUs));
							vg.groupUs[g] = vg.groupVoices[g] = 0;
						}
						std::fprintf(f, "\n");
						std::fclose(f);
					}
					pTick = h.tickUs; pDsp = h.dspUs; pFx = eng.fxUs; pMix = eng.mixUs;
					worstUs = sumUs = worstGap = 0; nUs = 0; maxActive = 0;
				}
			}
		}
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "[machinedrum] %s\n", e.what());
	}
}

void* eCreate(const char* dataDir)
{
	auto* in = new Inst();
	std::srand(static_cast<unsigned>(std::time(nullptr)));	// the randomise actions: different each session
	if(const char* p = std::getenv("MD_OS")) in->osPath = p;
	else in->osPath = std::string(dataDir && *dataDir ? dataDir : ".") + "/Elektron_SPS1-1UW_OS1.63.syx";
	const std::string base = dataDir && *dataDir ? dataDir : ".";
	in->dataDir = base;
	// factory: the installer's copy of the OS's own kits; kits: the user's packs (SD card copy, or MPC's Documents browser)
	in->kitDirs = {base + "/factory", base + "/kits", "/sdcard/Force Documents/Machinedrum Kits"};
	in->catTh = std::thread([in] {
		while(!in->stop.load(std::memory_order_acquire))
		{
			const uint64_t sig = scanKits(in->kitDirs, nullptr);
			const Catalog* cur = in->cat.load(std::memory_order_relaxed);
			if(!cur || cur->signature != sig)
			{
				Catalog* next = buildCatalog(in->kitDirs, sig);
				if(Catalog* old = in->cat.exchange(next, std::memory_order_release)) in->retiredCats.push_back(old);
			}
			for(int i = 0; i < 30 && !in->stop.load(std::memory_order_acquire); ++i)	// ~3 s between scans
			{
				struct timespec ts{0, 100000000};
				nanosleep(&ts, nullptr);
			}
		}
	});
	in->th = std::thread([in] { in->run(); });
	return in;
}

void eDestroy(void* p)
{
	auto* in = static_cast<Inst*>(p);
	in->stop.store(true, std::memory_order_release);
	if(in->th.joinable()) in->th.join();
	if(in->catTh.joinable()) in->catTh.join();
	{ const void* me = in; g_tap.owner.compare_exchange_strong(me, nullptr); }
	for(auto* c : in->retiredCats) delete c;
	delete in->cat.load();
	delete in;
}

void eMidi(void* p, const uint8_t* msg, int len)
{
	auto* in = static_cast<Inst*>(p);
	if(len < 3) return;
	const uint8_t status = msg[0] & 0xf0;
	if(status == 0x90 && msg[2] > 0)	// note on
	{
		const int track = static_cast<int>(msg[1]) - kBaseNote;
		if(track < 0 || track >= kTracks) return;
		const uint32_t w = in->nWrite.load(std::memory_order_relaxed);
		in->notes[w & 255] = {static_cast<uint8_t>(track), msg[2]};
		in->nWrite.store(w + 1, std::memory_order_release);
	}
}

void eSet(void* p, const char* key, const char* val)
{
	auto* in = static_cast<Inst*>(p);
	in->maybeDefaultKit();
	if(!std::strcmp(key, "lfo_bpm"))
	{
		in->param[kSlotHostBpm].store(static_cast<int>(std::lround(std::atof(val) * 100.0)), std::memory_order_relaxed);
		return;
	}
	// momentary actions (the wrapper springs them back; they read back as 0)
	const bool on = std::atof(val) > 0.5;
	if(!std::strcmp(key, "kit_prev") || !std::strcmp(key, "kit_next")) { if(on) in->stepKit(key[4] == 'n' ? 1 : -1); return; }
	if(!std::strcmp(key, "bank_prev") || !std::strcmp(key, "bank_next")) { if(on) in->stepBank(key[5] == 'n' ? 1 : -1); return; }
	if(!std::strcmp(key, "randomize_all")) { if(on) in->randomiseMachines(0, 15); return; }
	if(!std::strcmp(key, "randomize_1_8")) { if(on) in->randomiseMachines(0, 7); return; }
	if(!std::strcmp(key, "randomize_9_16")) { if(on) in->randomiseMachines(8, 15); return; }
	if(!std::strcmp(key, "randomize_kit"))
	{
		const auto list = in->kitsInBank();
		if(on && !list.empty()) in->loadKit(std::rand() % static_cast<int>(list.size()));
		return;
	}
	const int slot = slotOf(key);
	if(slot < 0) return;
	const int v = std::atoi(val);
	const int t = (slot - kSlotTrack) / kSlotsPerTrack, s = (slot - kSlotTrack) % kSlotsPerTrack;
	if(slot >= kSlotTempo)
		in->param[slot].store(v, std::memory_order_relaxed);
	else if(s == 0)
		in->setMachine(t, v);
	else
	{
		if(s >= kSlotSyn) in->synUntouched[t][s - kSlotSyn].store(false);
		in->param[slot].store(v, std::memory_order_relaxed);
	}
}

int eGet(void* p, const char* key, char* buf, int bufLen)
{
	auto* in = static_cast<Inst*>(p);
	in->maybeDefaultKit();
	if(!std::strcmp(key, "kit_name")) return std::snprintf(buf, static_cast<size_t>(bufLen), "%s", in->kitLabel().c_str()) > 0;
	if(!std::strcmp(key, "bank_name")) return std::snprintf(buf, static_cast<size_t>(bufLen), "%s", in->bankLabel().c_str()) > 0;
	if(!std::strncmp(key, "kit_", 4) || !std::strncmp(key, "bank_", 5) || !std::strncmp(key, "randomize_", 10))
		return std::snprintf(buf, static_cast<size_t>(bufLen), "0") > 0;	// momentary: always reads back off
	if(!std::strcmp(key, "ready"))
		return std::snprintf(buf, static_cast<size_t>(bufLen), "%d", in->ready.load(std::memory_order_acquire) ? 1 : 0) > 0;
	if(!std::strcmp(key, "underruns"))
		return std::snprintf(buf, static_cast<size_t>(bufLen), "%u", in->underruns.load(std::memory_order_relaxed)) > 0;
	if(!std::strcmp(key, "blocks"))
		return std::snprintf(buf, static_cast<size_t>(bufLen), "%u", in->blocks.load(std::memory_order_relaxed)) > 0;
	if(!std::strcmp(key, "core"))
		return std::snprintf(buf, static_cast<size_t>(bufLen), "%d", in->core.load(std::memory_order_relaxed)) > 0;
	// "track<N>_lfo_param_display": the LFO destination's label, as the MD shows it (params.json dynamic_display)
	{
		int t = -1, n = 0;
		if(std::sscanf(key, "track%d_lfo_param_display%n", &t, &n) == 1 && key[n] == '\0' && t >= 0 && t < kTracks)
		{
			static constexpr const char* kFixed[16] = {"AMD", "AMF", "EQF", "EQG", "FLTF", "FLTW", "FLTQ", "SRR", "DIST", "VOL", "PAN", "DEL", "REV", "LFOS", "LFOD", "LFOM"};
			const int base = kSlotTrack + t * kSlotsPerTrack + kSlotLfo;
			const int dt = std::clamp(in->param[base].load(std::memory_order_relaxed), 0, kTracks - 1);
			const int dp = std::clamp(in->param[base + 1].load(std::memory_order_relaxed), 0, 23);
			const char* name = "?";
			if(dp >= 8) name = kFixed[dp - 8];
			else
			{
				const int m = in->param[kSlotTrack + dt * kSlotsPerTrack].load(std::memory_order_relaxed);
				if(!in->machines.ready.load(std::memory_order_acquire) || m < 0 || m >= 256 || !in->machines.valid[m]) name = "-";
				else name = *in->machines.names[m][dp] ? in->machines.names[m][dp] : "-";
			}
			return std::snprintf(buf, static_cast<size_t>(bufLen), "%s", name) > 0;
		}
	}
	// "track<N>_syn<P>_name": the current machine's label for that SYN knob (params.json dynamic_name)
	{
		int t = -1, p = 0, n = 0;
		if(std::sscanf(key, "track%d_syn%d_name%n", &t, &p, &n) == 2 && key[n] == '\0' && t >= 0 && t < kTracks && p >= 1 && p <= kNumSyn)
		{
			const int m = in->param[kSlotTrack + t * kSlotsPerTrack].load(std::memory_order_relaxed);
			if(!in->machines.ready.load(std::memory_order_acquire) || m < 0 || m >= 256 || !in->machines.valid[m]) return 0;
			const char* name = in->machines.names[m][p - 1];
			if(!*name) name = "-";
			return std::snprintf(buf, static_cast<size_t>(bufLen), "%s", name) > 0;
		}
	}
	const int slot = slotOf(key);
	if(slot < 0) return 0;
	return std::snprintf(buf, static_cast<size_t>(bufLen), "%d", in->param[slot].load(std::memory_order_relaxed)) > 0;
}

void eRender(void* p, int16_t* out, int frames)
{
	auto* in = static_cast<Inst*>(p);
	if(!in->ready.load(std::memory_order_acquire) || frames != kFrames)
	{
		std::memset(out, 0, sizeof(int16_t) * static_cast<size_t>(frames) * 2);
		return;
	}
	const uint32_t r = in->rRead.load(std::memory_order_relaxed);
	if(int32_t(in->rWrite.load(std::memory_order_acquire) - r) > 0)
	{
		std::memcpy(out, in->ring[r % kRing], sizeof(int16_t) * kFrames * 2);
		in->rRead.store(r + 1, std::memory_order_release);
		if(g_tap.owner.load(std::memory_order_relaxed) == in) g_tap.hostRead.store(r + 1, std::memory_order_release);
	}
	else
	{
		std::memset(out, 0, sizeof(int16_t) * kFrames * 2);
		in->underruns.fetch_add(1, std::memory_order_relaxed);
	}
	if(g_tap.owner.load(std::memory_order_relaxed) == in)
		g_tap.hostCallUs.store(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_release);
}

const mpc_engine_t kEngine = {eCreate, eDestroy, eMidi, eSet, eGet, eRender, nullptr};

}

extern "C" const mpc_engine_t* mpc_engine(void) { return &kEngine; }

extern "C" __attribute__((visibility("default"))) mdtap::Shared* md_tap_shared(void) { return &g_tap; }
