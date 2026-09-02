#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "Processors/RackProcessor.h"
#include "ProcessorFactory.h"
#include "Project.h"
#include "Track.h"
#include "Undo/UndoManager.h"
#include "Views/DeviceRackOps.h"

// ================================================================
// RACKS
// ================================================================
// a rack is a device that contains devices: parallel chains summed together, and a
// bank of macros driving parameters anywhere inside it. everything below is the
// mechanism - what grouping moves where, what a macro writes, what survives a save
// and what an undo puts back. the panels that drive it are verified in the app

namespace {

	// a block of `frames` stereo frames, every sample the same value
	std::vector<float> MakeBlock(int frames, float value) {
		return std::vector<float>((size_t)frames * 2, value);
	}

	ProcessContext MakeContext() {
		ProcessContext context;
		context.sampleRate = 48000.0;
		context.isPlaying = false;
		return context;
	}

	void RunBlock(AudioProcessor& processor, std::vector<float>& buffer) {
		std::vector<MIDIMessage> messages;
		processor.Process(buffer.data(), (int)buffer.size() / 2, 2, messages, MakeContext());
	}

	std::shared_ptr<AudioProcessor> MakeDevice() {
		return ProcessorFactory::Instance().Create("BitCrusher");
	}

	// a track carrying three plain devices, the shape every grouping test starts from
	std::shared_ptr<Track> MakeTrackWithDevices(Project& project, int count) {
		project.CreateTrack();
		auto track = project.GetTracks().back();
		for (int i = 0; i < count; ++i)
			track->AddProcessor(MakeDevice());
		return track;
	}

	Parameter* FindParameterNamed(AudioProcessor& device, const char* name) {
		for (const auto& parameter : device.GetParameters()) {
			if (parameter->name == name)
				return parameter.get();
		}
		return nullptr;
	}

} // namespace

// ---- registration ----

TEST(Rack, RegistersWithTheFactory) {
	EXPECT_TRUE(ProcessorFactory::Instance().IsRegistered("Rack"));
	EXPECT_NE(ProcessorFactory::Instance().Create("Rack"), nullptr);
}

// a rack is an instrument exactly when it holds one, because that is what decides
// whether the track sends it note-offs and whether the track can play notes at all
TEST(Rack, CountsAsAnInstrumentOnlyWhenItHoldsOne) {
	RackProcessor rack;
	auto chain = rack.AddChain("Chain");
	EXPECT_FALSE(rack.IsInstrument());

	chain->AddProcessor(MakeDevice());
	EXPECT_FALSE(rack.IsInstrument());
}

// ---- signal flow ----

TEST(Rack, AnEmptyRackPassesAudioThrough) {
	RackProcessor rack;
	auto buffer = MakeBlock(64, 0.25f);
	RunBlock(rack, buffer);

	EXPECT_FLOAT_EQ(buffer[0], 0.25f);
	EXPECT_FLOAT_EQ(buffer.back(), 0.25f);
}

// every chain is handed the same input and their outputs are summed, so two chains
// carrying the same signal come out at twice the level
TEST(Rack, ParallelChainsSumTheirOutputs) {
	RackProcessor rack;
	rack.AddChain("A");
	rack.AddChain("B");

	auto buffer = MakeBlock(64, 0.25f);
	RunBlock(rack, buffer);

	EXPECT_FLOAT_EQ(buffer[0], 0.5f);
}

TEST(Rack, AMutedChainContributesNothing) {
	RackProcessor rack;
	auto first = rack.AddChain("A");
	rack.AddChain("B");
	first->SetMute(true);

	auto buffer = MakeBlock(64, 0.25f);
	RunBlock(rack, buffer);

	EXPECT_FLOAT_EQ(buffer[0], 0.25f);
}

