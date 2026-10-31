// The Machinedrum's master effects (Gate Box reverb, Rhythm Echo delay, EQ, dynamix): the master section of the mixer DSP
// (DSP1, section 2 of the OS file, P:$342-$9A8) run as it is on the dsp56300 emulator, on what engine/Mixer produces (the dry
// main mix and the reverb and delay sends). The track effects and the mix stay native C++ (engine/TrackFx, Mixer): this is the
// part left to the real program, about 9,700 DSP instructions a 32-sample block. Its parameters are the words the OS's ColdFire
// code makes of the kit's 4 x 8 master knobs: convertMasterFx runs that code (the sections of the OS tick, MainOS 1.63).
// Adapted from ModuleDrum (https://github.com/K-X420/ModuleDrum, AGPL-3.0, KX): which sections, addresses and DSP patches.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "Mixer.h"
#include "MachineRunner.h"
#include "../tools/mdfw/Firmware.h"

namespace dsp56k { class DSP; class Memory; class Peripherals56303; class PeripheralsNop; class DefaultMemoryValidator; }

namespace md::engine
{
	enum class MasterFxId { Reverb = 0, Delay, Eq, Dynamics };
	constexpr int kNumMasterFx = 4;

	// Where the OS keeps one effect: the entry of its conversion section, the raw knob words (8 x u16, value << 7), the
	// parameter words it leaves (u32 each, 24-bit) and where DSP1 reads them (Y:) and how many.
	struct MasterFxSection { uint32_t entry, rawAddr, mirror, dspAddr; int words; };
	const MasterFxSection& masterFxSection(MasterFxId _fx);

	// The OS's own conversion of one effect's 8 knob values (0-127) into DSP1's parameter words. False if the OS code faults.
	bool convertMasterFx(MachineRunner& _os, MasterFxId _fx, const uint8_t* _knobs, std::array<uint32_t, 16>& _words);

	class MasterFx
	{
	public:
		static constexpr int kFrames = Mixer::kBlock;
		using Stereo = Mixer::Stereo;

		explicit MasterFx(const fw::Firmware& _fw);	// throws if the DSP does not boot
		~MasterFx();

		// The words convertMasterFx made, to DSP1's parameter area.
		void setParams(MasterFxId _fx, const std::array<uint32_t, 16>& _words);

		// One block: _mix is what Mixer::process made. _out gets the main outputs after the master section (24-bit, the
		// hardware's own level: the dry mix comes out about x5.76 at neutral settings). False on a fault (_out is silent).
		bool process(const Mixer::Output& _mix, Stereo& _out);

		uint64_t instructionsLastBlock() const { return m_lastInstructions; }
		const std::string& faultReason() const { return m_fault; }
		dsp56k::DSP& dsp() { return *m_dsp; }

	private:
		bool runToPark(uint64_t _max);

		std::unique_ptr<dsp56k::DefaultMemoryValidator> m_validator;
		std::unique_ptr<dsp56k::Memory> m_mem;
		std::unique_ptr<dsp56k::Peripherals56303> m_periphX;
		std::unique_ptr<dsp56k::PeripheralsNop> m_periphY;
		std::unique_ptr<dsp56k::DSP> m_dsp;
		uint64_t m_lastInstructions = 0;
		std::string m_fault;
	};
}
