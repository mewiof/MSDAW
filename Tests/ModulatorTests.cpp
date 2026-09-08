#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <sstream>
#include <vector>

#include "ProcessorFactory.h"
#include "ProcessorIO.h"
#include "Processors/ModulatorProcessor.h"
#include "Processors/RackProcessor.h"
#include "Project.h"
#include "SidechainHub.h"
#include "Track.h"
#include "Undo/UndoManager.h"
#include "Views/DeviceRackOps.h"

// ================================================================
// MODULATOR
// ================================================================
// the device that makes no sound: it turns a generator into a value and writes that
// into other devices' parameters. everything below is the mechanism - what it leaves
// the audio alone, what it drives, what happens when the thing it drives goes away,
// and what survives a save. the panels that drive it are verified in the app

namespace {

	ProcessContext MakeContext(int64_t sample, bool playing) {
		ProcessContext context;
		context.sampleRate = 48000.0;
		context.bpm = 120.0;
		context.currentSample = sample;
		context.isPlaying = playing;
		return context;
	}

	void RunBlock(AudioProcessor& processor, std::vector<float>& buffer, const ProcessContext& context) {
		std::vector<MIDIMessage> messages;
		processor.Process(buffer.data(), (int)buffer.size() / 2, 2, messages, context);
	}

	Parameter* FindParameterNamed(AudioProcessor& device, const char* name) {
		for (const auto& parameter : device.GetParameters()) {
			if (parameter->name == name)
				return parameter.get();
		}
		return nullptr;
	}

	void SetParameter(AudioProcessor& device, const char* name, float value) {
		Parameter* parameter = FindParameterNamed(device, name);
		ASSERT_NE(parameter, nullptr) << name;
		parameter->value = value;
	}

	// the modulator addresses parameters through the live project, exactly the way a
	// device UI reaches the track list, so a test has to publish one first
	struct LiveProject {
		Project project;
		LiveProject() { SidechainHub::Instance().SetProject(&project); }
		~LiveProject() { SidechainHub::Instance().SetProject(nullptr); }
	};

	std::shared_ptr<ModulatorProcessor> MakeModulator() {
		return std::dynamic_pointer_cast<ModulatorProcessor>(ProcessorFactory::Instance().Create("Modulator"));
	}

} // namespace

// ---- registration ----

TEST(Modulator, RegistersWithTheFactory) {
	EXPECT_TRUE(ProcessorFactory::Instance().IsRegistered("Modulator"));
	EXPECT_NE(MakeModulator(), nullptr);
	EXPECT_FALSE(ProcessorFactory::Instance().IsInstrument("Modulator"));
}

// the whole point of the device: it is in the chain for what it does to parameters,
// never for what it does to the signal
TEST(Modulator, LeavesTheAudioExactlyAsItFoundIt) {
	ModulatorProcessor modulator;
	modulator.PrepareToPlay(48000.0);

	std::vector<float> buffer(256, 0.0f);
	for (size_t i = 0; i < buffer.size(); ++i)
		buffer[i] = std::sin((float)i * 0.1f);
	const std::vector<float> original = buffer;

	RunBlock(modulator, buffer, MakeContext(0, true));
	EXPECT_EQ(buffer, original);
}

// ---- generator ----

TEST(Modulator, ASyncedCycleFollowsTheTransportRatherThanAccumulating) {
	ModulatorProcessor modulator;
	modulator.PrepareToPlay(48000.0);
	SetParameter(modulator, "Mode", (float)ModulatorProcessor::ModeLFO);
	SetParameter(modulator, "Shape A", (float)ModulatorProcessor::ShapeSawUp);
	SetParameter(modulator, "XFade", 0.0f);
	SetParameter(modulator, "Sync", 1.0f);
	SetParameter(modulator, "Restart", 0.0f);
	SetParameter(modulator, "Division", 3.0f); // one bar, four beats = two seconds at 120 bpm

	std::vector<float> buffer(256, 0.0f);

	// a quarter of the way into the cycle, whether or not any block before it ran: the
	// phase is read off the transport, so a seek lands where it should
	RunBlock(modulator, buffer, MakeContext(24000, true));
	EXPECT_NEAR(modulator.GetVisualPhase(), 0.25f, 0.001f);

	RunBlock(modulator, buffer, MakeContext(72000, true));
	EXPECT_NEAR(modulator.GetVisualPhase(), 0.75f, 0.001f);
}

