#include "book.h"
#include "renderer.h"
#include "button.h"

#include <algorithm>
#include <cstring>
#include <locale>

namespace
{

template<typename charT>
struct my_equal {
	my_equal(const std::locale& loc) : loc_(loc) {}
	bool operator()(charT ch1, charT ch2) {
		return std::toupper(ch1, loc_) == std::toupper(ch2, loc_);
	}
private:
	const std::locale& loc_;
};

template<typename T>
int ci_find_substr(const T& str1, const T& str2, const std::locale& loc = std::locale())
{
	typename T::const_iterator it = std::search(
		str1.begin(), str1.end(),
		str2.begin(), str2.end(),
		my_equal<typename T::value_type>(loc));
	return (it != str1.end()) ? int(it - str1.begin()) : -1;
}

const string CIstr("Case insensitive: ");

bool unifind(bool CI, const string& str1, const string& str2)
{
	return CI ? (ci_find_substr(str1, str2) != -1)
		: (str1.find(str2) != string::npos);
}

string compactText(const string& text, size_t maxChars)
{
	if(text.size() <= maxChars) return text;
	return text.substr(0, maxChars - 3) + "...";
}

bool promptSearchText(string& searchstr)
{
	SwkbdState swkbd;
	char input[256];
	memset(input, 0, sizeof(input));
	if(!searchstr.empty())
		strncpy(input, searchstr.c_str(), sizeof(input) - 1);

	swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 1, sizeof(input) - 1);
	swkbdSetHintText(&swkbd, "Search text");
	if(swkbdInputText(&swkbd, input, sizeof(input)) != SWKBD_BUTTON_CONFIRM)
		return false;

	searchstr = input;
	return !searchstr.empty();
}

void drawSearchPanel(const string& searchstr, bool doCI, const string& status, button& prev, button& next, button& toggle, button& edit, button& close)
{
	renderer::clearScreens(settings::bgCol, bottom_scr);
	renderer::printStr(eUtf8, bottom_scr, 10, 20, "Search", 0, 0, 18);
	renderer::printStr(eUtf8, bottom_scr, 10, 42, compactText(searchstr, 38), 0, 0, 12);
	renderer::printStr(eUtf8, bottom_scr, 10, 60, compactText(status, 42), 0, 0, 11);

	prev.draw();
	next.draw();
	toggle.setText(CIstr + (doCI ? "yes" : "no"));
	toggle.draw();
	edit.draw();
	close.draw();
	renderer::printClock(bottom_scr, true);
}

} // namespace

void Book::search()
{
	string searchstr;
	if(!promptSearchText(searchstr)) {
		draw_page();
		return;
	}

	bool doCI = true;
	const Layout old_layout = settings::layout;
	settings::layout = d0;
	current_page.line_num = 0;
	draw_page(true);
	setBacklightMode(blOverlay);

	button prev("Previous", 10, 82, 152, 106, 12);
	button next("Next", 168, 82, 310, 106, 12);
	button toggle("", 10, 116, 310, 140, 11);
	button edit("Edit text", 10, 150, 152, 174, 12);
	button close("Close", 168, 150, 310, 174, 12);
	string status("A/Right = next, B/Left = previous");
	const u32 total_parag = total_paragraths();

	while(pumpPowerManagement()) {
		drawSearchPanel(searchstr, doCI, status, prev, next, toggle, edit, close);
		swiWaitForVBlank();
		scanKeys();

		bool forward = false;
		bool backward = false;
		const int down = keysDown();
		if(down & (KEY_SELECT | KEY_START)) break;
		if(down & (KEY_A | rKey(rRight))) forward = true;
		if(down & (KEY_B | rKey(rLeft))) backward = true;

		if(down & KEY_TOUCH) {
			if(next.touched()) forward = true;
			else if(prev.touched()) backward = true;
			else if(toggle.touched()) doCI = !doCI;
			else if(edit.touched()) {
				promptSearchText(searchstr);
				status = "Updated search text";
			}
			else if(close.touched()) {
				break;
			}
		}

		if(!forward && !backward) continue;
		if(searchstr.empty()) {
			status = "Search text is empty";
			continue;
		}

		bool found = false;
		if(forward) {
			for(u32 i = current_page.parag_num + 1; i < total_parag; ++i) {
				parag_str(i);
				if(unifind(doCI, parag.str, searchstr)) {
					current_page.parag_num = i;
					current_page.line_num = 0;
					queueMarksSave();
					draw_page(true);
					status = "Found next match";
					found = true;
					break;
				}
			}
		}
		else {
			for(int i = (int)current_page.parag_num - 1; i >= 0; --i) {
				parag_str(i);
				if(unifind(doCI, parag.str, searchstr)) {
					current_page.parag_num = i;
					current_page.line_num = 0;
					queueMarksSave();
					draw_page(true);
					status = "Found previous match";
					found = true;
					break;
				}
			}
		}

		if(!found)
			status = forward ? "No later match found" : "No earlier match found";
	}

	settings::layout = old_layout;
	if(appShouldExit()) return;
	draw_page();
}
