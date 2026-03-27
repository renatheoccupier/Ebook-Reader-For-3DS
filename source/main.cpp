#include "renderer.h"
#include "epub.h"
#include "settings.h"
#include "button.h"
#include "file_browser.h"
#include "utf8.h"

#include <dirent.h>
#include <fstream>

namespace
{

const string kMenuTitle("EBook Reader");
const string kMenuSubtitle("Nintendo 3DS EPUB Library");

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
	return screens::layoutY() - buttonFontSize * 3 / 2;
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

void drawMenuTopScreen()
{
	const int width = renderer::screenTextWidth(top_scr) - 1;
	const int clockTop = statusTop();

	renderer::clearScreens(settings::bgCol, top_scr);
	renderer::fillRect(0, 0, width, 44, Blend(32), top_scr);
	renderer::printStr(eUtf8, top_scr, 12, 22, kMenuTitle, 0, 0, 24);
	renderer::printStr(eUtf8, top_scr, 14, 38, kMenuSubtitle, 0, 0, 11);

	const int cardGap = 8;
	const int left = 12;
	const int right = width - 12;
	const int topRowY1 = 54;
	const int topRowY2 = 128;
	const int mid = (left + right) / 2;

	renderer::rect(left, topRowY1, mid - cardGap / 2, topRowY2, top_scr);
	renderer::rect(mid + cardGap / 2, topRowY1, right, topRowY2, top_scr);
	renderer::rect(left, 138, right, clockTop - 8, top_scr);

	drawCardTitle(left + 8, topRowY1 + 14, "Recent");
	drawCardValue(left + 8, topRowY1 + 34, mid - left - 20, settings::recent_book.empty() ? "Open Files to pick a book." : noPath(settings::recent_book), 12, 3);

	drawCardTitle(mid + cardGap / 2 + 8, topRowY1 + 14, "Library");
	renderer::printStr(eUtf8, top_scr, mid + cardGap / 2 + 8, topRowY1 + 38, compactValue("sdmc:/books/", right - mid - cardGap / 2 - 20, 10), 0, 0, 10);
	renderer::printStr(eUtf8, top_scr, mid + cardGap / 2 + 8, topRowY1 + 52, compactValue(appBooksPath(), right - mid - cardGap / 2 - 20, 10), 0, 0, 10);

	drawCardTitle(left + 8, 152, "Controls");
	drawCardValue(left + 8, 172, right - left - 20, "Touch a tile or use A/Right to open files. Start exits the app. Reading uses both screens.", 10, 4);
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

static grid menu(0);
static Layout gReadingLayout = d0;

void drawMenu()
{
	renderer::setTopScreenMirror(false);
	menu = grid(0);
	if(book_ok(settings::recent_book)) menu.push(SAY(resume), 1);
	menu.push(SAY(files), 2);
	menu.push(SAY(light), 3);
	renderer::clearScreens(settings::bgCol);
	menu.draw();
	drawMenuTopScreen();
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
	settings::layout = d0;
	while(pumpPowerManagement()) {
		const string file = browseForBook();
		if(file.empty()) {
			if(appShouldExit()) return;
			settings::layout = d0;
			drawMenu();
			return;
		}
		waitForInputRelease();
		if(appShouldExit()) return;
		settings::layout = gReadingLayout;
		openBook(file);
		if(appShouldExit()) return;
		gReadingLayout = settings::layout;
		settings::layout = d0;
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
	settings::layout = d0;
	if(!book_ok(settings::recent_book)) browseLoop();
	if(appShouldExit()) {
		renderer::shutdownVideo();
		return 0;
	}

	settings::layout = d0;
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
		const string* t = menu.update();
		if(SAY(files) == t) browseLoop();
		else if(SAY(light) == t) cycleBacklight();
		else if(SAY(resume) == t && book_ok(settings::recent_book)) {
			waitForInputRelease();
			if(appShouldExit()) break;
			settings::layout = gReadingLayout;
			openBook(settings::recent_book);
			gReadingLayout = settings::layout;
			settings::layout = d0;
			drawMenu();
		}
	}

	renderer::shutdownVideo();
	return 0;
}
