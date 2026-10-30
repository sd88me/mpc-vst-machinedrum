// Adapted from shnolk's Monomodule (src/plugin/one/OneEditor.cpp), AGPL-3.0.
#include "MdEditor.h"
#include "MachineText.h"
#include <cmath>

namespace mm
{
	using namespace ui;

	namespace
	{
		const char* const kLfoKeysAll[8] = {"lfo_track", "lfo_param", "lfo_shp1", "lfo_shp2", "lfo_type", "lfos", "lfod", "lfom"};
		constexpr int kGap = 12;
	}

	MdEditor::MdEditor(MdProcessor& p)
		: AudioProcessorEditor(p), m_proc((skin::apply(skin::load()), p)),	// the skin first: m_lnf reads it when it is built
		  m_mark(spec::kFontBold8(), "MD", kScale, true, juce::Justification::centred),
		  m_footerVersion(spec::kFontSmall4x5(), "V0.2.1", 2),
		  m_footerCredit(spec::kFontSmall4x5(), "MACHINEMODULE - UI AFTER MONOMODULE BY SHNOLK", 2, false, juce::Justification::centredRight),
		  m_footerMachine(spec::kFontSmall4x5(), "", 2, false, juce::Justification::centred),
		  m_bpmLabel(spec::kFontBold8(), "BPM", kScale, false, juce::Justification::centredRight),
		  m_syn(p.apvts, "SYN"), m_amp(p.apvts, "AMP/EFX"), m_route(p.apvts, "ROUTE"), m_lfo(p.apvts, "LFO"),
		  m_mixer(p.apvts), m_missingOs([this] { openOsChooser(); })
	{
		setLookAndFeel(&m_lnf);
		m_menuButton.onClick = [this] { showConfigMenu(); };
		for(auto* c : std::initializer_list<juce::Component*>{&m_mark, &m_footerVersion, &m_footerCredit, &m_footerMachine, &m_bpmLabel, &m_menuButton,
			&m_masterOn, &m_machineBar, &m_kitStrip, &m_bpm, &m_level, &m_syn, &m_amp, &m_route, &m_lfo, &m_tracks, &m_mixer})
			addAndMakeVisible(*c);
		m_status.setFont(juce::Font(juce::FontOptions(12.0f)));
		addChildComponent(m_status);
		m_masterOn.setVisible(false);
		m_masterOnAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(m_proc.apvts, "master_on", m_masterOn);
		m_mixer.setVisible(false);

		m_machineBar.onOpen = [this] { if(m_picker.isOpen()) m_picker.close(); else m_picker.open(); };
		m_machineBar.onContentChanged = [this] { resized(); };
		m_picker.onOpenChanged = [this](bool open) { m_machineBar.setOpen(open); };
		addMouseListener(this, true);	// to close the picker on a press anywhere else
		m_lfo.setValuesSource(1, [this] { return lfoParamNames(); });

		m_tracks.onSelect = [this](int t) { selectTrack(t); setView(View::Track); };
		m_tracks.onLock = [this](int t) { m_proc.setTrackLocked(t, !m_proc.trackLocked(t)); timerCallback(); };
		m_tracks.onMute = [this](int t)
		{
			if(auto* prm = m_proc.apvts.getParameter(paramId(t, "mute")))
			{
				prm->beginChangeGesture();
				prm->setValueNotifyingHost(prm->getValue() >= 0.5f ? 0.0f : 1.0f);
				prm->endChangeGesture();
			}
			timerCallback();
		};
		m_mixer.onSelect = [this](int t) { selectTrack(t); };
		m_tracks.onMixer = [this] { setView(m_view == View::Mixer ? View::Track : View::Mixer); };
		m_tracks.onMaster = [this] { setView(m_view == View::Master ? View::Track : View::Master); };
		m_kitStrip.onPart = [this](ui::KitStrip::Part part)
		{
			if(part == ui::KitStrip::Part::Prev) m_proc.stepKit(-1);
			else if(part == ui::KitStrip::Part::Next) m_proc.stepKit(1);
			else showKitMenu();
			timerCallback();
		};

		addChildComponent(m_picker);
		m_skinDialog.onChanged = [this] { skinChanged(); };
		addChildComponent(m_skinDialog);
		addChildComponent(m_missingOs);
		m_missingOs.setVisible(!m_proc.engineReady());

		buildMasterParams();
		bindTrackPages();
		const int levW = 19 * kScale;
		setSize(20 + levW + 8 + 3 * ui::KnobPage::kWidth + 2 * kGap, 16 + 16 + 48 + 6 + 30 + 6 + 2 * ui::KnobPage::kHeight + kGap);
		startTimerHz(15);
		timerCallback();
	}

