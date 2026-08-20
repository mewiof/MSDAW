#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "ProcessorFactory.h"
#include "Processors/EQEightProcessor.h"

// ================================================================
// EQ EIGHT
// ================================================================

namespace {

	constexpr double kRate = 48000.0;

	// the parameters are addressed by name everywhere else in the project (automation
	// rebinds by name, the project file stores the name), so the tests do too
	Parameter* Find(AudioProcessor& processor, const std::string& name) {
		for (const auto& parameter : processor.GetParameters()) {
			if (parameter->name == name)
				return parameter.get();
		}
		return nullptr;
	}

	void SetBand(AudioProcessor& processor, int band, EQFilterType type,
				 float frequency, float gainDb, float q, bool active = true) {
		const std::string suffix = " " + std::to_string(band + 1) + "A";
		Find(processor, "Type" + suffix)->value = (float)(int)type;
		Find(processor, "Freq" + suffix)->value = frequency;
		Find(processor, "Gain" + suffix)->value = gainDb;
		Find(processor, "Q" + suffix)->value = q;
		Find(processor, "On" + suffix)->value = active ? 1.0f : 0.0f;
	}

	void SilenceEveryBand(AudioProcessor& processor) {
		for (int band = 0; band < EQEightProcessor::kNumBands; ++band) {
			for (int set = 0; set < EQEightProcessor::kNumSets; ++set) {
				const std::string suffix = " " + std::to_string(band + 1) + (set == 0 ? "A" : "B");
				Find(processor, "On" + suffix)->value = 0.0f;
			}
		}
	}

	float ResponseAt(const EQEightProcessor& processor, double frequency, int setIndex = 0) {
		float db = 0.0f;
		processor.GetResponseDb(setIndex, -1, &frequency, &db, 1);
		return db;
	}

	std::vector<float> MakeSine(double frequency, int frames, int channels, float amplitude = 0.5f) {
		std::vector<float> buffer((size_t)frames * (size_t)channels, 0.0f);
		for (int i = 0; i < frames; ++i) {
			const float sample = amplitude * (float)std::sin(2.0 * 3.14159265358979323846 * frequency * i / kRate);
			for (int c = 0; c < channels; ++c)
				buffer[(size_t)i * channels + c] = sample;
		}
		return buffer;
	}

	// peak of the tail, so the filter's own settling transient is never measured
	float TailPeak(const std::vector<float>& buffer, int channels, int channel, int tailFrames) {
		const int frames = (int)(buffer.size() / (size_t)channels);
		float peak = 0.0f;
		for (int i = std::max(frames - tailFrames, 0); i < frames; ++i)
			peak = std::max(peak, std::abs(buffer[(size_t)i * channels + channel]));
		return peak;
	}

	void RunBlocks(AudioProcessor& processor, std::vector<float>& buffer, int channels) {
		std::vector<MIDIMessage> midi;
		ProcessContext context;
		context.sampleRate = kRate;

		// in blocks, the way the audio thread would: the coefficient smoothing and the
		// denormal sweep both run per block, so one giant call would not drive them
		const int blockSize = 256;
		const int frames = (int)(buffer.size() / (size_t)channels);
		for (int start = 0; start < frames; start += blockSize) {
			const int count = std::min(blockSize, frames - start);
			processor.Process(buffer.data() + (size_t)start * channels, count, channels, midi, context);
		}
	}

} // namespace

TEST(EQEight, RegistersWithTheFactory) {
	EXPECT_TRUE(ProcessorFactory::Instance().IsRegistered("EQEight"));
	EXPECT_NE(ProcessorFactory::Instance().Create("EQEight"), nullptr);
	EXPECT_FALSE(ProcessorFactory::Instance().IsInstrument("EQEight"));
}

// the default state has to be audibly nothing: an EQ that colors a track the moment
// it is dropped on it is a bug, not a preset
TEST(EQEight, DefaultCurveIsFlat) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);

	for (double frequency : {20.0, 100.0, 1000.0, 5000.0, 15000.0})
		EXPECT_NEAR(ResponseAt(eq, frequency), 0.0f, 0.01f) << "at " << frequency << " Hz";
}

