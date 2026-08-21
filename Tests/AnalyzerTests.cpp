#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <cmath>
#include <sstream>
#include <vector>

#include "ProcessorFactory.h"
#include "Processors/AnalyzerProcessor.h"

// ================================================================
// ANALYZER
// ================================================================

namespace {

	constexpr double kRate = 48000.0;
	constexpr double kPi = 3.14159265358979323846;

	// the engine hands processors a block at a time, and several of the things measured
	// here (the true peak tail, the loudness sub-blocks) only work if the boundaries are
	// real, so nothing below ever feeds one giant buffer
	constexpr int kBlock = 512;

	// a cheap deterministic source for the decorrelated-noise cases. std::rand would do
	// but its sequence is not guaranteed between implementations, and a stereo width
	// assertion that depends on the standard library is a flaky test waiting to happen
	struct Noise {
		uint32_t state;

		explicit Noise(uint32_t seed)
			: state(seed) {}

		float Next() {
			state = state * 1664525u + 1013904223u;
			return (float)((double)(state >> 8) / (double)(1u << 24)) * 2.0f - 1.0f;
		}
	};

	// run `frames` of audio through the analyzer, generated a sample at a time - the
	// generator returns one channel of one frame
	template <typename Generator>
	void RunGenerated(AnalyzerProcessor& analyzer, int frames, int channels, Generator generator) {
		ProcessContext context;
		context.sampleRate = kRate;
		context.isPlaying = true;

		std::vector<MIDIMessage> messages;
		std::vector<float> block((size_t)kBlock * channels, 0.0f);

		for (int offset = 0; offset < frames; offset += kBlock) {
			const int count = std::min(kBlock, frames - offset);
			for (int i = 0; i < count; ++i) {
				for (int channel = 0; channel < channels; ++channel)
					block[(size_t)i * channels + channel] = generator(offset + i, channel);
			}
			context.currentSample = offset;
			analyzer.Process(block.data(), count, channels, messages, context);
		}
	}

	void RunSine(AnalyzerProcessor& analyzer, double frequency, int frames, float amplitude, double phase = 0.0) {
		RunGenerated(analyzer, frames, 2, [&](int index, int) {
			return amplitude * (float)std::sin(2.0 * kPi * frequency * (double)index / kRate + phase);
		});
	}

	int LoudestSpectrumPoint(const AnalyzerProcessor& analyzer, int trace = 0) {
		int loudest = 0;
		float best = -1000.0f;
		for (int point = 0; point < analyzer.GetSpectrumPointCount(); ++point) {
			const float db = analyzer.GetSpectrumPointDb(trace, point);
			if (db > best) {
				best = db;
				loudest = point;
			}
		}
		return loudest;
	}

	// the level the curve shows at the display point nearest a frequency
	float SpectrumDbAt(const AnalyzerProcessor& analyzer, double frequency, int trace = 0) {
		int nearest = 0;
		double best = 1.0e18;
		for (int point = 0; point < analyzer.GetSpectrumPointCount(); ++point) {
			const double distance = std::fabs(analyzer.GetSpectrumPointFrequency(point) - frequency);
			if (distance < best) {
				best = distance;
				nearest = point;
			}
		}
		return analyzer.GetSpectrumPointDb(trace, nearest);
	}

	float BandCorrelation(const AnalyzerProcessor& analyzer, int band) {
		double low = 0.0;
		double high = 0.0;
		float correlation = 0.0f;
		float width = 0.0f;
		float db = 0.0f;
		analyzer.GetStereoBand(band, low, high, correlation, width, db);
		return correlation;
	}

	float BandWidth(const AnalyzerProcessor& analyzer, int band) {
		double low = 0.0;
		double high = 0.0;
		float correlation = 0.0f;
		float width = 0.0f;
		float db = 0.0f;
		analyzer.GetStereoBand(band, low, high, correlation, width, db);
		return width;
	}

	// the band whose span contains a frequency, so the assertions below name the
	// frequency they care about rather than an index that moves if kStereoBands does
	int BandContaining(const AnalyzerProcessor& analyzer, double frequency) {
		for (int band = 0; band < AnalyzerProcessor::kStereoBands; ++band) {
			double low = 0.0;
			double high = 0.0;
			float correlation = 0.0f;
			float width = 0.0f;
			float db = 0.0f;
			analyzer.GetStereoBand(band, low, high, correlation, width, db);
			if (frequency >= low && frequency < high)
				return band;
		}
		return -1;
	}

} // namespace

