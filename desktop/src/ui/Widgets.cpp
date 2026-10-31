// Adapted from shnolk's Monomodule (src/plugin/one/OneEditor.cpp), AGPL-3.0.
#include "Widgets.h"
#include <cmath>

namespace mm::ui {

// ---------------------------------------------------------------------------
// KnobCell

KnobCell::KnobCell()
{
	setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
	setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
	setMouseDragSensitivity(160);
	setMouseCursor(juce::MouseCursor::UpDownLeftRightResizeCursor);	// drag in any direction
	setOpaque(false);
}

void KnobCell::mouseMove(const juce::MouseEvent& e)
{
	Slider::mouseMove(e);
	setMouseCursor(m_valueArea.contains(e.getPosition()) ? m_valueCursor : juce::MouseCursor(juce::MouseCursor::UpDownLeftRightResizeCursor));
}

void KnobCell::mouseUp(const juce::MouseEvent& e)
{
	Slider::mouseUp(e);
	// a click that did not turn the knob, released on the value row, edits the value
	if(onValueClick && !e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1
		&& m_valueArea.contains(e.getMouseDownPosition()) && m_valueArea.contains(e.getPosition()))
		onValueClick();
}

void KnobCell::mouseDoubleClick(const juce::MouseEvent& e)
{
	if(m_valueArea.contains(e.getPosition())) return;	// double-click on the value is editing, not reset
	Slider::mouseDoubleClick(e);
}

// ---------------------------------------------------------------------------
// KnobPage

KnobPage::KnobPage(juce::AudioProcessorValueTreeState& apvts, const char* title) : m_apvts(apvts), m_title(title)
{
	for(int k = 0; k < 8; ++k)
	{
		auto& c = m_cells[size_t(k)];
		c.onValueChange = [this] { repaint(); };
		c.onValueClick = [this, k]
		{
			const auto d = m_params[size_t(k)].display;
			if(d == spec::Display::Numeric || d == spec::Display::Bipolar) beginEdit(k);
			else showValueList(k);
		};
		addAndMakeVisible(c);
	}
	m_editor.setJustification(juce::Justification::centred);
	m_editor.setInputRestrictions(4, "0123456789+-");
	m_editor.setSelectAllWhenFocused(true);
	m_editor.setFont(juce::Font(juce::FontOptions(float(4 * kScale))));
	m_editor.setIndents(2, 1);
	m_editor.onReturnKey = [this] { endEdit(true); };
	m_editor.onEscapeKey = [this] { endEdit(false); };
	m_editor.onFocusLost = [this] { endEdit(true); };
	addChildComponent(m_editor);
	setOpaque(true);
}

void KnobPage::beginEdit(int k)
{
	if(m_editing >= 0) endEdit(true);
	const auto& p = m_params[size_t(k)];
	m_editing = k;
	m_editor.setText(valueText(p, int(std::lround(m_cells[size_t(k)].getValue()))), false);
	const int x0 = (k % 4) * kCell, y0 = kGridY + (k / 4) * kCell;
	m_editor.setBounds(knobValueBox(x0, y0) * kScale);
	m_editor.setVisible(true);
	m_editor.grabKeyboardFocus();
	m_editor.selectAll();
	// clicks that land on components which do not take keyboard focus (the knobs, the page itself) never make the editor lose focus,
	// so watch the whole window for the next press
	if(auto* top = getTopLevelComponent()) { m_listenedTop = top; top->addMouseListener(this, true); }
	repaint();
}

void KnobPage::mouseDown(const juce::MouseEvent& e)
{
	if(m_editing >= 0)
	{
		auto* c = e.eventComponent;
		if(c == &m_editor || m_editor.isParentOf(c)) return;
		endEdit(true);
	}
}

void KnobPage::endEdit(bool commit)
{
	if(m_editing < 0) return;
	const int k = m_editing;
	m_editing = -1;
	if(m_listenedTop != nullptr) m_listenedTop->removeMouseListener(this);
	m_listenedTop = nullptr;
	m_editor.setVisible(false);
	const juce::String txt = m_editor.getText().trim();
	if(commit && txt.isNotEmpty() && txt.containsAnyOf("0123456789"))
	{
		const auto& p = m_params[size_t(k)];
		int v = txt.getIntValue();	// "+3" -> 3, "-19" -> -19
		v = p.display == spec::Display::Bipolar ? juce::jlimit(-64, 63, v) + 64 : juce::jlimit(0, int(p.maxRaw), v);	// out of range snaps to the limits
		m_cells[size_t(k)].setValue(double(v), juce::sendNotificationSync);
	}
	repaint();
}

void KnobPage::showValueList(int k)
{
	refreshDynamic();
	const auto& p = m_params[size_t(k)];
	if(p.display != spec::Display::Text && p.display != spec::Display::Shape) return;
	const int current = int(std::lround(m_cells[size_t(k)].getValue()));
	juce::PopupMenu m;
	for(int i = 0; i <= int(p.maxRaw); ++i)
	{
		juce::String name = p.display == spec::Display::Shape ? "SHAPE " + juce::String(i + 1) : (p.values && p.values[i] ? juce::String(p.values[i]) : juce::String(i));
		m.addItem(i + 1, name, true, i == current);
	}
	const int x0 = (k % 4) * kCell, y0 = kGridY + (k / 4) * kCell;
	const auto anchor = (knobValueBox(x0, y0) * kScale) + getScreenPosition();
	m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(anchor).withMinimumWidth(kCell * kScale),
		[this, k](int r) { if(r > 0) m_cells[size_t(k)].setValue(double(r - 1), juce::sendNotificationSync); });
}

