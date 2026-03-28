#include "default.h"
#include "button.h"

enum entity {file, folder};
typedef std::pair<entity, string> entry;
typedef std::pair<entity, button> fbutton;

bool copyCachedPreview(const string& file_name, vector<u16>& pixels, u16& width, u16& height);

struct file_browser
{
	file_browser() : previewWidth(0), previewHeight(0), previewHasImage(false), previewPending(false), previewDelayFrames(0),
		promptActive(false), scrollbarFrames(0), previewAnimTick(0), pos(0), cursor(0), num(0) {}
	string run();
private:
	void cd(), upd(bool refreshScrollbar = true);
	u16 draw(bool showScrollbar = true);
	void resetPreview();
	void showPreview(const string& file_name);
	void drawPreview();
	void drawPrompt();
	void clampCursor();
	bool tickPreview();
	void syncPreviewToCursor(bool force = false);
	string activateCursor();
	
	vector<entry> flist;
	vector<fbutton> buttons;
	vector<u16> previewPixels;
	string path;
	string previewFile;
	u16 previewWidth, previewHeight;
	bool previewHasImage, previewPending;
	int previewDelayFrames;
	bool promptActive;
	int scrollbarFrames;
	u32 previewAnimTick;
	button promptOpen, promptKeep;
	scrollbar sbar;
	int pos;
	int cursor;
	u16 num;
}; 
