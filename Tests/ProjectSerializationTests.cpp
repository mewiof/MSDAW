#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "Clips/MIDIClip.h"
#include "Project.h"

// ================================================================
// PROJECT SERIALIZATION
// ================================================================

namespace {

	// the format is hand-rolled and line-oriented, so a round-trip through a
	// real file on disk is the only honest way to drive it
	class ProjectSerializationTest : public ::testing::Test {
	protected:
		void SetUp() override {
			const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
			mPath = std::filesystem::temp_directory_path() /
					(std::string("msdaw-") + info->name() + ".msdaw");
			std::filesystem::remove(mPath);
		}

		void TearDown() override {
			std::filesystem::remove(mPath);
		}

		std::filesystem::path mPath;
	};

} // namespace

TEST_F(ProjectSerializationTest, SavedFileCarriesAVersionHeader) {
	Project project;
	project.Initialize();
	project.Save(mPath.string());

	ASSERT_TRUE(std::filesystem::exists(mPath));

	std::ifstream in(mPath);
	std::string first;
	std::getline(in, first);

	EXPECT_EQ(first, "PROJECT_BEGIN");
}

TEST_F(ProjectSerializationTest, TrackCountAndNamesRoundTrip) {
	Project saved;
	saved.Initialize();
	saved.CreateTrack();
	saved.CreateTrack();
	saved.GetTracks()[0]->SetName("Drums");
	saved.GetTracks()[1]->SetName("Bass");
	saved.Save(mPath.string());

	Project loaded;
	loaded.Initialize();
	loaded.Load(mPath.string());

	ASSERT_EQ(loaded.GetTracks().size(), 2u);
	EXPECT_EQ(loaded.GetTracks()[0]->GetName(), "Drums");
	EXPECT_EQ(loaded.GetTracks()[1]->GetName(), "Bass");
}

TEST_F(ProjectSerializationTest, BpmRoundTrips) {
	Project saved;
	saved.Initialize();
	saved.SetBpm(93.5);
	saved.Save(mPath.string());

	Project loaded;
	loaded.Initialize();
	loaded.Load(mPath.string());

	EXPECT_DOUBLE_EQ(loaded.GetTransport().GetBpm(), 93.5);
}

// load clears before it reads, so loading into a populated project must not
// leave the previous project's tracks behind
TEST_F(ProjectSerializationTest, LoadReplacesRatherThanAppends) {
	Project saved;
	saved.Initialize();
	saved.CreateTrack();
	saved.Save(mPath.string());

	Project loaded;
	loaded.Initialize();
	loaded.CreateTrack();
	loaded.CreateTrack();
	loaded.CreateTrack();
	loaded.Load(mPath.string());

	EXPECT_EQ(loaded.GetTracks().size(), 1u);
}

// a missing file is a user mistake, not a crash, and it must not wipe the
// project that is already open
TEST_F(ProjectSerializationTest, LoadingAMissingFileLeavesTheProjectIntact) {
	Project project;
	project.Initialize();
	project.CreateTrack();

	project.Load((mPath.parent_path() / "msdaw-does-not-exist.msdaw").string());

	EXPECT_EQ(project.GetTracks().size(), 1u);
}

// activation lives on the clip, so two clips on one track must come back with the
// flags they were saved with
TEST_F(ProjectSerializationTest, ClipActivationRoundTrips) {
	Project saved;
	saved.Initialize();
	saved.CreateTrack();

	auto active = std::make_shared<MIDIClip>();
	active->SetName("On");
	active->SetStartBeat(0.0);
	active->SetDuration(4.0);

	auto deactivated = std::make_shared<MIDIClip>();
	deactivated->SetName("Off");
	deactivated->SetStartBeat(4.0);
	deactivated->SetDuration(4.0);
	deactivated->SetEnabled(false);

	saved.GetTracks()[0]->AddClip(active);
	saved.GetTracks()[0]->AddClip(deactivated);
	saved.Save(mPath.string());

	Project loaded;
	loaded.Initialize();
	loaded.Load(mPath.string());

	ASSERT_EQ(loaded.GetTracks().size(), 1u);
	const auto& clips = loaded.GetTracks()[0]->GetClips();
	ASSERT_EQ(clips.size(), 2u);
	EXPECT_TRUE(clips[0]->IsEnabled());
	EXPECT_FALSE(clips[1]->IsEnabled());
}

// version 1 files predate the flag entirely; those clips have to load as active
TEST_F(ProjectSerializationTest, AClipSavedWithoutAnActivationFlagLoadsActive) {
	Project saved;
	saved.Initialize();
	saved.CreateTrack();

	auto clip = std::make_shared<MIDIClip>();
	clip->SetEnabled(false);
	saved.GetTracks()[0]->AddClip(clip);
	saved.Save(mPath.string());

	// strip the ENABLED lines back out, leaving the file as an older version wrote it
	std::vector<std::string> lines;
	{
		std::ifstream in(mPath);
		std::string line;
		while (std::getline(in, line)) {
			if (line.rfind("ENABLED", 0) != 0)
				lines.push_back(line);
		}
	}
	{
		std::ofstream out(mPath, std::ios::trunc);
		for (const auto& line : lines)
			out << line << "\n";
	}

	Project loaded;
	loaded.Initialize();
	loaded.Load(mPath.string());

	ASSERT_EQ(loaded.GetTracks().size(), 1u);
	ASSERT_EQ(loaded.GetTracks()[0]->GetClips().size(), 1u);
	EXPECT_TRUE(loaded.GetTracks()[0]->GetClips()[0]->IsEnabled());
}

// ================================================================
// LINKED (NON-UNIQUE) MIDI CLIPS
// ================================================================