void KnobPage::bind(const spec::Param* params8, const std::function<juce::String(int)>& paramId)
{
	for(int k = 0; k < 8; ++k)
	{
		auto& cell = m_cells[size_t(k)];
		m_attach[size_t(k)].reset();
		m_params[size_t(k)] = params8[k];
		const auto d = params8[k].display;
		cell.setVisible(d != spec::Display::Blank);	// a blank knob is dead on the hardware: nothing drawn, nothing to turn
		const bool dial = d == spec::Display::Numeric || d == spec::Display::Bipolar;
		cell.setValueArea(juce::Rectangle<int>(0, (kValueBoxY - 1) * kScale, (kCell - 1) * kScale, kValueBoxH * kScale),
			dial ? juce::MouseCursor::IBeamCursor : juce::MouseCursor::PointingHandCursor);
		if(d == spec::Display::Blank) continue;
		m_attach[size_t(k)] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(m_apvts, paramId(k), cell);
		cell.setDoubleClickReturnValue(true, double(params8[k].defaultRaw));
	}
	endEdit(false);
	repaint();
}

void KnobPage::refreshDynamic()
{
	for(int k = 0; k < 8; ++k)
		if(m_valuesFn[size_t(k)]) m_params[size_t(k)].values = m_valuesFn[size_t(k)]();
}

void KnobPage::paint(juce::Graphics& g)
{
	refreshDynamic();
	LcdCanvas cv(kLcdW, kLcdH);
	cv.fillRect(0, 0, kLcdW, kTitleH, true);	// the full-width title bar, on every page
	cv.text(spec::kFontBold8(), m_title.toRawUTF8(), 2, 1, false);
	if(m_badge.isNotEmpty())	// paper box with ink text inside the inverted bar
	{
		const int bw = LcdCanvas::textWidth(spec::kFontBold8(), m_badge.toRawUTF8()) + 4;
		cv.fillRect(kLcdW - bw - 1, 1, bw, kTitleH - 2, false);
		cv.text(spec::kFontBold8(), m_badge.toRawUTF8(), kLcdW - bw + 1, 1, true);
	}
	for(int k = 0; k < 8; ++k)
	{
		const auto& p = m_params[size_t(k)];
		const auto& cell = m_cells[size_t(k)];
		const int x0 = (k % 4) * kCell, y0 = kGridY + (k / 4) * kCell;
		const int raw = int(std::lround(cell.getValue()));
		const bool hot = cell.isVisible() && (cell.isMouseOverOrDragging() || m_editing == k);
		drawKnobCell(cv, x0, y0, p, raw, hot);
	}
	cv.dotsV(kLcdW - 1, kGridY, kLcdH - 1);	// grid right edge
	cv.dotsH(0, kLcdW - 1, kLcdH - 1);	// grid bottom edge
	cv.draw(g, 0, 0);
}

void KnobPage::resized()
{
	for(int k = 0; k < 8; ++k)
	{
		const int x0 = (k % 4) * kCell, y0 = kGridY + (k / 4) * kCell;
		m_cells[size_t(k)].setBounds((x0 + 1) * kScale, (y0 + 1) * kScale, (kCell - 1) * kScale, (kCell - 1) * kScale);
	}
	if(m_editing >= 0)
	{
		const int x0 = (m_editing % 4) * kCell, y0 = kGridY + (m_editing / 4) * kCell;
		m_editor.setBounds(knobValueBox(x0, y0) * kScale);
	}
}

// ---------------------------------------------------------------------------
// MachineBar

