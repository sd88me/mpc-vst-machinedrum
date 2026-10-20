// mm-snapshot: renders every editor page to <outdir>/page-<name>.png (the OS file loads first so machine names and labels show).
//   mm-snapshot <OS.syx> <outdir> [width]
#include <juce_audio_utils/juce_audio_utils.h>
#include <cstdio>
#include "MdProcessor.h"
#include "MdEditor.h"

using namespace mm;

static void setParam(MdProcessor& p, const juce::String& id, int v)
{
	auto* prm = p.apvts.getParameter(id);
	prm->setValueNotifyingHost(prm->convertTo0to1(float(v)));
}

int main(int argc, char** argv)
{
	juce::ScopedJuceInitialiser_GUI init;
	if(argc < 3) { std::fprintf(stderr, "usage: mm-snapshot <OS.syx> <outdir> [width]\n"); return 2; }
	const int width = argc > 3 ? juce::String(argv[3]).getIntValue() : 720;
	MdProcessor proc;
	proc.setOsPath(argv[1], false);
	for(int i = 0; i < 600 && !proc.engineReady(); ++i) juce::Thread::sleep(50);
	if(!proc.engineReady()) { std::fprintf(stderr, "engine not ready: %s\n", proc.statusText().toRawUTF8()); return 2; }
	// a few different machines and levels so the pictures are not all the same
	const int machines[] = {17, 25, 33, 41, 49, 57};
	for(int t = 0; t < 6; ++t) { setParam(proc, paramId(t, "machine"), machines[t]); setParam(proc, paramId(t, "level"), 60 + t * 12); }
	std::unique_ptr<juce::AudioProcessorEditor> base(proc.createEditor());
	auto* ed = dynamic_cast<MdEditor*>(base.get());
	if(!ed) return 2;
	ed->setSize(width, width * 480 / 720);
	const juce::File out(argv[2]);
	out.createDirectory();
	const char* names[MdEditor::kPages] = {"syn", "fx", "route", "lfo", "master", "mixer"};
	for(int p = 0; p < MdEditor::kPages; ++p)
	{
		ed->show(MdEditor::Page(p), p == MdEditor::Mixer ? 2 : 1);
		juce::MessageManager::getInstance()->runDispatchLoopUntil(400);
		auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 1.0f);
		juce::PNGImageFormat png;
		const auto f = out.getChildFile(juce::String("page-") + names[p] + ".png");
		f.deleteFile();
		juce::FileOutputStream fos(f);
		png.writeImageToStream(img, fos);
		std::printf("%s\n", f.getFullPathName().toRawUTF8());
	}
	return 0;
}
