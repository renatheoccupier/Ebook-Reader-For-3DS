#include "renderer.h"
#include "utf8.h"
#include "encoding_tables.h"

#include <algorithm>
#include <ctime>
#include <cstring>
#include <dirent.h>
#include <set>
#include <sys/stat.h>

#include FT_LCD_FILTER_H
#include FT_CACHE_H

namespace renderer
{

u16* bmp[2];
TFlashClock flashClock;

static u16 gBuffers[2][kBufferWidth * kBufferHeight];
static u16 gFlashBuffer[kBufferWidth * kBufferHeight];
static bool gVideoReady = false;
static bool gFrameDirty = false;
static bool gMirrorTopFromBottom = false;

static const u8 gamma150[32] = {0, 0, 1, 1, 1, 2, 3, 3, 4, 5, 6, 7, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 19, 20, 21, 22, 24, 25, 27, 28, 30, 31};
static const u8 gamma067[32] = {0, 3, 5, 7, 8, 9, 10, 11, 13, 14, 15, 16, 16, 17, 18, 19, 20, 21, 22, 22, 23, 24, 25, 25, 26, 27, 28, 28, 29, 30, 30, 31};

namespace
{

int textLimitX(scr_id scr)
{
	return screenTextWidth(scr) - 1;
}

inline u8 expand5(u8 value)
{
	return (value << 3) | (value >> 2);
}

bool entryIsDirectory(const string& basePath, const dirent* ent)
{
	if(ent == NULL) return false;
	if(ent->d_type == DT_DIR) return true;
	if(ent->d_type != DT_UNKNOWN) return false;

	struct stat st;
	return 0 == stat((basePath + ent->d_name).c_str(), &st) && S_ISDIR(st.st_mode);
}

inline void writeFramebufferPixel(u8* fb, int width, int x, int y, u16 color)
{
	if(x < 0 || x >= width || y < 0 || y >= kBufferHeight) return;
	const int pos = (x * kBufferHeight + (kBufferHeight - 1 - y)) * 3;
	fb[pos + 0] = expand5((color >> 10) & 0x1F);
	fb[pos + 1] = expand5((color >> 5) & 0x1F);
	fb[pos + 2] = expand5(color & 0x1F);
}

void fillFramebufferRect(u8* fb, int width, int x1, int y1, int x2, int y2, u16 color)
{
	if(fb == NULL) return;
	if(x1 > x2) std::swap(x1, x2);
	if(y1 > y2) std::swap(y1, y2);
	x1 = MAX(0, x1);
	y1 = MAX(0, y1);
	x2 = MIN(width - 1, x2);
	y2 = MIN(kBufferHeight - 1, y2);
	for(int x = x1; x <= x2; ++x)
		for(int y = y1; y <= y2; ++y)
			writeFramebufferPixel(fb, width, x, y, color);
}

void drawStatusBar(scr_id scr, bool framed)
{
	char buf[14];
	time_t unixTime = time(NULL);
	struct tm* timeStruct = localtime(&unixTime);
	sprintf(buf, "%02d:%02d:%02d", timeStruct->tm_hour, timeStruct->tm_min, timeStruct->tm_sec);

	const int width = textLimitX(scr);
	const int height = screenTextHeight(scr) - 1;
	const u16 y1 = height - buttonFontSize * 3 / 2;
	const u16 y2 = height;
	const u16 baselineY = height - buttonFontSize / 2;

	fillRect(0, y1, width, y2, settings::bgCol, scr);
	if(framed) rect(0, y1, width, y2, scr);
	printStr(eUtf8, scr, 5, baselineY, buf, 0, 0, buttonFontSize);
	printStr(eUtf8, scr, width - 36, baselineY, "3DS", 0, 0, buttonFontSize);
}

void fillFramebuffer(gfxScreen_t screen, u16 color)
{
	const int width = (screen == GFX_TOP) ? 400 : 320;
	u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);
	if(fb == NULL) return;
	fillFramebufferRect(fb, width, 0, 0, width - 1, kBufferHeight - 1, color);
}

void blitScreen(scr_id scr, gfxScreen_t screen)
{
	const int height = 240;
	const int width = (screen == GFX_TOP) ? 400 : 320;
	const int copyWidth = MIN(width, screenPixelWidth(scr));
	const int offsetX = 0;
	u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);
	if(fb == NULL) return;