void MachineBar::setParameter(juce::RangedAudioParameter& param)
{
	m_attach = std::make_unique<juce::ParameterAttachment>(param, [this](float v) { m_id = int(std::lround(v)); update(); });
	m_attach->sendInitialUpdate();
}

void MachineBar::update()
{
	juce::String group = "GND", name = "--";
	if(m_list)
		for(const auto& m : *m_list)
			if(m.id == m_id) { group = MachineNames::group(m); name = MachineNames::shortName(m); break; }
	if(group != m_group || name != m_name)
	{
		m_group = group;
		m_name = name;
		if(onContentChanged) onContentChanged();	// re-layout: the block's width follows the name
	}
	repaint();
}

int MachineBar::widthLcd() const
{
	return kLogoX + LcdCanvas::textWidth(spec::kFontBold8(), m_group.toRawUTF8()) + kLogoGap
		+ LcdCanvas::textWidth(spec::kFontBold8(), m_name.toRawUTF8()) + kArrowGap + kArrowW + kPadR;
}

void MachineBar::paint(juce::Graphics& g)
{
	const int w = getWidth() / kScale, h = getHeight() / kScale;
	LcdCanvas cv(w, h);
	cv.fillRect(0, 0, w, h, true);
	const auto& bold = spec::kFontBold8();
	const int nameX = kLogoX + LcdCanvas::textWidth(bold, m_group.toRawUTF8()) + kLogoGap;
	cv.text(bold, m_group.toRawUTF8(), kLogoX, (h - bold.h) / 2, false);
	cv.text(bold, m_name.toRawUTF8(), nameX, (h - bold.h) / 2, false);
	// the picker arrow after the name: down when closed, up while the picker is open
	const int ax = nameX + LcdCanvas::textWidth(bold, m_name.toRawUTF8()) + kArrowGap, ay = h / 2 - 1;
	for(int r = 0; r < 3; ++r)
	{
		const int half = m_open ? r : 2 - r;
		for(int c = 2 - half; c <= 2 + half; ++c) cv.set(ax + c, ay + r, false);
	}
	cv.draw(g, 0, 0);
}

// ---------------------------------------------------------------------------
// TrackPanel

juce::Rectangle<int> TrackPanel::cellRect(int t) const
{
	const int col = t / kRows, row = t % kRows;
	const int x = col == 0 ? 0 : kLcdW / 2, w = col == 0 ? kLcdW / 2 : kLcdW - kLcdW / 2;
	return {x, kKeysH + kTitleH + 1 + row * kCellH, w, kCellH};
}

juce::Rectangle<int> TrackPanel::lockBox(int t) const { const auto r = cellRect(t); return {r.getRight() - 22, r.getY() + 8, 9, 8}; }
juce::Rectangle<int> TrackPanel::muteBox(int t) const { const auto r = cellRect(t); return {r.getRight() - 12, r.getY() + 8, 9, 8}; }

juce::Rectangle<int> TrackPanel::headerKey(int k) const
{
	const auto r = cellRect(k * kRows);
	return {r.getX(), 0, r.getWidth(), kKeysH - 2};
}

int TrackPanel::trackAt(juce::Point<int> local) const
{
	const auto p = local / kScale;
	for(int t = 0; t < 16; ++t) if(cellRect(t).contains(p)) return t;
	return -1;
}

