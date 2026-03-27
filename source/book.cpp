#include "book.h"
#include "screens.h"
#include "renderer.h"
#include "controls.h"
#include "utf8.h"
#include <fstream>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

namespace
{

const string kThemePaper("Paper");
const string kThemeNight("Night");
const int kTabY1 = 4;
const int kTabY2 = 24;
const int kActionY1 = 30;
const int kActionY2 = 50;
const int kListY1 = 56;
const int kArrowWidth = 22;
const int kBookmarkRowHeight = 24;
const int kBookmarkRowHeightRotated = 36;
const int kListTextPadX = 6;
const int kListTextPadY = 4;
const int kListLineGap = 2;
const int kListFooterGap = 4;
const int kBookmarkProgressWidth = 38;
const u32 kBookmarkListLines = 2;
const int kBookmarkProgressGap = 6;
const int kBookmarkProgressThickness = 12;
const int kDeferredMarksSaveFrames = 24;

bool bookmarkMenuRotated()
{
	return settings::layout == d90 || settings::layout == d270;
}

int bookmarkMenuRowHeight()
{
	return bookmarkMenuRotated() ? kBookmarkRowHeightRotated : kBookmarkRowHeight;
}

u32 bookmarkMenuFont()
{
	return bookmarkMenuRotated() ? 12u : 14u;
}

u32 bookmarkMenuListFont()
{
	return bookmarkMenuRotated() ? 11u : 12u;
}

int bookmarkStatusTop()
{
	return renderer::screenTextHeight(top_scr) - buttonFontSize * 3 / 2;
}

int bookmarkProgressBottom()
{
	return bookmarkStatusTop() - kBookmarkProgressGap;
}

int bookmarkProgressTop()
{
	return bookmarkProgressBottom() - kBookmarkProgressThickness;
}

int bookmarkListBottom()
{
	return bookmarkProgressTop() - kListFooterGap;
}

u32 bookmarkMenuRows()
{
	const int rowHeight = bookmarkMenuRowHeight();
	const int available = bookmarkListBottom() - kListY1 + 2;
	const int rows = available / rowHeight;
	return MAX(2, rows);
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

u32 skipSpaces(const string& text, u32 start)
{
	while(start < text.size() && text[start] == ' ') ++start;
	return start;
}

u32 trimSpaces(const string& text, u32 start, u32 end)
{
	while(end > start && text[end - 1] == ' ') --end;
	return end;
}

string ellipsizedSlice(const string& text, u32 start, int width, u32 fontSize)
{
	const u32 end = clippedUtf8End(text, start, width, fontSize);
	if(end >= text.size())
		return text.substr(start, trimSpaces(text, start, end) - start);

	const string ellipsis("...");
	const int ellipsisWidth = renderer::strWidth(eUtf8, ellipsis, 0, 0, fontSize);
	if(ellipsisWidth >= width)
		return text.substr(start, trimSpaces(text, start, end) - start);

	const u32 clipped = clippedUtf8End(text, start, width - ellipsisWidth, fontSize);
	const u32 lineEnd = trimSpaces(text, start, clipped);
	if(lineEnd <= start) return ellipsis;
	return text.substr(start, lineEnd - start) + ellipsis;
}

vector<string> wrapListText(const string& text, int width, u32 fontSize, u32 maxLines)
{
	vector<string> lines;
	if(text.empty() || width <= 0 || maxLines == 0) return lines;

	u32 start = skipSpaces(text, 0);
	while(start < text.size() && lines.size() + 1 < maxLines) {
		const u32 hardEnd = clippedUtf8End(text, start, width, fontSize);
		if(hardEnd >= text.size()) {
			lines.push_back(text.substr(start, trimSpaces(text, start, hardEnd) - start));
			return lines;
		}

		u32 breakPos = hardEnd;
		while(breakPos > start && text[breakPos - 1] != ' ') --breakPos;
		u32 lineEnd = hardEnd;
		u32 nextStart = hardEnd;
		if(breakPos > start) {
			lineEnd = trimSpaces(text, start, breakPos);
			nextStart = skipSpaces(text, breakPos);
		}
		if(lineEnd <= start) {
			lineEnd = hardEnd;
			nextStart = hardEnd;
		}
		lines.push_back(text.substr(start, lineEnd - start));
		start = skipSpaces(text, nextStart);
	}

	if(start < text.size()) lines.push_back(ellipsizedSlice(text, start, width, fontSize));
	return lines;
}

u32 wrappedListFont(const string& text, int width, int height, u32 maxFont, u32 minFont)
{
	for(int font = (int)maxFont; font >= (int)minFont; --font) {
		const vector<string> lines = wrapListText(text, width, font, kBookmarkListLines);
		const int lineCount = MAX(1, (int)lines.size());
		const int totalHeight = lineCount * font + (lineCount - 1) * kListLineGap;
		if(totalHeight <= height - 2 * kListTextPadY) return font;
	}
	return minFont;
}

void drawListTextBlock(int x1, int y1, int x2, int y2, const string& text, u32 maxFont, u32 minFont)
{
	const int width = x2 - x1 + 1;
	const int height = y2 - y1 + 1;
	if(width <= 0 || height <= 0 || text.empty()) return;

	const u32 font = wrappedListFont(text, width, height, maxFont, minFont);
	const vector<string> lines = wrapListText(text, width, font, kBookmarkListLines);
	if(lines.empty()) return;

	const int totalHeight = lines.size() * font + (int(lines.size()) - 1) * kListLineGap;
	const int top = y1 + MAX(kListTextPadY, (height - totalHeight) / 2);
	for(u32 i = 0; i < lines.size(); ++i) {
		const int baseline = top + font - 1 + i * (font + kListLineGap);
		renderer::printStr(eUtf8, bottom_scr, x1, baseline, lines[i], 0, 0, font);
	}
}

void drawBookmarkListRow(int x1, int y1, int x2, int y2, const string& label, u32 maxFont, u32 minFont)
{
	renderer::rect(x1, y1, x2, y2, bottom_scr);
	drawListTextBlock(x1 + kListTextPadX, y1, x2 - kListTextPadX, y2, label, maxFont, minFont);
}

void drawBookmarkMarkRow(int x1, int y1, int x2, int y2, const string& progress, const string& label, u32 maxFont, u32 minFont)
{
	renderer::rect(x1, y1, x2, y2, bottom_scr);

	const u32 progressFont = MIN(maxFont, 11u);
	const int dividerX = x1 + kBookmarkProgressWidth;
	const int progressWidth = renderer::strWidth(eUtf8, progress, 0, 0, progressFont);
	const int progressBaseline = y1 + progressFont - 1 + (y2 - y1 - progressFont) / 2;
	renderer::printStr(eUtf8, bottom_scr, dividerX - progressWidth - 4, progressBaseline, progress, 0, 0, progressFont);
	renderer::vLine(dividerX, y1 + 4, y2 - 4, Blend(48));

	drawListTextBlock(dividerX + 5, y1, x2 - kListTextPadX, y2, label, maxFont, minFont);
}

string collapseWhitespace(const string& text)
{
	string out;
	bool pending_space = false;
	for(u32 i = 0; i < text.size(); ++i) {
		const unsigned char c = (unsigned char)text[i];
		if(c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v') {
			if(!out.empty()) pending_space = true;
			continue;
		}
		if(pending_space) out += ' ';
		pending_space = false;
		out += text[i];
	}
	return out;
}

string progressLabel(u32 parag_num, u32 total)
{
	char buf[16];
	const u32 percent = (total > 1u) ? (100u * parag_num) / (total - 1u) : 0u;
	sprintf(buf, "%3lu%%", (unsigned long)percent);
	return buf;
}

string themeLabel()
{
	if(settings::lowLightMode()) return string("Low-light");
	return settings::nightMode() ? kThemeNight : kThemePaper;
}

string screenModeLabel()
{
	switch(settings::scrConf) {
		case scTop: return string("Top");
		case scBottom: return string("Bottom");
		default: return string("Dual");
	}
}

string layoutLabel()
{
	switch(settings::layout) {
		case d90: return string("90");
		case d180: return string("180");
		case d270: return string("270");
		default: return string("0");
	}
}

void drawOverlayTextBlock(int x, int y, int width, const string& text, u32 fontSize, u32 maxLines)
{
	const vector<string> lines = wrapListText(text, width, fontSize, maxLines);
	for(u32 i = 0; i < lines.size(); ++i) {
		const int baseline = y + fontSize - 1 + i * (fontSize + 2);
		renderer::printStr(eUtf8, top_scr, x, baseline, lines[i], 0, 0, fontSize);
	}
}

void drawTopContextOverlay(const string& bookTitle, const string& modeTitle, const string& detailText, const string& summaryText, const string& footerText)
{
	const int width = renderer::screenTextWidth(top_scr) - 1;
	const int clockTop = bookmarkStatusTop();
	const int left = 10;
	const int right = width - 10;
	const bool portrait = width < 300;
	const int headerY2 = 32;
	const int footerY1 = clockTop - 28;
	const int footerY2 = clockTop - 4;

	renderer::fillRect(0, 0, width, headerY2, Blend(28), top_scr);
	renderer::rect(left, 4, right, headerY2, top_scr);
	renderer::printStr(eUtf8, top_scr, left + 8, 18, ellipsizedSlice(bookTitle, 0, right - left - 96, 14), 0, 0, 14);
	renderer::printStr(eUtf8, top_scr, right - renderer::strWidth(eUtf8, modeTitle, 0, 0, 10) - 8, 18, modeTitle, 0, 0, 10);

	if(!portrait) {
		const int cardY1 = clockTop - 70;
		const int cardY2 = clockTop - 34;
		const int detailX2 = width - 118;
		const int summaryX1 = detailX2 + 8;
		renderer::fillRect(left, cardY1, detailX2, cardY2, Blend(18), top_scr);
		renderer::rect(left, cardY1, detailX2, cardY2, top_scr);
		renderer::fillRect(summaryX1, cardY1, right, cardY2, Blend(18), top_scr);
		renderer::rect(summaryX1, cardY1, right, cardY2, top_scr);
		drawOverlayTextBlock(left + 8, cardY1 + 8, detailX2 - left - 16, detailText, 10, 2);
		drawOverlayTextBlock(summaryX1 + 8, cardY1 + 8, right - summaryX1 - 16, summaryText, 10, 2);
	}
	else {
		const int detailY1 = clockTop - 104;
		const int detailY2 = detailY1 + 32;
		const int summaryY1 = detailY2 + 8;
		const int summaryY2 = summaryY1 + 24;
		renderer::fillRect(left, detailY1, right, detailY2, Blend(18), top_scr);
		renderer::rect(left, detailY1, right, detailY2, top_scr);
		renderer::fillRect(left, summaryY1, right, summaryY2, Blend(18), top_scr);
		renderer::rect(left, summaryY1, right, summaryY2, top_scr);
		drawOverlayTextBlock(left + 8, detailY1 + 6, right - left - 16, detailText, 10, 2);
		drawOverlayTextBlock(left + 8, summaryY1 + 5, right - left - 16, summaryText, 10, 1);
	}

	renderer::fillRect(left, footerY1, right, footerY2, Blend(28), top_scr);
	renderer::rect(left, footerY1, right, footerY2, top_scr);
	drawOverlayTextBlock(left + 8, footerY1 + 4, right - left - 16, footerText, 10, 1);
	renderer::printClock(top_scr, true);
}

int lineScrollForwardKey()
{
	switch(settings::layout) {
		case d90:
		case d180:
			return KEY_L;
		default:
			return KEY_R;
	}
}

int lineScrollBackwardKey()
{
	return (lineScrollForwardKey() == KEY_L) ? KEY_R : KEY_L;
}

}

void Book :: clearParagraphCache()
{
	for(u32 i = 0; i < paragraphCache.size(); ++i)
		paragraphCache[i].valid = false;
	prev_par_num = 1 << 30;
}

bool Book :: tryLoadCachedParagraph(u32 parag_num)
{
	for(u32 i = 0; i < paragraphCache.size(); ++i) {
		ParagraphCacheEntry& entry = paragraphCache[i];
		if(!entry.valid || entry.parag_num != parag_num) continue;
		parag = entry.value;
		refreshCachedParagraph(parag);
		entry.stamp = paragraphCacheStamp++;
		prev_par_num = parag_num;
		return true;
	}
	return false;
}

void Book :: storeCachedParagraph(u32 parag_num)
{
	if(paragraphCache.empty()) return;

	ParagraphCacheEntry* slot = &paragraphCache[0];
	for(u32 i = 0; i < paragraphCache.size(); ++i) {
		ParagraphCacheEntry& entry = paragraphCache[i];
		if(!entry.valid) {
			slot = &entry;
			break;
		}
		if(entry.stamp < slot->stamp) slot = &entry;
	}

	slot->valid = true;
	slot->parag_num = parag_num;
	slot->stamp = paragraphCacheStamp++;
	slot->value = parag;
	if(slot->value.type == pimage && !slot->value.image_ref.empty())
		vector<u16>().swap(slot->value.image_pixels);
}

void Book :: queueMarksSave()
{
	marksDirty = true;
	marksSaveFrames = kDeferredMarksSaveFrames;
}

void Book :: flushMarks()
{
	if(!marksDirty) return;
	saveMarks();
	marksDirty = false;
	marksSaveFrames = 0;
}

void Book :: tickMarksSave()
{
	if(!marksDirty) return;
	if(marksSaveFrames > 0) {
		--marksSaveFrames;
		return;
	}
	flushMarks();
}

void Book :: read()
{
	renderer::clearScreens(settings::bgCol);
	parse();
	if(0 == total_paragraths()) return;
	clearParagraphCache();
	loadMarks();
	consoleClear();
	draw_page();
	if(settings::recent_book != bookFile) {
		settings::recent_book = bookFile;
		settings::save();
	}
	otherGrid = false;

	touchPosition t1, t2;
	t1.px = t1.py = t2.px = t2.py = 0;
	
	while(pumpPowerManagement()) {
		renderer::flashClock.hide();
		swiWaitForVBlank();
		scanKeys();
		int down = keysDown();
		if(down & lineScrollForwardKey()) next_line();
		else if(down & lineScrollBackwardKey()) previous_line();
		else if(down & (rKey(rRight) & ~(KEY_L | KEY_R))) next_page();
		else if(down & (rKey(rLeft) & ~(KEY_L | KEY_R))) previous_page();
		else if(down & rKey(rUp)) {
			bookmarkMenu();
			if(appShouldExit()) {
				flushMarks();
				return;
			}
			draw_page(false, false);
		}
		else if(down & rKey(rDown)) {
			bool doexit = menu();
			if(doexit) {
				flushMarks();
				return;
			}
			otherGrid = false;
			draw_page(false, false);
		}
		else if(down & KEY_SELECT) {
			search();
			if(appShouldExit()) {
				flushMarks();
				return;
			}
		}
		else if(down & KEY_TOUCH) {
			touchRead(&t1);
		}
		else if(keysHeld() & KEY_TOUCH) {
			touchRead(&t2);
		}
		else if(keysUp() & KEY_TOUCH) {
			if(10 > abs(t1.px - t2.px) + abs(t1.py - t2.py)) {
				if(t2.px == 0) continue;
				int X1 = 0, Y1 = 0, X2 = screens::layoutX()/2, Y2 = screens::layoutY();
				toLayoutSpace(X1, Y1);
				toLayoutSpace(X2, Y2);
				if(	!(t2.px >= MIN(X1, X2) && t2.py >= MIN(Y1, Y2) &&
					t2.px <  MAX(X1, X2) && t2.py < MAX(Y1, Y2)) ) 
						 next_page();
					else previous_page();
			}
			else {
				int deltaX = t1.px - t2.px;
				int deltaY = t1.py - t2.py;
				int tehDelta = 0;
				switch (settings::layout) {
					case d0:   tehDelta = deltaX;  break;
					case d90:  tehDelta = deltaY;  break;
					case d180: tehDelta = -deltaX; break;
					case d270: tehDelta = -deltaY; break;
				}
				if(tehDelta > 30) next_page();
				else if (tehDelta < -30) previous_page();
			}
		}
		tickMarksSave();
	}
	flushMarks();
}

void Book :: next_page()
{
	int i = screens::capacity(settings::scrConf != scBoth) + current_page.line_num;
	for (u16 j = current_page.parag_num; j < total_paragraths(); j++) {
		if(j != prev_par_num) fetch_paragrath(j);
		i -= parag.lines.size();
		if (j >= total_paragraths()-1 && i > 0) return; //last page, don't redraw
		if (i <= 0) {
			current_page.parag_num = j;
			current_page.line_num = i + parag.lines.size();
			break;
		}
	}
	draw_page(false, false);
	queueMarksSave();
}

void Book :: previous_page()
{
	if (0 == current_page.parag_num && 0 == current_page.line_num) return; //first page, don't redraw
	if(current_page.parag_num != prev_par_num) fetch_paragrath (current_page.parag_num);
	int i = screens::capacity(settings::scrConf != scBoth) + parag.lines.size() - current_page.line_num;
	for (u32 j = current_page.parag_num; j >= 0; j--) {
		if(j != prev_par_num) fetch_paragrath(j);
		i -= parag.lines.size();
		if (i <= 0) {
			current_page.parag_num = j;
			current_page.line_num = -i;
			break;
		}
		if (0 == j){
			current_page.parag_num = 0;
			current_page.line_num = 0;
			break;
		}
	}
	draw_page(false, false);
	queueMarksSave();
}

void Book :: next_line()
{
	if(current_page.parag_num != prev_par_num) fetch_paragrath(current_page.parag_num);
	if(current_page.line_num + 1u < parag.lines.size()) {
		++current_page.line_num;
		draw_page(false, false);
		queueMarksSave();
		return;
	}

	for(u32 j = current_page.parag_num + 1u; j < total_paragraths(); ++j) {
		if(j != prev_par_num) fetch_paragrath(j);
		if(parag.lines.empty()) continue;
		current_page.parag_num = j;
		current_page.line_num = 0;
		draw_page(false, false);
		queueMarksSave();
		return;
	}
}

void Book :: previous_line()
{
	if(0 == current_page.parag_num && 0 == current_page.line_num) return;
	if(current_page.parag_num != prev_par_num) fetch_paragrath(current_page.parag_num);
	if(current_page.line_num > 0) {
		--current_page.line_num;
		draw_page(false, false);
		queueMarksSave();
		return;
	}

	for(int j = (int)current_page.parag_num - 1; j >= 0; --j) {
		if((u32)j != prev_par_num) fetch_paragrath(j);
		if(parag.lines.empty()) continue;
		current_page.parag_num = j;
		current_page.line_num = parag.lines.size() - 1u;
		draw_page(false, false);
		queueMarksSave();
		return;
	}
}

void Book :: draw_page(bool onlyTop, bool cachePar)
{
	renderer::setTopScreenMirror(false);
	const backlightMode mode = (onlyTop && settings::scrConf == scTop) ? blReading
		: (onlyTop ? blBoth : blReading);
	setBacklightMode(mode);
	
	int line_num = current_page.line_num;
	u32 lines_total = onlyTop ? screens::screen_capacity(top_scr)
		: screens::capacity(settings::scrConf != scBoth);
	u32 k = 0;
	for (u16 i = current_page.parag_num; k < lines_total && i < total_paragraths(); i++)
	{
		if(i != prev_par_num || cachePar) fetch_paragrath(i);
		for (u16 j = line_num; j < parag.lines.size() && k < lines_total; j++,k++)
			screens::print_line (encoding, parag, j, k, onlyTop);
		line_num = 0;
	}
	if(k == 0) renderer::clearScreens(settings::bgCol);
	if(!onlyTop) {
		if(settings::scrConf != scBoth)
			renderer::clearScreens(0, (scr_id)!first_scr());
		else if(k <= screens::first_screen_capacity())
			renderer::clearScreens(settings::bgCol, (scr_id)!first_scr());
	}
	
	if(settings::pbar) {
		u16 fcol = settings::fCol, gray = Blend(64);
		scr_id scr = (scr_id)(settings::scrConf == scBoth ? !first_scr(): first_scr());
		if(onlyTop) scr = top_scr;
		const u16 xw = renderer::screenTextWidth(scr);
		u32 upto = current_page.parag_num * xw / total_paragraths();
		const u32 screenBottom = screens::screen_text_height(scr) - 1u;
		for(u32 i = 0, y = screenBottom - 1u, y2 = screenBottom; i < xw; i++) {
			renderer::putPixel(scr, i, y, (i >= upto ? gray : fcol));
			renderer::putPixel(scr, i, y2, (i >= upto ? gray : settings::bgCol));
		}
		if(upto > 0) renderer::putPixel(scr, upto - 1, screenBottom, fcol);
	}
	if(!onlyTop) {
		if(settings::scrConf != scBoth) renderer::flashClock.show(first_scr());
		else renderer::flashClock.show((scr_id)!first_scr());
	}
}

void Book :: ensureToc()
{
	if(tocReady) return;
	if(loadTocEntries()) {
		tocReady = true;
		return;
	}
	buildFallbackToc();
}

void Book :: buildFallbackToc()
{
	tocEntries.clear();
	const u32 total = total_paragraths();
	for(u32 i = 0; i < total; ++i) {
		if(paragraphType(i) != ptitle) continue;
		if(i != prev_par_num) fetch_paragrath(i);
		const string title = collapseWhitespace(parag.str);
		if(title.empty()) continue;
		if(!tocEntries.empty() && tocEntries.back().place.parag_num == i) continue;
		tocEntries.push_back(toc_entry(i, title));
	}
	tocReady = true;
}

string Book :: paragraphMenuLabel(u32 parag_num)
{
	if(parag_num >= total_paragraths()) return string();
	if(parag_num != prev_par_num) fetch_paragrath(parag_num);
	if(parag.type == pimage) return "[Image]";

	string label = collapseWhitespace(parag.str);
	if(!label.empty()) return label;

	char buf[20];
	sprintf(buf, "#%lu", (unsigned long)(parag_num + 1u));
	return buf;
}

u32 Book :: bookmarkTotalItems() const
{
	return (bookmarkView == bookmarkViewContents) ? tocEntries.size() : bookmarks.size();
}

u32* Book :: activeBookmarkScroll()
{
	return (bookmarkView == bookmarkViewContents) ? &tocScroll : &bookmarkScroll;
}

void Book :: clampBookmarkCursor()
{
	u32* activeScroll = activeBookmarkScroll();
	const u32 totalItems = bookmarkTotalItems();
	if(0 == totalItems) {
		bookmarkCursor = 0;
		*activeScroll = 0;
		return;
	}

	if(bookmarkCursor >= totalItems) bookmarkCursor = totalItems - 1;
	const u32 visibleRows = bookmarkMenuRows();
	const u32 maxScroll = (totalItems > visibleRows) ? totalItems - visibleRows : 0;
	if(*activeScroll > maxScroll) *activeScroll = maxScroll;
	if(bookmarkCursor < *activeScroll) *activeScroll = bookmarkCursor;
	else if(bookmarkCursor >= *activeScroll + visibleRows) *activeScroll = bookmarkCursor - visibleRows + 1;
	if(*activeScroll > maxScroll) *activeScroll = maxScroll;
}

void Book :: moveBookmarkCursor(int delta)
{
	const u32 totalItems = bookmarkTotalItems();
	if(0 == totalItems) {
		bookmarkCursor = 0;
		return;
	}

	int next = int(bookmarkCursor) + delta;
	clamp(next, 0, int(totalItems) - 1);
	bookmarkCursor = next;
	clampBookmarkCursor();
}

bool Book :: activateBookmarkCursor()
{
	if(bookmarkTargets.empty()) return false;
	const u32* activeScroll = activeBookmarkScroll();
	if(bookmarkCursor < *activeScroll) return false;
	const u32 visibleIndex = bookmarkCursor - *activeScroll;
	if(visibleIndex >= bookmarkTargets.size()) return false;
	current_page = bookmarkTargets[visibleIndex];
	current_page.line_num = 0;
	draw_page(true);
	queueMarksSave();
	return true;
}

void Book :: bookmarkMenu()
{
	ensureToc();
	bookmarkView = bookmarkViewMarks;
	bookmarkCursor = 0;
	const int width = screens::layoutX();
	const int half = width / 2;
	const u32 uiFont = bookmarkMenuFont();
	const u32 visibleRows = bookmarkMenuRows();
	const int rowHeight = bookmarkMenuRowHeight();
	tabMarks = button(SAY2(bookmarks), 5, kTabY1, half - 2, kTabY2, uiFont);
	tabContents = button(SAY2(contents), half + 2, kTabY1, width - 5, kTabY2, uiFont);
	older = button(SAY2(older), 5, kActionY1, 70, kActionY2, uiFont);
	setMark = button("___Set___", 74, kActionY1, width - 74, kActionY2, uiFont);
	newer = button(SAY2(newer), width - 70, kActionY1, width - 5, kActionY2, uiFont);
	listUp = button("^", width - kArrowWidth, kListY1, width - 5, kListY1 + rowHeight - 2, uiFont);
	listDown = button("v", width - kArrowWidth, kListY1 + (visibleRows - 1) * rowHeight, width - 5, kListY1 + visibleRows * rowHeight - 2, uiFont);
	prbar = progressbar();
	draw_page(true, false);
	drawBookmarkMenu();
	while(pumpPowerManagement()) {
		swiWaitForVBlank();
		renderer::printClock(bottom_scr);
		scanKeys();
		int down = keysDown();
		if(down & KEY_TOUCH) {
			const float p = prbar.touched();
			u32* activeScroll = (bookmarkView == bookmarkViewContents) ? &tocScroll : &bookmarkScroll;
			const u32 totalItems = (bookmarkView == bookmarkViewContents) ? tocEntries.size() : bookmarks.size();
			if(tabMarks.touched()) {
				bookmarkView = bookmarkViewMarks;
				drawBookmarkMenu();
			}
			else if(tabContents.touched()) {
				bookmarkView = bookmarkViewContents;
				drawBookmarkMenu();
			}
			else if(setMark.touched()) {
				if(bookmarks.find(current_page) == bookmarks.end()) {
					bookmarks.insert(bookmark(current_page.parag_num,0));
				}
				else
					bookmarks.erase(current_page);
				queueMarksSave();
				drawBookmarkMenu();
			}
			else if(newer.touched() && moreNew()) {
				current_page = *bookmarks.upper_bound(current_page);
				queueMarksSave();
				draw_page(true);
				drawBookmarkMenu();
			}
			else if(older.touched() && moreOld()) {
				current_page = *--bookmarks.lower_bound(current_page);
				queueMarksSave();
				draw_page(true);
				drawBookmarkMenu();
			}
			else if(listUp.touched() && *activeScroll > 0) {
				--*activeScroll;
				drawBookmarkMenu();
			}
			else if(listDown.touched() && *activeScroll + bookmarkVisible < totalItems) {
				++*activeScroll;
				drawBookmarkMenu();
			}
			else {
				bool changed = false;
				for(u32 i = 0; i < bookmarkRows.size(); ++i)
					if(bookmarkRows[i].touched()) {
						bookmarkCursor = *activeScroll + i;
						current_page = bookmarkTargets[i];
						current_page.line_num = 0;
						queueMarksSave();
						draw_page(true);
						changed = true;
						break;
					}
				if(changed) {
					drawBookmarkMenu();
				}
				else if (p < 1.0f) {
					u32 parag_num = p * total_paragraths();
					if(parag_num == current_page.parag_num) continue;
					current_page.parag_num = parag_num;
					current_page.line_num = 0;
					queueMarksSave();
					draw_page(true);
					drawBookmarkMenu();
				}
			}
		}
		else if(down & KEY_START) break;
		else if(down & KEY_SELECT) {
			if(activateBookmarkCursor()) drawBookmarkMenu();
		}
		else if(down & rKey(rUp)) {
			moveBookmarkCursor(-1);
			drawBookmarkMenu();
		}
		else if(down & rKey(rDown)) {
			moveBookmarkCursor(1);
			drawBookmarkMenu();
		}
		else if(down & rKey(rLeft)) {
			if(bookmarkView != bookmarkViewMarks) {
				bookmarkView = bookmarkViewMarks;
				clampBookmarkCursor();
				drawBookmarkMenu();
			}
			else break;
		}
		else if(down & rKey(rRight)) {
			if(bookmarkView != bookmarkViewContents) {
				bookmarkView = bookmarkViewContents;
				clampBookmarkCursor();
				drawBookmarkMenu();
			}
			else if(activateBookmarkCursor()) drawBookmarkMenu();
		}
	}
	flushMarks();
	renderer::clearScreens(settings::bgCol);
}

void Book :: drawBookmarkMenu()
{
	renderer::setTopScreenMirror(false);
	prbar = progressbar();
	if(bookmarks.find(current_page) == bookmarks.end()) setMark.setText(SAY2(set));
	else setMark.setText(SAY2(remove));
	bookmarkRows.clear();
	bookmarkTargets.clear();

	const int width = screens::layoutX();
	const int half = width / 2;
	const int listRight = width - kArrowWidth - 4;
	const u32 visibleRows = bookmarkMenuRows();
	const int rowHeight = bookmarkMenuRowHeight();
	const u32 itemFont = bookmarkMenuListFont();
	const u32 minItemFont = bookmarkMenuRotated() ? 10u : 9u;
	const bool showContents = (bookmarkView == bookmarkViewContents);
	const u32 totalItems = showContents ? tocEntries.size() : bookmarks.size();
	u32* activeScroll = showContents ? &tocScroll : &bookmarkScroll;
	clampBookmarkCursor();

	renderer::clearScreens(settings::bgCol, bottom_scr);
	renderer::fillRect(showContents ? half + 2 : 5, kTabY1 + 1, showContents ? width - 5 : half - 2, kTabY2 - 1, Blend(72), bottom_scr);
	tabMarks.draw();
	tabContents.draw();
	setMark.draw();
	if(moreNew()) newer.draw();
	if(moreOld()) older.draw();

	bookmarkVisible = 0;
	if(showContents) {
		for(u32 i = *activeScroll; i < tocEntries.size() && bookmarkVisible < visibleRows; ++i, ++bookmarkVisible) {
			const int rowTop = kListY1 + bookmarkVisible * rowHeight;
			const int rowBottom = kListY1 + (bookmarkVisible + 1) * rowHeight - 2;
			if(i == bookmarkCursor)
				renderer::fillRect(6, rowTop, listRight, rowBottom, Blend(72), bottom_scr);
			button item("", 6, rowTop, listRight, rowBottom, itemFont);
			bookmarkRows.push_back(item);
			bookmarkTargets.push_back(tocEntries[i].place);
			drawBookmarkListRow(6, rowTop, listRight, rowBottom, tocEntries[i].title, itemFont, minItemFont);
		}
	}
	else {
		u32 skip = *activeScroll;
		for (std::set<bookmark>::iterator it = bookmarks.begin(); it != bookmarks.end() && bookmarkVisible < visibleRows; ++it) {
			if(skip) {
				--skip;
				continue;
			}
			const int rowTop = kListY1 + bookmarkVisible * rowHeight;
			const int rowBottom = kListY1 + (bookmarkVisible + 1) * rowHeight - 2;
			if(*activeScroll + bookmarkVisible == bookmarkCursor)
				renderer::fillRect(6, rowTop, listRight, rowBottom, Blend(72), bottom_scr);
			const string progress = progressLabel(it->parag_num, total_paragraths());
			const string label = paragraphMenuLabel(it->parag_num);
			button item("", 6, rowTop, listRight, rowBottom, itemFont);
			bookmarkRows.push_back(item);
			bookmarkTargets.push_back(*it);
			drawBookmarkMarkRow(6, rowTop, listRight, rowBottom, progress, label, itemFont, minItemFont);
			++bookmarkVisible;
		}
	}

	if(0 == bookmarkVisible) {
		renderer::rect(6, kListY1, listRight, kListY1 + visibleRows * rowHeight - 2, bottom_scr);
		drawListTextBlock(12, kListY1 + rowHeight / 2, listRight - 12, kListY1 + visibleRows * rowHeight - rowHeight / 2, showContents ? SAY2(contents) : SAY2(bookmarks), itemFont, minItemFont);
		renderer::printStr(eUtf8, bottom_scr, listRight - renderer::strWidth(eUtf8, "0", 0, 0, itemFont) - 12, kListY1 + rowHeight + itemFont / 2, "0", 0, 0, itemFont);
	}

	string detailText;
	if(0 == totalItems) {
		detailText = showContents
			? string("No contents entries were found for this book.")
			: string("No bookmarks saved yet. Use Set to store the current page.");
	}
	else {
		const u32 visibleIndex = bookmarkCursor - *activeScroll;
		if(visibleIndex < bookmarkTargets.size()) {
			const bookmark target = bookmarkTargets[visibleIndex];
			const string label = showContents
				? tocEntries[bookmarkCursor].title
				: paragraphMenuLabel(target.parag_num);
			detailText = label.empty() ? string("Untitled entry") : label;
		}
	}
	if(detailText.empty())
		detailText = showContents ? string("Open a chapter entry from the list.") : string("Pick a bookmark entry from the list.");

	char countBuf[40];
	sprintf(countBuf, "%lu items", (unsigned long)totalItems);
	const string summaryText = progressLabel(current_page.parag_num, total_paragraths()) + " | " +
		screenModeLabel() + " | " + countBuf;
	const string footerText = showContents
		? string("Left/Right switches tabs. Select or Right opens the highlighted chapter.")
		: string("Set toggles the current page. Up/Down moves. Select opens the highlighted bookmark.");
	drawTopContextOverlay(noPath(bookFile), showContents ? string("Contents") : string("Bookmarks"), detailText, summaryText, footerText);

	if(*activeScroll > 0) listUp.draw();
	if(*activeScroll + bookmarkVisible < totalItems) listDown.draw();

	prbar.draw (float(current_page.parag_num) / total_paragraths());
	for (std::set<bookmark>::iterator it = bookmarks.begin(); it != bookmarks.end(); ++it)
		prbar.mark(float(it->parag_num) / total_paragraths());
	renderer::printClock(bottom_scr, true);
	setBacklightMode(blOverlay);
}

bool Book :: menu()
{
	drawMenu(false);
	bool running = true;
	bool settingsDirty = false;
	bool exitBook = false;
	while((running = pumpPowerManagement())) {
		swiWaitForVBlank();
		scanKeys();
		int down = keysDown();
		if(down & rKey(rDown)) break;
		
		if(!(down & KEY_TOUCH)) continue;
		
		const string* t = menuGrid.update();
		if(SAY(rotate) == t) {
			int i = settings::layout;
			++i;
			if(i > d270) i = d0;
			current_page.line_num = 0;
			settings::layout = Layout(i);
			clearParagraphCache();
			drawMenu();
			settingsDirty = true;
		}
		else if(SAY(close) == t) {
			exitBook = true;
			break;
		}
		else if(SAY(light) == t) {
			cycleBacklight();
			settingsDirty = true;
		}
		else if(SAY(invert) == t) {
			settings::setLowLightMode(!settings::lowLightMode());
			clearParagraphCache();
			drawMenu(false);
			settingsDirty = true;
		}
		else if(SAY(screens) == t) {
			int i = settings::scrConf;
			i++;
			if(i > scBoth) i = scTop;
			settings::scrConf  = scrConfig(i);
			current_page.line_num = 0;
			clearParagraphCache();
			drawMenu();
			settingsDirty = true;
			string o;
			switch(settings::scrConf) {
				case scTop: o = *SAY(top); break;
				case scBottom: o = *SAY(bottom); break;
				case scBoth: o = *SAY(both); break;
			}
			menuGrid.print(SAY(screens), o);
		}
		else if(SAY(font) == t) {
			renderer::changeFont();
			current_page.line_num = 0;
			clearParagraphCache();
			drawMenu();
			settingsDirty = true;
			menuGrid.print(SAY(font), settings::font);
		}
		else if(SAY(colors) == t) {
			settings::setNightMode(!settings::nightMode());
			current_page.line_num = 0;
			clearParagraphCache();
			drawMenu(false);
			settingsDirty = true;
		}
		else if(SAY(style) == t) {
			using settings::tech;
			using namespace renderTech;
			switch(tech) {
				case linux:   case linux_v:		tech = windows; break;
				case windows: case windows_v:	tech = phone; 	break;
				case phone:		tech = linux;	break; default: ;
			}
			current_page.line_num = 0;
			clearParagraphCache();
			drawMenu(false);
			settingsDirty = true;
		}
		else if(SAY(sharp) == t) {
			sharpness();
			if(appShouldExit()) return true;
			drawMenu(false);
		}
		else if(SAY(pbar) == t) {
			settings::pbar = !settings::pbar;
			drawMenu(false);
			settingsDirty = true;
		}
		else if(SAY(justify) == t) {
			settings::justify = !settings::justify;
			drawMenu(false);
			settingsDirty = true;
		}
		else if(SAY(size) == t) {
			using settings::font_size;
			int old = font_size;
			font_size += (rLeft == menuGrid.val) ? -1: 1;
			clamp(font_size, 10, 32);
			if(old == font_size) continue;
			current_page.line_num = 0;
			clearParagraphCache();
			drawMenu();
			settingsDirty = true;
			char buf[5];
			sprintf(buf, "%d", settings::font_size);
			menuGrid.print(SAY(size), string(buf));
		}
		else if(SAY(gamma) == t) {
			using settings::gamma;
			int old = gamma;
			gamma += (rLeft == menuGrid.val) ? -1: 1;
			clamp(gamma, 0, 2);
			if(old == gamma) continue;
			drawMenu(false);
			settingsDirty = true;
		}
		else if(SAY(gap) == t) {
			using settings::line_gap;
			int old = line_gap;
			line_gap += (rLeft == menuGrid.val) ? -1: 1;
			clamp(line_gap, 0, 20);
			if(old == line_gap) continue;
			clearParagraphCache();
			drawMenu(false);
			settingsDirty = true;
		}
		else if(SAY(indent) == t) {
			using settings::first_indent;
			int old = first_indent;
			first_indent += (rLeft == menuGrid.val) ? -1: 1;
			clamp(first_indent, 0, 50);
			if(old == first_indent) continue;
			clearParagraphCache();
			drawMenu();
			settingsDirty = true;
		}
		else if(SAY(language) == t) {
			translation();
			if(appShouldExit()) return true;
			drawMenu(false);
			settingsDirty = true;
		}
	}
	if(settingsDirty) settings::save();
	renderer::clearScreens(settings::bgCol);
	return exitBook || !running;
}

void Book :: drawMenu(bool recache)
{
	u32 it = menuGrid.iter;
	menuGrid = grid(it);
	menuGrid.push(SAY(close), 1)
	->push(SAY(invert))
	->push(SAY(justify))	
	->push(SAY(rotate))
	->push(SAY(gamma), 0 , true)
	->push(SAY(pbar))
	->push(SAY(screens))
	->push(SAY(light))
	->push(SAY(size), 0 , true)
	->push(SAY(font))
	->push(SAY(style))
	->push(SAY(gap), 0 , true)
	->push(SAY(indent), 0 , true)	
	->push(SAY(colors))
	->push(SAY(sharp))
	->push(SAY(language));
	draw_page(true, recache);
	{
		char fontBuf[16];
		sprintf(fontBuf, "Font %d", settings::font_size);
		const string detailText = screenModeLabel() + " screen mode. " + themeLabel() + " theme. " + fontBuf + ".";
		const string summaryText = progressLabel(current_page.parag_num, total_paragraths()) +
			" | rot " + layoutLabel() + " | " + (settings::justify ? string("J on") : string("J off"));
		drawTopContextOverlay(noPath(bookFile), string("Reading Settings"), detailText, summaryText,
			string("Tap a tile to change it. Down or B closes the settings panel."));
	}
	renderer::setTopScreenMirror(false);
	renderer::clearScreens(settings::bgCol, bottom_scr);
	menuGrid.draw();
	if(settings::lowLightMode()) menuGrid.print(SAY(colors), SAY2(invert));
	else menuGrid.print(SAY(colors), settings::nightMode() ? kThemeNight : kThemePaper);
	setBacklightMode(blOverlay);
}

void Book :: saveMarks()
{
	std::ofstream os((appBookmarksPath() + noPath(bookFile) + ".bm").c_str());
	if(!os.good()) {
		os.close();
		os.clear();
		os.open(encname.c_str());
	}
	if(!os.good()) bsod("book.save_marks: Can't save bookmarks.");
	os<<current_page.parag_num<<'\n';
	for (std::set<bookmark>::iterator it = bookmarks.begin(); it != bookmarks.end(); ++it)
		os<<it->parag_num<<'\n';
}

void Book :: loadMarks()
{
	std::ifstream is((appBookmarksPath() + noPath(bookFile) + ".bm").c_str());
	if(!is.good()) {
		is.close();
		is.clear();
		is.open(encname.c_str());
	}
	if(!is.good()) return;
	const u32 total = total_paragraths();
	u32 temp;
	is >> temp;
	if(temp < total) current_page.parag_num = temp;
	while(is >> temp) if(temp < total) bookmarks.insert(bookmark(temp,0));
}

string fileReq(const string& path)
{
	DIR* dir = opendir(path.c_str());
	struct dirent* ent;
	if(!dir) bsod(("book.filereq: cannot open "  + path).c_str());

	vector<button> buttons;
	int peny = 0;
	renderer::setTopScreenMirror(false);
	renderer::clearScreens(settings::bgCol);

	while ((ent = readdir(dir)) != NULL) {
		if(strcmp(".", ent->d_name) == 0 || strcmp("..", ent->d_name) == 0)
			continue;

		if (ent->d_type != DT_DIR)
		{
			string filename = ent->d_name;
			if(extention(filename) == "txt") {
				buttons.push_back(button(filename.erase(filename.find_last_of('.')), peny));
				buttons.back().draw();
				peny += buttonFontSize;
			}
		}
	}
	closedir(dir);
	while(pumpPowerManagement()) {
		swiWaitForVBlank();
		scanKeys();
		if(!(keysDown() & KEY_TOUCH)) continue;
		else for(u32 i = 0; i < buttons.size(); i++)
			if(buttons[i].touched()) {
				return buttons[i].getText() + ".txt";
			}
	}
	return string();
}

void Book :: translation()
{
	string f = fileReq(transPath);
	if(f.empty()) return;
	settings::translname = f;
	loadTrans(transPath + f);
}