	for(int y = 0; y < kBufferHeight; ++y) {
		for(int srcX = 0; srcX < copyWidth; ++srcX) {
			const int dstX = offsetX + srcX;
			const u16 pixel = bmp[scr][y * kBufferWidth + srcX];
			const int pos = (dstX * height + (height - 1 - y)) * 3;
			fb[pos + 0] = expand5((pixel >> 10) & 0x1F);
			fb[pos + 1] = expand5((pixel >> 5) & 0x1F);
			fb[pos + 2] = expand5(pixel & 0x1F);
		}
	}
}

} // namespace

inline void putPixel150(scr_id scr, int x, int y, Color c24)
{
	mapToScreen(scr, x, y);
	if(x < 0 || x >= kBufferWidth || y < 0 || y >= kBufferHeight) return;
	u16 p = bmp[scr][y * kBufferWidth + x];
	u8 r = p & 0x1F;
	u8 g = (p >> 5) & 0x1F;
	u8 b = (p >> 10) & 0x1F;
	using namespace settings;
	Color c5(gamma150[blend(c24.R, fCol.R, r)], gamma150[blend(c24.G, fCol.G, g)], gamma150[blend(c24.B, fCol.B, b)]);
	bmp[scr][y * kBufferWidth + x] = c5;
}

inline void putPixel067(scr_id scr, int x, int y, Color c24)
{
	mapToScreen(scr, x, y);
	if(x < 0 || x >= kBufferWidth || y < 0 || y >= kBufferHeight) return;
	u16 p = bmp[scr][y * kBufferWidth + x];
	u8 r = p & 0x1F;
	u8 g = (p >> 5) & 0x1F;
	u8 b = (p >> 10) & 0x1F;
	using namespace settings;
	Color c5(gamma067[blend(c24.R, fCol.R, r)], gamma067[blend(c24.G, fCol.G, g)], gamma067[blend(c24.B, fCol.B, b)]);
	bmp[scr][y * kBufferWidth + x] = c5;
}

template<class T> inline void swapRB(T& r, T& b, scr_id scr)
{
	switch(settings::BGR) {
		case 1: std::swap(r, b); break;
		case 2: if(scr == top_scr) std::swap(r, b); break;
		case 3: if(scr != top_scr) std::swap(r, b); break;
		default: break;
	}
}

void charLcd(scr_id scr, int x, int y, FTC_SBit bitmap)
{
	u8* srcLine = bitmap->buffer;
	u8 width = bitmap->width;
	u8 height = bitmap->height;
	u8 pitch = bitmap->pitch;

	if(settings::tech == renderTech::phone) {
		u16 col = settings::fCol;
		for(u8 j = 0; j < height; ++j, srcLine += pitch)
			for(u8 i = 0; i < width; ++i)
				if(srcLine[i >> 3] & (0x80 >> (i & 7)))
					putPixel(scr, x + i, j + y, (u16)col);
		return;
	}

	u8 gap_line, gap_pix, s0, s1, s2;
	if(settings::tech == renderTech::windows || settings::tech == renderTech::linux) {
		width /= 3;
		gap_line = pitch;
		gap_pix = 3;
		s0 = 0; s1 = 1; s2 = 2;
	}
	else {
		height /= 3;
		gap_line = 3 * pitch;
		gap_pix = 1;
		s0 = 0; s1 = pitch; s2 = pitch * 2;
	}

	swapRB(s0, s2, scr);
	if(d90 == settings::layout || d180 == settings::layout) std::swap(s0, s2);

	void (*putPix)(scr_id, int, int, Color) = &putPixel;
	if(settings::gamma != 1) {
		u8 summ = settings::fCol.R + settings::fCol.G + settings::fCol.B;
		if(settings::gamma == 0) summ = 93 - summ;
		putPix = (summ > 46) ? &putPixel067 : &putPixel150;
	}

	for(u8 j = 0; j < height; ++j, srcLine += gap_line) {
		for(u8 i = 0, *src = srcLine; i < width; ++i, src += gap_pix) {
			if(src[s0] || src[s1] || src[s2])
				putPix(scr, x + i, j + y, Color(src[s0], src[s1], src[s2]));
		}
	}
}

