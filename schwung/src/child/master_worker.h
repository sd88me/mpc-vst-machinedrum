// MasterWorker (below), used by md-engine and md-busybench.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <linux/futex.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "MdEngine.h"

namespace mm
{
	inline void futexWaitPrivate(std::atomic<uint32_t>* addr, uint32_t expected)
	{
		timespec ts{0, 20 * 1000 * 1000};
		syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAIT | FUTEX_PRIVATE_FLAG, expected, &ts, nullptr, 0);
	}

	inline void futexWakePrivate(std::atomic<uint32_t>* addr)
	{
		syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, nullptr, nullptr, 0);
	}

	// Waits until *addr reaches target: spinning for up to about a 32-frame block's work (the other side is usually that close: a futex
	// wake-up costs tens of microseconds on the Move's A72, four times per block), then the futex. Between Move blocks it sleeps.
	inline void waitFor(std::atomic<uint32_t>& a, uint32_t target, int spinUs = 500)
	{
		const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(spinUs);
		for(int i = 0;; ++i)
		{
			if(int32_t(a.load(std::memory_order_acquire) - target) >= 0) return;
			if((i & 63) == 63 && std::chrono::steady_clock::now() > until) break;
#if defined(__aarch64__)
			asm volatile("yield");
#endif
		}
		for(uint32_t v; int32_t((v = a.load(std::memory_order_acquire)) - target) < 0;) futexWaitPrivate(&a, v);
	}

	// The master effects on a second thread, one 32-frame block behind the voices (Engine::renderDry / renderMaster): the section costs
	// about as much as a busy kit's voices on the Move (1.3 ms per 128 frames), so running them side by side keeps a block near the
	// larger of the two instead of their sum. Same audio as Engine::render.
	constexpr int kSub = 128 / md::engine::Engine::kBlock;	// engine blocks (32 frames) per Move block (128 frames)

	struct MasterWorker
	{
		md::engine::Engine* eng = nullptr;
		md::engine::Engine::Output outs[kSub];
		md::engine::Engine::MasterJob jobs[kSub];
		std::atomic<uint32_t> posted{0}, done{0}, quit{0}, failed{0};
		std::thread thread;

		void start()
		{
			thread = std::thread([this]
			{
				prctl(PR_SET_NAME, "md-master");
				for(uint32_t n = 0;; ++n)
				{
					waitFor(posted, n + 1);
					if(quit.load(std::memory_order_acquire)) return;
					if(!eng->renderMaster(outs[n % kSub], jobs[n % kSub])) failed.store(1);
					done.store(n + 1, std::memory_order_release);
					futexWakePrivate(&done);
				}
			});
		}
		void post() { posted.fetch_add(1, std::memory_order_release); futexWakePrivate(&posted); }
		void stop()
		{
			quit.store(1, std::memory_order_release);
			post();
			if(thread.joinable()) thread.join();
		}
	};
}
