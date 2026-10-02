// mpc_engine() for "Machinemodule Tap": one Machinedrum track (mono) or send (stereo), read from the shared state of the
// "Machinemodule" instance in the same MPC process (vst/tap_shared.h), so MPC's own mixer, submixes and send effects
// can process it. A tap has no engine of its own: it only reads. Silence until a Machinemodule exists.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <dlfcn.h>
#include <unistd.h>

#include "tap_shared.h"

extern "C" {
#include "engine.h"
}

namespace {

using mdtap::Shared;

Shared* fromHandle(void* h)
{
	if(!h) return nullptr;
	auto fn = reinterpret_cast<Shared* (*)()>(dlsym(h, "md_tap_shared"));
	Shared* s = fn ? fn() : nullptr;
	return s && s->magic == Shared::kMagic ? s : nullptr;
}

// The Module's shared state, if MPC has loaded machinedrum_one.so (RTLD_NOLOAD: never loads it ourselves, only finds the copy
// whose statics the Module instances use). Each plugin is installed in its own Synths folder, so the Module's path isn't known:
// by its soname first, then by the path MPC loaded it from (/proc/self/maps).
Shared* findShared()
{
	static Shared* s = nullptr;
	if(s) return s;
	if((s = fromHandle(dlopen("machinedrum_one.so", RTLD_NOLOAD | RTLD_LAZY)))) return s;
	if(FILE* f = std::fopen("/proc/self/maps", "r"))
	{
		char line[1024];
		while(!s && std::fgets(line, sizeof line, f))
		{
			const char* p = std::strchr(line, '/');
			if(!p || !std::strstr(p, "/machinedrum_one.so")) continue;
			std::string path(p);
			while(!path.empty() && (path.back() == '\n' || path.back() == ' ')) path.pop_back();
			s = fromHandle(dlopen(path.c_str(), RTLD_NOLOAD | RTLD_LAZY));
		}
		std::fclose(f);
	}
	return s;
}

constexpr int64_t kPeriodUs = 2902;	// 128 frames at 44.1 kHz

struct Tap
{
#ifdef TAP_FX
	std::atomic<uint32_t> mask{1u << mdtap::kBitRev};	// the sources this tap sums (bit per source, tap_shared.h)
#else
	std::atomic<uint32_t> mask{0};
#endif
	std::atomic<int> through{0};
	uint32_t counted = 0;	// the sources currently registered in Shared (render/host thread only)
	Shared* sh = nullptr;
	int calls = 0;
	// Alignment diagnostics: with /tmp/md-stats-on present, once a second /tmp/md-tap-stats.<pid> gets the counts of calls that
	// found the primary already called this period vs not, and the time-since-primary range of each case.
	int64_t nFirst = 0, nSecond = 0, minFirst = 1 << 30, maxFirst = -(1 << 30), minSecond = 1 << 30, maxSecond = -(1 << 30);
	std::chrono::steady_clock::time_point statT = std::chrono::steady_clock::now();
	void note(bool first, int64_t dt)
	{
		if(access("/tmp/md-stats-on", F_OK) != 0) return;
		(first ? nFirst : nSecond)++;
		auto& lo = first ? minFirst : minSecond; auto& hi = first ? maxFirst : maxSecond;
		lo = std::min(lo, dt); hi = std::max(hi, dt);
		const auto n = std::chrono::steady_clock::now();
		if(n - statT < std::chrono::seconds(1)) return;
		statT = n;
		if(FILE* f = std::fopen(("/tmp/md-tap-stats." + std::to_string(getpid())).c_str(), "a"))
		{
			std::fprintf(f, "mask=%05x primary_first=%lld (dt %lld..%lld us) primary_second=%lld (dt %lld..%lld us)\n", mask.load(), (long long)nFirst, (long long)minFirst, (long long)maxFirst, (long long)nSecond, (long long)minSecond, (long long)maxSecond);
			std::fclose(f);
		}
		nFirst = nSecond = 0; minFirst = minSecond = 1 << 30; maxFirst = maxSecond = -(1 << 30);
	}

