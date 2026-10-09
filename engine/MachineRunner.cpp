#include "MachineRunner.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "mc68k/mc68k.h"
#include "mc68k/cpuState.h"
#include "Musashi/m68k.h"

namespace md::engine
{
	namespace
	{
		constexpr uint32_t kMainBase = 0x200000, kMainSize = 0x100000;		// main RAM (OS loaded here)
		constexpr uint32_t kSramBase = 0x01000000, kSramSize = 0x10000;		// ColdFire internal SRAM
		constexpr uint32_t kSramImage = 0x2622f4;							// the OS copies its fast routines from here
		constexpr uint32_t kScratch = 0x0100e000;							// our args/stack area (unused by the OS routines)
		constexpr uint32_t kParams = kScratch, kOut = kScratch + 0x100;
		constexpr uint32_t kStackTop = kSramBase + kSramSize - 0x10;
		constexpr uint32_t kReturnSentinel = 0x00000100;	// unmapped, never executed: we stop when PC gets here
		constexpr uint64_t kMaxInstructions = 2'000'000;
		constexpr size_t kRecordSize = 86;
	}

	class MachineCpu final : public mc68k::Mc68k
	{
	public:
		MachineCpu() : Mc68k(M68K_CPU_TYPE_MCF5206E), m_main(kMainSize, 0), m_sram(kSramSize, 0) {}

		uint8_t read8(const uint32_t _addr) override { if(const auto* p = ptr(_addr, 1)) return *p; bad(_addr, 1, false); return 0; }
		uint16_t read16(const uint32_t _addr) override { if(const auto* p = ptr(_addr, 2)) return static_cast<uint16_t>(p[0] << 8 | p[1]); bad(_addr, 2, false); return 0; }
		uint16_t readImm16(const uint32_t _addr) override { return read16(_addr); }
		void write8(const uint32_t _addr, const uint8_t _val) override { if(auto* p = ptr(_addr, 1)) *p = _val; else bad(_addr, 1, true); }
		void write16(const uint32_t _addr, const uint16_t _val) override
		{
			if(auto* p = ptr(_addr, 2)) { p[0] = static_cast<uint8_t>(_val >> 8); p[1] = static_cast<uint8_t>(_val); } else bad(_addr, 2, true);
		}
		void bad(const uint32_t _addr, const int _n, const bool _write)
		{
			if(m_badAccesses < 8 && std::getenv("MD_BADLOG")) std::fprintf(stderr, "bad %s%d @ %08x pc=%08x\n", _write ? "write" : "read", _n * 8, _addr, m68k_get_reg(getCpuState(), M68K_REG_PC));
			++m_badAccesses;
		}
		uint32_t exec() override { return execInstruction(); }	// CPU only: no on-chip peripherals

		void setAReg(const int _i, const uint32_t _v) { m68k_set_reg(getCpuState(), static_cast<m68k_register_t>(M68K_REG_A0 + _i), _v); }
		void setSR(const uint32_t _v) { m68k_set_reg(getCpuState(), M68K_REG_SR, _v); }

		uint8_t* ptr(const uint32_t _addr, const uint32_t _n)
		{
			if(_addr >= kMainBase && _addr + _n <= kMainBase + kMainSize) return &m_main[_addr - kMainBase];
			if(_addr >= kSramBase && _addr + _n <= kSramBase + kSramSize) return &m_sram[_addr - kSramBase];
			return nullptr;
		}

		std::vector<uint8_t> m_main, m_sram;
		uint32_t m_badAccesses = 0;
	};
}

#define MC68K_CLASS md::engine::MachineCpu
#include "mc68k/musashiEntry.h"

namespace md::engine
{
	MachineRunner::MachineRunner(std::vector<uint8_t> _osImage) : m_os(std::move(_osImage))
	{
		if(m_os.size() > kMainSize) throw std::runtime_error("OS image too large");
		m_index.fill(-1);
		m_cpu = std::make_unique<MachineCpu>();
		std::memcpy(m_cpu->m_main.data(), m_os.data(), m_os.size());
		if(m_os.size() > kSramImage - kMainBase)
		{
			const size_t n = std::min<size_t>(m_os.size() - (kSramImage - kMainBase), kSramSize);
			std::memcpy(m_cpu->m_sram.data(), m_os.data() + (kSramImage - kMainBase), n);
		}
		// Runtime data the OS's boot writes into its internal SRAM (read from the booted full-system emulation, not in the
		// OS file). The ROM machines' coefficient function divides by the word at $0100150c ($bb8); left at 0 it takes a
		// divide-by-zero, returns nothing, and every ROM track played the same, glitchy, all-zero slot (2026-09-29).
		{
			static const uint8_t kBootSram[16] = {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 0, 0, 0x0b, 0xb8};
			std::memcpy(m_cpu->m_sram.data() + 0x1500, kBootSram, sizeof kBootSram);
		}
		m_cpu->setSR(0x2700);	// supervisor, interrupts masked
		parseMachineTable();
	}

	MachineRunner::~MachineRunner() = default;