TEST(Modulator, TheStepperHoldsTheLevelDrawnForEachStep) {
	LiveProject live;
	live.project.CreateTrack();
	auto track = live.project.GetTracks().back();

	auto modulator = MakeModulator();
	auto sink = ProcessorFactory::Instance().Create("BitCrusher");
	track->AddProcessor(modulator);
	track->AddProcessor(sink);

	// a parameter whose own range contains 0..1, so the mapping under test is the one
	// the modulator computes rather than the clamp into the target's range
	Parameter* driven = FindParameterNamed(*sink, "Drive dB");
	ASSERT_NE(driven, nullptr);
	ASSERT_TRUE(modulator->AddTarget(&live.project, driven));

	SetParameter(*modulator, "Mode", (float)ModulatorProcessor::ModeStepper);
	SetParameter(*modulator, "Sync", 1.0f);
	SetParameter(*modulator, "Restart", 0.0f);
	SetParameter(*modulator, "Division", 3.0f); // one bar = two seconds at 120 bpm
	SetParameter(*modulator, "Steps", 2.0f);
	SetParameter(*modulator, "XFade", 0.0f);

	// the target's range is its own, so the written value is the step level mapped onto it
	ModulatorProcessor::Target& target = modulator->GetTargetsMutable().front();
	target.minValue = 0.0f;
	target.maxValue = 1.0f;

	ModulatorProcessor::State pattern = modulator->CaptureState();
	pattern.levels[0][0] = 0.25f;
	pattern.levels[0][1] = 0.75f;
	modulator->ApplyState(pattern);

	std::vector<float> buffer(64, 0.0f);
	RunBlock(*modulator, buffer, MakeContext(0, true));
	EXPECT_NEAR(driven->value, 0.25f, 0.001f);

	RunBlock(*modulator, buffer, MakeContext(72000, true)); // second half of the cycle
	EXPECT_NEAR(driven->value, 0.75f, 0.001f);
}

// a performer step is two things at once, the way the reference product's is: the shape
// it plays and the height it plays at. the same curve at half height is half the
// modulation, not a different curve
TEST(Modulator, APerformerStepPlaysItsCurveAtItsOwnHeight) {
	LiveProject live;
	live.project.CreateTrack();
	auto track = live.project.GetTracks().back();

	auto modulator = MakeModulator();
	auto sink = ProcessorFactory::Instance().Create("BitCrusher");
	track->AddProcessor(modulator);
	track->AddProcessor(sink);

	Parameter* driven = FindParameterNamed(*sink, "Drive dB");
	ASSERT_NE(driven, nullptr);
	ASSERT_TRUE(modulator->AddTarget(&live.project, driven));
	ModulatorProcessor::Target& target = modulator->GetTargetsMutable().front();
	target.minValue = 0.0f;
	target.maxValue = 1.0f;

	SetParameter(*modulator, "Mode", (float)ModulatorProcessor::ModePerformer);
	SetParameter(*modulator, "Steps", 2.0f);
	SetParameter(*modulator, "XFade", 0.0f);
	SetParameter(*modulator, "Sync", 1.0f);
	SetParameter(*modulator, "Restart", 0.0f);
	SetParameter(*modulator, "Division", 3.0f); // one bar = two seconds at 120 bpm

	// Hold reads 1 for the whole step, so what lands on the target is the height alone
	ModulatorProcessor::State pattern = modulator->CaptureState();
	pattern.curves[0][0] = ModulatorProcessor::CurveHold;
	pattern.curves[0][1] = ModulatorProcessor::CurveHold;
	pattern.heights[0][0] = 1.0f;
	pattern.heights[0][1] = 0.4f;
	modulator->ApplyState(pattern);

	std::vector<float> buffer(64, 0.0f);
	RunBlock(*modulator, buffer, MakeContext(0, true));
	EXPECT_NEAR(driven->value, 1.0f, 0.001f);

	RunBlock(*modulator, buffer, MakeContext(72000, true)); // second half of the cycle
	EXPECT_NEAR(driven->value, 0.4f, 0.001f);

	// a flattened step contributes nothing, whatever shape is drawn on it
	pattern = modulator->CaptureState();
	pattern.heights[0][1] = 0.0f;
	modulator->ApplyState(pattern);
	RunBlock(*modulator, buffer, MakeContext(72000, true));
	EXPECT_NEAR(driven->value, 0.0f, 0.001f);
}

