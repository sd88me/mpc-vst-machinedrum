// Machinedrum One's engine: the OS's own control code (HostModel + MachineRunner), the voice DSP (VoiceEngine,
// emulated) and the mixer DSP's per-track effects and mix as native C++ (TrackFx, Mixer). One 32-sample block
// at a time: all 16 tracks, the dry main mix, the reverb/delay sends and each track's own output.
#pragma once
#include <algorithm>
#include <chrono>
#include <array>
#include <memory>
#include <vector>

#include <type_traits>
#include <vector>
#include "HostModel.h"
#include "MachineRunner.h"
#include "MasterFx.h"
#include "MasterFxOs.h"
#include "Mixer.h"
#include "ParallelVoiceEngine.h"
#include "TrackFx.h"
#include "VoiceEngine.h"

namespace md::engine
{
	// TVoices: VoiceEngine (default, one DSP2 instance) or ParallelVoiceEngine (splits the 16 voices across
	// threads - see its header and HANDOFF.md, "per-machine cost profiled"). Template so both share this same
	// code; defined inline here (not in Engine.cpp, which is now just an include of this header, kept so
	// existing build commands that name engine/Engine.cpp as a source file still work) so each consumer gets
	// its own instantiation without needing whole-class explicit instantiation, which would force every
	// TVoices to support every constructor overload below.
	template<class TVoices = VoiceEngine>
	class EngineT
	{
	public:
		static constexpr int kTracks = 16;
		static constexpr int kBlock = 32;

		// _osImage: the OS file's section 0 (the ColdFire OS), decompressed. Constructs TVoices with just the
		// firmware (VoiceEngine's own constructor).
		EngineT(const fw::Firmware& _fw, std::vector<uint8_t> _osImage)
			: m_fw(&_fw), m_tables(_fw), m_mixer(m_tables)
		{
			m_os = std::make_unique<MachineRunner>(std::move(_osImage));
			m_voices = std::make_unique<TVoices>(_fw);
			init();
		}

		// As above, but forwards _voicesArg to TVoices' constructor too (e.g. ParallelVoiceEngine's group
		// count). Only ever instantiated for a TVoices whose constructor actually takes (firmware, _voicesArg).
		template<class TVoicesArg>
		EngineT(const fw::Firmware& _fw, std::vector<uint8_t> _osImage, TVoicesArg _voicesArg)
			: m_fw(&_fw), m_tables(_fw), m_mixer(m_tables)
		{
			m_os = std::make_unique<MachineRunner>(std::move(_osImage));
			m_voices = std::make_unique<TVoices>(_fw, _voicesArg);
			init();
		}

		~EngineT() = default;

		// The Machinedrum's master effects (see MasterFxOs.h). On: the OS's parameter code runs every tick and render() runs the mixer
		// DSP's master section on the mix, and Output::mix.main becomes the processed main output. Off (default, the Force build):
		// Output::mix.main is the dry mix, bit-exact with the plain mixer.
		void enableMasterFx(const bool _on)
		{
			if(_on && !m_masterOs) { m_masterOs = std::make_unique<MasterFxOs>(*m_fw); m_masterOs->setReverbEchoOnly(m_masterReverbEchoOnly); }
			m_host->enableMasterParams(_on);
			if(!_on) m_masterOs.reset();
		}
		bool masterFxEnabled() const { return m_masterOs != nullptr; }
		// Bypassed: the main output is the dry mix again and the section is not run (it keeps its state, so switching back resumes).
		void setMasterBypass(const bool _b) { m_masterBypass = _b; }
		const Mixer& mixer() const { return m_mixer; }
		// Reverb and echo only (MasterFx::setReverbEchoOnly): kept across enableMasterFx. Call between renders (with a master thread:
		// while no renderMaster is running).
		void setMasterReverbEchoOnly(const bool _on) { m_masterReverbEchoOnly = _on; if(m_masterOs) m_masterOs->setReverbEchoOnly(_on); }
		void setMasterFx(const uint8_t* _bytes32, const bool _snap = false) { m_host->setMasterFx(_bytes32); if(_snap) m_host->snapMasterFx(); }


		// The audio input for the next render(): 32 stereo frames, interleaved L R, 24-bit signed (what the INP machines process;
		// see VoiceEngine::setInput). Needed before every render once used.
		void setInput(const int32_t* _lr) { m_voices->setInput(_lr); }

		HostModel<TVoices>& host() { return *m_host; }
		const MachineRunner& os() const { return *m_os; }
		TVoices& voices() { return *m_voices; }

