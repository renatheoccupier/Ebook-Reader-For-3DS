#include "file_browser.h"
#include "renderer.h"
#include "screens.h"
#include "controls.h"
#include "pugixml.h"
#include "settings.h"
#include "utf8.h"
#include "unzip.h"
#include <algorithm>
#include <map>
#include <new>
#include <setjmp.h>
#include <ctype.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

extern "C" {
#include "jpeglib.h"
}

namespace
{

const int kBrowserScrollbarGutter = 8;
const int kPreviewMargin = 8;
const int kPreviewTitleFont = 10;
const int kPreviewTitleLines = 3;
const int kPreviewTitleGap = 2;
const int kPreviewImageGap = 8;
const int kPromptFont = 12;
const int kPreviewWarmupFrames = 45;
const u32 kPreviewMaxEntryBytes = 768u * 1024u;
const u32 kPreviewCacheEntries = 12u;
const u32 kPreviewPathCacheEntries = 16u;
const bool kPreviewCoversEnabled = true;
const int kScrollbarFlashFrames = 30;
button gBrowserHomeButton;

string gLastBrowserPath;
int gLastBrowserPos = 0;
int gLastBrowserCursor = 0;
string gCachedDirPath;
vector<entry> gCachedDirEntries;

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

struct PreviewPathCacheEntry
{
	string file;
	string imagePath;
	u32 stamp;
	PreviewPathCacheEntry() : stamp(0) {}
};

vector<PreviewPathCacheEntry> gPreviewPathCache(kPreviewPathCacheEntries);
u32 gPreviewPathCacheStamp = 1;

bool topPreviewPortrait(int width)
{
	return width < 300;
}

bool breakablePreviewChar(char c)
{
	return c == ' ' || c == '-' || c == '_' || c == '/' || c == ':';
}

void previewFrameRect(int width, int bottomLimit, int& x1, int& y1, int& x2, int& y2)
{
	x1 = 10;
	x2 = width - 10;
	y1 = topPreviewPortrait(width) ? 34 : 32;
	y2 = bottomLimit;
}

int browserListRight(bool reserveScrollbar)
{
	return MAX(80, int(screens::layoutX()) - (reserveScrollbar ? kBrowserScrollbarGutter + 18 : 12));
}

bool browserNeedsScrollbar(u32 totalEntries)
{
	const int rowHeight = buttonFontSize + 6;
	const int visibleRows = MAX(1, (int(screens::layoutY()) - 104) / rowHeight);
	return totalEntries > u32(visibleRows);
}

bool statPath(const string& path, struct stat& st)
{
	return 0 == stat(path.c_str(), &st);
}

int classifyEntry(const string& basePath, const dirent* ent)
{
	if(ent == NULL) return -1;
	if(ent->d_type == DT_DIR) return folder;
	if(ent->d_type == DT_REG) return file;
	if(ent->d_type != DT_UNKNOWN) return -1;

	struct stat st;
	if(!statPath(basePath + ent->d_name, st)) return -1;
	if(S_ISDIR(st.st_mode)) return folder;
	if(S_ISREG(st.st_mode)) return file;
	return -1;
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
		while(breakPos > start && !breakablePreviewChar(text[breakPos - 1])) --breakPos;
		u32 lineEnd = hardEnd;
		u32 nextStart = hardEnd;
		if(breakPos > start) {
			lineEnd = trimSpaces(text, start, breakPos);
			nextStart = skipSpaces(text, breakPos);
		}
		if(lineEnd <= start) {
			lines.push_back(ellipsizedSlice(text, start, width, fontSize));
			return lines;
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

string previewLabel(const entry& item)
{
	if(item.first == file) return noExt(item.second);
	return item.second;
}

string loadingDots(u32 tick)
{
	string dots;
	const u32 count = (tick / 12u) % 4u;
	for(u32 i = 0; i < count; ++i) dots += '.';
	return dots;
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
	vector<unsigned char> row(srcWidth * comp);
	u32 nextDstY = 0;
	while(cinfo.output_scanline < srcHeight) {
		JSAMPROW rowPtr = &row[0];
		jpeg_read_scanlines(&cinfo, &rowPtr, 1);
		const u32 srcY = cinfo.output_scanline - 1u;
		while(nextDstY < dstHeight && ((nextDstY * srcHeight) / dstHeight) <= srcY) {
			for(u32 x = 0; x < dstWidth; ++x) {
				const u32 srcX = (x * srcWidth) / dstWidth;
				const u32 index = srcX * comp;
				const u8 r = row[index];
				const u8 g = (comp == 3) ? row[index + 1] : r;
				const u8 b = (comp == 3) ? row[index + 2] : r;
				pixels[nextDstY * dstWidth + x] = previewImagePixel(r, g, b);
			}
			++nextDstY;
		}
	}
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	while(nextDstY < dstHeight) {
		const u32 copyFrom = (nextDstY == 0) ? 0 : (nextDstY - 1u);
		for(u32 x = 0; x < dstWidth; ++x)
			pixels[nextDstY * dstWidth + x] = pixels[copyFrom * dstWidth + x];
		++nextDstY;
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

bool loadXmlFromZip(unzFile& zip, const string& file, pugi::xml_document& doc)
{
	char* buf = NULL;
	u32 size = 0;
	if(!loadZipEntry(zip, file, buf, size) || NULL == buf) return false;
	const pugi::xml_parse_result result = doc.load_buffer(buf, size);
	delete[] buf;
	return result.status == pugi::status_ok;
}

string stripFragment(const string& path)
{
	const string::size_type pos = path.find('#');
	return (string::npos == pos) ? path : path.substr(0, pos);
}

string dirName(const string& path)
{
	const string::size_type pos = path.find_last_of('/');
	return (string::npos == pos) ? string() : path.substr(0, pos + 1);
}

bool hasUriScheme(const string& path)
{
	const string::size_type colon = path.find(':');
	const string::size_type slash = path.find('/');
	return string::npos != colon && (string::npos == slash || colon < slash);
}

string normalizeZipPath(const string& base, const string& raw_path)
{
	string path = stripFragment(raw_path);
	if(path.empty() || hasUriScheme(path)) return string();
	if(path[0] != '/') path = base + path;
	else path.erase(0, 1);

	vector<string> parts;
	string::size_type start = 0;
	while(start <= path.length()) {
		string::size_type end = path.find('/', start);
		string part = (string::npos == end) ? path.substr(start) : path.substr(start, end - start);
		if(part == "..") {
			if(!parts.empty()) parts.pop_back();
		}
		else if(!part.empty() && part != ".") parts.push_back(part);
		if(string::npos == end) break;
		start = end + 1;
	}

	string normalized;
	for(u32 i = 0; i < parts.size(); ++i) {
		if(i) normalized += '/';
		normalized += parts[i];
	}
	return normalized;
}

string lowerPath(const string& path)
{
	string out = path;
	for(u32 i = 0; i < out.size(); ++i)
		out[i] = tolower((unsigned char)out[i]);
	return out;
}

bool isPreviewJpegPath(const string& path)
{
	const string lower = lowerPath(path);
	return lower.length() > 4 &&
		(lower.compare(lower.length() - 4, 4, ".jpg") == 0 ||
			 lower.compare(lower.length() - 5, 5, ".jpeg") == 0);
}

bool containsWord(const string& words, const string& word)
{
	string::size_type start = 0;
	while(start <= words.length()) {
		const string::size_type end = words.find(' ', start);
		const string item = (string::npos == end) ? words.substr(start) : words.substr(start, end - start);
		if(item == word) return true;
		if(string::npos == end) break;
		start = end + 1;
	}
	return false;
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

bool copyCachedPreviewLocal(const string& file_name, vector<u16>& pixels, u16& width, u16& height)
{
	const PreviewCacheEntry* best = NULL;
	for(u32 i = 0; i < gPreviewCache.size(); ++i) {
		const PreviewCacheEntry& entry = gPreviewCache[i];
		if(entry.file != file_name || !entry.hasImage || entry.pixels.empty())
			continue;
		if(best == NULL || entry.width * entry.height > best->width * best->height)
			best = &entry;
	}
	if(best == NULL) return false;

	pixels = best->pixels;
	width = best->width;
	height = best->height;
	return true;
}

bool tryLoadPreviewPathCache(const string& file_name, string& imagePath)
{
	for(u32 i = 0; i < gPreviewPathCache.size(); ++i) {
		PreviewPathCacheEntry& entry = gPreviewPathCache[i];
		if(entry.file != file_name) continue;
		imagePath = entry.imagePath;
		entry.stamp = gPreviewPathCacheStamp++;
		return true;
	}
	return false;
}

void storePreviewPathCache(const string& file_name, const string& imagePath)
{
	if(gPreviewPathCache.empty()) return;

	PreviewPathCacheEntry* slot = &gPreviewPathCache[0];
	for(u32 i = 0; i < gPreviewPathCache.size(); ++i) {
		PreviewPathCacheEntry& entry = gPreviewPathCache[i];
		if(entry.file.empty()) {
			slot = &entry;
			break;
		}
		if(entry.stamp < slot->stamp) slot = &entry;
	}

	slot->file = file_name;
	slot->imagePath = imagePath;
	slot->stamp = gPreviewPathCacheStamp++;
}

bool findPreviewCoverInOpf(unzFile& zip, const string& opfPath, string& imagePath)
{
	pugi::xml_document doc;
	if(!loadXmlFromZip(zip, opfPath, doc)) return false;

	const string opfDir = dirName(opfPath);
	std::map<string, string> manifestFiles;
	string coverId;
	string namedCandidate;
	for(pugi::xml_node meta = doc.child("package").child("metadata").first_child(); meta; meta = meta.next_sibling()) {
		if(strcmp(meta.name(), "meta")) continue;
		const string name = lowerPath(meta.attribute("name").value());
		if(name == "cover") {
			const string content = meta.attribute("content").value();
			if(!content.empty()) coverId = content;
		}
	}

	for(pugi::xml_node item = doc.child("package").child("manifest").first_child(); item; item = item.next_sibling()) {
		if(strcmp(item.name(), "item")) continue;
		const string id = item.attribute("id").value();
		const string href = normalizeZipPath(opfDir, item.attribute("href").value());
		if(href.empty()) continue;
		manifestFiles[id] = href;

		const string mediaType = lowerPath(item.attribute("media-type").value());
		if(mediaType != "image/jpeg" && mediaType != "image/jpg" && !isPreviewJpegPath(href))
			continue;

		const string properties = lowerPath(item.attribute("properties").value());
		if(containsWord(properties, "cover-image")) {
			imagePath = href;
			return true;
		}

		const string lowerId = lowerPath(id);
		const string lowerHref = lowerPath(href);
		if(namedCandidate.empty() &&
			(lowerId.find("cover") != string::npos ||
			 lowerHref.find("cover") != string::npos ||
			 lowerHref.find("front") != string::npos))
			namedCandidate = href;
	}

	if(!coverId.empty()) {
		std::map<string, string>::const_iterator it = manifestFiles.find(coverId);
		if(it != manifestFiles.end() && isPreviewJpegPath(it->second)) {
			imagePath = it->second;
			return true;
		}
	}

	for(pugi::xml_node ref = doc.child("package").child("guide").first_child(); ref; ref = ref.next_sibling()) {
		if(strcmp(ref.name(), "reference")) continue;
		const string type = lowerPath(ref.attribute("type").value());
		if(type.find("cover") == string::npos) continue;
		const string href = normalizeZipPath(opfDir, ref.attribute("href").value());
		if(isPreviewJpegPath(href)) {
			imagePath = href;
			return true;
		}
	}

	if(!namedCandidate.empty()) {
		imagePath = namedCandidate;
		return true;
	}
	return false;
}

bool findPreviewCoverByScan(unzFile& zip, string& imagePath, const string& skipPath = string())
{
	imagePath.clear();
	if(unzGoToFirstFile(zip) != UNZ_OK) return false;

	int bestScore = -1;
	u32 bestSize = 0xFFFFFFFFu;
	do {
		unz_file_info info;
		char name[256];
		if(unzGetCurrentFileInfo(zip, &info, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK)
			continue;
		if(info.uncompressed_size > kPreviewMaxEntryBytes) continue;

		const string path(name);
		if(path.empty() || path[path.size() - 1] == '/' || path == skipPath) continue;
		if(!isPreviewJpegPath(path)) continue;

		const string lower = lowerPath(path);
		int score = 0;
		if(lower.find("cover") != string::npos) score += 8;
		if(lower.find("front") != string::npos) score += 4;
		if(lower.find("thumb") != string::npos) score += 2;
		if(lower.find("image") != string::npos) score += 1;
		if(score > bestScore || (score == bestScore && info.uncompressed_size < bestSize)) {
			imagePath = path;
			bestScore = score;
			bestSize = info.uncompressed_size;
			if(score >= 8) return true;
		}
	} while(unzGoToNextFile(zip) == UNZ_OK);

	return !imagePath.empty();
}

bool resolvePreviewImagePath(unzFile& zip, const string& epubFile, string& imagePath)
{
	if(tryLoadPreviewPathCache(epubFile, imagePath))
		return !imagePath.empty();

	imagePath.clear();
	pugi::xml_document containerDoc;
	if(loadXmlFromZip(zip, "META-INF/container.xml", containerDoc)) {
		const string opfPath = containerDoc.child("container").child("rootfiles").child("rootfile").attribute("full-path").value();
		if(!opfPath.empty())
			findPreviewCoverInOpf(zip, opfPath, imagePath);
	}

	if(imagePath.empty())
		findPreviewCoverByScan(zip, imagePath);

	storePreviewPathCache(epubFile, imagePath);
	return !imagePath.empty();
}

bool decodePreviewEntry(unzFile& zip, const string& path, u16 maxWidth, u16 maxHeight, vector<u16>& pixels, u16& width, u16& height)
{
	char* buf = NULL;
	u32 size = 0;
	if(!loadZipEntry(zip, path, buf, size) || NULL == buf) return false;
	const bool ok =
		size > 2u &&
		(u8)buf[0] == 0xFF &&
		(u8)buf[1] == 0xD8 &&
		decodePreviewJpeg(buf, size, maxWidth, maxHeight, pixels, width, height);
	delete[] buf;
	return ok && width >= 24u && height >= 24u;
}

bool loadPreviewImage(const string& epubFile, u16 maxWidth, u16 maxHeight, vector<u16>& pixels, u16& width, u16& height)
{
	pixels.clear();
	width = height = 0;

	unzFile zip = unzOpen(epubFile.c_str());
	if(zip == NULL) return false;

	string candidate;
	if(!resolvePreviewImagePath(zip, epubFile, candidate) || candidate.empty()) {
		unzClose(zip);
		return false;
	}

	if(decodePreviewEntry(zip, candidate, maxWidth, maxHeight, pixels, width, height)) {
		unzClose(zip);
		return true;
	}

	const string failedCandidate = candidate;
	if(findPreviewCoverByScan(zip, candidate, failedCandidate) && decodePreviewEntry(zip, candidate, maxWidth, maxHeight, pixels, width, height)) {
		storePreviewPathCache(epubFile, candidate);
		unzClose(zip);
		return true;
	}

	storePreviewPathCache(epubFile, string());
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

void drawFolderIcon(int x1, int y1, int x2, int y2)
{
	const int boxW = x2 - x1;
	const int boxH = y2 - y1;
	const int folderW = MIN(84, boxW - 20);
	const int folderH = MIN(56, boxH - 30);
	const int left = x1 + (boxW - folderW) / 2;
	const int top = y1 + (boxH - folderH) / 2 + 8;
	const int tabW = folderW / 3;
	const int tabH = 12;

	renderer::fillRect(left, top + tabH, left + folderW, top + folderH, Blend(56), top_scr);
	renderer::rect(left, top + tabH, left + folderW, top + folderH, top_scr);
	renderer::fillRect(left + 6, top, left + 6 + tabW, top + tabH + 6, Blend(80), top_scr);
	renderer::rect(left + 6, top, left + 6 + tabW, top + tabH + 6, top_scr);
	renderer::printStr(eUtf8, top_scr, left + 16, top + folderH + 20, "Folder", 0, 0, 12);
}

} // namespace

bool copyCachedPreview(const string& file_name, vector<u16>& pixels, u16& width, u16& height)
{
	return copyCachedPreviewLocal(file_name, pixels, width, height);
}

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
	if(path == gCachedDirPath) {
		flist = gCachedDirEntries;
		return;
	}

	DIR* dir = opendir(path.c_str());
	struct dirent* ent;
	if(!dir) bsod(("file_browser.cd:cannot open "  + path).c_str());

	if(path != sdRootPath()) flist.push_back(entry(folder, ".."));

	while ((ent = readdir(dir)) != NULL) {
		if(strcmp(".", ent->d_name) == 0 || strcmp("..", ent->d_name) == 0)
			continue;

		const int kind = classifyEntry(path, ent);
		if(folder == kind) {
			flist.push_back(entry(folder, ent->d_name));
		}
		else if(file == kind) {
			string ext(extention(ent->d_name));
			if(ext == "epub") flist.push_back(entry(file, ent->d_name));
		}
	}
	closedir(dir);
	sort (flist.begin(), flist.end(), comp);
	gCachedDirPath = path;
	gCachedDirEntries = flist;
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
	previewAnimTick = 0;
	previewTitleMarquee = false;
	promptActive = false;
}

void file_browser :: showPreview(const string& file_name)
{
	previewFile = file_name;
	vector<u16>().swap(previewPixels);
	previewWidth = previewHeight = 0;
	previewHasImage = false;
	if(kPreviewCoversEnabled) {
		const int width = renderer::screenTextWidth(top_scr) - 1;
		int frameX1, frameY1, frameX2, frameY2;
		previewFrameRect(width, renderer::screenTextHeight(top_scr) - 8, frameX1, frameY1, frameX2, frameY2);
		const int innerWidth = frameX2 - frameX1 - 2;
		const int innerHeight = frameY2 - frameY1 - 2;
		if(innerWidth > 24 && innerHeight > 24 &&
			!tryLoadPreviewCache(file_name, innerWidth, innerHeight, previewPixels, previewWidth, previewHeight, previewHasImage)) {
			previewHasImage = loadPreviewImage(file_name, innerWidth, innerHeight, previewPixels, previewWidth, previewHeight);
			storePreviewCache(file_name, previewPixels, previewWidth, previewHeight, innerWidth, innerHeight, previewHasImage);
		}
	}
	previewPending = false;
	previewDelayFrames = 0;
	previewAnimTick = 0;
	promptActive = false;
}

bool file_browser :: tickPreview()
{
	if(!kPreviewCoversEnabled) return false;
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
		previewFile.clear();
		vector<u16>().swap(previewPixels);
		previewWidth = previewHeight = 0;
		previewHasImage = false;
		previewPending = false;
		previewDelayFrames = 0;
		previewTitleMarquee = false;
		promptActive = false;
		return;
	}

	const string file_name = path + current.second;
	if(force || !kPreviewCoversEnabled) {
		showPreview(file_name);
		return;
	}

	if(previewFile == file_name) return;
	previewFile = file_name;
	vector<u16>().swap(previewPixels);
	previewWidth = previewHeight = 0;
	previewHasImage = false;
	previewPending = true;
	previewDelayFrames = kPreviewWarmupFrames;
	previewAnimTick = 0;
	previewTitleMarquee = false;
	promptActive = false;
}

string file_browser :: activateCursor()
{
	if(flist.empty()) return string();
	clampCursor();
	const entry& current = flist[cursor];
	if(folder == current.first) {
		resetPreview();
		if(".." != current.second) path += current.second + '/';
		else path.erase(path.find_last_of('/', path.size() - 2) + 1);
		cd();
		return string();
	}

	return path + current.second;
}

void file_browser :: drawPreview()
{
	renderer::clearScreens(settings::bgCol, top_scr);
	previewTitleMarquee = false;

	const int width = renderer::screenTextWidth(top_scr) - 1;
	const int height = renderer::screenTextHeight(top_scr) - 1;
	const int leftPad = 10;
	const int rightPad = width - 10;
	const bool portrait = topPreviewPortrait(width);
	int previewX1, previewY1, previewX2, previewY2;
	previewFrameRect(width, height - 8, previewX1, previewY1, previewX2, previewY2);
	const int titleY1 = 6;
	const int titleY2 = portrait ? 26 : 24;
	renderer::fillRect(leftPad, titleY1, rightPad, titleY2, Blend(22), top_scr);
	renderer::rect(leftPad, titleY1, rightPad, titleY2, top_scr);
	renderer::fillRect(previewX1, previewY1, previewX2, previewY2, Blend(16), top_scr);
	renderer::rect(previewX1, previewY1, previewX2, previewY2, top_scr);

	if(flist.empty()) {
		drawWrappedText(top_scr, leftPad + 8, titleY1 + (portrait ? 12 : 10), rightPad - leftPad - 16,
			"No EPUB files in this folder", topPreviewPortrait(width) ? 10 : 11, topPreviewPortrait(width) ? 1 : 2);
		drawPreviewIcon(previewX1 + 6, previewY1 + 6, previewX2 - 6, previewY2 - 34);
		drawWrappedText(top_scr, previewX1 + 14, previewY2 - 34, previewX2 - previewX1 - 28, "Copy books into sdmc:/books/ or press Left to go back.", 10, 2);
		return;
	}

	const entry& current = flist[cursor];
	previewTitleMarquee = renderer::drawMarqueeText(top_scr, leftPad, titleY1, rightPad, titleY2, previewLabel(current), portrait ? 10 : 11, previewAnimTick, 8);

	if(current.first == folder) {
		drawFolderIcon(previewX1 + 8, previewY1 + 8, previewX2 - 8, previewY2 - 34);
		drawWrappedText(top_scr, previewX1 + 12, previewY2 - 28, previewX2 - previewX1 - 24, "Open folder", 10, 1);
	}
	else {
		if(previewPending && previewFile == path + current.second) {
			drawPreviewIcon(previewX1 + 4, previewY1 + 4, previewX2 - 4, previewY2 - 4);
			drawWrappedText(top_scr, previewX1 + 12, previewY2 - 28, previewX2 - previewX1 - 24,
				"Loading preview" + loadingDots(previewAnimTick), 10, 1);
		}
		else if(previewHasImage && !previewPixels.empty()) {
			const int innerX1 = previewX1 + 1;
			const int innerY1 = previewY1 + 1;
			const int innerX2 = previewX2 - 1;
			const int innerY2 = previewY2 - 1;
			const int boxW = innerX2 - innerX1 + 1;
			const int boxH = innerY2 - innerY1 + 1;
			const int drawX = innerX1 + (boxW - previewWidth) / 2;
			const int drawY = innerY1 + (boxH - previewHeight) / 2;
			renderer::drawImageSlice(top_scr, drawX, drawY, previewPixels, previewWidth, previewHeight, 0, previewHeight);
		}
		else {
			drawPreviewIcon(previewX1 + 4, previewY1 + 4, previewX2 - 4, previewY2 - 22);
			drawWrappedText(top_scr, previewX1 + 12, previewY2 - 28, previewX2 - previewX1 - 24, "No embedded cover", 10, 1);
		}
	}
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

u16 file_browser :: draw(bool showScrollbar)
{
	buttons.clear();
	const bool needsScroll = browserNeedsScrollbar(flist.size());
	const int width = screens::layoutX();
	const int height = screens::layoutY();
	const int pad = 10;
	const int headerX1 = pad;
	const int headerX2 = width - pad;
	const int headerY1 = 10;
	const int headerY2 = 48;
	const int footerY2 = height - 10;
	const int footerY1 = footerY2 - 36;
	const int listY1 = headerY2 + 8;
	const int listY2 = footerY1 - 8;
	const int rowHeight = buttonFontSize + 6;
	const int listX1 = pad;
	const int listX2 = browserListRight(needsScroll && showScrollbar);
	const int countWidth = 64;
	const int summaryX2 = headerX2 - countWidth - 8;
	int pen = listY1;

	renderer::clearScreens(settings::bgCol, bottom_scr);

	renderer::fillRect(headerX1, headerY1, headerX2, headerY2, Blend(20), bottom_scr);
	renderer::rect(headerX1, headerY1, headerX2, headerY2, bottom_scr);
	renderer::printStr(eUtf8, bottom_scr, headerX1 + 8, headerY1 + 16, "File Browser", 0, 0, 12);
	drawWrappedText(bottom_scr, headerX1 + 8, headerY1 + 30, summaryX2 - headerX1 - 8, path, 8, 1);
	char countBuf[16];
	sprintf(countBuf, "%lu items", (unsigned long)flist.size());
	renderer::printStr(eUtf8, bottom_scr, headerX2 - countWidth, headerY1 + 30, countBuf, 0, 0, 8);

	renderer::rect(listX1, listY1, listX2, listY2, bottom_scr);
	u16 i = pos;
	for( ; i < flist.size() && pen + rowHeight - 2 <= listY2; ++i) {
		const int rowY1 = pen;
		const int rowY2 = pen + rowHeight - 2;
		if(int(i) == cursor)
			renderer::fillRect(listX1 + 1, rowY1 + 1, listX2 - 1, rowY2 - 1, Blend(72), bottom_scr);
		button item(flist[i].second, listX1, rowY1, listX2, rowY2, buttonFontSize);
		item.enableAutoFit(12);
		buttons.push_back(fbutton(flist[i].first, item));
		buttons.back().second.draw();
		pen += rowHeight;
	}
	const u16 visible = i - pos;
	if(showScrollbar && flist.size()) {
		const int scrollX1 = listX2 + 6;
		const int scrollX2 = headerX2 - 2;
		renderer::rect(scrollX1, listY1, scrollX2, listY2, bottom_scr);
		const float size = MIN(1.0f, float(MAX(1, int(visible))) / flist.size());
		const float posf = (flist.size() > visible) ? float(pos) / (flist.size() - visible) : 0.0f;
		const int trackHeight = listY2 - listY1;
		const int low = listY1 + int(posf * (1.0f - size) * trackHeight);
		const int high = MIN(listY2, low + MAX(10, int(size * trackHeight)));
		renderer::fillRect(scrollX1 + 1, low + 1, scrollX2 - 1, high - 1, Blend(128), bottom_scr);
	}

	gBrowserHomeButton = button("Home", pad, footerY1, width - pad, footerY2, 12);
	gBrowserHomeButton.enableAutoFit(9);
	gBrowserHomeButton.draw();

	return i - pos;
}

void file_browser :: upd(bool refreshScrollbar)
{
	renderer::setTopScreenMirror(false);
	clampCursor();
	syncPreviewToCursor();
	saveBrowserState(path, pos, cursor);
	const bool needsScroll = browserNeedsScrollbar(flist.size());
	(void)needsScroll;
	(void)refreshScrollbar;
	scrollbarFrames = kScrollbarFlashFrames;
	num = draw(true);
	drawPreview();
}

string file_browser :: run()
{
	setBacklightMode(blOverlay);
	renderer::setTopScreenMirror(false);
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
		if(previewPending || previewTitleMarquee) {
			++previewAnimTick;
			if((previewPending && 0 == (previewAnimTick % 12u)) ||
				(previewTitleMarquee && 0 == (previewAnimTick % 6u)))
				drawPreview();
		}
		const bool previewUpdated = tickPreview();
		if(previewUpdated) drawPreview();
		scanKeys();
		int down = keysDown();
		if(!down) continue;

		if(down & KEY_START) {
			saveBrowserState(path, pos, cursor);
			resetPreview();
			return string();
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
			const string selected = activateCursor();
			if(!selected.empty()) {
				saveBrowserState(path, pos, cursor);
				resetPreview();
				return selected;
			}
			upd();
		}
		else if(down & KEY_TOUCH) {
			if(gBrowserHomeButton.touched()) {
				saveBrowserState(path, pos, cursor);
				resetPreview();
				return string();
			}
			for(u16 i = 0; i < buttons.size(); i++) {
				if(!buttons[i].second.touched()) continue;
				cursor = pos + i;
				const string selected = activateCursor();
				if(!selected.empty()) {
					saveBrowserState(path, pos, cursor);
					resetPreview();
					return selected;
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
