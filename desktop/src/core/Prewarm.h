// Plays every machine once on track 1 so the dsp56300 JIT compiles its code now, not on the machine's first hit (measured on a Move:
// a first hit costs 15-18 ms, a dropout at any latency; much less on a desktop CPU, but still a block or more). Then back to the empty
// machine, and enough blocks for the tail to be released (or to decay). Run it on the thread that builds the engine, before it plays.
// Used by the Schwung module's engine process. Not by the desktop plugin: the engine is then no longer a fresh one (time, LFOs, the
// voice DSP's state have moved on), and the desktop keeps the fresh engine's exact audio (mm-render checks it). For that it would need
// a reset that keeps the compiled code, as Monomodule's port has (MonoVoice::resetKeepCode).
#pragma once
#include <cstdlib>
#include <string>

#include "MdEngine.h"

namespace mm
{
	// Offered: all but MID (no MIDI out), RAM (no sampling) and the empty ROM slots above 32; the ROM machines only with samples.
	// withInput: the INP machines too (the desktop plugin has an audio input; a Schwung sound generator does not).
	template<class E> int prewarmMachines(E& _eng, const bool _romSamples, const bool _withInput)
	{
		auto& h = _eng.host();
		typename E::Output o;
		int warmed = 0;
		for(const auto& m : _eng.os().machines())
		{
			const std::string g = m.name.substr(0, 3);
			if(m.name.empty() || g == "MID" || g == "RAM" || (g == "INP" && !_withInput)) continue;
			if(g == "ROM" && (!_romSamples || std::atoi(m.name.c_str() + 3) > 32)) continue;
			h.setMachine(0, m.id);
			h.trigger(0, 127, false);
			for(int b = 0; b < 8; ++b) _eng.render(o);
			++warmed;
		}
		h.setMachine(0, 0);
		h.trigger(0, 1, false);
		for(int b = 0; b < 600; ++b) _eng.render(o);	// 0.4 s
		return warmed;
	}
}