void initVideo()
{
	if(gVideoReady) return;
	gfxInitDefault();
	gfxSetDoubleBuffering(GFX_TOP, true);
	gfxSetDoubleBuffering(GFX_BOTTOM, true);
	bmp[bottom_scr] = gBuffers[bottom_scr];
	bmp[top_scr] = gBuffers[top_scr];
	gVideoReady = true;
	gFrameDirty = true;
	clearScreens(0);
	present();
}

void shutdownVideo()
{
	if(!gVideoReady) return;
	gfxExit();
	gVideoReady = false;
	gFrameDirty = false;
}

void markDirty()
{
	gFrameDirty = true;
}

int screenPixelWidth(scr_id scr)
{
	return (scr == top_scr) ? 400 : 320;
}

int screenTextWidth(scr_id scr)
{
	return screens::screen_text_width(scr);
}

int screenTextHeight(scr_id scr)
{
	return screens::screen_text_height(scr);
}

void mapToScreen(scr_id scr, int& x, int& y)
{
	const int width = screenTextWidth(scr);
	const int height = screenTextHeight(scr);
	switch(settings::layout) {
		case d0:
			return;
		case d90: {
			const int c = x;
			x = height - 1 - y;
			y = c;
			return;
		}
		case d180:
			x = width - 1 - x;
			y = height - 1 - y;
			return;
		case d270: {
			const int c = x;
			x = y;
			y = width - 1 - c;
			return;
		}
	}
}

void setTopScreenMirror(bool enabled)
{
	if(gMirrorTopFromBottom == enabled) return;
	gMirrorTopFromBottom = enabled;
	markDirty();
}

void present()
{
	if(!gVideoReady) return;
	if(!gFrameDirty) {
		gspWaitForVBlank();
		return;
	}
	fillFramebuffer(GFX_TOP, settings::bgCol);
	fillFramebuffer(GFX_BOTTOM, settings::bgCol);
	blitScreen(gMirrorTopFromBottom ? bottom_scr : top_scr, GFX_TOP);
	blitScreen(bottom_scr, GFX_BOTTOM);
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
	gFrameDirty = false;
}

void drawLine(int x1, int y1, int x2, int y2, u16 color, scr_id scr)
{
	mapToScreen(scr, x1, y1);
	mapToScreen(scr, x2, y2);
	markDirty();

	int deltaX = abs(x2 - x1);
	int deltaY = abs(y2 - y1);
	int signX = x1 < x2 ? 1 : -1;
	int signY = y1 < y2 ? 1 : -1;
	int error = deltaX - deltaY;
	while(true) {
		if(x1 >= 0 && x1 < screenPixelWidth(scr) && y1 >= 0 && y1 < kBufferHeight)
			bmp[scr][y1 * kBufferWidth + x1] = color;
		if(x1 == x2 && y1 == y2) break;
		int error2 = error * 2;
		if(error2 > -deltaY) {
			error -= deltaY;
			x1 += signX;
		}
		if(error2 < deltaX) {
			error += deltaX;
			y1 += signY;
		}
	}
}

void clearScreens(u16 color, u8 onlyone)
{
	const size_t pixels = kBufferWidth * kBufferHeight;
	markDirty();
	if(onlyone != top_scr && onlyone != bottom_scr) {
		std::fill_n(bmp[top_scr], pixels, color);
		std::fill_n(bmp[bottom_scr], pixels, color);
	}
	else {
		std::fill_n(bmp[onlyone], pixels, color);
	}
}

