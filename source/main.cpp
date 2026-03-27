#include "renderer.h"
#include "epub.h"
#include "settings.h"
#include "button.h"
#include "file_browser.h"
#include "utf8.h"

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

void waitForInputRelease()
{
	while(pumpPowerManagement()) {
		swiWaitForVBlank();
		scanKeys();
		if(0 == keysHeld()) return;
	}
}

int statusTop()
{
	return renderer::screenTextHeight(top_scr) - buttonFontSize * 3 / 2;
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

string compactValue(const string& text, int width, u32 fontSize)
{
	if(text.empty()) return string("None");
	if(renderer::strWidth(eUtf8, text, 0, 0, fontSize) <= width) return text;
	return ellipsizedSlice(text, 0, width, fontSize);
}

string themeLabel()
{
	if(settings::lowLightMode()) return string("Low-light");
	return settings::nightMode() ? string("Night") : string("Paper");
}

string screenModeLabel()
{
	switch(settings::scrConf) {
		case scTop: return string("Top screen");
		case scBottom: return string("Bottom screen");
		default: return string("Dual screen");
	}
}

string layoutLabel()
{
	switch(gReadingLayout) {
		case d90: return string("Rotated 90");
		case d180: return string("Rotated 180");
		case d270: return string("Rotated 270");
		default: return string("Standard");
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
	const int buttonWidth = portrait ? width - 22 : MIN(width - 28, 220);
	const int buttonHeight = portrait ? 56 : 48;
	const int gap = 10;
	const int count = book_ok(settings::recent_book) ? 3 : 2;
	const int left = (width - buttonWidth) / 2;
	const int totalHeight = count * buttonHeight + (count - 1) * gap;
	int top = portrait ? height - totalHeight - 14 : MAX(18, (height - totalHeight) / 2);

	if(book_ok(settings::recent_book)) {
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
	top += buttonHeight + gap;

	button light("Backlight", left, top, left + buttonWidth, top + buttonHeight, 16);
	light.enableAutoFit(12);
	gMenuButtons.push_back(light);
	gMenuActions.push_back(SAY(light));

	for(u32 i = 0; i < gMenuButtons.size(); ++i)
		gMenuButtons[i].draw();
}

void drawMenuTopScreen()
{
	const int width = renderer::screenTextWidth(top_scr) - 1;
	const int height = renderer::screenTextHeight(top_scr) - 1;
	const int clockTop = statusTop();
	const int left = 12;
	const int right = width - 12;
	const bool portrait = width < 300;
	const bool canResume = !settings::recent_book.empty() && book_ok(settings::recent_book);

	renderer::clearScreens(settings::bgCol, top_scr);
	renderer::fillRect(0, 0, width, 50, Blend(28), top_scr);
	renderer::fillRect(0, 50, width, 52, Blend(72), top_scr);
	renderer::printStr(eUtf8, top_scr, 16, 24, kMenuTitle, 0, 0, 25);
	renderer::printStr(eUtf8, top_scr, 18, 42, kMenuSubtitle, 0, 0, 11);

	if(!portrait) {
		const int gap = 10;
		const int heroX2 = 230;
		const int infoX1 = heroX2 + gap;
		const int infoX2 = right;
		renderer::fillRect(left, 64, heroX2, 122, Blend(18), top_scr);
		renderer::rect(left, 64, heroX2, 122, top_scr);
		drawCardTitle(left + 10, 80, canResume ? "Recent" : "Start");
		drawCardValue(left + 10, 100, heroX2 - left - 20, canResume
			? noPath(settings::recent_book)
			: string("Use Files to browse sdmc:/books/ or the app folder."), 12, 2);

		renderer::rect(infoX1, 64, infoX2, 92, top_scr);
		drawCardTitle(infoX1 + 8, 80, "Library");
		renderer::printStr(eUtf8, top_scr, infoX1 + 8, 90, compactValue("sdmc:/books/", infoX2 - infoX1 - 16, 10), 0, 0, 10);

		renderer::rect(infoX1, 100, infoX2, 122, top_scr);
		renderer::printStr(eUtf8, top_scr, infoX1 + 8, 116, compactValue(themeLabel() + " | " + layoutLabel(), infoX2 - infoX1 - 16, 10), 0, 0, 10);

		renderer::rect(left, 132, right, 170, top_scr);
		drawCardTitle(left + 10, 148, "Mode");
		renderer::printStr(eUtf8, top_scr, left + 10, 164, compactValue(screenModeLabel(), right - left - 20, 11), 0, 0, 11);

		renderer::fillRect(left, clockTop - 28, right, clockTop - 4, Blend(28), top_scr);
		renderer::rect(left, clockTop - 28, right, clockTop - 4, top_scr);
		renderer::printStr(eUtf8, top_scr, left + 8, clockTop - 12,
			canResume ? "Tap Resume or Files below. Start exits." : "Tap Files below to open the browser. Start exits.",
			0, 0, 10);
	}
	else {
		int cardY = 64;
		const int cardH = 54;
		renderer::fillRect(left, cardY, right, cardY + cardH, Blend(18), top_scr);
		renderer::rect(left, cardY, right, cardY + cardH, top_scr);
		drawCardTitle(left + 10, cardY + 16, canResume ? "Recent Book" : "Start Reading");
		drawCardValue(left + 10, cardY + 36, right - left - 20, canResume
			? noPath(settings::recent_book)
			: string("Open Files to browse your EPUB library."), 11, 2);
		cardY += cardH + 10;

		renderer::rect(left, cardY, right, cardY + 40, top_scr);
		drawCardTitle(left + 10, cardY + 16, "Library");
		renderer::printStr(eUtf8, top_scr, left + 10, cardY + 34, compactValue("sdmc:/books/", right - left - 20, 10), 0, 0, 10);
		cardY += 50;

		renderer::rect(left, cardY, right, cardY + 40, top_scr);
		drawCardTitle(left + 10, cardY + 16, "Reader");
		renderer::printStr(eUtf8, top_scr, left + 10, cardY + 34, compactValue(screenModeLabel() + " | " + themeLabel(), right - left - 20, 10), 0, 0, 10);
		cardY += 50;

		renderer::rect(left, cardY, right, MIN(height - 40, cardY + 48), top_scr);
		drawCardTitle(left + 10, cardY + 16, "Layout");
		renderer::printStr(eUtf8, top_scr, left + 10, cardY + 34, compactValue(layoutLabel(), right - left - 20, 10), 0, 0, 10);
	}
	renderer::printClock(top_scr, true);
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
			else if(SAY(light) == t) cycleBacklight();
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
