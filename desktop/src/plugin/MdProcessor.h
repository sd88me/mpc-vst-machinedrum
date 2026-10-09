// Machinemodule for the desktop: all 16 Machinedrum tracks in one instrument plugin. The sound is the parent repo's
// engine (voice DSP in the dsp56300 JIT, the OS's own control code, the per-track effects and mixer as bit-exact C++),
// the plugin framework follows shnolk's Monomodule (OS file chosen at run time, 44.1 kHz engine behind a band-limited
// converter, parameters as raw 0-127 kit values).
//   Outputs: "Main" (the dry main mix: all tracks, panned, with VOL) and "Track 1".."Track 16" (each track after its effects
//   and VOL, no pan, mono on both channels). A track whose bus the host enables leaves the main mix.
//   MIDI: notes 36-51 (and 0-15) play tracks 1-16, velocity = trigger velocity.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "KitCodec.h"
#include "MdEngine.h"
#include "Firmware.h"
#include "Resampler.h"

namespace mm
{
	// The 24 per-track kit values in HostModel's raw order, and their parameter id suffixes.
	constexpr int kRaw = 24;
	extern const char* const kRawKeys[kRaw];
	extern const int kRawDefault[kRaw];
	extern const char* const kLfoKeys[kNumLfo];

	juce::String paramId(int track, const juce::String& key);	// track 0-15 -> "t01_<key>"
	juce::File dataDir();	// per-user folder (OS file path, factory kits, ROM samples)

	class MdProcessor : public juce::AudioProcessor,
	                    private juce::AudioProcessorValueTreeState::Listener,
	                    private juce::AsyncUpdater
	{
	public:
		MdProcessor();
		~MdProcessor() override;

		void prepareToPlay(double sampleRate, int samplesPerBlock) override;
		void releaseResources() override {}
		bool isBusesLayoutSupported(const BusesLayout&) const override;
		void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
		using juce::AudioProcessor::processBlock;

		juce::AudioProcessorEditor* createEditor() override;
		bool hasEditor() const override { return true; }
		const juce::String getName() const override { return "Machinemodule"; }
		bool acceptsMidi() const override { return true; }
		bool producesMidi() const override { return false; }
		bool isMidiEffect() const override { return false; }
		double getTailLengthSeconds() const override { return 10.0; }
		int getNumPrograms() override { return 1; }
		int getCurrentProgram() override { return 0; }
		void setCurrentProgram(int) override {}
		const juce::String getProgramName(int) override { return {}; }
		void changeProgramName(int, const juce::String&) override {}
		void getStateInformation(juce::MemoryBlock&) override;
		void setStateInformation(const void*, int) override;

		// OS file (message thread). Loading runs on a background thread; the plugin is silent until it is ready.
		juce::String osPath() const { return m_osPath; }
		void setOsPath(const juce::String& path, bool persist = true);
		bool engineReady() const { return m_ready.load(); }
		juce::String statusText() const;

		// Kits (message thread): a kit .syx file may hold several; kit 0 is applied on load.
		bool loadKitFile(const juce::File&, juce::String& error);
		void stepKit(int dir);
		juce::String kitLabel() const;

		juce::AudioProcessorValueTreeState apvts;

	private:
		using Engine = md::engine::Engine;
		static constexpr int kCh = 2 + kTracks;	// engine channels: main L, main R, track 1-16
		static constexpr int kBlock = Engine::kBlock;
		struct PendingNote { int64_t frame; uint8_t track, velocity; };

		static BusesProperties makeBuses();
		static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
		void parameterChanged(const juce::String& id, float value) override;
		void handleAsyncUpdate() override;
		void applyKit(const Kit&);
		void startLoad(const juce::String& path);
		void joinLoader();
		void applyParameters();
		void renderFrames(int n, int64_t firstFrame);
		void processChunk(juce::AudioBuffer<float>&, int offset, int n);
		int64_t engineFrameOf(int64_t hostSample) const { return m_needsResample ? m_engineAt.ceilAt(hostSample) : hostSample; }

		struct TrackParams
		{
			std::atomic<float>* machine = nullptr;
			std::atomic<float>* raw[kRaw] = {};
			std::atomic<float>* level = nullptr;
			std::atomic<float>* lfo[kNumLfo] = {};
		};
		std::array<TrackParams, kTracks> m_par{};
		std::array<std::atomic<bool>, kTracks> m_needsDefaults{};
		std::array<std::atomic<bool>, kTracks> m_defaultsReady{};
		std::array<std::array<std::atomic<int>, 8>, kTracks> m_defaultSyn{};
		std::atomic<bool> m_suppressDefaults{false};

		// the engine, built on a background thread; the audio thread uses it under m_lock (try-lock, silent when busy)
		juce::CriticalSection m_lock;
		std::unique_ptr<md::fw::Firmware> m_fw;
		std::unique_ptr<Engine> m_engine;
		std::thread m_loader;
		std::atomic<bool> m_ready{false}, m_loading{false};
		juce::String m_osPath;
		mutable juce::CriticalSection m_statusLock;
		juce::String m_status;
		bool m_stateRestored = false;

		// what the engine has been given (-1 = nothing yet), so only changes are sent
		int m_appliedMachine[kTracks], m_appliedRaw[kTracks][kRaw], m_appliedLevel[kTracks], m_appliedLfo[kTracks][kNumLfo];
		void resetApplied();

		std::vector<Kit> m_kits;
		int m_kitIndex = -1;
		juce::String m_kitSource;

		// audio
		double m_hostRate = 44100.0;
		bool m_needsResample = false;
		int m_maxChunk = 512, m_maxEngine = 512, m_outLag = 0;
		int64_t m_hostPos = 0, m_enginePos = 0, m_renderPos = 0;
		mnm::dsp::RateMap m_engineAt;
		mnm::dsp::SincKernel m_toHost;
		std::vector<float> m_taps;
		std::array<std::vector<float>, kCh> m_eng;	// the chunk's engine frames, per channel (44.1 kHz)
		std::array<std::vector<float>, kCh> m_carry;	// frames rendered beyond the chunk (a block is 32 frames)
		int m_carryN = 0;
		std::array<mnm::dsp::History, kCh> m_hist;
		std::vector<PendingNote> m_notes;
		std::atomic<float> m_hostBpm{120.0f};
		double m_appliedBpm = -1;
		uint32_t m_dryMute = 0;
		Engine::Output m_out;
	};
}
