#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "Library/FileBrowser.h"
#include "PathText.h"

// ================================================================
// LIBRARY FILE BROWSER
// ================================================================

// the browser is the headless half of the library panel: what a folder contains,
// what of it is importable, and what a recursive search over the roots finds. it
// touches the disk, so every test builds its own tree under a unique temp folder
// and takes it away again

namespace fs = std::filesystem;

// NOTE: the fixture stays out of an anonymous namespace - TEST_F names it from file
// scope, and a fixture hidden in one is not the class the macro would find
class FileBrowserTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
		mRoot = fs::temp_directory_path() / ("MSDAWBrowser_" + std::string(info->name()));
		std::error_code ec;
		fs::remove_all(mRoot, ec);
		fs::create_directories(mRoot, ec);
	}

	void TearDown() override {
		std::error_code ec;
		fs::remove_all(mRoot, ec);
	}

	// content is irrelevant here - the browser classifies by extension and never
	// opens what it lists
	void WriteFile(const std::string& relativePath) {
		const fs::path full = mRoot / relativePath;
		std::error_code ec;
		fs::create_directories(full.parent_path(), ec);
		std::ofstream out(full);
		out << "x";
	}

	// a file whose name has no representation in the machine's ANSI code page. this
	// is the shape that used to kill the process: std::filesystem::path::string()
	// throws on it, and the sweep that walked past it had no handler above it
	void WriteWideFile(const std::wstring& relativeName) {
		const fs::path full = mRoot / relativeName;
		std::error_code ec;
		fs::create_directories(full.parent_path(), ec);
		std::ofstream out(full);
		out << "x";
	}

	void MakeDirectory(const std::string& relativePath) {
		std::error_code ec;
		fs::create_directories(mRoot / relativePath, ec);
	}

	std::string RootPath() const { return mRoot.string(); }
	std::string PathOf(const std::string& relativePath) const { return (mRoot / relativePath).string(); }

	static bool WaitForSearch(const FileBrowser& browser) {
		for (int i = 0; i < 500 && browser.IsSearching(); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		return !browser.IsSearching();
	}

	fs::path mRoot;
};

// ---- classification ----

TEST(FileBrowserClassify, RecognizesTheThreeImportableKinds) {
	EXPECT_EQ(FileBrowser::ClassifyExtension(".wav"), FileKind::Audio);
	EXPECT_EQ(FileBrowser::ClassifyExtension(".mid"), FileKind::MIDI);
	EXPECT_EQ(FileBrowser::ClassifyExtension(".midi"), FileKind::MIDI);
	EXPECT_EQ(FileBrowser::ClassifyExtension(".msdaw"), FileKind::Project);
}

TEST(FileBrowserClassify, IgnoresCaseAndALeadingDot) {
	EXPECT_EQ(FileBrowser::ClassifyExtension(".WAV"), FileKind::Audio);
	EXPECT_EQ(FileBrowser::ClassifyExtension("Wav"), FileKind::Audio);
	EXPECT_EQ(FileBrowser::ClassifyExtension("MSDAW"), FileKind::Project);
}

// the panel promises importable files, and the WAV reader is all AudioClip has, so
// a format it would refuse must not be listed as one it takes
TEST(FileBrowserClassify, LeavesOutFormatsTheImporterCannotRead) {
	EXPECT_EQ(FileBrowser::ClassifyExtension(".mp3"), FileKind::Unsupported);
	EXPECT_EQ(FileBrowser::ClassifyExtension(".flac"), FileKind::Unsupported);
	EXPECT_EQ(FileBrowser::ClassifyExtension(".txt"), FileKind::Unsupported);
	EXPECT_EQ(FileBrowser::ClassifyExtension(""), FileKind::Unsupported);
}

// ---- roots ----

TEST_F(FileBrowserTest, TakesADirectoryAsARootAndRefusesAnythingElse) {
	MakeDirectory("Samples");
	WriteFile("Samples/kick.wav");

	FileBrowser browser;
	EXPECT_TRUE(browser.AddRoot(PathOf("Samples")));
	EXPECT_FALSE(browser.AddRoot(PathOf("Samples/kick.wav"))) << "a file is not a folder to browse";
	EXPECT_FALSE(browser.AddRoot(PathOf("NotThere")));
	EXPECT_EQ(browser.GetRoots().size(), 1u);
}

// dropping the same folder in twice is a natural thing to do, and the second one
// must not produce a duplicate tree - including when it arrives spelled differently
TEST_F(FileBrowserTest, DropsARootItAlreadyHas) {
	MakeDirectory("Samples");

	FileBrowser browser;
	ASSERT_TRUE(browser.AddRoot(PathOf("Samples")));
	EXPECT_FALSE(browser.AddRoot(PathOf("Samples")));
	EXPECT_FALSE(browser.AddRoot(PathOf("Samples") + "/"));
	EXPECT_FALSE(browser.AddRoot(PathOf("Samples/../Samples")));
	EXPECT_EQ(browser.GetRoots().size(), 1u);
}

