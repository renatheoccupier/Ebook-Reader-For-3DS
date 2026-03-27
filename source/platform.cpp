#include "default.h"
#include "platform.h"
#include "renderer.h"
#include "settings.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/types.h>

namespace
{

u32 gKeysDown = 0;
u32 gKeysHeld = 0;
u32 gKeysUp = 0;
bool gShouldExit = false;

void ensureDir(const string& path)
{
	mkdir(path.c_str(), 0777);
}

} // namespace

extern "C" {
	u32 __stacksize__ = 1024 * 1024;
	u32 __ctru_heap_size = 40 * 1024 * 1024;
	u32 __ctru_linear_heap_size = 4 * 1024 * 1024;
}

const string& sdRootPath()
{
	static const string kPath("sdmc:/");
	return kPath;
}

const string& appRootPath()
{
	static const string kPath("sdmc:/3ds/EBookReaderFor3DS");
	return kPath;
}

const string& appDataPath()
{
	static const string kPath(appRootPath() + "/data");
	return kPath;
}

const string& appFontsPath()
{
	static const string kPath(appDataPath() + "/fonts/");
	return kPath;
}

const string& appTranslationsPath()
{
	static const string kPath(appDataPath() + "/translations/");
	return kPath;
}

const string& appEncodingsPath()
{
	static const string kPath(appDataPath() + "/encodings/");
	return kPath;
}

const string& appBookmarksPath()
{
	static const string kPath(appDataPath() + "/bookmarks/");
	return kPath;
}

const string& appBooksPath()
{
	static const string kPath(appRootPath() + "/books/");
	return kPath;
}

void ensureRuntimeDirectories()
{
	ensureDir(appRootPath());
	ensureDir(appDataPath());
	ensureDir(appFontsPath());
	ensureDir(appTranslationsPath());
	ensureDir(appEncodingsPath());
	ensureDir(appBookmarksPath());
	ensureDir(appBooksPath());
}

void scanKeys()
{
	hidScanInput();
	gKeysDown = hidKeysDown();
	gKeysHeld = hidKeysHeld();
	gKeysUp = hidKeysUp();
}

u32 keysDown()
{
	return gKeysDown;
}

u32 keysHeld()
{
	return gKeysHeld;
}

u32 keysUp()
{
	return gKeysUp;
}

void touchRead(touchPosition* touch)
{
	hidTouchRead(touch);
}

void swiWaitForVBlank()
{
	renderer::present();
}

int iprintf(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	const int written = vprintf(fmt, args);
	va_end(args);
	return written;
}

void initPowerManagement()
{
	bool isNew3DS = false;
	if(R_SUCCEEDED(APT_CheckNew3DS(&isNew3DS)) && isNew3DS)
		osSetSpeedupEnable(true);
}

bool pumpPowerManagement()
{
	if(gShouldExit) return false;
	if(!aptMainLoop()) {
		gShouldExit = true;
		return false;
	}
	return true;
}

bool appShouldExit()
{
	return gShouldExit;
}

void setBacklightMode(backlightMode mode)
{
	(void)mode;
}

void applyBrightness()
{
}

void cycleBacklight()
{
	int& b = settings::brightness;
	b = (b + 1) % 4;
	applyBrightness();
}

void bsod(const char* msg)
{
	fprintf(stderr, "%s\n", msg);
	if(!platform::isGraphicsReady())
		gfxInitDefault();
	consoleInit(GFX_BOTTOM, NULL);
	printf("%s\n", msg);
	while(aptMainLoop())
		gspWaitForVBlank();
	exit(EXIT_FAILURE);
}

namespace platform
{

bool isGraphicsReady()
{
	return true;
}

} // namespace platform
