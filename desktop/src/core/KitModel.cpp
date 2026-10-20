#include "KitModel.h"
#include <algorithm>

namespace mm
{
	const char* const kRawKeys[kRaw] = {"syn1", "syn2", "syn3", "syn4", "syn5", "syn6", "syn7", "syn8",
		"amd", "amf", "eqf", "eqg", "fltf", "fltw", "fltq", "srr", "dist", "vol", "pan", "del", "rev", "lfos", "lfod", "lfom"};
	const int kRawDefault[kRaw] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 64, 64, 0, 127, 0, 0, 0, 100, 64, 0, 0, 0, 0, 0};
	const char* const kLfoKeys[kNumLfo] = {"lfo_track", "lfo_param", "lfo_shp1", "lfo_shp2", "lfo_type"};

	// The default kit's master effects (the factory kit TRX UW, as a unit has them after the first-run initialisation)
	const int kMasterDefault[kMasterBytes] = {0x00, 0x00, 0x44, 0x32, 0x01, 0x52, 0x47, 0x6d, 0x10, 0x00, 0x20, 0x1b, 0x00, 0x2c, 0x00, 0x47,
		0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x00, 0x00};

	KitValues KitValues::defaults()
	{
		KitValues v;
		for(int t = 0; t < kTracks; ++t)
		{
			auto& tr = v.track[size_t(t)];
			std::copy_n(kRawDefault, kRaw, tr.raw.begin());
			tr.lfo[0] = t;	// each track's LFO starts on its own track
		}
		std::copy_n(kMasterDefault, kMasterBytes, v.master.begin());
		return v;
	}

	void KitValues::load(const Kit& k, uint32_t keep)
	{
		for(int t = 0; t < kTracks; ++t)
		{
			if((keep >> t) & 1) continue;
			auto& tr = track[size_t(t)];
			tr.machine = k.machine[t];
			for(int i = 0; i < kRaw; ++i) tr.raw[size_t(i)] = k.params[t][i];
			tr.level = k.level[t];
			for(int i = 0; i < kNumLfo; ++i) tr.lfo[size_t(i)] = k.lfo[t][i];
		}
		for(int i = 0; i < kMasterBytes; ++i) master[size_t(i)] = k.master[i];
	}

	// ---- EngineFeeder -----------------------------------------------------------------------------------------------

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

	void EngineFeeder::apply(md::engine::Engine& engine, const KitValues& v, double bpm, Host& host)
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
			h.setMute(t, tr.mute);
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

	// ---- ControlRules -----------------------------------------------------------------------------------------------

	void ControlRules::resync(const KitValues& v)
	{
		for(int t = 0; t < kTracks; ++t)
			for(int k = 0; k < 8; ++k) m_allLast[size_t(t)][size_t(k)] = v.track[size_t(t)].raw[size_t(k)];
	}

	void ControlRules::machineDefaults(int t, const int* syn8)
	{
		for(int k = 0; k < 8; ++k) m_allLast[size_t(t)][size_t(k)] = syn8[k];
	}

	// CTR-AL (the OS's $207e0e): a knob move of delta adds delta to the same SYN on every track that makes sound; the knob itself is kept
	// in 1-126 (the next move is measured from there). CTR-RE / GB / EQ / DX: SYN k is the master effect block's parameter k.
	bool ControlRules::synMoved(KitValues& v, int t, int k)
	{
		const int mid = v.track[size_t(t)].machine;
		const int val = v.track[size_t(t)].raw[size_t(k)];
		if(mid == kCtrAll)
		{
			auto& last = m_allLast[size_t(t)][size_t(k)];
			const int delta = val - last;
			last = std::clamp(val, 1, 126);
			if(delta == 0) return true;
			for(int d = 0; d < kTracks; ++d)
			{
				const int dm = v.track[size_t(d)].machine;
				if(isControlMachine(dm) || (dm & 0xf0) == 0x60) continue;	// CTR and MID tracks make no sound
				if(dm == 160 || dm == 161 || dm == 165 || dm == 166) continue;	// the OS leaves these (SYN 1-8 are not parameters there)
				auto& x = v.track[size_t(d)].raw[size_t(k)];
				x = std::clamp(x + delta, 0, 127);
			}
			return true;
		}
		if(isMasterControl(mid))
		{
			v.master[size_t(kCtrMasterBlock[mid - kCtrEcho] * 8 + k)] = val;
			return true;
		}
		return false;
	}
}
