// The ROM machines' sample memory from a dump of the emulated MD's voice DSP (tools/mdkits/mdsamples.cpp for release builds,
// vst/engine.cpp on the device, from mdextract's dump): the words above the OS program ($140000) that differ from what
// VoiceEngine loaded itself, as records [u32 address][u32 count][count x u32 word]. The MD writes the sample directory
// ($147e00-$147f08, 4 words per slot: start, length, loop, flags) and the sample data after it ($150000-) there at boot;
// lower differences are the program's runtime state, not sample data.
#pragma once
#include <cstdint>
#include <vector>

namespace md::romsamples
{
	constexpr uint32_t kProgEnd = 0x140000;

	struct Run { uint32_t address; std::vector<uint32_t> words; };

	// dump[i] is P word (base + i); own(a) is the engine's own P word a (24 bits compared)
	template<class Own>
	std::vector<Run> diff(const std::vector<uint32_t>& _dump, uint32_t _base, uint32_t _sizeP, Own _own)
	{
		std::vector<Run> runs;
		const uint32_t n = _base + static_cast<uint32_t>(_dump.size());
		const auto differs = [&](uint32_t a) { return (_dump[a - _base] & 0xffffff) != (_own(a) & 0xffffff); };
		for(uint32_t a = kProgEnd > _base ? kProgEnd : _base; a < n && a < _sizeP;)
		{
			if(!differs(a)) { ++a; continue; }
			uint32_t e = a;
			while(e < n && e < _sizeP && differs(e)) ++e;
			runs.push_back({a, std::vector<uint32_t>(_dump.begin() + (a - _base), _dump.begin() + (e - _base))});
			a = e;
		}
		return runs;
	}
}
