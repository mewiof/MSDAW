#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// what the DAW can do with a file the browser found. anything that classifies as
// Unsupported is left out of every listing: the explorer is a way into the project,
// not a file manager, and a folder of album art and readmes should read as empty
enum class FileKind {
	Unsupported,
	Folder,
	Audio,	 // importable as an audio clip
	MIDI,	 // importable as a MIDI clip
	Project	 // openable as a project
};

struct FileEntry {
	std::string path;
	std::string name; // the file's own name, or the folder's - never the full path
	FileKind kind = FileKind::Unsupported;
};

// the model behind the library panel's file explorer: a set of folders the user
// pointed at, plus the importable contents of any directory under them
//
// listings are read lazily, one directory at a time, and cached - an explorer tree
// asks for the same folders on every frame it is open, and a sample folder is slow
// enough to stat that doing it 60 times a second is felt. the recursive sweep is
// the search, and that runs on a thread of its own (same shape as PluginManager)
//
// the roots are not persisted here. the view writes them back to AppConfig, which
// keeps this class free of app-wide state and drivable from a test
class FileBrowser {
public:
	FileBrowser() = default;
	~FileBrowser();

	FileBrowser(const FileBrowser&) = delete;
	FileBrowser& operator=(const FileBrowser&) = delete;

	// ================================================================
	// ROOTS
	// ================================================================

	// false when the path is not a directory, or is already a root. an existing root
	// is not an error the user needs telling about - dropping the same folder twice
	// is a natural thing to do
	bool AddRoot(const std::string& path);
	void RemoveRoot(int index);
	void SetRoots(std::vector<std::string> roots);
	const std::vector<std::string>& GetRoots() const { return mRoots; }

	// ================================================================
	// LISTING
	// ================================================================

	// one directory's importable contents: folders first, then files, each half
	// sorted case-insensitively by name. the returned reference lives until the next
	// Refresh, so a caller may hold it across the recursion that draws its children
	const std::vector<FileEntry>& List(const std::string& directory);

	void Refresh();								// forget every listing
	void Refresh(const std::string& directory); // ... just this one

	// ================================================================
	// SEARCH
	// ================================================================

	// walk every root looking for importable files whose name contains the query,
	// case-insensitively. runs on a background thread; a query arriving while one is
	// in flight cancels it rather than queueing behind it, because the usual source
	// of a new query is the user typing one more letter
	void Search(const std::string& query);
	void CancelSearch();

	bool IsSearching() const { return mSearching.load(std::memory_order_relaxed); }
	std::vector<FileEntry> GetSearchResults() const;

	// true when the sweep stopped at kMaxSearchResults, so the panel can say the
	// list is not the whole answer
	bool SearchTruncated() const;

	// a query the search is running, or has finished. empty means the panel is back
	// to showing the tree
	const std::string& GetSearchQuery() const { return mQuery; }

	// ================================================================
	// CLASSIFICATION
	// ================================================================

	// hits the filesystem, so it can tell a folder from a file
	static FileKind Classify(const std::string& path);

	// the extension alone, with or without its leading dot, in any case. never
	// returns Folder - a path is what knows that
	static FileKind ClassifyExtension(const std::string& extension);

	static const char* KindLabel(FileKind kind);

	// a sweep that filled this many results stops there. a user who types "kick" and
	// gets two thousand hits is not going to scroll to the end of them, and the list
	// is rebuilt from scratch on the next letter
	static constexpr size_t kMaxSearchResults = 500;
private:
	// the sweep, run on mSearchThread. `roots` is a copy: mRoots belongs to the UI
	// thread and can be edited while this runs
	void RunSearch(std::vector<std::string> roots, std::string query);

	std::vector<std::string> mRoots; // UI thread only

	// keyed by directory path. std::map rather than unordered: List hands out a
	// reference into it that has to survive the insertions its own caller makes
	// while recursing into the children it just got back
	std::map<std::string, std::vector<FileEntry>> mListings;

	std::thread mSearchThread;
	std::atomic<bool> mSearching{false};
	std::atomic<bool> mAbortSearch{false};
	std::string mQuery; // UI thread only

	mutable std::mutex mResultMutex;
	std::vector<FileEntry> mResults;
	bool mResultsTruncated = false;
};
