#include "PrecompHeader.h"
#include "PhaserProcessor.h"
#include "ProcessorFactory.h"
#include "Theme.h"
#include "Parameters/KnobParameter.h"
#include "Parameters/SliderParameter.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

REGISTER_PROCESSOR(PhaserProcessor, "Phaser", false)

namespace {

	// ================================================================
	// TABLES
	// ================================================================

	const char* kSpacingNames[] = {"Earth", "Space"};
	const char* kShapeNames[] = {"Sine", "Triangle", "Saw", "Square", "Random"};

	// beat divisions for the synced rate, longest first
	const char* kDivisionNames[] = {"8 Bars", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/32"};
	const double kDivisionBeats[] = {32.0, 16.0, 8.0, 4.0, 2.0, 1.0, 2.0 / 3.0, 0.5, 1.0 / 3.0, 0.25, 0.125};
	constexpr int kNumDivisions = 11;
	constexpr int kDefaultDivision = 5; // 1/4

	constexpr float kPi = 3.14159265359f;

	// how far the LFO pulls the frequency at Amount 100%. measured off a reference render:
	// at Amount 30% its center traces a 0.1 Hz sine of +-0.267 octaves about the frequency
	// knob, so the full throw is a little under an octave either way - a phaser sweep,
	// not a filter sweep
	constexpr float kLFOOctaves = 0.89f;

	// the same for the envelope follower. NOTE: unlike the LFO figure above this one is a
	// judgement call - the reference render was made with Envelope Amount at zero, so
	// there was nothing to measure it against
	constexpr float kEnvOctaves = 2.0f;

	// what the Feedback control is worth as a loop gain. the reference product's readout
	// runs to 0.99, but a loop gain of 0.99 around a unit-magnitude allpass chain would
	// resonate to +34 dB, and the reference does not: its render tops out at +4.5 dB and
	// its notches sit at -14.8 dB, which is exactly what this structure gives at a loop
	// gain of 0.576. so the readout is the reference's, and this is what it drives
	constexpr float kMaxLoopGain = 0.58f;

	// what Color turns into: how sharply a section turns its phase, and so how narrow the
	// notch it makes. the span is geometric, so the middle of the dial lands on 1.15 -
	// which is what the reference product's render measures at Color 50%
	constexpr float kColorQMin = 0.40f;
	constexpr float kColorQMax = 3.30f;

	// Color is not Space's to shape, so a Space section is broad and even
	constexpr float kSpaceQ = 0.70f;

	// coefficients are refreshed once per this many samples rather than per sample. twelve
	// tan() calls per channel is not free, and 16 samples is a third of a millisecond at
	// 48 kHz - far finer than the 30 Hz the LFO tops out at
	constexpr int kControlBlock = 16;

	// the vertical breathing room between two rows of one column
	constexpr float kRowGap = 3.0f;

	// a constant this small is inaudible at any gain the loop can reach, and it keeps the
	// filter states out of the denormal range after the input goes quiet
	constexpr float kAntiDenormal = 1.0e-18f;

	// the highest a section may sit: the coefficients run away as the center approaches
	// Nyquist
	double FrequencyCeiling(double sampleRate) {
		return sampleRate * 0.45;
	}

	// the two coefficients of a second-order allpass centered on `frequency`, from the
	// bilinear transform of (s^2 - (w/Q)s + w^2) / (s^2 + (w/Q)s + w^2). its numerator is
	// its denominator reversed, which is what makes the section allpass, so the whole
	// thing is two numbers and its phase runs 0 to -360 degrees, passing -180 at center
	void AllpassCoefficients(double frequency, double q, double sampleRate, double& c1, double& c2) {
		const double k = std::tan(kPi * std::clamp(frequency, 10.0, FrequencyCeiling(sampleRate)) / sampleRate);
		const double damping = k / std::max(q, 0.05);
		const double norm = 1.0 + damping + k * k;
		c1 = 2.0 * (k * k - 1.0) / norm;
		c2 = (1.0 - damping + k * k) / norm;
	}

	// one-pole smoothing coefficient for a given time constant
	float TimeCoeff(float seconds, double sampleRate) {
		if (seconds <= 0.0f)
			return 1.0f;
		return std::clamp(1.0f - std::exp(-1.0f / (seconds * (float)sampleRate)), 0.0f, 1.0f);
	}

	// bipolar -1..1, so the modulation swings the frequency both ways around the knob.
	// Random is handled by the caller, which owns the held value
	float EvaluateShape(int shape, double phase) {
		phase -= std::floor(phase);
		const float t = (float)phase;
		switch (shape) {
		case PhaserProcessor::ShapeTriangle:
			return t < 0.5f ? (t * 4.0f - 1.0f) : (3.0f - t * 4.0f);
		case PhaserProcessor::ShapeSaw:
			return 1.0f - t * 2.0f;
		case PhaserProcessor::ShapeSquare:
			return t < 0.5f ? 1.0f : -1.0f;
		case PhaserProcessor::ShapeSine:
		default:
			return std::sin(t * 2.0f * kPi);
		}
	}

	// ================================================================
	// GLYPHS
	// ================================================================
	// the reference product labels these controls with a waveform, a note head, a phi and
	// a spin arrow. the app loads no glyph range past Latin-1, so they are drawn rather
	// than typed

	void DrawShapeGlyph(ImDrawList* drawList, const ImVec2& center, float width, float height, int shape, ImU32 color) {
		const float halfWidth = width * 0.5f;
		const float halfHeight = height * 0.5f;
		const float thickness = 1.4f;

		if (shape == PhaserProcessor::ShapeRandom) {
			// a stepped run of held values, which is what a sample-and-hold puts out
			const float levels[4] = {0.7f, -0.3f, 0.4f, -0.8f};
			const float step = width * 0.25f;
			for (int i = 0; i < 4; ++i) {
				const float x = center.x - halfWidth + step * (float)i;
				const float y = center.y - levels[i] * halfHeight;
				drawList->AddLine(ImVec2(x, y), ImVec2(x + step, y), color, thickness);
				if (i > 0) {
					const float previousY = center.y - levels[i - 1] * halfHeight;
					drawList->AddLine(ImVec2(x, previousY), ImVec2(x, y), color, thickness);
				}
			}
			return;
		}

		const int segments = 24;
		ImVec2 previous;
		for (int i = 0; i <= segments; ++i) {
			const double phase = (double)i / (double)segments;
			const ImVec2 point(center.x - halfWidth + width * (float)phase,
							   center.y - EvaluateShape(shape, phase) * halfHeight);
			// a square's vertical edge has to be drawn, not interpolated across
			if (i > 0 && shape == PhaserProcessor::ShapeSquare && previous.y != point.y)
				drawList->AddLine(ImVec2(point.x, previous.y), point, color, thickness);
			else if (i > 0)
				drawList->AddLine(previous, point, color, thickness);
			previous = point;
		}
	}

	// an eighth note: head, stem and flag
	void DrawNoteGlyph(ImDrawList* drawList, const ImVec2& center, float size, ImU32 color) {
		const float head = size * 0.16f;
		const ImVec2 headCenter(center.x - size * 0.12f, center.y + size * 0.22f);
		drawList->AddCircleFilled(headCenter, head, color, 10);
		drawList->AddLine(ImVec2(headCenter.x + head, headCenter.y),
						  ImVec2(headCenter.x + head, center.y - size * 0.30f), color, 1.4f);
		drawList->AddBezierQuadratic(ImVec2(headCenter.x + head, center.y - size * 0.30f),
									 ImVec2(headCenter.x + size * 0.34f, center.y - size * 0.18f),
									 ImVec2(headCenter.x + head + size * 0.06f, center.y + size * 0.02f),
									 color, 1.4f, 0);
	}

	// phi: the stereo offset between the two LFOs
	void DrawPhiGlyph(ImDrawList* drawList, const ImVec2& center, float size, ImU32 color) {
		drawList->AddCircle(center, size * 0.24f, color, 12, 1.4f);
		drawList->AddLine(ImVec2(center.x, center.y - size * 0.40f),
						  ImVec2(center.x, center.y + size * 0.40f), color, 1.4f);
	}

	// an arrow chasing its own tail: the two LFOs running at different rates
	void DrawSpinGlyph(ImDrawList* drawList, const ImVec2& center, float size, ImU32 color) {
		const float radius = size * 0.30f;
		drawList->PathArcTo(center, radius, kPi * 0.35f, kPi * 1.95f, 20);
		drawList->PathStroke(color, 0, 1.4f);
		const ImVec2 tip(center.x + radius * std::cos(kPi * 1.95f), center.y + radius * std::sin(kPi * 1.95f));
		drawList->AddTriangleFilled(ImVec2(tip.x - size * 0.10f, tip.y - size * 0.04f),
									ImVec2(tip.x + size * 0.06f, tip.y - size * 0.10f),
									ImVec2(tip.x + size * 0.02f, tip.y + size * 0.08f), color);
	}

	// a small square button whose face the caller paints, and the rect it painted into
	bool IconToggle(const char* id, bool active, const ImVec2& size, ImVec2& outMin) {
		const Theme& th = Theme::Instance();
		outMin = ImGui::GetCursorScreenPos();
		const bool pressed = ImGui::InvisibleButton(id, size);
		const ImU32 fill = active ? th.accent : (ImGui::IsItemHovered() ? th.bgHover : th.bgPanelAlt);
		ImGui::GetWindowDrawList()->AddRectFilled(outMin, ImVec2(outMin.x + size.x, outMin.y + size.y),
												  fill, ImGui::GetStyle().FrameRounding);
		return pressed;
	}

	// flip a chooser or a switch and record it as one undo entry
	void SetChoice(Parameter* parameter, float wanted) {
		const float oldValue = parameter->value;
		parameter->value = wanted;
		parameter->CommitEditImmediate(oldValue);
	}

} //namespace

// ================================================================
// SETUP
// ================================================================

PhaserProcessor::PhaserProcessor() {
	// NOTE: the names are the serialization and automation keys. renaming one silently
	// orphans its curve, so they stay put even when the panel prints something shorter
	pPoles = AddParameter(std::make_unique<KnobParameter>("Poles", 4.0f, 1.0f, (float)kMaxPoles, ImGuiKnobVariant_Integer));
	pSpacing = AddParameter(std::make_unique<SliderParameter>("Spacing", (float)SpacingEarth, 0.0f, (float)(kNumSpacings - 1)));
	pColor = AddParameter(std::make_unique<KnobParameter>("Color", 50.0f, 0.0f, 100.0f, ImGuiKnobVariant_Percent));
	pDryWet = AddParameter(std::make_unique<KnobParameter>("Dry/Wet", 50.0f, 0.0f, 100.0f, ImGuiKnobVariant_Percent));

	// the frequency span and its default are the reference product's; feedback stops just
	// short of 1 because the loop's own gain is what a phaser's resonance is made of
	pFrequency = AddParameter(std::make_unique<KnobParameter>("Frequency", 1050.0f, kMinFrequency, kMaxFrequency, ImGuiKnobVariant_Hertz));
	pFeedback = AddParameter(std::make_unique<KnobParameter>("Feedback", 0.5f, 0.0f, 0.99f, ImGuiKnobVariant_Linear));

	pEnvAmount = AddParameter(std::make_unique<KnobParameter>("Env Amount", 0.0f, -100.0f, 100.0f, ImGuiKnobVariant_PercentBipolar));
	pEnvAttack = AddParameter(std::make_unique<KnobParameter>("Env Attack", 6.0f, 0.1f, 100.0f, ImGuiKnobVariant_Milliseconds));
	pEnvRelease = AddParameter(std::make_unique<KnobParameter>("Env Release", 200.0f, 1.0f, 1000.0f, ImGuiKnobVariant_Milliseconds));

	// NOTE: the reference product opens with the LFO amount at zero, so the device does
	// nothing until it is turned up. this one opens sweeping - a phaser dropped on a track
	// should sound like a phaser - and everything else about the section matches
	pLFOAmount = AddParameter(std::make_unique<KnobParameter>("LFO Amount", 30.0f, 0.0f, 100.0f, ImGuiKnobVariant_Percent));
	pLFOShape = AddParameter(std::make_unique<SliderParameter>("LFO Shape", (float)ShapeSine, 0.0f, (float)(kNumShapes - 1)));
	pLFOSync = AddParameter(std::make_unique<SliderParameter>("LFO Sync", 0.0f, 0.0f, 1.0f));
	pLFORate = AddParameter(std::make_unique<KnobParameter>("LFO Rate", 0.5f, 0.01f, 30.0f, ImGuiKnobVariant_Hertz));
	pLFODivision = AddParameter(std::make_unique<SliderParameter>("LFO Division", (float)kDefaultDivision, 0.0f, (float)(kNumDivisions - 1)));
	pLFOStereo = AddParameter(std::make_unique<SliderParameter>("LFO Stereo", (float)StereoPhase, 0.0f, (float)(kNumStereoModes - 1)));
	pLFOPhase = AddParameter(std::make_unique<KnobParameter>("LFO Phase", 180.0f, 0.0f, 360.0f, ImGuiKnobVariant_Degrees));
	pLFOSpin = AddParameter(std::make_unique<KnobParameter>("LFO Spin", 0.0f, -100.0f, 100.0f, ImGuiKnobVariant_PercentBipolar));
}

void PhaserProcessor::PrepareToPlay(double sampleRate) {
	mSampleRate = std::max(1.0, sampleRate);
	// stereo up front so the common case never grows the vector from the audio thread
	mChannels.resize(2);
	Reset();
}

void PhaserProcessor::Reset() {
	for (auto& channel : mChannels)
		channel = ChannelState();
	mEnvelope = 0.0f;
	mLFOPhase[0] = 0.0;
	mLFOPhase[1] = 0.0;
	mRandomCycle[0] = -1;
	mRandomCycle[1] = -1;
}

int PhaserProcessor::SectionCount() const {
	return std::clamp((int)std::lround(pPoles->value), 1, kMaxPoles);
}

float PhaserProcessor::SectionQ() const {
	if (pSpacing->value >= 0.5f)
		return kSpaceQ;
	const float t = std::clamp(pColor->value / 100.0f, 0.0f, 1.0f);
	return kColorQMin * std::pow(kColorQMax / kColorQMin, t);
}

float PhaserProcessor::LoopGain() const {
	const float span = std::max(pFeedback->maxValue, 1.0e-6f);
	return kMaxLoopGain * std::clamp(pFeedback->value / span, 0.0f, 1.0f);
}

int PhaserProcessor::CurrentShape() const {
	return std::clamp((int)std::lround(pLFOShape->value), 0, kNumShapes - 1);
}

float PhaserProcessor::NextRandom() {
	mRandomState ^= mRandomState << 13;
	mRandomState ^= mRandomState >> 17;
	mRandomState ^= mRandomState << 5;
	return (float)(mRandomState & 0xFFFFFFu) / (float)0xFFFFFF * 2.0f - 1.0f;
}

// ================================================================
// SECTION PLACEMENT
// ================================================================

int PhaserProcessor::GetSectionFrequencies(float centerFrequency, float* out) const {
	const int sections = SectionCount();
	const float center = std::clamp(centerFrequency, kMinFrequency, kMaxFrequency);
	const bool space = pSpacing->value >= 0.5f;
	const float ceiling = (float)FrequencyCeiling(mSampleRate);

	for (int section = 0; section < sections; ++section) {
		// Earth stacks them: every section on the frequency knob, so the notches come out
		// in the pattern one resonance repeated N times produces. Space puts them on
		// harmonics of it, which spaces the notches evenly in Hz - a comb, the metallic one
		const float frequency = space ? center * (float)(section + 1) : center;
		out[section] = std::clamp(frequency, 10.0f, ceiling);
	}
	return sections;
}

float PhaserProcessor::ModulatedFrequency(float modulation) const {
	const float envAmount = std::clamp(pEnvAmount->value / 100.0f, -1.0f, 1.0f);
	const float lfoAmount = std::clamp(pLFOAmount->value / 100.0f, 0.0f, 1.0f);
	const float octaves = envAmount * kEnvOctaves * mEnvelope + lfoAmount * kLFOOctaves * modulation;
	return std::clamp(std::clamp(pFrequency->value, kMinFrequency, kMaxFrequency) * std::exp2(octaves),
					  kMinFrequency, kMaxFrequency);
}

void PhaserProcessor::UpdateCoefficients(ChannelState& state, float centerFrequency) {
	float frequencies[kMaxPoles];
	const int count = GetSectionFrequencies(centerFrequency, frequencies);
	const double q = (double)SectionQ();
	for (int section = 0; section < count; ++section) {
		double c1 = 0.0, c2 = 0.0;
		AllpassCoefficients(frequencies[section], q, mSampleRate, c1, c2);
		state.c1[section] = (float)c1;
		state.c2[section] = (float)c2;
	}
}

// ================================================================
// PROCESS
// ================================================================

void PhaserProcessor::Process(float* buffer, int numFrames, int numChannels,
							  std::vector<MIDIMessage>& mIDIMessages,
							  const ProcessContext& context) {
	// a phaser holds no notes and generates none
	(void)mIDIMessages;

	if (numFrames <= 0 || numChannels <= 0)
		return;

	if ((int)mChannels.size() < numChannels)
		mChannels.resize((size_t)numChannels);

	if (context.sampleRate > 0.0)
		mSampleRate = context.sampleRate;
	const double sampleRate = mSampleRate;

	const int sections = SectionCount();
	const int shape = CurrentShape();
	const float wet = std::clamp(pDryWet->value / 100.0f, 0.0f, 1.0f);
	const float feedback = LoopGain();
	const float attackCoeff = TimeCoeff(pEnvAttack->value * 0.001f, sampleRate);
	const float releaseCoeff = TimeCoeff(pEnvRelease->value * 0.001f, sampleRate);

	// ---- how fast the two LFOs run, and how far apart ----
	const bool synced = pLFOSync->value >= 0.5f;
	double cyclesPerSecond;
	if (synced) {
		const int division = std::clamp((int)std::lround(pLFODivision->value), 0, kNumDivisions - 1);
		cyclesPerSecond = std::max(context.bpm, 1.0) / 60.0 / kDivisionBeats[division];
	} else {
		cyclesPerSecond = (double)std::max(pLFORate->value, 0.01f);
	}

	// NOTE: Spin only means anything to a free-running pair. a synced LFO is chased off
	// the transport, and two synced rates that differ are just two divisions - which the
	// chooser beside the rate already offers
	const bool spinning = pLFOStereo->value >= 0.5f && !synced;
	const double rateLeft = cyclesPerSecond;
	const double rateRight = spinning
								 ? cyclesPerSecond * (1.0 + (double)std::clamp(pLFOSpin->value / 100.0f, -0.9f, 0.9f))
								 : cyclesPerSecond;
	const double phaseOffset = spinning ? 0.0 : (double)pLFOPhase->value / 360.0;

	// a synced LFO is read off the transport rather than accumulated, so a seek lands on
	// the right point of the cycle and an offline render matches what was heard. the whole
	// cycle count is kept, not just the fraction, so the sample-and-hold below still sees
	// a cycle boundary go by
	if (synced && context.isPlaying) {
		mLFOPhase[0] = (double)context.currentSample / sampleRate * cyclesPerSecond;
		mLFOPhase[1] = mLFOPhase[0];
	}

	const double incrementLeft = rateLeft / sampleRate;
	const double incrementRight = rateRight / sampleRate;

	// where each lane's stages sit this control block, carried across the frames between
	// two refreshes
	float laneFrequency[2] = {mVisFrequency[0], mVisFrequency[1]};

	for (int frame = 0; frame < numFrames; ++frame) {
		// the detector runs first: the coefficients below are computed from where it is
		float peak = 0.0f;
		for (int channel = 0; channel < numChannels; ++channel)
			peak = std::max(peak, std::fabs(buffer[(size_t)frame * numChannels + channel]));
		mEnvelope += (std::min(peak, 1.0f) - mEnvelope) * (peak > mEnvelope ? attackCoeff : releaseCoeff);

		if (frame % kControlBlock == 0) {
			for (int lane = 0; lane < 2; ++lane) {
				const double phase = mLFOPhase[lane] + (lane == 1 ? phaseOffset : 0.0);
				float modulation;
				if (shape == ShapeRandom) {
					// one fresh value per cycle, which is the whole of a sample-and-hold
					const int64_t cycle = (int64_t)std::floor(phase);
					if (cycle != mRandomCycle[lane]) {
						mRandomCycle[lane] = cycle;
						mRandomValue[lane] = NextRandom();
					}
					modulation = mRandomValue[lane];
				} else {
					modulation = EvaluateShape(shape, phase);
				}
				laneFrequency[lane] = ModulatedFrequency(modulation);
			}
			for (int channel = 0; channel < numChannels; ++channel)
				UpdateCoefficients(mChannels[(size_t)channel], laneFrequency[channel == 0 ? 0 : 1]);
		}

		for (int channel = 0; channel < numChannels; ++channel) {
			ChannelState& state = mChannels[(size_t)channel];
			const float dry = buffer[(size_t)frame * numChannels + channel];

			// NOTE: no saturator in the loop. an allpass chain has unit magnitude at every
			// frequency, so the loop gain IS the resonance, and kMaxLoopGain keeps it
			// where the reference product's is - a peak of +4.5 dB, nowhere near needing
			// to be clipped back
			float value = dry + feedback * state.feedback + kAntiDenormal;

			for (int section = 0; section < sections; ++section) {
				const float c1 = state.c1[section];
				const float c2 = state.c2[section];
				const float output = c2 * value + c1 * state.x1[section] + state.x2[section] -
									 c1 * state.y1[section] - c2 * state.y2[section];
				state.x2[section] = state.x1[section];
				state.x1[section] = value;
				state.y2[section] = state.y1[section];
				state.y1[section] = output;
				value = output;
			}
			state.feedback = value;

			// the notches live in the SUM, not in the chain: halved so a full-wet cancel
			// is a null rather than a 6 dB lift everywhere it does not cancel
			const float wetSample = 0.5f * (dry + value);
			buffer[(size_t)frame * numChannels + channel] = dry + wet * (wetSample - dry);
		}

		mLFOPhase[0] += incrementLeft;
		mLFOPhase[1] += incrementRight;
	}

	// the accumulator counts cycles forever, so it is only pulled back when it gets big
	// enough for a double to start losing the fraction. a whole number of cycles is
	// subtracted, which costs at most one extra sample-and-hold roll every few hours
	for (int lane = 0; lane < 2; ++lane) {
		if (mLFOPhase[lane] > 1.0e6)
			mLFOPhase[lane] -= 1.0e6;
	}

	mVisFrequency[0] = laneFrequency[0];
	mVisFrequency[1] = laneFrequency[1];
	mVisEnvelope = mEnvelope;
}

// ================================================================
// RESPONSE
// ================================================================

void PhaserProcessor::GetResponseDb(const double* frequencies, float* outDb, int count) const {
	float sectionFrequency[kMaxPoles];
	const int sections = GetSectionFrequencies(pFrequency->value, sectionFrequency);

	double c1[kMaxPoles], c2[kMaxPoles];
	const double q = (double)SectionQ();
	for (int section = 0; section < sections; ++section)
		AllpassCoefficients(sectionFrequency[section], q, mSampleRate, c1[section], c2[section]);

	const double feedback = (double)LoopGain();
	const double wet = std::clamp((double)pDryWet->value / 100.0, 0.0, 1.0);

	for (int i = 0; i < count; ++i) {
		const double omega = 2.0 * (double)kPi * frequencies[i] / mSampleRate;
		const std::complex<double> zInverse = std::polar(1.0, -omega);

		const std::complex<double> zInverse2 = zInverse * zInverse;
		std::complex<double> chain(1.0, 0.0);
		for (int section = 0; section < sections; ++section) {
			chain *= (c2[section] + c1[section] * zInverse + zInverse2) /
					 (1.0 + c1[section] * zInverse + c2[section] * zInverse2);
		}

		// the closed loop the audio path runs: the chain sees the input plus its own last
		// output, and the sum of dry and chain is what the dry/wet control blends toward
		const std::complex<double> looped = chain / (1.0 - feedback * chain);
		const std::complex<double> response = (1.0 - wet) + wet * 0.5 * (1.0 + looped);
		outDb[i] = (float)(20.0 * std::log10(std::max(std::abs(response), 1.0e-9)));
	}
}

// ================================================================
// UI
// ================================================================

bool PhaserProcessor::RenderCustomUI(const ImVec2& size) {
	ImGuiStyle& style = ImGui::GetStyle();
	const float lineHeight = ImGui::GetTextLineHeight();
	const float frameHeight = ImGui::GetFrameHeight();

	// the LFO column is the tallest of the four - a section header over three knob rows -
	// so it is what decides the dial size the other three then share. the dial never grows
	// past the size knobs are drawn at everywhere else in the app
	const float radius = std::min(KnobParameter::RadiusForHeight((size.y - lineHeight - kRowGap * 3.0f) / 3.0f),
								  KnobParameter::kDefaultRadius);

	// each column is as wide as the widest thing it has to hold without eliding
	const float mainWidth = std::max(ImGui::CalcTextSize("Dry/Wet").x, radius * 2.0f) + 8.0f;
	const float envWidth = std::max(ImGui::CalcTextSize("Envelope").x, radius * 2.0f) + 8.0f;
	const float lfoKnobWidth = std::max(ImGui::CalcTextSize("0.01 Hz").x, radius * 2.0f) + 8.0f;
	const float lfoIconWidth = std::max(ImGui::CalcTextSize("Shape").x + 4.0f, frameHeight * 2.0f);
	const float lfoWidth = lfoKnobWidth + style.ItemInnerSpacing.x + lfoIconWidth;
	const float fieldWidth = size.x - mainWidth - envWidth - lfoWidth - style.ItemSpacing.x * 3.0f;

	// the left column carries the most rows; below this there is no honest way to draw the
	// panel and the rack falls back to the plain parameter list
	const float mainNatural = KnobParameter::SizedHeight(radius) * 2.0f + frameHeight * 2.0f + lineHeight + kRowGap * 3.0f;
	if (radius <= 0.0f || fieldWidth < 130.0f || size.y < mainNatural)
		return false;

	// every column is placed explicitly rather than with SameLine: the field submits its
	// own items at absolute positions, which leaves nothing sane for SameLine to measure
	// against. the block is closed with an item at the end - ImGui asserts on a child that
	// ends on a bare SetCursorScreenPos
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	float x = origin.x;

	ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
	RenderMainColumn(mainWidth, size.y, radius);
	x += mainWidth + style.ItemSpacing.x;

	RenderField(ImVec2(x, origin.y), ImVec2(fieldWidth, size.y));
	x += fieldWidth + style.ItemSpacing.x;

	ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
	RenderEnvelopeColumn(envWidth, size.y, radius);
	x += envWidth + style.ItemSpacing.x;

	ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
	RenderLFOColumn(lfoWidth, lfoKnobWidth, size.y, radius);

	ImGui::SetCursorScreenPos(origin);
	ImGui::Dummy(size);
	return true;
}

// poles over the Earth / Space chooser over color and dry/wet, which is the column the
// reference product stands down its left edge
void PhaserProcessor::RenderMainColumn(float width, float height, float radius) {
	const Theme& th = Theme::Instance();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const float lineHeight = ImGui::GetTextLineHeight();
	const float frameHeight = ImGui::GetFrameHeight();
	const float blockHeight = KnobParameter::SizedHeight(radius);
	const float natural = blockHeight * 2.0f + frameHeight * 2.0f + lineHeight;
	const float gap = std::max((height - natural) / 3.0f, kRowGap);
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	float y = origin.y;

	pPoles->DrawSized(radius, width, "Poles");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Allpass sections in the chain, and so notches in the spectrum");
	y += blockHeight + gap;

	// the chooser reads as the mode it is in rather than offering the other one, which is
	// how the reference product labels it
	const int spacing = pSpacing->value >= 0.5f ? SpacingSpace : SpacingEarth;
	ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
	ImVec2 spacingMin;
	if (IconToggle("##PhaserSpacing", true, ImVec2(width, frameHeight), spacingMin))
		SetChoice(pSpacing, spacing == SpacingEarth ? (float)SpacingSpace : (float)SpacingEarth);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Earth: every section on one frequency, Color sets how sharp - the warm one\n"
						  "Space: the sections sit on harmonics, so the notches comb evenly - the metallic one");
	{
		const ImVec2 textSize = ImGui::CalcTextSize(kSpacingNames[spacing]);
		drawList->AddText(ImVec2(spacingMin.x + (width - textSize.x) * 0.5f,
								 spacingMin.y + (frameHeight - textSize.y) * 0.5f),
						  th.textOnAccent, kSpacingNames[spacing]);
	}
	y += frameHeight + gap;

	ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
	ImGui::TextUnformatted("Color");
	ImGui::SetCursorScreenPos(ImVec2(origin.x, y + lineHeight));
	// Color only reaches Earth's spread, so in Space it goes dim rather than pretending
	const bool colorLive = spacing == SpacingEarth;
	if (!colorLive)
		ImGui::BeginDisabled();
	pColor->DrawCompact(width, nullptr);
	if (!colorLive)
		ImGui::EndDisabled();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(colorLive ? "How sharply each section turns, and so how narrow its notch" : "Earth only");
	y += lineHeight + frameHeight + gap;

	ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
	pDryWet->DrawSized(radius, width, "Dry/Wet");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("How much of the summed signal replaces the dry one - the notches\n"
						  "live in that sum, so this is also how deep they cut");