		struct Output
		{
			Mixer::Output mix;													// main, sends, individual outputs
			std::array<std::array<int32_t, kBlock>, kTracks> tracks{};			// each track after its effects
		};

		// Main output sample f, channel c (0 = left) of a rendered block, with or without the master effects.
		int32_t mainOut(const Output& _o, const int _f, const int _c) const { return _o.mix.main[_f][_c]; }

		// Optional stage timing (microseconds, accumulated): track effects, and the mix. The OS tick and the voice DSP
		// are HostModel's (host().tickUs / dspUs).
		uint32_t dryMute = 0;	// tracks kept out of the dry main mix (see Mixer::process); set by the caller between renders
		bool timingOn = false;
		double fxUs = 0, mixUs = 0, masterUs = 0;

		// The master effects (reverb, delay, EQ, dynamix: MasterFx, DSP1's own section on the emulator) take the dry main mix
		// and the two sends and give the hardware's main output. _knobs: the kit's 4 x 8 master knobs (0-127), converted by the
		// OS's own code when they change. Built on first use (it boots a DSP: do it before the audio runs). False if it cannot
		// run; then the plain mix is what render() gives.
		bool setMasterFx(const bool _on, const std::array<std::array<uint8_t, 8>, 4>& _knobs)
		{
			m_masterOn = false;
			if(!_on) return true;
			if(m_masterFault) return false;
			if(!m_master)
			{
				try { m_master = std::make_unique<MasterFx>(*m_fw); }
				catch(const std::exception&) { m_masterFault = true; return false; }
				m_masterKnobs = {};
				m_masterKnown = false;
			}
			for(int fx = 0; fx < kNumMasterFx; ++fx)
			{
				const auto f = static_cast<size_t>(fx);
				if(m_masterKnown && m_masterKnobs[f] == _knobs[f]) continue;
				std::array<uint32_t, 16> words{};
				if(!convertMasterFx(*m_os, static_cast<MasterFxId>(fx), _knobs[f].data(), words)) { m_masterFault = true; return false; }
				m_master->setParams(static_cast<MasterFxId>(fx), words);
				m_masterKnobs[f] = _knobs[f];
			}
			m_masterKnown = true;
			m_masterOn = true;
			return true;
		}
		bool masterFxOn() const { return m_masterOn; }

		static constexpr bool kParallel = std::is_same_v<TVoices, ParallelVoiceEngine>;

		// One track's effects: a settled silent track's chain gives the same again while its input stays silent and
		// its state (which holds its params) stays the same, so it is skipped. Exact - see HANDOFF.md, "fixed
		// per-block cost". Touches only track _t's state, so tracks can run on different threads (each with its own
		// TrackFx: its scratch buffers are per instance).
		void fxTrack(TrackFx& _fx, const int _t, const int32_t* _src, int32_t* _dst)
		{
			TrackFx::setParams(m_state[_t], m_host->mixerInput(_t).fx.data());
			const bool silentIn = std::all_of(_src, _src + kBlock, [](int32_t s) { return s == 0; });
			if(skipSettled && silentIn && m_settled[_t] && m_state[_t].y == m_settledState[_t].y)
			{
				std::fill(_dst, _dst + kBlock, 0);
				return;
			}
			const auto before = m_state[_t];
			_fx.process(m_state[_t], _src, _dst);
			m_settled[_t] = silentIn && m_state[_t].y == before.y && std::all_of(_dst, _dst + kBlock, [](int32_t s) { return s == 0; });
			if(m_settled[_t])
				m_settledState[_t] = m_state[_t];
		}

		bool render(Output& _out)
		{
			if(!timingOn) return renderBlockDry(_out) && knobMaster(_out) && masterBlock(_out);
			if(!renderBlockDry(_out)) return false;
			const auto t0 = std::chrono::steady_clock::now();
			if(!knobMaster(_out) || !masterBlock(_out)) return false;
			mixUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
			return true;
		}