// the two stepped modes describe the same idea but are not the same pattern: flipping
// modes to hear one has to leave the other exactly as it was
TEST(Modulator, ThePerformerAndTheStepperKeepSeparatePatterns) {
	ModulatorProcessor modulator;

	ModulatorProcessor::State pattern = modulator.CaptureState();
	pattern.levels[0][0] = 0.2f;
	pattern.heights[0][0] = 0.9f;
	modulator.ApplyState(pattern);

	SetParameter(modulator, "Mode", (float)ModulatorProcessor::ModePerformer);
	ModulatorProcessor::State edited = modulator.CaptureState();
	edited.heights[0][0] = 0.1f;
	modulator.ApplyState(edited);

	SetParameter(modulator, "Mode", (float)ModulatorProcessor::ModeStepper);
	EXPECT_NEAR(modulator.CaptureState().levels[0][0], 0.2f, 0.0001f);
}

// depth scales the swing and offset lifts the floor, so the two together are what a
// single modulator-wide "turn it all down" control has to be
TEST(Modulator, DepthAndOffsetShapeTheWrittenValue) {
	LiveProject live;
	live.project.CreateTrack();
	auto track = live.project.GetTracks().back();

	auto modulator = MakeModulator();
	auto sink = ProcessorFactory::Instance().Create("BitCrusher");
	track->AddProcessor(modulator);
	track->AddProcessor(sink);

	Parameter* driven = FindParameterNamed(*sink, "Drive dB");
	ASSERT_NE(driven, nullptr);
	ASSERT_TRUE(modulator->AddTarget(&live.project, driven));
	ModulatorProcessor::Target& target = modulator->GetTargetsMutable().front();
	target.minValue = 0.0f;
	target.maxValue = 1.0f;

	SetParameter(*modulator, "Mode", (float)ModulatorProcessor::ModeStepper);
	SetParameter(*modulator, "Steps", 1.0f);
	SetParameter(*modulator, "XFade", 0.0f);
	SetParameter(*modulator, "Sync", 1.0f);
	SetParameter(*modulator, "Restart", 0.0f);

	ModulatorProcessor::State pattern = modulator->CaptureState();
	pattern.levels[0][0] = 1.0f;
	modulator->ApplyState(pattern);

	std::vector<float> buffer(64, 0.0f);
	RunBlock(*modulator, buffer, MakeContext(0, true));
	EXPECT_NEAR(driven->value, 1.0f, 0.001f);

	SetParameter(*modulator, "Depth", 50.0f);
	RunBlock(*modulator, buffer, MakeContext(0, true));
	EXPECT_NEAR(driven->value, 0.5f, 0.001f);

	SetParameter(*modulator, "Offset", 25.0f);
	RunBlock(*modulator, buffer, MakeContext(0, true));
	EXPECT_NEAR(driven->value, 0.75f, 0.001f);
}

// ---- targets ----

// the case the device exists for: a modulator on a track driving a macro of a rack
// sitting beside it, without either knowing about the other
TEST(Modulator, DrivesARackMacroOnTheSameTrack) {
	LiveProject live;
	live.project.CreateTrack();
	auto track = live.project.GetTracks().back();

	auto modulator = MakeModulator();
	auto rack = std::dynamic_pointer_cast<RackProcessor>(ProcessorFactory::Instance().Create("Rack"));
	ASSERT_NE(rack, nullptr);
	rack->AddChain("Chain");
	track->AddProcessor(modulator);
	track->AddProcessor(rack);

	Parameter* macro = rack->GetMacroParameter(0);
	ASSERT_NE(macro, nullptr);
	ASSERT_TRUE(modulator->AddTarget(&live.project, macro));
	EXPECT_TRUE(modulator->IsParameterTargeted(macro));

	SetParameter(*modulator, "Mode", (float)ModulatorProcessor::ModeStepper);
	SetParameter(*modulator, "Steps", 1.0f);
	SetParameter(*modulator, "XFade", 0.0f);
	SetParameter(*modulator, "Sync", 1.0f);
	SetParameter(*modulator, "Restart", 0.0f);

	ModulatorProcessor::State pattern = modulator->CaptureState();
	pattern.levels[0][0] = 1.0f;
	modulator->ApplyState(pattern);

	std::vector<float> buffer(64, 0.0f);
	RunBlock(*modulator, buffer, MakeContext(0, true));
	// a fresh mapping opens at the parameter's own full range, which for a macro is 127
	EXPECT_NEAR(macro->value, RackProcessor::kMacroMax, 0.01f);
}

