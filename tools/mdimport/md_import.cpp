// md-import: the factory data of a Machinedrum from the user's own flash image, in one step (what tools/mdkits/make_factory.py does,
// for the desktop plugin's "Import flash image..." menu item). It runs two helpers that sit next to it:
//   mdProbe    boots the emulated Machinedrum (gearmulator-md-mm) from the flash image; dumps the 64 kits and the voice DSP's memory
//   mdsamples  keeps the ROM machines' sample directory and data from that dump
// and writes FACTORY.syx (the named kits) and ROM_SAMPLES.bin to the output folder. Both are Elektron's data from the user's own
// files: per-user output, never committed or distributed.
//   md-import <flash image .bin> <MD OS .syx> <out dir>
// Exit code 0 = done; on failure the last line printed says what went wrong. What the helpers print is kept in <out dir>/import.log.
// On Windows the helpers are started with CreateProcess (no shell, so no quoting trouble) and paths are Unicode. The working files (a
// 32 MB DSP dump) go to a folder inside <out dir>, never the system's temp folder (a Move's root filesystem is nearly full).
// On Linux it lowers its own priority and its locked-memory limit first (the Schwung module runs it on the Move; the helpers inherit
// both): the emulator mlocks ~5 GB of address space where it may, which makes it slow to exit.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/resource.h>
#include <sys/wait.h>
#endif
#ifdef __linux__
#include <linux/capability.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{
#ifdef _WIN32
std::wstring quoteArg(const std::wstring& a)	// the CreateProcess command-line rules (CommandLineToArgvW's inverse)
{
	if(!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
	std::wstring out = L"\"";
	for(size_t i = 0; i < a.size(); ++i)
	{
		size_t slashes = 0;
		while(i < a.size() && a[i] == L'\\') { ++slashes; ++i; }
		if(i == a.size()) { out.append(slashes * 2, L'\\'); break; }
		if(a[i] == L'"') { out.append(slashes * 2 + 1, L'\\'); out += L'"'; }
		else { out.append(slashes, L'\\'); out += a[i]; }
	}
	return out + L"\"";
}

// Runs a helper with its output appended to `log`; returns its exit code, or -1 when it could not be started (`why` says why).
int run(const std::vector<std::wstring>& args, const fs::path& log, std::string& why)
{
	std::wstring cmd;
	for(const auto& a : args) { if(!cmd.empty()) cmd += L' '; cmd += quoteArg(a); }
	SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
	HANDLE h = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	STARTUPINFOW si{};
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = si.hStdError = h;
	PROCESS_INFORMATION pi{};
	std::vector<wchar_t> buf(cmd.begin(), cmd.end());
	buf.push_back(0);
	const BOOL ok = CreateProcessW(args[0].c_str(), buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	if(!ok)
	{
		why = "Windows could not start the helper (error " + std::to_string(GetLastError()) + ")";
		if(h != INVALID_HANDLE_VALUE) CloseHandle(h);
		return -1;
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
	if(h != INVALID_HANDLE_VALUE) CloseHandle(h);
	return int(code);
}

fs::path selfPath(const std::vector<fs::path>&)
{
	wchar_t buf[32768];
	const DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
	return fs::path(std::wstring(buf, n));
}

std::vector<fs::path> arguments(int, char**)
{
	int n = 0;
	wchar_t** w = CommandLineToArgvW(GetCommandLineW(), &n);
	std::vector<fs::path> out;
	for(int i = 0; i < n; ++i) out.emplace_back(w[i]);
	LocalFree(w);
	return out;
}
#else
std::string quote(const std::string& s)
{
	std::string out = "'";
	for(char c : s) { if(c == '\'') out += "'\\''"; else out += c; }
	return out + "'";
}

int run(const std::vector<std::string>& args, const fs::path& log, std::string& why)
{
	std::string cmd;
	for(const auto& a : args) { if(!cmd.empty()) cmd += ' '; cmd += quote(a); }
	cmd += " >>" + quote(log.string()) + " 2>&1";
	const int r = std::system(cmd.c_str());
	if(r == -1) { why = "could not start the helper"; return -1; }
	return WIFEXITED(r) ? WEXITSTATUS(r) : 128;
}

fs::path selfPath(const std::vector<fs::path>& args)
{
	std::error_code ec;
	const auto link = fs::read_symlink("/proc/self/exe", ec);	// Linux; elsewhere the path it was started with
	return !ec && !link.empty() ? link : fs::absolute(args[0]);
}

std::vector<fs::path> arguments(int argc, char** argv)
{
	std::vector<fs::path> out;
	for(int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
	return out;
}
#endif

fs::path helper(const fs::path& self, const char* name)
{
	fs::path p = self.parent_path() / name;
#ifdef _WIN32
	p += ".exe";
#endif
	return p;
}

// The line of the log that says what went wrong: the last one that looks like an error, else the last one.
std::string lastLine(const fs::path& log)
{
	std::ifstream f(log, std::ios::binary);
	std::string line, last, error;
	while(std::getline(f, line))
	{
		while(!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
		if(line.empty()) continue;
		last = line;
		for(const char* key : {"not valid", "cannot", "did not", "fail", "error", "unknown", "no dump", "exception"})
			if(line.find(key) != std::string::npos) { error = line; break; }
	}
	return error.empty() ? last : error;
}

int fail(const std::string& what, const fs::path& log)
{
	const std::string detail = lastLine(log);
	std::fprintf(stderr, "%s%s%s\n", what.c_str(), detail.empty() ? "" : ": ", detail.c_str());
	return 1;
}
}

#ifdef _WIN32
#define S(p) (p).wstring()
#else
#define S(p) (p).string()
#endif

int main(int argc, char** argv)
{
#ifdef __linux__
	{
		const rlimit rl{64 * 1024, 64 * 1024};
		setrlimit(RLIMIT_MEMLOCK, &rl);
		__user_cap_header_struct hdr{_LINUX_CAPABILITY_VERSION_3, 0};
		__user_cap_data_struct data[2] = {};
		syscall(SYS_capset, &hdr, data);	// CAP_IPC_LOCK would bypass the limit
		setpriority(PRIO_PROCESS, 0, 10);
	}
#endif
	const auto args = arguments(argc, argv);
	if(args.size() != 4) { std::fprintf(stderr, "usage: md-import <flash image .bin> <MD OS .syx> <out dir>\n"); return 2; }
	const fs::path flash = args[1], os = args[2], out = args[3];
	const fs::path self = selfPath(args);
	const fs::path probe = helper(self, "mdProbe"), samples = helper(self, "mdsamples");
	std::error_code ec;
	fs::create_directories(out, ec);
	const fs::path log = out / "import.log";
	fs::remove(log, ec);
	if(!fs::exists(flash, ec)) { std::fprintf(stderr, "cannot read the flash image %s\n", flash.string().c_str()); return 1; }
	if(!fs::exists(os, ec)) { std::fprintf(stderr, "cannot read the OS file %s\n", os.string().c_str()); return 1; }
	if(fs::file_size(flash, ec) != 8u * 1024 * 1024) { std::fprintf(stderr, "that is not an 8 MB Machinedrum flash image\n"); return 1; }
	if(!fs::exists(probe, ec) || !fs::exists(samples, ec)) { std::fprintf(stderr, "mdProbe and mdsamples must be next to md-import (%s)\n", self.parent_path().string().c_str()); return 1; }
	const fs::path tmp = out / ".md-import-work";
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);
	struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{tmp};

	const fs::path dump = tmp / "dsp2_p.bin";
	std::string why;
#ifdef _WIN32
	const std::vector<std::wstring> probeArgs = {S(probe), S(flash), S(tmp / "flash.bin"), L"kitdump:" + S(tmp), L"wait:441000", L"dspdump:1:0:800000:" + S(dump)};
#else
	// wait: 10 s of the emulated Machinedrum's time between the kits and the DSP dump: the OS uploads the ROM samples to the voice DSP
	// after it has booted, and on a slow host (a Move) the dump came before they were all there
	const std::vector<std::string> probeArgs = {S(probe), S(flash), S(tmp / "flash.bin"), "kitdump:" + S(tmp), "wait:441000", "dspdump:1:0:800000:" + S(dump)};
#endif
	const int pr = run(probeArgs, log, why);
	if(pr != 0) return fail("booting the emulated Machinedrum failed (" + (why.empty() ? "exit code " + std::to_string(pr) : why) + "; log: " + log.string() + ")", log);

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
	if(!count) return fail("no kits found in the flash image (log: " + log.string() + ")", log);
	{ std::ofstream o(out / "FACTORY.syx", std::ios::binary); o.write(kits.data(), std::streamsize(kits.size())); }
#ifdef _WIN32
	const std::vector<std::wstring> sampleArgs = {S(samples), S(os), S(dump), S(out / "ROM_SAMPLES.bin")};
#else
	const std::vector<std::string> sampleArgs = {S(samples), S(os), S(dump), S(out / "ROM_SAMPLES.bin")};
#endif
	const int sr = run(sampleArgs, log, why);
	if(sr != 0) return fail("reading the ROM samples failed (" + (why.empty() ? "exit code " + std::to_string(sr) : why) + "; is that the OS 1.63 file?)", log);
	std::printf("%d factory kits and the ROM samples written to %s\n", count, out.string().c_str());
	return 0;
}
