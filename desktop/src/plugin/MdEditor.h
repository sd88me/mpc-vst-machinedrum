// Machinedrum-style editor: 16 track buttons, pages SYN / FX / ROUTE / LFO / MASTER / MIXER drawn like the unit's LCD,
// a machine picker and a kit browser. Everything is laid out on a fixed 720 x 480 surface that is scaled to the window.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <memory>
#include <vector>
#include "MdProcessor.h"

namespace mm
{
	// One LCD-style dial: arc and value, label above. Drag up/down (shift = fine), wheel, double-click = default.
	class Dial : public juce::Component
	{
	public:
		Dial() = default;
		void bind(juce::AudioProcessorValueTreeState&, const juce::String& paramId);
		void unbind() { m_att.reset(); m_param = nullptr; repaint(); }
		void setLabel(const juce::String& s) { if(s != m_label) { m_label = s; repaint(); } }
		void paint(juce::Graphics&) override;
		void mouseDown(const juce::MouseEvent&) override;
		void mouseDrag(const juce::MouseEvent&) override;
		void mouseUp(const juce::MouseEvent&) override;
		void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
		void mouseDoubleClick(const juce::MouseEvent&) override;

	private:
		void setValue(float v);	// 0..127
		float value() const { return m_param ? m_param->convertFrom0to1(m_param->getValue()) : 0.0f; }
		juce::RangedAudioParameter* m_param = nullptr;
		std::unique_ptr<juce::ParameterAttachment> m_att;
		juce::String m_label;
		float m_dragStart = 0.0f;
	};

	// Vertical bar for the mixer page: click or drag to set, double-click = 100; click on the name selects the track.
	class LevelBar : public juce::Component
	{
	public:
		std::function<void()> onSelect;
		void bind(juce::AudioProcessorValueTreeState&, const juce::String& paramId);
		void setCaption(const juce::String& number, const juce::String& machine, bool selected);
		void paint(juce::Graphics&) override;
		void mouseDown(const juce::MouseEvent&) override;
		void mouseDrag(const juce::MouseEvent&) override;
		void mouseUp(const juce::MouseEvent&) override;
		void mouseDoubleClick(const juce::MouseEvent&) override;

	private:
		void setFromY(int y);
		juce::RangedAudioParameter* m_param = nullptr;
		std::unique_ptr<juce::ParameterAttachment> m_att;
		juce::String m_num, m_machine;
		bool m_sel = false, m_dragging = false;
	};

	class MdEditor : public juce::AudioProcessorEditor, private juce::Timer
	{
	public:
		explicit MdEditor(MdProcessor&);
		~MdEditor() override;
		void paint(juce::Graphics&) override;
		void resized() override;

		enum Page { Syn, Fx, Route, Lfo, Master, Mixer, kPages };
		void show(Page p, int track) { selectTrack(track); selectPage(p); }	// for the snapshot test

	private:
		struct Root;
		void timerCallback() override;
		void selectTrack(int);
		void selectPage(Page);
		void rebind();	// after a track or page change
		void refreshLabels();
		void fillMachineBox();
		void chooseOs();
		void chooseKit();
		void layoutRoot();

		MdProcessor& m_proc;
		std::unique_ptr<Root> m_root;
		int m_track = 0;
		Page m_page = Syn;
		int m_shownMachine = -2;
		const void* m_shownList = nullptr;
	};
}