	MdEditor::~MdEditor() { stopTimer(); setLookAndFeel(nullptr); }

	const md::engine::MachineInfo* MdEditor::info(int machineId) const
	{
		if(m_list) for(const auto& m : *m_list) if(m.id == machineId) return &m;
		return nullptr;
	}

	// LFO PARAM's names: the destination track's SYN labels (its machine), then the fixed AMD..LFOM.
	const char* const* MdEditor::lfoParamNames()
	{
		const int dt = juce::jlimit(0, kTracks - 1, int(std::lround(m_proc.apvts.getRawParameterValue(paramId(m_track, "lfo_track"))->load())));
		const auto* mi = info(int(std::lround(m_proc.apvts.getRawParameterValue(paramId(dt, "machine"))->load())));
		for(int i = 0; i < 8; ++i) m_paramNames[size_t(i)] = mi && !mi->params[size_t(i)].empty() ? mi->params[size_t(i)].c_str() : "-";
		for(int i = 0; i < 16; ++i) m_paramNames[size_t(8 + i)] = spec::kFixedParamNames[i];
		return m_paramNames.data();
	}

	void MdEditor::buildMasterParams()
	{
		for(int g = 0; g < 4; ++g)
			for(int k = 0; k < 8; ++k)
			{
				auto p = spec::kMasterPages[g][k];
				if(const char* l = spec::masterLabel(g * 8 + k)) p.label = l;
				m_masterParams[size_t(g)][size_t(k)] = p;
			}
	}

