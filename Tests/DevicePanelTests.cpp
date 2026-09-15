#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ProcessorFactory.h"
#include "ProcessorIO.h"
#include "Project.h"
#include "Track.h"
#include "Undo/Actions.h"

// ================================================================
// DEVICE PANEL
// ================================================================

// a plugin publishes as many parameters as it likes - Surge XT publishes 2855 - and a
// panel listing all of them is unusable however fast it draws. a configured panel names
// the handful worth having on it, built by touching those controls in the plugin's own
// editor. these pin down the list itself: what it does to a device that has never been
// configured, that it survives a save and a copy, and that capture only takes what it
// was turned on for

namespace {

	std::shared_ptr<AudioProcessor> MakeDevice() {
		return ProcessorFactory::Instance().Create("BitCrusher");
	}

	// the panel is stored by ProcessorIO, not by the device, so a round-trip has to go
	// through the same two calls a track uses
	std::shared_ptr<AudioProcessor> RoundTrip(AudioProcessor& device) {
		std::stringstream stream;
		ProcessorIO::SaveProcessor(stream, device);

		std::string header;
		std::getline(stream, header); // "PROCESSOR <id>", which the caller consumes
		const std::string processorId = header.substr(header.find(' ') + 1);
		return ProcessorIO::LoadProcessor(stream, processorId);
	}

} // namespace

TEST(DevicePanel, ADeviceStartsShowingEverything) {
	auto device = MakeDevice();
	ASSERT_TRUE(device);

	// empty is not "show nothing", it is "never configured" - the panel draws the
	// whole parameter list, which is what every built-in device wants
	EXPECT_TRUE(device->GetPanelParameters().empty());
	EXPECT_FALSE(device->GetParameters().empty());
}

TEST(DevicePanel, AnUnconfiguredDeviceWritesNoPanelLine) {
	auto device = MakeDevice();
	ASSERT_TRUE(device);

	std::stringstream stream;
	ProcessorIO::SaveProcessor(stream, *device);

	EXPECT_EQ(stream.str().find("PROC_PANEL"), std::string::npos);
}

TEST(DevicePanel, AConfiguredPanelRoundTripsInOrder) {
	auto device = MakeDevice();
	ASSERT_TRUE(device);
	ASSERT_GE((int)device->GetParameters().size(), 3);

	// insertion order is the panel's order: it is the order the controls were touched
	device->SetPanelParameters({2, 0});

	auto loaded = RoundTrip(*device);
	ASSERT_TRUE(loaded);
	EXPECT_EQ(loaded->GetPanelParameters(), std::vector<int>({2, 0}));
}

TEST(DevicePanel, AScalingOverrideStillRoundTripsBesideAPanel) {
	auto device = MakeDevice();
	ASSERT_TRUE(device);

	// both are optional PROC_ lines in the same place, and the loader reads them in a
	// loop - so one must not eat the other
	device->SetEditorScalingMode(EditorScalingMode::Native);
	device->SetPanelParameters({1});

	auto loaded = RoundTrip(*device);
	ASSERT_TRUE(loaded);
	EXPECT_EQ(loaded->GetEditorScalingMode(), EditorScalingMode::Native);
	EXPECT_EQ(loaded->GetPanelParameters(), std::vector<int>({1}));
}

TEST(DevicePanel, ADeviceBlockWithoutOptionalLinesStillLoads) {
	// what a project saved before any of this looks like: the id, then the body
	std::stringstream stream;
	stream << "PARAMS_BEGIN\n";
	stream << "PARAMS_END\n";
	stream << "PROCESSOR_END\n";

	auto loaded = ProcessorIO::LoadProcessor(stream, "BitCrusher");
	ASSERT_TRUE(loaded);
	EXPECT_TRUE(loaded->GetPanelParameters().empty());
	EXPECT_EQ(loaded->GetEditorScalingMode(), EditorScalingMode::Default);
}

TEST(DevicePanel, CaptureOnlyTakesParametersWhileItIsOn) {
	auto device = MakeDevice();
	ASSERT_TRUE(device);
	ASSERT_GE((int)device->GetParameters().size(), 2);

	device->CapturePanelParameter(0); // capture off: ignored
	EXPECT_TRUE(device->GetPanelParameters().empty());

	device->SetPanelCapture(true);
	device->CapturePanelParameter(1);
	device->CapturePanelParameter(0);
	device->SetPanelCapture(false);

	device->CapturePanelParameter(1); // off again: ignored
	EXPECT_EQ(device->GetPanelParameters(), std::vector<int>({1, 0}));
}

TEST(DevicePanel, CaptureNeitherDuplicatesNorAcceptsAStrayIndex) {
	auto device = MakeDevice();
	ASSERT_TRUE(device);
	const int count = (int)device->GetParameters().size();

	device->SetPanelCapture(true);
	device->CapturePanelParameter(0);
	device->CapturePanelParameter(0); // a second touch of the same control
	device->CapturePanelParameter(count); // one past the end
	device->CapturePanelParameter(-1);

	EXPECT_EQ(device->GetPanelParameters(), std::vector<int>({0}));
}

