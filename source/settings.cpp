#include "settings.h"
#include "renderer.h"
#include "screens.h"
#include <fstream>

namespace settings
{
s8 font_size, first_indent, line_gap, BGR;
bool justify, pbar;
string binname;
string font, font_bold, font_italic, recent_book, encname, translname;
renderTech::type tech;
Layout layout;
Color bgCol, fCol;
scrConfig scrConf;
int brightness, gamma;

namespace
{

const Color kLegacyPaperBg(31, 30, 27);
const Color kLegacyPaperFg(2, 2, 2);
const Color kOldPaperBg(30, 28, 23);
const Color kOldPaperFg(4, 3, 2);
const Color kPaperBg(28, 25, 18);
const Color kPaperFg(4, 3, 2);
const Color kNightBg(1, 1, 0);
const Color kNightFg(20, 17, 10);
const Color kLowLightBg(0, 0, 0);
const Color kLowLightFg(5, 3, 2);
bool restoreNightAfterLowLight = true;

bool sameColor(const Color& left, const Color& right)
{
	return left.R == right.R && left.G == right.G && left.B == right.B;
}

string settingsFilePath()
{
	return appDataPath() + "/settings_" + binname + ".b";
}

} // namespace

void reset()
{
	font_size = 18; first_indent = 20; line_gap = 1;
	BGR = 0;
	justify = true;
	font = "LiberationSerif.ttf";
	font_bold = "LiberationSerifB.ttf";
	font_italic = "LiberationSerifI.ttf";
	tech = renderTech::windows;
	encname = "CP1251_Russian.txt";
	translname = "English.txt";
	recent_book = "";
	layout = d0;
	fCol  = kPaperFg;
	bgCol = kPaperBg;
	scrConf = scBoth;
	brightness = 3;
	pbar = true;
	gamma = 1;
	restoreNightAfterLowLight = true;
}

bool nightMode()
{
	return sameColor(bgCol, kNightBg) && sameColor(fCol, kNightFg);
}

bool lowLightMode()
{
	return sameColor(bgCol, kLowLightBg) && sameColor(fCol, kLowLightFg);
}

void setNightMode(bool enable)
{
	bgCol = enable ? kNightBg : kPaperBg;
	fCol = enable ? kNightFg : kPaperFg;
	restoreNightAfterLowLight = enable;
}

void setLowLightMode(bool enable)
{
	if(enable) {
		restoreNightAfterLowLight = nightMode();
		bgCol = kLowLightBg;
		fCol = kLowLightFg;
		return;
	}
	setNightMode(restoreNightAfterLowLight);
}

struct settStruct {
	s8 Font_size, First_indent, Line_gap, bGR;
	bool Justify;
	Layout llayout;
	renderTech::type Tech;
	Color BgCol, FCol;
	scrConfig ScrConf;
	settStruct() : Font_size(font_size), First_indent(first_indent), Line_gap(line_gap), bGR(BGR),
					Justify(justify), llayout(layout),Tech(tech),BgCol(bgCol),FCol(fCol), ScrConf(scrConf) {}
	void apply() {
		font_size = Font_size; first_indent = First_indent;
		line_gap = Line_gap; BGR = bGR, justify = Justify;
		layout = llayout; tech = Tech;
		bgCol = BgCol; fCol = FCol;
		scrConf = ScrConf;
		clamp(font_size, 10, 32);
		clamp(first_indent, 0, 50);
		clamp(line_gap, 0, 20);
		clamp(BGR, 0, 3);
		if(layout < d0 || layout > d270) layout = d0;
		if(scrConf < scTop || scrConf > scBoth) scrConf = scBoth;
	}
};

void load()
{
	reset();
	std::ifstream is(settingsFilePath().c_str(), std::ios::binary);
	is.seekg(0 , std::ios::beg);
	
	if(!is.good() || !is.is_open()) {
		is.close();
		save();
		return;
	}
	
	settStruct st;
	string end;
	
	is.read((char*)&st, sizeof(settStruct));
	getline(is, font);
	getline(is, font_bold);
	getline(is, font_italic);
	getline(is, recent_book);
	getline(is, end);
	is >> brightness >> pbar >> gamma >> encname >> translname;
	is.close();
	
	bool techCorrect = true;
	switch(st.Tech) {
		using namespace renderTech;
		case linux:   case linux_v:		
		case windows: case windows_v:
		case phone:	break;
		default: techCorrect = false;
	}
	
	if(end.compare("end") || !techCorrect) {  //bad settings file
		reset();
		save();
	}
	else {
		st.apply();
		clamp(brightness, 0, 3);
		clamp(gamma, 0, 2);
		if((sameColor(bgCol, kLegacyPaperBg) && sameColor(fCol, kLegacyPaperFg)) ||
			(sameColor(bgCol, kOldPaperBg) && sameColor(fCol, kOldPaperFg))) {
			bgCol = kPaperBg;
			fCol = kPaperFg;
		}
		if(lowLightMode()) restoreNightAfterLowLight = true;
		else restoreNightAfterLowLight = nightMode();
		applyBrightness();
	}
}

void save()
{
	std::ofstream os;
	os.open(settingsFilePath().c_str(), std::ios::binary);
	os.seekp(0 , std::ios::beg);
	if(!os.good() || !os.is_open()) bsod("settings.save:Can't save settings.");
	settStruct st;
	os.write((char*)&st, sizeof(settStruct));
	os<<font		<<'\n'<<
		font_bold	<<'\n'<<
		font_italic	<<'\n'<<
		recent_book	<<'\n'<<
		"end"		<<'\n'<<
		brightness <<'\n'<<
		pbar <<'\n'<<
		gamma <<'\n'<<
		encname <<'\n'<<
		translname;
}

} // namespace settings
