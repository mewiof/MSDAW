#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include "ProcessorFactory.h"
#include "Processors/PhaserProcessor.h"

// ================================================================
// PHASER
// ================================================================

namespace {

	constexpr double kRate = 48000.0;
	constexpr double kTwoPi = 6.283185307179586;

	// the parameters are addressed by name everywhere else in the project (automation
	// rebinds by name, the project file stores the name), so the tests do too
	Parameter* Find(AudioProcessor& processor, const std::string& name) {
		for (const auto& parameter : processor.GetParameters()) {
			if (parameter->name == name)
				return parameter.get();
		}
		return nullptr;
	}

	void Set(AudioProcessor& processor, const std::string& name, float value) {
		Parameter* parameter = Find(processor, name);
		ASSERT_NE(parameter, nullptr) << name;
		parameter->value = value;
	}

	// a phaser with nothing moving: the notches sit where the frequency knob puts them,
	// which is the only state a response can be checked against
	std::unique_ptr<PhaserProcessor> MakeStatic() {
		auto phaser = std::make_unique<PhaserProcessor>();
		phaser->PrepareToPlay(kRate);
		Set(*phaser, "LFO Amount", 0.0f);
		Set(*phaser, "Env Amount", 0.0f);
		Set(*phaser, "Feedback", 0.0f);
		Set(*phaser, "Dry/Wet", 100.0f);
		return phaser;
	}

	float ResponseAt(const PhaserProcessor& phaser, double frequency) {
		float db = 0.0f;
		phaser.GetResponseDb(&frequency, &db, 1);
		return db;
	}

	// the frequencies where the response dips below `thresholdDb` and comes back up, swept
	// logarithmically so a notch an octave up is resolved as finely as one an octave down
	std::vector<double> FindNotches(const PhaserProcessor& phaser, float thresholdDb = -20.0f) {
		const int points = 4000;
		const double low = 30.0, high = 20000.0;

		std::vector<double> frequencies((size_t)points);
		std::vector<float> response((size_t)points);
		for (int i = 0; i < points; ++i)
			frequencies[(size_t)i] = low * std::pow(high / low, (double)i / (double)(points - 1));
		phaser.GetResponseDb(frequencies.data(), response.data(), points);

		std::vector<double> found;
		for (int i = 1; i < points - 1; ++i) {
			if (response[(size_t)i] < thresholdDb &&
				response[(size_t)i] <= response[(size_t)(i - 1)] &&
				response[(size_t)i] < response[(size_t)(i + 1)])
				found.push_back(frequencies[(size_t)i]);
		}
		return found;
	}

	std::vector<float> MakeSine(double frequency, int frames, int channels, float amplitude = 0.5f) {
		std::vector<float> buffer((size_t)frames * (size_t)channels, 0.0f);
		for (int i = 0; i < frames; ++i) {
			const float sample = amplitude * (float)std::sin(kTwoPi * frequency * (double)i / kRate);
			for (int channel = 0; channel < channels; ++channel)
				buffer[(size_t)i * channels + channel] = sample;
		}
		return buffer;
	}

	// the tail only, so the allpass chain's own settling is not measured as level
	float TailRms(const std::vector<float>& buffer, int channels) {
		const size_t frames = buffer.size() / (size_t)channels;
		const size_t first = frames / 2;
		double sum = 0.0;
		for (size_t frame = first; frame < frames; ++frame)
			sum += (double)buffer[frame * channels] * buffer[frame * channels];
		return (float)std::sqrt(sum / (double)(frames - first));
	}

	ProcessContext StoppedContext() {
		ProcessContext context;
		context.sampleRate = kRate;
		context.isPlaying = false;
		return context;
	}

} //namespace

// ---- registration ----

TEST(Phaser, RegistersWithTheFactory) {
	EXPECT_TRUE(ProcessorFactory::Instance().IsRegistered("Phaser"));
	EXPECT_NE(ProcessorFactory::Instance().Create("Phaser"), nullptr);
	EXPECT_FALSE(ProcessorFactory::Instance().IsInstrument("Phaser"));
}

// ---- section placement ----

