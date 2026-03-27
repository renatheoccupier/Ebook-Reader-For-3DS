#pragma once
#include "default.h"
#include "settings.h"

struct paragrath;

namespace screens
{
	void print_line (Encoding enc, const paragrath& parag, u16 linenum, u16 total_l, bool onlyTop);
	u32 capacity(bool onlyOne);	//maximum number of lines
	u32 screen_capacity(scr_id scr);
	u32 first_screen_capacity();
	const u16 dimX = 319, dimY = 239;
	extern u8 up_margin, side_margins, bottom_margin;
	extern const u16 *layoutDimX[4], *layoutDimY[4];
	extern scr_id current_scr;
	inline u16 layoutX(){return *layoutDimX[settings::layout];}
	inline u16 layoutY(){return *layoutDimY[settings::layout];}
	inline int screen_text_width(scr_id scr)
	{
		if(settings::layout == d0 || settings::layout == d180)
			return (scr == top_scr) ? 400 : 320;
		return dimY + 1;
	}
	inline int screen_text_height(scr_id scr)
	{
		if(settings::layout == d90 || settings::layout == d270)
			return (scr == top_scr) ? 400 : 320;
		return dimY + 1;
	}
	inline int line_width()
	{
		if(settings::scrConf == scTop) return screen_text_width(top_scr) - side_margins * 2;
		if(settings::scrConf == scBottom) return screen_text_width(bottom_scr) - side_margins * 2;
		return layoutX() + 1 - side_margins * 2;
	}
	inline int line_width(scr_id scr) {return screen_text_width(scr) - side_margins * 2;}
}


inline void toLayoutSpace(int& x, int& y)
{
	using namespace screens;
	switch (settings::layout) {
		case d0:	return;
		case d90:	{int c = x; x = dimX - y; y = c; return;}
		case d180:	x=dimX-x; y=dimY-y; return;
		case d270:	{int c = x; x = y; y = dimY - c;}
	}
}