TEST(EQEight, DefaultStatePassesAudioThrough) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);

	const std::vector<float> input = MakeSine(1000.0, 4096, 2);
	std::vector<float> output = input;
	RunBlocks(eq, output, 2);

	for (size_t i = 0; i < input.size(); ++i)
		ASSERT_NEAR(output[i], input[i], 1.0e-5f) << "at sample " << i;
}

TEST(EQEight, BellReachesItsGainAtTheCenterFrequency) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);
	SetBand(eq, 3, EQFilterType::Bell, 1000.0f, 6.0f, 1.0f);

	EXPECT_NEAR(ResponseAt(eq, 1000.0), 6.0f, 0.05f);
	// and leaves the rest of the spectrum where it found it
	EXPECT_NEAR(ResponseAt(eq, 40.0), 0.0f, 0.2f);
	EXPECT_NEAR(ResponseAt(eq, 16000.0), 0.0f, 0.2f);
}

TEST(EQEight, ShelvesReachTheirGainAwayFromTheCorner) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);

	SetBand(eq, 0, EQFilterType::LowShelf, 200.0f, 8.0f, 0.71f);
	EXPECT_NEAR(ResponseAt(eq, 20.0), 8.0f, 0.3f);
	EXPECT_NEAR(ResponseAt(eq, 10000.0), 0.0f, 0.1f);

	SetBand(eq, 0, EQFilterType::HighShelf, 4000.0f, -8.0f, 0.71f);
	EXPECT_NEAR(ResponseAt(eq, 20000.0), -8.0f, 0.3f);
	EXPECT_NEAR(ResponseAt(eq, 50.0), 0.0f, 0.1f);
}

// a cut's corner is its -3 dB point, and that is also where the graph parks its
// handle, so this is the number the UI leans on as well
TEST(EQEight, CutFiltersSitThreeDecibelsDownAtTheirCorner) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);

	SetBand(eq, 0, EQFilterType::LowCut12, 1000.0f, 0.0f, 0.7071f);
	EXPECT_NEAR(ResponseAt(eq, 1000.0), -3.0f, 0.15f);

	SetBand(eq, 0, EQFilterType::HighCut12, 1000.0f, 0.0f, 0.7071f);
	EXPECT_NEAR(ResponseAt(eq, 1000.0), -3.0f, 0.15f);
}

TEST(EQEight, TheSteepCutFallsFourTimesFasterThanTheShallowOne) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);

	SetBand(eq, 0, EQFilterType::LowCut12, 1000.0f, 0.0f, 0.7071f);
	const float shallow = ResponseAt(eq, 250.0); // two octaves down: about -24 dB

	SetBand(eq, 0, EQFilterType::LowCut48, 1000.0f, 0.0f, 0.7071f);
	const float steep = ResponseAt(eq, 250.0); // the same two octaves: about -96 dB

	EXPECT_NEAR(shallow, -24.0f, 1.0f);
	EXPECT_NEAR(steep, -96.0f, 2.0f);
}

TEST(EQEight, NotchDropsOutAtItsFrequencyAndNowhereElse) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);
	SetBand(eq, 0, EQFilterType::Notch, 1000.0f, 0.0f, 4.0f);

	EXPECT_LT(ResponseAt(eq, 1000.0), -40.0f);
	EXPECT_NEAR(ResponseAt(eq, 250.0), 0.0f, 0.5f);
	EXPECT_NEAR(ResponseAt(eq, 4000.0), 0.0f, 0.5f);
}

TEST(EQEight, InactiveBandsAreNotInTheCurve) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);
	SetBand(eq, 2, EQFilterType::Bell, 1000.0f, 12.0f, 1.0f, false);

	EXPECT_NEAR(ResponseAt(eq, 1000.0), 0.0f, 0.01f);

	Find(eq, "On 3A")->value = 1.0f;
	EXPECT_NEAR(ResponseAt(eq, 1000.0), 12.0f, 0.05f);
}

