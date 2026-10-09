#include "MdEditor.h"

namespace mm
{
	MdEditor::MdEditor(MdProcessor& p) : AudioProcessorEditor(p), m_proc(p)
	{
		setSize(520, 180);
		addAndMakeVisible(m_os); addAndMakeVisible(m_kitFile); addAndMakeVisible(m_prev); addAndMakeVisible(m_next);
		addAndMakeVisible(m_status); addAndMakeVisible(m_kit);
		m_os.onClick = [this] { chooseOs(); };
		m_kitFile.onClick = [this] { chooseKit(); };
		m_prev.onClick = [this] { m_proc.stepKit(-1); timerCallback(); };
		m_next.onClick = [this] { m_proc.stepKit(1); timerCallback(); };
		m_status.setJustificationType(juce::Justification::centredLeft);
		m_kit.setJustificationType(juce::Justification::centred);
		timerCallback();
		startTimerHz(4);
	}

	void MdEditor::paint(juce::Graphics& g)
	{
		g.fillAll(juce::Colour(0xff1b1b1b));
		g.setColour(juce::Colour(0xffe8402a));
		g.setFont(juce::FontOptions(22.0f, juce::Font::bold));
		g.drawText("MACHINEMODULE", 16, 10, getWidth() - 32, 28, juce::Justification::centredLeft);
		g.setColour(juce::Colours::grey);
		g.setFont(juce::FontOptions(12.0f));
		g.drawText("Elektron Machinedrum SPS-1 UW engine. MIDI notes 36-51 play tracks 1-16. Not affiliated with Elektron.",
			16, 38, getWidth() - 32, 16, juce::Justification::centredLeft);
	}

	void MdEditor::resized()
	{
		auto r = getLocalBounds().reduced(16);
		r.removeFromTop(48);
		auto row = r.removeFromTop(28);
		m_os.setBounds(row.removeFromLeft(160));
		row.removeFromLeft(8);
		m_kitFile.setBounds(row.removeFromLeft(160));
		r.removeFromTop(8);
		row = r.removeFromTop(28);
		m_prev.setBounds(row.removeFromLeft(36));
		m_next.setBounds(row.removeFromRight(36));
		m_kit.setBounds(row);
		r.removeFromTop(8);
		m_status.setBounds(r.removeFromTop(24));
	}

	void MdEditor::timerCallback()
	{
		m_status.setText(m_proc.statusText(), juce::dontSendNotification);
		m_kit.setText(m_proc.kitLabel(), juce::dontSendNotification);
	}

	void MdEditor::chooseOs()
	{
		m_chooser = std::make_unique<juce::FileChooser>("Select your Machinedrum OS 1.63 file", juce::File(), "*.syx");
		m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc)
		{
			const auto f = fc.getResult();
			if(f.existsAsFile()) m_proc.setOsPath(f.getFullPathName());
		});
	}

	void MdEditor::chooseKit()
	{
		m_chooser = std::make_unique<juce::FileChooser>("Load a Machinedrum kit (.syx)", juce::File(), "*.syx");
		m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc)
		{
			const auto f = fc.getResult();
			if(!f.existsAsFile()) return;
			juce::String err;
			if(!m_proc.loadKitFile(f, err)) juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Machinemodule", err);
			timerCallback();
		});
	}
}
