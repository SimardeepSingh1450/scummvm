/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef SCI_GRAPHICS_TEXT16_H
#define SCI_GRAPHICS_TEXT16_H

namespace Graphics {
class Font;
}

namespace Sci {

#define SCI_TEXT16_ALIGNMENT_RIGHT -1
#define SCI_TEXT16_ALIGNMENT_CENTER 1
#define SCI_TEXT16_ALIGNMENT_LEFT	0

typedef Common::Array<Common::Rect> CodeRefRectArray;

class GfxPorts;
class GfxPaint16;
class GfxScreen;
class GfxFont;
class GfxMacFontManager;
/**
 * Text16 class, handles text calculation and displaying of text for SCI0->SCI1.1 games
 */
class GfxText16 {
public:
	GfxText16(GfxCache *fonts, GfxPorts *ports, GfxPaint16 *paint16, GfxScreen *screen, GfxMacFontManager *macFontManager);
	~GfxText16();

	GuiResourceId GetFontId();
	GfxFont *GetFont();
	void SetFont(GuiResourceId fontId);

	int16 CodeProcessing(const char *&text, GuiResourceId orgFontId, int16 orgPenColor, bool doingDrawing);

#if 0
	void ClearChar(int16 chr);
#endif

	int16 GetLongest(const char *&text, int16 maxWidth, GuiResourceId orgFontId);
	void Width(const char *text, int16 from, int16 len, GuiResourceId orgFontId, int16 &textWidth, int16 &textHeight, bool restoreFont);
	void StringWidth(const Common::String &str, GuiResourceId orgFontId, int16 &textWidth, int16 &textHeight);
#if 0
	void ShowString(const Common::String &str, GuiResourceId orgFontId, int16 orgPenColor);
#endif
	void DrawString(const Common::String &str, GuiResourceId orgFontId, int16 orgPenColor);
	int16 Size(Common::Rect &rect, const char *text, uint16 textLanguage, GuiResourceId fontId, int16 maxWidth);
	void Draw(const char *text, int16 from, int16 len, GuiResourceId orgFontId, int16 orgPenColor);
	void Show(const char *text, int16 from, int16 len, GuiResourceId orgFontId, int16 orgPenColor);
	void Box(const char *text, uint16 languageSplitter, bool show, const Common::Rect &rect, TextAlignment alignment, GuiResourceId fontId);

	void Box(const char *text, bool show, const Common::Rect &rect, TextAlignment alignment, GuiResourceId fontId) {
		Box(text, 0, show, rect, alignment, fontId);
	}

	void DrawString(const Common::String &str);
	void DrawStatus(const Common::String &str);

	GfxFont *_font;

	reg_t allocAndFillReferenceRectArray();

	void kernelTextSize(const char *text, uint16 textLanguage, int16 font, int16 maxWidth, int16 *textWidth, int16 *textHeight);
	void kernelTextFonts(int argc, reg_t *argv);
	void kernelTextColors(int argc, reg_t *argv);

	void macTextSize(const Common::String &text, GuiResourceId sciFontId, GuiResourceId origSciFontId, int16 maxWidth, int16 *textWidth, int16 *textHeight);
	void macDraw(const Common::String &text, Common::Rect rect, TextAlignment alignment, GuiResourceId sciFontId, GuiResourceId origSciFontId, int16 color);
private:
	void init();
	bool SwitchToFont1001OnKorean(const char *text, uint16 languageSplitter);
	bool SwitchToFont900OnSjis(const char *text, uint16 languageSplitter);
	static bool isJapaneseNewLine(int16 curChar, int16 nextChar);
	int16 macGetLongest(const Common::String &text, uint start, const Graphics::Font *font, int16 maxWidth, int16 *lineWidth);

	GfxCache *_cache;
	GfxPorts *_ports;
	GfxPaint16 *_paint16;
	GfxScreen *_screen;
	GfxMacFontManager *_macFontManager; // null when not applicable

	int _codeFontsCount;
	GuiResourceId *_codeFonts;
	int _codeColorsCount;
	uint16 *_codeColors;

	bool _useEarlyGetLongestTextCalculations;

	Common::Rect _codeRefTempRect;
	CodeRefRectArray _codeRefRects;
};

} // End of namespace Sci

#endif