// the whole device rests on this one: an analyzer that alters what it measures is
// reporting on a signal the rest of the chain never receives
TEST(Analyzer, PassthroughIsBitExact) {
	AnalyzerProcessor analyzer;
	analyzer.PrepareToPlay(kRate);

	Noise noise(12345);
	std::vector<float> buffer((size_t)kBlock * 2, 0.0f);
	for (float& sample : buffer)
		sample = noise.Next() * 0.8f;

	const std::vector<float> original = buffer;

	ProcessContext context;
	context.sampleRate = kRate;
	std::vector<MIDIMessage> messages;
	analyzer.Process(buffer.data(), kBlock, 2, messages, context);

	for (size_t i = 0; i < buffer.size(); ++i)
		EXPECT_EQ(buffer[i], original[i]) << "sample " << i << " was modified";
}

// ================================================================
// SPECTRUM
// ================================================================

TEST(Analyzer, SpectrumPutsAToneAtItsOwnFrequencyAndLevel) {
	AnalyzerProcessor analyzer;
	analyzer.PrepareToPlay(kRate);
	analyzer.SetTilt(0.0f);

	RunSine(analyzer, 1000.0, 16384, 0.5f);
	analyzer.Refresh();

	const int loudest = LoudestSpectrumPoint(analyzer);
	EXPECT_NEAR(analyzer.GetSpectrumPointFrequency(loudest), 1000.0, 50.0);

	// 0.5 of full scale is -6 dBFS, less the scalloping loss of a tone that does not
	// land on a bin center
	EXPECT_NEAR(analyzer.GetSpectrumPointDb(0, loudest), -6.0f, 2.0f);
}

// the tilt exists so a mix that falls off toward the top reads flat. it has to pivot,
// not lift: if it moved 1 kHz too, comparing two tilts would tell you nothing
TEST(Analyzer, TiltRotatesAboutOneKilohertz) {
	AnalyzerProcessor flat;
	AnalyzerProcessor tilted;
	flat.PrepareToPlay(kRate);
	tilted.PrepareToPlay(kRate);
	flat.SetTilt(0.0f);
	tilted.SetTilt(6.0f);

	Noise noise(9001);
	std::vector<float> samples(16384);
	for (float& sample : samples)
		sample = noise.Next() * 0.3f;

	auto generator = [&](int index, int) { return samples[(size_t)index]; };
	RunGenerated(flat, 16384, 2, generator);
	RunGenerated(tilted, 16384, 2, generator);
	flat.Refresh();
	tilted.Refresh();

	// one octave up from the pivot is one whole tilt step up, and the pivot itself
	// does not move at all
	EXPECT_NEAR(SpectrumDbAt(tilted, 1000.0) - SpectrumDbAt(flat, 1000.0), 0.0f, 0.2f);
	EXPECT_NEAR(SpectrumDbAt(tilted, 2000.0) - SpectrumDbAt(flat, 2000.0), 6.0f, 0.2f);
	EXPECT_NEAR(SpectrumDbAt(tilted, 500.0) - SpectrumDbAt(flat, 500.0), -6.0f, 0.2f);
}

// mid/side is taken bin by bin off the two channel transforms rather than by
// transforming the summed time signals, so it is worth proving the two agree
TEST(Analyzer, MidSideModeSeparatesACenteredToneFromASidesOnlyOne) {
	AnalyzerProcessor analyzer;
	analyzer.PrepareToPlay(kRate);
	analyzer.SetTilt(0.0f);
	analyzer.SetChannelMode(AnalyzerChannelMode::MidSide);

	// 200 Hz dead center, 5 kHz in anti-phase so it exists only in the sides
	RunGenerated(analyzer, 16384, 2, [](int index, int channel) {
		const double center = std::sin(2.0 * kPi * 200.0 * (double)index / kRate) * 0.5;
		const double sides = std::sin(2.0 * kPi * 5000.0 * (double)index / kRate) * 0.5;
		return (float)(center + (channel == 0 ? sides : -sides));
	});
	analyzer.Refresh();

	EXPECT_GT(SpectrumDbAt(analyzer, 200.0, 0), SpectrumDbAt(analyzer, 200.0, 1) + 20.0f)
		<< "a centered tone belongs to mid";
	EXPECT_GT(SpectrumDbAt(analyzer, 5000.0, 1), SpectrumDbAt(analyzer, 5000.0, 0) + 20.0f)
		<< "an anti-phase tone belongs to side";
}

