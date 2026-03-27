#include "book.h"
#include "pugixml.h"
#include "unzip.h"
#include <map>

struct epub_entry
{
	string text;
	string image;
	vector<marked> marks;
	parType type;
	epub_entry() : type(pnormal) {}
	epub_entry(const string& txt, parType t, const vector<marked>& m = vector<marked>()) : text(txt), marks(m), type(t) {}
	epub_entry(const string& img) : image(img), type(pimage) {}
	bool isImage() const { return !image.empty(); }
};

class epub_book : public Book
{
public:
	epub_book(const string& filename) : Book(filename), archive(NULL), imageCacheStamp(1) {
		imageCache.resize(3);
	}
	~epub_book();
private:
	struct ImageCacheEntry
	{
		string path;
		vector<u16> pixels;
		u16 width, height;
		u16 maxWidth, maxHeight;
		bool themed;
		Color bgCol, fCol;
		u32 stamp;
		ImageCacheEntry() : width(0), height(0), maxWidth(0), maxHeight(0), themed(false), stamp(0) {}
	};

	void parse();
	void parag_str (int parag_num);
	parType paragraphType(u32 parag_num) {
		if(parag_num >= par_index.size()) return pnormal;
		return par_index[parag_num].type;
	}
	void refreshCachedParagraph(paragrath& paragraph);
	bool loadTocEntries();
	bool load_image(const string& zip_path);
	bool load_image_into(paragrath& target, const string& zip_path);
	bool ensureArchiveOpen();
	void closeArchive();
	void clearImageCache();
	bool tryLoadCachedImage(const string& zip_path, u16 max_width, u16 max_height, paragrath& target);
	void storeCachedImage(const string& zip_path, const vector<u16>& pixels, u16 width, u16 height, u16 max_width, u16 max_height);
	vector<epub_entry> par_index;
	int parse_doc(const pugi::xml_node& node, const string& chapter_path, const string& chapter_base);
	int extract_par(const pugi::xml_node& node, paragrath& target);
	void appendTextEntry(const pugi::xml_node& node, bool isTitle = false);
	void appendEmptyEntry();
	u32 total_paragraths() {return par_index.size();}
	bool push_it;
	std::map<string, u32> chapter_targets;
	std::map<string, u32> anchor_targets;
	std::map<string, unz_file_pos> zip_index;
	unzFile archive;
	vector<ImageCacheEntry> imageCache;
	u32 imageCacheStamp;
	string tocNavFile;
	string tocNcxFile;
};
