#include "renderer.h"
#include "epub.h"
#include "settings.h"
#include "button.h"
#include "file_browser.h"
#include "controls.h"
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
int gMenuCursor = 0;
vector<u8> gMenuArtRgba;
u32 gMenuArtWidth = 0;
u32 gMenuArtHeight = 0;
bool gMenuArtTried = false;
u32 gMenuTitleTick = 0;
bool gMenuNeedsTitleMarquee = false;

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

bool menuHasArt()
{
	return ensureMenuArtLoaded() && !gMenuArtRgba.empty() && gMenuArtWidth != 0 && gMenuArtHeight != 0;
}

string menuRecentLabel(bool& canResume)
{
	canResume = !settings::recent_book.empty() && book_ok(settings::recent_book);
	return canResume
		? noExt(noPath(settings::recent_book))
		: string("No recent book yet. Open Files on the bottom screen to browse your EPUB library.");
}

bool drawMenuCurrentBookTitle()
{
	const int width = renderer::screenTextWidth(top_scr) - 1;
	const int left = 12;
	const int right = width - 12;
	const bool portrait = width < 300;
	bool canResume = false;
	const string recentLabel = menuRecentLabel(canResume);

	if(!portrait) {
		const int textRight = menuHasArt() ? right - 148 : right - 12;
		const int titleBoxY1 = 108;
		const int titleBoxY2 = 144;
		renderer::fillRect(left + 12, titleBoxY1, textRight, titleBoxY2, Blend(12), top_scr);
		renderer::rect(left + 12, titleBoxY1, textRight, titleBoxY2, top_scr);
		if(canResume)
			return renderer::drawMarqueeText(top_scr, left + 12, titleBoxY1, textRight, titleBoxY2, recentLabel, 14, gMenuTitleTick, 8);
		drawCardValue(left + 16, 124, textRight - left - 12, recentLabel, 12, 4);
		return false;
	}

	const int titleBoxY1 = 98;
	const int titleBoxY2 = 132;
	renderer::fillRect(left + 12, titleBoxY1, right - 12, titleBoxY2, Blend(12), top_scr);
	renderer::rect(left + 12, titleBoxY1, right - 12, titleBoxY2, top_scr);
	if(canResume)
		return renderer::drawMarqueeText(top_scr, left + 12, titleBoxY1, right - 12, titleBoxY2, recentLabel, 11, gMenuTitleTick, 6);
	drawCardValue(left + 16, 112, right - left - 32, recentLabel, 10, 4);
	return false;
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
	const int cardX1 = 12;
	const int cardX2 = width - 12;
	const int headerY1 = 12;
	const int headerY2 = portrait ? 70 : 64;
	const int actionY1 = headerY2 + 12;
	const int rowHeight = portrait ? 40 : 34;
	const int gap = 8;
	const int listX1 = cardX1;
	const int listX2 = cardX2;
	int rowY = actionY1;
	const string lead = canResume
		? string("Resume or browse the library.")
		: string("Browse sdmc:/books/ and app books.");

	renderer::fillRect(cardX1, headerY1, cardX2, headerY2, Blend(22), bottom_scr);
	renderer::rect(cardX1, headerY1, cardX2, headerY2, bottom_scr);
	renderer::printStr(eUtf8, bottom_scr, cardX1 + 10, headerY1 + 18, "Home", 0, 0, 15);
	renderer::printStr(eUtf8, bottom_scr, cardX1 + 10, headerY1 + 36, lead, 0, 0, portrait ? 9 : 10);

	if(canResume) {
		button resume("Resume", listX1, rowY, listX2, rowY + rowHeight, 14);
		resume.enableAutoFit(10);
		gMenuButtons.push_back(resume);
		gMenuActions.push_back(SAY(resume));
		rowY += rowHeight + gap;
	}

	button files("Files", listX1, rowY, listX2, rowY + rowHeight, 14);
	files.enableAutoFit(10);
	gMenuButtons.push_back(files);
	gMenuActions.push_back(SAY(files));

	if(gMenuButtons.empty()) gMenuCursor = 0;
	else clamp(gMenuCursor, 0, int(gMenuButtons.size()) - 1);

	if(!gMenuButtons.empty()) {
		const int highlightY = actionY1 + gMenuCursor * (rowHeight + gap);
		renderer::fillRect(listX1 + 1, highlightY + 1, listX2 - 1, highlightY + rowHeight - 1, Blend(72), bottom_scr);
	}

	for(u32 i = 0; i < gMenuButtons.size(); ++i)
		gMenuButtons[i].draw();

	const int footerY1 = rowY + rowHeight + 10;
	const int footerY2 = height - 12;
	if(footerY2 > footerY1) {
		renderer::fillRect(cardX1, footerY1, cardX2, footerY2, Blend(14), bottom_scr);
		renderer::rect(cardX1, footerY1, cardX2, footerY2, bottom_scr);
		renderer::printStr(eUtf8, bottom_scr, cardX1 + 10, footerY1 + 16, "A/Right opens. B/Left backs out.", 0, 0, portrait ? 8 : 9);
		renderer::printStr(eUtf8, bottom_scr, cardX1 + 10, footerY1 + 30, "Tap a button or press Start to exit.", 0, 0, portrait ? 8 : 9);
	}
}