// the roots come back from the config file, where a folder may have been deleted or
// unplugged since it was written
TEST_F(FileBrowserTest, KeepsOnlyTheRootsThatStillExist) {
	MakeDirectory("Samples");

	FileBrowser browser;
	browser.SetRoots({PathOf("Samples"), PathOf("Gone"), PathOf("Samples")});

	ASSERT_EQ(browser.GetRoots().size(), 1u);
	EXPECT_EQ(fs::path(browser.GetRoots()[0]).filename().string(), "Samples");
}

TEST_F(FileBrowserTest, RemovesARootByIndexAndIgnoresOneOutOfRange) {
	MakeDirectory("A");
	MakeDirectory("B");

	FileBrowser browser;
	browser.SetRoots({PathOf("A"), PathOf("B")});

	browser.RemoveRoot(7);
	EXPECT_EQ(browser.GetRoots().size(), 2u);

	browser.RemoveRoot(0);
	ASSERT_EQ(browser.GetRoots().size(), 1u);
	EXPECT_EQ(fs::path(browser.GetRoots()[0]).filename().string(), "B");
}

// ---- listing ----

TEST_F(FileBrowserTest, ListsOnlyImportableFilesAndPutsFoldersFirst) {
	MakeDirectory("Loops");
	WriteFile("readme.txt");
	WriteFile("cover.png");
	WriteFile("snare.wav");
	WriteFile("bassline.mid");
	WriteFile("Track.msdaw");

	FileBrowser browser;
	const auto& entries = browser.List(RootPath());

	ASSERT_EQ(entries.size(), 4u) << "the two files nothing can be done with should not be listed";
	EXPECT_EQ(entries[0].kind, FileKind::Folder);
	EXPECT_EQ(entries[0].name, "Loops");

	// files after the folders, alphabetically among themselves
	EXPECT_EQ(entries[1].name, "bassline.mid");
	EXPECT_EQ(entries[1].kind, FileKind::MIDI);
	EXPECT_EQ(entries[2].name, "snare.wav");
	EXPECT_EQ(entries[2].kind, FileKind::Audio);
	EXPECT_EQ(entries[3].name, "Track.msdaw");
	EXPECT_EQ(entries[3].kind, FileKind::Project);
}

TEST_F(FileBrowserTest, ListsNothingForAFolderThatIsNotThere) {
	FileBrowser browser;
	EXPECT_TRUE(browser.List(PathOf("Missing")).empty());
}

// the tree asks for the same folders every frame it is open, so a listing is read
// once and held until something asks for it again
TEST_F(FileBrowserTest, ServesACachedListingUntilItIsRefreshed) {
	WriteFile("kick.wav");

	FileBrowser browser;
	ASSERT_EQ(browser.List(RootPath()).size(), 1u);

	WriteFile("snare.wav");
	EXPECT_EQ(browser.List(RootPath()).size(), 1u) << "the cache should not have noticed the new file";

	browser.Refresh(RootPath());
	EXPECT_EQ(browser.List(RootPath()).size(), 2u);

	WriteFile("hat.wav");
	browser.Refresh();
	EXPECT_EQ(browser.List(RootPath()).size(), 3u);
}

// a listing handed out while the tree recurses into a child must survive the
// listings that recursion reads
TEST_F(FileBrowserTest, KeepsAnEarlierListingValidWhileDeeperOnesAreRead) {
	WriteFile("kick.wav");
	WriteFile("Deep/One/two.wav");

	FileBrowser browser;
	const auto& top = browser.List(RootPath());
	ASSERT_EQ(top.size(), 2u);
	const std::string firstName = top[0].name;

	browser.List(PathOf("Deep"));
	browser.List(PathOf("Deep/One"));

	EXPECT_EQ(top.size(), 2u);
	EXPECT_EQ(top[0].name, firstName);
}

// ---- search ----

TEST_F(FileBrowserTest, FindsImportableFilesAnywhereUnderTheRoots) {
	MakeDirectory("Kits");
	WriteFile("Kits/Acoustic/big kick.wav");
	WriteFile("Kits/Electronic/kick 808.wav");
	WriteFile("Kits/Electronic/snare.wav");
	WriteFile("Kits/notes about kicks.txt");

	FileBrowser browser;
	ASSERT_TRUE(browser.AddRoot(RootPath()));

	browser.Search("kick");
	ASSERT_TRUE(WaitForSearch(browser)) << "the search never cleared its in-flight flag";

	const auto results = browser.GetSearchResults();
	ASSERT_EQ(results.size(), 2u) << "the text file matches the query but is not importable";
	EXPECT_EQ(results[0].name, "big kick.wav");
	EXPECT_EQ(results[1].name, "kick 808.wav");
	EXPECT_FALSE(browser.SearchTruncated());
}

