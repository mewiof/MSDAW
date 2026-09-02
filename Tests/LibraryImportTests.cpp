#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "Clips/AudioClip.h"
#include "Library/LibraryImport.h"
#include "PathText.h"
#include "Project.h"
#include "Track.h"

// ================================================================
// LIBRARY IMPORT
// ================================================================

// the one road from a file on disk into the arrangement: the explorer's drag, its
// context menu and a drop from the OS all end up here, so what is tested is the
// landing - the clip's kind, its length in beats, and the track it ends up on

namespace fs = std::filesystem;

// NOTE: the fixture stays out of an anonymous namespace - TEST_F names it from file
// scope, and a fixture hidden in one is not the class the macro would find
class LibraryImportTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
		mRoot = fs::temp_directory_path() / ("MSDAWImport_" + std::string(info->name()));
		std::error_code ec;
		fs::remove_all(mRoot, ec);
		fs::create_directories(mRoot, ec);
	}

	void TearDown() override {
		std::error_code ec;
		fs::remove_all(mRoot, ec);
	}

	// a mono 16-bit wav of `frames` at 48 kHz. the samples are silence: what the
	// import is asked about is the length, not the content
	std::string WriteWav(const std::string& name, uint32_t frames) {
		const fs::path path = mRoot / name;
		std::ofstream out(path, std::ios::binary);
		auto put32 = [&](uint32_t v) { out.write((const char*)&v, 4); };
		auto put16 = [&](uint16_t v) { out.write((const char*)&v, 2); };
		const uint32_t dataBytes = frames * 2;
		out.write("RIFF", 4);
		put32(36 + dataBytes);
		out.write("WAVE", 4);
		out.write("fmt ", 4);
		put32(16);
		put16(1);
		put16(1);
		put32(48000);
		put32(48000 * 2);
		put16(2);
		put16(16);
		out.write("data", 4);
		put32(dataBytes);
		for (uint32_t i = 0; i < frames; ++i)
			put16(0);
		return path.string();
	}

	// the same wav, under a name the machine's ANSI code page cannot spell
	std::string WriteWideWav(const std::wstring& name, uint32_t frames) {
		const std::string ascii = WriteWav("scratch.wav", frames);
		const fs::path target = mRoot / name;
		std::error_code ec;
		fs::rename(PathText::ToPath(ascii), target, ec);
		return PathText::FromPath(target);
	}

	std::string WriteText(const std::string& name) {
		const fs::path path = mRoot / name;
		std::ofstream out(path);
		out << "not audio";
		return path.string();
	}

	fs::path mRoot;
};

// two seconds at 48 kHz is four beats at 120 bpm, and eight at 240: beats are the
// authoring unit, so the tempo the project is at decides how long the clip reads
TEST_F(LibraryImportTest, SizesAnAudioClipAgainstTheProjectTempo) {
	const std::string path = WriteWav("two-seconds.wav", 96000);

	auto atOneTwenty = LibraryImport::MakeClip(path, 120.0);
	ASSERT_NE(atOneTwenty, nullptr);
	EXPECT_DOUBLE_EQ(atOneTwenty->GetDuration(), 4.0);
	EXPECT_EQ(atOneTwenty->GetName(), "two-seconds.wav");

	auto atTwoForty = LibraryImport::MakeClip(path, 240.0);
	ASSERT_NE(atTwoForty, nullptr);
	EXPECT_DOUBLE_EQ(atTwoForty->GetDuration(), 8.0);
}

TEST_F(LibraryImportTest, MakesNothingOfAFileTheDAWDoesNotImport) {
	EXPECT_EQ(LibraryImport::MakeClip(WriteText("readme.txt"), 120.0), nullptr);
	EXPECT_EQ(LibraryImport::MakeClip(WriteText("song.msdaw"), 120.0), nullptr) << "a project is opened, not imported";
	EXPECT_EQ(LibraryImport::MakeClip((mRoot / "gone.wav").string(), 120.0), nullptr);
	// the extension says audio but there is no RIFF header behind it
	EXPECT_EQ(LibraryImport::MakeClip(WriteText("broken.wav"), 120.0), nullptr);
}

TEST_F(LibraryImportTest, PutsTheClipOnTheTrackAtTheBeatItWasDroppedOn) {
	Project project;
	project.Initialize();
	project.SetBpm(120.0);
	project.CreateTrack();
	auto track = project.GetTracks().back();
	const size_t clipsBefore = track->GetClips().size();

	ASSERT_TRUE(LibraryImport::ImportToTrack(&project, track, WriteWav("kick.wav", 48000), 6.5));

	ASSERT_EQ(track->GetClips().size(), clipsBefore + 1);
	auto clip = track->GetClips().back();
	EXPECT_DOUBLE_EQ(clip->GetStartBeat(), 6.5);
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 2.0);
	EXPECT_NE(std::dynamic_pointer_cast<AudioClip>(clip), nullptr);
}

TEST_F(LibraryImportTest, RefusesATrackThatHoldsNoClips) {
	Project project;
	project.Initialize();
	project.CreateTrack();
	auto track = project.GetTracks().back();
	track->SetGroup(true); // a group has no clip lane of its own

	EXPECT_FALSE(LibraryImport::ImportToTrack(&project, track, WriteWav("kick.wav", 48000), 0.0));
	EXPECT_TRUE(track->GetClips().empty());
}

TEST_F(LibraryImportTest, NamesTheNewTrackAfterTheFile) {
	Project project;
	project.Initialize();
	project.SetBpm(120.0);
	const int tracksBefore = (int)project.GetTracks().size();

	const int index = LibraryImport::ImportToNewTrack(&project, WriteWav("Deep Kick.wav", 48000), 2.0);

	ASSERT_EQ(index, tracksBefore);
	ASSERT_EQ((int)project.GetTracks().size(), tracksBefore + 1);
	auto track = project.GetTracks()[index];
	EXPECT_EQ(track->GetName(), "Deep Kick");
	ASSERT_EQ(track->GetClips().size(), 1u);
	EXPECT_DOUBLE_EQ(track->GetClips()[0]->GetStartBeat(), 2.0);
}

// the file is read before the track is made, so a file that turns out not to load
// leaves no empty track behind for the user to clean up
TEST_F(LibraryImportTest, LeavesNoTrackBehindWhenTheFileDoesNotLoad) {
	Project project;
	project.Initialize();
	const size_t tracksBefore = project.GetTracks().size();

	EXPECT_EQ(LibraryImport::ImportToNewTrack(&project, WriteText("cover.png"), 0.0), -1);
	EXPECT_EQ(project.GetTracks().size(), tracksBefore);
}

// a sample whose name is outside the code page still loads: the browser hands the
// path over as UTF-8 and the loader opens it as a path rather than as ANSI bytes
TEST_F(LibraryImportTest, ImportsAFileTheAnsiCodePageCannotSpell) {
	Project project;
	project.Initialize();
	project.SetBpm(120.0);

	const std::string path = WriteWideWav(L"кик.wav", 48000); // "кик.wav"

	auto clip = LibraryImport::MakeClip(path, 120.0);
	ASSERT_NE(clip, nullptr) << "the loader could not open a path it was handed as UTF-8";
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 2.0);

	const int index = LibraryImport::ImportToNewTrack(&project, path, 0.0);
	ASSERT_GE(index, 0);
	EXPECT_FALSE(project.GetTracks()[index]->GetName().empty());
}
