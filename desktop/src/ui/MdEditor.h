// Machinemodule's editor: the Machinedrum's LCD pages drawn from the OS file's own glyphs and fonts, laid out as shnolk's Monomodule
// One/Six editor (header with the machine block and kit strip, the LEV column, the pages, the track column).
//   Track view   SYN | AMP/EFX over ROUTE | LFO for the selected track, and the 16 track keys on the right.
//   Mixer view   the 16 LEV columns with output meters.
//   Master view  the four master-effect pages (reverb, echo, EQ, dynamix) in place of the track pages.
// Adapted from shnolk's Monomodule (src/plugin/one/OneEditor.*), AGPL-3.0.
#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "../plugin/MdProcessor.h"
#include "LookAndFeel.h"
#include "Lcd.h"
#include "MachinePicker.h"
#include "SkinDialog.h"
#include "Widgets.h"

namespace mm
{
	class MdEditor : public juce::AudioProcessorEditor, private juce::Timer
	{
	public:
		explicit MdEditor(MdProcessor&);
		~MdEditor() override;
		void paint(juce::Graphics&) override;
		void paintOverChildren(juce::Graphics&) override {}
		void resized() override;
		void mouseDown(const juce::MouseEvent&) override;	// a press outside the picker closes it

		enum class View { Track, Mixer, Master };
		void selectTrack(int t);
		void setView(View v);
		void showMachinePicker() { m_picker.open(false); }	// snapshot: fully open at once
		void refresh() { timerCallback(); }	// snapshot: apply pending changes without the message loop

	private:
		void bindTrackPages();
		void rebuildSynPage();
		void bindFxPages();	// AMP/EFX and ROUTE: the usual parameters, or CTR-8P's eight targets
		void showConfigMenu();
		void showKitMenu();
		void applySkin(const ui::skin::Skin&, bool save);
		void skinChanged();
		void openOsChooser();
		void openKitChooser();
		void openFlashChooser();
		void timerCallback() override;
		const md::engine::MachineInfo* info(int machineId) const;
		const char* const* lfoParamNames();

		MdProcessor& m_proc;
		ui::LcdLookAndFeel m_lnf;
		ui::MachineList m_list;
		ui::LcdText m_mark, m_footerVersion, m_footerCredit, m_bpmLabel;
		ui::LcdButton m_menuButton{"MENU"};
		ui::MachineBar m_machineBar;
		ui::KitStrip m_kitStrip;
		ui::BpmReadout m_bpm;
		ui::LevelColumn m_level;
		std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> m_levelAttach;
		ui::KnobPage m_syn, m_amp, m_route, m_lfo;
		ui::TrackPanel m_tracks;
		ui::MixerPage m_mixer;
		ui::MachinePicker m_picker;
		ui::SkinDialog m_skinDialog;
		ui::MissingOsOverlay m_missingOs;
		juce::Label m_status;
		std::unique_ptr<juce::FileChooser> m_chooser;
		int m_track = 0;
		View m_view = View::Track;
		int m_shownMachine = -1;
		bool m_fxCtr8 = false;
		bool m_showStatus = false;
		int m_skinPollCountdown = 0;
		juce::String m_artPath;
		std::array<const char*, 24> m_paramNames{};
		std::array<ui::spec::Param, 8> m_synParams{};
		std::array<std::array<ui::spec::Param, 8>, 4> m_masterParams{};
		void buildMasterParams();	// the OS's own labels once the artwork is read, else the generic names
		ui::LcdToggle m_masterOn{"FX ON"};
		std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_masterOnAttach;
	};
}
