#include "default.h"
#include "platform.h"
#include "renderer.h"
#include "settings.h"

#include <3ds/services/gsplcd.h>
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
bool gBacklightReady = false;
u32 gCurrentBacklightMask = GSPLCD_SCREEN_BOTH;
backlightMode gCurrentBacklightMode = blBoth;
bool gAppSuspended = false;
bool gAptHookInstalled = false;
aptHookCookie gAptHookCookie;

void ensureDir(const string& path)
{
	mkdir(path.c_str(), 0777);
}

u32 brightnessValue()
{
	static const u32 kBrightnessLevels[] = {25u, 45u, 70u, 100u};
	int level = settings::brightness;
	clamp(level, 0, 3);
	return kBrightnessLevels[level];
}

u32 targetBacklightMask(backlightMode mode)
{
	if(mode != blReading) return GSPLCD_SCREEN_BOTH;

	switch(settings::scrConf) {
		case scTop:
			return GSPLCD_SCREEN_TOP;
		case scBottom:
			return GSPLCD_SCREEN_BOTTOM;
		case scBoth:
		default:
			return GSPLCD_SCREEN_BOTH;
	}
}

void setScreenBacklight(u32 screen, bool enabled)
{
	if(enabled) {
		GSPLCD_PowerOnBacklight(screen);
		return;
	}
	GSPLCD_PowerOffBacklight(screen);
}

void aptStatusHook(APT_HookType hook, void* param)
{
	(void)param;
	switch(hook) {
		case APTHOOK_ONSUSPEND:
		case APTHOOK_ONSLEEP:
			gAppSuspended = true;
			break;
		case APTHOOK_ONRESTORE:
		case APTHOOK_ONWAKEUP:
			gAppSuspended = false;
			if(gBacklightReady) {
				const u32 mask = targetBacklightMask(gCurrentBacklightMode);
				setScreenBacklight(GSPLCD_SCREEN_TOP, 0 != (mask & GSPLCD_SCREEN_TOP));
				setScreenBacklight(GSPLCD_SCREEN_BOTTOM, 0 != (mask & GSPLCD_SCREEN_BOTTOM));
				gCurrentBacklightMask = mask;
				GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTH, brightnessValue());
			}
			renderer::markDirty();
			gKeysDown = gKeysHeld = gKeysUp = 0;
			break;
		case APTHOOK_ONEXIT:
			gShouldExit = true;
			break;
		default:
			break;
	}
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
	if(!gAptHookInstalled) {
		aptHook(&gAptHookCookie, aptStatusHook, NULL);
		gAptHookInstalled = true;
	}
	if(!gBacklightReady && R_SUCCEEDED(gspLcdInit())) {
		gBacklightReady = true;
		gCurrentBacklightMask = GSPLCD_SCREEN_BOTH;
		gCurrentBacklightMode = blBoth;
		GSPLCD_PowerOnAllBacklights();
	}
}

bool pumpPowerManagement()
{
	if(gShouldExit) return false;
	if(!aptMainLoop()) {
		gShouldExit = true;
		return false;
	}
	while(!gShouldExit && (gAppSuspended || !aptIsActive())) {
		gspWaitForVBlank();
		if(!aptMainLoop()) {
			gShouldExit = true;
			return false;
		}
	}
	return true;
}

bool appShouldExit()
{
	return gShouldExit;
}

void setBacklightMode(backlightMode mode)
{
	if(!gBacklightReady) return;
	gCurrentBacklightMode = mode;

	const u32 mask = targetBacklightMask(mode);
	if(mask == gCurrentBacklightMask) return;

	setScreenBacklight(GSPLCD_SCREEN_TOP, 0 != (mask & GSPLCD_SCREEN_TOP));
	setScreenBacklight(GSPLCD_SCREEN_BOTTOM, 0 != (mask & GSPLCD_SCREEN_BOTTOM));
	gCurrentBacklightMask = mask;
	GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTH, brightnessValue());
}

void applyBrightness()
{
	if(!gBacklightReady) return;
	GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTH, brightnessValue());
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