	ImGui::SetCursorScreenPos(origin);
	ImGui::Dummy(ImVec2(width, height));
}

// the XY field over frequency and feedback, and the two readouts under it
void PhaserProcessor::RenderField(const ImVec2& position, const ImVec2& size) {
	const Theme& th = Theme::Instance();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImGuiStyle& style = ImGui::GetStyle();

	const float lineHeight = ImGui::GetTextLineHeight();
	const float rowHeight = ImGui::GetFrameHeight();
	const float fieldHeight = std::max(size.y - rowHeight - kRowGap, 40.0f);
	const ImVec2 fieldMin = position;
	const ImVec2 fieldMax(position.x + size.x, position.y + fieldHeight);

	drawList->AddRectFilled(fieldMin, fieldMax, th.bgDeepest);

	const float logSpan = std::log(kMaxFrequency / kMinFrequency);
	auto frequencyToX = [&](float hz) {
		const float t = (float)(std::log(std::clamp(hz, kMinFrequency, kMaxFrequency) / kMinFrequency) / logSpan);
		return fieldMin.x + size.x * std::clamp(t, 0.0f, 1.0f);
	};
	auto feedbackToY = [&](float value) {
		const float t = (value - pFeedback->minValue) / std::max(pFeedback->maxValue - pFeedback->minValue, 1.0e-6f);
		return fieldMax.y - fieldHeight * std::clamp(t, 0.0f, 1.0f);
	};

	// the handle stays on the knob, the way the reference product's does. these two lines
	// are where the envelope and the LFO have actually taken the stages this frame, one
	// per stereo lane, so a Phase or Spin setting can be seen as well as heard
	for (int lane = 0; lane < 2; ++lane) {
		const float x = frequencyToX(mVisFrequency[lane]);
		drawList->AddLine(ImVec2(x, fieldMin.y), ImVec2(x, fieldMax.y),
						  Theme::WithAlpha(th.graphCurveCool, lane == 0 ? 110 : 55), 1.0f);
	}

	ImGui::SetCursorScreenPos(fieldMin);
	ImGui::InvisibleButton("##PhaserField", ImVec2(size.x, fieldHeight));
	const bool held = ImGui::IsItemActive();
	const bool hovered = ImGui::IsItemHovered();

	if (ImGui::IsItemActivated()) {
		mDragOldFrequency = pFrequency->value;
		mDragOldFeedback = pFeedback->value;
		pFrequency->Select();
	}
	if (held) {
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		const float tx = std::clamp((mouse.x - fieldMin.x) / std::max(size.x, 1.0f), 0.0f, 1.0f);
		const float ty = std::clamp((fieldMax.y - mouse.y) / std::max(fieldHeight, 1.0f), 0.0f, 1.0f);
		pFrequency->value = kMinFrequency * std::exp(logSpan * tx);
		pFeedback->value = pFeedback->minValue + ty * (pFeedback->maxValue - pFeedback->minValue);
	}
	if (ImGui::IsItemDeactivated()) {
		// a drag moves two parameters at once and the edit gesture is a single global
		// slot, so each axis that actually moved is committed on release instead
		pFrequency->CommitEditImmediate(mDragOldFrequency);
		pFeedback->CommitEditImmediate(mDragOldFeedback);
	}
	if (hovered && !held)
		ImGui::SetTooltip("Drag: frequency across, feedback up");

	const ImVec2 handle(frequencyToX(pFrequency->value), feedbackToY(pFeedback->value));
	drawList->AddCircle(handle, 7.0f, (held || hovered) ? th.accentHover : th.accent, 0, 2.0f);
	drawList->AddRect(fieldMin, fieldMax, th.border);

	// ---- readouts ----
	const float rowY = fieldMax.y + kRowGap;
	const float half = (size.x - style.ItemInnerSpacing.x) * 0.5f;
	const float textY = rowY + (rowHeight - lineHeight) * 0.5f;

	auto readout = [&](float columnX, const char* label, KnobParameter* parameter) {
		const float labelWidth = ImGui::CalcTextSize(label).x + 4.0f;
		drawList->AddText(ImVec2(columnX, textY), th.textMuted, label);
		ImGui::SetCursorScreenPos(ImVec2(columnX + labelWidth, rowY));
		parameter->DrawCompact(std::max(half - labelWidth, 30.0f), nullptr);
	};

	readout(fieldMin.x, "Frequency", pFrequency);
	readout(fieldMin.x + half + style.ItemInnerSpacing.x, "Feedback", pFeedback);

	ImGui::SetCursorScreenPos(fieldMin);
	ImGui::Dummy(size);
}

