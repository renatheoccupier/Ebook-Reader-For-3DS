#include "button.h"
#include "renderer.h"
#include "screens.h"
#include "settings.h"
#include "controls.h"
#include "algorithm"
#include "utf8.h"

namespace
{

static const u32 marqueeHoldSteps = 18;
static const u32 marqueeFramesPerChar = 5;

vector<u32> utf8Offsets(const string& str)
{
	vector<u32> offsets;
	offsets.push_back(0);
	const char* it = str.c_str();
	const char* end = it + str.size();
	while(it < end) {
		utf8::unchecked::next(it);
		offsets.push_back(it - str.c_str());
	}
	return offsets;
}

u32 advanceChars(const string& str, u32 start, int count)
{
	const char* it = str.c_str() + start;
	const char* end = str.c_str() + str.size();
	for(int i = 0; i < count && it < end; ++i)
		utf8::unchecked::next(it);
	return it - str.c_str();
}

u32 clippedEnd(const string& str, u32 start, int width, u32 fontSize)
{
	int breakat = 0;
	renderer::strWidth(eUtf8, str, start, 0, fontSize, fnormal, &breakat, width);
	if(breakat <= 0) breakat = 1;
	return advanceChars(str, start, breakat);
}

string ellipsizedButtonText(const string& str, int width, u32 fontSize)
{
	const u32 end = clippedEnd(str, 0, width, fontSize);
	if(end >= str.size()) return str.substr(0, end);

	const string ellipsis("...");
	const int ellipsisWidth = renderer::strWidth(eUtf8, ellipsis, 0, 0, fontSize);
	if(ellipsisWidth >= width) return str.substr(0, end);

	const u32 clipped = clippedEnd(str, 0, width - ellipsisWidth, fontSize);
	return str.substr(0, clipped) + ellipsis;
}

u32 marqueeStart(const string& str, int width, u32 fontSize, u32 marqueeStep)
{
	const vector<u32> offsets = utf8Offsets(str);
	if(offsets.size() <= 1) return 0;

	u32 endStart = offsets.size() - 2;
	for(u32 i = 0; i + 1 < offsets.size(); ++i) {
		if(renderer::strWidth(eUtf8, str, offsets[i], 0, fontSize) <= width) {
			endStart = i;
			break;
		}
	}
	if(0 == endStart) return 0;

	const u32 travelSteps = endStart * marqueeFramesPerChar;
	const u32 cycle = marqueeHoldSteps + travelSteps;
	if(0 == cycle) return 0;
	const u32 phase = marqueeStep % cycle;
	if(phase < marqueeHoldSteps) return 0;
	return MIN(endStart, (phase - marqueeHoldSteps) / marqueeFramesPerChar);
}

} // namespace

button :: button(const string& str, int X1, int Y1, int X2, int Y2, u32 fs)
	:txt(str), solid(), x1(X1), y1(Y1), strx(1), stry(),  fsize(fs), marquee(), autoFit(), minFontSize(fs)
{
	if(-1 == Y1) {
		x1 = 0;
		y1 = X1;
		x2 = screens::layoutX();
		y2 = y1 + fsize;
	}
	else if(-1 == X2) {
		x2 = x1 + renderer::strWidth(eUtf8, str,0,0,fsize) + 5;
		y2 = y1 + fsize;
	}
	else {
		x2 = X2;
		y2 = Y2;
		setText(str);
	}
}

void button :: enableAutoFit(u32 minFont)
{
	autoFit = true;
	marquee = false;
	minFontSize = MIN(minFont, fsize);
}

void button :: enableMarquee(u32 minFont)
{
	autoFit = true;
	marquee = true;
	minFontSize = MIN(minFont, fsize);
}

void button :: setText(const string& str)
{
	u16 width = renderer::strWidth(eUtf8, str,0,0,fsize);
	txt = str;
	strx = (x2 - x1 - width) / 2 - 2; 		
	stry = (y2 - y1 - fsize) / 2;
}

int button :: textAreaWidth() const
{
	return MAX(1, int(x2) - int(x1) - 4);
}

u32 button :: drawFontSize() const
{
	u32 font = fsize;
	if(!autoFit) return font;
	while(font > minFontSize && renderer::strWidth(eUtf8, txt, 0, 0, font) > textAreaWidth())
		--font;
	return font;
}

bool button :: needsMarquee() const
{
	if(!marquee) return false;
	return renderer::strWidth(eUtf8, txt, 0, 0, drawFontSize()) > textAreaWidth();
}

