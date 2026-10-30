// The editor's widgets, every one drawn from the LCD glyphs and fonts. Adapted from shnolk's Monomodule (src/plugin/one/OneEditor.*),
// AGPL-3.0: KnobCell, KnobPage, MachineBar, LevelColumn, LcdText, LcdButton, LcdToggle keep his design; TrackPanel, MixerPage, KitStrip
// and the read-only tempo readout are the Machinedrum's (16 tracks, level mixer, kits).
#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <functional>
#include "Lcd.h"
#include "MachinePicker.h"

namespace mm::ui {

// Invisible drag surface over one knob cell; the page draws the cell from the slider's value. A press anywhere in the cell drags the
// knob; a clean click (no drag) on the value row fires onValueClick, which opens the inline editor or the value list.
class KnobCell : public juce::Slider
{
public:
	KnobCell();
	std::function<void()> onValueClick;
	void setValueArea(juce::Rectangle<int> r, juce::MouseCursor valueCursor) { m_valueArea = r; m_valueCursor = valueCursor; }
	void paint(juce::Graphics&) override {}
	void mouseEnter(const juce::MouseEvent& e) override { Slider::mouseEnter(e); notifyPage(); }
	void mouseExit(const juce::MouseEvent& e) override { Slider::mouseExit(e); notifyPage(); }
	void mouseMove(const juce::MouseEvent& e) override;
	void mouseUp(const juce::MouseEvent& e) override;
	void mouseDoubleClick(const juce::MouseEvent& e) override;
	void startedDragging() override { notifyPage(); }
	void stoppedDragging() override { notifyPage(); }

private:
	void notifyPage() { if(auto* p = getParentComponent()) p->repaint(); }
	juce::Rectangle<int> m_valueArea;
	juce::MouseCursor m_valueCursor{juce::MouseCursor::IBeamCursor};
};

// One hardware page: inverted title bar over the 2x4 knob grid, drawn at LCD resolution.
class KnobPage : public juce::Component
{
public:
	static constexpr int kTitleH = 10, kGridY = kTitleH + 1;
	static constexpr int kLcdW = 4 * kCell + 1, kLcdH = kGridY + 2 * kCell + 1;
	static constexpr int kWidth = kLcdW * kScale, kHeight = kLcdH * kScale;

	explicit KnobPage(juce::AudioProcessorValueTreeState& apvts, const char* title);
	void setTitle(const char* title) { m_title = title; repaint(); }
	// Tag drawn at the right end of the title bar (e.g. "TRACK 3"); empty clears it.
	void setBadge(const juce::String& badge) { if(badge != m_badge) { m_badge = badge; repaint(); } }
	// Per-cell hook for a Text cell whose names follow another value (the LFO's PARAM follows its destination track).
	void setValuesSource(int k, std::function<const char* const*()> fn) { m_valuesFn[size_t(k)] = std::move(fn); }
	int cellValue(int k) const { return int(std::lround(m_cells[size_t(k)].getValue())); }
	// Binds the eight cells to parameters; the descriptors are copied (the SYN page changes with the machine).
	void bind(const spec::Param* params8, const std::function<juce::String(int)>& paramId);
	void paint(juce::Graphics&) override;
	void resized() override;
	void mouseDown(const juce::MouseEvent&) override;	// any press outside the inline editor commits it

private:
	void beginEdit(int k);	// numeric/bipolar: inline text entry, clamped to the displayed range
	void endEdit(bool commit);
	void showValueList(int k);	// text/shape: pick an entry by name
	void refreshDynamic();