FT_Library ft_lib;
FT_Face face;
FT_Face faceB;
FT_Face faceI;
FTC_Manager ftcManager;
FTC_SBitCache ftcSBitCache;
FTC_SBit ftcSBit;
FTC_ImageTypeRec ftcImageTypeRec;
FTC_ImageType ftcImageType = &ftcImageTypeRec;

void setFontSize(u8 f)
{
	ftcImageType->height = ftcImageType->width = f;
}

void correctTech();

FT_Error ftcFaceRequester(FTC_FaceID faceID, FT_Library lib, FT_Pointer reqData, FT_Face* aface)
{
	(void)lib;
	(void)reqData;
	string* st = NULL;
	using namespace settings;
	if(faceID == &font) st = &settings::font;
	else if(faceID == &font_bold) st = &settings::font_bold;
	else if(faceID == &font_italic) st = &settings::font_italic;
	string file;
	if(st->compare(0, appFontsPath().length(), appFontsPath()) == 0) file = *st;
	else file = appFontsPath() + *st;

	FT_Error ft_err = FT_New_Face(ft_lib, file.c_str(), 0, aface);
	if(ft_err) bsod("renderer.ftcFaceRequester:Can't load font.");
	FT_Select_Charmap(*aface, FT_ENCODING_UNICODE);
	return ft_err;
}

void initFonts()
{
	setFontSize(settings::font_size);
	ftcImageType->flags = settings::tech;

	string file(appFontsPath() + settings::font);
	if(!file_ok(file)) changeFont();

	FT_Error ft_err = FT_Init_FreeType(&ft_lib);
	if(ft_err) bsod("renderer.initFonts: FT_Init_FreeType failed.");

	// Some FreeType builds omit LCD filtering support. Glyph rendering still works,
	// so treat this as an optional enhancement instead of a hard startup failure.
	FT_Library_SetLcdFilter(ft_lib, FT_LCD_FILTER_DEFAULT);

	ft_err = FTC_Manager_New(ft_lib, 3, 0, 0, ftcFaceRequester, 0, &ftcManager);
	if(ft_err) bsod("renderer.initFonts: FTC_Manager_New failed.");

	ft_err = FTC_SBitCache_New(ftcManager, &ftcSBitCache);
	if(ft_err) bsod("renderer.initFonts: FTC_SBitCache_New failed.");

	if(FTC_Manager_LookupFace(ftcManager, &settings::font, &face))
		bsod("renderer.initFonts: Can't load regular font.");
	if(FTC_Manager_LookupFace(ftcManager, &settings::font_bold, &faceB))
		bsod("renderer.initFonts: Can't load bold font.");
	if(FTC_Manager_LookupFace(ftcManager, &settings::font_italic, &faceI))
		bsod("renderer.initFonts: Can't load italic font.");
}

FT_Face* selectStyle(fontStyle style)
{
	FT_Face* f[] = {&face, &faceB, &faceI};
	string* f2[] = {&settings::font, &settings::font_bold, &settings::font_italic};
	ftcImageType->face_id = f2[style];
	return f[style];
}