bool button :: touched()
{
	touchPosition t;
	touchRead(&t);
	int X1 = x1, Y1 = y1, X2 = x2, Y2 = y2;
	toLayoutSpace(X1, Y1);
	toLayoutSpace(X2, Y2);
	if(	!(t.px >= MIN(X1, X2) && t.py >= MIN(Y1, Y2) &&
		t.px <  MAX(X1, X2) && t.py < MAX(Y1, Y2)) ) return false;
	u16 dif;
	if(settings::layout == d0 || settings::layout == d180) dif = abs(X1 - t.px);
	else dif = abs(Y1 - t.py);
	val =  ((2*dif) / (x2 - x1)) ? rRight : rLeft;
	return true;
}

static const int third = screens::dimY / 3, fifth = screens::dimY / 5;

static const string emptystr;

grid :: grid(u32 it) : iter(it)
{
	using namespace renderer;
	using namespace screens;
	const int height = third - 14, sp = (dimX - dimY) / 2;
	int x1 = 5, y1 = (dimY - height) / 2, x2 = sp - 5, y2 = (dimY + height) / 2;
	if(settings::layout == d0 || settings::layout == d180) {
		less = button("<", x1, y1, x2, y2);
		more = button(">", dimX - x2, y1, dimX - x1, y2);
	}
	else {
		less = button("<", 10, 6, layoutX() / 2 - 6, 26, 12);
		more = button(">", layoutX() / 2 + 6, 6, layoutX() - 10, 26, 12);
	}

	const bool portrait = layoutY() > layoutX();
	int offx = (layoutX() - dimY)/2 + (screens::dimX % third)/2;
	int offy = portrait
		? MAX(3, int(layoutY()) - int(dimY) - 8)
		: (layoutY() - dimY)/2 + (screens::dimX % third)/2;
	for(u32 i = 0; i < 9; i++)
		blanks.push_back(button("", offx + third*(i%3), offy + i/3*third, offx + third*(i%3+1), offy + (i/3+1)*third, 12));
}

grid* grid :: push(const string* str, int skip, bool plusmin)
{
	for(int i = 0; i < skip; i++) {
		cells.push_back(button());
		strPtrs.push_back(0);
		plusMinus.push_back(false);
	}
	cells.push_back(blanks[cells.size() % 9]);
	cells.back().setText(*str);
	strPtrs.push_back(str);
	plusMinus.push_back(plusmin);
	return this;
}

void grid :: print(const string* targ, string mess)
{
	vector<const string*>::iterator found = std::find(strPtrs.begin(), strPtrs.end(), targ);
	if(found == strPtrs.end()) return;
	int i = found - strPtrs.begin();
	renderer::printStr(eUtf8, bottom_scr, cells[i].x1 + 1, cells[i].y2 - 1, mess, 0,0,12);
	//renderer::printStr(eUtf8, top_scr,5,17,"IkuReader 0.064",0,0,12);
}

const string* grid :: update()
{
	touchPosition t;
	touchRead(&t);

	const auto touchedAt = [&](button& b, int padLeft, int padTop, int padRight, int padBottom) {
		int X1 = b.x1, Y1 = b.y1, X2 = b.x2, Y2 = b.y2;
		toLayoutSpace(X1, Y1);
		toLayoutSpace(X2, Y2);
		const int minX = MIN(X1, X2) - padLeft;
		const int minY = MIN(Y1, Y2) - padTop;
		const int maxX = MAX(X1, X2) + padRight;
		const int maxY = MAX(Y1, Y2) + padBottom;
		if(!(t.px >= minX && t.py >= minY && t.px < maxX && t.py < maxY))
			return false;
		u16 dif;
		if(settings::layout == d0 || settings::layout == d180) dif = abs(X1 - t.px);
		else dif = abs(Y1 - t.py);
		b.val = ((2 * dif) / MAX(1, abs(int(b.x2) - int(b.x1)))) ? rRight : rLeft;
		return true;
	};
	
	for(u32 i = iter; i < cells.size() && i < iter + 9; i++)
		if(!cells[i].txt.empty()) {
			const u32 pageIndex = i - iter;
			const int col = pageIndex % 3;
			const int row = pageIndex / 3;
			const int padLeft = (0 == col) ? 8 : 0;
			const int padTop = (0 == row) ? 6 : 0;
			const int padRight = (2 == col) ? 8 : 0;
			const int padBottom = (2 == row) ? 8 : 0;
			if(!touchedAt(cells[i], padLeft, padTop, padRight, padBottom))
				continue;
			val = cells[i].val;
			return strPtrs[i];
		}
	if(touchedAt(more, 0, 0, 0, 0) && nextPage()) return 0;
	if(touchedAt(less, 0, 0, 0, 0) && prevPage()) return 0;
	return 0;
}