namespace {

	std::shared_ptr<MIDIClip> LoadedMIDIClip(Project& project, size_t trackIndex, size_t clipIndex) {
		return std::dynamic_pointer_cast<MIDIClip>(project.GetTracks()[trackIndex]->GetClips()[clipIndex]);
	}

} // namespace

// two clips playing one sequence have to come back playing one sequence, or every
// linked clip in a project silently becomes unique the first time it is reopened
TEST_F(ProjectSerializationTest, LinkedClipsStayLinkedAcrossASaveAndLoad) {
	Project saved;
	saved.Initialize();
	saved.CreateTrack();

	auto original = std::make_shared<MIDIClip>();
	original->AddNote({60, 100, 0.0, 1.0});
	auto ghost = std::make_shared<MIDIClip>(*original); // the clone Duplicate makes
	ghost->SetStartBeat(4.0);
	ASSERT_TRUE(original->IsLinkedTo(*ghost));

	saved.GetTracks()[0]->AddClip(original);
	saved.GetTracks()[0]->AddClip(ghost);
	saved.Save(mPath.string());

	Project loaded;
	loaded.Initialize();
	loaded.Load(mPath.string());

	ASSERT_EQ(loaded.GetTracks().size(), 1u);
	ASSERT_EQ(loaded.GetTracks()[0]->GetClips().size(), 2u);
	auto first = LoadedMIDIClip(loaded, 0, 0);
	auto second = LoadedMIDIClip(loaded, 0, 1);
	ASSERT_TRUE(first && second);
	EXPECT_TRUE(first->IsLinkedTo(*second));
	EXPECT_TRUE(first->IsSequenceShared());

	// and it is a real link, not two vectors that happen to match
	first->GetNotesEx()[0].noteNumber = 67;
	ASSERT_EQ(second->GetNotes().size(), 1u);
	EXPECT_EQ(second->GetNotes()[0].noteNumber, 67);
}

// the link spans tracks: a ghost dragged onto another lane is still the same material
TEST_F(ProjectSerializationTest, LinkedClipsStayLinkedAcrossTracks) {
	Project saved;
	saved.Initialize();
	saved.CreateTrack();
	saved.CreateTrack();

	auto original = std::make_shared<MIDIClip>();
	original->AddNote({48, 90, 0.0, 2.0});
	auto ghost = std::make_shared<MIDIClip>(*original);
	saved.GetTracks()[0]->AddClip(original);
	saved.GetTracks()[1]->AddClip(ghost);
	saved.Save(mPath.string());

	Project loaded;
	loaded.Initialize();
	loaded.Load(mPath.string());

	ASSERT_EQ(loaded.GetTracks().size(), 2u);
	auto first = LoadedMIDIClip(loaded, 0, 0);
	auto second = LoadedMIDIClip(loaded, 1, 0);
	ASSERT_TRUE(first && second);
	EXPECT_TRUE(first->IsLinkedTo(*second));
}

// the other half of the contract: a clip that was made unique must not come back
// sharing notes with the clip it was detached from
TEST_F(ProjectSerializationTest, AClipMadeUniqueReloadsUnique) {
	Project saved;
	saved.Initialize();
	saved.CreateTrack();

	auto original = std::make_shared<MIDIClip>();
	original->AddNote({60, 100, 0.0, 1.0});
	auto detached = std::make_shared<MIDIClip>(*original);
	detached->SetStartBeat(4.0);
	detached->MakeUnique();
	ASSERT_FALSE(original->IsLinkedTo(*detached));

	saved.GetTracks()[0]->AddClip(original);
	saved.GetTracks()[0]->AddClip(detached);
	saved.Save(mPath.string());

	Project loaded;
	loaded.Initialize();
	loaded.Load(mPath.string());

	auto first = LoadedMIDIClip(loaded, 0, 0);
	auto second = LoadedMIDIClip(loaded, 0, 1);
	ASSERT_TRUE(first && second);
	EXPECT_FALSE(first->IsLinkedTo(*second));
	EXPECT_FALSE(first->IsSequenceShared());
	EXPECT_FALSE(second->IsSequenceShared());
}

// projects written before SEQ existed have no ids at all; every clip in one has to
// load as its own material rather than collapsing onto a shared sequence
TEST_F(ProjectSerializationTest, ClipsSavedWithoutASequenceIdLoadUnique) {
	Project saved;
	saved.Initialize();
	saved.CreateTrack();

	auto original = std::make_shared<MIDIClip>();
	original->AddNote({60, 100, 0.0, 1.0});
	auto ghost = std::make_shared<MIDIClip>(*original);
	ghost->SetStartBeat(4.0);
	saved.GetTracks()[0]->AddClip(original);
	saved.GetTracks()[0]->AddClip(ghost);
	saved.Save(mPath.string());

	// strip the SEQ lines back out, leaving the file as an older version wrote it
	std::vector<std::string> lines;
	{
		std::ifstream in(mPath);
		std::string line;
		while (std::getline(in, line)) {
			if (line.rfind("SEQ", 0) != 0)
				lines.push_back(line);
		}
	}
	{
		std::ofstream out(mPath, std::ios::trunc);
		for (const auto& line : lines)
			out << line << "\n";
	}

	Project loaded;
	loaded.Initialize();
	loaded.Load(mPath.string());

	ASSERT_EQ(loaded.GetTracks()[0]->GetClips().size(), 2u);
	auto first = LoadedMIDIClip(loaded, 0, 0);
	auto second = LoadedMIDIClip(loaded, 0, 1);
	ASSERT_TRUE(first && second);
	EXPECT_FALSE(first->IsLinkedTo(*second));
}
