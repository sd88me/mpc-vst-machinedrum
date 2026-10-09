#include "MdProcessor.h"
#include "MdEditor.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mm
{
	const char* const kRawKeys[kRaw] = {"syn1", "syn2", "syn3", "syn4", "syn5", "syn6", "syn7", "syn8",
		"amd", "amf", "eqf", "eqg", "fltf", "fltw", "fltq", "srr", "dist", "vol", "pan", "del", "rev", "lfos", "lfod", "lfom"};
	const int kRawDefault[kRaw] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 64, 64, 0, 127, 0, 0, 0, 100, 64, 0, 0, 0, 0, 0};
	const char* const kLfoKeys[kNumLfo] = {"lfo_track", "lfo_param", "lfo_shp1", "lfo_shp2", "lfo_type"};

	juce::String paramId(int track, const juce::String& key) { return juce::String::formatted("t%02d_", track + 1) + key; }

	juce::File dataDir()
	{
		auto base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
#if JUCE_MAC
		base = base.getChildFile("Application Support");
#endif
		return base.getChildFile("Machinemodule");
	}

	juce::AudioProcessor::BusesProperties MdProcessor::makeBuses()
	{
		auto b = BusesProperties().withOutput("Main", juce::AudioChannelSet::stereo(), true);
		for(int t = 0; t < kTracks; ++t) b = b.withOutput("Track " + juce::String(t + 1), juce::AudioChannelSet::stereo(), false);
		return b;
	}

	juce::AudioProcessorValueTreeState::ParameterLayout MdProcessor::makeLayout()
	{
		juce::AudioProcessorValueTreeState::ParameterLayout layout;
		auto add = [](juce::AudioProcessorParameterGroup& g, const juce::String& id, const juce::String& name, int lo, int hi, int def)
		{
			g.addChild(std::make_unique<juce::AudioParameterInt>(juce::ParameterID{id, 1}, name, lo, hi, def));
		};
		for(int t = 0; t < kTracks; ++t)
		{
			auto g = std::make_unique<juce::AudioProcessorParameterGroup>(juce::String::formatted("t%02d", t + 1), "Track " + juce::String(t + 1), "|");
			const juce::String pre = "T" + juce::String(t + 1) + " ";
			add(*g, paramId(t, "machine"), pre + "Machine", 0, 191, 0);
			for(int i = 0; i < kRaw; ++i) add(*g, paramId(t, kRawKeys[i]), pre + juce::String(kRawKeys[i]).toUpperCase(), 0, 127, kRawDefault[i]);
			add(*g, paramId(t, "level"), pre + "LEV", 0, 127, 100);
			for(int i = 0; i < kNumLfo; ++i) add(*g, paramId(t, kLfoKeys[i]), pre + juce::String(kLfoKeys[i]).toUpperCase(), 0, kLfoMax[i], i == 0 ? t : 0);
			layout.add(std::move(g));
		}
		return layout;
	}

	MdProcessor::MdProcessor()
		: AudioProcessor(makeBuses()), apvts(*this, nullptr, "MACHINEMODULE", makeLayout())
	{
		for(int t = 0; t < kTracks; ++t)
		{
			auto& p = m_par[size_t(t)];
			p.machine = apvts.getRawParameterValue(paramId(t, "machine"));
			for(int i = 0; i < kRaw; ++i) p.raw[i] = apvts.getRawParameterValue(paramId(t, kRawKeys[i]));
			p.level = apvts.getRawParameterValue(paramId(t, "level"));
			for(int i = 0; i < kNumLfo; ++i) p.lfo[i] = apvts.getRawParameterValue(paramId(t, kLfoKeys[i]));
			apvts.addParameterListener(paramId(t, "machine"), this);
			m_needsDefaults[size_t(t)] = false;
			m_defaultsReady[size_t(t)] = false;
		}
		m_notes.reserve(1024);
		resetApplied();

		// factory kits, if the user imported them (Phase 4); kit 0 is the starting sound
		const auto factory = dataDir().getChildFile("factory").getChildFile("FACTORY.syx");
		if(factory.existsAsFile())
		{
			juce::MemoryBlock mb;
			if(factory.loadFileAsData(mb))
			{
				m_kits = parseKits(std::vector<uint8_t>(static_cast<const uint8_t*>(mb.getData()), static_cast<const uint8_t*>(mb.getData()) + mb.getSize()));
				m_kitSource = "FACTORY";
				if(!m_kits.empty()) { m_kitIndex = 0; applyKit(m_kits[0]); }
			}
		}

		juce::String path = juce::SystemStats::getEnvironmentVariable("MM_OS_SYX", {});
		if(path.isEmpty())
		{
			const auto saved = dataDir().getChildFile("os_path.txt");
			if(saved.existsAsFile()) path = saved.loadFileAsString().trim();
		}
		if(path.isNotEmpty()) { m_osPath = path; startLoad(path); }
		else { const juce::ScopedLock sl(m_statusLock); m_status = "Select your Machinedrum OS 1.63 file (.syx)"; }
	}

	MdProcessor::~MdProcessor()
	{
		cancelPendingUpdate();
		joinLoader();
	}

	void MdProcessor::resetApplied()
	{
		for(int t = 0; t < kTracks; ++t)
		{
			m_appliedMachine[t] = -1;
			m_appliedLevel[t] = -1;
			for(int i = 0; i < kRaw; ++i) m_appliedRaw[t][i] = -1;
			for(int i = 0; i < kNumLfo; ++i) m_appliedLfo[t][i] = -1;
		}
		m_appliedBpm = -1;
	}

	// ---- engine loading ---------------------------------------------------------------------------------------------

	void MdProcessor::joinLoader()
	{
		if(m_loader.joinable()) m_loader.join();
	}

	juce::String MdProcessor::statusText() const
	{
		const juce::ScopedLock sl(m_statusLock);
		return m_status;
	}

	void MdProcessor::setOsPath(const juce::String& path, bool persist)
	{
		m_osPath = path;
		if(persist)
		{
			const auto f = dataDir().getChildFile("os_path.txt");
			f.getParentDirectory().createDirectory();
			f.replaceWithText(path);
		}
		startLoad(path);
	}

	void MdProcessor::startLoad(const juce::String& path)
	{
		joinLoader();
		m_ready = false;
		{ const juce::ScopedLock sl(m_statusLock); m_status = "Loading " + juce::File(path).getFileName() + " ..."; }
		m_loader = std::thread([this, path]
		{
			auto setStatus = [this](const juce::String& s) { const juce::ScopedLock sl(m_statusLock); m_status = s; };
			try
			{
				const std::string p = path.toStdString();
				auto fw = std::make_unique<md::fw::Firmware>(md::fw::loadFirmware(p));
				auto container = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(p)));
				auto eng = std::make_unique<Engine>(*fw, std::vector<uint8_t>(container.sections.at(0).data));
				int romRecords = 0;
				if(std::FILE* rf = std::fopen(dataDir().getChildFile("factory").getChildFile("ROM_SAMPLES.bin").getFullPathName().toRawUTF8(), "rb"))
				{
					char magic[4]; std::vector<uint32_t> words; uint32_t head[2];
					if(std::fread(magic, 1, 4, rf) == 4)
						while(std::fread(head, 4, 2, rf) == 2 && head[1] > 0 && head[1] < 0x800000)
						{
							words.resize(head[1]);
							if(std::fread(words.data(), 4, head[1], rf) != head[1]) break;
							eng->voices().writeP(head[0], words.data(), words.size());
							++romRecords;
						}
					std::fclose(rf);
				}
				Engine::Output o;
				for(int b = 0; b < 40; ++b) eng->render(o);	// the DSP's own start-up, as the build's machine sweep does
				{
					const juce::ScopedLock sl(m_lock);
					m_engine.reset();
					m_fw = std::move(fw);
					m_engine = std::move(eng);
					resetApplied();
					m_hostPos = m_enginePos = m_renderPos = 0;
					m_carryN = 0;
					m_notes.clear();
					for(auto& h : m_hist) h.clear();
				}
				m_ready = true;
				setStatus(juce::String("Ready") + (romRecords ? " (ROM samples loaded)" : " (no ROM samples: ROM machines are silent)"));
			}
			catch(const std::exception& e)
			{
				setStatus(juce::String("Cannot load the OS file: ") + e.what());
			}
		});
	}

	// ---- parameters and kits ----------------------------------------------------------------------------------------

	void MdProcessor::parameterChanged(const juce::String& id, float)
	{
		if(m_suppressDefaults.load()) return;
		int t = -1;
		if(std::sscanf(id.toRawUTF8(), "t%d_machine", &t) == 1 && t >= 1 && t <= kTracks) m_needsDefaults[size_t(t - 1)] = true;
	}

	void MdProcessor::handleAsyncUpdate()
	{
		// a machine change on the audio thread adopted the new machine's SYN defaults: show them in the parameters
		for(int t = 0; t < kTracks; ++t)
			if(m_defaultsReady[size_t(t)].exchange(false))
				for(int i = 0; i < 8; ++i)
					if(auto* p = apvts.getParameter(paramId(t, kRawKeys[i])))
						p->setValueNotifyingHost(p->convertTo0to1(float(m_defaultSyn[size_t(t)][size_t(i)].load())));
	}

	static void setInt(juce::AudioProcessorValueTreeState& apvts, const juce::String& id, int v)
	{
		if(auto* p = apvts.getParameter(id)) p->setValueNotifyingHost(p->convertTo0to1(float(v)));
	}

	void MdProcessor::applyKit(const Kit& k)
	{
		m_suppressDefaults = true;	// the kit brings its own SYN values with the machine
		for(int t = 0; t < kTracks; ++t)
		{
			setInt(apvts, paramId(t, "machine"), k.machine[t]);
			for(int i = 0; i < kRaw; ++i) setInt(apvts, paramId(t, kRawKeys[i]), k.params[t][i]);
			setInt(apvts, paramId(t, "level"), k.level[t]);
			for(int i = 0; i < kNumLfo; ++i) setInt(apvts, paramId(t, kLfoKeys[i]), k.lfo[t][i]);
			m_needsDefaults[size_t(t)] = false;
		}
		m_suppressDefaults = false;
	}

	bool MdProcessor::loadKitFile(const juce::File& f, juce::String& error)
	{
		juce::MemoryBlock mb;
		if(!f.loadFileAsData(mb)) { error = "Cannot read " + f.getFileName(); return false; }
		const auto* d = static_cast<const uint8_t*>(mb.getData());
		auto kits = parseKits(std::vector<uint8_t>(d, d + mb.getSize()));
		if(kits.empty()) { error = f.getFileName() + " holds no Machinedrum kit"; return false; }
		m_kits = std::move(kits);
		m_kitSource = f.getFileNameWithoutExtension().toUpperCase();
		m_kitIndex = 0;
		applyKit(m_kits[0]);
		return true;
	}

	void MdProcessor::stepKit(int dir)
	{
		if(m_kits.empty()) return;
		const int n = int(m_kits.size());
		m_kitIndex = ((m_kitIndex + dir) % n + n) % n;
		applyKit(m_kits[size_t(m_kitIndex)]);
	}

	juce::String MdProcessor::kitLabel() const
	{
		if(m_kitIndex < 0 || m_kitIndex >= int(m_kits.size())) return "no kit loaded";
		return m_kitSource + " " + juce::String(m_kitIndex + 1) + "/" + juce::String(int(m_kits.size())) + "  " + juce::String(m_kits[size_t(m_kitIndex)].name).trim();
	}

	void MdProcessor::getStateInformation(juce::MemoryBlock& dest)
	{
		auto state = apvts.copyState();
		if(auto xml = state.createXml()) copyXmlToBinary(*xml, dest);
	}

	void MdProcessor::setStateInformation(const void* data, int size)
	{
		if(auto xml = getXmlFromBinary(data, size))
			if(xml->hasTagName(apvts.state.getType()))
			{
				m_suppressDefaults = true;
				apvts.replaceState(juce::ValueTree::fromXml(*xml));
				for(auto& f : m_needsDefaults) f = false;
				m_suppressDefaults = false;
			}
	}

	// ---- audio ------------------------------------------------------------------------------------------------------

	bool MdProcessor::isBusesLayoutSupported(const BusesLayout& l) const
	{
		if(l.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
		for(int b = 1; b < l.outputBuses.size(); ++b)
		{
			const auto& s = l.outputBuses.getReference(b);
			if(!s.isDisabled() && s != juce::AudioChannelSet::stereo() && s != juce::AudioChannelSet::mono()) return false;
		}
		return l.inputBuses.isEmpty() || l.getMainInputChannelSet().isDisabled();
	}

	void MdProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
	{
		m_hostRate = sampleRate;
		m_needsResample = std::abs(sampleRate - 44100.0) > 0.5;
		m_maxChunk = std::max(1, samplesPerBlock);
		int historyFrames = 0;
		if(m_needsResample)
		{
			m_toHost.design(44100.0, sampleRate);
			m_engineAt.set(44100.0, sampleRate);
			// output m reads engine frames up to floor((m - outLag) * 44100 / rate) + H; the chunk ending at m has rendered
			// those up to floor(m * 44100 / rate): outLag * 44100 / rate >= H + 1 keeps every read inside (as Monomodule)
			m_outLag = int(std::ceil((m_toHost.halfTaps() + 1) * sampleRate / 44100.0));
			m_maxEngine = int(std::ceil(m_maxChunk * 44100.0 / sampleRate)) + 2;
			historyFrames = m_maxEngine + 2 * m_toHost.halfTaps() + 64;
			m_taps.assign(size_t(m_toHost.numTaps()), 0.f);
		}
		else { m_outLag = 0; m_maxEngine = m_maxChunk; }
		setLatencySamples(m_outLag);
		const juce::ScopedLock sl(m_lock);
		for(int c = 0; c < kCh; ++c)
		{
			m_eng[size_t(c)].assign(size_t(m_maxEngine + 1), 0.f);
			m_carry[size_t(c)].assign(size_t(kBlock), 0.f);
			if(m_needsResample) m_hist[size_t(c)].prepare(historyFrames);
		}
		m_carryN = 0;
		m_hostPos = m_enginePos = m_renderPos = 0;
		m_notes.clear();
	}

	void MdProcessor::applyParameters()
	{
		auto& h = m_engine->host();
		const double bpm = std::clamp<double>(m_hostBpm.load(), 30.0, 300.0);
		if(bpm != m_appliedBpm) { m_appliedBpm = bpm; h.setTempo(bpm); }
		for(int t = 0; t < kTracks; ++t)
		{
			const auto& p = m_par[size_t(t)];
			const int m = std::clamp(int(std::lround(p.machine->load())), 0, 191);
			if(m != m_appliedMachine[t])
			{
				m_appliedMachine[t] = m;
				h.setMachine(t, uint8_t(m));	// raw SYN1-8 take the machine's defaults
				if(m_needsDefaults[size_t(t)].exchange(false))
				{
					for(int i = 0; i < 8; ++i) { m_appliedRaw[t][i] = h.param(t, i); m_defaultSyn[size_t(t)][size_t(i)] = m_appliedRaw[t][i]; }
					m_defaultsReady[size_t(t)] = true;
					triggerAsyncUpdate();
				}
				else
					for(int i = 0; i < 8; ++i) m_appliedRaw[t][i] = -1;	// the parameters' own SYN values win (a kit or a restored project)
			}
			for(int i = 0; i < kRaw; ++i)
			{
				const int v = std::clamp(int(std::lround(p.raw[i]->load())), 0, 127);
				if(v != m_appliedRaw[t][i]) { m_appliedRaw[t][i] = v; h.setParam(t, i, v); }
			}
			const int lev = std::clamp(int(std::lround(p.level->load())), 0, 127);
			if(lev != m_appliedLevel[t]) { m_appliedLevel[t] = lev; h.setLevel(t, lev); }
			int v[kNumLfo];
			bool changed = false;
			for(int i = 0; i < kNumLfo; ++i)
			{
				v[i] = std::clamp(int(std::lround(p.lfo[i]->load())), 0, kLfoMax[i]);
				changed |= v[i] != m_appliedLfo[t][i];
				m_appliedLfo[t][i] = v[i];
			}
			if(changed) h.setLfo(t, v[0], v[1], v[2], v[3], v[4]);
		}
	}

	// n engine frames (44.1 kHz) into m_eng, starting at engine frame `first`. The engine renders 32-frame blocks; what a block
	// has beyond the chunk waits in m_carry. Notes and parameters take effect at block boundaries, as on the hardware's tick.
	void MdProcessor::renderFrames(int n, int64_t)
	{
		const float scale = 1.0f / 8388608.0f;	// 24-bit full scale
		auto& eng = *m_engine;
		int got = std::min(m_carryN, n);
		for(int c = 0; c < kCh; ++c)
		{
			std::copy_n(m_carry[size_t(c)].data(), got, m_eng[size_t(c)].data());
			std::copy(m_carry[size_t(c)].begin() + got, m_carry[size_t(c)].begin() + m_carryN, m_carry[size_t(c)].begin());
		}
		m_carryN -= got;
		while(got < n)
		{
			applyParameters();	// before the notes: a machine change takes effect at the track's next trigger
			size_t used = 0;
			while(used < m_notes.size() && m_notes[used].frame < m_renderPos + kBlock) { eng.host().trigger(m_notes[used].track, m_notes[used].velocity); ++used; }
			m_notes.erase(m_notes.begin(), m_notes.begin() + std::ptrdiff_t(used));
			eng.dryMute = m_dryMute;
			float blk[kCh][kBlock];
			if(eng.render(m_out))
			{
				for(int f = 0; f < kBlock; ++f)
				{
					blk[0][f] = float(m_out.mix.main[size_t(f)][0]) * scale;
					blk[1][f] = float(m_out.mix.main[size_t(f)][1]) * scale;
				}
				for(int t = 0; t < kTracks; ++t)
				{
					if(!((m_dryMute >> t) & 1)) { std::fill_n(blk[2 + t], kBlock, 0.f); continue; }
					const uint32_t vol = eng.host().mixerInput(t).mix[1];
					for(int f = 0; f < kBlock; ++f) blk[2 + t][f] = float(md::engine::Mixer::solo(m_out.tracks[size_t(t)][size_t(f)], vol)) * scale;
				}
			}
			else
				for(auto& ch : blk) std::fill_n(ch, kBlock, 0.f);	// a DSP fault: silence (eng.fault() has the reason)
			m_renderPos += kBlock;
			const int use = std::min(kBlock, n - got);
			for(int c = 0; c < kCh; ++c)
			{
				std::copy_n(blk[c], use, m_eng[size_t(c)].data() + got);
				std::copy(blk[c] + use, blk[c] + kBlock, m_carry[size_t(c)].begin());
			}
			m_carryN = kBlock - use;
			got += use;
		}
	}

	void MdProcessor::processChunk(juce::AudioBuffer<float>& buffer, int offset, int n)
	{
		const int64_t hostEnd = m_hostPos + n, e0 = m_enginePos;
		const int64_t e1 = !m_needsResample ? hostEnd : std::max(e0, m_engineAt.floorAt(hostEnd - 1) + 1);
		const int nEngine = int(e1 - e0);
		renderFrames(nEngine, e0);

		struct Stream { int chL, chR; float* outL; float* outR; };
		Stream streams[1 + kTracks];
		int count = 0;
		for(int b = 0; b < getBusCount(false); ++b)
		{
			if(!getBus(false, b)->isEnabled()) continue;
			auto bus = getBusBuffer(buffer, false, b);
			if(bus.getNumChannels() == 0) continue;
			const int chL = b == 0 ? 0 : 2 + (b - 1), chR = b == 0 ? 1 : chL;
			streams[count++] = {chL, chR, bus.getWritePointer(0) + offset, bus.getNumChannels() > 1 ? bus.getWritePointer(1) + offset : nullptr};
		}
		if(!m_needsResample)
		{
			for(int s = 0; s < count; ++s)
			{
				std::copy_n(m_eng[size_t(streams[s].chL)].data(), n, streams[s].outL);
				if(streams[s].outR) std::copy_n(m_eng[size_t(streams[s].chR)].data(), n, streams[s].outR);
			}
		}
		else
		{
			for(int c = 0; c < kCh; ++c) m_hist[size_t(c)].push(m_eng[size_t(c)].data(), nEngine);	// every channel, heard or not
			const int half = m_toHost.halfTaps(), taps = m_toHost.numTaps();
			const int64_t first = m_hostPos - m_outLag;
			for(int k = 0; k < n; ++k)
			{
				const auto at = m_engineAt.at(first + k);
				const float* t = m_toHost.taps(at, m_taps.data());
				for(int s = 0; s < count; ++s)
				{
					streams[s].outL[k] = m_hist[size_t(streams[s].chL)].dot(at.index - half + 1, t, taps);
					if(streams[s].outR) streams[s].outR[k] = m_hist[size_t(streams[s].chR)].dot(at.index - half + 1, t, taps);
				}
			}
		}
		m_hostPos = hostEnd;
		m_enginePos = e1;
	}

	void MdProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
	{
		juce::ScopedNoDenormals noDenormals;
		const int n = buffer.getNumSamples();
		buffer.clear();
		if(auto* ph = getPlayHead())
			if(const auto pos = ph->getPosition())
				if(const auto bpm = pos->getBpm(); bpm && *bpm > 0.0) m_hostBpm.store(float(*bpm));
		const juce::ScopedTryLock sl(m_lock);
		if(!sl.isLocked() || !m_ready.load() || !m_engine) { midi.clear(); return; }
		uint32_t mask = 0;
		for(int t = 0; t < kTracks; ++t)
			if(t + 1 < getBusCount(false) && getBus(false, t + 1)->isEnabled()) mask |= 1u << t;
		m_dryMute = mask;
		for(const auto meta : midi)
		{
			const auto msg = meta.getMessage();
			if(!msg.isNoteOn() || m_notes.size() >= m_notes.capacity()) continue;
			const int note = msg.getNoteNumber();
			const int track = note < kTracks ? note : note - 36;	// notes 36-51 (the Machinedrum's map); 0-15 too (MPC OS drum-pad patch)
			if(track < 0 || track >= kTracks) continue;
			m_notes.push_back({engineFrameOf(m_hostPos + meta.samplePosition), uint8_t(track), uint8_t(std::max(1, int(msg.getVelocity())))});
		}
		for(int done = 0; done < n;)
		{
			const int c = std::min(m_maxChunk, n - done);
			processChunk(buffer, done, c);
			done += c;
		}
		midi.clear();
	}

	juce::AudioProcessorEditor* MdProcessor::createEditor() { return new MdEditor(*this); }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new mm::MdProcessor(); }