void TrackPanel::paint(juce::Graphics& g)
{
	LcdCanvas cv(kLcdW, kLcdH);
	cv.fillRect(0, kKeysH, kLcdW, kTitleH, true);
	cv.text(spec::kFontBold8(), "TRACKS", 2, kKeysH + 1, false);
	for(int k = 0; k < 2; ++k)	// MIXER above track 1, MASTER above track 9: framed like the track keys, solid while their view is shown
	{
		const auto r = headerKey(k);
		const bool on = m_mode == k + 1;
		if(on) cv.fillRect(r.getX(), r.getY(), r.getWidth(), r.getHeight(), true);
		else
		{
			cv.dotsH(r.getX(), r.getRight() - 1, r.getY()); cv.dotsH(r.getX(), r.getRight() - 1, r.getBottom() - 1);
			cv.dotsV(r.getX(), r.getY(), r.getBottom() - 1); cv.dotsV(r.getRight() - 1, r.getY(), r.getBottom() - 1);
		}
		cv.text(spec::kFontBold8(), k == 0 ? "MIXER" : "MASTER", r.getX() + 3, r.getY() + 3, !on);
	}
	for(int t = 0; t < 16; ++t)
	{
		const auto r = cellRect(t);
		const bool sel = t == m_selected;
		if(sel) cv.fillRect(r.getX(), r.getY(), r.getWidth(), r.getHeight(), true);
		else
		{
			cv.dotsH(r.getX(), r.getRight() - 1, r.getY()); cv.dotsH(r.getX(), r.getRight() - 1, r.getBottom() - 1);
			cv.dotsV(r.getX(), r.getY(), r.getBottom() - 1); cv.dotsV(r.getRight() - 1, r.getY(), r.getBottom() - 1);
		}
		const bool ink = !sel;
		const juce::String num = juce::String(t + 1).paddedLeft('0', 2);
		cv.text(spec::kFontBold8(), num.toRawUTF8(), r.getX() + 3, r.getY() + 5, ink);
		cv.text(spec::kFontTiny3x5(), m_machine[size_t(t)].toRawUTF8(), r.getX() + 19, r.getY() + 3, ink);
		// activity LED: a 4x4 block while the track outputs signal, its outline otherwise
		const int lx = r.getRight() - 7, ly = r.getY() + 3;
		if(m_active[size_t(t)]) cv.fillRect(lx, ly, 4, 4, ink);
		else
		{
			for(int c = 0; c < 4; ++c) { cv.set(lx + c, ly, ink); cv.set(lx + c, ly + 3, ink); }
			cv.set(lx, ly + 1, ink); cv.set(lx, ly + 2, ink); cv.set(lx + 3, ly + 1, ink); cv.set(lx + 3, ly + 2, ink);
		}
		// LOCK and MUTE keys: solid while on (the track keeps its sound across kit loads / is silent), a dotted frame otherwise
		auto key = [&](juce::Rectangle<int> b, const char* letter, bool on)
		{
			if(on) { cv.fillRect(b.getX(), b.getY(), b.getWidth(), b.getHeight(), ink); cv.text(spec::kFontTiny3x5(), letter, b.getX() + 3, b.getY() + 1, !ink); }
			else
			{
				cv.dotsH(b.getX(), b.getRight() - 1, b.getY()); cv.dotsH(b.getX(), b.getRight() - 1, b.getBottom() - 1);
				cv.dotsV(b.getX(), b.getY(), b.getBottom() - 1); cv.dotsV(b.getRight() - 1, b.getY(), b.getBottom() - 1);
				cv.text(spec::kFontTiny3x5(), letter, b.getX() + 3, b.getY() + 1, ink);
			}
		};
		key(lockBox(t), "L", m_locked[size_t(t)]);
		key(muteBox(t), "M", m_muted[size_t(t)]);
	}
	cv.draw(g, 0, 0);
}

void TrackPanel::mouseDown(const juce::MouseEvent& e)
{
	const auto lcd = e.getPosition() / kScale;
	if(headerKey(0).contains(lcd)) { if(onMixer) onMixer(); return; }
	if(headerKey(1).contains(lcd)) { if(onMaster) onMaster(); return; }
	const int t = trackAt(e.getPosition());
	if(t < 0) return;
	if(lockBox(t).contains(lcd)) { if(onLock) onLock(t); return; }
	if(muteBox(t).contains(lcd)) { if(onMute) onMute(t); return; }
	setSelected(t);
	if(onSelect) onSelect(t);	// also on the selected track: in the mixer / master view it returns to the track view
}

