#include "EngineFeeder.h"
#include <algorithm>

namespace mm
{
	void EngineFeeder::reset()
	{
		for(int t = 0; t < kTracks; ++t)
		{
			m_machine[t] = -1;
			m_level[t] = -1;
			for(int i = 0; i < kRaw; ++i) m_raw[t][i] = -1;
			for(int i = 0; i < kNumLfo; ++i) m_lfo[t][i] = -1;
		}
		m_bpm = -1;
		for(int& v : m_master) v = -1;
		m_masterSnap = true;
	}

	template<class E> void EngineFeeder::apply(E& engine, const KitValues& v, double bpm, Host& host)
	{
		auto& h = engine.host();
		bpm = std::clamp(bpm, 30.0, 300.0);
		if(bpm != m_bpm) { m_bpm = bpm; h.setTempo(bpm); }
		engine.setMasterBypass(!v.masterOn);
		{
			uint8_t bytes[kMasterBytes];
			bool changed = false;
			for(int i = 0; i < kMasterBytes; ++i)
			{
				bytes[i] = uint8_t(std::clamp(v.master[size_t(i)], 0, 127));
				changed |= bytes[i] != m_master[i];
				m_master[i] = bytes[i];
			}
			// a change is a target for the OS's smoother (the master effects glide to it, as on the unit); the very first set settles at once
			if(changed) { engine.setMasterFx(bytes, m_masterSnap); m_masterSnap = false; }
		}
		bool anySolo = false;
		for(const auto& t : v.track) anySolo |= t.solo;
		int mach[kTracks];
		for(int t = 0; t < kTracks; ++t) mach[t] = std::clamp(v.track[size_t(t)].machine, 0, 191);
		auto rawNow = [&](int t, int i) { return std::clamp(v.track[size_t(t)].raw[size_t(i)], 0, 127); };
		// CTR-8P: P1-P8 (its SYN1-8) are written, every tick on the unit, into the (track, parameter) targets its parameters 9-24 name
		// (track 1-16 and parameter 1-24 as the OS keeps them, clamped), except into control machines.
		int target[kTracks][kRaw];
		for(auto& row : target) for(int& x : row) x = -1;
		for(int c = 0; c < kTracks; ++c)
		{
			if(mach[c] != kCtr8P) continue;
			for(int k = 0; k < 8; ++k)
			{
				const int dt = std::min(rawNow(c, 8 + 2 * k), kTracks - 1), dp = std::min(rawNow(c, 9 + 2 * k), kRaw - 1);
				if(!isControlMachine(mach[dt])) target[dt][dp] = rawNow(c, k);
			}
		}
		for(int t = 0; t < kTracks; ++t)
		{
			const auto& tr = v.track[size_t(t)];
			const int m = mach[t];
			if(m != m_machine[t])
			{
				m_machine[t] = m;
				h.setMachine(t, uint8_t(m));	// raw SYN1-8 take the machine's defaults
				if(host.takeDefaults(t))
				{
					int count = 8, syn[kRaw];
					for(int i = 0; i < 8; ++i) syn[i] = h.param(t, i);
					if(isMasterControl(m))	// as the OS assigns them: SYN1-8 start at the master block's current values
						for(int i = 0; i < 8; ++i) syn[i] = std::clamp(v.master[size_t(kCtrMasterBlock[m - kCtrEcho] * 8 + i)], 0, 127);
					else if(m == kCtr8P)	// P1-P8 -> SYN1 of tracks 1-8 (the OS's own assignment), starting at their current values
					{
						count = kRaw;
						for(int k = 0; k < 8; ++k)
						{
							syn[8 + 2 * k] = k;
							syn[9 + 2 * k] = 0;
							syn[k] = rawNow(k, 0);
						}
					}
					for(int i = 0; i < count; ++i) { m_raw[t][i] = syn[i]; if(count > 8 || isMasterControl(m)) h.setParam(t, i, syn[i]); }
					host.defaultsChosen(t, syn, count);
				}
				else
					for(int i = 0; i < 8; ++i) m_raw[t][i] = -1;	// the table's own SYN values win (a kit or a restored state)
			}
			for(int i = 0; i < kRaw; ++i)
			{
				const int x = target[t][i] >= 0 ? target[t][i] : rawNow(t, i);
				if(x != m_raw[t][i]) { m_raw[t][i] = x; h.setParam(t, i, x); }
			}
			const int lev = std::clamp(tr.level, 0, 127);
			if(lev != m_level[t]) { m_level[t] = lev; h.setLevel(t, lev); }
			h.setMute(t, tr.mute || (anySolo && !tr.solo));
			h.setMuteGroup(t, tr.muteGroup - 1);
			int l[kNumLfo];
			bool changed = false;
			for(int i = 0; i < kNumLfo; ++i)
			{
				l[i] = std::clamp(tr.lfo[size_t(i)], 0, kLfoMax[i]);
				changed |= l[i] != m_lfo[t][i];
				m_lfo[t][i] = l[i];
			}
			if(changed) h.setLfo(t, l[0], l[1], l[2], l[3], l[4]);
		}
	}

	template void EngineFeeder::apply(md::engine::Engine&, const KitValues&, double, Host&);
	template void EngineFeeder::apply(md::engine::ParallelEngine&, const KitValues&, double, Host&);
}
