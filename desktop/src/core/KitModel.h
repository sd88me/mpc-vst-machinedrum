// The Machinedrum's sound as a plain table of kit values, and the rules that turn it into engine calls, without a plugin framework:
// shared by the desktop plugin (desktop/src/plugin, values in JUCE parameters) and the Schwung module (schwung/, values in its own
// param table). What the OS does with the values is the engine's; what is here is what the OS does around them (EngineFeeder.h
// sends a table to an engine):
//   ControlRules   the control machines that move other values: CTR-AL (a SYN knob move shifts that SYN on every sounding track) and
//                  CTR-RE / GB / EQ / DX (SYN k is the master effect block's parameter k)
#pragma once
#include <array>
#include <cstdint>

#include "KitCodec.h"

namespace mm
{
	// The 24 per-track kit values in HostModel's raw order, and their parameter id suffixes.
	constexpr int kRaw = 24;
	extern const char* const kRawKeys[kRaw];
	extern const int kRawDefault[kRaw];
	extern const char* const kLfoKeys[kNumLfo];
	constexpr int kMasterBytes = 32;	// the kit's master effects: reverb 8, echo 8, EQ 8, dynamix 8
	extern const int kMasterDefault[kMasterBytes];

	// The control machines (MachineInfo::id)
	constexpr int kCtrAll = 112, kCtr8P = 113, kCtrEcho = 120, kCtrDynamix = 123;
	constexpr bool isControlMachine(int id) { return (id & 0xf0) == 0x70; }	// CTR: no audio, never the target of another control machine
	constexpr int kCtrMasterBlock[4] = {1, 0, 2, 3};	// CTR-RE / GB / EQ / DX -> the kit's master block (reverb 0, echo 1, EQ 2, dynamix 3), as the OS's table at $2462e3
	constexpr bool isMasterControl(int id) { return id >= kCtrEcho && id <= kCtrDynamix; }

	struct TrackValues
	{
		int machine = 0;
		std::array<int, kRaw> raw{};
		int level = 100;
		bool mute = false;
		bool solo = false;	// while any track is soloed, the tracks that are not are silent (as a mute); not a kit value
		int muteGroup = 0;	// the kit's MUTE GROUP: 0 none, 1-16 = the track a trigger of this one silences
		std::array<int, kNumLfo> lfo{};
	};

	struct KitValues
	{
		std::array<TrackValues, kTracks> track{};
		std::array<int, kMasterBytes> master{};
		bool masterOn = true;

		static KitValues defaults();	// what a new instance starts with: every track GND, the default master effects
		// A kit's values into the table; tracks whose bit is set in `keep` are left as they are (a locked track)
		void load(const Kit&, uint32_t keep = 0);
	};

	class ControlRules
	{
	public:
		// The table was replaced (a kit or a saved state): remember the CTR-AL knobs as they are, without acting on them.
		void resync(const KitValues& v);
		// Track t's SYN k (0-7) now holds a new value in v: apply its control machine's rule, editing v (other tracks' SYN values,
		// the master effects). Returns false when t is not a control machine that acts on SYN moves.
		bool synMoved(KitValues& v, int t, int k);
		// A track's machine was set to a control machine with these starting values: they are the knob positions from now on.
		void machineDefaults(int t, const int* syn8);

	private:
		std::array<std::array<int, 8>, kTracks> m_allLast{};	// each CTR-AL knob as the OS keeps it (1-126)
	};
}