TEST(Phaser, EarthStacksEverySectionOnTheFrequencyKnob) {
	auto phaser = MakeStatic();
	Set(*phaser, "Spacing", (float)PhaserProcessor::SpacingEarth);
	Set(*phaser, "Poles", 8.0f);

	float sections[PhaserProcessor::kMaxPoles];
	const int count = phaser->GetSectionFrequencies(1000.0f, sections);
	ASSERT_EQ(count, 8);
	for (int section = 0; section < count; ++section)
		EXPECT_NEAR(sections[section], 1000.0f, 0.5f) << section;

	// and Color moves none of them - it is their Q, not their place
	Set(*phaser, "Color", 0.0f);
	const float soft = phaser->SectionQ();
	Set(*phaser, "Color", 100.0f);
	const float sharp = phaser->SectionQ();
	phaser->GetSectionFrequencies(1000.0f, sections);
	EXPECT_NEAR(sections[0], 1000.0f, 0.5f);
	EXPECT_GT(sharp, soft * 4.0f);

	Set(*phaser, "Color", 50.0f);
	// the reference product's render measures 1.15 at the middle of the dial
	EXPECT_NEAR(phaser->SectionQ(), 1.15f, 0.03f);
}

TEST(Phaser, ColorNarrowsTheNotchesWithoutMovingThem) {
	auto phaser = MakeStatic();
	Set(*phaser, "Poles", 1.0f);
	Set(*phaser, "Frequency", 1000.0f);

	auto notchWidthOctaves = [&]() {
		const std::vector<double> found = FindNotches(*phaser, -20.0f);
		EXPECT_EQ(found.size(), 1u);
		const double notch = found.front();
		// how far either side the response is still 6 dB down
		double low = notch, high = notch;
		while (low > 40.0 && ResponseAt(*phaser, low) < -6.0f)
			low /= 1.01;
		while (high < 18000.0 && ResponseAt(*phaser, high) < -6.0f)
			high *= 1.01;
		return std::log2(high / low);
	};

	Set(*phaser, "Color", 0.0f);
	const double wide = notchWidthOctaves();
	Set(*phaser, "Color", 100.0f);
	const double narrow = notchWidthOctaves();
	EXPECT_LT(narrow, wide * 0.5);
}

TEST(Phaser, SpaceStandsTheSectionsOnHarmonics) {
	auto phaser = MakeStatic();
	Set(*phaser, "Spacing", (float)PhaserProcessor::SpacingSpace);
	Set(*phaser, "Poles", 4.0f);
	Set(*phaser, "Color", 0.0f); // Color must not reach Space at all

	float sections[PhaserProcessor::kMaxPoles];
	const int count = phaser->GetSectionFrequencies(500.0f, sections);
	ASSERT_EQ(count, 4);
	for (int section = 0; section < count; ++section)
		EXPECT_NEAR(sections[section], 500.0f * (float)(section + 1), 1.0f);

	const float soft = phaser->SectionQ();
	Set(*phaser, "Color", 100.0f);
	EXPECT_FLOAT_EQ(phaser->SectionQ(), soft);
}

// ---- response ----

TEST(Phaser, FullyDryIsFlat) {
	auto phaser = MakeStatic();
	Set(*phaser, "Dry/Wet", 0.0f);
	for (double frequency : {50.0, 500.0, 1050.0, 5000.0, 15000.0})
		EXPECT_NEAR(ResponseAt(*phaser, frequency), 0.0f, 1.0e-4f);
}

TEST(Phaser, AnAllpassChainOnItsOwnPassesTheLowEndThrough) {
	// every section is unity at DC, so summing the chain with the dry signal there is a
	// doubling that the halved sum takes straight back to unity. a phaser that cut the
	// bottom out would be a filter, not a phaser
	auto phaser = MakeStatic();
	EXPECT_NEAR(ResponseAt(*phaser, 1.0), 0.0f, 0.1f);
}

TEST(Phaser, ThereIsOneNotchPerPole) {
	auto phaser = MakeStatic();
	Set(*phaser, "Spacing", (float)PhaserProcessor::SpacingEarth);
	Set(*phaser, "Color", 50.0f);
	Set(*phaser, "Frequency", 700.0f);

	for (int poles = 1; poles <= 6; ++poles) {
		Set(*phaser, "Poles", (float)poles);
		EXPECT_EQ((int)FindNotches(*phaser).size(), poles) << "poles " << poles;
	}
}

// the two numbers the reference product's render pins down: with the feedback control
// wide open its peaks sit at +4.5 dB and its notches at -14.8 dB, whatever the frequency
// or the pole count. those come out of the loop gain alone, so they are the tightest
// check there is that this device's feedback is worth what that one's is
TEST(Phaser, WideOpenFeedbackLandsOnTheReferenceLevels) {
	auto phaser = MakeStatic();
	Set(*phaser, "Poles", 4.0f);
	Set(*phaser, "Feedback", Find(*phaser, "Feedback")->maxValue);

	const int points = 8000;
	std::vector<double> frequencies((size_t)points);
	std::vector<float> response((size_t)points);
	for (int i = 0; i < points; ++i)
		frequencies[(size_t)i] = 60.0 * std::pow(18000.0 / 60.0, (double)i / (double)(points - 1));
	phaser->GetResponseDb(frequencies.data(), response.data(), points);

	float peak = -120.0f, dip = 120.0f;
	for (float db : response) {
		peak = std::max(peak, db);
		dip = std::min(dip, db);
	}
	EXPECT_NEAR(peak, 4.5f, 0.6f);
	EXPECT_NEAR(dip, -14.8f, 1.0f);
}

