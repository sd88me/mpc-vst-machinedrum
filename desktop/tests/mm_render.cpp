// mm-render: notes through the plugin processor to a WAV, and (default) a check against the bare engine.
//   mm-render <OS.syx> [out.wav]
// The check plays the same notes on track 1 (machine MM_MACHINE, default 17) through (a) the processor at 44.1 kHz and (b) the
// engine directly, and requires the main outputs to be sample-identical; then renders at 48 kHz and checks the level against
// the 44.1 kHz render; then the same with track 1's own bus enabled (it must leave the main mix and appear on the bus).
#include <juce_audio_utils/juce_audio_utils.h>
#include <cstdio>
#include "MdProcessor.h"

using namespace mm;

static void setParam(MdProcessor& p, const juce::String& id, int v)
{
	auto* prm = p.apvts.getParameter(id);
	prm->setValueNotifyingHost(prm->convertTo0to1(float(v)));
}

struct Render { std::vector<float> L, R, bus; };

static Render run(const juce::String& os, int machine, double rate, int block, bool trackBus, bool mute = false)
{
	MdProcessor proc;
	proc.setOsPath(os, false);
	for(int i = 0; i < 600 && !proc.engineReady(); ++i) juce::Thread::sleep(50);
	if(!proc.engineReady()) { std::fprintf(stderr, "engine not ready: %s\n", proc.statusText().toRawUTF8()); std::exit(2); }
	setParam(proc, paramId(0, "machine"), machine);
	setParam(proc, paramId(0, "vol"), 110);
	if(mute) setParam(proc, paramId(0, "mute"), 1);
	if(trackBus) proc.enableAllBuses();
	proc.setRateAndBufferSizeDetails(rate, block);
	proc.prepareToPlay(rate, block);
	const int total = int(rate * 2.0);
	Render r;
	juce::AudioBuffer<float> buf(2 + 2 * kTracks, block);
	for(int pos = 0; pos < total; pos += block)
	{
		juce::MidiBuffer midi;
		if(pos == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 36, uint8_t(100)), 7);
		if(pos <= int(rate * 0.5) && int(rate * 0.5) < pos + block) midi.addEvent(juce::MidiMessage::noteOn(1, 36, uint8_t(60)), int(rate * 0.5) - pos);
		buf.clear();
		proc.processBlock(buf, midi);
		for(int i = 0; i < block; ++i) { r.L.push_back(buf.getSample(0, i)); r.R.push_back(buf.getSample(1, i)); r.bus.push_back(trackBus ? buf.getSample(2, i) : 0.f); }
	}
	return r;
}

