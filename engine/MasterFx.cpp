#include "MasterFx.h"

#include <sstream>
#include <stdexcept>

#include "dsp56kEmu/audio.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/jit.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

namespace md::engine
{
	using namespace dsp56k;

	namespace
	{
		constexpr TWord kSizeP = 0x800000, kSizeXY = 0x800000, kBridge = 0x020000;
		// DSP1 state after the ColdFire's first-stage loader, read from a running gearmulator-md-mm (OS 1.63)
		constexpr TWord kAar[4] = {0x100539, 0x140639, 0x180539, 0x1c0639};
		constexpr TWord kOmr = 0x00490d;
		constexpr TWord kEntry = 0x24, kAfterInit = 0x2d;
		constexpr TWord kMasterStart = 0x342;	// after the mix (P:$294-$341), which engine/Mixer does natively
		constexpr TWord kBlockEnd = 0x9a8;		// 'bra $3c' back to the block loop: parked instead
		constexpr TWord kMainDry = 0x180, kReverbSend = 0x1c0, kDelaySend = 0x600;	// X: 32 interleaved L/R pairs each
		constexpr TWord kFrameBuffer = 0x400, kFrameBase = 0x640;	// X: 32 frames x 6 channels; the pointer the output stage reads
		constexpr int kMainLeft = 2, kMainRight = 5;	// the main outputs' channels in a frame
		constexpr uint64_t kMaxInit = 100'000'000, kMaxBlock = 4'000'000;
	}

	const MasterFxSection& masterFxSection(const MasterFxId _fx)
	{
		static const MasterFxSection sections[kNumMasterFx] = {
			{0x20B940, 0x1000D9C, 0x1001B74, 0x185, 8},		// reverb (Gate Box)
			{0x20B4AE, 0x1000DAC, 0x1001AF4, 0x150, 9},		// delay (Rhythm Echo)
			{0x20B74E, 0x1000DBC, 0x1001B44, 0x170, 10},	// EQ
			{0x20B604, 0x1000DCC, 0x1001B18, 0x17A, 11},	// dynamics
		};
		return sections[static_cast<int>(_fx)];
	}

	bool convertMasterFx(MachineRunner& _os, const MasterFxId _fx, const uint8_t* _knobs, std::array<uint32_t, 16>& _words)
	{
		constexpr uint32_t kJoin = 0x20BA4C;	// where the effects' sections meet again in the tick
		const auto& s = masterFxSection(_fx);
		for(uint32_t k = 0; k < 8; ++k)
			_os.poke16(s.rawAddr + 2 * k, static_cast<uint16_t>(_knobs[k] << 7));
		if(!_os.runTo(s.entry, kJoin))
			return false;
		_words.fill(0);
		for(int k = 0; k < s.words && k < 16; ++k)
			_words[static_cast<size_t>(k)] = _os.peek32(s.mirror + 4 * static_cast<uint32_t>(k)) & 0xffffff;
		return true;
	}

	MasterFx::MasterFx(const fw::Firmware& _fw)
	{
		m_validator = std::make_unique<DefaultMemoryValidator>();
		m_mem = std::make_unique<Memory>(*m_validator, kSizeP, kSizeXY, kBridge);
		m_periphX = std::make_unique<Peripherals56303>();
		m_periphY = std::make_unique<PeripheralsNop>();
		m_dsp = std::make_unique<DSP>(*m_mem, m_periphX.get(), m_periphY.get());
		if constexpr(g_useJIT)
		{
			auto cfg = m_dsp->getJit().getConfig();
			cfg.dynamicFastInterrupts = true;
			cfg.aguSupportBitreverse = true;
			cfg.linkJitBlocks = false;
			cfg.interruptRegionIsCode = true;
			m_dsp->getJit().setConfig(cfg);
		}
		else
		{
			m_dsp->setInterpreterEnabled(true);	// see VoiceEngine: no JIT on 32-bit ARM
		}
		for(auto* essi : {&m_periphX->getEssi0(), &m_periphX->getEssi1()})
		{
			essi->setReadRxCallback([](uint64_t& _i, Audio::RxFrame& _f) { _f.resize(2); _f[0] = {}; _f[1] = {}; ++_i; });
			essi->setWriteTxCallback([](uint64_t& _i, const Audio::TxFrame&) { ++_i; });
		}

		m_dsp->resetHW();
		for(const auto& r : _fw.dspB.records)	// section 2 = mixer program
			for(size_t k = 0; k < r.words.size(); ++k)
			{
				const TWord a = r.addr + static_cast<TWord>(k);
				if(r.space == fw::Space::P || a >= kBridge)
					m_dsp->memWriteP(a, r.words[k]);
				else
					m_dsp->memWrite(r.space == fw::Space::X ? MemArea_X : MemArea_Y, a, r.words[k]);
			}
		for(int i = 0; i < 4; ++i)
			m_periphX->write(0xfffff9 - static_cast<TWord>(i), kAar[i]);
		m_dsp->regs().omr.var = kOmr;

		m_dsp->memWriteP(kAfterInit, 0x0c0000 | kAfterInit);	// the program's init ends in a wait for the link: spin here
		m_dsp->setPC(kEntry);
		{
			const auto start = m_dsp->getInstructionCounter();
			while(m_dsp->getPC().toWord() != kAfterInit)
			{
				m_dsp->exec();
				if(m_dsp->getInstructionCounter() - start > kMaxInit)
					throw std::runtime_error("mixer DSP init did not complete");
			}
		}
		m_dsp->memWriteP(kBlockEnd, 0x0c0000 | kBlockEnd);	// park at the end of the block
	}