// ================================================================
// STEREO
// ================================================================

TEST(Analyzer, CorrelationReadsThePhaseRelationship) {
	AnalyzerProcessor mono;
	AnalyzerProcessor inverted;
	AnalyzerProcessor independent;
	mono.PrepareToPlay(kRate);
	inverted.PrepareToPlay(kRate);
	independent.PrepareToPlay(kRate);

	Noise source(4242);
	std::vector<float> samples(48000);
	for (float& sample : samples)
		sample = source.Next() * 0.5f;

	RunGenerated(mono, 48000, 2, [&](int index, int) { return samples[(size_t)index]; });
	RunGenerated(inverted, 48000, 2, [&](int index, int channel) {
		return channel == 0 ? samples[(size_t)index] : -samples[(size_t)index];
	});

	Noise left(1);
	Noise right(999983);
	std::vector<float> leftSamples(48000);
	std::vector<float> rightSamples(48000);
	for (int i = 0; i < 48000; ++i) {
		leftSamples[(size_t)i] = left.Next() * 0.5f;
		rightSamples[(size_t)i] = right.Next() * 0.5f;
	}
	RunGenerated(independent, 48000, 2, [&](int index, int channel) {
		return channel == 0 ? leftSamples[(size_t)index] : rightSamples[(size_t)index];
	});

	EXPECT_NEAR(mono.GetCorrelation(), 1.0f, 0.01f);
	EXPECT_NEAR(inverted.GetCorrelation(), -1.0f, 0.01f);
	EXPECT_NEAR(independent.GetCorrelation(), 0.0f, 0.15f);
}

// the readout the stereo tab exists for. one correlation number for the whole mix
// cannot tell a mono sub under a wide top from a mix that is half wide everywhere
TEST(Analyzer, StereoBandsSeparateAMonoLowEndFromAWideTop) {
	AnalyzerProcessor analyzer;
	analyzer.PrepareToPlay(kRate);

	Noise left(7);
	Noise right(70007);
	std::vector<float> highLeft(32768);
	std::vector<float> highRight(32768);
	for (int i = 0; i < 32768; ++i) {
		highLeft[(size_t)i] = left.Next() * 0.2f;
		highRight[(size_t)i] = right.Next() * 0.2f;
	}

	// 60 Hz identical in both channels, hiss that is independent per channel on top
	RunGenerated(analyzer, 32768, 2, [&](int index, int channel) {
		const float sub = 0.5f * (float)std::sin(2.0 * kPi * 60.0 * (double)index / kRate);
		return sub + (channel == 0 ? highLeft[(size_t)index] : highRight[(size_t)index]);
	});
	analyzer.Refresh();

	const int lowBand = BandContaining(analyzer, 60.0);
	const int highBand = BandContaining(analyzer, 8000.0);
	ASSERT_GE(lowBand, 0);
	ASSERT_GE(highBand, 0);

	EXPECT_GT(BandCorrelation(analyzer, lowBand), 0.9f) << "the sub is the same in both channels";
	EXPECT_LT(BandCorrelation(analyzer, highBand), 0.3f) << "the hiss is independent per channel";

	EXPECT_LT(BandWidth(analyzer, lowBand), 0.1f) << "a centered band has almost no side energy";
	EXPECT_GT(BandWidth(analyzer, highBand), 0.35f) << "decorrelated channels split evenly into mid and side";
}

// ================================================================
// LEVELS
// ================================================================