int main(int argc, char** argv)
{
	{	// hermetic: an empty data folder, so the user's factory kits and ROM samples do not change what the processor starts from
		const auto empty = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mm-render-empty-data");
		empty.createDirectory();
#if JUCE_WINDOWS
		_putenv_s("MACHINEMODULE_DATA_DIR", empty.getFullPathName().toRawUTF8());
#else
		setenv("MACHINEMODULE_DATA_DIR", empty.getFullPathName().toRawUTF8(), 1);
#endif
	}
	juce::ScopedJuceInitialiser_GUI init;
	if(argc < 2) { std::fprintf(stderr, "usage: mm-render <OS.syx> [out.wav]\n"); return 2; }
	const juce::String os = argv[1];
	const int machine = std::getenv("MM_MACHINE") ? std::atoi(std::getenv("MM_MACHINE")) : 17;
	int bad = 0;

	// (a) processor at 44.1 kHz
	const auto a = run(os, machine, 44100.0, 480, false);
	double peak = 0; for(float v : a.L) peak = std::max(peak, double(std::abs(v)));
	std::printf("44.1 kHz: peak %.4f\n", peak);
	if(peak < 0.01) { std::printf("FAIL: silent\n"); ++bad; }

	// (b) the bare engine, same notes
	{
		const std::string p = os.toStdString();
		auto fw = md::fw::loadFirmware(p);
		auto c = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(p)));
		md::engine::Engine e(fw, std::vector<uint8_t>(c.sections.at(0).data));
		md::engine::Engine::Output o;
		for(int b = 0; b < 40; ++b) e.render(o);
		uint8_t master[32]; for(int i = 0; i < 32; ++i) master[i] = uint8_t(kMasterDefault[i]);
		e.enableMasterFx(true);
		e.setMasterFx(master, true);
		auto& h = e.host();
		h.setMachine(0, uint8_t(machine));
		for(int i = 0; i < kRaw; ++i) h.setParam(0, i, i == 17 ? 110 : kRawDefault[i]);
		h.setLevel(0, 100);
		for(int t = 0; t < kTracks; ++t) h.setLfo(t, t, 0, 0, 0, 0);
		h.setTempo(120.0);
		for(int t = 1; t < kTracks; ++t) h.setLevel(t, 100);
		size_t mismatches = 0, first = ~size_t(0);
		for(size_t blk = 0; blk * 32 < a.L.size() - 32; ++blk)
		{
			const int64_t frame = int64_t(blk) * 32;
			if(frame < 7 + 32 && frame + 32 > 7 && blk == 0) h.trigger(0, 100);	// event at frame 7: applied at the block that holds it
			if(blk > 0 && frame <= 22050 && 22050 < frame + 32) h.trigger(0, 60);
			e.render(o);
			for(int f = 0; f < 32; ++f)
			{
				const float ref = float(e.mainOut(o, f, 0)) / 8388608.0f;
				if(ref != a.L[size_t(frame) + size_t(f)]) { ++mismatches; if(first == ~size_t(0)) { first = size_t(frame) + size_t(f); for(int k = 0; k < 6 && first + size_t(k) < a.L.size(); ++k) std::printf("  [%zu] processor %.8f\n", first + size_t(k), a.L[first + size_t(k)]); std::printf("  engine ref at first: %.8f\n", ref); } }
			}
		}
		std::printf("processor vs engine: %zu mismatching samples%s\n", mismatches, mismatches ? "" : " (identical)");
		if(mismatches) { std::printf("first mismatch at sample %zu\n", first); ++bad; }
	}

	// (c) 48 kHz: same level, same shape
	{
		const auto b = run(os, machine, 48000.0, 256, false);
		double pk = 0; for(float v : b.L) pk = std::max(pk, double(std::abs(v)));
		std::printf("48 kHz: peak %.4f (44.1 kHz: %.4f)\n", pk, peak);
		if(std::abs(pk - peak) > 0.08 * peak) { std::printf("FAIL: level differs\n"); ++bad; }
		if(argc > 2)
		{
			juce::File f(argv[2]);
			f.deleteFile();
			juce::WavAudioFormat wav;
			if(auto out = std::unique_ptr<juce::AudioFormatWriter>(wav.createWriterFor(new juce::FileOutputStream(f), 48000.0, 2, 24, {}, 0)))
			{
				juce::AudioBuffer<float> w(2, int(b.L.size()));
				w.copyFrom(0, 0, b.L.data(), int(b.L.size())); w.copyFrom(1, 0, b.R.data(), int(b.R.size()));
				out->writeFromAudioSampleBuffer(w, 0, w.getNumSamples());
			}
		}
	}

	// (d) track 1 on its own bus: leaves the main mix, appears on the bus
	{
		const auto t = run(os, machine, 44100.0, 480, true);
		double mainPk = 0, busPk = 0;
		for(float v : t.L) mainPk = std::max(mainPk, double(std::abs(v)));
		for(float v : t.bus) busPk = std::max(busPk, double(std::abs(v)));
		size_t hot = 0; for(float v : t.bus) hot += std::abs(v) >= 0.9999f;
		std::printf("track bus: main peak %.4f, bus peak %.4f (%zu samples at full scale)\n", mainPk, busPk, hot);
		if(mainPk > 1e-6 || busPk < 0.01) { std::printf("FAIL: track did not move to its bus\n"); ++bad; }
	}
	// (e) a muted track is silent on the main mix and on its own bus
	{
		const auto m = run(os, machine, 44100.0, 480, false, true);
		double pk = 0; for(float v : m.L) pk = std::max(pk, double(std::abs(v)));
		const auto mb = run(os, machine, 44100.0, 480, true, true);
		double bpk = 0; for(float v : mb.bus) bpk = std::max(bpk, double(std::abs(v)));
		std::printf("muted: main peak %.6f, bus peak %.6f\n", pk, bpk);
		if(pk > 1e-6 || bpk > 1e-6) { std::printf("FAIL: a muted track is not silent\n"); ++bad; }
	}
	// (f) the control machines: the OS's rules for CTR-AL (relative, on every sounding track), CTR-RE.. (a master effect block) and CTR-8P (P1-P8 to targets)
	{
		MdProcessor p;
		p.setOsPath(os, false);
		for(int i = 0; i < 600 && !p.engineReady(); ++i) juce::Thread::sleep(50);
		auto val = [&](int t, const char* k) { return int(std::lround(p.apvts.getRawParameterValue(paramId(t, k))->load())); };
		auto master = [&](int i) { return int(std::lround(p.apvts.getRawParameterValue(masterId(i))->load())); };
		auto pump = [&] { juce::MessageManager::getInstance()->runDispatchLoopUntil(60); };
		p.setRateAndBufferSizeDetails(44100.0, 256);
		p.prepareToPlay(44100.0, 256);
		juce::AudioBuffer<float> buf(2 + 2 * kTracks, 256);
		auto tick = [&] { juce::MidiBuffer m; buf.clear(); p.processBlock(buf, m); pump(); };
		setParam(p, paramId(0, "machine"), kCtrAll);
		setParam(p, paramId(1, "machine"), 16);
		setParam(p, paramId(2, "machine"), kCtrEcho);
		setParam(p, paramId(3, "machine"), kCtr8P);
		setParam(p, paramId(4, "machine"), 16);
		setParam(p, paramId(5, "machine"), 17);
		tick(); tick();
		std::printf("CTR defaults: CTR-AL syn1 %d, CTR-RE syn1 %d (echo parameter 1: %d), CTR-8P first target: track %d, parameter %d\n", val(0, "syn1"), val(2, "syn1"), master(8), val(3, "amd") + 1, val(3, "amf") + 1);
		if(val(0, "syn1") != 64 || val(0, "syn8") != 64) { std::printf("FAIL: CTR-AL starts at 64\n"); ++bad; }
		if(val(2, "syn1") != master(8)) { std::printf("FAIL: CTR-RE starts at the echo block's value\n"); ++bad; }
		if(val(3, "amd") != 0 || val(3, "amf") != 0 || val(3, "eqf") != 1 || val(3, "eqg") != 0) { std::printf("FAIL: CTR-8P default targets\n"); ++bad; }
		// CTR-AL: +10 on SYN1 of the sounding tracks
		setParam(p, paramId(1, "syn1"), 40); setParam(p, paramId(4, "syn1"), 120); setParam(p, paramId(5, "syn1"), 7);
		setParam(p, paramId(0, "syn1"), 74); pump();
		std::printf("CTR-AL +10 on SYN1: track 2 %d (40), track 5 %d (120, clamps), track 6 %d (7), CTR-RE track unchanged\n", val(1, "syn1"), val(4, "syn1"), val(5, "syn1"));
		if(val(1, "syn1") != 50 || val(4, "syn1") != 127 || val(5, "syn1") != 17 || val(2, "syn1") != master(8)) { std::printf("FAIL: CTR-AL did not shift SYN1 on the other tracks\n"); ++bad; }
		setParam(p, paramId(0, "syn1"), 54); pump();
		if(val(1, "syn1") != 30) { std::printf("FAIL: CTR-AL -20 (got %d, want 30)\n", val(1, "syn1")); ++bad; }
		// CTR-RE: SYN1 is the echo block's first parameter
		setParam(p, paramId(2, "syn1"), 99); pump();
		std::printf("CTR-RE syn1 99 -> echo parameter 1 = %d\n", master(8));
		if(master(8) != 99) { std::printf("FAIL: CTR-RE did not set the echo parameter\n"); ++bad; }
		// CTR-8P: P1 -> track 5 (index 4) parameter SYN2; P2 (default track 2, SYN1) untouched by the first
		setParam(p, paramId(3, "amd"), 4); setParam(p, paramId(3, "amf"), 1); setParam(p, paramId(3, "syn1"), 77);
		tick(); tick();
		std::printf("CTR-8P: engine's track 5 SYN2 = %d (want 77)\n", p.appliedValue(4, 1));
		if(p.appliedValue(4, 1) != 77) { std::printf("FAIL: CTR-8P did not drive its target\n"); ++bad; }
		// a control machine as the target is ignored
		setParam(p, paramId(3, "amd"), 0); setParam(p, paramId(3, "amf"), 3); setParam(p, paramId(3, "syn1"), 55); tick();
		if(p.appliedValue(0, 3) == 55) { std::printf("FAIL: CTR-8P wrote into a control machine\n"); ++bad; }
	}
	// (g) the audio input: a sine on the left channel through INP-GA (track 1) and nothing through INP-GB (track 2)
	for(const double rate : {44100.0, 48000.0})
	{
		MdProcessor p;
		p.setOsPath(os, false);
		for(int i = 0; i < 600 && !p.engineReady(); ++i) juce::Thread::sleep(50);
		setParam(p, paramId(0, "machine"), 80);
		setParam(p, paramId(1, "machine"), 81);
		for(int t = 0; t < 2; ++t) { setParam(p, paramId(t, "syn1"), 127); setParam(p, paramId(t, "syn2"), 0); setParam(p, paramId(t, "vol"), 100); }
		const int block = 256;
		p.enableAllBuses();
		p.setRateAndBufferSizeDetails(rate, block);
		p.prepareToPlay(rate, block);
		juce::AudioBuffer<float> buf(2 + 2 * kTracks, block);
		double pk = 0, pkSilentTrack = 0; int crossings = 0, last = 0, counted = 0;
		for(int pos = 0; pos < int(rate * 1.5); pos += block)
		{
			juce::MidiBuffer m;
			if(pos == 0) { m.addEvent(juce::MidiMessage::noteOn(1, 36, uint8_t(100)), 0); m.addEvent(juce::MidiMessage::noteOn(1, 37, uint8_t(100)), 0); }
			buf.clear();
			for(int i = 0; i < block; ++i) buf.setSample(0, i, float(0.5 * std::sin(2.0 * 3.14159265358979 * 441.0 * double(pos + i) / rate)));
			p.processBlock(buf, m);
			if(pos < int(rate * 0.5)) continue;
			for(int i = 0; i < block; ++i)
			{
				pk = std::max(pk, double(std::abs(buf.getSample(2, i))));	// track 1's bus (stereo, after the input bus)
				pkSilentTrack = std::max(pkSilentTrack, double(std::abs(buf.getSample(4, i))));
				const float s = buf.getSample(2, i);
				const int sg = s > 0 ? 1 : s < 0 ? -1 : 0;
				if(sg && last && sg != last) ++crossings;
				if(sg) last = sg;
				++counted;
			}
		}
		const double hz = crossings / 2.0 / (counted / rate);
		std::printf("input at %.0f Hz: INP-GA on its track bus peak %.4f, %.0f Hz; INP-GB (no right input) peak %.6f\n", rate, pk, hz, pkSilentTrack);
		if(pk < 0.01 || std::abs(hz - 441.0) > 25.0 || pkSilentTrack > 1e-4) { std::printf("FAIL: the audio input through INP machines\n"); ++bad; }
	}
	// (h) mute groups: a trigger of track 1 silences track 2 (its mixer words go to 0) until track 2 is triggered again; with no group it keeps sounding
	{
		auto peakAfterTrigger = [&](int group)
		{
			MdProcessor proc;
			proc.setOsPath(os, false);
			for(int i = 0; i < 600 && !proc.engineReady(); ++i) juce::Thread::sleep(50);
			for(int t = 0; t < 2; ++t)
			{
				setParam(proc, paramId(t, "machine"), 17);
				setParam(proc, paramId(t, "vol"), 110);
				setParam(proc, paramId(t, "syn2"), 127);	// DEC: a long tail
			}
			setParam(proc, paramId(0, "mutegroup"), group);	// 0 none, 2 = track 2
			proc.enableAllBuses();
			const double rate = 44100.0; const int block = 480;
			proc.setRateAndBufferSizeDetails(rate, block);
			proc.prepareToPlay(rate, block);
			juce::AudioBuffer<float> buf(2 + 2 * kTracks, block);
			double pk = 0;
			for(int pos = 0; pos < int(rate * 1.5); pos += block)
			{
				juce::MidiBuffer midi;
				if(pos == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 37, uint8_t(100)), 0);	// track 2 sounds
				if(pos <= int(rate * 0.4) && int(rate * 0.4) < pos + block) midi.addEvent(juce::MidiMessage::noteOn(1, 36, uint8_t(100)), int(rate * 0.4) - pos);	// track 1 triggers
				buf.clear();
				proc.processBlock(buf, midi);
				if(pos >= int(rate * 0.5))	// well after the control tick that applies the group
					for(int i = 0; i < block; ++i) pk = std::max(pk, double(std::abs(buf.getSample(2 + 2 * 1, i))));	// track 2's own bus
			}
			return pk;
		};
		const double with = peakAfterTrigger(2), without = peakAfterTrigger(0);
		std::printf("mute group: track 2 bus after track 1's trigger: %.6f with the group, %.6f without\n", with, without);
		if(with > 1e-6 || without < 0.001) { std::printf("FAIL: mute groups\n"); ++bad; }
	}
	// (i) solo: with track 1 soloed, track 2 is silent (on its bus and so in the main mix); with nothing soloed both sound
	{
		auto busPeaks = [&](bool solo1)
		{
			MdProcessor proc;
			proc.setOsPath(os, false);
			for(int i = 0; i < 600 && !proc.engineReady(); ++i) juce::Thread::sleep(50);
			for(int t = 0; t < 2; ++t) { setParam(proc, paramId(t, "machine"), 17); setParam(proc, paramId(t, "vol"), 110); }
			if(solo1) setParam(proc, paramId(0, "solo"), 1);
			proc.enableAllBuses();
			const double rate = 44100.0; const int block = 480;
			proc.setRateAndBufferSizeDetails(rate, block);
			proc.prepareToPlay(rate, block);
			juce::AudioBuffer<float> buf(2 + 2 * kTracks, block);
			double pk[2] = {0, 0};
			for(int pos = 0; pos < int(rate * 1.0); pos += block)
			{
				juce::MidiBuffer midi;
				if(pos == 0) { midi.addEvent(juce::MidiMessage::noteOn(1, 36, uint8_t(100)), 0); midi.addEvent(juce::MidiMessage::noteOn(1, 37, uint8_t(100)), 0); }
				buf.clear();
				proc.processBlock(buf, midi);
				if(pos >= int(rate * 0.1))
					for(int i = 0; i < block; ++i) for(int t = 0; t < 2; ++t) pk[t] = std::max(pk[t], double(std::abs(buf.getSample(2 + 2 * t, i))));
			}
			return std::pair<double, double>(pk[0], pk[1]);
		};
		const auto none = busPeaks(false), soloed = busPeaks(true);
		std::printf("solo: no solo: track 1 %.4f, track 2 %.4f; track 1 soloed: track 1 %.4f, track 2 %.6f\n", none.first, none.second, soloed.first, soloed.second);
		if(none.first < 0.001 || none.second < 0.001 || soloed.first < 0.001 || soloed.second > 1e-6) { std::printf("FAIL: solo\n"); ++bad; }
	}
	std::printf(bad ? "FAILED\n" : "ALL OK\n");
	return bad;
}