	void MdEditor::bindTrackPages()
	{
		const int t = m_track;
		auto& apvts = m_proc.apvts;
		auto& machineParam = *apvts.getParameter(paramId(t, "machine"));
		m_machineBar.setParameter(machineParam);
		m_picker.setParameter(machineParam);
		if(m_view == View::Master)
		{
			ui::KnobPage* pages[4] = {&m_syn, &m_amp, &m_route, &m_lfo};
			for(int i = 0; i < 4; ++i)
			{
				pages[i]->setTitle(spec::kMasterPageNames[i]);
				pages[i]->setBadge({});
				pages[i]->bind(m_masterParams[size_t(i)].data(), [i](int k) { return masterId(i * 8 + k); });
			}
		}
		else
		{
			m_lfo.setTitle("LFO"); m_syn.setTitle("SYN");
			m_amp.setBadge({}); m_route.setBadge({}); m_lfo.setBadge({});
			m_fxCtr8 = false;
			bindFxPages();
			m_lfo.bind(spec::kLfoPage, [t](int k) { return paramId(t, kLfoKeysAll[k]); });
			m_shownMachine = -1;
			rebuildSynPage();
		}
		m_levelAttach.reset();
		m_levelAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, paramId(t, "level"), m_level);
		m_tracks.setSelected(t);
		m_mixer.setSelected(t);
	}

	void MdEditor::bindFxPages()
	{
		const int t = m_track;
		if(m_fxCtr8)
		{
			m_amp.setTitle(spec::kCtr8PageNames[0]); m_route.setTitle(spec::kCtr8PageNames[1]);
			m_amp.bind(spec::kCtr8Pages[0], [t](int k) { return paramId(t, kRawKeys[8 + k]); });
			m_route.bind(spec::kCtr8Pages[1], [t](int k) { return paramId(t, kRawKeys[16 + k]); });
		}
		else
		{
			m_amp.setTitle("AMP/EFX"); m_route.setTitle("ROUTE");
			m_amp.bind(spec::kAmpFxPage, [t](int k) { return paramId(t, kRawKeys[8 + k]); });
			m_route.bind(spec::kRoutePage, [t](int k) { return paramId(t, kRawKeys[16 + k]); });
		}
	}

	// SYN: the same eight parameters, described by the machine (its own labels; an unused cell is blank).
	void MdEditor::rebuildSynPage()
	{
		if(m_view == View::Master) return;
		const int mid = int(std::lround(m_proc.apvts.getRawParameterValue(paramId(m_track, "machine"))->load()));
		if(mid == m_shownMachine) return;
		m_shownMachine = mid;
		if((mid == kCtr8P) != m_fxCtr8) { m_fxCtr8 = mid == kCtr8P; bindFxPages(); }
		const auto* mi = info(mid);
		for(int k = 0; k < 8; ++k)
		{
			if(mi && !mi->params[size_t(k)].empty()) m_synParams[size_t(k)] = {mi->params[size_t(k)].c_str(), spec::Display::Numeric, mi->defaults[size_t(k)], 127, nullptr};
			else m_synParams[size_t(k)] = spec::blank();
		}
		const int t = m_track;
		m_syn.bind(m_synParams.data(), [t](int k) { return paramId(t, kRawKeys[k]); });
		m_syn.setBadge("TRACK " + juce::String(t + 1));
	}

	void MdEditor::selectTrack(int t)
	{
		t = juce::jlimit(0, kTracks - 1, t);
		if(t == m_track) return;
		if(m_picker.isOpen()) m_picker.close(false);
		m_track = t;
		bindTrackPages();
		resized();	// the machine block's width follows the track's machine
		repaint();
	}

	void MdEditor::setView(View v)
	{
		if(v == m_view) return;
		m_view = v;
		m_tracks.setMode(v == View::Mixer ? 1 : v == View::Master ? 2 : 0);
		if(m_picker.isOpen()) m_picker.close(false);
		m_mixer.setVisible(v == View::Mixer);
		for(auto* pg : {&m_syn, &m_amp, &m_route, &m_lfo}) pg->setVisible(v != View::Mixer);
		m_masterOn.setVisible(v == View::Master);
		bindTrackPages();
		resized();
		repaint();
	}

	void MdEditor::mouseDown(const juce::MouseEvent& e)
	{
		if(!m_picker.isOpen()) return;
		auto* c = e.eventComponent;
		if(c == &m_picker || m_picker.isParentOf(c) || c == &m_machineBar) return;
		m_picker.close();
	}

	void MdEditor::applySkin(const skin::Skin& s, bool save)
	{
		skin::apply(s);
		if(save) skin::save(s);
		skinChanged();
	}

	void MdEditor::skinChanged()
	{
		m_lnf.applySkin();
		sendLookAndFeelChange();	// every child re-reads the widget colours
		repaint();
	}

	void MdEditor::showKitMenu()
	{
		const int n = m_proc.kitCount();
		if(n <= 0) { openKitChooser(); return; }
		m_kitStrip.setOpen(true);
		juce::PopupMenu m;
		for(int i = 0; i < n; ++i) m.addItem(i + 1, juce::String(i + 1).paddedLeft('0', 2) + "  " + m_proc.kitName(i), true, i == m_proc.kitIndex());
		m.addSeparator();
		m.addItem(10000, "Load Kit File...");
		m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&m_kitStrip), [this](int r)
		{
			m_kitStrip.setOpen(false);
			if(r == 10000) openKitChooser();
			else if(r > 0) m_proc.selectKit(r - 1);
		});
	}

	void MdEditor::showConfigMenu()
	{
		juce::PopupMenu m;
		m.addItem(2, "Select OS File...");
		m.addItem(3, "Load Kit File...");
		m.addItem(6, "Import Flash Image (factory kits, ROM samples)...", !m_proc.importRunning());
		m.addSeparator();
		{	// the two colours of the UI
			juce::PopupMenu skins;
			const auto cur = skin::current().preset;
			static const char* const names[skin::kNumPresets] = {"MD LCD", "Black on White", "Inverted", "Low Contrast", "Custom..."};
			for(int i = 0; i < skin::kNumPresets; ++i) skins.addItem(50 + i, names[i], true, cur == skin::Preset(i));
			m.addSubMenu("Skin", skins);
		}
		m.addItem(4, "Show Engine Status", true, m_showStatus);
		m.addSeparator();
		m.addItem(5, "About Machinemodule...");
		m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&m_menuButton), [this](int r)
		{
			switch(r)
			{
				case 2: openOsChooser(); break;
				case 3: openKitChooser(); break;
				case 6: openFlashChooser(); break;
				case 4: m_showStatus = !m_showStatus; m_status.setVisible(m_showStatus); break;
				case 5:
					juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Machinemodule",
						"Machinemodule runs the Elektron Machinedrum's own sound engine from the OS file you supply.\n\n"
						"The editor follows shnolk's Monomodule (AGPL-3.0); the LCD artwork is read from your OS file.\n"
						"Not affiliated with Elektron.");
					break;
				case 50: case 51: case 52: case 53: applySkin(skin::presetSkin(skin::Preset(r - 50)), true); break;
				case 54: m_skinDialog.open(); break;
				default: break;
			}
		});
	}

	void MdEditor::openOsChooser()
	{
		const juce::File current(m_proc.osPath());
		const auto startDir = current.existsAsFile() ? current.getParentDirectory() : juce::File::getSpecialLocation(juce::File::userHomeDirectory);
		m_chooser = std::make_unique<juce::FileChooser>("Select your Machinedrum OS 1.63 file (Elektron_SPS1-1UW_OS1.63.syx)", startDir, "*.syx");
		m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc)
		{
			const auto f = fc.getResult();
			if(f.existsAsFile()) m_proc.setOsPath(f.getFullPathName());
		});
	}

	void MdEditor::openFlashChooser()
	{
		if(const auto helper = MdProcessor::importHelper(); !helper.existsAsFile())
		{
			juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Machinemodule",
				"The import needs three small helper programs (md-import, mdProbe, mdsamples) that are not installed yet.\n\n"
				"Put them in:\n" + helper.getParentDirectory().getFullPathName() + "\n\n"
				"They are built together with the plugin when it is configured with -DMM_MDPROBE=<mdProbe>, or by running "
				"tools/mdimport/build_helpers.sh in the source folder.");
			return;
		}
		m_chooser = std::make_unique<juce::FileChooser>("Select your Machinedrum UW flash image (8 MB .bin)", juce::File::getSpecialLocation(juce::File::userHomeDirectory), "*.bin");
		m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc)
		{
			const auto f = fc.getResult();
			if(f.existsAsFile()) m_proc.importFlashImage(f);
		});
	}

	void MdEditor::openKitChooser()
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

	void MdEditor::timerCallback()
	{
		const auto list = m_proc.machines();
		if(list != m_list)	// the engine finished loading: the OS's machine table is known
		{
			m_list = list;
			m_machineBar.setMachines(m_list);
			m_picker.setMachines(m_list);
			m_shownMachine = -1;
			rebuildSynPage();
			resized();
		}
		if(--m_skinPollCountdown <= 0)	// another instance chose a skin: follow it
		{
			m_skinPollCountdown = 30;
			if(!m_skinDialog.isVisible()) if(const auto s = skin::load(); s != skin::current()) { skin::apply(s); skinChanged(); }
		}
		if(m_proc.osPath() != m_artPath)	// OS file chosen or changed: draw with its LCD artwork from now on
		{
			m_artPath = m_proc.osPath();
			if(loadLcdArt(m_artPath)) { buildMasterParams(); if(m_view == View::Master) bindTrackPages(); resized(); repaint(); }
		}
		const bool ready = m_proc.engineReady();
		if(m_missingOs.isVisible() == ready) m_missingOs.setVisible(!ready);
		const auto st = m_proc.statusText();
		m_missingOs.setStatusMessage(ready || st.startsWith("Loading") ? juce::String() : st);
		m_status.setText(st, juce::dontSendNotification);
		m_status.setVisible(m_showStatus || m_proc.importRunning() || st.startsWith("Flash image"));
		m_status.setColour(juce::Label::textColourId, lcd::ink);
		m_footerMachine.setVisible(!m_status.isVisible());
		if(const auto* mi = info(int(std::lround(m_proc.apvts.getRawParameterValue(paramId(m_track, "machine"))->load()))))
			m_footerMachine.setText(ui::machineDescription(mi->id, mi->name));

		if(m_view == View::Track) rebuildSynPage();
		if(ready) m_level.setMeter(m_proc.trackPeak(m_track));
		for(int t = 0; t < kTracks; ++t)
		{
			const auto* mi = info(int(std::lround(m_proc.apvts.getRawParameterValue(paramId(t, "machine"))->load())));
			m_tracks.setTrack(t, mi ? MachineNames::group(*mi) + " " + MachineNames::shortName(*mi) : juce::String("-"));
			const float pk = ready ? m_proc.trackPeak(t) : 0.0f;
			m_tracks.setPeak(t, pk);
			m_tracks.setMuted(t, m_proc.apvts.getRawParameterValue(paramId(t, "mute"))->load() >= 0.5f);
			m_tracks.setLocked(t, m_proc.trackLocked(t));
			m_mixer.setPeak(t, pk);
		}
		m_bpm.setBpm(m_proc.hostBpm());
		const int ki = m_proc.kitIndex();
		m_kitStrip.setKit(ki >= 0 ? m_proc.kitName(ki) : juce::String("NO KIT"), ki >= 0);
	}

	void MdEditor::paint(juce::Graphics& g) { g.fillAll(lcd::paper); }

	void MdEditor::resized()
	{
		auto r = getLocalBounds().reduced(10, 8);
		auto footer = r.removeFromBottom(16);
		m_footerVersion.setBounds(footer.removeFromLeft(200));
		m_footerCredit.setBounds(footer.removeFromRight(640));
		m_status.setBounds(footer.reduced(8, 0));
		m_footerMachine.setBounds(footer.reduced(8, 0));

		const int levW = 19 * kScale, top = r.getY();
		auto header = r.removeFromTop(48);
		m_mark.setBounds(header.removeFromLeft(levW).withY(top).withHeight(18 * kScale));	// where Monomodule's logo stands
		header.removeFromLeft(8);
		// the machine block hangs from the top edge beside the mark, down to just above the pages
		m_machineBar.setBounds(header.getX(), top, m_machineBar.preferredWidth(), m_machineBar.preferredHeight());
		auto headerTop = header.removeFromTop(30);
		m_menuButton.setBounds(headerTop.removeFromRight(32 * kScale));
		headerTop.removeFromRight(10);
		m_masterOn.setBounds(headerTop.removeFromRight(m_masterOn.isVisible() ? 34 * kScale : 0));
		headerTop.removeFromRight(10);
		m_bpm.setBounds(headerTop.removeFromRight(62 * kScale));	// "120.0" in the tall digit face is 59 px
		m_bpmLabel.setBounds(headerTop.removeFromRight(22 * kScale));
		{	// the kit strip: between the machine block and the tempo, as wide as that leaves (at most 130 LCD px)
			const int x0 = m_machineBar.getRight() + 12, x1 = m_bpmLabel.getX() - 4;
			const int w = juce::jlimit(0, 130 * kScale, x1 - x0);
			m_kitStrip.setBounds(x0 + (x1 - x0 - w) / 2, top, w, ui::KitStrip::kLcdH * kScale);
		}
		r.removeFromTop(6);

		auto body = r;
		auto lev = body.removeFromLeft(levW);
		body.removeFromLeft(8);
		body.removeFromTop(36);	// the machine block's lower part and the gap under it
		auto col = body.removeFromRight(ui::KnobPage::kWidth);	// the track keys stand beside the pages, as tall as the two rows
		body.removeFromRight(kGap);
		m_tracks.setBounds(col.getX(), col.getY() - ui::TrackPanel::kKeysH * kScale, col.getWidth(), ui::TrackPanel::kLcdH * kScale);	// its title lines up with the pages'; the MIXER / MASTER keys stand above it under BPM and MENU
		// LEV: its label sits under the mark and its frame (11 LCD rows down) starts level with the pages
		lev = lev.withTop(body.getY() - 11 * kScale);
		m_level.setBounds(lev.withHeight((lev.getHeight() / kScale) * kScale));

		auto row1 = body.removeFromTop(ui::KnobPage::kHeight);
		body.removeFromTop(kGap);
		auto row2 = body.removeFromTop(ui::KnobPage::kHeight);
		m_syn.setBounds(row1.removeFromLeft(ui::KnobPage::kWidth)); row1.removeFromLeft(kGap);
		m_amp.setBounds(row1.removeFromLeft(ui::KnobPage::kWidth));
		m_route.setBounds(row2.removeFromLeft(ui::KnobPage::kWidth)); row2.removeFromLeft(kGap);
		m_lfo.setBounds(row2.removeFromLeft(ui::KnobPage::kWidth));
		const juce::Rectangle<int> pages(m_syn.getX(), m_syn.getY(), m_amp.getRight() - m_syn.getX(), m_lfo.getBottom() - m_syn.getY());
		m_mixer.setBounds(pages.withWidth((pages.getWidth() / kScale) * kScale).withHeight((pages.getHeight() / kScale) * kScale));
		m_picker.setTargetBounds(pages);	// the pages' area (the track keys stay in view)
		m_skinDialog.setBounds(getLocalBounds());
		m_missingOs.setBounds(getLocalBounds());
	}
}
