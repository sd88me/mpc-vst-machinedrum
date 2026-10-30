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
			tr.muteGroup = k.muteGroup[t] + 1;
			for(int i = 0; i < kNumLfo; ++i) tr.lfo[size_t(i)] = k.lfo[t][i];
		}
		for(int i = 0; i < kMasterBytes; ++i) master[size_t(i)] = k.master[i];
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
