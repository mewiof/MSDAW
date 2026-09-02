#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "Clips/MIDIClip.h"
#include "ProcessorFactory.h"
#include "Processors/RackProcessor.h"
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

// ================================================================
// RACKS
// ================================================================
// a rack is nested devices, a chain list and a bank of macro mappings, all of which
// have to come back pointing at each other. the mappings are the sharp part: they
// name their target by its path inside the rack, and the devices at those paths do
// not exist yet while the mapping lines are being read

namespace {

	// a rack with two chains, a device in the first, and macro 1 mapped to it
	std::shared_ptr<RackProcessor> MakeSavedRack() {
		auto rack = std::make_shared<RackProcessor>();
		rack->SetName("Bass Grit");
		rack->SetColor(0xFF3366CCu);
		rack->SetVisibleMacroCount(12);

		auto first = rack->AddChain("Dirt");
		first->SetColor(0xFF11AA22u);
		first->GetVolumeParameter()->value = -4.5f;
		first->SetSolo(true);
		auto device = ProcessorFactory::Instance().Create("BitCrusher");
		first->AddProcessor(device);

		auto second = rack->AddChain("Clean");
		second->SetMute(true);

		rack->GetMacroMutable(0).title = "Grit";
		rack->GetMacroMutable(0).color = 0xFFAA5511u;
		rack->MapMacro(0, device, "Bits", 4.0f, 16.0f);
		rack->GetMacroParameter(0)->value = RackProcessor::kMacroMax;
		return rack;
	}

	std::shared_ptr<RackProcessor> LoadedRack(Project& project, int trackIndex) {
		auto& devices = project.GetTracks()[trackIndex]->GetProcessors();
		return devices.empty() ? nullptr : std::dynamic_pointer_cast<RackProcessor>(devices.front());
	}

} // namespace

TEST_F(ProjectSerializationTest, RackChainsAndTheirDevicesRoundTrip) {
	Project saved;
	saved.Initialize();
	saved.CreateTrack();
	saved.GetTracks()[0]->AddProcessor(MakeSavedRack());
	saved.Save(mPath.string());

	Project loaded;
	loaded.Initialize();
	loaded.Load(mPath.string());

	auto rack = LoadedRack(loaded, 0);
	ASSERT_NE(rack, nullptr);
	EXPECT_EQ(std::string(rack->GetName()), "Bass Grit");
	EXPECT_EQ(rack->GetColor(), 0xFF3366CCu);
	EXPECT_EQ(rack->GetVisibleMacroCount(), 12);

	ASSERT_EQ(rack->GetChains().size(), 2u);
	auto first = rack->GetChains()[0];
	EXPECT_EQ(first->GetName(), "Dirt");
	EXPECT_EQ(first->GetColor(), 0xFF11AA22u);
	EXPECT_TRUE(first->GetSolo());
	EXPECT_FLOAT_EQ(first->GetVolumeParameter()->value, -4.5f);
	ASSERT_EQ(first->GetProcessors().size(), 1u);
	EXPECT_EQ(first->GetProcessors()[0]->GetProcessorId(), "BitCrusher");

	EXPECT_EQ(rack->GetChains()[1]->GetName(), "Clean");
	EXPECT_TRUE(rack->GetChains()[1]->GetMute());
}

TEST_F(ProjectSerializationTest, MacroTitlesColorsAndMappingsRoundTrip) {
	Project saved;
	saved.Initialize();
	saved.CreateTrack();
	saved.GetTracks()[0]->AddProcessor(MakeSavedRack());
	saved.Save(mPath.string());

	Project loaded;
	loaded.Initialize();
	loaded.Load(mPath.string());

	auto rack = LoadedRack(loaded, 0);
	ASSERT_NE(rack, nullptr);
	EXPECT_EQ(rack->GetMacro(0).title, "Grit");
	EXPECT_EQ(rack->GetMacro(0).color, 0xFFAA5511u);
	ASSERT_EQ(rack->GetMacro(0).mappings.size(), 1u);
	EXPECT_EQ(rack->GetMacro(0).mappings[0].paramName, "Bits");
	EXPECT_FLOAT_EQ(rack->GetMacro(0).mappings[0].minValue, 4.0f);
	EXPECT_FLOAT_EQ(rack->GetMacro(0).mappings[0].maxValue, 16.0f);

	// the mapping is not just data: it has to reach the device it named
	auto device = rack->GetChains()[0]->GetProcessors()[0];
	Parameter* bits = nullptr;
	for (const auto& parameter : device->GetParameters()) {
		if (parameter->name == "Bits")
			bits = parameter.get();
	}
	ASSERT_NE(bits, nullptr);
	bits->value = 1.0f;

	std::vector<float> block(64, 0.0f);
	std::vector<MIDIMessage> messages;
	ProcessContext context;
	rack->Process(block.data(), 32, 2, messages, context);
	EXPECT_FLOAT_EQ(bits->value, 16.0f);
}

// automation is bound by parameter name, and a device inside a rack has to be
// reachable by that walk or its curve is silently orphaned on load
TEST_F(ProjectSerializationTest, AutomationOnADeviceInsideARackRebindsOnLoad) {
	Project saved;
	saved.Initialize();
	saved.CreateTrack();
	auto rack = MakeSavedRack();
	saved.GetTracks()[0]->AddProcessor(rack);

	Parameter* bits = rack->GetChains()[0]->GetProcessors()[0]->GetParameters()[0].get();
	saved.GetTracks()[0]->AddAutomationPoint(bits, 0.0, 8.0f);
	saved.GetTracks()[0]->AddAutomationPoint(bits, 4.0, 24.0f);
	saved.Save(mPath.string());

	Project loaded;
	loaded.Initialize();
	loaded.Load(mPath.string());

	auto loadedRack = LoadedRack(loaded, 0);
	ASSERT_NE(loadedRack, nullptr);
	Parameter* loadedBits = loadedRack->GetChains()[0]->GetProcessors()[0]->GetParameters()[0].get();

	auto* curve = loaded.GetTracks()[0]->GetAutomationCurve(loadedBits);
	ASSERT_NE(curve, nullptr);
	ASSERT_EQ(curve->points.size(), 2u);
	EXPECT_FLOAT_EQ(curve->Evaluate(4.0), 24.0f);
}