// a device inside a rack is reachable by exactly the same walk as one on the track,
// which is what lets a modulator drive a plugin knob three levels down
TEST(Modulator, ReachesADeviceNestedInsideARack) {
	LiveProject live;
	live.project.CreateTrack();
	auto track = live.project.GetTracks().back();

	auto modulator = MakeModulator();
	auto rack = std::dynamic_pointer_cast<RackProcessor>(ProcessorFactory::Instance().Create("Rack"));
	auto nested = ProcessorFactory::Instance().Create("BitCrusher");
	rack->AddChain("Chain")->AddProcessor(nested);
	track->AddProcessor(modulator);
	track->AddProcessor(rack);

	Parameter* driven = nested->GetParameters().front().get();
	EXPECT_TRUE(modulator->AddTarget(&live.project, driven));
	EXPECT_TRUE(modulator->IsParameterTargeted(driven));
}

TEST(Modulator, RefusesToDriveItsOwnControls) {
	LiveProject live;
	live.project.CreateTrack();
	auto track = live.project.GetTracks().back();

	auto modulator = MakeModulator();
	track->AddProcessor(modulator);

	Parameter* own = modulator->GetParameters().front().get();
	EXPECT_FALSE(modulator->AddTarget(&live.project, own));
	EXPECT_TRUE(modulator->GetTargets().empty());
}

// a device dragged out of the project is still alive while the undo history holds it.
// it must stop being driven the moment it leaves, or the modulator keeps writing into
// something nobody can see
TEST(Modulator, StopsDrivingADeviceThatHasLeftTheProject) {
	LiveProject live;
	live.project.CreateTrack();
	auto track = live.project.GetTracks().back();

	auto modulator = MakeModulator();
	auto sink = ProcessorFactory::Instance().Create("BitCrusher");
	track->AddProcessor(modulator);
	track->AddProcessor(sink);

	Parameter* driven = sink->GetParameters().front().get();
	ASSERT_TRUE(modulator->AddTarget(&live.project, driven));
	EXPECT_TRUE(modulator->IsParameterTargeted(driven));

	track->RemoveProcessor(1); // `sink` stays alive here, exactly as an undo would keep it
	EXPECT_FALSE(modulator->IsParameterTargeted(driven));

	SetParameter(*modulator, "Mode", (float)ModulatorProcessor::ModeStepper);
	const float untouched = driven->value;
	std::vector<float> buffer(64, 0.0f);
	RunBlock(*modulator, buffer, MakeContext(0, true));
	EXPECT_FLOAT_EQ(driven->value, untouched);
}

TEST(Modulator, ReMappingTheSameParameterUpdatesTheEntryInsteadOfStackingOne) {
	LiveProject live;
	live.project.CreateTrack();
	auto track = live.project.GetTracks().back();

	auto modulator = MakeModulator();
	auto sink = ProcessorFactory::Instance().Create("BitCrusher");
	track->AddProcessor(modulator);
	track->AddProcessor(sink);

	Parameter* driven = sink->GetParameters().front().get();
	ASSERT_TRUE(modulator->AddTarget(&live.project, driven));
	modulator->GetTargetsMutable().front().minValue = 0.3f;

	ASSERT_TRUE(modulator->AddTarget(&live.project, driven));
	ASSERT_EQ(modulator->GetTargets().size(), 1u);
	EXPECT_FLOAT_EQ(modulator->GetTargets().front().minValue, driven->minValue);
}

// ---- serialization ----