TEST(EQEight, ScaleRidesEveryGainBearingBand) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);
	SetBand(eq, 0, EQFilterType::Bell, 1000.0f, 10.0f, 1.0f);

	Find(eq, "Scale")->value = 50.0f;
	EXPECT_NEAR(ResponseAt(eq, 1000.0), 5.0f, 0.05f);

	Find(eq, "Scale")->value = 0.0f;
	EXPECT_NEAR(ResponseAt(eq, 1000.0), 0.0f, 0.01f);
}

// Scale is a gain control, so a cut - which has no gain - must not answer to it
TEST(EQEight, ScaleLeavesCutFiltersAlone) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);
	SetBand(eq, 0, EQFilterType::LowCut12, 1000.0f, 0.0f, 0.7071f);

	const float full = ResponseAt(eq, 250.0);
	Find(eq, "Scale")->value = 0.0f;
	EXPECT_NEAR(ResponseAt(eq, 250.0), full, 0.01f);
}

TEST(EQEight, AdaptiveQNarrowsABellWithoutMovingItsPeak) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);
	SetBand(eq, 0, EQFilterType::Bell, 1000.0f, 15.0f, 1.0f);

	const float wideSkirt = ResponseAt(eq, 2000.0);

	Find(eq, "Adapt Q")->value = 1.0f;
	EXPECT_NEAR(ResponseAt(eq, 1000.0), 15.0f, 0.05f) << "the peak itself must not move";
	EXPECT_LT(ResponseAt(eq, 2000.0), wideSkirt) << "an octave out the boost should have fallen further";
}

TEST(EQEight, OutputGainShiftsTheWholeCurve) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);
	Find(eq, "Gain")->value = -6.0f;

	EXPECT_NEAR(ResponseAt(eq, 100.0), -6.0f, 0.01f);
	EXPECT_NEAR(ResponseAt(eq, 10000.0), -6.0f, 0.01f);
}

// the curve is a promise about what the filters will do. this is the test that keeps
// the drawing and the DSP from drifting apart
TEST(EQEight, ProcessedAudioMatchesTheDrawnCurve) {
	for (double frequency : {120.0, 1000.0, 6000.0}) {
		EQEightProcessor eq;
		eq.PrepareToPlay(kRate);
		SilenceEveryBand(eq);
		SetBand(eq, 2, EQFilterType::Bell, 1000.0f, 9.0f, 1.4f);
		SetBand(eq, 5, EQFilterType::HighShelf, 5000.0f, -6.0f, 0.71f);

		const float predicted = ResponseAt(eq, frequency);

		std::vector<float> buffer = MakeSine(frequency, 16384, 1);
		RunBlocks(eq, buffer, 1);

		const float measuredDb = 20.0f * std::log10(TailPeak(buffer, 1, 0, 4096) / 0.5f);
		EXPECT_NEAR(measuredDb, predicted, 0.3f) << "at " << frequency << " Hz";
	}
}

TEST(EQEight, MidSideModeLeavesAMonoSignalAloneWhenOnlyTheSideIsCut) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);
	Find(eq, "Mode")->value = (float)(int)EQChannelMode::MidSide;
	// set B is the side, and a mono signal has none of it
	Find(eq, "Type 3B")->value = (float)(int)EQFilterType::Bell;
	Find(eq, "Freq 3B")->value = 1000.0f;
	Find(eq, "Gain 3B")->value = -15.0f;
	Find(eq, "Q 3B")->value = 1.0f;
	Find(eq, "On 3B")->value = 1.0f;

	const std::vector<float> input = MakeSine(1000.0, 8192, 2);
	std::vector<float> output = input;
	RunBlocks(eq, output, 2);

	EXPECT_NEAR(TailPeak(output, 2, 0, 2048), 0.5f, 0.005f);
	EXPECT_NEAR(TailPeak(output, 2, 1, 2048), 0.5f, 0.005f);
}

