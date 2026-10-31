// Adapted from shnolk's Monomodule (src/plugin/one/MachinePicker.cpp), AGPL-3.0.
#include "MachinePicker.h"
#include <algorithm>
#include <cmath>

namespace mm::ui {

namespace {
constexpr int kColGap = 6, kBorder = 3, kHeaderH = 36, kPad = 6, kColRows = 16, kNameScale = 2;
constexpr int kAnimHz = 60; constexpr float kAnimSeconds = 0.16f;

void dottedH(juce::Graphics& g, int x0, int x1, int y) { for(int x = x0; x < x1; x += 4) g.fillRect(x, y, 2, 2); }
}

MachinePicker::MachinePicker()
{
	setOpaque(true);
	setWantsKeyboardFocus(true);
}

void MachinePicker::setMachines(MachineList list)
{
	m_list = std::move(list);
	build();
	layout();
}

void MachinePicker::setParameter(juce::RangedAudioParameter& machineParam)
{
	m_attach = std::make_unique<juce::ParameterAttachment>(machineParam, [this](float v) { m_current = int(std::lround(v)); repaint(); });
	m_attach->sendInitialUpdate();
}

// One column per group, in the order of the lowest machine id of the group; the ROM machines are split so no column scrolls.
void MachinePicker::build()
{
	m_columns.clear();
	if(!m_list) return;
	for(const auto& m : *m_list)
	{
		if(!MachineNames::offered(m)) continue;
		const auto group = MachineNames::group(m);
		Column* col = nullptr;
		for(auto it = m_columns.rbegin(); it != m_columns.rend(); ++it)
			if(it->group == group && int(it->rows.size()) < kColRows) { col = &*it; break; }
		if(!col) { m_columns.push_back({group, {}, {}, {}}); col = &m_columns.back(); }
		col->rows.push_back({m.id, MachineNames::shortName(m), {}});
	}
	auto key = [](const Column& c) { return (c.group == "ROM" ? 1000 : 0) + c.rows.front().id; };
	std::stable_sort(m_columns.begin(), m_columns.end(), [&](const Column& a, const Column& b) { return key(a) < key(b); });
}

void MachinePicker::setTargetBounds(juce::Rectangle<int> fullyOpen)
{
	m_target = fullyOpen;
	layout();
	applyAnimation();
}

void MachinePicker::open(bool animate)
{
	m_cursor = m_current;
	const bool was = m_wantOpen;
	m_wantOpen = true;
	setVisible(true);
	toFront(false);
	grabKeyboardFocus();
	if(!animate) { m_anim = 1.0f; stopTimer(); applyAnimation(); }
	else if(!isTimerRunning()) startTimerHz(kAnimHz);
	if(!was && onOpenChanged) onOpenChanged(true);
	repaint();
}

void MachinePicker::close(bool animate)
{
	const bool was = m_wantOpen;
	m_wantOpen = false;
	m_cursor = -1;
	if(!animate) { m_anim = 0.0f; stopTimer(); applyAnimation(); }
	else if(!isTimerRunning()) startTimerHz(kAnimHz);
	if(was && onOpenChanged) onOpenChanged(false);
}

// The picker unrolls from the top edge: its bounds grow down from the target's top, the layout is fixed for the fully open size.
void MachinePicker::applyAnimation()
{
	const float eased = 1.0f - (1.0f - m_anim) * (1.0f - m_anim) * (1.0f - m_anim);
	const int h = int(std::lround(m_target.getHeight() * eased));
	setBounds(m_target.withHeight(juce::jmax(0, h)));
	if(m_anim <= 0.0f && !m_wantOpen) setVisible(false);
}

void MachinePicker::timerCallback()
{
	const float step = 1.0f / (kAnimSeconds * float(kAnimHz));
	m_anim = m_wantOpen ? juce::jmin(1.0f, m_anim + step) : juce::jmax(0.0f, m_anim - step);
	applyAnimation();
	if(m_anim <= 0.0f || m_anim >= 1.0f) stopTimer();
}

void MachinePicker::pick(int id)
{
	if(m_attach) m_attach->setValueAsCompleteGesture(float(id));
	close();
}

void MachinePicker::layout()
{
	if(m_columns.empty()) return;
	const auto r = m_target.withPosition(0, 0);
	const int n = int(m_columns.size());
	const int colW = (r.getWidth() - (n - 1) * kColGap) / n;
	const int rowH = (r.getHeight() - 2 * kBorder - kHeaderH - 6) / kColRows;
	int x = r.getX();
	for(auto& col : m_columns)
	{
		col.bounds = {x, r.getY(), colW, r.getHeight()};
		x += colW + kColGap;
		auto inner = col.bounds.reduced(kBorder);
		col.header = inner.removeFromTop(kHeaderH);
		inner.removeFromTop(6);
		for(auto& row : col.rows) row.bounds = inner.removeFromTop(rowH);
	}
}

const MachinePicker::Row* MachinePicker::rowAt(juce::Point<int> p) const
{
	for(const auto& col : m_columns)
		for(const auto& row : col.rows)
			if(row.bounds.contains(p)) return &row;
	return nullptr;
}

void MachinePicker::paint(juce::Graphics& g)
{
	g.fillAll(lcd::paper);
	for(const auto& col : m_columns)
	{
		g.setColour(lcd::ink);
		g.drawRect(col.bounds, kBorder);
		g.fillRect(col.header);	// the group's name in the title face, paper on ink
		const int tw = LcdCanvas::textWidth(spec::kFontBold8(), col.group.toRawUTF8()) * kScale;
		drawLcdText(g, spec::kFontBold8(), col.group.toRawUTF8(), col.header.getCentreX() - tw / 2,
			col.header.getCentreY() - spec::kFontBold8().h * kScale / 2, kScale, lcd::paper);
		const int inner0 = col.bounds.getX() + kBorder, inner1 = col.bounds.getRight() - kBorder;
		g.setColour(lcd::ink);
		if(!col.rows.empty()) dottedH(g, inner0, inner1, col.rows.front().bounds.getY() - 1);
		for(const auto& row : col.rows)
		{
			const bool current = row.id == m_current, cursor = row.id == m_cursor;
			if(current) { g.setColour(lcd::ink); g.fillRect(row.bounds); }
			else if(cursor) { g.setColour(lcd::ink.withAlpha(0.15f)); g.fillRect(row.bounds); }
			const auto ink = current ? lcd::paper : lcd::ink;
			const int th = spec::kFontBold8().h * kNameScale;
			drawLcdText(g, spec::kFontBold8(), row.name.toRawUTF8(), row.bounds.getX() + kPad, row.bounds.getCentreY() - th / 2, kNameScale, ink);
			g.setColour(lcd::ink);
			dottedH(g, inner0, inner1, row.bounds.getBottom() - 1);
		}
	}
}

void MachinePicker::mouseMove(const juce::MouseEvent& e)
{
	const auto* row = rowAt(e.getPosition());
	setMouseCursor(row ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
	const int id = row ? row->id : -1;
	if(id != m_cursor) { m_cursor = id; repaint(); }
}

void MachinePicker::mouseExit(const juce::MouseEvent&)
{
	if(m_cursor >= 0) { m_cursor = -1; repaint(); }
}

void MachinePicker::mouseDown(const juce::MouseEvent& e)
{
	if(const auto* row = rowAt(e.getPosition())) pick(row->id);
}

bool MachinePicker::keyPressed(const juce::KeyPress& k)
{
	if(k == juce::KeyPress::escapeKey) { close(); return true; }
	if(k == juce::KeyPress::returnKey) { pick(m_cursor >= 0 ? m_cursor : m_current); return true; }
	const bool up = k == juce::KeyPress::upKey, down = k == juce::KeyPress::downKey;
	const bool left = k == juce::KeyPress::leftKey, right = k == juce::KeyPress::rightKey;
	if(!(up || down || left || right) || m_columns.empty()) return false;
	const int id = m_cursor >= 0 ? m_cursor : m_current;
	int c = 0, i = 0;
	for(int ci = 0; ci < int(m_columns.size()); ++ci)
		for(int ri = 0; ri < int(m_columns[size_t(ci)].rows.size()); ++ri)
			if(m_columns[size_t(ci)].rows[size_t(ri)].id == id) { c = ci; i = ri; }
	if(up) i = juce::jmax(0, i - 1);
	if(down) i = i + 1;
	if(left) c = juce::jmax(0, c - 1);
	if(right) c = juce::jmin(int(m_columns.size()) - 1, c + 1);
	i = juce::jmin(i, int(m_columns[size_t(c)].rows.size()) - 1);
	m_cursor = m_columns[size_t(c)].rows[size_t(i)].id;
	repaint();
	return true;
}

}	// namespace mm::ui
