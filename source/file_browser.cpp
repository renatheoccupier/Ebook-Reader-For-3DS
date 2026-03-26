#include "file_browser.h"
#include "renderer.h"
#include "screens.h"
#include "controls.h"
#include "settings.h"
#include "utf8.h"
#include "unzip.h"
#include <algorithm>
#include <new>
#include <setjmp.h>
#include <time.h>
#include <ctype.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

extern "C" {
#include "jpeglib.h"
}

namespace
{

const int kBrowserScrollbarGutter = 24;
const int kPreviewMargin = 8;
const int kPreviewTitleFont = 10;
const int kPreviewTitleLines = 3;
const int kPreviewTitleGap = 2;
const int kPreviewImageGap = 8;
const int kPromptFont = 12;
const int kPreviewWarmupFrames = 10;
const u32 kPreviewCandidatePool = 12u;
const u32 kPreviewMaxEntryBytes = 768u * 1024u;
const u32 kPreviewCacheEntries = 3u;

string gLastBrowserPath;
int gLastBrowserPos = 0;
int gLastBrowserCursor = 0;

struct PreviewCacheEntry
{
	string file;
	vector<u16> pixels;
	u16 width, height;
	u16 maxWidth, maxHeight;
	bool hasImage;
	u32 stamp;
	PreviewCacheEntry() : width(0), height(0), maxWidth(0), maxHeight(0), hasImage(false), stamp(0) {}
};

vector<PreviewCacheEntry> gPreviewCache(kPreviewCacheEntries);
u32 gPreviewCacheStamp = 1;

int browserListRight()
{
	return MAX(40, int(screens::layoutX()) - kBrowserScrollbarGutter);
}

bool folderExists(const string& candidate)
{
	DIR* dir = opendir(candidate.c_str());
	if(NULL == dir) return false;
	closedir(dir);
	return true;
}

void saveBrowserState(const string& path, int pos)
{
	gLastBrowserPath = path;
	gLastBrowserPos = MAX(0, pos);
}

void saveBrowserState(const string& path, int pos, int cursor)
{
	saveBrowserState(path, pos);
	gLastBrowserCursor = MAX(0, cursor);
}

string defaultBrowserPath()
{
	if(!gLastBrowserPath.empty() && folderExists(gLastBrowserPath))
		return gLastBrowserPath;

	if(!settings::recent_book.empty()) {
		const string::size_type slash = settings::recent_book.find_last_of('/');
		if(string::npos != slash) {
			const string recentPath = settings::recent_book.substr(0, slash + 1);
			if(folderExists(recentPath)) return recentPath;
		}
	}

	if(folderExists("sdmc:/books/")) return "sdmc:/books/";
	if(folderExists(appBooksPath())) return appBooksPath();
	return sdRootPath();
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

vector<string> wrapPreviewText(const string& text, int width, u32 fontSize, u32 maxLines)
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

void drawWrappedText(scr_id scr, int x1, int y1, int width, const string& text, u32 fontSize, u32 maxLines)
{
	const vector<string> lines = wrapPreviewText(text, width, fontSize, maxLines);
	for(u32 i = 0; i < lines.size(); ++i) {
		const int baseline = y1 + fontSize - 1 + i * (fontSize + kPreviewTitleGap);
		renderer::printStr(eUtf8, scr, x1, baseline, lines[i], 0, 0, fontSize);
	}
}

struct jpeg_error_state
{
	jpeg_error_mgr pub;
	jmp_buf setjmp_buffer;
};

void jpegErrorExit(j_common_ptr cinfo)
{
	jpeg_error_state* err = (jpeg_error_state*)cinfo->err;
	longjmp(err->setjmp_buffer, 1);
}

void jpegInitSource(j_decompress_ptr cinfo) { (void)cinfo; }
boolean jpegFillInputBuffer(j_decompress_ptr cinfo)
{
	static const JOCTET eoi_buffer[2] = { 0xFF, JPEG_EOI };
	cinfo->src->next_input_byte = eoi_buffer;
	cinfo->src->bytes_in_buffer = 2;
	return TRUE;
}

void jpegSkipInputData(j_decompress_ptr cinfo, long num_bytes)
{
	if(num_bytes <= 0) return;
	while(num_bytes > (long)cinfo->src->bytes_in_buffer) {
		num_bytes -= (long)cinfo->src->bytes_in_buffer;
		jpegFillInputBuffer(cinfo);
	}
	cinfo->src->next_input_byte += (size_t)num_bytes;
	cinfo->src->bytes_in_buffer -= (size_t)num_bytes;
}

void jpegTermSource(j_decompress_ptr cinfo) { (void)cinfo; }

void jpegMemorySrc(j_decompress_ptr cinfo, const unsigned char* data, size_t len)
{
	if(cinfo->src == NULL)
		cinfo->src = (jpeg_source_mgr*)(*cinfo->mem->alloc_small)((j_common_ptr)cinfo, JPOOL_PERMANENT, sizeof(jpeg_source_mgr));

	cinfo->src->init_source = jpegInitSource;
	cinfo->src->fill_input_buffer = jpegFillInputBuffer;
	cinfo->src->skip_input_data = jpegSkipInputData;
	cinfo->src->resync_to_restart = jpeg_resync_to_restart;
	cinfo->src->term_source = jpegTermSource;
	cinfo->src->bytes_in_buffer = len;
	cinfo->src->next_input_byte = data;
}

u16 previewImagePixel(u8 r, u8 g, u8 b)
{
	if(!settings::nightMode() && !settings::lowLightMode()) return RGB15(r >> 3, g >> 3, b >> 3) | BIT(15);

	const unsigned lum = (r * 30u + g * 59u + b * 11u) / 100u;
	const u8 toneR = (settings::bgCol.R * (255u - lum) + settings::fCol.R * lum) / 255u;
	const u8 toneG = (settings::bgCol.G * (255u - lum) + settings::fCol.G * lum) / 255u;
	const u8 toneB = (settings::bgCol.B * (255u - lum) + settings::fCol.B * lum) / 255u;
	return RGB15(toneR, toneG, toneB) | BIT(15);
}

bool decodePreviewJpeg(const char* data, u32 size, u16 maxWidth, u16 maxHeight, vector<u16>& pixels, u16& width, u16& height)
{
	width = height = 0;
	jpeg_decompress_struct cinfo;
	jpeg_error_state jerr;
	cinfo.err = jpeg_std_error(&jerr.pub);
	jerr.pub.error_exit = jpegErrorExit;

	if(setjmp(jerr.setjmp_buffer)) {
		jpeg_destroy_decompress(&cinfo);
		return false;
	}

	jpeg_create_decompress(&cinfo);
	jpegMemorySrc(&cinfo, (const unsigned char*)data, size);
	jpeg_read_header(&cinfo, TRUE);
	cinfo.out_color_space = JCS_RGB;
	cinfo.do_fancy_upsampling = FALSE;
	cinfo.dct_method = JDCT_IFAST;
	cinfo.scale_num = 1;
	cinfo.scale_denom = 1;
	if(cinfo.image_width > maxWidth * 4u || cinfo.image_height > maxHeight * 4u) cinfo.scale_denom = 8;
	else if(cinfo.image_width > maxWidth * 2u || cinfo.image_height > maxHeight * 2u) cinfo.scale_denom = 4;
	else if(cinfo.image_width > maxWidth || cinfo.image_height > maxHeight) cinfo.scale_denom = 2;

	jpeg_start_decompress(&cinfo);

	const u32 srcWidth = cinfo.output_width;
	const u32 srcHeight = cinfo.output_height;
	const u32 comp = cinfo.output_components;
	if(0 == srcWidth || 0 == srcHeight || (comp != 1 && comp != 3)) {
		jpeg_finish_decompress(&cinfo);
		jpeg_destroy_decompress(&cinfo);
		return false;
	}

	vector<unsigned char> rgb(srcWidth * srcHeight * comp);
	while(cinfo.output_scanline < srcHeight) {
		JSAMPROW row = &rgb[cinfo.output_scanline * srcWidth * comp];
		jpeg_read_scanlines(&cinfo, &row, 1);
	}
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);

	u32 dstWidth = srcWidth;
	u32 dstHeight = srcHeight;
	if(dstWidth > maxWidth || dstHeight > maxHeight) {
		if(dstWidth * maxHeight > dstHeight * maxWidth) {
			dstHeight = (dstHeight * maxWidth) / dstWidth;
			dstWidth = maxWidth;
		}
		else {
			dstWidth = (dstWidth * maxHeight) / dstHeight;
			dstHeight = maxHeight;
		}
	}
	if(0 == dstWidth) dstWidth = 1;
	if(0 == dstHeight) dstHeight = 1;

	pixels.resize(dstWidth * dstHeight);
	for(u32 y = 0; y < dstHeight; ++y) {
		const u32 srcY = (y * srcHeight) / dstHeight;
		for(u32 x = 0; x < dstWidth; ++x) {
			const u32 srcX = (x * srcWidth) / dstWidth;
			const u32 index = (srcY * srcWidth + srcX) * comp;
			u8 r = rgb[index];
			u8 g = (comp == 3) ? rgb[index + 1] : r;
			u8 b = (comp == 3) ? rgb[index + 2] : r;
			pixels[y * dstWidth + x] = previewImagePixel(r, g, b);
		}
	}

	width = dstWidth;
	height = dstHeight;
	return true;
}

bool loadZipEntry(unzFile& zip, const string& file, char *&buf, u32& size)
{
	unz_file_info info;
	if(unzLocateFile(zip, file.c_str(), 0) != UNZ_OK) return false;
	if(unzOpenCurrentFile(zip) != UNZ_OK) return false;
	unzGetCurrentFileInfo(zip, &info, NULL, 0, NULL, 0, NULL, 0);
	size = info.uncompressed_size;
	buf = new (std::nothrow) char[size + 1u];
	if(buf == NULL) {
		unzCloseCurrentFile(zip);
		return false;
	}
	const int read = unzReadCurrentFile(zip, buf, size);
	unzCloseCurrentFile(zip);
	if(read < 0 || (u32)read != size) {
		delete[] buf;
		buf = NULL;
		return false;
	}
	buf[size] = '\0';
	return true;
}

string lowerPath(const string& path)
{
	string out = path;
	transform(out.begin(), out.end(), out.begin(), tolower);
	return out;
}

bool isPreviewJpegPath(const string& path)
{
	const string lower = lowerPath(path);
	return lower.length() > 4 &&
		(lower.compare(lower.length() - 4, 4, ".jpg") == 0 ||
		 lower.compare(lower.length() - 5, 5, ".jpeg") == 0);
}

u32 hashString(const string& text)
{
	u32 hash = 2166136261u;
	for(u32 i = 0; i < text.size(); ++i) {
		hash ^= (unsigned char)text[i];
		hash *= 16777619u;
	}
	return hash;
}

bool tryLoadPreviewCache(const string& file_name, u16 maxWidth, u16 maxHeight, vector<u16>& pixels, u16& width, u16& height, bool& hasImage)
{
	for(u32 i = 0; i < gPreviewCache.size(); ++i) {
		PreviewCacheEntry& entry = gPreviewCache[i];
		if(entry.file != file_name || entry.maxWidth != maxWidth || entry.maxHeight != maxHeight)
			continue;

		pixels = entry.pixels;
		width = entry.width;
		height = entry.height;
		hasImage = entry.hasImage;
		entry.stamp = gPreviewCacheStamp++;
		return true;
	}
	return false;
}

void storePreviewCache(const string& file_name, const vector<u16>& pixels, u16 width, u16 height, u16 maxWidth, u16 maxHeight, bool hasImage)
{
	if(gPreviewCache.empty()) return;

	PreviewCacheEntry* slot = &gPreviewCache[0];
	for(u32 i = 0; i < gPreviewCache.size(); ++i) {
		PreviewCacheEntry& entry = gPreviewCache[i];
		if(entry.file.empty()) {
			slot = &entry;
			break;
		}
		if(entry.stamp < slot->stamp) slot = &entry;
	}

	slot->file = file_name;
	slot->pixels = pixels;
	slot->width = width;
	slot->height = height;
	slot->maxWidth = maxWidth;
	slot->maxHeight = maxHeight;
	slot->hasImage = hasImage;
	slot->stamp = gPreviewCacheStamp++;
}

bool loadPreviewImage(const string& epubFile, u16 maxWidth, u16 maxHeight, vector<u16>& pixels, u16& width, u16& height)
{
	pixels.clear();
	width = height = 0;

	unzFile zip = unzOpen(epubFile.c_str());
	if(zip == NULL) return false;

	vector<string> candidates;
	candidates.reserve(kPreviewCandidatePool);
	u32 seed = hashString(epubFile) ^ (u32)time(NULL);
	u32 eligible = 0;
	if(unzGoToFirstFile(zip) != UNZ_OK) {
		unzClose(zip);
		return false;
	}

	do {
		unz_file_info info;
		char name[256];
		if(unzGetCurrentFileInfo(zip, &info, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK)
			continue;
		if(info.uncompressed_size > kPreviewMaxEntryBytes) continue;
		const string path(name);
		if(path.empty() || path[path.size() - 1] == '/') continue;
		if(!isPreviewJpegPath(path)) continue;

		++eligible;
		if(candidates.size() < kPreviewCandidatePool) candidates.push_back(path);
		else {
			seed = seed * 1664525u + 1013904223u;
			const u32 slot = seed % eligible;
			if(slot < kPreviewCandidatePool) candidates[slot] = path;
		}
	} while(unzGoToNextFile(zip) == UNZ_OK);

	if(candidates.empty()) {
		unzClose(zip);
		return false;
	}

	const u32 first = seed % candidates.size();
	const u32 attempts = MIN((u32)candidates.size(), 3u);
	for(u32 i = 0; i < attempts; ++i) {
		const string& candidate = candidates[(first + i) % candidates.size()];
		char* buf = NULL;
		u32 size = 0;
		if(!loadZipEntry(zip, candidate, buf, size) || NULL == buf) continue;
		const bool ok =
			size > 2u &&
			(u8)buf[0] == 0xFF &&
			(u8)buf[1] == 0xD8 &&
			decodePreviewJpeg(buf, size, maxWidth, maxHeight, pixels, width, height);
		delete[] buf;
		if(ok && width >= 24u && height >= 24u) {
			unzClose(zip);
			return true;
		}
	}

	unzClose(zip);
	return false;
}

void drawPreviewIcon(int x1, int y1, int x2, int y2)
{
	const int boxW = x2 - x1;
	const int boxH = y2 - y1;
	const int iconW = MIN(68, boxW - 16);
	const int iconH = MIN(90, boxH - 20);
	const int left = x1 + (boxW - iconW) / 2;
	const int top = y1 + (boxH - iconH) / 2;
	const int right = left + iconW;
	const int bottom = top + iconH;

	renderer::rect(left, top, right, bottom, top_scr);
	for(int y = top + 2; y < bottom - 1; ++y)
		renderer::putPixel(top_scr, left + 10, y, Blend(96));
	renderer::printStr(eUtf8, top_scr, left + 16, top + 28, "EPUB", 0, 0, 12);
	renderer::printStr(eUtf8, top_scr, left + 10, bottom - 10, "Preview", 0, 0, 10);
}

} // namespace

bool comp(entry e1, entry e2)
{
	if(e1.first == e2.first) return 0 > strcmp(e1.second.c_str(), e2.second.c_str());
	else return e1.first > e2.first;
}

void file_browser :: cd()
{
	pos = 0;
	cursor = 0;
	flist.clear();

	DIR* dir = opendir(path.c_str());
	struct dirent* ent;
	if(!dir) bsod(("file_browser.cd:cannot open "  + path).c_str());

	if(path != sdRootPath()) flist.push_back(entry(folder, ".."));

	while ((ent = readdir(dir)) != NULL) {
		if(strcmp(".", ent->d_name) == 0 || strcmp("..", ent->d_name) == 0)
			continue;

		if ((ent->d_type == DT_DIR)) {
			flist.push_back(entry(folder, ent->d_name));
		}
		else {
			string ext(extention(ent->d_name));
			if(ext == "epub") flist.push_back(entry(file, ent->d_name));
		}
	}
	closedir(dir);
	sort (flist.begin(), flist.end(), comp);
}

void file_browser :: clampCursor()
{
	if(flist.empty()) {
		pos = 0;
		cursor = 0;
		return;
	}

	clamp(cursor, 0, int(flist.size()) - 1);
	const int visible = MAX(1, int(num));
	const int maxPos = MAX(0, int(flist.size()) - visible);
	if(cursor < pos) pos = cursor;
	else if(cursor >= pos + visible) pos = cursor - visible + 1;
	clamp(pos, 0, maxPos);
}

void file_browser :: resetPreview()
{
	vector<u16>().swap(previewPixels);
	previewFile.clear();
	previewWidth = previewHeight = 0;
	previewHasImage = false;
	previewPending = false;
	previewDelayFrames = 0;
	promptActive = false;
}

void file_browser :: showPreview(const string& file_name)
{
	previewFile = file_name;
	vector<u16>().swap(previewPixels);
	previewWidth = previewHeight = 0;
	previewHasImage = false;
	const int width = screens::layoutX();
	const int height = screens::layoutY();
	const int titleY = kPreviewMargin + 2;
	const int titleHeight = kPreviewTitleLines * kPreviewTitleFont + (kPreviewTitleLines - 1) * kPreviewTitleGap;
	const int frameX1 = kPreviewMargin + 4;
	const int frameX2 = width - kPreviewMargin - 4;
	const int frameY1 = titleY + titleHeight + kPreviewImageGap;
	const int frameY2 = height - kPreviewMargin - 4;
	const int innerWidth = frameX2 - frameX1 - 3;
	const int innerHeight = frameY2 - frameY1 - 3;
	if(innerWidth > 24 && innerHeight > 24 &&
		!tryLoadPreviewCache(file_name, innerWidth, innerHeight, previewPixels, previewWidth, previewHeight, previewHasImage)) {
		previewHasImage = loadPreviewImage(file_name, innerWidth, innerHeight, previewPixels, previewWidth, previewHeight);
		storePreviewCache(file_name, previewPixels, previewWidth, previewHeight, innerWidth, innerHeight, previewHasImage);
	}
	previewPending = false;
	previewDelayFrames = 0;
	promptActive = false;
}

bool file_browser :: tickPreview()
{
	if(!previewPending || previewFile.empty()) return false;
	if(previewDelayFrames > 0) {
		--previewDelayFrames;
		return false;
	}
	showPreview(previewFile);
	return true;
}

void file_browser :: syncPreviewToCursor(bool force)
{
	if(flist.empty()) {
		resetPreview();
		return;
	}

	clampCursor();
	const entry& current = flist[cursor];
	if(folder == current.first) {
		resetPreview();
		return;
	}

	const string file_name = path + current.second;
	if(!force && previewFile == file_name) return;
	if(force) {
		showPreview(file_name);
		return;
	}

	previewFile = file_name;
	vector<u16>().swap(previewPixels);
	previewWidth = previewHeight = 0;
	previewHasImage = false;
	previewPending = true;
	previewDelayFrames = kPreviewWarmupFrames;
	promptActive = false;
}

void file_browser :: activateCursor()
{
	if(flist.empty()) return;
	clampCursor();
	const entry& current = flist[cursor];
	if(folder == current.first) {
		resetPreview();
		if(".." != current.second) path += current.second + '/';
		else path.erase(path.find_last_of('/', path.size() - 2) + 1);
		cd();
	}
	else {
		syncPreviewToCursor(true);
		promptActive = !previewFile.empty();
	}
}

void file_browser :: drawPreview()
{
	renderer::clearScreens(settings::bgCol, top_scr);

	const int width = screens::layoutX();
	const int height = screens::layoutY();
	const int titleX = kPreviewMargin;
	const int titleY = kPreviewMargin + 2;
	const int titleWidth = width - 2 * kPreviewMargin;
	const int titleHeight = kPreviewTitleLines * kPreviewTitleFont + (kPreviewTitleLines - 1) * kPreviewTitleGap;
	const int imageY1 = titleY + titleHeight + kPreviewImageGap;
	const int imageY2 = height - kPreviewMargin;

	renderer::rect(kPreviewMargin, kPreviewMargin, width - kPreviewMargin, height - kPreviewMargin, top_scr);

	if(previewFile.empty()) {
		drawWrappedText(top_scr, titleX, titleY, titleWidth, "Select an EPUB to preview", kPreviewTitleFont, 2);
		drawPreviewIcon(kPreviewMargin + 4, imageY1, width - kPreviewMargin - 4, imageY2 - 4);
		return;
	}

	drawWrappedText(top_scr, titleX, titleY, titleWidth, noPath(previewFile), kPreviewTitleFont, kPreviewTitleLines);
	renderer::rect(kPreviewMargin + 4, imageY1, width - kPreviewMargin - 4, imageY2 - 4, top_scr);

	if(previewHasImage && !previewPixels.empty()) {
		const int innerX1 = kPreviewMargin + 6;
		const int innerY1 = imageY1 + 2;
		const int innerX2 = width - kPreviewMargin - 6;
		const int innerY2 = imageY2 - 6;
		const int boxW = innerX2 - innerX1 + 1;
		const int boxH = innerY2 - innerY1 + 1;
		const int drawX = innerX1 + (boxW - previewWidth) / 2;
		const int drawY = innerY1 + (boxH - previewHeight) / 2;
		renderer::drawImageSlice(top_scr, drawX, drawY, previewPixels, previewWidth, previewHeight, 0, previewHeight);
	}
	else drawPreviewIcon(kPreviewMargin + 4, imageY1, width - kPreviewMargin - 4, imageY2 - 4);
}

void file_browser :: drawPrompt()
{
	const int width = screens::layoutX();
	const int height = screens::layoutY();
	const int boxX1 = 12;
	const int boxX2 = width - 12;
	const int boxY2 = height - 10;
	const int boxY1 = boxY2 - 56;

	renderer::fillRect(boxX1, boxY1, boxX2, boxY2, settings::bgCol, bottom_scr);
	renderer::rect(boxX1, boxY1, boxX2, boxY2, bottom_scr);

	const string prompt("Open this file?");
	const int promptWidth = renderer::strWidth(eUtf8, prompt, 0, 0, kPromptFont);
	renderer::printStr(eUtf8, bottom_scr, boxX1 + MAX(4, (boxX2 - boxX1 - promptWidth) / 2), boxY1 + 14, prompt, 0, 0, kPromptFont);

	const int buttonY1 = boxY1 + 22;
	const int buttonY2 = boxY2 - 8;
	const int gap = 6;
	const int buttonWidth = (boxX2 - boxX1 - gap - 12) / 2;
	promptKeep = button("Keep", boxX1 + 6, buttonY1, boxX1 + 6 + buttonWidth, buttonY2, kPromptFont);
	promptOpen = button("Open", boxX2 - 6 - buttonWidth, buttonY1, boxX2 - 6, buttonY2, kPromptFont);
	promptKeep.draw();
	promptOpen.draw();
}

u16 file_browser :: draw()
{
	buttons.clear();
	u16 pen = 0;
	const int listRight = browserListRight();
	button header(path, 0, 0, listRight, buttonFontSize);
	header.enableAutoFit(12);
	renderer::clearScreens(settings::bgCol, bottom_scr);
	header.draw();
	u16 height = header.height();
	u16 i = pos;
	for( ; i < flist.size() && pen <= screens::layoutY() - 2*height; i++) {
		pen += height;
		if(int(i) == cursor)
			renderer::fillRect(0, pen, listRight, pen + height, Blend(72), bottom_scr);
		button item(flist[i].second, 0, pen, listRight, pen + height, buttonFontSize);
		item.enableAutoFit(12);
		buttons.push_back(fbutton(flist[i].first, item));
		buttons.back().second.draw();
	}
	return i - pos;
}

void file_browser :: upd()
{
	clampCursor();
	syncPreviewToCursor();
	saveBrowserState(path, pos, cursor);
	num = draw();
	if (num < flist.size() && flist.size()) sbar.draw(float(pos) / (flist.size() - num), float(buttons.size())/flist.size());
	if(promptActive) drawPrompt();
	drawPreview();
}

string file_browser :: run()
{
	setBacklightMode(blOverlay);
	resetPreview();

	path = defaultBrowserPath();

	cd();
	if(path == gLastBrowserPath) {
		pos = gLastBrowserPos;
		cursor = gLastBrowserCursor;
	}
	clampCursor();
	upd();

	while(pumpPowerManagement()){
		swiWaitForVBlank();
		if(!promptActive && tickPreview()) drawPreview();
		scanKeys();
		int down = keysDown();
		if(!down) continue;

		if(promptActive) {
			if(down & rKey(rRight)) {
				return previewFile;
			}
			if(down & rKey(rLeft)) {
				promptActive = false;
				upd();
				continue;
			}
			if(down & KEY_TOUCH) {
				if(promptOpen.touched()) return previewFile;
				if(promptKeep.touched()) {
					promptActive = false;
					upd();
				}
				else {
					promptActive = false;
					upd();
				}
			}
			continue;
		}

		if(down & rKey(rLeft)) {
			if(path != sdRootPath()) {
				resetPreview();
				path.erase(path.find_last_of('/', path.size() - 2) + 1);
				cd();
				upd();
				continue;
			}
			saveBrowserState(path, pos, cursor);
			resetPreview();
			return string();
		}
		if(down & rKey(rUp)){
			if(flist.empty() || 0 == cursor) continue;
			--cursor;
			upd();
		}
		else if(down & rKey(rDown)){
			if(flist.empty() || cursor >= int(flist.size()) - 1) continue;
			++cursor;
			upd();
		}
		else if(down & rKey(rRight)) {
			activateCursor();
			upd();
		}
		else if(down & KEY_TOUCH) {
			for(u16 i = 0; i < buttons.size(); i++) {
				if(!buttons[i].second.touched()) continue;
				cursor = pos + i;
				if(folder == buttons[i].first) {
					activateCursor();
				}
				else {
					activateCursor();
				}
				upd();
				break;
			}
		}
	}

	saveBrowserState(path, pos, cursor);
	resetPreview();
	return string();
}

string extention(string name)
{
	string ext (name.substr(name.find_last_of('.') + 1));
	transform (ext.begin(), ext.end(), ext.begin(), tolower);
	return ext;
}

string noExt(string name)
{
	unsigned int found = name.find_last_of('/');
	string n;
	if (found == string::npos)
		 n = name.substr(0, name.find_last_of('.'));
	else n = name.substr(found + 1, name.find_last_of('.') - found - 1);
	transform (n.begin(), n.end(), n.begin(), tolower);
	return n;
}

string noPath(string name)
{
	unsigned int found = name.find_last_of('/');
	string n;
	if (found == string::npos)
		 n = name;
	else n = name.substr(found + 1);
	return n;
}
