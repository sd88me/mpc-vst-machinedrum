// The machine picker: fills the page area with one column per machine group (the MD OS's own machine table: GND, P-I, TRX, EFM,
// E12 and the ROM machines, split into columns of 16), the group's name in the title face, then its machines as rows in the LCD faces.
// The current machine's row is inverted; a click assigns and closes. The machine block above toggles it; Escape or a click elsewhere
// cancels. Opening unrolls it from the top edge over the pages (and closing rolls it back up).
// Adapted from shnolk's Monomodule (src/plugin/one/MachinePicker.*), AGPL-3.0.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <vector>
#include "Lcd.h"
#include "MachineRunner.h"

namespace mm::ui {

using MachineList = std::shared_ptr<const std::vector<md::engine::MachineInfo>>;

// What the UI shows of a machine: its group ("TRX") and the rest of the name ("B2"), split as the OS names them.
struct MachineNames
{
	static juce::String group(const md::engine::MachineInfo& m) { return juce::String(m.name).substring(0, 3); }
	static juce::String shortName(const md::engine::MachineInfo& m) { return juce::String(m.name).substring(3); }
	// Offered in the picker: MIDI and control machines make no audio, INP has no audio input here, RAM has no sampling; ROM slots
	// above 32 are empty in the factory image.
	static bool offered(const md::engine::MachineInfo& m)
	{
		const auto g = group(m);
		if(g == "MID" || g == "CTR" || g == "INP" || g == "RAM" || m.name.empty()) return false;
		if(g == "ROM" && shortName(m).getIntValue() > 32) return false;
		return true;
	}
};

class MachinePicker : public juce::Component, private juce::Timer
{
public:
	MachinePicker();
	void setMachines(MachineList list);
	void setParameter(juce::RangedAudioParameter& machineParam);	// the machine parameter of the shown track
	void setTargetBounds(juce::Rectangle<int> fullyOpen);	// the page area, in the parent
	void open(bool animate = true);
	void close(bool animate = true);
	bool isOpen() const { return m_wantOpen; }	// requested state (true while unrolling)
	std::function<void(bool)> onOpenChanged;	// the machine block follows (arrow direction)
	void paint(juce::Graphics&) override;
	void mouseMove(const juce::MouseEvent&) override;
	void mouseExit(const juce::MouseEvent&) override;
	void mouseDown(const juce::MouseEvent&) override;
	bool keyPressed(const juce::KeyPress&) override;

private:
	struct Row { int id; juce::String name; juce::Rectangle<int> bounds; };
	struct Column { juce::String group; std::vector<Row> rows; juce::Rectangle<int> bounds, header; };
	void build();
	void layout();
	const Row* rowAt(juce::Point<int>) const;
	void pick(int id);
	void timerCallback() override;
	void applyAnimation();

	MachineList m_list;
	std::unique_ptr<juce::ParameterAttachment> m_attach;
	int m_current = 0;	// machine id the parameter holds
	int m_cursor = -1;	// hovered / keyboard-selected machine id
	std::vector<Column> m_columns;
	juce::Rectangle<int> m_target;	// bounds when fully open; the layout is always computed for these
	bool m_wantOpen = false;
	float m_anim = 0.0f;	// 0 = rolled up, 1 = fully open
};

}	// namespace mm::ui
