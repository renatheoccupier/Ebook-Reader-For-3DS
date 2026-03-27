#include "screens.h"
#include "renderer.h"
#include "book.h"
#include "settings.h"

extern fontStyle accMarks(int start, int end, const vector<marked>& marks, fontStyle defstyle);

namespace screens
{

u8 up_margin=6, side_margins=8, bottom_margin=0;
u16 penX, penY;
scr_id current_scr;
const u16* layoutDimX[] = {&dimX, &dimY,&dimX, &dimY};
const u16* layoutDimY[] = {&dimY, &dimX,&dimY, &dimX};

namespace
{

u32 lineCapacityForScreen(scr_id scr)
{
	return (screen_text_height(scr) - 1 - up_margin - bottom_margin - settings::font_size/4)
		/ (settings::font_size + settings::line_gap);
}

} // namespace

u32 capacity(bool onlyOne)
{
	if(onlyOne) return first_screen_capacity();
	return first_screen_capacity() + screen_capacity((scr_id)!first_scr());
}

u32 screen_capacity(scr_id scr)
{
	return lineCapacityForScreen(scr);
}

u32 first_screen_capacity()
{
	return lineCapacityForScreen(first_scr());
}

void print_line(Encoding enc, const paragrath& parag, u16 linenum, u16 total_l, bool onlyTop)
{
	if(total_l == 0) {
		scr_id scr = onlyTop ? top_scr : first_scr();
		renderer::clearScreens(settings::bgCol, scr);
		current_scr = scr;
		penY = up_margin;
	}
	else if(!onlyTop && settings::scrConf == scBoth && total_l == first_screen_capacity()) {
		current_scr = (scr_id)!current_scr;
		renderer::clearScreens(settings::bgCol, current_scr);
		penY = up_margin;
	}
	if(penY != up_margin) penY += settings::line_gap;
	penY += settings::font_size;
	u8 space_width = renderer::strWidth(e1251, " ");
	const int available_width = line_width(current_scr);
	const bool wideTopScreen = (current_scr == top_scr && (settings::layout == d0 || settings::layout == d180));
	const int text_width = wideTopScreen ? MIN(available_width, line_width() + 64) : available_width;
	int text_left = side_margins;
	if(wideTopScreen && settings::layout == d180)
		text_left = screen_text_width(current_scr) - text_width - side_margins;
	penX = text_left;
	if (0 == linenum && pnormal == parag.type) penX += settings::first_indent;
	int remained = text_width - parag.lines[linenum].width;
	
	if(0 == linenum) remained -= settings::first_indent;
	u8 space_justify = 0;
	if (settings::justify && linenum != parag.lines.size()-1 &&
			pnormal == parag.type && remained > 0 && parag.lines[linenum].words.size() > 1)
		space_justify = remained / (parag.lines[linenum].words.size()-1);
	if(wideTopScreen && space_justify > 4) space_justify = 4;
	
	if(parag.type == pimage) {
		const u16 slot_top = penY - settings::font_size;
		const u16 centered_x = text_left + (text_width - parag.image_width) / 2;
		const u16 slice_y = linenum * (settings::font_size + settings::line_gap);
		renderer::drawImageSlice(current_scr, centered_x, slot_top, parag.image_pixels, parag.image_width, parag.image_height, slice_y, settings::font_size + settings::line_gap);
		return;
	}

	if(parag.type == ptitle) penX = text_left + (text_width - parag.lines[linenum].width) / 2;
	
	fontStyle st[] = {fnormal, fbold, fitalic};
	fontStyle style = st[parag.type];
	for(vector<Word>::const_iterator it = parag.lines[linenum].words.begin(); it != parag.lines[linenum].words.end(); ++it) {
		fontStyle actual_style = style;
		actual_style = accMarks(it->start, it->end, parag.marks, style);
		
		penX += renderer::printStr(enc, current_scr, penX, penY, parag.str, it->start, it->end, settings::font_size, actual_style)
			+ space_width;
		if (it->hyphen) renderer::printStr(enc, current_scr, penX - space_width, penY, "-");
		penX += space_justify;
	}
}

} //namespace screens
