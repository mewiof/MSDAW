#include "PrecompHeader.h"
#include "FileBrowser.h"

#include "PathText.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

	std::string ToLower(std::string text) {
		std::transform(text.begin(), text.end(), text.begin(),
					   [](unsigned char c) { return (char)std::tolower(c); });
		return text;
	}

	// case-insensitive name order, so a folder of Kick/kick/KICK reads alphabetically
	// rather than in the order the codepoints happen to fall in
	bool NameLess(const FileEntry& a, const FileEntry& b) {
		return ToLower(a.name) < ToLower(b.name);
	}

	// the one spelling of a folder the browser compares and stores. lexically_normal
	// keeps whatever trailing separator the path was written with, so without this a
	// folder dropped in with a trailing separator would join one already there without
	std::string NormalizedPath(const std::string& path) {
		// ToPath, not fs::path(path): the string is UTF-8, and building a path from it
		// as code-page bytes would spell the folder differently every time it made the
		// round trip - so the same root would stop comparing equal to itself
		std::string normalized = PathText::FromPath(PathText::ToPath(path).lexically_normal());
		// ... but never past a drive: a bare "C:" names the current directory on that
		// drive rather than its root
		while (normalized.size() > 1 && (normalized.back() == '\\' || normalized.back() == '/') &&
			   normalized[normalized.size() - 2] != ':')
			normalized.pop_back();
		return normalized;
	}

	bool Contains(const std::string& haystack, const std::string& needle) {
		return ToLower(haystack).find(needle) != std::string::npos;
	}

} // namespace

FileBrowser::~FileBrowser() {
	// the sweep writes mResults through `this`. a thread outliving the browser is a
	// use-after-free that lands on whatever gets built where it used to be
	CancelSearch();
}

// ================================================================
// ROOTS
// ================================================================

bool FileBrowser::AddRoot(const std::string& path) {
	if (path.empty())
		return false;

	std::error_code ec;
	if (!fs::is_directory(PathText::ToPath(path), ec))
		return false;

	// compare normalized: the same folder typed with a trailing slash, or with the
	// other kind of slash, is the same root and must not be listed twice
	const std::string normalized = NormalizedPath(path);
	for (const auto& root : mRoots) {
		// the stored roots went through the same normalization on the way in, so they
		// are compared as they are rather than normalized a second time
		if (root == normalized)
			return false;
	}

	mRoots.push_back(normalized);
	return true;
}

void FileBrowser::RemoveRoot(int index) {
	if (index >= 0 && index < (int)mRoots.size())
		mRoots.erase(mRoots.begin() + index);
}

void FileBrowser::SetRoots(std::vector<std::string> roots) {
	mRoots.clear();
	for (auto& root : roots)
		AddRoot(root); // a folder that has since been deleted or unplugged drops out here
}

// ================================================================
// LISTING
// ================================================================

const std::vector<FileEntry>& FileBrowser::List(const std::string& directory) {
	auto cached = mListings.find(directory);
	if (cached != mListings.end())
		return cached->second;

	std::vector<FileEntry> entries;

	std::error_code ec;
	fs::directory_iterator it(PathText::ToPath(directory), fs::directory_options::skip_permission_denied, ec);
	if (!ec) {
		for (const auto& item : it) {
			std::error_code itemEc;
			const bool isDirectory = item.is_directory(itemEc);
			if (itemEc)
				continue; // a file that vanished between the read and the stat

			FileEntry entry;
			entry.path = PathText::FromPath(item.path());
			entry.name = PathText::FromPath(item.path().filename());

			if (isDirectory) {
				entry.kind = FileKind::Folder;
			} else {
				entry.kind = ClassifyExtension(PathText::FromPath(item.path().extension()));
				if (entry.kind == FileKind::Unsupported)
					continue;
			}
			entries.push_back(std::move(entry));
		}
	}

	// folders above files, each half alphabetical - the shape every file explorer
	// has, and the one that keeps a deep tree readable at the panel's width
	std::stable_sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
		const bool aFolder = a.kind == FileKind::Folder;
		const bool bFolder = b.kind == FileKind::Folder;
		if (aFolder != bFolder)
			return aFolder;
		return NameLess(a, b);
	});

	return mListings.emplace(directory, std::move(entries)).first->second;
}

void FileBrowser::Refresh() {
	mListings.clear();
}

void FileBrowser::Refresh(const std::string& directory) {
	mListings.erase(directory);
}

// ================================================================
// SEARCH
// ================================================================