	private:
		bool knobMaster(Output& _out)	// the plugin's master section: knob words from setMasterFx(on, knobs)
		{
			Mixer::Stereo res;
			if(m_masterOn && m_master && m_master->process(_out.mix, res)) _out.mix.main = res;
			return true;
		}
		bool renderBlockDry(Output& _out)
		{
			m_host->timingOn = timingOn;
			m_out = &_out;
			if(!m_host->renderBlock(m_voiceOut))
				return false;
			const auto tf0 = timingOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			std::array<const int32_t*, kTracks> in{};
			std::array<std::array<uint32_t, 5>, kTracks> mix{};
			for(int t = 0; t < kTracks; ++t)
			{
				// Parallel: the groups' threads already ran their tracks' effects (see init()).
				if constexpr(!kParallel)
					fxTrack(*m_fxs[0], t, m_voiceOut[t].data(), _out.tracks[t].data());
				in[t] = _out.tracks[t].data();
				mix[t] = m_host->mixerInput(t).mix;
			}
			if(!timingOn) { m_mixer.process(in.data(), mix.data(), _out.mix, dryMute); return true; }
			const auto tf1 = std::chrono::steady_clock::now();
			m_mixer.process(in.data(), mix.data(), _out.mix, dryMute);
			const auto tf2 = std::chrono::steady_clock::now();
			const auto tf3 = std::chrono::steady_clock::now();
			fxUs += std::chrono::duration<double, std::micro>(tf1 - tf0).count();
			mixUs += std::chrono::duration<double, std::micro>(tf2 - tf1).count();
			masterUs += std::chrono::duration<double, std::micro>(tf3 - tf2).count();
			return true;
		}

	public:

		// render() in two halves, so the master effects can run on another thread while the next block's voices render (the Schwung
		// module's engine process): renderDry() is everything but the master section and takes the section's parameter words as the
		// OS has them after this block's tick; renderMaster() runs the section on that block's mix. Call them in order, one
		// renderMaster per renderDry, from at most two threads (the section's state is touched only by renderMaster). The same
		// audio as render().
		struct MasterJob
		{
			bool active = false;
			uint32_t a[MasterFxOs::kWordsA], b[MasterFxOs::kWordsB];
		};
		bool renderDry(Output& _out, MasterJob& _job)
		{
			_job.active = false;
			if(!renderBlockDry(_out)) return false;
			if(m_masterOs && !m_masterBypass) { _job.active = true; m_host->masterWords(_job.a, _job.b); }
			return true;
		}
		bool renderMaster(Output& _out, const MasterJob& _job)
		{
			if(!_job.active || !m_masterOs) return true;
			m_masterOs->setParams(_job.a, _job.b);
			return m_masterOs->process(_out.mix);
		}

		const std::string& fault() const { return m_voices->faultReason(); }

		bool skipSettled = true;	// false: run every track's effects every block (for verifying the skip)

	private:
		bool masterBlock(Output& _out)
		{
			if(!m_masterOs || m_masterBypass) return true;
			uint32_t a[MasterFxOs::kWordsA], b[MasterFxOs::kWordsB];
			m_host->masterWords(a, b);
			m_masterOs->setParams(a, b);
			return m_masterOs->process(_out.mix);
		}

		void init()
		{
			m_host = std::make_unique<HostModel<TVoices>>(*m_os, *m_voices);
			for(auto& s : m_state)
				TrackFx::init(s);
			m_fxs.push_back(std::make_unique<TrackFx>(m_tables));
			if constexpr(kParallel)
			{
				// Each group's thread runs the effects of its own tracks right after its voices render.
				for(int g = 1; g < m_voices->groupCount(); ++g)
					m_fxs.push_back(std::make_unique<TrackFx>(m_tables));
				m_voices->setPost([this](int _g, const typename TVoices::Block& _blk)
				{
					for(int t = 0; t < kTracks; ++t)
						if(m_voices->groupOf(t) == _g)
							fxTrack(*m_fxs[static_cast<size_t>(_g)], t, _blk[t].data(), m_out->tracks[t].data());
				});
			}
		}

		const fw::Firmware* m_fw;
		std::unique_ptr<MasterFx> m_master;
		std::array<std::array<uint8_t, 8>, 4> m_masterKnobs{};
		bool m_masterOn = false, m_masterFault = false, m_masterKnown = false;
		std::unique_ptr<MachineRunner> m_os;
		std::unique_ptr<TVoices> m_voices;
		std::unique_ptr<HostModel<TVoices>> m_host;
		TrackFx::Tables m_tables;
		std::vector<std::unique_ptr<TrackFx>> m_fxs;	// one per voice group (one when not parallel)
		Output* m_out = nullptr;
		Mixer m_mixer;
		std::unique_ptr<MasterFxOs> m_masterOs;
		bool m_masterBypass = false, m_masterReverbEchoOnly = false;
		std::array<TrackFx::State, kTracks> m_state{};
		std::array<TrackFx::State, kTracks> m_settledState{};
		std::array<bool, kTracks> m_settled{};
		typename TVoices::Block m_voiceOut{};
	};

	using Engine = EngineT<VoiceEngine>;
	// Constructed with (firmware, osImage, groupCount) - see ParallelVoiceEngine.h.
	using ParallelEngine = EngineT<ParallelVoiceEngine>;
}