int printStr(Encoding enc, scr_id scr, u16 x, u16 y, const string& str, u32 start, u32 end, u8 fontSize, fontStyle style)
{
	setFontSize(fontSize);
	correctTech();
	const u16 startx = x;
	if(0 == end) end = str.size();
	if(end > str.size() || start >= end) return 0;
	FT_Face* fc = selectStyle(style);
	const bool use_kern = false;
	FT_UInt glyph_index, old_gi = 0;
	FT_Vector delta;
	for(const char* str_it = &str[start]; str_it < &str[end]; ) {
		u32 cp;
		if(eUtf8 == enc) cp = utf8::unchecked::next(str_it);
		else cp = cp1251toUtf32[(u8)*str_it++];
		if(cp == L'­') continue;
		glyph_index = FT_Get_Char_Index(*fc, cp);
		if(use_kern) {
			FT_Get_Kerning(*fc, old_gi, glyph_index, FT_KERNING_DEFAULT, &delta);
			x += delta.x >> 6;
		}
		FT_Error err = FTC_SBitCache_Lookup(ftcSBitCache, ftcImageType, glyph_index, &ftcSBit, NULL);
		if(err) {
			old_gi = 0;
			continue;
		}
		if(x + ftcSBit->xadvance > textLimitX(scr)) break;
		charLcd(scr, x + ftcSBit->left, y - ftcSBit->top, ftcSBit);
		x += ftcSBit->xadvance;
		old_gi = glyph_index;
	}
	return x - startx;
}

int strWidth(Encoding enc, const string& str, u32 start, u32 end, u8 fontSize, fontStyle style, int* breakat, int spaceleft)
{
	if(breakat != NULL) *breakat = 0;
	setFontSize(fontSize);
	correctTech();
	int width = 0;
	if(0 == end) end = str.length();
	if(end > str.size() || start >= end) return 0;
	FT_Face* fc = selectStyle(style);
	const bool use_kern = false;
	FT_UInt glyph_index, old_gi = 0;
	FT_Vector delta;
	int i = 0;
	int prev_width = 0;
	for(const char* str_it = &str[start], *prev = NULL; str_it < &str[end]; ) {
		if(breakat != NULL) {
			if(prev != NULL) *breakat = i - 1;
			if(width >= spaceleft) {
				width = prev_width;
				break;
			}
		}
		prev_width = width;
		prev = str_it;
		i++;

		u32 cp;
		if(eUtf8 == enc) cp = utf8::unchecked::next(str_it);
		else cp = cp1251toUtf32[(u8)*str_it++];
		if(cp == L'­') continue;
		glyph_index = FT_Get_Char_Index(*fc, cp);
		if(use_kern) {
			FT_Get_Kerning(*fc, old_gi, glyph_index, FT_KERNING_DEFAULT, &delta);
			width += delta.x >> 6;
		}
		FT_Error err = FTC_SBitCache_Lookup(ftcSBitCache, ftcImageType, glyph_index, &ftcSBit, NULL);
		if(err) {
			old_gi = 0;
			continue;
		}
		width += ftcSBit->xadvance;
		if(width > MAX(screens::layoutX(), screenTextWidth(top_scr) - 1)) return 9999;
		old_gi = glyph_index;
	}
	return width;
}

void rect(u16 x1, u16 y1, u16 x2, u16 y2, scr_id scr)
{
	const u16 gray = Blend(128);
	drawLine(x1, y1 + 2, x1, y2 - 2, gray, scr);
	drawLine(x2, y1 + 2, x2, y2 - 2, gray, scr);
	drawLine(x1 + 2, y1, x2 - 2, y1, gray, scr);
	drawLine(x1 + 2, y2, x2 - 2, y2, gray, scr);
	putPixel(scr, x1 + 1, y1 + 1, gray);
	putPixel(scr, x2 - 1, y1 + 1, gray);
	putPixel(scr, x1 + 1, y2 - 1, gray);
	putPixel(scr, x2 - 1, y2 - 1, gray);
}

void fillRect(u16 x1, u16 y1, u16 x2, u16 y2, u16 col, scr_id scr)
{
	drawLine(x1 + 2, y1, x2 - 2, y1, col, scr);
	drawLine(x1 + 1, y1 + 1, x2 - 1, y1 + 1, col, scr);
	for(u32 i = y1 + 2u; i <= y2 - 2u; ++i)
		drawLine(x1, i, x2, i, col, scr);
	drawLine(x1 + 1, y2 - 1, x2 - 1, y2 - 1, col, scr);
	drawLine(x1 + 2, y1, x2 - 2, y1, col, scr);
}