	MasterFx::~MasterFx() = default;

	void MasterFx::setParams(const MasterFxId _fx, const std::array<uint32_t, 16>& _words)
	{
		const auto& s = masterFxSection(_fx);
		for(int k = 0; k < s.words; ++k)
			m_dsp->memWrite(MemArea_Y, s.dspAddr + static_cast<TWord>(k), _words[static_cast<size_t>(k)] & 0xffffff);
	}

	bool MasterFx::runToPark(const uint64_t _max)
	{
		const auto start = m_dsp->getInstructionCounter();
		while(m_dsp->getPC().toWord() != kBlockEnd)
		{
			m_dsp->exec();
			if(m_dsp->getInstructionCounter() - start > _max)
			{
				std::ostringstream o;
				o << "master section: instruction budget exceeded at PC=$" << std::hex << m_dsp->getPC().toWord();
				m_fault = o.str();
				return false;
			}
		}
		m_lastInstructions = m_dsp->getInstructionCounter() - start;
		return true;
	}

	bool MasterFx::process(const Mixer::Output& _mix, Stereo& _out)
	{
		if(!m_fault.empty())
		{
			for(auto& f : _out) f = {0, 0};
			return false;
		}
		for(int i = 0; i < kFrames; ++i)
		{
			const auto k = static_cast<TWord>(2 * i);
			for(int c = 0; c < 2; ++c)
			{
				m_dsp->memWrite(MemArea_X, kMainDry + k + static_cast<TWord>(c), static_cast<TWord>(_mix.main[static_cast<size_t>(i)][static_cast<size_t>(c)]) & 0xffffff);
				m_dsp->memWrite(MemArea_X, kReverbSend + k + static_cast<TWord>(c), static_cast<TWord>(_mix.rev[static_cast<size_t>(i)][static_cast<size_t>(c)]) & 0xffffff);
				m_dsp->memWrite(MemArea_X, kDelaySend + k + static_cast<TWord>(c), static_cast<TWord>(_mix.del[static_cast<size_t>(i)][static_cast<size_t>(c)]) & 0xffffff);
			}
			for(int c = 0; c < 6; ++c)
				m_dsp->memWrite(MemArea_X, kFrameBuffer + static_cast<TWord>(6 * i + c), static_cast<TWord>(_mix.frame[static_cast<size_t>(i)][static_cast<size_t>(c)]) & 0xffffff);
		}
		m_dsp->memWrite(MemArea_X, kFrameBase, kFrameBuffer);
		for(int i = 0; i < 8; ++i)
			m_dsp->writeReg(static_cast<EReg>(Reg_M0 + i), TReg24(0xffffff));
		m_dsp->writeReg(Reg_SR, TReg24(m_dsp->getSR().toWord() | 0x300));	// interrupts masked: only this section runs
		m_dsp->setPC(kMasterStart);
		if(!runToPark(kMaxBlock))
		{
			for(auto& f : _out) f = {0, 0};
			return false;
		}
		for(int i = 0; i < kFrames; ++i)
		{
			const auto l = m_mem->get(MemArea_X, kFrameBuffer + static_cast<TWord>(6 * i + kMainLeft)) & 0xffffff;
			const auto r = m_mem->get(MemArea_X, kFrameBuffer + static_cast<TWord>(6 * i + kMainRight)) & 0xffffff;
			_out[static_cast<size_t>(i)] = {static_cast<int32_t>(l << 8) >> 8, static_cast<int32_t>(r << 8) >> 8};
		}
		return true;
	}
}