TEST(Rack, SoloingAChainSilencesTheOthers) {
	RackProcessor rack;
	auto first = rack.AddChain("A");
	rack.AddChain("B");
	rack.AddChain("C");
	first->SetSolo(true);

	auto buffer = MakeBlock(64, 0.25f);
	RunBlock(rack, buffer);

	EXPECT_FLOAT_EQ(buffer[0], 0.25f);
}

// a chain has its own fader, which is what makes two parallel branches blendable
TEST(Rack, AChainAppliesItsOwnLevel) {
	RackProcessor rack;
	auto chain = rack.AddChain("A");
	chain->GetVolumeParameter()->value = -6.0f;

	auto buffer = MakeBlock(64, 1.0f);
	RunBlock(rack, buffer);

	EXPECT_NEAR(buffer[0], 0.501f, 0.002f);
}

TEST(Rack, ARackNestedInAChainIsRunLikeAnyOtherDevice) {
	RackProcessor outer;
	auto outerChain = outer.AddChain("Outer");

	auto inner = std::make_shared<RackProcessor>();
	auto innerChain = inner->AddChain("Inner");
	innerChain->GetVolumeParameter()->value = -6.0f;
	outerChain->AddProcessor(inner);

	auto buffer = MakeBlock(64, 1.0f);
	RunBlock(outer, buffer);
	EXPECT_NEAR(buffer[0], 0.501f, 0.002f);

	// deactivating the nested rack takes its level out of the path entirely
	inner->SetBypassed(true);
	buffer = MakeBlock(64, 1.0f);
	RunBlock(outer, buffer);
	EXPECT_FLOAT_EQ(buffer[0], 1.0f);
}

// ---- macros ----

TEST(Rack, AMacroDrivesItsMappedParameterAcrossTheMappedRange) {
	RackProcessor rack;
	auto chain = rack.AddChain("Chain");
	auto device = MakeDevice();
	chain->AddProcessor(device);

	Parameter* bits = FindParameterNamed(*device, "Bits");
	ASSERT_NE(bits, nullptr);
	rack.MapMacro(0, device, "Bits", 4.0f, 16.0f);

	auto buffer = MakeBlock(8, 0.0f);
	rack.GetMacroParameter(0)->value = 0.0f;
	RunBlock(rack, buffer);
	EXPECT_FLOAT_EQ(bits->value, 4.0f);

	rack.GetMacroParameter(0)->value = RackProcessor::kMacroMax;
	RunBlock(rack, buffer);
	EXPECT_FLOAT_EQ(bits->value, 16.0f);
}

// min above max is a deliberate inverted mapping, not an error
TEST(Rack, AnInvertedMappingRunsBackwards) {
	RackProcessor rack;
	auto chain = rack.AddChain("Chain");
	auto device = MakeDevice();
	chain->AddProcessor(device);
	Parameter* bits = FindParameterNamed(*device, "Bits");
	ASSERT_NE(bits, nullptr);

	rack.MapMacro(0, device, "Bits", 16.0f, 4.0f);

	auto buffer = MakeBlock(8, 0.0f);
	rack.GetMacroParameter(0)->value = RackProcessor::kMacroMax;
	RunBlock(rack, buffer);
	EXPECT_FLOAT_EQ(bits->value, 4.0f);
}

// one macro reaches as many parameters as it is pointed at, including inside a
// nested rack
TEST(Rack, OneMacroDrivesEveryParameterMappedToIt) {
	RackProcessor rack;
	auto chain = rack.AddChain("Chain");
	auto first = MakeDevice();
	auto nested = std::make_shared<RackProcessor>();
	auto nestedChain = nested->AddChain("Inner");
	auto second = MakeDevice();
	nestedChain->AddProcessor(second);
	chain->AddProcessor(first);
	chain->AddProcessor(nested);

	rack.MapMacro(0, first, "Bits", 4.0f, 16.0f);
	rack.MapMacro(0, second, "Bits", 8.0f, 24.0f);
	rack.GetMacroParameter(0)->value = RackProcessor::kMacroMax;

	auto buffer = MakeBlock(8, 0.0f);
	RunBlock(rack, buffer);

	EXPECT_FLOAT_EQ(FindParameterNamed(*first, "Bits")->value, 16.0f);
	EXPECT_FLOAT_EQ(FindParameterNamed(*second, "Bits")->value, 24.0f);
}

