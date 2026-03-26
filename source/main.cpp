#include "renderer.h"
#include "epub.h"
#include "settings.h"
#include "button.h"
#include "file_browser.h"

#include <dirent.h>
#include <fstream>

namespace
{

const string kMenuTitle("Rena");
const string kMenuSubtitle("EPUB Reader for 3DS");

void drawMenuInfoLine(int y, const string& label, const string& value, u32 valueFont = 12)
{
	renderer::printStr(eUtf8, top_scr, 18, y, label, 0, 0, 11);
	renderer::printStr(eUtf8, top_scr, 18, y + 15, value, 0, 0, valueFont);
}

void drawMenuTopScreen()
{
	const int width = screens::layoutX();
	const int titleWidth = renderer::strWidth(eUtf8, kMenuTitle, 0, 0, 28);
	const int subtitleWidth = renderer::strWidth(eUtf8, kMenuSubtitle, 0, 0, 13);
	const int titleX = MAX(12, (width - titleWidth) / 2);
	const int subtitleX = MAX(12, (width - subtitleWidth) / 2);

	renderer::printStr(eUtf8, top_scr, titleX, 34, kMenuTitle, 0, 0, 28);
	renderer::fillRect(MAX(14, titleX - 6), 42, MIN(width - 14, titleX + titleWidth + 6), 44, Blend(112), top_scr);
	renderer::printStr(eUtf8, top_scr, subtitleX, 64, kMenuSubtitle, 0, 0, 13);

	renderer::rect(14, 82, width - 14, 214, top_scr);
	drawMenuInfoLine(94, "Books", "sdmc:/books/ or " + appBooksPath(), 10);
	drawMenuInfoLine(132, "Data", appDataPath(), 10);
	drawMenuInfoLine(170, "Recent", settings::recent_book.empty() ? "None" : noPath(settings::recent_book), 12);
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

void drawMenu()
{
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
	while(pumpPowerManagement()) {
		const string file = browseForBook();
		if(file.empty()) {
			if(appShouldExit()) return;
			drawMenu();
			return;
		}
		openBook(file);
		if(appShouldExit()) return;
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
	applyBrightness();
	initPowerManagement();
	renderer::initFonts();
	string trans = transPath + settings::translname;
	if(file_ok(trans)) loadTrans(trans);

	if(book_ok(argfile)) openBook(argfile);
	if(appShouldExit()) {
		renderer::shutdownVideo();
		return 0;
	}
	if(!book_ok(settings::recent_book)) browseLoop();
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
		if(!(down & KEY_TOUCH)) continue;
		const string* t = menu.update();
		if(SAY(files) == t) browseLoop();
		else if(SAY(light) == t) cycleBacklight();
		else if(SAY(resume) == t && book_ok(settings::recent_book)) {
			openBook(settings::recent_book);
			drawMenu();
		}
	}

	renderer::shutdownVideo();
	return 0;
}