// a signal whose every sample sits at 0.707 but whose waveform reaches 1.0 between
// them. this is the case a plain sample-peak meter cannot see, and the one that makes
// a limiter set to -0.1 dBFS clip anyway once something resamples it
TEST(Analyzer, TruePeakSeesBetweenTheSamples) {
	AnalyzerProcessor analyzer;
	analyzer.PrepareToPlay(kRate);

	// a quarter of the sample rate, offset an eighth of a cycle: every sample lands on
	// +-sin(pi/4), and the peaks fall exactly halfway between two of them
	RunSine(analyzer, kRate / 4.0, 8192, 1.0f, kPi / 4.0);

	const float samplePeak = analyzer.GetPeakDb(0);
	const float truePeak = analyzer.GetTruePeakDb(0);

	EXPECT_NEAR(samplePeak, -3.01f, 0.1f);
	EXPECT_GT(truePeak, samplePeak + 2.0f) << "the interpolator never looked between the samples";
	EXPECT_NEAR(truePeak, 0.0f, 1.0f);
}

// crest factor is the OTT gauge: it is what collapses when a compressor is overdone,
// and a sine and a square bracket the range from both ends
TEST(Analyzer, CrestFactorTellsAPeakySignalFromASquashedOne) {
	AnalyzerProcessor sine;
	AnalyzerProcessor square;
	sine.PrepareToPlay(kRate);
	square.PrepareToPlay(kRate);

	RunSine(sine, 220.0, 32768, 0.8f);
	RunGenerated(square, 32768, 2, [](int index, int) {
		return std::sin(2.0 * kPi * 220.0 * (double)index / kRate) >= 0.0 ? 0.8f : -0.8f;
	});
	sine.Refresh();
	square.Refresh();

	// a sine is 3.01 dB peak over RMS by definition; a square is 0
	EXPECT_NEAR(sine.GetCrestFactorDb(), 3.01f, 0.3f);
	EXPECT_NEAR(square.GetCrestFactorDb(), 0.0f, 0.3f);
}

TEST(Analyzer, OffsetReportsADCBias) {
	AnalyzerProcessor analyzer;
	analyzer.PrepareToPlay(kRate);

	// several seconds, because the dc mean deliberately runs on a one second pole
	RunGenerated(analyzer, 240000, 2, [](int index, int) {
		return 0.2f + 0.3f * (float)std::sin(2.0 * kPi * 100.0 * (double)index / kRate);
	});

	EXPECT_NEAR(analyzer.GetOffset(0), 0.2f, 0.01f);
}

// ================================================================
// LOUDNESS
// ================================================================

// the aggregation is independent of the K-weighting curve, so this pins it down
// exactly: halving an amplitude is 6.02 dB whatever the filter did to the signal
TEST(Analyzer, LoudnessFollowsLevelExactly) {
	AnalyzerProcessor loud;
	AnalyzerProcessor quiet;
	loud.PrepareToPlay(kRate);
	quiet.PrepareToPlay(kRate);

	RunSine(loud, 1000.0, 144000, 0.5f);
	RunSine(quiet, 1000.0, 144000, 0.25f);
	loud.Refresh();
	quiet.Refresh();

	EXPECT_NEAR(loud.GetIntegratedLoudness() - quiet.GetIntegratedLoudness(), 6.02f, 0.05f);
	EXPECT_NEAR(loud.GetMomentaryLoudness() - quiet.GetMomentaryLoudness(), 6.02f, 0.05f);
}

// the gates are the whole reason integrated loudness is not just an average. without
// them, trailing silence quietly drags a master's reading down by several LU
TEST(Analyzer, IntegratedLoudnessGatesOutSilence) {
	AnalyzerProcessor withSilence;
	AnalyzerProcessor toneOnly;
	withSilence.PrepareToPlay(kRate);
	toneOnly.PrepareToPlay(kRate);

	auto tone = [](int index, int) {
		return 0.5f * (float)std::sin(2.0 * kPi * 1000.0 * (double)index / kRate);
	};

	RunGenerated(toneOnly, 144000, 2, tone);
	RunGenerated(withSilence, 144000, 2, tone);
	RunGenerated(withSilence, 480000, 2, [](int, int) { return 0.0f; });

	withSilence.Refresh();
	toneOnly.Refresh();

	// ungated, ten seconds of silence after three of tone would divide the mean power
	// by more than four and read some 6 LU low. what is left here is only the three
	// 400 ms blocks that straddle the boundary, which really are quieter and really do
	// belong in the measurement
	EXPECT_NEAR(withSilence.GetIntegratedLoudness(), toneOnly.GetIntegratedLoudness(), 0.5f);
	EXPECT_GT(withSilence.GetIntegratedLoudness(), toneOnly.GetIntegratedLoudness() - 1.0f);
}