void vLine(u16 x, u16 y1, u16 y2, u16 col)
{
	for(u32 i = y1; i <= y2; ++i)
		putPixel(bottom_scr, x, i, col);
}

void drawImageSlice(scr_id scr, int x, int y, const vector<u16>& pixels, u16 width, u16 height, u16 srcY, u16 sliceHeight)
{
	if(pixels.empty() || 0 == width || 0 == height || srcY >= height) return;

	int srcX = 0;
	int drawWidth = width;
	int drawHeight = height - srcY;
	if(drawHeight > sliceHeight) drawHeight = sliceHeight;
	if(drawWidth <= 0 || drawHeight <= 0) return;

	if(x < 0) {
		srcX = -x;
		drawWidth -= srcX;
		x = 0;
	}
	if(y < 0) {
		const int skipRows = -y;
		srcY += skipRows;
		drawHeight -= skipRows;
		y = 0;
	}

	const int maxWidth = screenTextWidth(scr);
	const int maxHeight = screenTextHeight(scr);
	if(x >= maxWidth || y >= maxHeight) return;
	if(x + drawWidth > maxWidth) drawWidth = maxWidth - x;
	if(y + drawHeight > maxHeight) drawHeight = maxHeight - y;
	if(drawWidth <= 0 || drawHeight <= 0) return;

	for(int row = 0; row < drawHeight; ++row) {
		const u32 offset = (srcY + row) * width + srcX;
		for(int col = 0; col < drawWidth; ++col)
			putPixel(scr, x + col, y + row, pixels[offset + col]);
	}
}

void correctTech()
{
	using namespace settings;
	using namespace renderTech;
	if(windows == tech || windows_v == tech)
		tech = (layout == d0 || layout == d180) ? windows : windows_v;
	else if(linux == tech || linux_v == tech)
		tech = (layout == d0 || layout == d180) ? linux : linux_v;
	ftcImageType->flags = tech;
}

void printClock(scr_id scr, bool forced)
{
	static int olds = -1;
	time_t unixTime = time(NULL);
	struct tm* timeStruct = localtime(&unixTime);
	int s = timeStruct->tm_sec;
	if(s == olds && !forced) return;
	olds = s;
	drawStatusBar(scr, false);
}

void changeFont()
{
	DIR* dir = opendir(appFontsPath().c_str());
	if(dir == NULL) bsod("renderer.changeFont:Cannot open fonts directory.");
	struct dirent* ent;
	std::set<string> files;
	while((ent = readdir(dir)) != NULL) {
		if(!entryIsDirectory(appFontsPath(), ent) && extention(ent->d_name) == "ttf")
			files.insert(noExt(ent->d_name));
	}
	closedir(dir);

	std::set<string> fonts;
	for(std::set<string>::iterator it = files.begin(); it != files.end(); ++it) {
		if(files.find(*it + 'b') != files.end() && files.find(*it + 'i') != files.end())
			fonts.insert(*it);
	}

	if(fonts.empty()) bsod("renderer.changeFont:No fonts found.");

	std::set<string>::iterator current = fonts.find(noExt(settings::font));
	if(current != fonts.end()) ++current;
	if(current == fonts.end()) current = fonts.begin();

	using namespace settings;
	font = *current + ".ttf";
	font_bold = *current + "b.ttf";
	font_italic = *current + "i.ttf";
	FTC_Manager_Done(ftcManager);
	FT_Done_FreeType(ft_lib);
	initFonts();
	save();
}

void TFlashClock::show(scr_id scr)
{
	_scr = scr;
	frames = 0;
	memcpy(gFlashBuffer, bmp[_scr], sizeof(gFlashBuffer));
	drawStatusBar(scr, true);
}

void TFlashClock::hide()
{
	if(++frames > 20 && _scr != 66) {
		memcpy(bmp[_scr], gFlashBuffer, sizeof(gFlashBuffer));
		_scr = 66;
	}
}

} // namespace renderer
