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

static Render run(const juce::String& os, int machine, double rate, int block, bool trackBus)
{
	MdProcessor proc;
	proc.setOsPath(os, false);
	for(int i = 0; i < 600 && !proc.engineReady(); ++i) juce::Thread::sleep(50);
	if(!proc.engineReady()) { std::fprintf(stderr, "engine not ready: %s\n", proc.statusText().toRawUTF8()); std::exit(2); }
	setParam(proc, paramId(0, "machine"), machine);
	setParam(proc, paramId(0, "vol"), 110);
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
				const float ref = float(o.mix.main[size_t(f)][0]) / 8388608.0f;
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
	std::printf(bad ? "FAILED\n" : "ALL OK\n");
	return bad;
}
