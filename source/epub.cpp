#include "epub.h"
#include <algorithm>
#include <setjmp.h>
#include <string.h>
#include "renderer.h"
#include "screens.h"
#include "zlib.h"
#include "unzip.h"
#include <stdio.h>
#include <cstdlib>
#include <fstream>
#include <map>
#include <new>

extern "C" {
#include "jpeglib.h"
}

namespace
{

typedef std::map<string, unz_file_pos> zip_index_map;
const u32 kOpenProgressUpdateStep = 8u;
const u32 kMaxImageEntryBytes = 8u * 1024u * 1024u;
const u32 kMaxChapterEntryBytes = 6u * 1024u * 1024u;
const u32 kMaxTotalChapterBytes = 24u * 1024u * 1024u;

bool buildZipIndex(unzFile zip, zip_index_map& index)
{
	index.clear();
	if(unzGoToFirstFile(zip) != UNZ_OK) return false;

	do {
		char name[512];
		if(unzGetCurrentFileInfo(zip, NULL, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK)
			continue;

		unz_file_pos pos;
		if(unzGetFilePos(zip, &pos) != UNZ_OK) continue;
		index[name] = pos;
	} while(unzGoToNextFile(zip) == UNZ_OK);

	return !index.empty();
}

bool locateZipEntry(unzFile zip, const string& file, const zip_index_map* index)
{
	if(index != NULL) {
		zip_index_map::const_iterator it = index->find(file);
		if(it != index->end()) {
			unz_file_pos pos = it->second;
			return unzGoToFilePos(zip, &pos) == UNZ_OK;
		}
	}
	return unzLocateFile(zip, file.c_str(), 0) == UNZ_OK;
}

bool loadFromZip(unzFile& zip, const string& file, char *&buf, u32& size, bool fatal = true, const zip_index_map* index = NULL)
{
	unz_file_info info;
	if(!locateZipEntry(zip, file, index)) {
		if(fatal) bsod("epub.loadFromZip:Can't locate file.");
		return false;
	}
	if(unzOpenCurrentFile(zip) != UNZ_OK) {
		if(fatal) bsod("epub.loadFromZip:Can't open archive entry.");
		return false;
	}
	unzGetCurrentFileInfo(zip, &info, NULL, 0, NULL, 0, NULL, 0);
	size = info.uncompressed_size;
	buf = new (std::nothrow) char[size + 1u];
	if(buf == NULL) bsod("epub.loadFromZip:Out of memory.");
	const int read = unzReadCurrentFile(zip, buf, size);
	unzCloseCurrentFile(zip);
	if(read < 0 || (u32)read != size) {
		delete[] buf;
		buf = NULL;
		if(fatal) bsod("epub.loadFromZip:Error while reading entry.");
		return false;
	}
	buf[size] = '\0';
	return true;
}

bool zipEntrySize(unzFile& zip, const string& file, u32& size, const zip_index_map* index = NULL)
{
	size = 0;
	if(!locateZipEntry(zip, file, index)) return false;
	unz_file_info info;
	if(unzGetCurrentFileInfo(zip, &info, NULL, 0, NULL, 0, NULL, 0) != UNZ_OK)
		return false;
	size = info.uncompressed_size;
	return true;
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

string normalizeZipHref(const string& base, const string& raw_path)
{
	const string::size_type pos = raw_path.find('#');
	const string fragment = (string::npos == pos) ? string() : raw_path.substr(pos);
	const string path = normalizeZipPath(base, (string::npos == pos) ? raw_path : raw_path.substr(0, pos));
	if(path.empty()) return string();
	return path + fragment;
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

bool isTextChapterType(const char* media_type)
{
	return !strcmp(media_type, "application/xhtml+xml") || !strcmp(media_type, "text/html");
}

pugi::xml_node findNodeByName(const pugi::xml_node& node, const char* name)
{
	vector<pugi::xml_node> stack;
	if(node) stack.push_back(node);
	while(!stack.empty()) {
		pugi::xml_node current = stack.back();
		stack.pop_back();
		if(!strcmp(current.name(), name)) return current;
		for(pugi::xml_node child = current.last_child(); child; child = child.previous_sibling())
			stack.push_back(child);
	}
	return pugi::xml_node();
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

void appendNodeText(const pugi::xml_node& node, string& out)
{
	if(node.type() == pugi::node_pcdata || node.type() == pugi::node_cdata)
		out += node.value();
	for(pugi::xml_node child = node.first_child(); child; child = child.next_sibling())
		appendNodeText(child, out);
}

string nodeText(const pugi::xml_node& node)
{
	string text;
	appendNodeText(node, text);
	return collapseWhitespace(text);
}

struct toc_link
{
	string title;
	string href;
	u16 depth;
	toc_link() {}
	toc_link(const string& t, const string& h, u16 d = 0) : title(t), href(h), depth(d) {}
};

void collectHtmlTocItems(const pugi::xml_node& list, const string& base, vector<toc_link>& out, u16 depth)
{
	for(pugi::xml_node item = list.first_child(); item; item = item.next_sibling()) {
		if(strcmp(item.name(), "li")) continue;

		for(pugi::xml_node child = item.first_child(); child; child = child.next_sibling()) {
			if(!strcmp(child.name(), "a")) {
				const string href = normalizeZipHref(base, child.attribute("href").value());
				const string title = nodeText(child);
				if(!href.empty() && !title.empty())
					out.push_back(toc_link(title, href, depth));
			}
			if(!strcmp(child.name(), "ol"))
				collectHtmlTocItems(child, base, out, depth + 1u);
		}
	}
}

bool collectHtmlTocNav(const pugi::xml_node& node, const string& base, vector<toc_link>& out)
{
	if(!strcmp(node.name(), "nav")) {
		const string navType = node.attribute("epub:type").value();
		const string plainType = node.attribute("type").value();
		if(containsWord(navType, "toc") || containsWord(plainType, "toc")) {
			for(pugi::xml_node child = node.first_child(); child; child = child.next_sibling())
				if(!strcmp(child.name(), "ol"))
					collectHtmlTocItems(child, base, out, 0);
			return !out.empty();
		}
	}

	for(pugi::xml_node child = node.first_child(); child; child = child.next_sibling())
		if(collectHtmlTocNav(child, base, out)) return true;
	return false;
}

bool loadHtmlTocLinks(unzFile& zip, const string& toc_file, vector<toc_link>& out, const zip_index_map* index)
{
	char* buf = NULL;
	u32 size = 0;
	if(!loadFromZip(zip, toc_file, buf, size, false, index) || NULL == buf) return false;

	pugi::xml_document doc;
	const pugi::xml_parse_result result = doc.load_buffer_inplace(buf, size);
	if(result.status != pugi::status_ok) {
		delete[] buf;
		return false;
	}

	const bool found = collectHtmlTocNav(doc, dirName(toc_file), out);
	delete[] buf;
	return found && !out.empty();
}

void collectNcxNavPoints(const pugi::xml_node& node, const string& base, vector<toc_link>& out, u16 depth)
{
	for(pugi::xml_node navPoint = node.first_child(); navPoint; navPoint = navPoint.next_sibling()) {
		if(strcmp(navPoint.name(), "navPoint")) continue;
		const string href = normalizeZipHref(base, navPoint.child("content").attribute("src").value());
		const string title = nodeText(navPoint.child("navLabel"));
		if(!href.empty() && !title.empty())
			out.push_back(toc_link(title, href, depth));
		collectNcxNavPoints(navPoint, base, out, depth + 1u);
	}
}

bool loadNcxTocLinks(unzFile& zip, const string& toc_file, vector<toc_link>& out, const zip_index_map* index)
{
	char* buf = NULL;
	u32 size = 0;
	if(!loadFromZip(zip, toc_file, buf, size, false, index) || NULL == buf) return false;

	pugi::xml_document doc;
	const pugi::xml_parse_result result = doc.load_buffer_inplace(buf, size);
	if(result.status != pugi::status_ok) {
		delete[] buf;
		return false;
	}

	const pugi::xml_node navMap = findNodeByName(doc, "navMap");
	if(navMap) collectNcxNavPoints(navMap, dirName(toc_file), out, 0);
	delete[] buf;
	return !out.empty();
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

u16 imagePixel(u8 r, u8 g, u8 b)
{
	if(!settings::nightMode() && !settings::lowLightMode()) return RGB15(r >> 3, g >> 3, b >> 3) | BIT(15);

	const unsigned lum = (r * 30u + g * 59u + b * 11u) / 100u;
	const u8 toneR = (settings::bgCol.R * (255u - lum) + settings::fCol.R * lum) / 255u;
	const u8 toneG = (settings::bgCol.G * (255u - lum) + settings::fCol.G * lum) / 255u;
	const u8 toneB = (settings::bgCol.B * (255u - lum) + settings::fCol.B * lum) / 255u;
	return RGB15(toneR, toneG, toneB) | BIT(15);
}

u16 imagePanelWidth()
{
	const int width = screens::line_width();
	return (width > 0) ? width : 1;
}

u16 imagePanelHeight()
{
	int height = screens::screen_text_height(first_scr()) - 1 - screens::up_margin - screens::bottom_margin;
	if(height < 1) height = 1;
	return height;
}

bool decodeJpeg(const char* data, u32 size, u16 max_width, u16 max_height, vector<u16>& pixels, u16& width, u16& height)
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
	if(0 == max_width) max_width = 1;
	if(0 == max_height) max_height = 1;
	if(cinfo.image_width > max_width * 4u || cinfo.image_height > max_height * 4u) cinfo.scale_denom = 8;
	else if(cinfo.image_width > max_width * 2u || cinfo.image_height > max_height * 2u) cinfo.scale_denom = 4;
	else if(cinfo.image_width > max_width || cinfo.image_height > max_height) cinfo.scale_denom = 2;

	jpeg_start_decompress(&cinfo);

	const u32 src_width = cinfo.output_width;
	const u32 src_height = cinfo.output_height;
	const u32 comp = cinfo.output_components;
	if(0 == src_width || 0 == src_height || (comp != 1 && comp != 3)) {
		jpeg_finish_decompress(&cinfo);
		jpeg_destroy_decompress(&cinfo);
		return false;
	}

	u32 dst_width = src_width;
	u32 dst_height = src_height;
	if(dst_width > max_width || dst_height > max_height) {
		if(dst_width * max_height > dst_height * max_width) {
			dst_height = (dst_height * max_width) / dst_width;
			dst_width = max_width;
		}
		else {
			dst_width = (dst_width * max_height) / dst_height;
			dst_height = max_height;
		}
	}
	if(0 == dst_width) dst_width = 1;
	if(0 == dst_height) dst_height = 1;

	pixels.resize(dst_width * dst_height);
	vector<unsigned char> row(src_width * comp);
	u32 next_dst_y = 0;
	while(cinfo.output_scanline < src_height) {
		JSAMPROW row_ptr = &row[0];
		jpeg_read_scanlines(&cinfo, &row_ptr, 1);
		const u32 src_y = cinfo.output_scanline - 1u;
		while(next_dst_y < dst_height && ((next_dst_y * src_height) / dst_height) <= src_y) {
			for(u32 x = 0; x < dst_width; ++x) {
				const u32 src_x = (x * src_width) / dst_width;
				const u32 index = src_x * comp;
				const u8 r = row[index];
				const u8 g = (comp == 3) ? row[index + 1] : r;
				const u8 b = (comp == 3) ? row[index + 2] : r;
				pixels[next_dst_y * dst_width + x] = imagePixel(r, g, b);
			}
			++next_dst_y;
		}
	}
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	while(next_dst_y < dst_height) {
		const u32 copy_from = (next_dst_y == 0) ? 0 : (next_dst_y - 1u);
		for(u32 x = 0; x < dst_width; ++x)
			pixels[next_dst_y * dst_width + x] = pixels[copy_from * dst_width + x];
		++next_dst_y;
	}

	width = dst_width;
	height = dst_height;
	return true;
}

} // namespace

epub_book :: ~epub_book()
{
	closeArchive();
}

bool epub_book :: ensureArchiveOpen()
{
	if(archive != NULL) return true;
	archive = unzOpen(bookFile.c_str());
	return archive != NULL;
}

void epub_book :: closeArchive()
{
	if(archive != NULL) {
		unzClose(archive);
		archive = NULL;
	}
}

void epub_book :: clearImageCache()
{
	for(u32 i = 0; i < imageCache.size(); ++i) {
		imageCache[i].path.clear();
		vector<u16>().swap(imageCache[i].pixels);
		imageCache[i].width = imageCache[i].height = 0;
		imageCache[i].maxWidth = imageCache[i].maxHeight = 0;
		imageCache[i].themed = false;
		imageCache[i].bgCol = Color(0, 0, 0);
		imageCache[i].fCol = Color(0, 0, 0);
		imageCache[i].stamp = 0;
	}
	imageCacheStamp = 1;
}

bool epub_book :: tryLoadCachedImage(const string& zip_path, u16 max_width, u16 max_height, paragrath& target)
{
	const bool themed = settings::nightMode() || settings::lowLightMode();
	for(u32 i = 0; i < imageCache.size(); ++i) {
		ImageCacheEntry& entry = imageCache[i];
		if(entry.path != zip_path || entry.maxWidth != max_width || entry.maxHeight != max_height || entry.themed != themed)
			continue;
		if(themed && (entry.bgCol.R != settings::bgCol.R || entry.bgCol.G != settings::bgCol.G || entry.bgCol.B != settings::bgCol.B ||
			entry.fCol.R != settings::fCol.R || entry.fCol.G != settings::fCol.G || entry.fCol.B != settings::fCol.B))
			continue;

		target.image_ref = zip_path;
		target.image_pixels = entry.pixels;
		target.image_width = entry.width;
		target.image_height = entry.height;
		entry.stamp = imageCacheStamp++;
		return true;
	}
	return false;
}

void epub_book :: storeCachedImage(const string& zip_path, const vector<u16>& pixels, u16 width, u16 height, u16 max_width, u16 max_height)
{
	if(imageCache.empty()) return;

	ImageCacheEntry* slot = &imageCache[0];
	for(u32 i = 0; i < imageCache.size(); ++i) {
		ImageCacheEntry& entry = imageCache[i];
		if(entry.path.empty()) {
			slot = &entry;
			break;
		}
		if(entry.stamp < slot->stamp) slot = &entry;
	}

	slot->path = zip_path;
	slot->pixels = pixels;
	slot->width = width;
	slot->height = height;
	slot->maxWidth = max_width;
	slot->maxHeight = max_height;
	slot->themed = settings::nightMode() || settings::lowLightMode();
	slot->bgCol = settings::bgCol;
	slot->fCol = settings::fCol;
	slot->stamp = imageCacheStamp++;
}

void epub_book :: parse()
{
	encoding = eUtf8;
	par_index.clear();
	tocEntries.clear();
	tocReady = false;
	chapter_targets.clear();
	anchor_targets.clear();
	zip_index.clear();
	clearImageCache();
	closeArchive();
	tocNavFile.clear();
	tocNcxFile.clear();
	const char err[] = "epub_book::parse:Can't load epub.";

	char *buf = NULL;
	u32 size = 0;
	if(!ensureArchiveOpen()) bsod("epub_book::parse:Can't open epub.");
	buildZipIndex(archive, zip_index);

	pugi::xml_document doc;
	if(!loadFromZip(archive, "META-INF/container.xml", buf, size, true, &zip_index)) bsod(err);
	pugi::xml_parse_result result = doc.load_buffer_inplace(buf, size);
	if(result.status != pugi::status_ok) bsod(err);
	string cont_path = doc.child("container").child("rootfiles").child("rootfile").attribute("full-path").value();
	delete[] buf;
	buf = NULL;

	const string opf_dir = dirName(cont_path);
	doc.reset();
	if(!loadFromZip(archive, cont_path, buf, size, true, &zip_index)) bsod(err);
	result = doc.load_buffer_inplace(buf, size);
	if(result.status != pugi::status_ok) bsod(err);

	std::map<string, string> chapters_unordered;
	std::map<string, string> manifest_files;
	for(pugi::xml_node item = doc.child("package").child("manifest").first_child(); item; item = item.next_sibling()) {
		const string id = item.attribute("id").value();
		const string href = normalizeZipPath(opf_dir, item.attribute("href").value());
		const string mediaType = item.attribute("media-type").value();
		manifest_files[id] = href;
		if(isTextChapterType(mediaType.c_str()))
			chapters_unordered[id] = href;
		if(tocNavFile.empty() && containsWord(item.attribute("properties").value(), "nav"))
			tocNavFile = href;
		if(tocNcxFile.empty() && mediaType == "application/x-dtbncx+xml")
			tocNcxFile = href;
	}

	const string spine_toc_id = doc.child("package").child("spine").attribute("toc").value();
	if(!spine_toc_id.empty()) {
		std::map<string, string>::const_iterator found = manifest_files.find(spine_toc_id);
		if(found != manifest_files.end() && !found->second.empty())
			tocNcxFile = found->second;
	}

	vector<string> chapter_files;
	for(pugi::xml_node item = doc.child("package").child("spine").first_child(); item; item = item.next_sibling()) {
		const string chapter = chapters_unordered[item.attribute("idref").value()];
		if(!chapter.empty()) chapter_files.push_back(chapter);
	}
	delete[] buf;
	buf = NULL;

	u32 totalChapterBytes = 0;
	for(u32 i = 0; i < chapter_files.size(); ++i) {
		u32 chapterSize = 0;
		if(!zipEntrySize(archive, chapter_files[i], chapterSize, &zip_index)) continue;
		if(chapterSize > kMaxChapterEntryBytes)
			bsod("epub_book::parse:Chapter is too large for 3DS memory.\nSplit or optimize this EPUB.");
		if(totalChapterBytes > kMaxTotalChapterBytes - chapterSize)
			bsod("epub_book::parse:Book is too large for this 3DS build.\nSplit or optimize this EPUB.");
		totalChapterBytes += chapterSize;
	}

	renderer::clearScreens(0);
	push_it = true;
	for(u32 i = 0; i < chapter_files.size(); ++i) {
		if(i == 0 || i + 1u == chapter_files.size() || 0 == (i % kOpenProgressUpdateStep)) {
			consoleClear();
			iprintf("loading %lu/%d\n", i + 1, (int)chapter_files.size());
		}
		
		if(!loadFromZip(archive, chapter_files[i], buf, size, true, &zip_index)) continue;
		pugi::xml_document chapter_doc;
		result = chapter_doc.load_buffer_inplace(buf, size);
		if(result.status == pugi::status_out_of_memory) bsod("epub_book::parse:Out of memory.");
		
		if(result.status == pugi::status_ok) {
			const string chapter_path = chapter_files[i];
			const string chapter_base = dirName(chapter_path);
			if(chapter_targets.find(chapter_path) == chapter_targets.end())
				chapter_targets[chapter_path] = par_index.size();

			pugi::xml_node body = findNodeByName(chapter_doc, "body");
			if(body) {
				for(pugi::xml_node child = body.first_child(); child; child = child.next_sibling())
					parse_doc(child, chapter_path, chapter_base);
			}
			else if(chapter_doc.document_element()) {
				parse_doc(chapter_doc.document_element(), chapter_path, chapter_base);
			}
		}
		delete[] buf;
		buf = NULL;
	}
	consoleClear();
}

static const string nl_tags(" br div dt h1 h2 h3 h4 h5 h6 hr li p pre ol td ul body ");
static const string br_tags = " br ";
static const string skip_block_tags(" title script style binary image svg ");
static const string title_tags(" title h1 h2 h3 h4 h5 h6 ");
static const string mark_parent_tags(" b tt big small ");
static const string skip_inline_tags(" script style binary image img svg ");

bool epub_book :: load_image(const string& zip_path)
{
	return load_image_into(parag, zip_path);
}

bool epub_book :: load_image_into(paragrath& target, const string& zip_path)
{
	target.image_pixels.clear();
	target.image_width = target.image_height = 0;
	target.image_ref = zip_path;
	if(zip_path.empty()) return false;

	const u16 max_width = imagePanelWidth();
	const u16 max_height = imagePanelHeight();
	if(tryLoadCachedImage(zip_path, max_width, max_height, target)) return true;

	char* buf = NULL;
	u32 size = 0;
	if(!ensureArchiveOpen()) return false;
	if(!zipEntrySize(archive, zip_path, size, &zip_index) || size > kMaxImageEntryBytes)
		return false;
	const bool loaded = loadFromZip(archive, zip_path, buf, size, false, &zip_index);
	if(!loaded || NULL == buf) return false;

	bool ok = false;
	if(size > 2u && (u8)buf[0] == 0xFF && (u8)buf[1] == 0xD8)
		ok = decodeJpeg(buf, size, max_width, max_height, target.image_pixels, target.image_width, target.image_height);

	delete[] buf;
	if(ok)
		storeCachedImage(zip_path, target.image_pixels, target.image_width, target.image_height, max_width, max_height);
	return ok;
}

void epub_book :: refreshCachedParagraph(paragrath& paragraph)
{
	if(paragraph.type != pimage || !paragraph.image_pixels.empty() || paragraph.image_ref.empty())
		return;
	load_image_into(paragraph, paragraph.image_ref);
}

bool epub_book :: loadTocEntries()
{
	if(tocNavFile.empty() && tocNcxFile.empty()) return false;
	if(!ensureArchiveOpen()) return false;

	vector<toc_link> toc_links;
	if(!tocNavFile.empty()) loadHtmlTocLinks(archive, tocNavFile, toc_links, &zip_index);
	if(toc_links.empty() && !tocNcxFile.empty()) loadNcxTocLinks(archive, tocNcxFile, toc_links, &zip_index);

	for(u32 i = 0; i < toc_links.size(); ++i) {
		u32 target = 0;
		bool found = false;
		std::map<string, u32>::const_iterator anchor = anchor_targets.find(toc_links[i].href);
		if(anchor != anchor_targets.end()) {
			target = anchor->second;
			found = true;
		}
		else {
			const string chapter = stripFragment(toc_links[i].href);
			std::map<string, u32>::const_iterator chapter_it = chapter_targets.find(chapter);
			if(chapter_it != chapter_targets.end()) {
				target = chapter_it->second;
				found = true;
			}
		}
		if(!found || toc_links[i].title.empty()) continue;
		if(!tocEntries.empty() &&
			tocEntries.back().place.parag_num == target &&
			tocEntries.back().title == toc_links[i].title &&
			tocEntries.back().depth == toc_links[i].depth)
			continue;
		tocEntries.push_back(toc_entry(target, toc_links[i].title, toc_links[i].depth));
	}

	return !tocEntries.empty();
}

void epub_book :: parag_str (int parag_num)
{
	parag = paragrath();
	const epub_entry& entry = par_index[parag_num];
	parag.type = entry.type;
	if(entry.isImage()) {
		if(!load_image(entry.image)) {
			parag.type = pnormal;
			parag.str = "[Image: " + noPath(entry.image) + "]";
		}
		return;
	}
	parag.str = entry.text;
	parag.marks = entry.marks;
}

int epub_book :: parse_doc(const pugi::xml_node& node, const string& chapter_path, const string& chapter_base)
{
	struct ParseFrame
	{
		pugi::xml_node node;
		bool exitPhase;
		ParseFrame(const pugi::xml_node& value, bool exiting) : node(value), exitPhase(exiting) {}
	};

	vector<ParseFrame> stack;
	if(node) stack.push_back(ParseFrame(node, false));
	while(!stack.empty()) {
		const ParseFrame frame = stack.back();
		stack.pop_back();
		if(!frame.node) continue;

		if(frame.exitPhase) {
			if(push_it) appendEmptyEntry();
			push_it = true;
			continue;
		}

		const string tag = frame.node.name();
		const bool isTitle = !tag.empty() && string::npos != title_tags.find(' ' + tag + ' ');
		const char* attrs[2] = {"id", "name"};
		for(u32 i = 0; i < 2; ++i) {
			const char* value = frame.node.attribute(attrs[i]).value();
			if(!value || !value[0]) continue;
			const string anchorKey = chapter_path + "#" + value;
			if(anchor_targets.find(anchorKey) != anchor_targets.end()) continue;
			u32 target = par_index.size();
			if(!push_it && !par_index.empty()) target = par_index.size() - 1u;
			anchor_targets[anchorKey] = target;
		}

		if(tag == "img") {
			const string path = normalizeZipPath(chapter_base, frame.node.attribute("src").value());
			if(!path.empty()) {
				par_index.push_back(epub_entry(path));
				push_it = false;
			}
			continue;
		}

		const bool newl = !tag.empty() && string::npos != nl_tags.find(' ' + tag + ' ');
		const bool br = !tag.empty() && string::npos != br_tags.find(' ' + tag + ' ');
		const bool ret = !tag.empty() && string::npos != skip_block_tags.find(' ' + tag + ' ');

		if(newl) {
			if(br) {
				if(frame.node.next_sibling()) appendTextEntry(frame.node.next_sibling(), isTitle);
				else appendEmptyEntry();
			}
			else if(push_it) appendTextEntry(frame.node, isTitle);
			push_it = false;
		}
		if(ret) continue;

		if(newl && !br) stack.push_back(ParseFrame(frame.node, true));
		for(pugi::xml_node elem = frame.node.last_child(); elem; elem = elem.previous_sibling())
			stack.push_back(ParseFrame(elem, false));
	}
	return 0;
}

void epub_book :: appendTextEntry(const pugi::xml_node& node, bool isTitle)
{
	if(!node) {
		appendEmptyEntry();
		return;
	}

	paragrath paragraph;
	paragraph.type = isTitle ? ptitle : pnormal;
	extract_par(node, paragraph);
	par_index.push_back(epub_entry(paragraph.str, paragraph.type, paragraph.marks));
}

void epub_book :: appendEmptyEntry()
{
	par_index.push_back(epub_entry());
}

int epub_book :: extract_par(const pugi::xml_node& node, paragrath& target)
{
	struct ExtractFrame
	{
		pugi::xml_node node;
		bool exitPhase;
		bool newl;
		bool nospace;
		bool bold;
		bool italic;
		ExtractFrame(const pugi::xml_node& value, bool exiting)
			: node(value), exitPhase(exiting), newl(false), nospace(false), bold(false), italic(false) {}
	};

	vector<ExtractFrame> stack;
	if(node) stack.push_back(ExtractFrame(node, false));
	while(!stack.empty()) {
		ExtractFrame frame = stack.back();
		stack.pop_back();
		if(!frame.node) continue;

		if(frame.exitPhase) {
			if((frame.italic || frame.bold) && target.marks.size())
				target.marks.back().end = target.str.length();
			if(frame.newl) return -1;
			if(frame.node.type() == pugi::node_pcdata && !frame.nospace && frame.node.parent().next_sibling())
				target.str += ' ';
			continue;
		}

		const string tag = frame.node.name();
		const string parentTag = frame.node.parent().name();
		frame.newl = !tag.empty() && string::npos != nl_tags.find(' ' + tag + ' ');
		const bool isTitle = (!parentTag.empty() && string::npos != title_tags.find(' ' + parentTag + ' '))
			|| (!tag.empty() && string::npos != title_tags.find(' ' + tag + ' '));
		frame.bold = (parentTag == "b") || (parentTag == "strong");
		frame.italic = (parentTag == "i") || (parentTag == "em");
		frame.nospace = (frame.bold || frame.italic) || (!parentTag.empty() && string::npos != mark_parent_tags.find(' ' + parentTag + ' '));
		const bool ret = !tag.empty() && string::npos != skip_inline_tags.find(' ' + tag + ' ');

		if(ret) continue;
		if(isTitle) target.type = ptitle;

		if(frame.bold) {
			const int pos = target.str.length();
			marked mark = {pos, pos, fbold};
			target.marks.push_back(mark);
		}
		else if(frame.italic) {
			const int pos = target.str.length();
			marked mark = {pos, pos, fitalic};
			target.marks.push_back(mark);
		}

		if(frame.node.type() == pugi::node_pcdata) target.str += frame.node.value();
		stack.push_back(frame);
		stack.back().exitPhase = true;
		for(pugi::xml_node elem = frame.node.last_child(); elem; elem = elem.previous_sibling())
			stack.push_back(ExtractFrame(elem, false));
	}
	return 0;
}