// a device dragged or deleted out of the rack must stop being driven, even while the
// mapping is still on the macro (an undo can bring the device back)
TEST(Rack, AMacroStopsDrivingADeviceThatHasLeftTheRack) {
	RackProcessor rack;
	auto chain = rack.AddChain("Chain");
	auto device = MakeDevice();
	chain->AddProcessor(device);
	Parameter* bits = FindParameterNamed(*device, "Bits");

	rack.MapMacro(0, device, "Bits", 4.0f, 16.0f);
	rack.GetMacroParameter(0)->value = RackProcessor::kMacroMax;

	auto buffer = MakeBlock(8, 0.0f);
	RunBlock(rack, buffer);
	ASSERT_FLOAT_EQ(bits->value, 16.0f);

	chain->RemoveProcessor(0);
	bits->value = 2.0f;
	RunBlock(rack, buffer);
	EXPECT_FLOAT_EQ(bits->value, 2.0f);

	// and picks it back up when the device returns
	chain->AddProcessor(device);
	RunBlock(rack, buffer);
	EXPECT_FLOAT_EQ(bits->value, 16.0f);
}

// macros are ordinary parameters on the rack, which is the whole reason they can be
// automated - and a device inside a rack has to stay automatable too
TEST(Rack, MacrosAndNestedDevicesAreOnTheTrackParameterList) {
	Project project;
	project.Initialize();
	auto track = MakeTrackWithDevices(project, 0);

	auto rack = std::make_shared<RackProcessor>();
	auto chain = rack->AddChain("Chain");
	chain->AddProcessor(MakeDevice());
	track->AddProcessor(rack);

	auto parameters = track->GetAllParameters();
	auto has = [&](Parameter* parameter) {
		return std::find(parameters.begin(), parameters.end(), parameter) != parameters.end();
	};

	EXPECT_TRUE(has(rack->GetMacroParameter(0)));
	EXPECT_TRUE(has(chain->GetVolumeParameter()));
	EXPECT_TRUE(has(FindParameterNamed(*chain->GetProcessors().front(), "Bits")));

	// and are reachable by name, which is how a saved automation curve rebinds
	EXPECT_EQ(track->FindParameter("Macro 1"), rack->GetMacroParameter(0));
	EXPECT_EQ(track->FindParameter(chain->GetVolumeParameter()->name), chain->GetVolumeParameter());
}

// ---- grouping ----

TEST(RackOps, GroupingLiftsTheDevicesIntoARackAtTheFirstSlotTheyHeld) {
	Project project;
	UndoManager undo;
	project.Initialize();
	auto track = MakeTrackWithDevices(project, 4);
	auto second = track->GetProcessors()[1];
	auto third = track->GetProcessors()[2];

	auto rack = DeviceRackOps::GroupDevices(&project, undo, track, {second, third});

	ASSERT_NE(rack, nullptr);
	ASSERT_EQ(track->GetProcessors().size(), 3u);
	EXPECT_EQ(track->GetProcessors()[1], rack);
	ASSERT_EQ(rack->GetChains().size(), 1u);
	EXPECT_EQ(rack->GetChains()[0]->GetProcessors(),
			  (std::vector<std::shared_ptr<AudioProcessor>>{second, third}));
}

TEST(RackOps, UndoingAGroupPutsTheDevicesBackWhereTheyWere) {
	Project project;
	UndoManager undo;
	project.Initialize();
	auto track = MakeTrackWithDevices(project, 3);
	const auto before = track->GetProcessors();

	DeviceRackOps::GroupDevices(&project, undo, track, {before[0], before[1]});
	ASSERT_EQ(track->GetProcessors().size(), 2u);

	undo.Undo();
	EXPECT_EQ(track->GetProcessors(), before);

	undo.Redo();
	EXPECT_EQ(track->GetProcessors().size(), 2u);
}

