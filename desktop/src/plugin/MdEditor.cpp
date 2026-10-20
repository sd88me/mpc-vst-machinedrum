#include "MdEditor.h"
#include <cmath>

namespace mm
{
	namespace
	{
		constexpr int kW = 720, kH = 480;
		const juce::Colour kBezel(0xff1b1b1b), kLcd(0xff0b0c0a), kLcdEdge(0xff2c2c2a), kInk(0xffffa13a), kInkDim(0xff6b4a22),
			kAccent(0xffe8402a), kHot(0xffffd08a);

		juce::Font lcdFont(float h, bool bold = false)
		{
			return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), h, bold ? juce::Font::bold : juce::Font::plain));
		}

		const char* const kPageNames[MdEditor::kPages] = {"SYN", "FX", "ROUTE", "LFO", "MASTER", "MIXER"};
		const char* const kFxLabels[8] = {"AMD", "AMF", "EQF", "EQG", "FLTF", "FLTW", "FLTQ", "SRR"};
		const char* const kRouteLabels[8] = {"DIST", "VOL", "PAN", "DEL", "REV", "LFOS", "LFOD", "LFOM"};
		// raw order (kRawKeys) of each page's eight dials
		const int kPageFirst[3] = {0, 8, 16};
		const char* const kMasterGroups[4] = {"REV", "ECHO", "EQ", "DYN"};
		const char* const kLfoTypes[3] = {"FREE", "TRIG", "HOLD"};

		juce::String paramLabel(int i)
		{
			if(i < 8) return "SYN" + juce::String(i + 1);
			return juce::String(kRawKeys[i]).toUpperCase();
		}
	}

	// ---- Dial ----------------------------------------------------------------------------------------------------

	void Dial::bind(juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
	{
		m_att.reset();
		m_param = apvts.getParameter(id);
		if(!m_param) return;
		m_att = std::make_unique<juce::ParameterAttachment>(*m_param, [this](float) { repaint(); });
		m_att->sendInitialUpdate();
	}

	void Dial::setValue(float v)
	{
		if(m_att) m_att->setValueAsPartOfGesture(juce::jlimit(0.0f, 127.0f, std::round(v)));
	}

	void Dial::mouseDown(const juce::MouseEvent&)
	{
		if(!m_att) return;
		m_dragStart = value();
		m_att->beginGesture();
	}

	void Dial::mouseDrag(const juce::MouseEvent& e)
	{
		if(!m_att) return;
		const float perPixel = e.mods.isShiftDown() ? 0.15f : 0.8f;
		setValue(m_dragStart - float(e.getDistanceFromDragStartY()) * perPixel);
	}

	void Dial::mouseUp(const juce::MouseEvent&) { if(m_att) m_att->endGesture(); }

	void Dial::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w)
	{
		if(!m_att) return;
		const float step = w.deltaY > 0 ? 1.0f : (w.deltaY < 0 ? -1.0f : 0.0f);
		m_att->setValueAsCompleteGesture(juce::jlimit(0.0f, 127.0f, value() + step));
	}

	void Dial::mouseDoubleClick(const juce::MouseEvent&)
	{
		if(m_att && m_param) m_att->setValueAsCompleteGesture(m_param->convertFrom0to1(m_param->getDefaultValue()));
	}

	void Dial::paint(juce::Graphics& g)
	{
		auto r = getLocalBounds().toFloat();
		g.setColour(kInk);
		g.setFont(lcdFont(13.0f, true));
		g.drawText(m_label, r.removeFromTop(16).toNearestInt(), juce::Justification::centred);
		const float v = value() / 127.0f;
		auto d = r.reduced(4.0f);
		const float size = juce::jmin(d.getWidth(), d.getHeight() - 14.0f);
		auto c = juce::Rectangle<float>(size, size).withCentre({d.getCentreX(), d.getY() + size * 0.5f});
		const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = juce::MathConstants<float>::pi * 2.75f;
		juce::Path track;
		track.addCentredArc(c.getCentreX(), c.getCentreY(), size * 0.5f - 2.0f, size * 0.5f - 2.0f, 0.0f, a0, a1, true);
		g.setColour(kInkDim);
		g.strokePath(track, juce::PathStrokeType(3.0f));
		juce::Path val;
		val.addCentredArc(c.getCentreX(), c.getCentreY(), size * 0.5f - 2.0f, size * 0.5f - 2.0f, 0.0f, a0, a0 + (a1 - a0) * v, true);
		g.setColour(m_param ? kInk : kInkDim);
		g.strokePath(val, juce::PathStrokeType(3.0f));
		const float ang = a0 + (a1 - a0) * v;
		const float rr = size * 0.5f - 2.0f;
		g.setColour(kHot);
		g.drawLine(c.getCentreX() + std::sin(ang) * rr * 0.35f, c.getCentreY() - std::cos(ang) * rr * 0.35f,
			c.getCentreX() + std::sin(ang) * rr * 0.9f, c.getCentreY() - std::cos(ang) * rr * 0.9f, 2.0f);
		g.setColour(kInk);
		g.setFont(lcdFont(13.0f));
		g.drawText(m_param ? juce::String(int(std::round(value()))) : "-", d.withTop(d.getBottom() - 14.0f).toNearestInt(), juce::Justification::centred);
	}

	// ---- LevelBar ------------------------------------------------------------------------------------------------

	void LevelBar::bind(juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
	{
		m_param = apvts.getParameter(id);
		if(m_param) { m_att = std::make_unique<juce::ParameterAttachment>(*m_param, [this](float) { repaint(); }); m_att->sendInitialUpdate(); }
	}

	void LevelBar::setCaption(const juce::String& number, const juce::String& machine, bool selected)
	{
		if(number != m_num || machine != m_machine || selected != m_sel) { m_num = number; m_machine = machine; m_sel = selected; repaint(); }
	}

	void LevelBar::setFromY(int y)
	{
		if(!m_att) return;
		const float top = 18.0f, bottom = float(getHeight()) - 34.0f;
		const float v = 127.0f * (1.0f - juce::jlimit(0.0f, 1.0f, (float(y) - top) / (bottom - top)));
		m_att->setValueAsPartOfGesture(std::round(v));
	}

	void LevelBar::mouseDown(const juce::MouseEvent& e)
	{
		if(e.y > getHeight() - 32) { if(onSelect) onSelect(); return; }
		if(!m_att) return;
		m_dragging = true;
		m_att->beginGesture();
		setFromY(e.y);
	}

	void LevelBar::mouseDrag(const juce::MouseEvent& e) { if(m_dragging) setFromY(e.y); }

	void LevelBar::mouseUp(const juce::MouseEvent&)
	{
		if(m_dragging && m_att) m_att->endGesture();
		m_dragging = false;
	}

	void LevelBar::mouseDoubleClick(const juce::MouseEvent& e)
	{
		if(e.y <= getHeight() - 32 && m_att && m_param) m_att->setValueAsCompleteGesture(100.0f);
	}

	void LevelBar::paint(juce::Graphics& g)
	{
		const float v = m_param ? m_param->convertFrom0to1(m_param->getValue()) / 127.0f : 0.0f;
		auto r = getLocalBounds().toFloat();
		auto cap = r.removeFromBottom(32.0f);
		g.setColour(m_sel ? kHot : kInk);
		g.setFont(lcdFont(12.0f, true));
		g.drawText(m_num, cap.removeFromTop(14.0f).toNearestInt(), juce::Justification::centred);
		g.setFont(lcdFont(10.0f));
		g.drawText(m_machine, cap.toNearestInt(), juce::Justification::centred);
		auto bar = r.withTrimmedTop(18.0f).reduced(r.getWidth() * 0.3f, 0.0f);
		g.setColour(kInkDim);
		g.drawRect(bar, 1.0f);
		g.setColour(kInk);
		const float h = bar.getHeight() * v;
		g.fillRect(bar.withTop(bar.getBottom() - h).reduced(2.0f, 0.0f).withTrimmedBottom(1.0f));
		g.setFont(lcdFont(11.0f));
		g.drawText(m_param ? juce::String(int(std::round(v * 127.0f))) : "-", r.removeFromTop(16.0f).toNearestInt(), juce::Justification::centred);
	}

	// ---- editor root ---------------------------------------------------------------------------------------------

	// A combo box bound to an integer parameter whose value v is shown as the item with id v + 1.
	struct Choice : juce::Component
	{
		juce::ComboBox box;
		std::unique_ptr<juce::ParameterAttachment> att;
		bool updating = false;
		Choice() { addAndMakeVisible(box); box.onChange = [this] { if(!updating && att && box.getSelectedId() > 0) att->setValueAsCompleteGesture(float(box.getSelectedId() - 1)); }; }
		void resized() override { box.setBounds(getLocalBounds()); }
		void bind(juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
		{
			att.reset();
			if(auto* p = apvts.getParameter(id))
			{
				att = std::make_unique<juce::ParameterAttachment>(*p, [this](float v)
				{
					updating = true;
					box.setSelectedId(int(std::round(v)) + 1, juce::dontSendNotification);
					updating = false;
				});
				att->sendInitialUpdate();
			}
		}
		float value() const { return box.getSelectedId() > 0 ? float(box.getSelectedId() - 1) : 0.0f; }
	};

	struct MdEditor::Root : juce::Component
	{
		explicit Root(MdEditor& e) : ed(e)
		{
			for(int t = 0; t < kTracks; ++t)
			{
				auto& b = trackBtn[size_t(t)];
				b.setButtonText(juce::String(t + 1));
				b.setClickingTogglesState(true);
				b.setRadioGroupId(1);
				b.onClick = [this, t] { ed.selectTrack(t); };
				addAndMakeVisible(b);
			}
			for(int p = 0; p < kPages; ++p)
			{
				auto& b = pageBtn[size_t(p)];
				b.setButtonText(kPageNames[p]);
				b.setClickingTogglesState(true);
				b.setRadioGroupId(2);
				b.onClick = [this, p] { ed.selectPage(Page(p)); };
				addAndMakeVisible(b);
			}
			for(auto& b : trackBtn) { b.setColour(juce::TextButton::buttonOnColourId, kAccent); b.setColour(juce::TextButton::textColourOnId, juce::Colours::white); }
			for(auto& b : pageBtn) { b.setColour(juce::TextButton::buttonOnColourId, kAccent); b.setColour(juce::TextButton::textColourOnId, juce::Colours::white); }
			trackBtn[0].setToggleState(true, juce::dontSendNotification);
			pageBtn[0].setToggleState(true, juce::dontSendNotification);
			for(auto* b : {&osBtn, &kitFileBtn, &prevBtn, &nextBtn}) addAndMakeVisible(*b);
			addAndMakeVisible(status); addAndMakeVisible(kitLabel); addAndMakeVisible(machineBox); addAndMakeVisible(masterOn);
			status.setJustificationType(juce::Justification::centredRight);
			status.setColour(juce::Label::textColourId, juce::Colours::grey);
			status.setFont(juce::FontOptions(12.0f));
			kitLabel.setJustificationType(juce::Justification::centred);
			kitLabel.setColour(juce::Label::textColourId, kInk);
			kitLabel.setFont(lcdFont(15.0f, true));
			machineBox.box.setTextWhenNothingSelected("(load the OS file)");
			for(auto& d : dials) addAndMakeVisible(d);
			for(auto& d : masterDials) addAndMakeVisible(d);
			for(auto& b : bars) addAndMakeVisible(b);
			const char* lfoNames[5] = {"DEST TRACK", "DEST PARAM", "SHAPE 1", "SHAPE 2", "UPDATE"};
			for(int i = 0; i < 5; ++i)
			{
				addAndMakeVisible(lfo[size_t(i)]);
				lfoLabel[size_t(i)].setText(lfoNames[i], juce::dontSendNotification);
				lfoLabel[size_t(i)].setColour(juce::Label::textColourId, kInk);
				lfoLabel[size_t(i)].setFont(lcdFont(13.0f, true));
				addAndMakeVisible(lfoLabel[size_t(i)]);
			}
			for(int t = 0; t < kTracks; ++t) lfo[0].box.addItem("TRACK " + juce::String(t + 1), t + 1);
			for(int i = 0; i < kRaw; ++i) lfo[1].box.addItem(paramLabel(i), i + 1);
			for(int s = 0; s < 6; ++s) { lfo[2].box.addItem("SHAPE " + juce::String(s + 1), s + 1); lfo[3].box.addItem("SHAPE " + juce::String(s + 1), s + 1); }
			for(int s = 0; s < 3; ++s) lfo[4].box.addItem(kLfoTypes[s], s + 1);
			masterOn.setButtonText("MASTER FX ON");
			masterAtt = std::make_unique<juce::ButtonParameterAttachment>(*ed.m_proc.apvts.getParameter("master_on"), masterOn);
			prevBtn.setButtonText("<"); nextBtn.setButtonText(">");
			osBtn.setButtonText("OS file..."); kitFileBtn.setButtonText("Load kit file...");
			for(int i = 0; i < kMasterBytes; ++i)
			{
				masterDials[size_t(i)].bind(ed.m_proc.apvts, masterId(i));
				masterDials[size_t(i)].setLabel(juce::String(kMasterGroups[i / 8]) + " " + juce::String(i % 8 + 1));
			}
			for(int t = 0; t < kTracks; ++t)
			{
				bars[size_t(t)].bind(ed.m_proc.apvts, paramId(t, "level"));
				bars[size_t(t)].onSelect = [this, t] { ed.selectTrack(t); };
			}
		}

		void paint(juce::Graphics& g) override
		{
			g.fillAll(kBezel);
			g.setColour(kAccent);
			g.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
			g.drawText("MACHINEMODULE", 16, 8, 240, 28, juce::Justification::centredLeft);
			g.setColour(kLcdEdge);
			g.fillRoundedRectangle(lcdArea.expanded(6).toFloat(), 8.0f);
			g.setColour(kLcd);
			g.fillRoundedRectangle(lcdArea.toFloat(), 4.0f);
			g.setColour(juce::Colours::grey);
			g.setFont(juce::FontOptions(11.0f));
			g.drawText("Elektron Machinedrum SPS-1 UW engine. MIDI notes 36-51 (and 0-15) play tracks 1-16. Not affiliated with Elektron.",
				16, kH - 20, kW - 32, 14, juce::Justification::centredLeft);
			if(ed.m_page == Master)
			{
				g.setColour(kInk);
				g.setFont(lcdFont(13.0f, true));
				for(int row = 0; row < 4; ++row)
					g.drawText(kMasterGroups[row], lcdArea.getX() + 6, lcdArea.getY() + 40 + row * 62, 52, 20, juce::Justification::centredLeft);
			}
			if(ed.m_page != Mixer && ed.m_page != Master)
			{
				g.setColour(kInkDim);
				g.setFont(lcdFont(12.0f));
				g.drawText("TRACK " + juce::String(ed.m_track + 1), lcdArea.getX() + 10, lcdArea.getY() + 6, 90, 24, juce::Justification::centredLeft);
			}
		}

		void resized() override
		{
			for(int t = 0; t < kTracks; ++t) trackBtn[size_t(t)].setBounds(40 + t * 40, 44, 38, 26);
			const int pw = 100;
			for(int p = 0; p < kPages; ++p) pageBtn[size_t(p)].setBounds(60 + p * (pw + 8), 80, pw, 26);
			osBtn.setBounds(300, 8, 110, 26);
			kitFileBtn.setBounds(416, 8, 130, 26);
			status.setBounds(550, 8, 154, 26);
			prevBtn.setBounds(60, 116, 36, 24);
			nextBtn.setBounds(624, 116, 36, 24);
			kitLabel.setBounds(100, 116, 520, 24);
			lcdArea = {30, 148, kW - 60, 300};
			machineBox.setBounds(lcdArea.getX() + 110, lcdArea.getY() + 6, 260, 24);
			for(int i = 0; i < 8; ++i)
				dials[size_t(i)].setBounds(lcdArea.getX() + 40 + (i % 4) * 150, lcdArea.getY() + 50 + (i / 4) * 120, 130, 100);
			for(int i = 0; i < kMasterBytes; ++i)
				masterDials[size_t(i)].setBounds(lcdArea.getX() + 62 + (i % 8) * 74, lcdArea.getY() + 34 + (i / 8) * 62, 72, 60);
			masterOn.setBounds(lcdArea.getRight() - 170, lcdArea.getY() + 4, 160, 24);
			for(int i = 0; i < 5; ++i)
			{
				lfoLabel[size_t(i)].setBounds(lcdArea.getX() + 60, lcdArea.getY() + 50 + i * 42, 140, 26);
				lfo[size_t(i)].setBounds(lcdArea.getX() + 210, lcdArea.getY() + 50 + i * 42, 220, 26);
			}
			for(int t = 0; t < kTracks; ++t)
				bars[size_t(t)].setBounds(lcdArea.getX() + 10 + t * 40, lcdArea.getY() + 8, 40, lcdArea.getHeight() - 16);
		}

		MdEditor& ed;
		juce::Rectangle<int> lcdArea;
		std::array<juce::TextButton, kTracks> trackBtn;
		std::array<juce::TextButton, kPages> pageBtn;
		juce::TextButton osBtn, kitFileBtn, prevBtn, nextBtn;
		juce::Label status, kitLabel;
		Choice machineBox;
		juce::ToggleButton masterOn;
		std::unique_ptr<juce::ButtonParameterAttachment> masterAtt;
		std::array<Dial, 8> dials;
		std::array<Dial, kMasterBytes> masterDials;
		std::array<LevelBar, kTracks> bars;
		std::array<Choice, 5> lfo;
		std::array<juce::Label, 5> lfoLabel;
		std::unique_ptr<juce::FileChooser> chooser;
	};

	// ---- MdEditor ------------------------------------------------------------------------------------------------

	MdEditor::MdEditor(MdProcessor& p) : AudioProcessorEditor(p), m_proc(p)
	{
		m_root = std::make_unique<Root>(*this);
		addAndMakeVisible(*m_root);
		m_root->osBtn.onClick = [this] { chooseOs(); };
		m_root->kitFileBtn.onClick = [this] { chooseKit(); };
		m_root->prevBtn.onClick = [this] { m_proc.stepKit(-1); timerCallback(); };
		m_root->nextBtn.onClick = [this] { m_proc.stepKit(1); timerCallback(); };
		setResizable(true, true);
		setResizeLimits(kW / 2, kH / 2, kW * 3, kH * 3);
		getConstrainer()->setFixedAspectRatio(double(kW) / kH);
		setSize(kW, kH);
		m_root->setBounds(0, 0, kW, kH);
		rebind();
		timerCallback();
		startTimerHz(8);
	}

	MdEditor::~MdEditor() { stopTimer(); }

	void MdEditor::paint(juce::Graphics& g) { g.fillAll(kBezel); }

	void MdEditor::resized()
	{
		m_root->setTransform(juce::AffineTransform::scale(float(getWidth()) / kW));
	}

	void MdEditor::selectTrack(int t)
	{
		m_track = juce::jlimit(0, kTracks - 1, t);
		for(int i = 0; i < kTracks; ++i) m_root->trackBtn[size_t(i)].setToggleState(i == m_track, juce::dontSendNotification);
		rebind();
	}

	void MdEditor::selectPage(Page p)
	{
		m_page = p;
		for(int i = 0; i < kPages; ++i) m_root->pageBtn[size_t(i)].setToggleState(i == p, juce::dontSendNotification);
		rebind();
	}

	void MdEditor::rebind()
	{
		auto& r = *m_root;
		auto& apvts = m_proc.apvts;
		const bool trackPage = m_page == Syn || m_page == Fx || m_page == Route;
		for(auto& d : r.dials) { d.setVisible(trackPage); d.unbind(); }
		for(auto& d : r.masterDials) d.setVisible(m_page == Master);
		r.masterOn.setVisible(m_page == Master);
		for(auto& b : r.bars) b.setVisible(m_page == Mixer);
		for(int i = 0; i < 5; ++i) { r.lfo[size_t(i)].setVisible(m_page == Lfo); r.lfoLabel[size_t(i)].setVisible(m_page == Lfo); }
		r.machineBox.setVisible(trackPage || m_page == Lfo);
		if(trackPage)
		{
			const int first = kPageFirst[m_page == Syn ? 0 : m_page == Fx ? 1 : 2];
			for(int i = 0; i < 8; ++i) r.dials[size_t(i)].bind(apvts, paramId(m_track, kRawKeys[first + i]));
		}
		if(m_page == Lfo)
			for(int i = 0; i < 5; ++i) r.lfo[size_t(i)].bind(apvts, paramId(m_track, kLfoKeys[i]));
		r.machineBox.bind(apvts, paramId(m_track, "machine"));
		m_shownMachine = -2;
		refreshLabels();
		r.repaint();
	}

	void MdEditor::fillMachineBox()
	{
		auto& box = m_root->machineBox.box;
		const auto list = m_proc.machines();
		m_shownList = list.get();
		const int sel = box.getSelectedId();
		box.clear(juce::dontSendNotification);
		if(list)
			for(const auto& m : *list)
				if(m.id >= 2 && !m.name.empty()) box.addItem(juce::String(m.name) + "  (" + juce::String(m.id) + ")", m.id + 1);
		if(sel > 0) box.setSelectedId(sel, juce::dontSendNotification);
		m_shownMachine = -2;
	}

	void MdEditor::refreshLabels()
	{
		auto& r = *m_root;
		const auto list = m_proc.machines();
		const int mid = int(std::round(m_proc.apvts.getRawParameterValue(paramId(m_track, "machine"))->load()));
		const md::engine::MachineInfo* info = nullptr;
		if(list) for(const auto& m : *list) if(m.id == mid) { info = &m; break; }
		if(m_page == Syn)
			for(int i = 0; i < 8; ++i)
			{
				const juce::String l = info && !info->params[size_t(i)].empty() ? juce::String(info->params[size_t(i)]) : paramLabel(i);
				r.dials[size_t(i)].setLabel(l);
			}
		else if(m_page == Fx)
			for(int i = 0; i < 8; ++i) r.dials[size_t(i)].setLabel(kFxLabels[i]);
		else if(m_page == Route)
			for(int i = 0; i < 8; ++i) r.dials[size_t(i)].setLabel(kRouteLabels[i]);
		if(m_page == Mixer)
			for(int t = 0; t < kTracks; ++t)
			{
				const int m = int(std::round(m_proc.apvts.getRawParameterValue(paramId(t, "machine"))->load()));
				juce::String name = "-";
				if(list) for(const auto& mi : *list) if(mi.id == m) { name = juce::String(mi.name).substring(0, 6); break; }
				r.bars[size_t(t)].setCaption(juce::String(t + 1), name, t == m_track);
			}
		m_shownMachine = mid;
	}

	void MdEditor::timerCallback()
	{
		auto& r = *m_root;
		r.status.setText(m_proc.statusText(), juce::dontSendNotification);
		r.kitLabel.setText(m_proc.kitLabel(), juce::dontSendNotification);
		const auto list = m_proc.machines();
		if(list.get() != m_shownList) { fillMachineBox(); r.machineBox.bind(m_proc.apvts, paramId(m_track, "machine")); }
		const int mid = int(std::round(m_proc.apvts.getRawParameterValue(paramId(m_track, "machine"))->load()));
		if(mid != m_shownMachine || m_page == Mixer) refreshLabels();
	}

	void MdEditor::chooseOs()
	{
		m_root->chooser = std::make_unique<juce::FileChooser>("Select your Machinedrum OS 1.63 file", juce::File(), "*.syx");
		m_root->chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc)
		{
			const auto f = fc.getResult();
			if(f.existsAsFile()) m_proc.setOsPath(f.getFullPathName());
		});
	}

	void MdEditor::chooseKit()
	{
		m_root->chooser = std::make_unique<juce::FileChooser>("Load a Machinedrum kit (.syx)", juce::File(), "*.syx");
		m_root->chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc)
		{
			const auto f = fc.getResult();
			if(!f.existsAsFile()) return;
			juce::String err;
			if(!m_proc.loadKitFile(f, err)) juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Machinemodule", err);
			timerCallback();
		});
	}
}