bool grid :: nextPage()
{
	if(iter + 9 >= cells.size()) return false;
	iter += 9;
	renderer::clearScreens(settings::bgCol, bottom_scr);
	draw();
	return true;
}

bool grid :: prevPage()
{
	if(0 == iter) return false;
	iter -= (iter >= 9) ? 9 : iter;
	renderer::clearScreens(settings::bgCol, bottom_scr);
	draw();
	return true;
}

void grid :: draw()
{ 
	using namespace renderer;
	using namespace screens;
	for(u32 i = iter; i < cells.size() && i < iter + 9; i++) {
		if(!cells[i].txt.empty()) {
			if(plusMinus[i]) {
				int len = renderer::strWidth(eUtf8,"+",0,0,12); 
				printStr(eUtf8, bottom_scr,cells[i].x1+3,cells[i].y1+12,"\xE2\x80\x93",0,0,12); 
				printStr(eUtf8, bottom_scr,cells[i].x2-len-3,cells[i].y1+12,"+",0,0,12); 
				vLine(cells[i].x1 + (cells[i].x2 - cells[i].x1)/2, cells[i].y1, cells[i].y2, Blend(48));
			}
			cells[i].draw();
		}
	}
	if(iter) less.draw();
	if(iter + 9 < cells.size()) more.draw();
}

void button :: draw(u32 marqueeStep)
{ 
	using namespace renderer;
	if(solid) fillRect(x1,y1,x2,y2, settings::bgCol);
	rect(x1,y1,x2,y2); 
	const u32 font = drawFontSize();
	const int innerWidth = textAreaWidth();
	const int width = renderer::strWidth(eUtf8, txt, 0, 0, font);
	const int baselineY = y1 + font - 1 + (y2 - y1 - font) / 2;
	if(width <= innerWidth) {
		const int startX = x1 + 2 + MAX(0, (innerWidth - width) / 2);
		printStr(eUtf8, bottom_scr, startX, baselineY, txt, 0, 0, font);
		return;
	}

	if(!marquee) {
		const string clipped = ellipsizedButtonText(txt, innerWidth, font);
		printStr(eUtf8, bottom_scr, x1 + 2, baselineY, clipped, 0, 0, font);
		return;
	}

	const u32 start = marqueeStart(txt, innerWidth, font, marqueeStep);
	const u32 end = clippedEnd(txt, start, innerWidth, font);
	printStr(eUtf8, bottom_scr, x1 + 2, baselineY, txt, start, end, font);
}

scrollbar :: scrollbar()
{
	using namespace screens;
	x1 = layoutX() - 8;
	y1 = 0;
	x2 = layoutX();
	y2 = layoutY();
}

void scrollbar :: draw(float pos, float size)
{
	renderer::rect(x1,y1,x2,y2); 
	int low_border = y1 + int(pos * (1.0f - size) * (y2 - y1));
	int length = int(size * (y2 - y1));
	if(length < 0) length = 0;
	int high_border = low_border + length + 1;
	if(high_border > y2) high_border = y2;
	renderer::fillRect(x1,low_border, x2, high_border, Blend(128));
}

progressbar :: progressbar()
{
	using namespace screens;
	const u16 statusTop = layoutY() - buttonFontSize * 3 / 2;
	const u16 thickness = 12;
	x1 = 5;
	x2 = layoutX() - 5;
	y2 = statusTop - 6;
	y1 = y2 - thickness;
}

void progressbar :: draw(float pr)
{
	renderer::rect(x1,y1,x2,y2); 
	renderer::fillRect(x1, y1, x1 + pr * (x2 - x1), y2, Blend(128));
}

void progressbar :: mark(float pr)
{ renderer::vLine(x1 + pr * (x2 - x1), y1, y2); }

float progressbar :: touched()
{
	touchPosition t;
	touchRead(&t);
	int X1 = x1, Y1 = y1, X2 = x2, Y2 = y2;
	toLayoutSpace(X1, Y1);
	toLayoutSpace(X2, Y2);
	if(	!(t.px >= MIN(X1, X2) && t.py >= MIN(Y1, Y2) &&
		  t.px <  MAX(X1, X2) && t.py <  MAX(Y1, Y2)) ) return 2.0f;
	u16 dif;
	if(settings::layout == d0 || settings::layout == d180) dif = abs(X1 - t.px);
	else dif = abs(Y1 - t.py);
	return dif / float(x2 - x1);
}
