#pragma once
#include "default.h"
#include "button.h"
#include <set>
#include "base64.h"

struct Word	{int start, end, width; bool hyphen;};

struct marked {int start, end; fontStyle style;};

struct Line
{
	u16 width;
	vector<Word> words;
	Line() : width(0) {}
};

enum parType{pnormal, ptitle, pstanza, pimage};

struct paragrath
{
	parType type;
	string str;
	string image_ref;
	u16 image_width, image_height;
	vector<u16> image_pixels;
	vector<Line> lines;
	vector<marked> marks;
	paragrath() : type(pnormal), image_width(0), image_height(0) {}
};

struct bookmark
{
	u32 parag_num, line_num;
	bookmark(u32 p, u32 l) : parag_num(p), line_num(l) {}
	bookmark(){}
	bool operator<(const bookmark& b) const
	{ return parag_num < b.parag_num; }
};

struct toc_entry
{
	bookmark place;
	string title;
	toc_entry() {}
	toc_entry(u32 p, const string& t) : place(p, 0), title(t) {}
	toc_entry(const bookmark& b, const string& t) : place(b), title(t) {}
};

class Book
{
public:
	void read();
	Book(const string& filename) : bookFile(filename), paragraphCacheStamp(1), menuGrid(0) { 
		current_page.parag_num = 0;
		current_page.line_num = 0;
		prev_par_num = 1 << 30;
		string name = noPath(bookFile);
		encname = appBookmarksPath() + base64_encode(reinterpret_cast<const unsigned char*>(name.c_str()), name.length()) + ".bm";
		tocReady = false;
		bookmarkView = bookmarkViewMarks;
		bookmarkScroll = 0;
		tocScroll = 0;
		bookmarkVisible = 0;
		bookmarkCursor = 0;
		marksDirty = false;
		marksSaveFrames = 0;
		paragraphCache.resize(6);
	}
protected:
	string bookFile, encname;
	bookmark current_page;
	std::set<bookmark> bookmarks;
	vector<toc_entry> tocEntries;
	bool tocReady;
	virtual void parse() = 0;
	virtual void parag_str (int parag_num) = 0;
	void fetch_paragrath (int parag_num) {
		if(tryLoadCachedParagraph(parag_num)) return;
		parag_str (parag_num);
		parsePar();
		prev_par_num = parag_num;
		storeCachedParagraph(parag_num);
	}
	u32 prev_par_num;
	void parsePar();
	bool hyphenate(Word* word_it, int& space_left, fontStyle style);
	
	virtual u32 total_paragraths() = 0;
	paragrath parag;
	Encoding encoding;
private:
	struct ParagraphCacheEntry
	{
		bool valid;
		u32 parag_num;
		u32 stamp;
		paragrath value;
		ParagraphCacheEntry() : valid(false), parag_num(0), stamp(0) {}
	};

	void clearParagraphCache();
	bool tryLoadCachedParagraph(u32 parag_num);
	void storeCachedParagraph(u32 parag_num);
	enum bookmark_view {bookmarkViewMarks, bookmarkViewContents};
	void next_page(), previous_page(), draw_page(bool onlyTop = false, bool cachePar = true);
	void next_line(), previous_line();
	
	void ensureToc();
	void buildFallbackToc();
	string paragraphMenuLabel(u32 parag_num);
	u32 bookmarkTotalItems() const;
	u32* activeBookmarkScroll();
	void clampBookmarkCursor();
	void moveBookmarkCursor(int delta);
	bool activateBookmarkCursor();
	void queueMarksSave();
	void flushMarks();
	void tickMarksSave();
	void bookmarkMenu();
	void drawBookmarkMenu();
	button setMark, older, newer, tabMarks, tabContents, listUp, listDown;
	progressbar prbar;
	vector<button> bookmarkRows;
	vector<bookmark> bookmarkTargets;
	bookmark_view bookmarkView;
	u32 bookmarkScroll, tocScroll, bookmarkVisible, bookmarkCursor;
	bool marksDirty;
	int marksSaveFrames;
	vector<ParagraphCacheEntry> paragraphCache;
	u32 paragraphCacheStamp;
	bool moreNew() {return !bookmarks.empty() && current_page < *bookmarks.rbegin();}
	bool moreOld() {return !bookmarks.empty() && *bookmarks.begin()  < current_page;}
	
	bool menu();
	void drawMenu(bool recache = true);
	grid menuGrid;
	bool otherGrid;
	void colorPicker(), sharpness(), search();
	
	void loadMarks(), saveMarks(), translation();
};
