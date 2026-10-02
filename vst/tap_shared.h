// State shared between "Machinemodule" (the primary: owns the engine) and "Machinemodule Tap" instances, which live in
// the same MPC process. The tap library finds it through md_tap_shared(), exported by machinedrum_one.so (dlopen RTLD_NOLOAD).
// One track or send per tap, mono for tracks, so MPC's own mixer, submixes and send effects can process each one.
#pragma once
#include <atomic>
#include <cstdint>

namespace mdtap
{
	constexpr int kTracks = 16, kFrames = 128, kSlots = 8;
	// planes: 0-15 the tracks (mono, post effects and VOL), 16/17 reverb send L/R, 18/19 delay send L/R
	constexpr int kPlanes = kTracks + 4, kPlaneRev = 16, kPlaneDel = 18;
	// tap sources, one bit each in a tap's mask (any number at once, summed): bits 0-15 the tracks, 16 reverb send, 17 delay send
	constexpr int kBitRev = 16, kBitDel = 17, kSources = 18;

	struct Shared
	{
		static constexpr uint32_t kMagic = 0x4d445450;
		uint32_t magic = kMagic;
		std::atomic<const void*> owner{nullptr};	// the primary that publishes (the first one created)
		std::atomic<uint32_t> written{0};			// blocks published
		std::atomic<uint32_t> hostRead{0};			// blocks the primary's host has taken
		std::atomic<int64_t> hostCallUs{0};			// steady-clock time (us) of the primary's last host call
		std::atomic<int> tapped[kTracks];			// taps reading each track: that track leaves the primary's main mix
		std::atomic<int> tapsRev{0}, tapsDel{0};
		alignas(64) int16_t data[kSlots][kPlanes][kFrames];
	};
}

extern "C" mdtap::Shared* md_tap_shared(void);