TEST(DevicePanel, ACopiedDeviceKeepsItsPanel) {
	auto device = MakeDevice();
	ASSERT_TRUE(device);
	device->SetPanelParameters({1, 0});

	auto clone = ProcessorIO::CloneProcessor(device);
	ASSERT_TRUE(clone);
	EXPECT_EQ(clone->GetPanelParameters(), std::vector<int>({1, 0}));
}

TEST(DevicePanel, ConfiguringIsUndoneAsOneEntry) {
	auto device = MakeDevice();
	ASSERT_TRUE(device);

	// what a configuring session pushes: the list as it was when Add went on, and the
	// list as it stood when Add went off, however many controls were touched between
	std::vector<int> before = device->GetPanelParameters();
	device->SetPanelCapture(true);
	device->CapturePanelParameter(0);
	device->CapturePanelParameter(1);
	device->SetPanelCapture(false);

	DevicePanelAction action(device, before, device->GetPanelParameters(), "Configure device panel");
	ASSERT_EQ(device->GetPanelParameters(), std::vector<int>({0, 1}));

	action.Undo();
	EXPECT_TRUE(device->GetPanelParameters().empty());

	action.Redo();
	EXPECT_EQ(device->GetPanelParameters(), std::vector<int>({0, 1}));
}

// ================================================================
// WHAT THE AUTOMATION LANE OFFERS
// ================================================================

TEST(DevicePanel, TheLaneOffersEveryParameterOfAnUnconfigurableDevice) {
	Project project;
	project.Initialize();
	project.CreateTrack();
	auto track = project.GetTracks()[0];

	auto device = MakeDevice(); // a built-in: no editor of its own to configure from
	ASSERT_FALSE(device->HasEditor());
	track->AddProcessor(device);

	// every parameter it publishes is on offer, because its panel is the only way to
	// reach any of them in the first place
	std::vector<Parameter*> offered = track->GetPanelParameters();
	for (const auto& parameter : device->GetParameters())
		EXPECT_NE(std::find(offered.begin(), offered.end(), parameter.get()), offered.end());
}

TEST(DevicePanel, TheLaneNarrowsToAConfiguredPanel) {
	Project project;
	project.Initialize();
	project.CreateTrack();
	auto track = project.GetTracks()[0];

	auto device = MakeDevice();
	track->AddProcessor(device);
	device->SetPanelParameters({1});

	std::vector<Parameter*> offered = track->GetPanelParameters();
	const auto& parameters = device->GetParameters();
	EXPECT_NE(std::find(offered.begin(), offered.end(), parameters[1].get()), offered.end());
	EXPECT_EQ(std::find(offered.begin(), offered.end(), parameters[0].get()), offered.end());

	// narrowed, not lost: the full walk still reaches everything, which is what All
	// in the dropdown falls back to
	std::vector<Parameter*> all = track->GetAllParameters();
	EXPECT_NE(std::find(all.begin(), all.end(), parameters[0].get()), all.end());
}

TEST(DevicePanel, AnAutomatedParameterIsOfferedWhicheverPanelItIsOff) {
	Project project;
	project.Initialize();
	project.CreateTrack();
	auto track = project.GetTracks()[0];

	auto device = MakeDevice();
	track->AddProcessor(device);
	Parameter* drawnOn = device->GetParameters()[0].get();

	// a curve exists the moment a parameter is picked, so an empty one must not count
	track->GetAutomationCurve(drawnOn);
	EXPECT_TRUE(track->GetAutomatedParameters().empty());

	track->AddAutomationPoint(drawnOn, 0.0, 0.5);

	// configuring the device onto a panel that excludes it cannot strand the curve
	device->SetPanelParameters({1});
	std::vector<Parameter*> automated = track->GetAutomatedParameters();
	EXPECT_NE(std::find(automated.begin(), automated.end(), drawnOn), automated.end());
}

TEST(DevicePanel, APanelSurvivesAProjectSaveAndLoad) {
	const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
	std::filesystem::path path = std::filesystem::temp_directory_path() /
								 (std::string("msdaw-") + info->name() + ".msdaw");
	std::filesystem::remove(path);

	{
		Project saved;
		saved.Initialize();
		saved.CreateTrack();
		auto device = MakeDevice();
		device->SetPanelParameters({2, 1});
		saved.GetTracks()[0]->AddProcessor(device);
		saved.Save(path.string());
	}

	Project loaded;
	loaded.Load(path.string());
	ASSERT_FALSE(loaded.GetTracks().empty());
	ASSERT_FALSE(loaded.GetTracks()[0]->GetProcessors().empty());
	EXPECT_EQ(loaded.GetTracks()[0]->GetProcessors()[0]->GetPanelParameters(), std::vector<int>({2, 1}));

	std::filesystem::remove(path);
}