TEST(Phaser, TheNotchesFollowTheFrequencyKnob) {
	auto phaser = MakeStatic();
	Set(*phaser, "Poles", 4.0f);

	Set(*phaser, "Frequency", 400.0f);
	const std::vector<double> low = FindNotches(*phaser);
	Set(*phaser, "Frequency", 3200.0f);
	const std::vector<double> high = FindNotches(*phaser);

	ASSERT_EQ(low.size(), high.size());
	ASSERT_FALSE(low.empty());
	for (size_t i = 0; i < low.size(); ++i) {
		// three octaves up the knob is three octaves up the spectrum
		EXPECT_NEAR(high[i] / low[i], 8.0, 0.6) << "notch " << i;
	}
}

TEST(Phaser, FeedbackTurnsTheGapsBetweenTheNotchesIntoPeaks) {
	auto phaser = MakeStatic();
	Set(*phaser, "Poles", 4.0f);

	const int points = 2000;
	std::vector<double> frequencies((size_t)points);
	std::vector<float> response((size_t)points);
	for (int i = 0; i < points; ++i)
		frequencies[(size_t)i] = 30.0 * std::pow(20000.0 / 30.0, (double)i / (double)(points - 1));

	auto peakDb = [&]() {
		phaser->GetResponseDb(frequencies.data(), response.data(), points);
		float peak = -120.0f;
		for (float db : response)
			peak = std::max(peak, db);
		return peak;
	};

	Set(*phaser, "Feedback", 0.0f);
	const float flat = peakDb();
	Set(*phaser, "Feedback", Find(*phaser, "Feedback")->maxValue);
	const float resonant = peakDb();

	EXPECT_NEAR(flat, 0.0f, 0.1f);
	EXPECT_GT(resonant, flat + 3.0f);
}

// ---- audio path ----

TEST(Phaser, ANotchInTheResponseIsANotchInTheAudio) {
	auto phaser = MakeStatic();
	Set(*phaser, "Poles", 4.0f);

	const std::vector<double> notches = FindNotches(*phaser, -25.0f);
	ASSERT_FALSE(notches.empty());
	const double notch = notches.front();

	const int frames = 24000;
	std::vector<float> processed = MakeSine(notch, frames, 2);
	const std::vector<float> reference = processed;

	std::vector<MIDIMessage> messages;
	const ProcessContext context = StoppedContext();
	phaser->Process(processed.data(), frames, 2, messages, context);

	const float before = TailRms(reference, 2);
	const float after = TailRms(processed, 2);
	ASSERT_GT(before, 0.0f);
	// the response says it cancels, so the audio has to as well
	EXPECT_LT(20.0f * std::log10(after / before), -20.0f);
}

TEST(Phaser, AFrequencyAwayFromEveryNotchComesThroughIntact) {
	auto phaser = MakeStatic();
	Set(*phaser, "Poles", 1.0f);
	Set(*phaser, "Frequency", 4000.0f);

	const int frames = 24000;
	std::vector<float> processed = MakeSine(80.0, frames, 2);
	const std::vector<float> reference = processed;

	std::vector<MIDIMessage> messages;
	const ProcessContext context = StoppedContext();
	phaser->Process(processed.data(), frames, 2, messages, context);

	EXPECT_NEAR(TailRms(processed, 2), TailRms(reference, 2), 0.005f);
}

TEST(Phaser, FullyDryLeavesEverySampleAlone) {
	auto phaser = std::make_unique<PhaserProcessor>();
	phaser->PrepareToPlay(kRate);
	Set(*phaser, "Dry/Wet", 0.0f);

	const int frames = 512;
	std::vector<float> processed = MakeSine(220.0, frames, 2);
	const std::vector<float> reference = processed;

	std::vector<MIDIMessage> messages;
	const ProcessContext context = StoppedContext();
	phaser->Process(processed.data(), frames, 2, messages, context);

	for (size_t i = 0; i < processed.size(); ++i)
		EXPECT_FLOAT_EQ(processed[i], reference[i]) << "sample " << i;
}