void drawMenuTopScreen()
{
	const int width = renderer::screenTextWidth(top_scr) - 1;
	const int height = renderer::screenTextHeight(top_scr) - 1;
	const int left = 12;
	const int right = width - 12;
	const bool portrait = width < 300;
	gMenuNeedsTitleMarquee = false;

	renderer::clearScreens(settings::bgCol, top_scr);
	renderer::fillRect(0, 0, width, portrait ? 42 : 50, Blend(28), top_scr);
	renderer::fillRect(0, portrait ? 42 : 50, width, portrait ? 44 : 52, Blend(72), top_scr);
	renderer::printStr(eUtf8, top_scr, 16, portrait ? 22 : 24, kMenuTitle, 0, 0, portrait ? 22 : 25);
	renderer::printStr(eUtf8, top_scr, 18, portrait ? 38 : 42, kMenuSubtitle, 0, 0, 11);

	if(!portrait) {
		renderer::fillRect(left, 70, right, height - 16, Blend(18), top_scr);
		renderer::rect(left, 70, right, height - 16, top_scr);
		drawMenuArt(right - 134, 78, right - 18, height - 24);
		drawCardTitle(left + 12, 92, "Current Book");
		gMenuNeedsTitleMarquee = drawMenuCurrentBookTitle();
	}
	else {
		renderer::fillRect(left, 60, right, height - 18, Blend(18), top_scr);
		renderer::rect(left, 60, right, height - 18, top_scr);
		drawCardTitle(left + 12, 82, "Current Book");
		gMenuNeedsTitleMarquee = drawMenuCurrentBookTitle();
		drawMenuArt(left + 16, 146, right - 16, height - 30);
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
	gMenuTitleTick = 0;
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
	initPowerManagement();
	applyBrightness();
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
		if(gMenuNeedsTitleMarquee) {
			++gMenuTitleTick;
			gMenuNeedsTitleMarquee = drawMenuCurrentBookTitle();
		}
		scanKeys();
		const int down = keysDown();
		if(down & KEY_START) break;
		if(down & rKey(rUp)) {
			if(gMenuCursor > 0) {
				--gMenuCursor;
				drawMenuButtonStrip();
			}
			continue;
		}
		if(down & rKey(rDown)) {
			if(gMenuCursor + 1 < int(gMenuButtons.size())) {
				++gMenuCursor;
				drawMenuButtonStrip();
			}
			continue;
		}
		if(down & rKey(rRight)) {
			if(!gMenuButtons.empty()) {
				const string* t = gMenuActions[gMenuCursor];
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
				if(appShouldExit()) break;
			}
			continue;
		}
		if(!(down & KEY_TOUCH)) continue;
		for(u32 i = 0; i < gMenuButtons.size(); ++i) {
			if(!gMenuButtons[i].touched()) continue;
			gMenuCursor = i;
			drawMenuButtonStrip();
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