TEST(EQEight, MidSideModeStillCutsASideOnlySignal) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);
	Find(eq, "Mode")->value = (float)(int)EQChannelMode::MidSide;
	Find(eq, "Type 3B")->value = (float)(int)EQFilterType::Bell;
	Find(eq, "Freq 3B")->value = 1000.0f;
	Find(eq, "Gain 3B")->value = -15.0f;
	Find(eq, "Q 3B")->value = 1.0f;
	Find(eq, "On 3B")->value = 1.0f;

	// out of phase: pure side, no mid at all
	std::vector<float> buffer = MakeSine(1000.0, 8192, 2);
	for (size_t i = 1; i < buffer.size(); i += 2)
		buffer[i] = -buffer[i];

	RunBlocks(eq, buffer, 2);

	const float measuredDb = 20.0f * std::log10(TailPeak(buffer, 2, 0, 2048) / 0.5f);
	EXPECT_NEAR(measuredDb, -15.0f, 0.3f);
}

TEST(EQEight, LeftRightModeShapesEachChannelOnItsOwn) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);
	Find(eq, "Mode")->value = (float)(int)EQChannelMode::LeftRight;
	SetBand(eq, 2, EQFilterType::Bell, 1000.0f, -12.0f, 1.0f); // set A, so the left only

	std::vector<float> buffer = MakeSine(1000.0, 8192, 2);
	RunBlocks(eq, buffer, 2);

	const float leftDb = 20.0f * std::log10(TailPeak(buffer, 2, 0, 2048) / 0.5f);
	EXPECT_NEAR(leftDb, -12.0f, 0.3f);
	EXPECT_NEAR(TailPeak(buffer, 2, 1, 2048), 0.5f, 0.005f);
}

// the halfband pair is the only part of the signal path that is not a biquad, so it
// gets its own check: a flat EQ has to survive the 2x round trip intact
TEST(EQEight, OversamplingRoundTripIsTransparent) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);
	eq.SetOversampling(true);

	std::vector<float> buffer = MakeSine(1000.0, 8192, 1);
	RunBlocks(eq, buffer, 1);

	EXPECT_NEAR(TailPeak(buffer, 1, 0, 2048), 0.5f, 0.005f);
}

TEST(EQEight, OversamplingKeepsTheCurveItPromised) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);
	SetBand(eq, 2, EQFilterType::Bell, 1000.0f, 9.0f, 1.4f);
	eq.SetOversampling(true);

	std::vector<float> buffer = MakeSine(1000.0, 16384, 1);
	RunBlocks(eq, buffer, 1);

	const float measuredDb = 20.0f * std::log10(TailPeak(buffer, 1, 0, 4096) / 0.5f);
	EXPECT_NEAR(measuredDb, 9.0f, 0.3f);
}

// a corner dragged past Nyquist used to be where cookbook coefficients blow up
TEST(EQEight, ACornerAboveNyquistStaysStable) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);
	SetBand(eq, 0, EQFilterType::HighCut48, 22000.0f, 0.0f, 0.7071f);

	std::vector<float> buffer = MakeSine(1000.0, 4096, 1);
	RunBlocks(eq, buffer, 1);

	for (float sample : buffer)
		ASSERT_TRUE(std::isfinite(sample));
	EXPECT_LT(TailPeak(buffer, 1, 0, 1024), 1.0f);
}

TEST(EQEight, StateThatIsNotAParameterSurvivesADuplicate) {
	EQEightProcessor source;
	source.PrepareToPlay(kRate);
	source.SetOversampling(true);

	EQEightProcessor copy;
	copy.PrepareToPlay(kRate);
	copy.CopyStateFrom(source);

	EXPECT_TRUE(copy.IsOversampling());
}

