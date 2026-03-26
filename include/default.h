#pragma once

#include <3ds.h>
#include "ft2build.h"
#include FT_FREETYPE_H

#include <string>
#include <utility>
#include <vector>

using std::pair;
using std::string;
using std::vector;

#ifndef BIT
#define BIT(n) (1u << (n))
#endif

#ifndef RGB15
#define RGB15(r, g, b) ((u16)(((r) & 31u) | (((g) & 31u) << 5) | (((b) & 31u) << 10)))
#endif

#define buttonFontSize (16)

const string& sdRootPath();
const string& appRootPath();
const string& appDataPath();
const string& appFontsPath();
const string& appTranslationsPath();
const string& appEncodingsPath();
const string& appBookmarksPath();
const string& appBooksPath();
void ensureRuntimeDirectories();

void bsod(const char* msg);
void applyBrightness();
void cycleBacklight();
void initPowerManagement();
bool pumpPowerManagement();
bool appShouldExit();
void scanKeys();
u32 keysDown();
u32 keysHeld();
u32 keysUp();
void touchRead(touchPosition* touch);
void swiWaitForVBlank();
void consoleClear();
int iprintf(const char* fmt, ...);
string extention(string name);
string noExt(string name);
string noPath(string name);

enum Layout {d0 = 0, d90, d180, d270};
enum Encoding {eUtf8, e1251};
enum scr_id {top_scr, bottom_scr};
enum fontStyle {fnormal, fbold, fitalic};
enum scrConfig {scTop, scBottom, scBoth};
enum backlightMode {blReading, blOverlay, blBoth};

void setBacklightMode(backlightMode mode);

namespace renderTech {
enum type {
	linux = (FT_LOAD_RENDER | FT_LOAD_TARGET_LCD | FT_LOAD_FORCE_AUTOHINT),
	linux_v = (FT_LOAD_RENDER | FT_LOAD_TARGET_LCD_V | FT_LOAD_FORCE_AUTOHINT),
	windows = (FT_LOAD_RENDER | FT_LOAD_TARGET_LCD),
	windows_v = (FT_LOAD_RENDER | FT_LOAD_TARGET_LCD_V),
	phone = (FT_LOAD_RENDER | FT_LOAD_TARGET_MONO)
};
}

template<class T, class U> inline void clamp(T& x, U min, U max)
{
	if(x > max) x = max;
	else if(x < min) x = min;
}

struct Color
{
	u8 R, G, B;
	inline Color(u8 r, u8 g, u8 b) : R(r), G(g), B(b) {}
	Color() : R(0), G(0), B(0) {}
	inline operator u16() const { return RGB15(R, G, B) | BIT(15); }
	void invert() { R = 31 - R; G = 31 - G; B = 31 - B; }
};

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) > (b) ? (b) : (a))

bool file_ok(const string& file_name);

namespace LWord { enum lword {

	files, resume, light,
	codep, close, justify,
	invert, gamma, colors,
	screens, rotate, pbar,
	font, size, style,
	gap, sharp, indent,
	top, bottom, both,
	language, older, newer,
	set, remove, ok,
	bookmarks,
	contents,

	totalWords
}; }

void loadTrans(string file);

extern const string transPath;
extern string locDict[LWord::totalWords];
#define SAY(i) (&locDict[LWord::i])
#define SAY2(i) locDict[LWord::i]