TEST(Modulator, RoundTripsItsPatternAndTargetsThroughAProjectFile) {
	LiveProject live;
	live.project.CreateTrack();
	auto track = live.project.GetTracks().back();

	auto modulator = MakeModulator();
	auto rack = std::dynamic_pointer_cast<RackProcessor>(ProcessorFactory::Instance().Create("Rack"));
	auto nested = ProcessorFactory::Instance().Create("BitCrusher");
	rack->AddChain("Chain")->AddProcessor(nested);
	track->AddProcessor(modulator);
	track->AddProcessor(rack);

	Parameter* driven = nested->GetParameters().front().get();
	ASSERT_TRUE(modulator->AddTarget(&live.project, driven));
	modulator->GetTargetsMutable().front().minValue = 0.125f;
	modulator->GetTargetsMutable().front().maxValue = 0.875f;

	ModulatorProcessor::State pattern = modulator->CaptureState();
	pattern.levels[1][3] = 0.42f;
	pattern.heights[0][4] = 0.31f;
	pattern.curves[0][2] = ModulatorProcessor::CurveDoublePulse;
	modulator->ApplyState(pattern);
	SetParameter(*modulator, "Mode", (float)ModulatorProcessor::ModePerformer);

	std::stringstream stream;
	ProcessorIO::SaveProcessor(stream, *modulator);

	// the reader has already eaten the PROCESSOR line by the time a device is built
	std::string header;
	std::getline(stream, header);
	ASSERT_EQ(header, "PROCESSOR Modulator");
	auto loaded = std::dynamic_pointer_cast<ModulatorProcessor>(ProcessorIO::LoadProcessor(stream, "Modulator"));
	ASSERT_NE(loaded, nullptr);

	EXPECT_EQ(loaded->CurrentMode(), (int)ModulatorProcessor::ModePerformer);
	const ModulatorProcessor::State restored = loaded->CaptureState();
	EXPECT_NEAR(restored.levels[1][3], 0.42f, 0.0001f);
	EXPECT_NEAR(restored.heights[0][4], 0.31f, 0.0001f);
	EXPECT_EQ(restored.curves[0][2], (int)ModulatorProcessor::CurveDoublePulse);

	ASSERT_EQ(loaded->GetTargets().size(), 1u);
	const ModulatorProcessor::Target& target = loaded->GetTargets().front();
	EXPECT_EQ(target.trackId, track->GetId());
	EXPECT_EQ(target.paramName, driven->name);
	EXPECT_NEAR(target.minValue, 0.125f, 0.0001f);
	EXPECT_NEAR(target.maxValue, 0.875f, 0.0001f);

	// the saved address names the device by its path into the track, so a load that can
	// see the project binds straight back onto the same nested device
	track->AddProcessor(loaded);
	EXPECT_TRUE(loaded->IsParameterTargeted(driven));
}

// copy / paste / duplicate only carries parameter values through the base class, so
// everything else a modulator holds has to come across in CopyStateFrom
TEST(Modulator, ADuplicateKeepsThePatternAndDrivesTheSameThings) {
	LiveProject live;
	live.project.CreateTrack();
	auto track = live.project.GetTracks().back();

	auto modulator = MakeModulator();
	auto sink = ProcessorFactory::Instance().Create("BitCrusher");
	track->AddProcessor(modulator);
	track->AddProcessor(sink);

	Parameter* driven = sink->GetParameters().front().get();
	ASSERT_TRUE(modulator->AddTarget(&live.project, driven));

	ModulatorProcessor::State pattern = modulator->CaptureState();
	pattern.levels[0][5] = 0.6f;
	modulator->ApplyState(pattern);

	auto clone = std::dynamic_pointer_cast<ModulatorProcessor>(ProcessorIO::CloneProcessor(modulator));
	ASSERT_NE(clone, nullptr);
	EXPECT_NEAR(clone->CaptureState().levels[0][5], 0.6f, 0.0001f);
	ASSERT_EQ(clone->GetTargets().size(), 1u);

	track->AddProcessor(clone);
	EXPECT_TRUE(clone->IsParameterTargeted(driven));
}

// ---- undo ----

TEST(Modulator, ATargetEditIsOneHistoryEntry) {
	LiveProject live;
	live.project.CreateTrack();
	auto track = live.project.GetTracks().back();

	auto modulator = MakeModulator();
	auto sink = ProcessorFactory::Instance().Create("BitCrusher");
	track->AddProcessor(modulator);
	track->AddProcessor(sink);

	Parameter* driven = sink->GetParameters().front().get();
	UndoManager undoManager;
	Project* project = &live.project;

	DeviceRackOps::EditModulator(project, undoManager, modulator, "Modulation target",
								 [&]() { modulator->AddTarget(project, driven); });
	ASSERT_EQ(modulator->GetTargets().size(), 1u);

	undoManager.Undo();
	EXPECT_TRUE(modulator->GetTargets().empty());
	EXPECT_FALSE(modulator->IsParameterTargeted(driven));

	undoManager.Redo();
	ASSERT_EQ(modulator->GetTargets().size(), 1u);
	EXPECT_TRUE(modulator->IsParameterTargeted(driven));
}