TEST(EQEight, TheExtraLinesRoundTripThroughSaveAndLoad) {
	EQEightProcessor saved;
	saved.PrepareToPlay(kRate);
	saved.SetOversampling(true);
	Find(saved, "Freq 4A")->value = 777.0f;
	Find(saved, "Mode")->value = (float)(int)EQChannelMode::MidSide;

	std::stringstream stream;
	saved.Save(stream);

	EQEightProcessor loaded;
	loaded.PrepareToPlay(kRate);
	loaded.Load(stream);

	EXPECT_TRUE(loaded.IsOversampling());
	EXPECT_FLOAT_EQ(Find(loaded, "Freq 4A")->value, 777.0f);
	EXPECT_EQ(loaded.GetChannelMode(), EQChannelMode::MidSide);
	EXPECT_EQ(loaded.GetActiveSetCount(), 2);
}

// ================================================================
// ANALYZER
// ================================================================

namespace {

	// a deterministic broadband signal: every transform bin ends up with its own
	// non-zero magnitude, which is what makes the resolution check below meaningful
	std::vector<float> MakeNoise(int frames, int channels) {
		std::vector<float> buffer((size_t)frames * (size_t)channels, 0.0f);
		uint32_t state = 0x13579bdfu;
		for (int i = 0; i < frames; ++i) {
			state = state * 1664525u + 1013904223u;
			const float sample = ((float)(state >> 8) / 8388608.0f - 1.0f) * 0.25f;
			for (int c = 0; c < channels; ++c)
				buffer[(size_t)i * channels + c] = sample;
		}
		return buffer;
	}

	int LoudestAnalyzerPoint(const EQEightProcessor& eq) {
		int loudest = 0;
		float best = -1000.0f;
		for (int point = 0; point < eq.GetAnalyzerPointCount(); ++point) {
			double frequency = 0.0;
			float db = 0.0f;
			eq.GetAnalyzerPoint(point, frequency, db);
			if (db > best) {
				best = db;
				loudest = point;
			}
		}
		return loudest;
	}

} // namespace

TEST(EQEight, AnalyzerPutsAToneAtItsOwnFrequencyAndLevel) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);

	std::vector<float> buffer = MakeSine(1000.0, 16384, 1, 0.5f);
	RunBlocks(eq, buffer, 1);
	eq.RefreshAnalyzer();

	double frequency = 0.0;
	float db = 0.0f;
	eq.GetAnalyzerPoint(LoudestAnalyzerPoint(eq), frequency, db);

	EXPECT_NEAR(frequency, 1000.0, 50.0);
	// 0.5 full scale is -6 dBFS, less the scalloping loss of a tone that does not land
	// on a bin center
	EXPECT_NEAR(db, -6.0f, 2.0f);
}

// the bug this guards: below a few hundred Hz a display point is narrower than one
// transform bin, so looking the bin up directly repeated it across dozens of points
// and the spectrum came out as a staircase while the top end stayed smooth
TEST(EQEight, AnalyzerHasRealResolutionAtTheLowEnd) {
	EQEightProcessor eq;
	eq.PrepareToPlay(kRate);
	SilenceEveryBand(eq);

	std::vector<float> buffer = MakeNoise(16384, 1);
	RunBlocks(eq, buffer, 1);
	eq.RefreshAnalyzer();

	// points 8 to 60 cover roughly 12 Hz to 90 Hz, which is the stretch that used to
	// come out as a staircase. the first eight sit below the first transform bin's
	// center, where there is genuinely nothing to resolve
	int repeats = 0;
	double previousFrequency = 0.0;
	float previous = 0.0f;
	for (int point = 8; point < 60; ++point) {
		double frequency = 0.0;
		float db = 0.0f;
		eq.GetAnalyzerPoint(point, frequency, db);
		if (point > 8 && db == previous)
			++repeats;
		EXPECT_GT(frequency, previousFrequency) << "points must climb the axis";
		previousFrequency = frequency;
		previous = db;
	}

	EXPECT_EQ(repeats, 0) << "consecutive display points are sharing one transform bin";
}
