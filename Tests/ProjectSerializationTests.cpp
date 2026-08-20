#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

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
