#include "renderer.h"
#include "epub.h"
#include "settings.h"
#include "button.h"
#include "file_browser.h"
#include "utf8.h"

#include <png.h>

#include <cstring>
#include <dirent.h>
#include <fstream>
#include <vector>

bool file_ok(const string& file_name);
static bool book_ok(const string& file_name);
static Layout gReadingLayout = d0;

namespace
{

const string kMenuTitle("EBook Reader");
const string kMenuSubtitle("Nintendo 3DS EPUB Library");
vector<button> gMenuButtons;
vector<const string*> gMenuActions;
vector<u8> gMenuArtRgba;
u32 gMenuArtWidth = 0;
u32 gMenuArtHeight = 0;
bool gMenuArtTried = false;

void waitForInputRelease()
{
	while(pumpPowerManagement()) {
		swiWaitForVBlank();
		scanKeys();
		if(0 == keysHeld()) return;
	}
}

u8 expand5(u8 value)
{
	return (value << 3) | (value >> 2);
}

bool loadMenuArtFile(const string& path)
{
	png_image image;
	std::memset(&image, 0, sizeof(image));
	image.version = PNG_IMAGE_VERSION;
	if(!png_image_begin_read_from_file(&image, path.c_str()))
		return false;

	image.format = PNG_FORMAT_RGBA;
	vector<png_byte> rgba(PNG_IMAGE_SIZE(image));
	const bool ok = 0 != png_image_finish_read(&image, NULL, rgba.data(), 0, NULL);
	const u32 width = image.width;
	const u32 height = image.height;
	png_image_free(&image);
	if(!ok) return false;

	gMenuArtRgba.assign(rgba.begin(), rgba.end());
	gMenuArtWidth = width;
	gMenuArtHeight = height;
	return true;
}

bool ensureMenuArtLoaded()
{
	if(gMenuArtTried) return !gMenuArtRgba.empty();
	gMenuArtTried = true;
	return loadMenuArtFile(appDataPath() + "/rena.png") || loadMenuArtFile("data/rena.png");
}

u16 blendMenuArtPixel(u32 index)
{
	const u8 r = gMenuArtRgba[index];
	const u8 g = gMenuArtRgba[index + 1];
	const u8 b = gMenuArtRgba[index + 2];
	const u8 a = gMenuArtRgba[index + 3];
	if(a == 255)
		return RGB15(r >> 3, g >> 3, b >> 3) | BIT(15);

	const u8 bgR = expand5(settings::bgCol.R);
	const u8 bgG = expand5(settings::bgCol.G);
	const u8 bgB = expand5(settings::bgCol.B);
	const u8 outR = (r * a + bgR * (255 - a)) / 255;
	const u8 outG = (g * a + bgG * (255 - a)) / 255;
	const u8 outB = (b * a + bgB * (255 - a)) / 255;
	return RGB15(outR >> 3, outG >> 3, outB >> 3) | BIT(15);
}

bool drawMenuArt(int x1, int y1, int x2, int y2)
{
	if(!ensureMenuArtLoaded() || gMenuArtRgba.empty() || gMenuArtWidth == 0 || gMenuArtHeight == 0)
		return false;

	const int boxW = x2 - x1 + 1;
	const int boxH = y2 - y1 + 1;
	if(boxW <= 0 || boxH <= 0) return false;

	const double scaleX = double(boxW) / double(gMenuArtWidth);
	const double scaleY = double(boxH) / double(gMenuArtHeight);
	const double scale = MIN(scaleX, scaleY);
	if(scale <= 0.0) return false;

	const int drawW = MAX(1, int(gMenuArtWidth * scale));
	const int drawH = MAX(1, int(gMenuArtHeight * scale));
	const int drawX = x1 + (boxW - drawW) / 2;
	const int drawY = y1 + (boxH - drawH) / 2;
	renderer::fillRect(x1, y1, x2, y2, Blend(10), top_scr);
	renderer::rect(x1, y1, x2, y2, top_scr);
	for(int y = 0; y < drawH; ++y) {
		const u32 srcY = u32(y) * gMenuArtHeight / drawH;
		for(int x = 0; x < drawW; ++x) {
			const u32 srcX = u32(x) * gMenuArtWidth / drawW;
			const u32 index = 4u * (srcY * gMenuArtWidth + srcX);
			if(index + 3u >= gMenuArtRgba.size()) continue;
			if(gMenuArtRgba[index + 3u] < 8u) continue;
			renderer::putPixel(top_scr, drawX + x, drawY + y, blendMenuArtPixel(index));
		}
	}
	return true;
}

void drawCardTitle(int x, int y, const string& title)
{
	renderer::printStr(eUtf8, top_scr, x, y, title, 0, 0, 10);
}

u32 advanceUtf8(const string& str, u32 start, int count)
{
	const char* it = str.c_str() + start;
	const char* end = str.c_str() + str.size();
	for(int i = 0; i < count && it < end; ++i)
		utf8::unchecked::next(it);
	return it - str.c_str();
}

u32 clippedUtf8End(const string& str, u32 start, int width, u32 fontSize)
{
	int breakat = 0;
	renderer::strWidth(eUtf8, str, start, 0, fontSize, fnormal, &breakat, width);
	if(breakat <= 0) breakat = 1;
	return advanceUtf8(str, start, breakat);
}

string ellipsizedSlice(const string& text, u32 start, int width, u32 fontSize)
{
	const u32 end = clippedUtf8End(text, start, width, fontSize);
	if(end >= text.size()) return text.substr(start, end - start);

	const string ellipsis("...");
	const int ellipsisWidth = renderer::strWidth(eUtf8, ellipsis, 0, 0, fontSize);
	if(ellipsisWidth >= width) return text.substr(start, end - start);

	const u32 clipped = clippedUtf8End(text, start, width - ellipsisWidth, fontSize);
	return text.substr(start, clipped - start) + ellipsis;
}

void drawCardValue(int x, int y, int width, const string& value, u32 fontSize, u32 maxLines)
{
	const string text = value.empty() ? string("None") : value;
	u32 start = 0;
	int baseline = y;
	for(u32 line = 0; line < maxLines && start < text.size(); ++line) {
		u32 end = clippedUtf8End(text, start, width, fontSize);
		if(line + 1 < maxLines || end >= text.size())
			renderer::printStr(eUtf8, top_scr, x, baseline, text, start, end, fontSize);
		else {
			const string clipped = ellipsizedSlice(text, start, width, fontSize);
			renderer::printStr(eUtf8, top_scr, x, baseline, clipped, 0, 0, fontSize);
			break;
		}
		start = end;
		while(start < text.size() && text[start] == ' ') ++start;
		baseline += fontSize + 2;
	}
}

void drawMenuButtonStrip()
{
	gMenuButtons.clear();
	gMenuActions.clear();
	renderer::clearScreens(settings::bgCol, bottom_scr);

	const bool portrait = screens::layoutY() > screens::layoutX();
	const int width = screens::layoutX();
	const int height = screens::layoutY();
	const bool canResume = book_ok(settings::recent_book);
	const int buttonHeight = portrait ? 56 : 52;
	const int gap = 12;

	if(canResume && !portrait) {
		const int buttonWidth = MIN(150, (width - 34) / 2);
		const int left = (width - (buttonWidth * 2 + gap)) / 2;
		const int top = MAX(24, height - buttonHeight - 36);
		button resume("Resume", left, top, left + buttonWidth, top + buttonHeight, 16);
		resume.enableAutoFit(12);
		gMenuButtons.push_back(resume);
		gMenuActions.push_back(SAY(resume));
		button files("Files", left + buttonWidth + gap, top, left + buttonWidth * 2 + gap, top + buttonHeight, 16);
		files.enableAutoFit(12);
		gMenuButtons.push_back(files);
		gMenuActions.push_back(SAY(files));
	}
	else {
		const int count = canResume ? 2 : 1;
		const int sidePad = portrait ? 40 : 20;
		const int buttonWidth = portrait ? MAX(120, width - sidePad * 2) : MIN(width - 40, 220);
		const int left = MAX(sidePad, (width - buttonWidth) / 2);
		const int totalHeight = count * buttonHeight + (count - 1) * gap;
		int top = portrait ? MAX(22, (height - totalHeight) / 2 - 12) : MAX(24, height - totalHeight - 28);

		if(canResume) {
			button resume("Resume", left, top, left + buttonWidth, top + buttonHeight, 16);
			resume.enableAutoFit(12);
			gMenuButtons.push_back(resume);
			gMenuActions.push_back(SAY(resume));
			top += buttonHeight + gap;
		}

		button files("Files", left, top, left + buttonWidth, top + buttonHeight, 16);
		files.enableAutoFit(12);
		gMenuButtons.push_back(files);
		gMenuActions.push_back(SAY(files));
	}

	for(u32 i = 0; i < gMenuButtons.size(); ++i)
		gMenuButtons[i].draw();
	renderer::printClock(bottom_scr, true);
}

void drawMenuTopScreen()
{
	const int width = renderer::screenTextWidth(top_scr) - 1;
	const int height = renderer::screenTextHeight(top_scr) - 1;
	const int left = 12;
	const int right = width - 12;
	const bool portrait = width < 300;
	const bool canResume = !settings::recent_book.empty() && book_ok(settings::recent_book);
	const string recentLabel = canResume
		? noExt(noPath(settings::recent_book))
		: string("No recent book yet. Open Files on the bottom screen to browse your EPUB library.");

	renderer::clearScreens(settings::bgCol, top_scr);
	renderer::fillRect(0, 0, width, portrait ? 42 : 50, Blend(28), top_scr);
	renderer::fillRect(0, portrait ? 42 : 50, width, portrait ? 44 : 52, Blend(72), top_scr);
	renderer::printStr(eUtf8, top_scr, 16, portrait ? 22 : 24, kMenuTitle, 0, 0, portrait ? 22 : 25);
	renderer::printStr(eUtf8, top_scr, 18, portrait ? 38 : 42, kMenuSubtitle, 0, 0, 11);

	if(!portrait) {
		renderer::fillRect(left, 70, right, height - 16, Blend(18), top_scr);
		renderer::rect(left, 70, right, height - 16, top_scr);
		const bool hasArt = drawMenuArt(right - 134, 78, right - 18, height - 24);
		const int textRight = hasArt ? right - 148 : right - 12;
		drawCardTitle(left + 12, 92, "Recent Book");
		drawCardValue(left + 12, 118, textRight - left, recentLabel, canResume ? 16 : 12, canResume ? 4 : 5);
	}
	else {
		renderer::fillRect(left, 60, right, height - 18, Blend(18), top_scr);
		renderer::rect(left, 60, right, height - 18, top_scr);
		drawCardTitle(left + 12, 82, "Recent Book");
		drawCardValue(left + 12, 112, right - left - 24, recentLabel, canResume ? 11 : 10, canResume ? 3 : 4);
		drawMenuArt(left + 16, 164, right - 16, height - 30);
	}
}

} // namespace

