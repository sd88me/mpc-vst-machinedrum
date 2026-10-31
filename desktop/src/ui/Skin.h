// Skins: the two colours the whole UI is drawn in (the LCD's ink and paper), chosen by the user and kept in the plugin's
// settings file (the user's Machinemodule data folder, shared by every instance). Every drawing routine reads lcd::ink /
// lcd::paper (Lcd.h) at paint time, so applying a skin is a matter of setting the two colours and repainting.
//   MD LCD        the Machinedrum's own: maroon pixels on the red backlight (as the MPC skin; sampled from a photo)
//   BLACK/WHITE   the Monomodule default
//   INVERTED      white on black
//   LOW CONTRAST  dark grey on light grey
//   CUSTOM        any two colours, entered as hex
// Adapted from shnolk's Monomodule (src/plugin/Skin.h, SharedSettings.h), AGPL-3.0.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace mm::ui::skin {

enum class Preset : int { MdLcd = 0, BlackWhite, Inverted, LowContrast, Custom };
constexpr int kNumPresets = 5;
constexpr const char* kPresetNames[kNumPresets] = {"MD LCD", "BLACK ON WHITE", "INVERTED", "LOW CONTRAST", "CUSTOM"};

inline juce::File settingsFile()
{
	auto base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
#if JUCE_MAC
	base = base.getChildFile("Application Support");
#endif
	return base.getChildFile("Machinemodule").getChildFile("settings.xml");
}

inline juce::String loadSetting(const juce::String& name, const juce::String& fallback = {})
{
	if(auto xml = juce::parseXML(settingsFile()))
		if(xml->hasTagName("MachinemoduleSettings")) return xml->getStringAttribute(name, fallback);
	return fallback;
}

inline void saveSetting(const juce::String& name, const juce::String& value)
{
	auto f = settingsFile();
	f.getParentDirectory().createDirectory();
	auto xml = juce::parseXML(f);
	if(!xml || !xml->hasTagName("MachinemoduleSettings")) xml = std::make_unique<juce::XmlElement>("MachinemoduleSettings");
	xml->setAttribute(name, value);
	xml->writeTo(f);
}

struct Skin
{
	Preset preset = Preset::MdLcd;
	juce::Colour ink{0xff5e0c0c}, paper{0xffff4836};	// the colours in use (for a preset: its own)
	bool operator==(const Skin& o) const { return preset == o.preset && ink == o.ink && paper == o.paper; }
	bool operator!=(const Skin& o) const { return !(*this == o); }
};

inline Skin presetSkin(Preset p, juce::Colour customInk = juce::Colour(0xff000000), juce::Colour customPaper = juce::Colour(0xffffffff))
{
	switch(p)
	{
		case Preset::BlackWhite: return {p, juce::Colour(0xff000000), juce::Colour(0xffffffff)};
		case Preset::Inverted: return {p, juce::Colour(0xffffffff), juce::Colour(0xff000000)};
		case Preset::LowContrast: return {p, juce::Colour(0xff5c5c5c), juce::Colour(0xffc4c4c4)};
		case Preset::Custom: return {p, customInk.withAlpha(1.0f), customPaper.withAlpha(1.0f)};
		default: return {Preset::MdLcd, juce::Colour(0xff5e0c0c), juce::Colour(0xffff4836)};
	}
}

// The colours in use. Lcd.h's lcd::ink / lcd::paper are references to these.
inline juce::Colour& inkColour() { static juce::Colour c{0xff5e0c0c}; return c; }
inline juce::Colour& paperColour() { static juce::Colour c{0xffff4836}; return c; }
inline Skin& current() { static Skin s; return s; }

// "RRGGBB" or "#RRGGBB" (also "RGB"); false when the text is not a colour.
inline bool parseHex(juce::String text, juce::Colour& out)
{
	text = text.trim().trimCharactersAtStart("#").toUpperCase();
	if(text.length() == 3) text = juce::String::charToString(text[0]) + text[0] + text[1] + text[1] + text[2] + text[2];
	if(text.length() != 6 || !text.containsOnly("0123456789ABCDEF")) return false;
	out = juce::Colour(0xff000000u | uint32_t(text.getHexValue32()));
	return true;
}
inline juce::String hexOf(juce::Colour c) { return c.toDisplayString(false).toUpperCase(); }

// Makes `s` the skin in use (message thread; the caller repaints).
inline void apply(const Skin& s) { current() = s; inkColour() = s.ink; paperColour() = s.paper; }

inline Skin load()
{
	const int p = juce::jlimit(0, kNumPresets - 1, loadSetting("skin", "0").getIntValue());
	juce::Colour ink(0xff000000), paper(0xffffffff);
	parseHex(loadSetting("skinInk"), ink);
	parseHex(loadSetting("skinPaper"), paper);
	return presetSkin(Preset(p), ink, paper);
}
inline void save(const Skin& s)
{
	saveSetting("skin", juce::String(int(s.preset)));
	if(s.preset == Preset::Custom) { saveSetting("skinInk", hexOf(s.ink)); saveSetting("skinPaper", hexOf(s.paper)); }
}

inline juce::Colour savedCustomInk() { juce::Colour c = inkColour(); parseHex(loadSetting("skinInk"), c); return c; }
inline juce::Colour savedCustomPaper() { juce::Colour c = paperColour(); parseHex(loadSetting("skinPaper"), c); return c; }

}	// namespace mm::ui::skin
