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
		// DSP1 state after the ColdFire's first-stage loader, read from a running gearmulator-md-mm (OS 1.63), as tools/mdmix/MixerRef
		constexpr TWord kAar[4] = {0x100539, 0x140639, 0x180539, 0x1c0639};
		constexpr TWord kOmr = 0x00490d;
		constexpr TWord kEntry = 0x24;			// sr = $300, jsr $100000 (init), then unmask and wait for the link DMA
		constexpr TWord kAfterInit = 0x2d;
		constexpr TWord kMasterStart = 0x342;	// the master section; the mix before it is engine/Mixer
		constexpr TWord kOutputEnd = 0x9a8;		// after the output stage's last store (a `bra` back to the main loop)
		constexpr TWord kMain = 0x180, kRev = 0x1c0, kDel = 0x600, kFrames = 0x400, kFramePtr = 0x640;
		constexpr TWord kParamA = 0x150, kParamB = 0x170;
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
			cfg.interruptRegionIsCode = true;	// as VoiceEngine: code below $100 is code, not interrupt vectors
			m_dsp->getJit().setConfig(cfg);
		}
		else
			m_dsp->setInterpreterEnabled(true);	// no JIT on 32-bit ARM, see VoiceEngine.cpp
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

		m_dsp->memWriteP(kAfterInit, 0x0c0000 | kAfterInit);	// jmp <$2d: a spin after the init
		m_dsp->setPC(kEntry);
		if(!runUntil(kAfterInit, 100'000'000))
			throw std::runtime_error("mixer DSP init did not complete: " + m_fault);
		m_dsp->memWriteP(kAfterInit, 0x0c0000 | kAfterInit);
	}

	MasterFx::~MasterFx() = default;

	bool MasterFx::runUntil(const uint32_t _pc, const uint64_t _max)
	{
		const auto start = m_dsp->getInstructionCounter();
		while(m_dsp->getPC().toWord() != _pc)
		{
			m_dsp->exec();
			if(m_dsp->getInstructionCounter() - start > _max)
			{
				std::ostringstream o;
				o << "instruction budget exceeded at PC=$" << std::hex << m_dsp->getPC().toWord();
				m_fault = o.str();
				return false;
			}
		}
		m_lastInstructions = m_dsp->getInstructionCounter() - start;
		return true;
	}

	void MasterFx::setParams(const uint32_t* _a9, const uint32_t* _b29)
	{
		std::copy_n(_a9, kWordsA, m_a.begin());
		std::copy_n(_b29, kWordsB, m_b.begin());
	}

	bool MasterFx::process(Mixer::Output& _io)
	{
		auto& d = *m_dsp;
		for(int i = 0; i < kWordsA; ++i) d.memWrite(MemArea_Y, kParamA + static_cast<TWord>(i), m_a[static_cast<size_t>(i)] & 0xffffff);
		for(int i = 0; i < kWordsB; ++i) d.memWrite(MemArea_Y, kParamB + static_cast<TWord>(i), m_b[static_cast<size_t>(i)] & 0xffffff);
		for(int f = 0; f < kBlock; ++f)
		{
			const TWord o = static_cast<TWord>(2 * f);
			for(int c = 0; c < 2; ++c)
			{
				d.memWrite(MemArea_X, kMain + o + static_cast<TWord>(c), static_cast<uint32_t>(_io.main[static_cast<size_t>(f)][static_cast<size_t>(c)]) & 0xffffff);
				d.memWrite(MemArea_X, kRev + o + static_cast<TWord>(c), static_cast<uint32_t>(_io.rev[static_cast<size_t>(f)][static_cast<size_t>(c)]) & 0xffffff);
				d.memWrite(MemArea_X, kDel + o + static_cast<TWord>(c), static_cast<uint32_t>(_io.del[static_cast<size_t>(f)][static_cast<size_t>(c)]) & 0xffffff);
			}
			for(int c = 0; c < 6; ++c)
				d.memWrite(MemArea_X, kFrames + static_cast<TWord>(6 * f + c), static_cast<uint32_t>(_io.frame[static_cast<size_t>(f)][static_cast<size_t>(c)]) & 0xffffff);
		}
		d.memWrite(MemArea_X, kFramePtr, kFrames);
		d.writeReg(Reg_SR, TReg24(d.getSR().toWord() | 0x300));	// interrupts masked: only this code runs
		const TWord saved = d.memory().get(MemArea_P, kOutputEnd);
		d.memWriteP(kOutputEnd, 0x0c0000 | kOutputEnd);	// jmp to itself
		d.setPC(kMasterStart);
		const bool ok = runUntil(kOutputEnd, 10'000'000);
		d.memWriteP(kOutputEnd, saved);
		if(!ok) return false;
		// The main output is X:$180 after the section has run in place on it: the output routine ($270) copies those 64 words to the
		// serial port's DMA buffer (the individual outputs come from the 6-channel frames, where the section adds its output
		// with the opposite sign).
		for(int f = 0; f < kBlock; ++f)
			for(int c = 0; c < 2; ++c)
			{
				const TWord v = d.memory().get(MemArea_X, kMain + static_cast<TWord>(2 * f + c));
				_io.main[static_cast<size_t>(f)][static_cast<size_t>(c)] = static_cast<int32_t>(v << 8) >> 8;
			}
		for(int f = 0; f < kBlock; ++f)
			for(int c = 0; c < 6; ++c)
			{
				const TWord v = d.memory().get(MemArea_X, kFrames + static_cast<TWord>(6 * f + c));
				_io.frame[static_cast<size_t>(f)][static_cast<size_t>(c)] = static_cast<int32_t>(v << 8) >> 8;
			}
		return true;
	}
}