void TrackPanel::mouseMove(const juce::MouseEvent& e)
{
	const int t = trackAt(e.getPosition());
	const auto lcd = e.getPosition() / kScale;
	if(headerKey(0).contains(lcd) || headerKey(1).contains(lcd)) { setMouseCursor(juce::MouseCursor::PointingHandCursor); return; }
	const bool key = t >= 0 && (lockBox(t).contains(lcd) || muteBox(t).contains(lcd));
	setMouseCursor(t >= 0 && (t != m_selected || m_mode != 0 || key) ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

// ---------------------------------------------------------------------------
// LevelColumn

LevelColumn::LevelColumn()
{
	setSliderStyle(juce::Slider::LinearVertical);
	setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
	setSliderSnapsToMousePosition(false);
	setMouseDragSensitivity(200);
	setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
	setOpaque(true);
}

void LevelColumn::setMeter(float linear)
{
	const float v = juce::jlimit(0.0f, 1.0f, linear);
	if(std::abs(v - m_meter) < 0.01f) return;
	m_meter = v;
	repaint();
}

void LevelColumn::paint(juce::Graphics& g)
{
	const int w = getWidth() / kScale, h = getHeight() / kScale;
	LcdCanvas cv(w, h);
	cv.textCentred(spec::kFontBold8(), "LEV", 0, w, 1);
	// dotted frame, as the hardware's LEV column
	const int fx = 0, fy = 11, fw = w, fh = h - fy;
	cv.dotsH(fx, fx + fw - 1, fy); cv.dotsH(fx, fx + fw - 1, fy + fh - 1);
	cv.dotsV(fx, fy, fy + fh - 1); cv.dotsV(fx + fw - 1, fy, fy + fh - 1);
	const int innerY = fy + 2, innerH = fh - 4;
	const double frac = getMaximum() > getMinimum() ? (getValue() - getMinimum()) / (getMaximum() - getMinimum()) : 0.0;
	const int barW = (fw - 7) / 2;
	const int lvlH = int(std::lround(frac * innerH));
	cv.fillRect(fx + 2, innerY + innerH - lvlH, barW, lvlH, true);	// level: solid bar
	const int metH = int(std::lround(m_meter * innerH));
	for(int y = innerY + innerH - metH; y < innerY + innerH; y += 2)	// output: dotted bar
		for(int x = fx + 3 + barW; x < fx + fw - 2; ++x) cv.set(x, y);
	cv.draw(g, 0, 0);
}

// ---------------------------------------------------------------------------
// MixerPage

MixerPage::MixerPage(juce::AudioProcessorValueTreeState& apvts) : m_apvts(apvts)
{
	setOpaque(true);
	auto attach = [this](juce::RangedAudioParameter*& prm, std::unique_ptr<juce::ParameterAttachment>& at, const juce::String& id)
	{
		prm = m_apvts.getParameter(id);
		if(prm) { at = std::make_unique<juce::ParameterAttachment>(*prm, [this](float) { repaint(); }); at->sendInitialUpdate(); }
	};
	for(int t = 0; t < 16; ++t)
	{
		const auto pre = juce::String::formatted("t%02d_", t + 1);
		attach(m_param[size_t(t)], m_attach[size_t(t)], pre + "level");
		attach(m_panParam[size_t(t)], m_panAttach[size_t(t)], pre + "pan");
		attach(m_muteParam[size_t(t)], m_muteAttach[size_t(t)], pre + "mute");
		attach(m_soloParam[size_t(t)], m_soloAttach[size_t(t)], pre + "solo");
	}
}

void MixerPage::paint(juce::Graphics& g)
{
	LcdCanvas cv(kLcdW, kLcdH);
	cv.fillRect(0, 0, kLcdW, KnobPage::kTitleH, true);
	cv.text(spec::kFontBold8(), "MIXER", 2, 1, false);
	const int gy = KnobPage::kGridY, mfw = 11, mfx = 2, mfy = kFaderY, mfh = kFaderH, mmx = mfx + mfw + 2, mmw = 5, panW = kChan - 4;
	auto frame = [&](int x0, int x, int w) { cv.dotsH(x0 + x, x0 + x + w - 1, mfy); cv.dotsH(x0 + x, x0 + x + w - 1, mfy + mfh - 1); cv.dotsV(x0 + x, mfy, mfy + mfh - 1); cv.dotsV(x0 + x + w - 1, mfy, mfy + mfh - 1); };
	auto value = [](juce::RangedAudioParameter* p, float scale) { return p ? p->convertFrom0to1(p->getValue()) / scale : 0.0f; };
	for(int c = 0; c < 16; ++c)
	{
		const int x0 = c * kChan;
		const bool sel = c == m_selected;
		cv.dotsV(x0, gy, kLcdH - 1);
		const juce::String num(c + 1);
		if(sel) cv.fillRect(x0 + 1, gy + 1, kChan - 1, 8, true);
		cv.textCentred(spec::kFontTiny3x5(), num.toRawUTF8(), x0 + 1, kChan - 1, gy + kLabelY, !sel);
		// PAN: a dotted line across with a 3-wide marker, centre = 64
		cv.dotsH(x0 + 2, x0 + 2 + panW - 1, kPanY + kPanH / 2);
		cv.set(x0 + 2 + panW / 2, kPanY); cv.set(x0 + 2 + panW / 2, kPanY + kPanH - 1);
		cv.fillRect(x0 + 2 + int(std::lround(value(m_panParam[size_t(c)], 127.0f) * float(panW - 3))), kPanY, 3, kPanH, true);
		// LEV and its meter
		frame(x0, mfx, mfw); frame(x0, mmx, mmw);
		const int innerY = mfy + 2, innerH = mfh - 4;
		const int lvlH = int(std::lround(value(m_param[size_t(c)], 127.0f) * float(innerH))), barW = (mfw - 5) / 2;
		cv.fillRect(x0 + mfx + 2, innerY + innerH - lvlH, barW + 2, lvlH, true);
		const int metH = int(std::lround(juce::jlimit(0.0f, 1.0f, m_peak[size_t(c)]) * float(innerH)));
		cv.fillRect(x0 + mmx + 1, innerY + innerH - metH, mmw - 2, metH, true);
		// M and S
		for(int k = 0; k < 2; ++k)
		{
			const bool on = (k == 0 ? m_muteParam : m_soloParam)[size_t(c)] && (k == 0 ? m_muteParam : m_soloParam)[size_t(c)]->getValue() > 0.5f;
			const int bx = x0 + 2, by = kBtnY + k * (kBtnH + 1);
			if(on) cv.fillRect(bx, by, panW, kBtnH, true);
			else { cv.dotsH(bx, bx + panW - 1, by); cv.dotsH(bx, bx + panW - 1, by + kBtnH - 1); cv.dotsV(bx, by, by + kBtnH - 1); cv.dotsV(bx + panW - 1, by, by + kBtnH - 1); }
			cv.textCentred(spec::kFontSmall4x5(), k == 0 ? "M" : "S", bx, panW, by + (kBtnH - 5) / 2, !on);
		}
	}
	cv.dotsV(16 * kChan, gy, kLcdH - 1);
	cv.dotsH(0, kLcdW - 1, gy);
	cv.dotsH(0, kLcdW - 1, kLcdH - 1);
	cv.draw(g, 0, 0);
}

void MixerPage::setLevelFromY(int t, int yLcd)
{
	if(t < 0 || !m_attach[size_t(t)]) return;
	const int innerY = kFaderY + 2, innerH = kFaderH - 4;
	const float v = 127.0f * (1.0f - juce::jlimit(0.0f, 1.0f, float(yLcd - innerY) / float(innerH)));
	m_attach[size_t(t)]->setValueAsPartOfGesture(std::round(v));
}

void MixerPage::setPanFromX(int t, int xLcd)
{
	if(t < 0 || !m_panAttach[size_t(t)]) return;
	const int panW = kChan - 4, x = xLcd - t * kChan - 2 - 1;
	m_panAttach[size_t(t)]->setValueAsPartOfGesture(std::round(127.0f * juce::jlimit(0.0f, 1.0f, float(x) / float(panW - 3))));
}

void MixerPage::mouseDown(const juce::MouseEvent& e)
{
	const auto p = e.getPosition() / kScale;
	const int t = channelAt(p.x);
	if(p.y < kPanY - 1) { if(onSelect) onSelect(t); return; }	// the number selects the track
	if(p.y >= kBtnY)	// M / S
	{
		auto* prm = p.y < kBtnY + kBtnH + 1 ? m_muteParam[size_t(t)] : m_soloParam[size_t(t)];
		if(prm) { prm->beginChangeGesture(); prm->setValueNotifyingHost(prm->getValue() > 0.5f ? 0.0f : 1.0f); prm->endChangeGesture(); }
		return;
	}
	if(p.y < kFaderY - 1)	// PAN
	{
		m_dragTrack = t; m_dragWhat = 2;
		if(m_panAttach[size_t(t)]) m_panAttach[size_t(t)]->beginGesture();
		setPanFromX(t, p.x);
		return;
	}
	m_dragTrack = t; m_dragWhat = 1;
	if(m_attach[size_t(t)]) m_attach[size_t(t)]->beginGesture();
	setLevelFromY(t, p.y);
}

void MixerPage::mouseDrag(const juce::MouseEvent& e)
{
	if(m_dragTrack < 0) return;
	const auto p = e.getPosition() / kScale;
	if(m_dragWhat == 2) setPanFromX(m_dragTrack, p.x); else setLevelFromY(m_dragTrack, p.y);
}

void MixerPage::mouseUp(const juce::MouseEvent&)
{
	if(m_dragTrack >= 0)
	{
		auto& at = m_dragWhat == 2 ? m_panAttach[size_t(m_dragTrack)] : m_attach[size_t(m_dragTrack)];
		if(at) at->endGesture();
	}
	m_dragTrack = -1; m_dragWhat = 0;
}

void MixerPage::mouseDoubleClick(const juce::MouseEvent& e)
{
	const auto p = e.getPosition() / kScale;
	const int t = channelAt(p.x);
	if(p.y >= kBtnY || p.y < kPanY - 1) return;
	if(p.y < kFaderY - 1) { if(m_panAttach[size_t(t)]) m_panAttach[size_t(t)]->setValueAsCompleteGesture(64.0f); }	// centre
	else if(m_attach[size_t(t)]) m_attach[size_t(t)]->setValueAsCompleteGesture(100.0f);
}

// ---------------------------------------------------------------------------
// BpmReadout / LcdText / buttons

void BpmReadout::paint(juce::Graphics& g)
{
	const int w = getWidth() / kScale, h = getHeight() / kScale;
	LcdCanvas cv(w, h);
	const juce::String s(double(m_bpm), 1);
	const int tw = LcdCanvas::textWidth(spec::kFontDigitsTop(), s.toRawUTF8());
	const int x = w - tw, y = (h - 10) / 2;
	cv.text(spec::kFontDigitsTop(), s.toRawUTF8(), x, y);
	cv.text(spec::kFontDigitsBottom(), s.toRawUTF8(), x, y + spec::kFontDigitsTop().h);
	cv.draw(g, 0, 0);
}

void LcdText::paint(juce::Graphics& g)
{
	const int w = juce::jmax(1, getWidth() / m_scale), h = juce::jmax(1, getHeight() / m_scale);
	LcdCanvas cv(w, h);
	if(m_inverted) cv.fillRect(0, 0, w, h, true);
	const juce::String caps = m_text.toUpperCase();	// the LCD fonts have no lowercase
	const int tw = LcdCanvas::textWidth(m_font, caps.toRawUTF8());
	const int x = m_just.testFlags(juce::Justification::right) ? w - tw - 1
	            : m_just.testFlags(juce::Justification::horizontallyCentred) ? (w - tw) / 2 : 1;
	cv.text(m_font, caps.toRawUTF8(), x, (h - m_font.h) / 2, !m_inverted);
	cv.draw(g, 0, 0, m_scale);
}

void LcdButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
	const int w = getWidth() / kScale, h = getHeight() / kScale;
	LcdCanvas cv(w, h);
	const bool inv = !down;	// solid block normally, outlined while pressed
	if(inv) cv.fillRect(0, 0, w, h, true);
	else { cv.dotsH(0, w - 1, 0); cv.dotsH(0, w - 1, h - 1); cv.dotsV(0, 0, h - 1); cv.dotsV(w - 1, 0, h - 1); }
	cv.textCentred(spec::kFontBold8(), getButtonText().toRawUTF8(), 0, w, (h - spec::kFontBold8().h) / 2, !inv);
	cv.draw(g, 0, 0);
	if(highlighted && !down) { g.setColour(lcd::ink.withAlpha(0.15f)); g.fillRect(getLocalBounds()); }
}

void LcdToggle::paintButton(juce::Graphics& g, bool highlighted, bool)
{
	const int w = getWidth() / kScale, h = getHeight() / kScale;
	LcdCanvas cv(w, h);
	const bool on = getToggleState();
	if(on) cv.fillRect(0, 0, w, h, true);
	else { cv.dotsH(0, w - 1, 0); cv.dotsH(0, w - 1, h - 1); cv.dotsV(0, 0, h - 1); cv.dotsV(w - 1, 0, h - 1); }
	cv.textCentred(spec::kFontBold8(), getButtonText().toRawUTF8(), 0, w, (h - spec::kFontBold8().h) / 2, !on);
	cv.draw(g, 0, 0);
	if(highlighted) { g.setColour(lcd::ink.withAlpha(0.15f)); g.fillRect(getLocalBounds()); }
}

// ---------------------------------------------------------------------------
// KitStrip

namespace {
void frameBox(LcdCanvas& cv, int x, int y, int w, int h) { cv.fillRect(x, y, w, 1); cv.fillRect(x, y + h - 1, w, 1); cv.fillRect(x, y, 1, h); cv.fillRect(x + w - 1, y, 1, h); }
void arrowH(LcdCanvas& cv, int cx, int cy, bool left)
{
	for(int c = 0; c < 4; ++c) { const int h = left ? 2 * c + 1 : 7 - 2 * c; cv.fillRect(cx - 2 + c, cy - h / 2, 1, h); }
}
}

KitStrip::Part KitStrip::partAt(juce::Point<int> p) const
{
	const auto l = p / kScale;
	const int w = getWidth() / kScale;
	if(l.y < 0 || l.y >= kLcdH || l.x < 0 || l.x >= w) return Part::None;
	if(l.x < kArrowW) return Part::Prev;
	if(l.x >= w - kArrowW) return Part::Next;
	return Part::Name;
}

void KitStrip::paint(juce::Graphics& g)
{
	const int w = getWidth() / kScale, h = kLcdH;
	LcdCanvas cv(w, h);
	frameBox(cv, 0, 0, kArrowW, h); arrowH(cv, kArrowW / 2, h / 2, true);
	frameBox(cv, w - kArrowW, 0, kArrowW, h); arrowH(cv, w - kArrowW / 2, h / 2, false);
	frameBox(cv, kArrowW - 1, 0, w - 2 * (kArrowW - 1), h);
	const auto& tiny = spec::kFontTiny3x5();
	cv.text(tiny, "KIT", kArrowW + 3, (h - tiny.h) / 2);
	const int nameX = kArrowW + 3 + LcdCanvas::textWidth(tiny, "KIT") + 5;
	const juce::String name = m_name.toUpperCase();
	cv.text(spec::kFontBold8(), name.toRawUTF8(), nameX, (h - spec::kFontBold8().h) / 2);
	// list arrow before the right frame, down when closed
	const int ax = w - kArrowW - 9, ay = h / 2 - 1;
	for(int r = 0; r < 3; ++r) { const int half = m_open ? r : 2 - r; for(int c = 2 - half; c <= 2 + half; ++c) cv.set(ax + c, ay + r); }
	cv.draw(g, 0, 0);
}

void KitStrip::mouseDown(const juce::MouseEvent& e)
{
	const auto part = partAt(e.getPosition());
	if(part != Part::None && onPart) onPart(part);
}

// ---------------------------------------------------------------------------
// MissingOsOverlay

MissingOsOverlay::MissingOsOverlay(std::function<void()> onSelect)
{
	m_select.onClick = std::move(onSelect);
	addAndMakeVisible(m_select);
}

// All in LCD pixels: the title bar, the wrapped explanation, the button, then the status (a failed load says why).
MissingOsOverlay::Layout MissingOsOverlay::layout() const
{
	static const char* const para = "THIS PLUGIN RUNS THE MACHINEDRUM'S OWN SOUND ENGINE. IT NEEDS YOUR COPY OF THE MACHINEDRUM UW OS 1.63 "
		"(ELEKTRON_SPS1-1UW_OS1.63.SYX, A FREE DOWNLOAD FROM ELEKTRON). CHOOSE THAT FILE ONCE: EVERY INSTANCE SHARES IT.";
	constexpr int kTitleH = 11, kLine = 8, kPad = 5, kButtonH = 14, kButtonGap = 8;
	Layout l;
	l.bw = juce::jmax(120, juce::jmin(220, getWidth() / kScale - 16));
	l.text = wrapLcdText(spec::kFontSmall4x5(), para, l.bw - 2 * kPad);
	l.buttonY = kTitleH + kPad + int(l.text.size()) * kLine + kButtonGap;
	l.bh = l.buttonY + kButtonH + kPad;
	if(m_status.isNotEmpty())
	{
		l.status = wrapLcdText(spec::kFontSmall4x5(), m_status.toUpperCase(), l.bw - 2 * kPad);
		if(l.status.size() > 3) l.status.resize(3);
		l.statusY = l.bh;
		l.bh += int(l.status.size()) * kLine + kPad;
	}
	return l;
}

void MissingOsOverlay::resized()
{
	const auto l = layout();
	m_box = juce::Rectangle<int>(0, 0, l.bw * kScale, l.bh * kScale).withCentre(getLocalBounds().getCentre());
	const int bwLcd = juce::jmin(l.bw - 16, 120);
	m_select.setBounds(m_box.getX() + (m_box.getWidth() - bwLcd * kScale) / 2, m_box.getY() + l.buttonY * kScale, bwLcd * kScale, 14 * kScale);
}

void MissingOsOverlay::paint(juce::Graphics& g)
{
	g.fillAll(lcd::paper);
	const auto l = layout();
	LcdCanvas cv(l.bw, l.bh);
	cv.fillRect(0, 0, l.bw, 11, true);
	cv.text(spec::kFontBold8(), "MACHINEDRUM OS REQUIRED", 3, 1, false);
	cv.dotsH(0, l.bw - 1, 11); cv.dotsH(0, l.bw - 1, l.bh - 1);
	cv.dotsV(0, 11, l.bh - 1); cv.dotsV(l.bw - 1, 11, l.bh - 1);
	int y = 11 + 5;
	for(const auto& line : l.text) { cv.text(spec::kFontSmall4x5(), line.toRawUTF8(), 5, y); y += 8; }
	y = l.statusY + 3;
	for(const auto& line : l.status) { cv.text(spec::kFontSmall4x5(), line.toRawUTF8(), 5, y); y += 8; }
	cv.draw(g, m_box.getX(), m_box.getY());
}

}	// namespace mm::ui