// the envelope follower's three dials. the section title doubles as the first one's label,
// which is how the reference product saves the row
void PhaserProcessor::RenderEnvelopeColumn(float width, float height, float radius) {
	const float blockHeight = KnobParameter::SizedHeight(radius);
	const float gap = std::max((height - blockHeight * 3.0f) * 0.5f, kRowGap);
	const ImVec2 origin = ImGui::GetCursorScreenPos();

	KnobParameter* rows[3] = {pEnvAmount, pEnvAttack, pEnvRelease};
	const char* labels[3] = {"Envelope", "Attack", "Release"};
	const char* tooltips[3] = {
		"How far the input's own level pushes the frequency, and which way",
		"How fast the follower rises to a transient",
		"How long it takes to fall back"};

	for (int row = 0; row < 3; ++row) {
		ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + (float)row * (blockHeight + gap)));
		rows[row]->DrawSized(radius, width, labels[row]);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", tooltips[row]);
	}

	ImGui::SetCursorScreenPos(origin);
	ImGui::Dummy(ImVec2(width, height));
}

// amount and shape, then the rate with its Hz / note switch, then the stereo control with
// its phase / spin switch - each dial paired with the buttons that decide what it means
void PhaserProcessor::RenderLFOColumn(float width, float knobWidth, float height, float radius) {
	const Theme& th = Theme::Instance();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImGuiStyle& style = ImGui::GetStyle();

	const float lineHeight = ImGui::GetTextLineHeight();
	const float frameHeight = ImGui::GetFrameHeight();
	const float blockHeight = KnobParameter::SizedHeight(radius);
	const float headerHeight = lineHeight + kRowGap;
	const float gap = std::max((height - headerHeight - blockHeight * 3.0f) * 0.5f, kRowGap);
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const float iconX = origin.x + knobWidth + style.ItemInnerSpacing.x;
	const float iconWidth = width - knobWidth - style.ItemInnerSpacing.x;

	drawList->AddText(origin, th.textMuted, "LFO / S&H");

	const int shape = CurrentShape();
	const bool synced = pLFOSync->value >= 0.5f;
	// a synced pair has one rate to share, so Spin cannot be what is in effect however the
	// parameter was left. the panel shows what the audio is doing, not what was asked for
	const bool spinning = pLFOStereo->value >= 0.5f && !synced;

	// a pair of switches stacked in the icon column, centered against the dial beside them
	const float switchHeight = std::min((blockHeight - kRowGap) * 0.5f, frameHeight);
	auto switchTop = [&](float rowY) {
		return rowY + (blockHeight - switchHeight * 2.0f - kRowGap) * 0.5f;
	};

	// ---- amount and shape ----
	float y = origin.y + headerHeight;
	ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
	pLFOAmount->DrawSized(radius, knobWidth, "Amount");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("How far the LFO sweeps the frequency");

	drawList->AddText(ImVec2(iconX, y), th.text, "Shape");
	ImGui::SetCursorScreenPos(ImVec2(iconX, y + lineHeight + kRowGap));
	ImVec2 shapeMin;
	if (IconToggle("##PhaserShape", false, ImVec2(iconWidth, frameHeight), shapeMin))
		ImGui::OpenPopup("PhaserShapeMenu");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", kShapeNames[shape]);
	{
		const float arrow = 4.0f;
		DrawShapeGlyph(drawList, ImVec2(shapeMin.x + (iconWidth - arrow * 2.0f) * 0.5f, shapeMin.y + frameHeight * 0.5f),
					   iconWidth * 0.45f, frameHeight * 0.45f, shape, th.textMuted);
		const float arrowX = shapeMin.x + iconWidth - arrow - 3.0f;
		const float arrowY = shapeMin.y + frameHeight * 0.5f - 1.0f;
		drawList->AddTriangleFilled(ImVec2(arrowX - arrow, arrowY), ImVec2(arrowX + arrow, arrowY),
									ImVec2(arrowX, arrowY + arrow), th.textMuted);
	}
	if (ImGui::BeginPopup("PhaserShapeMenu")) {
		for (int candidate = 0; candidate < kNumShapes; ++candidate) {
			const ImVec2 rowMin = ImGui::GetCursorScreenPos();
			if (ImGui::Selectable(kShapeNames[candidate], candidate == shape, 0, ImVec2(90.0f, 0.0f)))
				SetChoice(pLFOShape, (float)candidate);
			DrawShapeGlyph(ImGui::GetWindowDrawList(),
						   ImVec2(rowMin.x + 74.0f, rowMin.y + lineHeight * 0.5f), 24.0f, lineHeight * 0.7f,
						   candidate, th.textMuted);
		}
		ImGui::EndPopup();
	}

	// ---- rate, free or synced ----
	y += blockHeight + gap;
	if (synced) {
		// a division names itself, so the dial gives way to the chooser that reads it
		const int division = std::clamp((int)std::lround(pLFODivision->value), 0, kNumDivisions - 1);
		drawList->AddText(ImVec2(origin.x, y), th.text, "Rate");
		ImGui::SetCursorScreenPos(ImVec2(origin.x, y + (blockHeight - frameHeight) * 0.5f));
		ImGui::SetNextItemWidth(knobWidth);
		if (ImGui::BeginCombo("##PhaserDivision", kDivisionNames[division], ImGuiComboFlags_NoArrowButton)) {
			for (int candidate = 0; candidate < kNumDivisions; ++candidate) {
				if (ImGui::Selectable(kDivisionNames[candidate], candidate == division))
					SetChoice(pLFODivision, (float)candidate);
			}
			ImGui::EndCombo();
		}
	} else {
		ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
		pLFORate->DrawSized(radius, knobWidth, "Rate");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Sweeps per second");
	}

	{
		float switchY = switchTop(y);
		ImGui::SetCursorScreenPos(ImVec2(iconX, switchY));
		ImVec2 buttonMin;
		if (IconToggle("##PhaserHz", !synced, ImVec2(iconWidth, switchHeight), buttonMin))
			SetChoice(pLFOSync, 0.0f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Free-running rate in Hertz");
		{
			const ImVec2 textSize = ImGui::CalcTextSize("Hz");
			drawList->AddText(ImVec2(buttonMin.x + (iconWidth - textSize.x) * 0.5f,
									 buttonMin.y + (switchHeight - textSize.y) * 0.5f),
							  synced ? th.textMuted : th.textOnAccent, "Hz");
		}

		switchY += switchHeight + kRowGap;
		ImGui::SetCursorScreenPos(ImVec2(iconX, switchY));
		if (IconToggle("##PhaserSync", synced, ImVec2(iconWidth, switchHeight), buttonMin))
			SetChoice(pLFOSync, 1.0f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Lock the rate to the project tempo");
		DrawNoteGlyph(drawList, ImVec2(buttonMin.x + iconWidth * 0.5f, buttonMin.y + switchHeight * 0.5f),
					  switchHeight, synced ? th.textOnAccent : th.textMuted);
	}

	// ---- the stereo pair ----
	// NOTE: the dial under these two switches is whichever of them is lit, because Phase
	// and Spin are two answers to the same question and only one of them is being asked
	y += blockHeight + gap;
	ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
	if (spinning) {
		pLFOSpin->DrawSized(radius, knobWidth, "Spin");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How far the right channel's rate is detuned from the left one");
	} else {
		pLFOPhase->DrawSized(radius, knobWidth, "Phase");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How far the right channel's LFO trails the left one.\n"
							  "180 puts one at its peak as the other bottoms out");
	}

	{
		float switchY = switchTop(y);
		ImGui::SetCursorScreenPos(ImVec2(iconX, switchY));
		ImVec2 buttonMin;
		if (IconToggle("##PhaserPhase", !spinning, ImVec2(iconWidth, switchHeight), buttonMin))
			SetChoice(pLFOStereo, (float)StereoPhase);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Same rate on both channels, one offset from the other");
		DrawPhiGlyph(drawList, ImVec2(buttonMin.x + iconWidth * 0.5f, buttonMin.y + switchHeight * 0.5f),
					 switchHeight, spinning ? th.textMuted : th.textOnAccent);

		switchY += switchHeight + kRowGap;
		ImGui::SetCursorScreenPos(ImVec2(iconX, switchY));
		// a synced pair is chased off the transport, so there are no two rates to detune
		if (synced)
			ImGui::BeginDisabled();
		if (IconToggle("##PhaserSpin", spinning, ImVec2(iconWidth, switchHeight), buttonMin))
			SetChoice(pLFOStereo, (float)StereoSpin);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(synced ? "Free-running rates only" : "Detune the two channels' rates so they drift apart");
		DrawSpinGlyph(drawList, ImVec2(buttonMin.x + iconWidth * 0.5f, buttonMin.y + switchHeight * 0.5f),
					  switchHeight, spinning ? th.textOnAccent : th.textMuted);
		if (synced)
			ImGui::EndDisabled();
	}

	ImGui::SetCursorScreenPos(origin);
	ImGui::Dummy(ImVec2(width, height));
}
