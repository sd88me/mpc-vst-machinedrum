// mdextract: the on-device half of "the plugin makes its factory data from the user's own flash image". Boots the emulated
// MD from the flash image (as tools/mdtrace/mdProbe does for release builds), asks it for its 64 kits and dumps the voice
// DSP's program memory above the OS program ($140000-). The plugin (vst/engine.cpp) runs this in a child process the first
// time it finds the flash image next to the OS file, then keeps the kits as factory/FACTORY.syx and the sample words that
// differ from its own DSP memory as factory/ROM_SAMPLES.bin (tools/mdkits/romsamples.h).
// Copied into gearmulator-md-mm/source/elektron/md/mdLibTest by tools/mdtrace/build_mdprobe.sh (target mdExtract).
// The output is Elektron's data from the user's own file: it stays on the user's device.
//
//   mdextract <MD flash image .bin> <out dir>      writes <out dir>/FACTORY.syx and <out dir>/dsp2_hi.bin (u32 LE words from $140000)
#include "mdLib/mdhardware.h"
#include "baseLib/filesystem.h"
#include "synthLib/midiTypes.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
	if(argc != 3) { std::cerr << "usage: mdextract <MD flash image .bin> <out dir>\n"; return 2; }
	std::vector<uint8_t> rom;
	if(!baseLib::filesystem::readFile(rom, argv[1])) { std::cerr << "cannot read the flash image\n"; return 1; }
	const std::string out = argv[2];

	// first-run flash initialisation: the OS makes the factory kits in an empty flash
	md::Hardware init(rom, argv[1], md::MachineModel::Machinedrum);
	if(!init.isValid()) { std::cerr << "not a valid Machinedrum flash image\n"; return 1; }
	init.disqualifyFactoryFlashCache();
	for(uint32_t f = 0; f < md::g_samplerate * 18 && !init.isFactoryFlashReadyForReboot(); f += 128) init.advance(128);
	if(!init.isFactoryFlashReadyForReboot()) { std::cerr << "flash init did not complete\n"; return 1; }
	const std::vector<uint8_t> flash = init.copyFlashData();

	md::Hardware hw(rom, argv[1], md::MachineModel::Machinedrum, {}, {}, flash);
	if(!hw.isValid()) { std::cerr << "hardware not valid\n"; return 1; }
	for(uint32_t boot = 0; boot < md::g_samplerate * 20; boot += 128) hw.advance(128);

	// the kits: the MD's own kit request ($53) for each slot; the named ones (not $7F/$00 at $0a) are the factory kits
	std::vector<uint8_t> kits;
	int named = 0;
	std::vector<synthLib::SMidiEvent> ev;
	for(int slot = 0; slot < 64; ++slot)
	{
		synthLib::SMidiEvent rq(synthLib::MidiEventSource::Host);
		for(const uint8_t b : {0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x53, slot, 0xf7}) rq.sysex.push_back(static_cast<uint8_t>(b));
		hw.sendMidi(rq);
		std::vector<uint8_t> dump;
		for(int f = 0; f < 44100 * 2 && (dump.empty() || dump.back() != 0xf7); f += 512)
		{
			hw.advance(512);
			ev.clear();
			hw.readMidiOut(ev);
			for(const auto& e : ev)
			{
				if(!e.sysex.empty()) dump.insert(dump.end(), e.sysex.begin(), e.sysex.end());
				else if(!dump.empty() || e.a == 0xf0) { dump.push_back(e.a); if(e.b != 0 || e.c != 0) { dump.push_back(e.b); dump.push_back(e.c); } }
			}
		}
		if(dump.size() >= 0x4d1 && dump.front() == 0xf0 && dump.back() == 0xf7 && dump[0x0a] != 0x00 && dump[0x0a] != 0x7f)
		{
			kits.insert(kits.end(), dump.begin(), dump.end());
			++named;
		}
	}
	if(!named) { std::cerr << "no kits dumped\n"; return 1; }
	std::ofstream(out + "/FACTORY.syx", std::ios::binary).write(reinterpret_cast<const char*>(kits.data()), static_cast<std::streamsize>(kits.size()));

	constexpr uint32_t from = 0x140000;
	auto& mem = hw.traceDsp(1).memory();
	std::vector<uint32_t> words;
	for(uint32_t a = from; a < 0x800000 && a < mem.sizeP(); ++a) words.push_back(mem.get(dsp56k::MemArea_P, a));
	std::ofstream(out + "/dsp2_hi.bin", std::ios::binary).write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
	std::cerr << named << " factory kits, " << words.size() << " DSP words\n";
	return 0;
}