	static void apply(Shared& sh, uint32_t m, int d)
	{
		for(int t = 0; t < mdtap::kTracks; ++t) if((m >> t) & 1) sh.tapped[t].fetch_add(d);
		if((m >> mdtap::kBitRev) & 1) sh.tapsRev.fetch_add(d);
		if((m >> mdtap::kBitDel) & 1) sh.tapsDel.fetch_add(d);
	}
	void unregister()
	{
		if(sh && counted) apply(*sh, counted, -1);
		counted = 0;
	}
	void sync(uint32_t want)
	{
		if(!sh || want == counted) return;
		unregister();
		apply(*sh, want, +1);
		counted = want;
	}
};

void* eCreate(const char*) { return new Tap(); }
void eDestroy(void* p) { auto* t = static_cast<Tap*>(p); t->unregister(); delete t; }
void eMidi(void*, const uint8_t*, int) {}
// keys: src1..src16, src_rev, src_del (one toggle each), through (FX build)
int bitOf(const char* key)
{
	if(!std::strcmp(key, "src_rev")) return mdtap::kBitRev;
	if(!std::strcmp(key, "src_del")) return mdtap::kBitDel;
	int k = 0, len = 0;
	if(std::sscanf(key, "src%d%n", &k, &len) == 1 && key[len] == '\0' && k >= 1 && k <= mdtap::kTracks) return k - 1;
	return -1;
}
void eSet(void* p, const char* key, const char* val)
{
	auto* t = static_cast<Tap*>(p);
	if(!std::strcmp(key, "through")) { t->through.store(std::atoi(val) != 0); return; }
	const int b = bitOf(key);
	if(b < 0) return;
	if(std::atoi(val) != 0) t->mask.fetch_or(1u << b); else t->mask.fetch_and(~(1u << b));
}
int eGet(void* p, const char* key, char* buf, int len)
{
	auto* t = static_cast<Tap*>(p);
	if(!std::strcmp(key, "through")) return std::snprintf(buf, static_cast<size_t>(len), "%d", t->through.load());
	const int b = bitOf(key);
	if(b < 0) return 0;
	return std::snprintf(buf, static_cast<size_t>(len), "%d", (t->mask.load() >> b) & 1);
}

void fill(Tap* t, int16_t* out, int frames)
{
	std::memset(out, 0, sizeof(int16_t) * static_cast<size_t>(frames) * 2);
	if(frames != mdtap::kFrames) return;
	if(!t->sh)
	{
		if(++t->calls % 64 != 1) return;	// look for the primary about every 190 ms
		t->sh = findShared();
		if(!t->sh) return;
	}
	Shared& s = *t->sh;
	if(!s.owner.load(std::memory_order_acquire)) { t->unregister(); return; }
	const uint32_t mask = t->mask.load(std::memory_order_relaxed);
	t->sync(mask);
	// Take the block the primary's host takes in this same period: if it has already been called this period (within half a
	// period) that is the one it just took (hostRead - 1), else the one it is about to take (hostRead). MPC calls the instances
	// in no fixed order, so this is decided per call from the primary's last call time.
	const uint32_t hr = s.hostRead.load(std::memory_order_acquire);
	const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	const int64_t dt = nowUs - s.hostCallUs.load(std::memory_order_acquire);
	const bool primaryFirst = dt >= 0 && dt < kPeriodUs / 2;
	const uint32_t w = s.written.load(std::memory_order_acquire);
	const uint32_t r = primaryFirst ? hr - 1 : hr;
	t->note(primaryFirst, dt);
	if(!mask || static_cast<int32_t>(w - r) <= 0 || static_cast<int32_t>(w - r) >= mdtap::kSlots - 1) return;
	const auto& blk = s.data[r % mdtap::kSlots];
	int32_t acc[mdtap::kFrames * 2] = {};
	for(int b = 0; b < mdtap::kSources; ++b)
	{
		if(!((mask >> b) & 1)) continue;
		if(b < mdtap::kTracks)
			for(int i = 0; i < frames; ++i) acc[2 * i] += blk[b][i], acc[2 * i + 1] += blk[b][i];
		else
		{
			const int base = b == mdtap::kBitRev ? mdtap::kPlaneRev : mdtap::kPlaneDel;
			for(int i = 0; i < frames; ++i) acc[2 * i] += blk[base][i], acc[2 * i + 1] += blk[base + 1][i];
		}
	}
	for(int i = 0; i < frames * 2; ++i) out[i] = static_cast<int16_t>(std::clamp(acc[i], -32768, 32767));
}

void eRender(void* p, int16_t* out, int frames) { fill(static_cast<Tap*>(p), out, frames); }

// The effect build ("Machinemodule Tap FX", on an MPC return/FX track): its output is the tapped source, plus the input it
// was given when "through" is on, so MPC's own insert effects after it process the Machinedrum's reverb or delay send.
void eProcess(void* p, const int16_t* in, int16_t* out, int frames)
{
	auto* t = static_cast<Tap*>(p);
	fill(t, out, frames);
	if(t->through.load(std::memory_order_relaxed))
		for(int i = 0; i < frames * 2; ++i) out[i] = static_cast<int16_t>(std::clamp(out[i] + in[i], -32768, 32767));
}

const mpc_engine_t kEngine = {eCreate, eDestroy, eMidi, eSet, eGet, eRender, eProcess};

}

extern "C" const mpc_engine_t* mpc_engine(void) { return &kEngine; }
