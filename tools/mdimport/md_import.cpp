// md-import: the factory data of a Machinedrum from the user's own flash image, in one step (what tools/mdkits/make_factory.py does,
// for the desktop plugin's "Import flash image..." menu item). It runs two helpers that sit next to it:
//   mdProbe    boots the emulated Machinedrum (gearmulator-md-mm) from the flash image; dumps the 64 kits and the voice DSP's memory
//   mdsamples  keeps the ROM machines' sample directory and data from that dump
// and writes FACTORY.syx (the named kits) and ROM_SAMPLES.bin to the output folder. Both are Elektron's data from the user's own
// files: per-user output, never committed or distributed.
//   md-import <flash image .bin> <MD OS .syx> <out dir>
// Exit code 0 = done; on failure a line says what went wrong.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
std::string quote(const std::string& s)
{
#ifdef _WIN32
	return "\"" + s + "\"";
#else
	std::string out = "'";
	for(char c : s) { if(c == '\'') out += "'\\''"; else out += c; }
	return out + "'";
#endif
}

bool run(const std::vector<std::string>& args)
{
	std::string cmd;
	for(const auto& a : args) { if(!cmd.empty()) cmd += ' '; cmd += quote(a); }
#ifdef _WIN32
	cmd = "\"" + cmd + " >NUL 2>NUL\"";
#else
	cmd += " >/dev/null 2>&1";
#endif
	return std::system(cmd.c_str()) == 0;
}

fs::path helper(const fs::path& self, const char* name)
{
	fs::path p = self.parent_path() / name;
#ifdef _WIN32
	p += ".exe";
#endif
	return p;
}
}

int main(int argc, char** argv)
{
	if(argc != 4) { std::fprintf(stderr, "usage: md-import <flash image .bin> <MD OS .syx> <out dir>\n"); return 2; }
	const fs::path flash = argv[1], os = argv[2], out = argv[3];
	const fs::path self = fs::absolute(argv[0]);
	const fs::path probe = helper(self, "mdProbe"), samples = helper(self, "mdsamples");
	std::error_code ec;
	if(!fs::exists(flash, ec)) { std::fprintf(stderr, "cannot read the flash image %s\n", flash.string().c_str()); return 1; }
	if(!fs::exists(os, ec)) { std::fprintf(stderr, "cannot read the OS file %s\n", os.string().c_str()); return 1; }
	if(fs::file_size(flash, ec) != 8u * 1024 * 1024) { std::fprintf(stderr, "that is not an 8 MB Machinedrum flash image\n"); return 1; }
	if(!fs::exists(probe, ec) || !fs::exists(samples, ec)) { std::fprintf(stderr, "mdProbe and mdsamples must be next to md-import (%s)\n", self.parent_path().string().c_str()); return 1; }
	fs::create_directories(out, ec);
	const fs::path tmp = fs::temp_directory_path(ec) / ("md-import-" + std::to_string(std::rand()));
	fs::create_directories(tmp, ec);
	struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{tmp};

	const fs::path dump = tmp / "dsp2_p.bin";
	if(!run({probe.string(), flash.string(), (tmp / "flash.bin").string(), "kitdump:" + tmp.string(), "dspdump:1:0:800000:" + dump.string()}))
	{ std::fprintf(stderr, "booting the emulated Machinedrum failed\n"); return 1; }

	std::string kits;
	int count = 0;
	for(int slot = 0; slot < 64; ++slot)
	{
		char name[32];
		std::snprintf(name, sizeof name, "kit_%02d.syx", slot);
		std::ifstream f(tmp / name, std::ios::binary);
		if(!f) continue;
		const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if(data.size() >= 0x4d1 && data[0x0a] != 0x00 && data[0x0a] != 0x7f) { kits += data; ++count; }	// a named kit, not an empty slot
	}
	if(!count) { std::fprintf(stderr, "no kits found in the flash image\n"); return 1; }
	{ std::ofstream o(out / "FACTORY.syx", std::ios::binary); o.write(kits.data(), std::streamsize(kits.size())); }
	if(!run({samples.string(), os.string(), dump.string(), (out / "ROM_SAMPLES.bin").string()}))
	{ std::fprintf(stderr, "reading the ROM samples failed (is that the OS 1.63 file?)\n"); return 1; }
	std::printf("%d factory kits and the ROM samples written to %s\n", count, out.string().c_str());
	return 0;
}