TEST_F(FileBrowserTest, MatchesRegardlessOfCase) {
	WriteFile("Deep/BIG KICK.wav");

	FileBrowser browser;
	ASSERT_TRUE(browser.AddRoot(RootPath()));

	browser.Search("kick");
	ASSERT_TRUE(WaitForSearch(browser));
	EXPECT_EQ(browser.GetSearchResults().size(), 1u);
}

// the panel goes back to the tree when the query does, and a cancelled search must
// not leave its results behind for the next one
TEST_F(FileBrowserTest, ForgetsTheQueryAndItsResultsWhenCancelled) {
	WriteFile("kick.wav");

	FileBrowser browser;
	ASSERT_TRUE(browser.AddRoot(RootPath()));

	browser.Search("kick");
	ASSERT_TRUE(WaitForSearch(browser));
	ASSERT_FALSE(browser.GetSearchQuery().empty());

	browser.CancelSearch();
	EXPECT_TRUE(browser.GetSearchQuery().empty());
	EXPECT_FALSE(browser.IsSearching());

	browser.Search("");
	EXPECT_TRUE(browser.GetSearchResults().empty());
	EXPECT_FALSE(browser.IsSearching());
}

TEST_F(FileBrowserTest, SurvivesASecondQueryArrivingOnTopOfTheFirst) {
	for (int i = 0; i < 40; ++i)
		WriteFile("Deep/kick " + std::to_string(i) + ".wav");

	FileBrowser browser;
	ASSERT_TRUE(browser.AddRoot(RootPath()));

	browser.Search("kick");
	browser.Search("kick 1");
	ASSERT_TRUE(WaitForSearch(browser));

	// 1, 10-19: everything whose name carries the query
	EXPECT_EQ(browser.GetSearchResults().size(), 11u);
	EXPECT_EQ(browser.GetSearchQuery(), "kick 1");
}

// ---- names the code page cannot spell ----

// the crash this guards: a sample folder holding one Cyrillic, CJK or emoji filename
// took the whole DAW down the moment a search walked past it, because
// std::filesystem::path::string() throws there and a background thread that throws
// is std::terminate
TEST_F(FileBrowserTest, ListsFilesTheAnsiCodePageCannotSpell) {
	WriteWideFile(L"кик.wav");			 // "кик"
	WriteWideFile(L"キック.wav");			 // "キック"
	WriteWideFile(L"kick😀.wav");				 // an emoji
	WriteWideFile(L"plain.wav");

	FileBrowser browser;
	const auto& entries = browser.List(RootPath());

	EXPECT_EQ(entries.size(), 4u);
	for (const auto& entry : entries) {
		EXPECT_EQ(entry.kind, FileKind::Audio);
		EXPECT_FALSE(entry.name.empty());
		// UTF-8 out, which is what the panel draws and what reopens the file
		EXPECT_EQ(PathText::ToPath(entry.path).filename(), PathText::ToPath(entry.name));
	}
}

TEST_F(FileBrowserTest, SearchesPastFilesTheAnsiCodePageCannotSpell) {
	WriteWideFile(L"Deep/кик.wav");
	WriteWideFile(L"Deep/キック.wav");
	WriteWideFile(L"Deep/kick😀.wav");
	WriteWideFile(L"Deep/kick plain.wav");

	FileBrowser browser;
	ASSERT_TRUE(browser.AddRoot(RootPath()));

	browser.Search("kick");
	ASSERT_TRUE(WaitForSearch(browser)) << "the sweep died on a name it could not convert";

	const auto results = browser.GetSearchResults();
	ASSERT_EQ(results.size(), 2u) << "both files whose name carries the query, and no crash getting there";
	EXPECT_EQ(results[0].name, "kick plain.wav");
}

// a root whose own name is outside the code page is still a root, and still lists
TEST_F(FileBrowserTest, TakesARootTheAnsiCodePageCannotSpell) {
	const fs::path folder = mRoot / L"Сэмплы"; // "Сэмплы"
	std::error_code ec;
	fs::create_directories(folder, ec);
	{
		std::ofstream out(folder / "kick.wav");
		out << "x";
	}

	FileBrowser browser;
	const std::string root = PathText::FromPath(folder);
	ASSERT_TRUE(browser.AddRoot(root));
	EXPECT_FALSE(browser.AddRoot(root)) << "the same root normalized twice must still compare equal";
	ASSERT_EQ(browser.GetRoots().size(), 1u);

	const auto& entries = browser.List(browser.GetRoots()[0]);
	ASSERT_EQ(entries.size(), 1u);
	EXPECT_EQ(entries[0].name, "kick.wav");
}