TEST(RackOps, UngroupingFlattensASingleChainRackBackIntoTheChainAroundIt) {
	Project project;
	UndoManager undo;
	project.Initialize();
	auto track = MakeTrackWithDevices(project, 3);
	const auto original = track->GetProcessors();

	auto rack = DeviceRackOps::GroupDevices(&project, undo, track, {original[0], original[1]});
	ASSERT_NE(rack, nullptr);
	ASSERT_TRUE(DeviceRackOps::CanUngroup(rack));

	auto released = DeviceRackOps::UngroupRack(&project, undo, track, rack);

	EXPECT_EQ(released.size(), 2u);
	EXPECT_EQ(track->GetProcessors(), original);
}

// two branches in parallel have no serial arrangement that sounds the same, so the
// command is refused rather than silently changing the sound
TEST(RackOps, ARackWithParallelChainsCannotBeUngrouped) {
	Project project;
	UndoManager undo;
	project.Initialize();
	auto track = MakeTrackWithDevices(project, 2);
	auto rack = DeviceRackOps::GroupDevices(&project, undo, track, {track->GetProcessors()[0]});
	ASSERT_NE(rack, nullptr);
	rack->AddChain("Second");

	EXPECT_FALSE(DeviceRackOps::CanUngroup(rack));
	EXPECT_TRUE(DeviceRackOps::UngroupRack(&project, undo, track, rack).empty());
	EXPECT_EQ(track->GetProcessors()[0], rack);
}

TEST(RackOps, ADeviceInsideARackIsLocatedInTheChainThatHoldsIt) {
	Project project;
	UndoManager undo;
	project.Initialize();
	auto track = MakeTrackWithDevices(project, 2);
	auto grouped = track->GetProcessors()[0];
	auto rack = DeviceRackOps::GroupDevices(&project, undo, track, {grouped});
	ASSERT_NE(rack, nullptr);

	auto location = DeviceRackOps::Locate(track, grouped);
	ASSERT_TRUE(location.IsValid());
	EXPECT_EQ(location.host, rack->GetChains()[0]);
	EXPECT_EQ(location.index, 0);
}

// dropping a rack into one of its own chains would build a cycle the audio thread
// walks forever
TEST(RackOps, ARackCannotBeMovedIntoItself) {
	Project project;
	UndoManager undo;
	project.Initialize();
	auto track = MakeTrackWithDevices(project, 2);
	auto rack = DeviceRackOps::GroupDevices(&project, undo, track, {track->GetProcessors()[0]});
	ASSERT_NE(rack, nullptr);
	auto chain = rack->GetChains()[0];
	const size_t chainSize = chain->GetProcessors().size();

	DeviceRackOps::MoveDevice(&project, undo, track, 0, chain, 0);

	EXPECT_EQ(track->GetProcessors()[0], rack);
	EXPECT_EQ(chain->GetProcessors().size(), chainSize);
}

TEST(RackOps, DeletingASelectionSpanningARackTakesEachDeviceOutOfItsOwnChain) {
	Project project;
	UndoManager undo;
	project.Initialize();
	auto track = MakeTrackWithDevices(project, 3);
	auto grouped = track->GetProcessors()[0];
	auto rack = DeviceRackOps::GroupDevices(&project, undo, track, {grouped});
	ASSERT_NE(rack, nullptr);
	auto onTrack = track->GetProcessors()[1];

	DeviceRackOps::RemoveDevices(&project, undo, track, {grouped, onTrack});

	EXPECT_TRUE(rack->GetChains()[0]->GetProcessors().empty());
	EXPECT_EQ(track->GetProcessors().size(), 2u);

	// one entry in the history, covering both chains
	undo.Undo();
	EXPECT_EQ(rack->GetChains()[0]->GetProcessors().size(), 1u);
	EXPECT_EQ(track->GetProcessors().size(), 3u);
}

