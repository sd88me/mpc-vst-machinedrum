#include "MdProcessor.h"
#include <cstdlib>
#include "MdEditor.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mm
{
	juce::String masterId(int i) { return juce::String::formatted("mfx%02d", i + 1); }

	juce::String paramId(int track, const juce::String& key) { return juce::String::formatted("t%02d_", track + 1) + key; }

	juce::File dataDir()
	{
		if(const char* env = std::getenv("MACHINEMODULE_DATA_DIR"); env && *env) return juce::File(juce::String(env));	// tests use an empty folder
		auto base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
#if JUCE_MAC
		base = base.getChildFile("Application Support");
#endif
		return base.getChildFile("Machinemodule");
	}

	juce::AudioProcessor::BusesProperties MdProcessor::makeBuses()
	{
		auto b = BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), false).withOutput("Main", juce::AudioChannelSet::stereo(), true);
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
			g->addChild(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{paramId(t, "mute"), 1}, pre + "Mute", false));
			for(int i = 0; i < kNumLfo; ++i) add(*g, paramId(t, kLfoKeys[i]), pre + juce::String(kLfoKeys[i]).toUpperCase(), 0, kLfoMax[i], i == 0 ? t : 0);
			layout.add(std::move(g));
		}
		{
			auto g = std::make_unique<juce::AudioProcessorParameterGroup>("master", "Master FX", "|");
			g->addChild(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"master_on", 1}, "Master FX", true));
			static const char* const groups[4] = {"REV", "ECHO", "EQ", "DYN"};
			for(int i = 0; i < kMasterBytes; ++i)
				add(*g, masterId(i), juce::String(groups[i / 8]) + " " + juce::String(i % 8 + 1), 0, 127, kMasterDefault[i]);
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
			p.mute = apvts.getRawParameterValue(paramId(t, "mute"));
			for(int i = 0; i < kNumLfo; ++i) p.lfo[i] = apvts.getRawParameterValue(paramId(t, kLfoKeys[i]));
			apvts.addParameterListener(paramId(t, "machine"), this);
			for(int i = 0; i < 8; ++i) apvts.addParameterListener(paramId(t, kRawKeys[i]), this);	// SYN1-8: the control machines act on these
			m_ctrDirty[size_t(t)] = 0;
			m_defaultCount[size_t(t)] = 8;
			m_needsDefaults[size_t(t)] = false;
			m_defaultsReady[size_t(t)] = false;
		}
		for(int i = 0; i < kMasterBytes; ++i) m_master[size_t(i)] = apvts.getRawParameterValue(masterId(i));
		m_masterOn = apvts.getRawParameterValue("master_on");
		m_notes.reserve(1024);

		loadFactoryKits();

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
		joinImporter();
		joinLoader();
	}

	// The factory kits, if the user imported them (Phase 4); kit 0 is the starting sound.
	void MdProcessor::loadFactoryKits()
	{
		const auto factory = dataDir().getChildFile("factory").getChildFile("FACTORY.syx");
		if(!factory.existsAsFile()) return;
		juce::MemoryBlock mb;
		if(!factory.loadFileAsData(mb)) return;
		auto kits = parseKits(std::vector<uint8_t>(static_cast<const uint8_t*>(mb.getData()), static_cast<const uint8_t*>(mb.getData()) + mb.getSize()));
		if(kits.empty()) return;
		m_kits = std::move(kits);
		m_kitSource = "FACTORY";
		m_kitIndex = 0;
		applyKit(m_kits[0]);
	}

	bool MdProcessor::trackLocked(int t) const { return (int(apvts.state.getProperty("locks", 0)) >> t) & 1; }

	void MdProcessor::setTrackLocked(int t, bool locked)
	{
		int mask = int(apvts.state.getProperty("locks", 0));
		mask = locked ? (mask | (1 << t)) : (mask & ~(1 << t));
		apvts.state.setProperty("locks", mask, nullptr);	// saved with the project
	}

	void MdProcessor::joinImporter()
	{
		if(m_importer.joinable()) m_importer.join();
	}

	juce::File MdProcessor::importHelper()
	{
#if JUCE_WINDOWS
		const char* name = "md-import.exe";
#else
		const char* name = "md-import";
#endif
		if(const char* env = std::getenv("MM_IMPORT_HELPER"); env && *env) return juce::File(juce::String(env));
		for(const auto& dir : {dataDir().getChildFile("bin"), juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory()})
			if(dir.getChildFile(name).existsAsFile()) return dir.getChildFile(name);
		return dataDir().getChildFile("bin").getChildFile(name);
	}

	void MdProcessor::importFlashImage(const juce::File& flashImage)
	{
		auto setStatus = [this](const juce::String& s) { const juce::ScopedLock sl(m_statusLock); m_status = s; };
		if(m_importing.load()) return;
		const juce::File helper = importHelper();
		if(m_osPath.isEmpty() || !juce::File(m_osPath).existsAsFile()) { setStatus("Select the OS file first, then import the flash image"); return; }
		if(!helper.existsAsFile()) { setStatus("The import helper (" + helper.getFileName() + ") was not found in " + helper.getParentDirectory().getFullPathName()); return; }
		joinImporter();
		m_importing = true;
		m_importOk = false;
		setStatus("Importing the flash image: booting the emulated Machinedrum, about a minute ...");
		const juce::String os = m_osPath;
		m_importer = std::thread([this, helper, flashImage, os]
		{
			const auto out = dataDir().getChildFile("factory");
			out.createDirectory();
			juce::ChildProcess proc;
			juce::String output;
			bool ok = proc.start(juce::StringArray{helper.getFullPathName(), flashImage.getFullPathName(), os, out.getFullPathName()});
			if(ok)
			{
				output = proc.readAllProcessOutput();
				ok = proc.getExitCode() == 0;
			}
			{
				const juce::ScopedLock sl(m_statusLock);
				m_status = ok ? "Flash image imported: factory kits and ROM samples" : "Flash image import failed: " + output.trim().fromLastOccurrenceOf("\n", false, false);
			}
			m_importOk = ok;
			m_importing = false;
			m_importFinished = true;
			triggerAsyncUpdate();
		});
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
				for(int b = 0; b < 40; ++b) eng->render(o);
				// after the warm-up and built here, not on the audio thread: the section must not run before it has its parameters (the first audio
				// block settles them before rendering), or its dynamics start from a state a unit never has
				eng->enableMasterFx(true);	// the DSP's own start-up, as the build's machine sweep does
				{
					const juce::ScopedLock sl(m_lock);
					m_engine.reset();
					m_fw = std::move(fw);
					m_engine = std::move(eng);
					m_feeder.reset();
					m_hostPos = m_enginePos = m_renderPos = 0;
					m_carryN = 0;
					m_notes.clear();
					for(auto& h : m_hist) h.clear();
				}
				{
					const juce::ScopedLock sl(m_statusLock);
					m_machineList = std::make_shared<const std::vector<md::engine::MachineInfo>>(m_engine->os().machines());
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
		int t = -1, k = -1;
		if(id.endsWith("_machine"))
		{
			if(std::sscanf(id.toRawUTF8(), "t%d_", &t) == 1 && t >= 1 && t <= kTracks) m_needsDefaults[size_t(t - 1)] = true;
		}
		else if(std::sscanf(id.toRawUTF8(), "t%d_syn%d", &t, &k) == 2 && t >= 1 && t <= kTracks && k >= 1 && k <= 8)
		{
			const int id2 = int(std::lround(m_par[size_t(t - 1)].machine->load()));
			if(id2 == kCtrAll || (id2 >= kCtrEcho && id2 <= kCtrDynamix))
			{
				m_ctrDirty[size_t(t - 1)].fetch_or(uint8_t(1u << (k - 1)));
				triggerAsyncUpdate();
			}
		}
	}

	void MdProcessor::handleAsyncUpdate()
	{
		if(m_importFinished.exchange(false) && m_importOk.load())	// new factory data: kits now, ROM samples with the next engine build
		{
			loadFactoryKits();
			if(m_osPath.isNotEmpty()) { const auto keep = statusText(); startLoad(m_osPath); const juce::ScopedLock sl(m_statusLock); m_status = keep + " (reloading)"; }
		}
		// a machine change on the audio thread adopted the new machine's SYN defaults (CTR-8P: its assignments too): show them in the parameters
		for(int t = 0; t < kTracks; ++t)
			if(m_defaultsReady[size_t(t)].exchange(false))
			{
				m_suppressDefaults = true;	// these are the machine's own values, not knob moves for a control machine to act on
				for(int i = 0; i < m_defaultCount[size_t(t)].load(); ++i)
					if(auto* p = apvts.getParameter(paramId(t, kRawKeys[i])))
						p->setValueNotifyingHost(p->convertTo0to1(float(m_defaultSyn[size_t(t)][size_t(i)].load())));
				m_suppressDefaults = false;
				int syn[8];
				for(int i = 0; i < 8; ++i) syn[i] = m_defaultSyn[size_t(t)][size_t(i)].load();
				m_rules.machineDefaults(t, syn);
				m_ctrDirty[size_t(t)] = 0;
			}
		applyControlMachines();
	}

	static void setInt(juce::AudioProcessorValueTreeState& apvts, const juce::String& id, int v)
	{
		if(auto* p = apvts.getParameter(id)) p->setValueNotifyingHost(p->convertTo0to1(float(v)));
	}

	void MdProcessor::readValues(KitValues& v) const
	{
		auto get = [](const std::atomic<float>* p) { return int(std::lround(p->load())); };
		for(int t = 0; t < kTracks; ++t)
		{
			const auto& p = m_par[size_t(t)];
			auto& tr = v.track[size_t(t)];
			tr.machine = get(p.machine);
			for(int i = 0; i < kRaw; ++i) tr.raw[size_t(i)] = get(p.raw[i]);
			tr.level = get(p.level);
			tr.mute = p.mute->load() >= 0.5f;
			for(int i = 0; i < kNumLfo; ++i) tr.lfo[size_t(i)] = get(p.lfo[i]);
		}
		for(int i = 0; i < kMasterBytes; ++i) v.master[size_t(i)] = get(m_master[size_t(i)]);
		v.masterOn = m_masterOn->load() >= 0.5f;
	}

	void MdProcessor::writeValues(const KitValues& now, const KitValues& was)
	{
		for(int t = 0; t < kTracks; ++t)
		{
			const auto &a = now.track[size_t(t)], &b = was.track[size_t(t)];
			if(a.machine != b.machine) setInt(apvts, paramId(t, "machine"), a.machine);
			for(int i = 0; i < kRaw; ++i) if(a.raw[size_t(i)] != b.raw[size_t(i)]) setInt(apvts, paramId(t, kRawKeys[i]), a.raw[size_t(i)]);
			if(a.level != b.level) setInt(apvts, paramId(t, "level"), a.level);
			if(a.mute != b.mute) setInt(apvts, paramId(t, "mute"), a.mute ? 1 : 0);
			for(int i = 0; i < kNumLfo; ++i) if(a.lfo[size_t(i)] != b.lfo[size_t(i)]) setInt(apvts, paramId(t, kLfoKeys[i]), a.lfo[size_t(i)]);
		}
		for(int i = 0; i < kMasterBytes; ++i) if(now.master[size_t(i)] != was.master[size_t(i)]) setInt(apvts, masterId(i), now.master[size_t(i)]);
		if(now.masterOn != was.masterOn) setInt(apvts, "master_on", now.masterOn ? 1 : 0);
	}

	void MdProcessor::defaultsChosen(int t, const int* values, int count)
	{
		for(int i = 0; i < count; ++i) m_defaultSyn[size_t(t)][size_t(i)] = values[i];
		m_defaultCount[size_t(t)] = count;
		m_defaultsReady[size_t(t)] = true;
		triggerAsyncUpdate();
	}

	void MdProcessor::resyncControlMachines()
	{
		KitValues v;
		readValues(v);
		m_rules.resync(v);
		for(auto& d : m_ctrDirty) d = 0;
	}

	// The SYN knobs of control machines that moved: their rules (KitModel's ControlRules) change other parameters.
	void MdProcessor::applyControlMachines()
	{
		KitValues v;
		readValues(v);
		const KitValues was = v;
		for(int t = 0; t < kTracks; ++t)
		{
			const unsigned dirty = m_ctrDirty[size_t(t)].exchange(0);
			for(int k = 0; k < 8; ++k)
				if((dirty >> k) & 1) m_rules.synMoved(v, t, k);
		}
		writeValues(v, was);
	}

	void MdProcessor::applyKit(const Kit& k)
	{
		m_suppressDefaults = true;	// the kit brings its own SYN values with the machine
		for(int t = 0; t < kTracks; ++t)
		{
			if(trackLocked(t)) continue;	// a locked track keeps its sound when a kit loads
			setInt(apvts, paramId(t, "machine"), k.machine[t]);
			for(int i = 0; i < kRaw; ++i) setInt(apvts, paramId(t, kRawKeys[i]), k.params[t][i]);
			setInt(apvts, paramId(t, "level"), k.level[t]);
			for(int i = 0; i < kNumLfo; ++i) setInt(apvts, paramId(t, kLfoKeys[i]), k.lfo[t][i]);
			m_needsDefaults[size_t(t)] = false;
		}
		for(int i = 0; i < kMasterBytes; ++i) setInt(apvts, masterId(i), k.master[i]);
		m_suppressDefaults = false;
		resyncControlMachines();
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

	juce::String MdProcessor::kitName(int i) const
	{
		return i >= 0 && i < int(m_kits.size()) ? juce::String(m_kits[size_t(i)].name).trim() : juce::String();
	}

	void MdProcessor::selectKit(int i)
	{
		if(i < 0 || i >= int(m_kits.size())) return;
		m_kitIndex = i;
		applyKit(m_kits[size_t(i)]);
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
				resyncControlMachines();
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
		if(l.inputBuses.isEmpty()) return true;
		const auto in = l.getMainInputChannelSet();
		return in.isDisabled() || in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono();
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
		for(auto& b : m_inBuf) b.assign(size_t(kInRing), 0.f);
		m_inWritten = int64_t(std::ceil(2.0 * kBlock * sampleRate / 44100.0)) + 4;	// a few blocks of lead, so a block never reads past what has arrived
		m_inPos = 0.0;
	}

	void MdProcessor::applyParameters()
	{
		readValues(m_now);
		m_feeder.apply(*m_engine, m_now, m_hostBpm.load(), *this);
	}

	// The host's input channels (L, or L twice for a mono bus) into the ring, n samples; zeros while the bus is off once it has been used.
	void MdProcessor::pushInput(juce::AudioBuffer<float>& buffer, int n)
	{
		const bool on = getBusCount(true) > 0 && getBus(true, 0)->isEnabled();
		if(on) m_inputUsed = true;
		if(!m_inputUsed) return;
		const float* in[2] = {nullptr, nullptr};
		if(on)
		{
			auto bus = getBusBuffer(buffer, true, 0);
			in[0] = bus.getReadPointer(0);
			in[1] = bus.getNumChannels() > 1 ? bus.getReadPointer(1) : in[0];
		}
		for(int i = 0; i < n; ++i)
			for(int c = 0; c < 2; ++c)
				m_inBuf[size_t(c)][size_t((m_inWritten + i) & (kInRing - 1))] = in[c] ? in[c][i] : 0.f;
		m_inWritten += n;
	}

	// 32 frames at the engine rate (L R interleaved, 24-bit) from the ring.
	void MdProcessor::engineInput(int32_t* lr)
	{
		const double step = m_hostRate / 44100.0;
		auto at = [&](int c, int64_t i) { return m_inBuf[size_t(c)][size_t(std::min(i, m_inWritten - 1) & (kInRing - 1))]; };
		for(int f = 0; f < kBlock; ++f)
		{
			const int64_t i0 = int64_t(std::floor(m_inPos));
			const float fr = float(m_inPos - double(i0));
			for(int c = 0; c < 2; ++c)
			{
				const float s = at(c, i0) + (at(c, i0 + 1) - at(c, i0)) * fr;
				lr[2 * f + c] = int32_t(std::lround(std::clamp(s, -1.0f, 1.0f) * 8388607.0f));
			}
			m_inPos += step;
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
			if(m_inputUsed) { int32_t lr[2 * kBlock]; engineInput(lr); eng.setInput(lr); }
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
		for(int t = 0; t < kTracks; ++t)	// the editor's meters
		{
			float pk = 0.0f;
			const float* s = m_eng[size_t(2 + t)].data();
			for(int i = 0; i < nEngine; ++i) pk = std::max(pk, std::abs(s[i]));
			m_peak[size_t(t)].store(std::max(pk, m_peak[size_t(t)].load(std::memory_order_relaxed) * 0.9f), std::memory_order_relaxed);
		}

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
		{
			const juce::ScopedTryLock inLock(m_lock);	// the ring belongs to the audio thread; prepareToPlay rebuilds it under the lock
			if(inLock.isLocked()) pushInput(buffer, n);
		}
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