	juce::AudioProcessorValueTreeState& m_apvts;
	juce::String m_title, m_badge;
	std::array<std::function<const char* const*()>, 8> m_valuesFn;
	std::array<spec::Param, 8> m_params{};
	std::array<KnobCell, 8> m_cells;
	std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, 8> m_attach;
	juce::TextEditor m_editor;
	int m_editing = -1;
	juce::Component::SafePointer<juce::Component> m_listenedTop;
};

// The machine block: an inverted block with the group and the machine name beside it, then the picker arrow; the block hugs its
// contents. A click toggles the machine picker (MachinePicker); the arrow points up while it is open.
class MachineBar : public juce::Component
{
public:
	static constexpr int kLcdH = 26, kLogoX = 4, kLogoGap = 6, kArrowGap = 4, kArrowW = 5, kPadR = 4;	// LCD px
	MachineBar() { setMouseCursor(juce::MouseCursor::PointingHandCursor); }
	void setMachines(MachineList list) { m_list = std::move(list); update(); }
	void setParameter(juce::RangedAudioParameter& param);	// the machine parameter of the shown track
	std::function<void()> onOpen;
	std::function<void()> onContentChanged;	// the width follows the machine; the parent must re-layout
	void setOpen(bool open) { if(m_open != open) { m_open = open; repaint(); } }
	int preferredWidth() const { return widthLcd() * kScale; }
	int preferredHeight() const { return kLcdH * kScale; }
	void paint(juce::Graphics&) override;
	void mouseDown(const juce::MouseEvent&) override { if(onOpen) onOpen(); }

private:
	void update();
	int widthLcd() const;
	MachineList m_list;
	std::unique_ptr<juce::ParameterAttachment> m_attach;
	int m_id = 0;
	juce::String m_group = "GND", m_name = "--";
	bool m_open = false;
};

// The track column: one cell per track in two columns (1-8, 9-16): the number, the machine's name, and an activity LED lit by the
// track's output. A click selects the track (inverted cell).
class TrackPanel : public juce::Component
{
public:
	static constexpr int kLcdW = KnobPage::kLcdW, kTitleH = 10, kCellH = 18, kRows = 8, kKeysH = 16, kLcdH = kKeysH + 2 * KnobPage::kLcdH + 4;	// the MIXER / MASTER keys, then as tall as the two page rows
	TrackPanel() { setOpaque(true); }
	std::function<void(int)> onSelect;
	std::function<void(int)> onMute;	// toggle
	std::function<void(int)> onLock;	// toggle
	std::function<void()> onMixer, onMaster;	// the keys above track 1 and track 9: toggle the mixer / master view
	void setMode(int m) { if(m_mode != m) { m_mode = m; repaint(); } }	// 0 tracks, 1 mixer, 2 master
	void setMuted(int t, bool m) { if(m_muted[size_t(t)] != m) { m_muted[size_t(t)] = m; repaint(); } }
	void setLocked(int t, bool l) { if(m_locked[size_t(t)] != l) { m_locked[size_t(t)] = l; repaint(); } }
	void setSelected(int t) { if(m_selected != t) { m_selected = t; repaint(); } }
	void setTrack(int t, const juce::String& machine) { if(m_machine[size_t(t)] != machine) { m_machine[size_t(t)] = machine; repaint(); } }
	void setPeak(int t, float linear) { const bool a = linear > 0.02f; if(m_active[size_t(t)] != a) { m_active[size_t(t)] = a; repaint(); } }
	void paint(juce::Graphics&) override;
	void mouseDown(const juce::MouseEvent&) override;
	void mouseMove(const juce::MouseEvent&) override;

private:
	juce::Rectangle<int> cellRect(int t) const;	// LCD px
	juce::Rectangle<int> lockBox(int t) const;	// LCD px: "L" key, bottom right of the cell
	juce::Rectangle<int> muteBox(int t) const;	// "M" key
	juce::Rectangle<int> headerKey(int k) const;	// LCD px: 0 = MIXER, 1 = MASTER
	int trackAt(juce::Point<int> local) const;
	int m_selected = 0, m_mode = 0;
	std::array<juce::String, 16> m_machine;
	std::array<bool, 16> m_active{}, m_muted{}, m_locked{};
};

// LEV: level fader drawn as the LCD level bar (dotted frame, filled to the value) with the output meter as a second bar beside it.
class LevelColumn : public juce::Slider
{
public:
	LevelColumn();
	void setMeter(float linear);
	void paint(juce::Graphics&) override;

private:
	float m_meter = 0.0f;
};

// The mixer view, after ModuleDrum's (github.com/K-X420/ModuleDrum, AGPL-3.0): per track its number (click: select), PAN (a bar with a
// marker: drag), LEV (the level column with the output meter beside it: drag) and the M (mute) and S (solo) keys.
class MixerPage : public juce::Component
{
public:
	static constexpr int kLcdW = 2 * KnobPage::kLcdW + 4, kLcdH = 2 * KnobPage::kLcdH + 4, kChan = kLcdW / 16;
	explicit MixerPage(juce::AudioProcessorValueTreeState& apvts);
	std::function<void(int)> onSelect;
	void setSelected(int t) { if(m_selected != t) { m_selected = t; repaint(); } }
	void setPeak(int t, float linear) { if(std::abs(m_peak[size_t(t)] - linear) > 0.01f) { m_peak[size_t(t)] = linear; repaint(); } }
	void paint(juce::Graphics&) override;
	void mouseDown(const juce::MouseEvent&) override;
	void mouseDrag(const juce::MouseEvent&) override;
	void mouseUp(const juce::MouseEvent&) override;
	void mouseDoubleClick(const juce::MouseEvent&) override;

private:
	// LCD rows of the parts of a channel
	static constexpr int kPanY = KnobPage::kGridY + 9, kPanH = 5, kBtnH = 8;
	static constexpr int kFaderY = kPanY + kPanH + 3, kBtnY = kLcdH - 2 - 2 * kBtnH - 1, kFaderH = kBtnY - 2 - kFaderY;
	int channelAt(int xLcd) const { return juce::jlimit(0, 15, xLcd / kChan); }
	void setLevelFromY(int t, int yLcd);
	void setPanFromX(int t, int xLcd);
	juce::AudioProcessorValueTreeState& m_apvts;
	std::array<juce::RangedAudioParameter*, 16> m_param{}, m_panParam{}, m_muteParam{}, m_soloParam{};
	std::array<std::unique_ptr<juce::ParameterAttachment>, 16> m_attach, m_panAttach, m_muteAttach, m_soloAttach;
	std::array<float, 16> m_peak{};
	int m_selected = 0, m_dragTrack = -1, m_dragWhat = 0;	// 1 level, 2 pan
};

// BPM: the tempo the engine runs at (the host's), in the tall digit face. Read only.
class BpmReadout : public juce::Component
{
public:
	BpmReadout() { setOpaque(true); }
	void setBpm(float bpm) { if(std::abs(bpm - m_bpm) >= 0.05f) { m_bpm = bpm; repaint(); } }
	void paint(juce::Graphics&) override;
private:
	float m_bpm = 120.0f;
};

// A two-state LCD button: solid when on, dotted frame when off.
class LcdToggle : public juce::Button
{
public:
	explicit LcdToggle(const juce::String& text) : juce::Button(text) { setClickingTogglesState(true); }
	void paintButton(juce::Graphics&, bool highlighted, bool down) override;
};

// Static text in an LCD font (footer, mark).
class LcdText : public juce::Component
{
public:
	LcdText(const spec::Font& font, juce::String text, int scale, bool inverted = false, juce::Justification just = juce::Justification::centredLeft)
		: m_font(font), m_text(std::move(text)), m_scale(scale), m_inverted(inverted), m_just(just) {}
	void paint(juce::Graphics&) override;
	void setText(juce::String s) { if(m_text != s) { m_text = std::move(s); repaint(); } }
private:
	const spec::Font& m_font;
	juce::String m_text;
	int m_scale;
	bool m_inverted;
	juce::Justification m_just;
};

// A push button drawn as inverted LCD text (MENU).
class LcdButton : public juce::Button
{
public:
	explicit LcdButton(const juce::String& text) : juce::Button(text) {}
	void paintButton(juce::Graphics&, bool highlighted, bool down) override;
};

// The kit strip: PREV, the kit's name (a click lists the kits), NEXT.
class KitStrip : public juce::Component
{
public:
	static constexpr int kLcdH = 15, kArrowW = 12;
	enum class Part { None, Prev, Name, Next };
	std::function<void(Part)> onPart;
	void setKit(const juce::String& name, bool loaded) { if(name != m_name || loaded != m_loaded) { m_name = name; m_loaded = loaded; repaint(); } }
	void setOpen(bool o) { if(m_open != o) { m_open = o; repaint(); } }
	void paint(juce::Graphics&) override;
	void mouseDown(const juce::MouseEvent&) override;
	void mouseMove(const juce::MouseEvent&) override { setMouseCursor(partAt(getMouseXYRelative()) != Part::None ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor); }
private:
	Part partAt(juce::Point<int> p) const;
	juce::String m_name = "NO KIT";
	bool m_loaded = false, m_open = false;
};

// Blocking screen shown until a valid Machinedrum OS file has been selected, in the LCD faces. The box is as tall as its wrapped text.
class MissingOsOverlay : public juce::Component
{
public:
	explicit MissingOsOverlay(std::function<void()> onSelect);
	void setStatusMessage(const juce::String& s) { if(s != m_status) { m_status = s; resized(); repaint(); } }
	void paint(juce::Graphics&) override;
	void resized() override;
	void lookAndFeelChanged() override { resized(); }
private:
	struct Layout { std::vector<juce::String> text, status; int bw = 0, bh = 0, buttonY = 0, statusY = 0; };
	Layout layout() const;
	LcdButton m_select{"SELECT OS FILE..."};
	juce::String m_status;
	juce::Rectangle<int> m_box;
};

}	// namespace mm::ui
