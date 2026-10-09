// Machinedrum kit sysex ($52, 1233 bytes: header, name, 16 x 24 track params, 16 levels, then 7-bit packed machines and
// LFO settings). Same layout as the MPC plugin's vst/engine.cpp (see HANDOFF.md, "kits"), without the folder scanning.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mm
{
	constexpr int kTracks = 16;
	constexpr int kNumLfo = 5;
	constexpr int kLfoMax[kNumLfo] = {15, 23, 5, 5, 2};

	struct Kit
	{
		char name[17]{};
		uint8_t machine[kTracks]{};
		uint8_t params[kTracks][24]{};	// HostModel raw order: SYN1-8, AMD AMF EQF EQG FLTF FLTW FLTQ SRR, DIST VOL PAN DEL REV LFOS LFOD LFOM
		uint8_t level[kTracks]{};
		uint8_t lfo[kTracks][kNumLfo]{};	// destination track, destination param, shape 1, shape 2, update
	};

	// Elektron's 7-bit packing: each group of up to 7 bytes is preceded by a byte holding their top bits
	inline std::vector<uint8_t> decode7(const uint8_t* p, size_t n)
	{
		std::vector<uint8_t> out;
		for(size_t i = 0; i < n; i += 8)
			for(size_t j = 1; j < 8 && i + j < n; ++j)
				out.push_back(static_cast<uint8_t>(p[i + j] | ((p[i] << j) & 0x80)));
		return out;
	}

	inline bool parseKit(const uint8_t* m, size_t n, Kit& k)
	{
		static constexpr uint8_t kHeader[] = {0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x52};
		if(n < 0x4d1 || std::memcmp(m, kHeader, sizeof kHeader)) return false;
		if(m[0x0a] == 0x7f || m[0x0a] == 0) return false;	// an empty slot
		k = Kit{};
		for(int i = 0; i < 16 && m[0x0a + i] >= 0x20 && m[0x0a + i] < 0x7f; ++i) k.name[i] = static_cast<char>(m[0x0a + i]);
		for(int t = 0; t < kTracks; ++t)
		{
			for(int i = 0; i < 24; ++i) k.params[t][i] = m[0x1a + t * 24 + i] & 0x7f;
			k.level[t] = m[0x19a + t] & 0x7f;
		}
		const auto mach = decode7(m + 0x1aa, 74);
		const auto lfo = decode7(m + 0x1f4, 664);
		if(mach.size() < 64 || lfo.size() < kTracks * 36) return false;
		for(int t = 0; t < kTracks; ++t)
		{
			k.machine[t] = mach[t * 4 + 3];	// a 32-bit big-endian word; the machine id is its low byte
			for(int i = 0; i < kNumLfo; ++i) k.lfo[t][i] = static_cast<uint8_t>(std::clamp<int>(lfo[t * 36 + i], 0, kLfoMax[i]));
		}
		return true;
	}

	// Every kit in a .syx file (it may hold several sysex messages).
	inline std::vector<Kit> parseKits(const std::vector<uint8_t>& data)
	{
		std::vector<Kit> kits;
		for(size_t i = 0; i < data.size(); ++i)
		{
			if(data[i] != 0xf0) continue;
			size_t e = i + 1;
			while(e < data.size() && data[e] != 0xf7) ++e;
			Kit k;
			if(e < data.size() && parseKit(&data[i], e - i + 1, k)) kits.push_back(k);
			i = e;
		}
		return kits;
	}
}
