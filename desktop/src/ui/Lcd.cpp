// Adapted from shnolk's Monomodule (src/plugin/one/Lcd.cpp), AGPL-3.0; the Machinedrum's cell kinds (text, LFO shape, toggle) are added.
#include "Lcd.h"
#include <cmath>

namespace mm::ui {

void LcdCanvas::blit(const spec::Bitmap& b, int x, int y, bool on)
{
	for(int r = 0; r < b.h; ++r)
		for(int c = 0; c < b.w; ++c)
			if(b.lit(c, r)) set(x + c, y + r, on);
}

void LcdCanvas::fillRect(int x, int y, int w, int h, bool on)
{
	for(int r = 0; r < h; ++r)
		for(int c = 0; c < w; ++c) set(x + c, y + r, on);
}

void LcdCanvas::invertRect(int x, int y, int w, int h)
{
	for(int r = 0; r < h; ++r)
		for(int c = 0; c < w; ++c) set(x + c, y + r, !get(x + c, y + r));
}

void LcdCanvas::dotsH(int x0, int x1, int y) { for(int x = x0; x <= x1; x += 2) set(x, y); }
void LcdCanvas::dotsV(int x, int y0, int y1) { for(int y = y0; y <= y1; y += 2) set(x, y); }

void LcdCanvas::line(int x0, int y0, int x1, int y1, bool on)
{
	const int n = std::max({std::abs(x1 - x0), std::abs(y1 - y0), 1});
	for(int i = 0; i <= n; ++i) set(x0 + int(std::lround(double(x1 - x0) * i / n)), y0 + int(std::lround(double(y1 - y0) * i / n)), on);
}

int LcdCanvas::textWidth(const spec::Font& f, const char* s)
{
	int w = 0;
	for(const char* c = s; *c; ++c)
	{
		const auto* g = f.glyph(static_cast<unsigned char>(*c));
		w += (g ? g->w : f.adv) + 1;
	}
	return w > 0 ? w - 1 : 0;
}

void LcdCanvas::text(const spec::Font& f, const char* s, int x, int y, bool on)
{
	for(const char* c = s; *c; ++c)
	{
		const auto* g = f.glyph(static_cast<unsigned char>(*c));
		if(g) { blit(*g, x, y, on); x += g->w + 1; }
		else x += f.adv + 1;
	}
}

void LcdCanvas::draw(juce::Graphics& g, int destX, int destY, int scale) const
{
	juce::Image img(juce::Image::ARGB, m_w, m_h, false);
	{
		juce::Image::BitmapData bd(img, juce::Image::BitmapData::writeOnly);
		for(int y = 0; y < m_h; ++y)
			for(int x = 0; x < m_w; ++x)
				bd.setPixelColour(x, y, m_px[size_t(y * m_w + x)] ? lcd::ink : lcd::paper);
	}
	g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
	g.drawImageTransformed(img, juce::AffineTransform::scale(float(scale)).translated(float(destX), float(destY)));
}

// ---------------------------------------------------------------------------

bool loadLcdArt(const juce::String& osPath)
{
	return osPath.isNotEmpty() && spec::ensureRomArt(std::filesystem::path(osPath.toStdString()));
}

juce::String valueText(const spec::Param& p, int raw)
{
	raw = juce::jlimit(0, int(p.maxRaw), raw);
	switch(p.display)
	{
		case spec::Display::Text: return p.values && p.values[raw] ? juce::String(p.values[raw]) : juce::String();
		case spec::Display::Bipolar: { const int b = raw - 64; return b > 0 ? "+" + juce::String(b) : juce::String(b); }
		case spec::Display::Toggle: return raw >= 64 ? "ON" : "OFF";
		case spec::Display::Shape: return juce::String(raw + 1);
		default: return juce::String(raw);
	}
}

// The MD's six LFO shapes (captured from its LFO page), 17 x 9.
void drawLfoShape(LcdCanvas& cv, int x, int y, int k)
{
	switch(k)
	{
		case 0: cv.line(x, y + 8, x + 8, y); cv.line(x + 8, y, x + 16, y + 8); break;	// triangle
		case 1: cv.line(x, y + 8, x + 7, y); cv.line(x + 7, y, x + 7, y + 8); cv.line(x + 8, y + 8, x + 15, y); cv.line(x + 15, y, x + 15, y + 8); break;	// saw
		case 2: cv.line(x, y + 8, x + 3, y + 8); cv.line(x + 3, y + 8, x + 3, y); cv.line(x + 3, y, x + 11, y); cv.line(x + 11, y, x + 11, y + 8); cv.line(x + 11, y + 8, x + 16, y + 8); break;	// square
		case 3: cv.line(x, y, x + 3, y); cv.line(x + 3, y, x + 11, y + 8); cv.line(x + 11, y + 8, x + 16, y + 8); break;	// ramp
		case 4:	// exponential
			cv.line(x, y + 8, x, y);
			for(int c = 1; c < 17; ++c) cv.set(x + c, y + int(std::lround(8 * (1 - std::exp(-c / 2.5)))));
			break;
		default:	// random
		{
			static const int hs[6] = {6, 2, 5, 0, 8, 3};
			for(int i = 0; i < 6; ++i)
			{
				cv.line(x + i * 3, y + hs[i], x + i * 3 + 2, y + hs[i]);
				if(i) cv.line(x + i * 3, y + hs[i - 1], x + i * 3, y + hs[i]);
			}
		}
	}
}

void drawKnobCell(LcdCanvas& cv, int x0, int y0, const spec::Param& p, int raw, bool highlightValue)
{
	raw = juce::jlimit(0, int(p.maxRaw), raw);
	// dotted top and left edge (the grid's right/bottom edges are the neighbours' left/top)
	cv.dotsH(x0, x0 + kCell, y0);
	cv.dotsV(x0, y0, y0 + kCell - 1);
	if(p.display == spec::Display::Blank) return;	// nothing at all, not even the label

	const auto& font = spec::kFontTiny3x5();
	const int innerX = x0 + 1, innerW = kCell - 1;
	cv.textCentred(font, p.label, innerX, innerW, y0 + kLabelY);

	const auto box = knobValueBox(x0, y0);
	switch(p.display)
	{
		case spec::Display::Numeric:
		case spec::Display::Bipolar:
		{
			const int rx = innerX + (innerW - spec::kDialRing().w) / 2, ry = y0 + kContentY + (kContentH - spec::kDialRing().h) / 2;
			cv.blit(spec::kDialRing(), rx, ry);
			cv.blit(spec::kDialDot(raw), rx + kDotX, ry + kDotY);
			const juce::String value = valueText(p, raw);
			cv.textCentred(font, value.toRawUTF8(), box.getX(), box.getWidth(), box.getY() + (box.getHeight() - font.h) / 2);
			break;
		}
		case spec::Display::Text:
		{
			const juce::String value = valueText(p, raw);
			cv.textCentred(font, value.toRawUTF8(), innerX, innerW, y0 + kContentY + (kContentH + kValueBoxH - font.h) / 2);
			break;
		}
		case spec::Display::Shape:
			drawLfoShape(cv, innerX + (innerW - 17) / 2, y0 + kContentY + (kContentH + kValueBoxH - 9) / 2, raw);
			break;
		case spec::Display::Toggle:
		{
			const auto& ic = spec::kToggle(raw >= 64);
			cv.blit(ic, innerX + (innerW - ic.w) / 2, y0 + kContentY + (kContentH - ic.h) / 2);
			cv.textCentred(font, raw >= 64 ? "ON" : "OFF", box.getX(), box.getWidth(), box.getY() + (box.getHeight() - font.h) / 2);
			break;
		}
		default: break;
	}
	if(highlightValue && (p.display == spec::Display::Numeric || p.display == spec::Display::Bipolar)) cv.invertRect(box.getX(), box.getY(), box.getWidth(), box.getHeight());
}

void drawLcdText(juce::Graphics& g, const spec::Font& f, const char* s, int x, int y, int scale, juce::Colour colour)
{
	const int w = LcdCanvas::textWidth(f, s), h = f.h;
	if(w <= 0) return;
	LcdCanvas cv(w, h);
	cv.text(f, s, 0, 0);
	g.setColour(colour);
	for(int r = 0; r < h; ++r)
		for(int c = 0; c < w; ++c)
			if(cv.get(c, r)) g.fillRect(x + c * scale, y + r * scale, scale, scale);
}

std::vector<juce::String> wrapLcdText(const spec::Font& f, const juce::String& text, int maxWidth)
{
	std::vector<juce::String> lines;
	juce::String line;
	for(const auto& word : juce::StringArray::fromTokens(text, " ", ""))
	{
		if(word.isEmpty()) continue;
		const juce::String candidate = line.isEmpty() ? word : line + " " + word;
		if(line.isEmpty() || LcdCanvas::textWidth(f, candidate.toRawUTF8()) <= maxWidth) line = candidate;
		else { lines.push_back(line); line = word; }
	}
	if(line.isNotEmpty()) lines.push_back(line);
	return lines;
}

}	// namespace mm::ui