TEST(Phaser, TheFeedbackLoopStaysBoundedWideOpen) {
	// an allpass chain has unit magnitude everywhere, so a loop gain of 0.99 would ring to
	// +40 dB if the saturator in the feedback path were not holding it
	auto phaser = std::make_unique<PhaserProcessor>();
	phaser->PrepareToPlay(kRate);
	Set(*phaser, "Feedback", 0.99f);
	Set(*phaser, "Dry/Wet", 100.0f);
	Set(*phaser, "Poles", 12.0f);
	Set(*phaser, "LFO Amount", 100.0f);
	Set(*phaser, "LFO Rate", 8.0f);

	const int frames = 480;
	std::vector<MIDIMessage> messages;
	ProcessContext context = StoppedContext();

	for (int block = 0; block < 200; ++block) {
		std::vector<float> buffer = MakeSine(120.0, frames, 2, 0.9f);
		phaser->Process(buffer.data(), frames, 2, messages, context);
		context.currentSample += frames;
		for (float sample : buffer) {
			ASSERT_TRUE(std::isfinite(sample)) << "block " << block;
			ASSERT_LT(std::fabs(sample), 4.0f) << "block " << block;
		}
	}
}

TEST(Phaser, ResetSilencesTheChain) {
	auto phaser = std::make_unique<PhaserProcessor>();
	phaser->PrepareToPlay(kRate);
	Set(*phaser, "Feedback", 0.9f);
	Set(*phaser, "Dry/Wet", 100.0f);

	const int frames = 4800;
	std::vector<MIDIMessage> messages;
	const ProcessContext context = StoppedContext();

	std::vector<float> loud = MakeSine(300.0, frames, 2, 0.9f);
	phaser->Process(loud.data(), frames, 2, messages, context);

	phaser->Reset();

	// the ring the loop was holding has to be gone, not just fading
	std::vector<float> silence((size_t)frames * 2, 0.0f);
	phaser->Process(silence.data(), frames, 2, messages, context);
	for (float sample : silence)
		EXPECT_LT(std::fabs(sample), 1.0e-6f);
}

// ---- modulation ----

TEST(Phaser, TheLFOMovesTheStagesAndAmountZeroHoldsThemStill) {
	auto phaser = std::make_unique<PhaserProcessor>();
	phaser->PrepareToPlay(kRate);
	Set(*phaser, "Env Amount", 0.0f);
	Set(*phaser, "LFO Rate", 4.0f);

	const int frames = 480;
	std::vector<MIDIMessage> messages;
	ProcessContext context = StoppedContext();

	auto sweep = [&](float amount) {
		Set(*phaser, "LFO Amount", amount);
		phaser->Reset();
		float low = 1.0e9f, high = 0.0f;
		for (int block = 0; block < 40; ++block) {
			std::vector<float> buffer = MakeSine(200.0, frames, 2, 0.2f);
			phaser->Process(buffer.data(), frames, 2, messages, context);
			context.currentSample += frames;
			low = std::min(low, phaser->GetVisualFrequency(0));
			high = std::max(high, phaser->GetVisualFrequency(0));
		}
		return high / low;
	};

	EXPECT_NEAR(sweep(0.0f), 1.0f, 1.0e-3f);
	// the reference product's render puts a full-amount sweep a little under an octave
	// either way, so the sections travel about 2^1.78 end to end
	EXPECT_GT(sweep(100.0f), 3.0f);
	EXPECT_LT(sweep(100.0f), 4.0f);
}

TEST(Phaser, ThePhaseControlSplitsTheTwoChannels) {
	auto phaser = std::make_unique<PhaserProcessor>();
	phaser->PrepareToPlay(kRate);
	Set(*phaser, "Env Amount", 0.0f);
	Set(*phaser, "LFO Amount", 100.0f);
	Set(*phaser, "LFO Rate", 1.0f);
	Set(*phaser, "LFO Stereo", (float)PhaserProcessor::StereoPhase);

	const int frames = 480;
	std::vector<MIDIMessage> messages;
	ProcessContext context = StoppedContext();

	// a quarter of a cycle in is where a 180 degree offset separates the two lanes most:
	// one is at the top of its sweep as the other bottoms out. at the start of a cycle
	// they coincide whatever the offset, which would prove nothing
	auto runQuarterCycle = [&]() {
		phaser->Reset();
		context.currentSample = 0;
		for (int block = 0; block < 25; ++block) {
			std::vector<float> buffer = MakeSine(200.0, frames, 2, 0.2f);
			phaser->Process(buffer.data(), frames, 2, messages, context);
			context.currentSample += frames;
		}
	};

	Set(*phaser, "LFO Phase", 0.0f);
	runQuarterCycle();
	EXPECT_FLOAT_EQ(phaser->GetVisualFrequency(0), phaser->GetVisualFrequency(1));

	Set(*phaser, "LFO Phase", 180.0f);
	runQuarterCycle();
	const float left = phaser->GetVisualFrequency(0);
	const float right = phaser->GetVisualFrequency(1);
	// nearly the whole sweep apart, one lane up and the other down
	EXPECT_GT(std::log2(std::max(left, right) / std::min(left, right)), 1.5f);
}