// a loose calibration check against the standard's own constant. the tolerance is
// wide on purpose: the exact figure depends on the K-weighting gain at 1 kHz, which
// is what this is spot-checking rather than restating
TEST(Analyzer, LoudnessOfAFullScaleToneLandsNearZero) {
	AnalyzerProcessor analyzer;
	analyzer.PrepareToPlay(kRate);

	RunSine(analyzer, 1000.0, 144000, 1.0f);
	analyzer.Refresh();

	EXPECT_NEAR(analyzer.GetIntegratedLoudness(), -0.69f, 2.0f);
}

TEST(Analyzer, LoudnessRangeSpansTheQuietAndLoudHalves) {
	AnalyzerProcessor analyzer;
	analyzer.PrepareToPlay(kRate);

	// ten seconds quiet then ten loud, both well clear of the absolute gate. 12 dB
	// apart in amplitude, so the range has an answer to find
	auto quiet = [](int index, int) {
		return 0.05f * (float)std::sin(2.0 * kPi * 1000.0 * (double)index / kRate);
	};
	auto loud = [](int index, int) {
		return 0.5f * (float)std::sin(2.0 * kPi * 1000.0 * (double)index / kRate);
	};
	RunGenerated(analyzer, 480000, 2, quiet);
	RunGenerated(analyzer, 480000, 2, loud);
	analyzer.Refresh();

	// the relative gate is 20 LU down, so both halves survive it and the spread is
	// most of the 20 dB between them
	EXPECT_GT(analyzer.GetLoudnessRange(), 12.0f);
	EXPECT_LT(analyzer.GetLoudnessRange(), 21.0f);
}

TEST(Analyzer, ResetClearsTheIntegratedHistory) {
	AnalyzerProcessor analyzer;
	analyzer.PrepareToPlay(kRate);

	RunSine(analyzer, 1000.0, 144000, 0.5f);
	analyzer.Refresh();
	ASSERT_GT(analyzer.GetIntegratedLoudness(), -30.0f);

	analyzer.ResetMeasurements();
	analyzer.Refresh();

	EXPECT_LE(analyzer.GetIntegratedLoudness(), -99.0f);
}

// ================================================================
// STATE
// ================================================================

TEST(Analyzer, TheViewRoundTripsThroughSaveAndLoad) {
	AnalyzerProcessor saved;
	saved.SetView(AnalyzerView::Stereo);
	saved.SetChannelMode(AnalyzerChannelMode::MidSide);
	saved.SetTilt(3.0f);

	std::stringstream stream;
	saved.Save(stream);

	AnalyzerProcessor loaded;
	loaded.Load(stream);

	EXPECT_EQ(loaded.GetView(), AnalyzerView::Stereo);
	EXPECT_EQ(loaded.GetChannelMode(), AnalyzerChannelMode::MidSide);
	EXPECT_FLOAT_EQ(loaded.GetTilt(), 3.0f);
}

// the device rack's duplicate only copies parameter values, and this device has none,
// so everything it carries has to come through CopyStateFrom
TEST(Analyzer, DuplicateCarriesTheViewOver) {
	AnalyzerProcessor source;
	source.SetView(AnalyzerView::Loudness);
	source.SetTilt(6.0f);

	AnalyzerProcessor copy;
	copy.CopyStateFrom(source);

	EXPECT_EQ(copy.GetView(), AnalyzerView::Loudness);
	EXPECT_FLOAT_EQ(copy.GetTilt(), 6.0f);
}

TEST(Analyzer, IsRegisteredAsAnEffect) {
	const auto& factory = ProcessorFactory::Instance();

	EXPECT_TRUE(factory.IsRegistered("Analyzer"));
	EXPECT_FALSE(factory.IsInstrument("Analyzer"));
	EXPECT_NE(factory.Create("Analyzer"), nullptr);
}