TEST(RackOps, TogglingAMixedSelectionDeactivatesAllOfIt) {
	Project project;
	UndoManager undo;
	project.Initialize();
	auto track = MakeTrackWithDevices(project, 3);
	auto devices = track->GetProcessors();
	devices[0]->SetBypassed(true);

	DeviceRackOps::ToggleDevicesBypassed(&project, undo, devices);
	for (const auto& device : devices)
		EXPECT_TRUE(device->IsBypassed());

	// everything off, so the next press turns everything back on
	DeviceRackOps::ToggleDevicesBypassed(&project, undo, devices);
	for (const auto& device : devices)
		EXPECT_FALSE(device->IsBypassed());

	undo.Undo();
	for (const auto& device : devices)
		EXPECT_TRUE(device->IsBypassed());
}

// the selection is held by pointer and outlives the indices around it, but not the
// device leaving the track
TEST(RackOps, TheSelectionDropsDevicesThatHaveLeftTheTrack) {
	Project project;
	UndoManager undo;
	EditorState state;
	project.Initialize();
	auto track = MakeTrackWithDevices(project, 2);
	state.SetDeviceSelection(track->GetProcessors());

	track->RemoveProcessor(0);
	DeviceRackOps::PruneSelection(state, track);

	ASSERT_EQ(state.selectedDevices.size(), 1u);
	EXPECT_EQ(state.selectedDevices[0], track->GetProcessors()[0]);
	EXPECT_EQ(state.selectedDevice, track->GetProcessors()[0]);
}

TEST(RackOps, ShiftClickSelectsTheRunBetweenTheFocusAndTheClickedDevice) {
	Project project;
	EditorState state;
	project.Initialize();
	auto track = MakeTrackWithDevices(project, 4);
	auto devices = track->GetProcessors();

	state.SelectDevice(devices[3]);
	DeviceRackOps::SelectRangeTo(state, track, devices[1]);

	EXPECT_EQ(state.selectedDevices.size(), 3u);
	EXPECT_TRUE(state.IsDeviceSelected(devices[1]));
	EXPECT_TRUE(state.IsDeviceSelected(devices[2]));
	EXPECT_TRUE(state.IsDeviceSelected(devices[3]));
	EXPECT_FALSE(state.IsDeviceSelected(devices[0]));
}

// ---- copies ----

// a copied rack is a copy all the way down, and its macros drive the copy rather than
// the devices of the rack that was copied
TEST(Rack, CopyingARackRepointsItsMacrosAtTheCopy) {
	RackProcessor source;
	auto chain = source.AddChain("Chain");
	auto device = MakeDevice();
	chain->AddProcessor(device);
	source.MapMacro(0, device, "Bits", 4.0f, 16.0f);
	source.GetMacroParameter(0)->value = RackProcessor::kMacroMax;

	RackProcessor copy;
	copy.CopyStateFrom(source);
	copy.GetMacroParameter(0)->value = RackProcessor::kMacroMax;

	ASSERT_EQ(copy.GetChains().size(), 1u);
	auto copiedDevice = copy.GetChains()[0]->GetProcessors().at(0);
	ASSERT_NE(copiedDevice, device);

	Parameter* originalBits = FindParameterNamed(*device, "Bits");
	Parameter* copiedBits = FindParameterNamed(*copiedDevice, "Bits");
	originalBits->value = 1.0f;
	copiedBits->value = 1.0f;

	auto buffer = MakeBlock(8, 0.0f);
	RunBlock(copy, buffer);

	EXPECT_FLOAT_EQ(copiedBits->value, 16.0f);
	EXPECT_FLOAT_EQ(originalBits->value, 1.0f);
}