TEST(Phaser, TheEnvelopeFollowerOpensOnLevelAndFallsBack) {
	auto phaser = std::make_unique<PhaserProcessor>();
	phaser->PrepareToPlay(kRate);
	Set(*phaser, "LFO Amount", 0.0f);
	Set(*phaser, "Env Amount", 100.0f);
	Set(*phaser, "Env Attack", 1.0f);
	Set(*phaser, "Env Release", 10.0f);
	Set(*phaser, "Frequency", 200.0f);

	const int frames = 4800;
	std::vector<MIDIMessage> messages;
	const ProcessContext context = StoppedContext();

	std::vector<float> loud = MakeSine(300.0, frames, 2, 0.95f);
	phaser->Process(loud.data(), frames, 2, messages, context);
	const float opened = phaser->GetVisualFrequency(0);
	EXPECT_GT(opened, 600.0f);

	std::vector<float> silence((size_t)frames * 2, 0.0f);
	phaser->Process(silence.data(), frames, 2, messages, context);
	EXPECT_NEAR(phaser->GetVisualFrequency(0), 200.0f, 5.0f);
}

TEST(Phaser, ASyncedLFOIsChasedOffTheTransport) {
	// two runs from the same transport position have to agree, or an offline render would
	// not match what was heard
	auto first = std::make_unique<PhaserProcessor>();
	auto second = std::make_unique<PhaserProcessor>();
	for (PhaserProcessor* phaser : {first.get(), second.get()}) {
		phaser->PrepareToPlay(kRate);
		Set(*phaser, "Env Amount", 0.0f);
		Set(*phaser, "LFO Amount", 100.0f);
		Set(*phaser, "LFO Sync", 1.0f);
	}

	const int frames = 480;
	std::vector<MIDIMessage> messages;
	ProcessContext context = StoppedContext();
	context.isPlaying = true;
	context.bpm = 120.0;
	context.currentSample = 96000; // two seconds in

	std::vector<float> a = MakeSine(200.0, frames, 2, 0.2f);
	first->Process(a.data(), frames, 2, messages, context);

	// the second one arrives at the same place having never seen the blocks before it
	std::vector<float> b = MakeSine(200.0, frames, 2, 0.2f);
	second->Process(b.data(), frames, 2, messages, context);

	EXPECT_FLOAT_EQ(first->GetVisualFrequency(0), second->GetVisualFrequency(0));
}

// ---- serialization ----

TEST(Phaser, EveryControlSurvivesASaveAndLoad) {
	PhaserProcessor saved;
	saved.PrepareToPlay(kRate);
	Set(saved, "Poles", 9.0f);
	Set(saved, "Spacing", (float)PhaserProcessor::SpacingSpace);
	Set(saved, "Color", 12.0f);
	Set(saved, "Dry/Wet", 77.0f);
	Set(saved, "Frequency", 620.0f);
	Set(saved, "Feedback", 0.83f);
	Set(saved, "Env Amount", -55.0f);
	Set(saved, "LFO Shape", (float)PhaserProcessor::ShapeRandom);
	Set(saved, "LFO Sync", 1.0f);
	Set(saved, "LFO Division", 8.0f);
	Set(saved, "LFO Stereo", (float)PhaserProcessor::StereoSpin);
	Set(saved, "LFO Spin", -40.0f);

	std::stringstream stream;
	saved.Save(stream);

	PhaserProcessor loaded;
	loaded.PrepareToPlay(kRate);
	std::string header;
	std::getline(stream, header);
	ASSERT_EQ(header, "PARAMS_BEGIN");
	loaded.Load(stream);

	for (const auto& parameter : saved.GetParameters()) {
		Parameter* other = Find(loaded, parameter->name);
		ASSERT_NE(other, nullptr) << parameter->name;
		EXPECT_FLOAT_EQ(other->value, parameter->value) << parameter->name;
	}
}
