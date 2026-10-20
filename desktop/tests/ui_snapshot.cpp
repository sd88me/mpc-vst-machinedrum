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

// mm-snapshot --import-test <OS.syx> <flash.bin>: the plugin's "Import flash image" path with MM_IMPORT_HELPER and an empty data folder.
static int importTest(const char* os, const char* flash)
{
	MdProcessor proc;
	proc.setOsPath(os, false);
	for(int i = 0; i < 600 && !proc.engineReady(); ++i) juce::Thread::sleep(50);
	if(!proc.engineReady()) { std::fprintf(stderr, "engine not ready: %s\n", proc.statusText().toRawUTF8()); return 2; }
	std::printf("before: %d kits, status: %s\n", proc.kitCount(), proc.statusText().toRawUTF8());
	proc.importFlashImage(juce::File(flash));
	for(int i = 0; i < 3000 && (proc.importRunning() || i < 20); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
	juce::MessageManager::getInstance()->runDispatchLoopUntil(300);	// the async update that reloads
	for(int i = 0; i < 600 && !proc.engineReady(); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
	std::printf("after: %d kits, kit 0 \"%s\", ready %d, status: %s\n", proc.kitCount(), proc.kitName(0).toRawUTF8(), int(proc.engineReady()), proc.statusText().toRawUTF8());
	return proc.kitCount() == 16 && proc.engineReady() ? 0 : 1;
}

int main(int argc, char** argv)
{
	juce::ScopedJuceInitialiser_GUI init;
	if(argc == 3 && juce::String(argv[1]) == "--missing-os")	// the first-run screen, no OS file
	{
		MdProcessor proc;
		std::unique_ptr<juce::AudioProcessorEditor> ed(proc.createEditor());
		juce::MessageManager::getInstance()->runDispatchLoopUntil(400);
		const juce::File f(argv[2]);
		f.deleteFile();
		juce::FileOutputStream fos(f);
		juce::PNGImageFormat().writeImageToStream(ed->createComponentSnapshot(ed->getLocalBounds(), true, 1.0f), fos);
		std::printf("%s\n", f.getFullPathName().toRawUTF8());
		return 0;
	}
	if(argc == 4 && juce::String(argv[1]) == "--import-test") return importTest(argv[2], argv[3]);
	if(argc < 3) { std::fprintf(stderr, "usage: mm-snapshot <OS.syx> <outdir> [width]\n"); return 2; }
	const int width = argc > 3 ? juce::String(argv[3]).getIntValue() : 0;
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
	if(width > 0) ed->setSize(width, width * ed->getHeight() / ed->getWidth());
	const juce::File out(argv[2]);
	out.createDirectory();
	juce::PNGImageFormat png;
	auto shot = [&](const char* name)
	{
		juce::MessageManager::getInstance()->runDispatchLoopUntil(400);
		ed->refresh();
		auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 1.0f);
		const auto f = out.getChildFile(juce::String("page-") + name + ".png");
		f.deleteFile();
		juce::FileOutputStream fos(f);
		png.writeImageToStream(img, fos);
		std::printf("%s\n", f.getFullPathName().toRawUTF8());
	};
	ed->selectTrack(0);
	shot("track");
	ed->setView(MdEditor::View::Mixer);
	shot("mixer");
	ed->setView(MdEditor::View::Master);
	shot("master");
	ed->setView(MdEditor::View::Track);
	ed->selectTrack(1);
	ed->showMachinePicker();
	shot("picker");
	return 0;
}