	uint8_t MachineRunner::peek8(const uint32_t _a) const { const auto* p = m_cpu->ptr(_a, 1); return p ? *p : 0; }
	uint16_t MachineRunner::peek16(const uint32_t _a) const { const auto* p = m_cpu->ptr(_a, 2); return p ? static_cast<uint16_t>(p[0] << 8 | p[1]) : 0; }
	uint32_t MachineRunner::peek32(const uint32_t _a) const { return static_cast<uint32_t>(peek16(_a)) << 16 | peek16(_a + 2); }
	void MachineRunner::poke8(const uint32_t _a, const uint8_t _v) { m_cpu->write8(_a, _v); }
	void MachineRunner::poke16(const uint32_t _a, const uint16_t _v) { m_cpu->write16(_a, _v); }
	void MachineRunner::poke32(const uint32_t _a, const uint32_t _v) { poke16(_a, static_cast<uint16_t>(_v >> 16)); poke16(_a + 2, static_cast<uint16_t>(_v)); }

	void MachineRunner::parseMachineTable()
	{
		// The descriptor table (86-byte records) is found by its shape around the TRX-B2 record's name.
		auto valid = [&](const size_t _o)
		{
			if(_o + kRecordSize > m_os.size()) return false;
			const uint32_t fn = static_cast<uint32_t>(m_os[_o]) << 24 | m_os[_o + 1] << 16 | m_os[_o + 2] << 8 | m_os[_o + 3];
			if(fn < kOsBase || fn >= kOsBase + m_os.size()) return false;
			for(size_t k = 5; k < 10; ++k)
				if(m_os[_o + k] != 0 && (m_os[_o + k] < 32 || m_os[_o + k] >= 127)) return false;
			return true;
		};
		static const char kAnchor[] = "TRXB2PTCH";
		const auto* hit = std::search(m_os.data(), m_os.data() + m_os.size(), kAnchor, kAnchor + sizeof(kAnchor) - 1);	// not memmem: absent on MSVC
		if(hit == m_os.data() + m_os.size() || hit - m_os.data() < 5) throw std::runtime_error("machine table not found (not an MD OS 1.63 image?)");
		size_t start = static_cast<size_t>(hit - m_os.data()) - 5;
		while(start >= kRecordSize && valid(start - kRecordSize)) start -= kRecordSize;
		for(size_t o = start; valid(o); o += kRecordSize)
		{
			MachineInfo m;
			m.function = static_cast<uint32_t>(m_os[o]) << 24 | m_os[o + 1] << 16 | m_os[o + 2] << 8 | m_os[o + 3];
			m.id = m_os[o + 4];
			for(size_t k = 5; k < 10 && m_os[o + k]; ++k) m.name += static_cast<char>(m_os[o + k]);
			for(size_t p = 0; p < 8; ++p)
			{
				for(size_t k = 0; k < 4 && m_os[o + 10 + 4 * p + k]; ++k) m.params[p] += static_cast<char>(m_os[o + 10 + 4 * p + k]);
				m.defaults[p] = m_os[o + 42 + p];
			}
			m_index[m.id] = static_cast<int>(m_machines.size());
			m_machines.push_back(std::move(m));
		}
	}

	const MachineInfo* MachineRunner::machine(const uint8_t _id) const
	{
		return m_index[_id] < 0 ? nullptr : &m_machines[static_cast<size_t>(m_index[_id])];
	}

	int64_t MachineRunner::call(const uint32_t _address, const std::initializer_list<uint32_t> _args)
	{
		m_fault.clear();
		auto& cpu = *m_cpu;
		uint32_t sp = kStackTop;
		// C calling convention: arguments pushed right to left, then the return address
		std::vector<uint32_t> args(_args);
		for(auto it = args.rbegin(); it != args.rend(); ++it) { sp -= 4; poke32(sp, *it); }
		sp -= 4; poke32(sp, kReturnSentinel);
		cpu.setAReg(7, sp);
		cpu.setPC(_address);
		cpu.m_badAccesses = 0;
		uint64_t n = 0;
		while(cpu.getPC() != kReturnSentinel)
		{
			cpu.exec();
			if(++n > kMaxInstructions) { m_fault = "instruction budget exceeded"; return -1; }
		}
		m_lastInstructions = n;
		if(cpu.m_badAccesses) { m_fault = "access outside main RAM / internal SRAM"; return -1; }
		return cpu.getDReg(0);
	}

	int MachineRunner::compute(const uint8_t _machineId, const uint16_t* _params, uint32_t* _out, const int _outCapacity, const bool _trigger)
	{
		const auto* m = machine(_machineId);
		if(!m) { m_fault = "unknown machine"; return -1; }
		for(uint32_t k = 0; k < 24; ++k)
			poke16(kParams + 2 * k, k < 8 ? _params[k] : 0);
		for(uint32_t k = 0; k < 32; ++k)
			poke32(kOut + 4 * k, 0);
		poke32(kOut, _trigger ? 1 : 0);
		const auto count = call(m->function, {kOut, kParams});
		if(count < 0) return -1;
		for(int k = 0; k < _outCapacity; ++k)
			_out[k] = peek32(kOut + 4 * static_cast<uint32_t>(k));
		return static_cast<int>(count);
	}
}
