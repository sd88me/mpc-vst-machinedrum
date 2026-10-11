// The Machinedrum's master effects (rhythm echo, gate box / reverb, EQ, dynamix): the mixer DSP's (DSP1's) master section
// P:$342-$970 plus the output stage ($971-$9a5), run unmodified from the user's OS file in the dsp56300 emulator on the dry main
// mix and the reverb/delay sends that engine/Mixer produces. The section needs no more than the OS gives it: its parameter
// words (Y:$150-$158 and Y:$170-$18c, computed by HostModel from the kit's 32 master bytes with the OS's own code) and its
// state, which lives in DSP memory (delay lines in the external memory). About 9,700 DSP instructions per 32-sample block.
// The Force build does not use it (not enough CPU); the desktop plugin does.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "Firmware.h"
#include "Mixer.h"

namespace dsp56k { class DSP; class Memory; class Peripherals56303; class PeripheralsNop; class DefaultMemoryValidator; }

namespace md::engine
{
	class MasterFx
	{
	public:
		static constexpr int kBlock = Mixer::kBlock;
		static constexpr int kWordsA = 9, kWordsB = 29;	// Y:$150.. and Y:$170..

		explicit MasterFx(const fw::Firmware& _fw);
		~MasterFx();

		// The parameter words (HostModel::masterWordsA/B); kept and written before every block.
		void setParams(const uint32_t* _a9, const uint32_t* _b29);

		// One block. _io holds what Mixer::process made: the dry main mix, the sends and the 6-channel output frames. On return
		// _io.main is the Machinedrum's main output (the dry mix, processed) and _io.frame has the section's output added as the
		// DSP does it. False on a DSP fault (then _io is untouched).
		bool process(Mixer::Output& _io);

		const std::string& fault() const { return m_fault; }
		uint64_t instructionsLastBlock() const { return m_lastInstructions; }

	private:
		bool runUntil(uint32_t _pc, uint64_t _max);

		std::unique_ptr<dsp56k::DefaultMemoryValidator> m_validator;
		std::unique_ptr<dsp56k::Memory> m_mem;
		std::unique_ptr<dsp56k::Peripherals56303> m_periphX;
		std::unique_ptr<dsp56k::PeripheralsNop> m_periphY;
		std::unique_ptr<dsp56k::DSP> m_dsp;
		std::array<uint32_t, kWordsA> m_a{};
		std::array<uint32_t, kWordsB> m_b{};
		uint64_t m_lastInstructions = 0;
		std::string m_fault;
	};
}
