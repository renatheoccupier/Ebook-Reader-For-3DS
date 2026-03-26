#include "default.h"
#include "button.h"

enum entity {file, folder};
typedef std::pair<entity, string> entry;
typedef std::pair<entity, button> fbutton;

struct file_browser
{
	string run();
private:
	void cd(), upd();
	u16 draw();
	void resetPreview();
	void showPreview(const string& file_name);
	void drawPreview();
	void drawPrompt();
	void clampCursor();
	bool tickPreview();
	void syncPreviewToCursor(bool force = false);
	void activateCursor();
	
	vector<entry> flist;
	vector<fbutton> buttons;
	vector<u16> previewPixels;
	string path;
	string previewFile;
	u16 previewWidth, previewHeight;
	bool previewHasImage, previewPending;
	int previewDelayFrames;
	bool promptActive;
	button promptOpen, promptKeep;
	scrollbar sbar;
	int pos;
	int cursor;
	u16 num;
}; 