void FileBrowser::Search(const std::string& query) {
	CancelSearch();

	mQuery = query;
	{
		std::lock_guard<std::mutex> lock(mResultMutex);
		mResults.clear();
		mResultsTruncated = false;
	}
	if (query.empty())
		return;

	mAbortSearch.store(false, std::memory_order_relaxed);
	mSearching.store(true, std::memory_order_relaxed);
	mSearchThread = std::thread(&FileBrowser::RunSearch, this, mRoots, ToLower(query));
}

void FileBrowser::CancelSearch() {
	mAbortSearch.store(true, std::memory_order_relaxed);
	if (mSearchThread.joinable())
		mSearchThread.join();
	mSearching.store(false, std::memory_order_relaxed);
	mQuery.clear();
}

void FileBrowser::RunSearch(std::vector<std::string> roots, std::string query) {
	std::vector<FileEntry> found;
	bool truncated = false;
	bool aborted = false;

	for (const auto& root : roots) {
		if (truncated || aborted || mAbortSearch.load(std::memory_order_relaxed))
			break;

		// NOTE: nothing here is allowed to escape. an exception leaving a thread that
		// has no handler above it is std::terminate - the whole DAW, mid-session,
		// because one folder somewhere was not readable the way the standard library
		// expected. one root failing costs that root, not the process
		try {
			std::error_code ec;
			fs::recursive_directory_iterator it(PathText::ToPath(root), fs::directory_options::skip_permission_denied, ec);
			if (ec)
				continue;

			const fs::recursive_directory_iterator end;
			for (; it != end; it.increment(ec)) {
				// a folder that cannot be read stops this root rather than the sweep
				if (ec)
					break;
				// typing another letter must not wait out a sweep of an entire drive
				if (mAbortSearch.load(std::memory_order_relaxed)) {
					aborted = true;
					break;
				}

				std::error_code itemEc;
				if (it->is_directory(itemEc) || itemEc)
					continue;

				const FileKind kind = ClassifyExtension(PathText::FromPath(it->path().extension()));
				if (kind == FileKind::Unsupported)
					continue;

				std::string name = PathText::FromPath(it->path().filename());
				if (!Contains(name, query))
					continue;

				FileEntry entry;
				entry.path = PathText::FromPath(it->path());
				entry.name = std::move(name);
				entry.kind = kind;
				found.push_back(std::move(entry));

				if (found.size() >= kMaxSearchResults) {
					truncated = true;
					break;
				}
			}
		} catch (const std::exception&) {
			continue;
		}
	}

	std::sort(found.begin(), found.end(), NameLess);

	// an abandoned sweep saw only part of the tree, so it must not replace the results
	// the panel is showing with a truncated set
	if (!aborted) {
		std::lock_guard<std::mutex> lock(mResultMutex);
		mResults = std::move(found);
		mResultsTruncated = truncated;
	}
	mSearching.store(false, std::memory_order_relaxed);
}

std::vector<FileEntry> FileBrowser::GetSearchResults() const {
	std::lock_guard<std::mutex> lock(mResultMutex);
	return mResults;
}

bool FileBrowser::SearchTruncated() const {
	std::lock_guard<std::mutex> lock(mResultMutex);
	return mResultsTruncated;
}

// ================================================================
// CLASSIFICATION
// ================================================================

FileKind FileBrowser::Classify(const std::string& path) {
	std::error_code ec;
	const fs::path file = PathText::ToPath(path);
	if (fs::is_directory(file, ec))
		return FileKind::Folder;
	if (ec)
		return FileKind::Unsupported;
	return ClassifyExtension(PathText::FromPath(file.extension()));
}

FileKind FileBrowser::ClassifyExtension(const std::string& extension) {
	std::string ext = ToLower(extension);
	if (!ext.empty() && ext.front() == '.')
		ext.erase(ext.begin());

	// NOTE: wav only, because AudioClip::LoadFromFile is a WAV reader. listing an
	// mp3 the importer would refuse is a worse answer than not listing it at all -
	// widen this the day the loader learns another container
	if (ext == "wav" || ext == "wave")
		return FileKind::Audio;
	if (ext == "mid" || ext == "midi")
		return FileKind::MIDI;
	if (ext == "msdaw")
		return FileKind::Project;
	return FileKind::Unsupported;
}

const char* FileBrowser::KindLabel(FileKind kind) {
	switch (kind) {
		case FileKind::Folder:
			return "Folder";
		case FileKind::Audio:
			return "Audio";
		case FileKind::MIDI:
			return "MIDI";
		case FileKind::Project:
			return "Project";
		case FileKind::Unsupported:
			break;
	}
	return "";
}
