// Phase 1 editor: OS file, kit file, kit stepping and status. The Machinedrum LCD editor is Phase 3.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "MdProcessor.h"

namespace mm
{
	class MdEditor : public juce::AudioProcessorEditor, private juce::Timer
	{
	public:
		explicit MdEditor(MdProcessor&);
		~MdEditor() override = default;
		void paint(juce::Graphics&) override;
		void resized() override;

	private:
		void timerCallback() override;
		void chooseOs();
		void chooseKit();

		MdProcessor& m_proc;
		juce::TextButton m_os{"Select OS file..."}, m_kitFile{"Load kit file..."}, m_prev{"<"}, m_next{">"};
		juce::Label m_status, m_kit;
		std::unique_ptr<juce::FileChooser> m_chooser;
	};
}
