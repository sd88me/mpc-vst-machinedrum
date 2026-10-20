// What a knob cell shows, as the Machinedrum's LCD presents it. Storage is the plugin's own parameter value (0-127 for a dial,
// 0..N for a list); everything here describes the *display*. Adapted from shnolk's Monomodule (src/plugin/one/SpecData.h), AGPL-3.0.
#pragma once
#include <cstdint>
#include <string>
#include "RomArt.h"

namespace mm::uispec {

// Numeric / Bipolar: the dial and the number (Bipolar shows value-64). Text: the value's name in the cell, no dial (the LFO page's
// TRACK / PARAM / UPDTE). Shape: one of the six LFO shapes. Toggle: the OFF/ON icon. Blank: nothing at all.
enum class Display : uint8_t { Blank, Numeric, Bipolar, Text, Shape, Toggle };

struct Param
{
	const char* label = "";	// as the LCD shows it
	Display display = Display::Blank;
	uint8_t defaultRaw = 0;
	uint8_t maxRaw = 127;
	const char* const* values = nullptr;	// Text: maxRaw + 1 names
};

constexpr Param dial(const char* label, int def = 0) { return {label, Display::Numeric, uint8_t(def), 127, nullptr}; }
constexpr Param bipolar(const char* label, int def = 64) { return {label, Display::Bipolar, uint8_t(def), 127, nullptr}; }
constexpr Param blank() { return {"", Display::Blank, 0, 127, nullptr}; }

// the pages every track has
extern const Param kAmpFxPage[8];	// AMD AMF EQF EQG FLTF FLTW FLTQ SRR
extern const Param kRoutePage[8];	// DIST VOL PAN DEL REV LFOS LFOD LFOM
extern const Param kLfoPage[8];	// TRACK PARAM SHP1 SHP2 UPDTE SPEED DEPTH SHMIX (PARAM's names follow the destination: set values at run time)
extern const char* const kTrackNames[16];	// 01-BD .. 16-M4, the LFO destination track
extern const char* const kUpdateNames[3];	// FREE TRIG HOLD
extern const char* const kFixedParamNames[16];	// the destination names of LFO PARAM 8-23: AMD .. LFOM

// CTR-8P's AMP/EFX and ROUTE pages: its parameters 9-24 are eight (track, parameter) targets for P1-P8 (TRK1 PRM1 TRK2 PRM2 ...).
extern const char* const kPairParamNames[24];	// SYN1-SYN8, AMD .. LFOM: the target parameters
extern const Param kCtr8Pages[2][8];
extern const char* const kCtr8PageNames[2];

// The master effects' four pages (the kit's 32 master bytes: reverb, echo, EQ, dynamix), eight knobs each.
extern const char* const kMasterPageNames[4];
extern const Param kMasterPages[4][8];

}	// namespace mm::uispec
