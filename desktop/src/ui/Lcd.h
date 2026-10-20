// LCD-style rendering for Machinemodule: every element is drawn at hardware pixel resolution from the LCD glyphs and fonts
// (RomArt.h: read from the user's OS file), then scaled up with nearest-neighbour sampling so each LCD pixel stays a crisp square.
// Adapted from shnolk's Monomodule (src/plugin/one/Lcd.h), AGPL-3.0.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include "Skin.h"
#include "Spec.h"

namespace mm::ui {

namespace spec = mm::uispec;

constexpr int kScale = 3;	// screen pixels per LCD pixel
// Knob cell: 32x32 pitch. Label (tiny-3x5) on row 3, dial/icon centred in rows 9..22, the value row (tiny-3x5, always shown) in a
// 9-row box at the bottom; pointer frame at (2, 4) in the ring.
constexpr int kCell = 32, kLabelY = 3, kContentY = 9, kContentH = 14, kValueBoxY = 23, kValueBoxH = 9, kDotX = 2, kDotY = 4;

// The two colours everything is drawn in: the skin's (Skin.h). Read at paint time, never cached across paints.
namespace lcd {
inline juce::Colour& paper = skin::paperColour();
inline juce::Colour& ink = skin::inkColour();
}

class LcdCanvas
{
public:
	LcdCanvas(int w, int h) : m_w(w), m_h(h), m_px(size_t(w * h), 0) {}
	int width() const { return m_w; }
	int height() const { return m_h; }
	void clear(bool on = false) { std::fill(m_px.begin(), m_px.end(), uint8_t(on ? 1 : 0)); }
	void set(int x, int y, bool on = true) { if(x >= 0 && y >= 0 && x < m_w && y < m_h) m_px[size_t(y * m_w + x)] = on ? 1 : 0; }
	bool get(int x, int y) const { return x >= 0 && y >= 0 && x < m_w && y < m_h && m_px[size_t(y * m_w + x)] != 0; }
	// Composites the bitmap's lit pixels (only), like the hardware's masked draw; on=false draws them as paper.
	void blit(const spec::Bitmap& b, int x, int y, bool on = true);
	void fillRect(int x, int y, int w, int h, bool on = true);
	void invertRect(int x, int y, int w, int h);
	void dotsH(int x0, int x1, int y);	// dotted rule, every other pixel from x0 to x1 inclusive
	void dotsV(int x, int y0, int y1);
	void line(int x0, int y0, int x1, int y1, bool on = true);
	static int textWidth(const spec::Font& f, const char* s);
	void text(const spec::Font& f, const char* s, int x, int y, bool on = true);	// 1-px gap between glyphs
	void textCentred(const spec::Font& f, const char* s, int x0, int w, int y, bool on = true) { text(f, s, x0 + (w - textWidth(f, s)) / 2, y, on); }
	void draw(juce::Graphics& g, int destX, int destY, int scale = kScale) const;

private:
	int m_w, m_h;
	std::vector<uint8_t> m_px;
};

// Draws one knob cell at cell origin (x0, y0): dotted top/left border, then label, dial or icon, and the value row; nothing at all
// when blank. `highlightValue` inverts the value box (hover/drag/edit).
void drawKnobCell(LcdCanvas& cv, int x0, int y0, const spec::Param& p, int raw, bool highlightValue = false);
// The value box of a cell at (x0, y0), in LCD pixels (the click target for editing).
inline juce::Rectangle<int> knobValueBox(int x0, int y0) { return {x0 + 1, y0 + kValueBoxY, kCell - 1, kValueBoxH}; }

// The MD's LFO shape icon (17 x 9): triangle, saw, square, ramp, exponential, random.
void drawLfoShape(LcdCanvas& cv, int x, int y, int shape);

// Value string for a raw value: raw, raw-64, or the name.
juce::String valueText(const spec::Param& p, int raw);

// Reads the LCD artwork (fonts, dial) from the user's OS file if it is not in use already; see RomArt.h. Message thread only.
// Returns true when the artwork of that file is in use (the caller then re-lays out and repaints).
bool loadLcdArt(const juce::String& osPath);

void drawLcdText(juce::Graphics& g, const spec::Font& f, const char* s, int x, int y, int scale, juce::Colour colour);
// Greedy word wrap of (caps) text to maxWidth LCD pixels; a word wider than the line stands alone.
std::vector<juce::String> wrapLcdText(const spec::Font& f, const juce::String& text, int maxWidth);

}	// namespace mm::ui