bool file_ok(const string& file_name)
{
	return std::ifstream(file_name.c_str()).good();
}

static bool book_ok(const string& file_name)
{
	return !file_name.empty() && file_ok(file_name) && extention(file_name) == "epub";
}

void drawMenu()
{
	renderer::setTopScreenMirror(false);
	renderer::clearScreens(settings::bgCol);
	drawMenuTopScreen();
	drawMenuButtonStrip();
	setBacklightMode(blOverlay);
}

void openBook(const string& file)
{
	if(extention(file) != "epub")
		bsod("main:Unsupported format.\n\nOnly EPUB files are supported.");
	epub_book(file).read();
}

string browseForBook()
{
	file_browser browser;
	return browser.run();
}

void browseLoop()
{
	while(pumpPowerManagement()) {
		const string file = browseForBook();
		if(file.empty()) {
			if(appShouldExit()) return;
			drawMenu();
			return;
		}
		waitForInputRelease();
		if(appShouldExit()) return;
		settings::layout = gReadingLayout;
		openBook(file);
		if(appShouldExit()) return;
		gReadingLayout = settings::layout;
	}
}

int main(int argc, char* argv[])
{
	renderer::initVideo();
	ensureRuntimeDirectories();

	string binname = "EBookReaderFor3DS";
	string argfile;
	if(argc) {
		binname = argv[0];
		if(binname.length() > 5 && !binname.compare(binname.length() - 5, 5, ".3dsx"))
			binname.erase(binname.length() - 5);
		u32 found = binname.find_last_of('/');
		if(found != string::npos) binname.erase(0, found + 1);
		if(argc >= 2) argfile = argv[1];
	}
	settings::binname = binname;

	DIR* fontsDir = opendir(appFontsPath().c_str());
	if(fontsDir == NULL)
		bsod("main:\nMissing runtime font data.\nCopy EBookReaderFor3DS/sdmc_template/data\ninto sdmc:/3ds/EBookReaderFor3DS/data");
	closedir(fontsDir);
	DIR* transDir = opendir(appTranslationsPath().c_str());
	if(transDir == NULL)
		bsod("main:\nMissing translation data.\nCopy EBookReaderFor3DS/sdmc_template/data\ninto sdmc:/3ds/EBookReaderFor3DS/data");
	closedir(transDir);

	settings::load();
	gReadingLayout = settings::layout;
	applyBrightness();
	initPowerManagement();
	renderer::initFonts();
	string trans = transPath + settings::translname;
	if(file_ok(trans)) loadTrans(trans);

	if(book_ok(argfile)) {
		settings::layout = gReadingLayout;
		openBook(argfile);
		gReadingLayout = settings::layout;
	}
	if(appShouldExit()) {
		renderer::shutdownVideo();
		return 0;
	}
	drawMenu();
	while(pumpPowerManagement()) {
		swiWaitForVBlank();
		renderer::printClock(bottom_scr);
		scanKeys();
		const int down = keysDown();
		if(down & KEY_START) break;
		if(down & (KEY_A | KEY_RIGHT | KEY_R)) {
			browseLoop();
			if(appShouldExit()) break;
			continue;
		}
		if(!(down & KEY_TOUCH)) continue;
		for(u32 i = 0; i < gMenuButtons.size(); ++i) {
			if(!gMenuButtons[i].touched()) continue;
			const string* t = gMenuActions[i];
			if(SAY(files) == t) {
				waitForInputRelease();
				browseLoop();
			}
			else if(SAY(resume) == t && book_ok(settings::recent_book)) {
				waitForInputRelease();
				if(appShouldExit()) break;
				settings::layout = gReadingLayout;
				openBook(settings::recent_book);
				gReadingLayout = settings::layout;
				drawMenu();
			}
			break;
		}
	}

	renderer::shutdownVideo();
	return 0;
}
