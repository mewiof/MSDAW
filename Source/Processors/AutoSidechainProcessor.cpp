#include "PrecompHeader.h"
#include "AutoSidechainProcessor.h"
#include "ProcessorFactory.h"
#include "SidechainHub.h"
#include "Project.h"
#include "Track.h"
#include "Theme.h"
#include "Parameters/KnobParameter.h"
#include "Parameters/SliderParameter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

REGISTER_PROCESSOR(AutoSidechainProcessor, "AutoSidechain", false)

namespace {
	const char* kModeNames[] = {"Follow", "Duck", "Free"};
	const int kNumModes = 3;

	// beat divisions for Free mode, longest first
	const char* kRateNames[] = {"1 Bar", "1/2", "1/4", "1/8", "1/8T", "1/16", "1/32"};
	const double kRateBeats[] = {4.0, 2.0, 1.0, 0.5, 1.0 / 3.0, 0.25, 0.125};
	const int kNumRates = 7;

	const float kMinDetector = 1.0e-6f; // -120 dB, the detector's noise floor
	const float kMaxReductionDb = -60.0f;

	float LinearToDb(float linear) {
		return 20.0f * std::log10(std::max(linear, kMinDetector));
	}

	float DbToLinear(float db) {
		return std::pow(10.0f, db / 20.0f);
	}

	// one-pole smoothing coefficient for a given time constant
	float TimeCoeff(float seconds, double sampleRate) {
		if (seconds <= 0.0f)
			return 1.0f;
		float c = 1.0f - std::exp(-1.0f / (seconds * (float)sampleRate));
		return std::clamp(c, 0.0f, 1.0f);
	}

	// one-pole cutoff coefficient
	float CutoffCoeff(float hz, double sampleRate) {
		float c = 1.0f - std::exp(-2.0f * 3.14159265359f * hz / (float)sampleRate);
		return std::clamp(c, 0.0f, 1.0f);
	}

	// the same tension shaping the automation lanes use, so a curve drawn here reads
	// like a curve drawn there. x and the result are both 0..1
	float ShapeCurve(float x, float curve) {
		x = std::clamp(x, 0.0f, 1.0f);
		float c = std::clamp(curve, -0.99f, 0.99f);
		if (std::abs(c) < 0.001f)
			return x;
		float exponent = std::pow(10.0f, std::abs(c));
		if (c > 0.0f)
			return 1.0f - std::pow(1.0f - x, exponent);
		return std::pow(x, exponent);
	}

	Track* FindTrackById(Project* project, uint32_t trackId) {
		if (!project || trackId == 0)
			return nullptr;
		for (auto& track : project->GetTracks()) {
			if (track->GetId() == trackId)
				return track.get();
		}
		return nullptr;
	}

	std::string ToLower(const std::string& in) {
		std::string out = in;
		std::transform(out.begin(), out.end(), out.begin(),
					   [](unsigned char ch) { return (char)std::tolower(ch); });
		return out;
	}
} //namespace

AutoSidechainProcessor::AutoSidechainProcessor() {
	// stepped parameters stay plain sliders: the custom UI draws them as combo boxes,
	// and automating them still lands on a sane value because the dsp rounds
	pMode = AddParameter(std::make_unique<SliderParameter>("Mode", (float)ModeDuck, 0.0f, (float)(kNumModes - 1)));
	pRate = AddParameter(std::make_unique<SliderParameter>("Rate", 2.0f, 0.0f, (float)(kNumRates - 1)));

	pAmount = AddParameter(std::make_unique<KnobParameter>("Amount", 100.0f, 0.0f, 100.0f, ImGuiKnobVariant_Percent));
	pThreshold = AddParameter(std::make_unique<KnobParameter>("Thresh", -24.0f, -60.0f, 0.0f, ImGuiKnobVariant_Decibel));
	pRatio = AddParameter(std::make_unique<KnobParameter>("Ratio", 4.0f, 1.0f, 20.0f, ImGuiKnobVariant_Linear));
	pKnee = AddParameter(std::make_unique<KnobParameter>("Knee", 6.0f, 0.0f, 24.0f, ImGuiKnobVariant_Linear));
	pDepth = AddParameter(std::make_unique<KnobParameter>("Depth", -9.0f, -48.0f, 0.0f, ImGuiKnobVariant_Decibel));
	pAttack = AddParameter(std::make_unique<KnobParameter>("Attack", 5.0f, 0.1f, 200.0f, ImGuiKnobVariant_Milliseconds));
	pHold = AddParameter(std::make_unique<KnobParameter>("Hold", 20.0f, 0.0f, 500.0f, ImGuiKnobVariant_Milliseconds));
	pRelease = AddParameter(std::make_unique<KnobParameter>("Release", 180.0f, 5.0f, 2000.0f, ImGuiKnobVariant_Milliseconds));
	pCurve = AddParameter(std::make_unique<KnobParameter>("Curve", 0.4f, -1.0f, 1.0f, ImGuiKnobVariant_Linear));
	pHighPass = AddParameter(std::make_unique<KnobParameter>("Det HP", 20.0f, 20.0f, 2000.0f, ImGuiKnobVariant_Hertz));
	pLowPass = AddParameter(std::make_unique<KnobParameter>("Det LP", 20000.0f, 200.0f, 20000.0f, ImGuiKnobVariant_Hertz));

	mScope.resize(kScopePoints);
}

AutoSidechainProcessor::~AutoSidechainProcessor() {
	SidechainHub::Instance().Unsubscribe(mSourceTrackId);
}

void AutoSidechainProcessor::SetSourceTrackId(uint32_t trackId) {
	if (trackId == mSourceTrackId)
		return;
	SidechainHub& hub = SidechainHub::Instance();
	hub.Unsubscribe(mSourceTrackId);
	mSourceTrackId = trackId;
	hub.Subscribe(mSourceTrackId);

	// remember the name so a project whose source track was deleted can still say
	// which track it used to duck from
	if (Track* track = FindTrackById(hub.GetProject(), trackId))
		mSourceTrackName = track->GetName();
	else if (trackId == 0)
		mSourceTrackName.clear();
}

void AutoSidechainProcessor::CopyStateFrom(const AudioProcessor& other) {
	const AutoSidechainProcessor* source = dynamic_cast<const AutoSidechainProcessor*>(&other);
	if (!source)
		return;
	mSourceTrackName = source->mSourceTrackName;
	SetSourceTrackId(source->mSourceTrackId);
	mAutoPickAttempted = true; // inherited a source (even "none"), do not second-guess it
}

int AutoSidechainProcessor::CurrentMode() const {
	int mode = (int)std::lround(pMode->value);
	return std::clamp(mode, 0, kNumModes - 1);
}

double AutoSidechainProcessor::FreeCycleBeats() const {
	int index = std::clamp((int)std::lround(pRate->value), 0, kNumRates - 1);
	return kRateBeats[index];
}

double AutoSidechainProcessor::EnvelopeLengthSec() const {
	return (double)(pAttack->value + pHold->value + pRelease->value) * 0.001;
}

float AutoSidechainProcessor::EnvelopeAmount(double tSec) const {
	if (tSec < 0.0)
		return 0.0f;

	const double attack = std::max(0.0, (double)pAttack->value * 0.001);
	const double hold = std::max(0.0, (double)pHold->value * 0.001);
	const double release = std::max(0.0, (double)pRelease->value * 0.001);
	const float curve = pCurve->value;

	if (tSec < attack)
		return attack > 0.0 ? ShapeCurve((float)(tSec / attack), curve) : 1.0f;
	if (tSec < attack + hold)
		return 1.0f;
	if (tSec < attack + hold + release)
		return release > 0.0 ? 1.0f - ShapeCurve((float)((tSec - attack - hold) / release), curve) : 0.0f;
	return 0.0f;
}

void AutoSidechainProcessor::PrepareToPlay(double sampleRate) {
	mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
	// ~240 scope points per second: a little over two seconds of history across the
	// display, fine enough that a 16th-note pump is several pixels wide
	mScopeStride = std::max(1, (int)(mSampleRate / 240.0));
	Reset();
}

void AutoSidechainProcessor::Reset() {
	mDetHpState = 0.0f;
	mDetLpState = 0.0f;
	mDetEnv = 0.0f;
	mGrDb = 0.0f;
	mHoldCounter = 0;
	mTriggerArmed = true;
	mRearmCounter = 0;
	mEnvActive = false;
	mEnvPosSec = 0.0;
	mScopeCounter = 0;
	mScopeDetPeak = 0.0f;
	mScopeGrPeak = 0.0f;
	mVisGrDb.store(0.0f, std::memory_order_relaxed);
	mVisDetDb.store(-90.0f, std::memory_order_relaxed);
	mVisEnvPhase.store(-1.0f, std::memory_order_relaxed);
}

void AutoSidechainProcessor::PushScopePoint(float detDb, float grDb) {
	int index = mScopeWrite.load(std::memory_order_relaxed);
	mScope[index] = ScopePoint{detDb, grDb};
	mScopeWrite.store((index + 1) % kScopePoints, std::memory_order_relaxed);
}

void AutoSidechainProcessor::Process(float* buffer, int numFrames, int numChannels,
									 std::vector<MIDIMessage>& mIDIMessages,
									 const ProcessContext& context) {
	(void)mIDIMessages;

	if (numFrames <= 0 || numChannels <= 0)
		return;

	const int mode = CurrentMode();
	const float* detector = SidechainHub::Instance().Read(mSourceTrackId);

	// free runs off the transport, so it is the one mode that works with no source
	if (!detector && mode != ModeFree) {
		mVisGrDb.store(0.0f, std::memory_order_relaxed);
		mVisDetDb.store(-90.0f, std::memory_order_relaxed);
		mVisEnvPhase.store(-1.0f, std::memory_order_relaxed);
		return;
	}

	const float amount = std::clamp(pAmount->value * 0.01f, 0.0f, 1.0f);
	const float threshold = pThreshold->value;
	const float ratio = std::max(1.0f, pRatio->value);
	const float knee = std::max(0.0f, pKnee->value);
	const float depthDb = std::min(0.0f, pDepth->value);

	const float hpCoeff = CutoffCoeff(pHighPass->value, mSampleRate);
	const float lpCoeff = CutoffCoeff(pLowPass->value, mSampleRate);

	// the level follower stays fast and fixed; the user's attack/release shape the
	// gain, which is what a compressor's controls are actually meant to do
	const float detAttack = TimeCoeff(0.0005f, mSampleRate);
	const float detRelease = TimeCoeff(0.050f, mSampleRate);

	const float gainAttack = TimeCoeff(pAttack->value * 0.001f, mSampleRate);
	const float gainRelease = TimeCoeff(pRelease->value * 0.001f, mSampleRate);
	const int holdSamples = (int)(pHold->value * 0.001f * (float)mSampleRate);

	const double sampleSec = 1.0 / mSampleRate;
	const double envLength = EnvelopeLengthSec();

	// free mode: where the transport sits inside the current cycle
	const double cycleBeats = FreeCycleBeats();
	const double secondsPerBeat = 60.0 / std::max(1.0, context.bpm);
	const double blockStartBeat = (context.currentSample / std::max(1.0, context.sampleRate)) / secondsPerBeat;
	const double beatsPerSample = 1.0 / (secondsPerBeat * mSampleRate);

	// duck mode retrigger guard: a kick's body should not fire the envelope twice
	const int rearmSamples = (int)(0.030 * mSampleRate);
	const float rearmHysteresisDb = 6.0f;

	float lastEnvPhase = -1.0f;

	for (int i = 0; i < numFrames; ++i) {
		float det = detector ? detector[i] : 0.0f;

		// detector filtering: a high pass keeps a bass-heavy source from smearing the
		// trigger, a low pass keeps hats and cymbals out of it
		mDetHpState += hpCoeff * (det - mDetHpState);
		det -= mDetHpState;
		mDetLpState += lpCoeff * (det - mDetLpState);
		det = mDetLpState;

		const float rectified = std::abs(det);
		if (rectified > mDetEnv)
			mDetEnv += detAttack * (rectified - mDetEnv);
		else
			mDetEnv += detRelease * (rectified - mDetEnv);

		const float detDb = LinearToDb(mDetEnv);
		float reductionDb = 0.0f;

		if (mode == ModeFollow) {
			// soft-knee feed-forward gain computer
			float outputDb = detDb;
			const float over = detDb - threshold;
			if (knee > 0.0f && 2.0f * over > -knee && 2.0f * over < knee) {
				const float t = over + knee * 0.5f;
				outputDb = detDb + (1.0f / ratio - 1.0f) * (t * t) / (2.0f * knee);
			} else if (over > 0.0f) {
				outputDb = threshold + over / ratio;
			}
			float targetDb = std::max(outputDb - detDb, kMaxReductionDb);

			if (targetDb < mGrDb) {
				mGrDb += gainAttack * (targetDb - mGrDb);
				mHoldCounter = holdSamples;
			} else if (mHoldCounter > 0) {
				mHoldCounter--;
			} else {
				mGrDb += gainRelease * (targetDb - mGrDb);
			}
			reductionDb = mGrDb;
		} else if (mode == ModeDuck) {
			// the source only decides *when*: every transient past the threshold fires
			// the same hand-drawn envelope, so the pump is identical every bar
			if (mTriggerArmed && detDb > threshold) {
				mEnvActive = true;
				mEnvPosSec = 0.0;
				mTriggerArmed = false;
				mRearmCounter = rearmSamples;
			} else if (!mTriggerArmed) {
				if (mRearmCounter > 0)
					mRearmCounter--;
				else if (detDb < threshold - rearmHysteresisDb)
					mTriggerArmed = true;
			}

			if (mEnvActive) {
				reductionDb = depthDb * EnvelopeAmount(mEnvPosSec);
				lastEnvPhase = envLength > 0.0 ? (float)(mEnvPosSec / envLength) : 0.0f;
				mEnvPosSec += sampleSec;
				if (mEnvPosSec >= envLength) {
					mEnvActive = false;
					mEnvPosSec = 0.0;
				}
			}
			mGrDb = reductionDb;
		} else { // ModeFree
			if (context.isPlaying) {
				const double beat = blockStartBeat + (double)i * beatsPerSample;
				double phase = std::fmod(beat, cycleBeats);
				if (phase < 0.0)
					phase += cycleBeats;
				const double tSec = phase * secondsPerBeat;
				reductionDb = depthDb * EnvelopeAmount(tSec);
				lastEnvPhase = (float)(tSec / std::max(1.0e-6, cycleBeats * secondsPerBeat));
			}
			mGrDb = reductionDb;
		}

		const float appliedDb = reductionDb * amount;
		const float gain = appliedDb < -0.001f ? DbToLinear(appliedDb) : 1.0f;

		if (mListen) {
			// audition the detector itself, post-filter - the fastest way to tell
			// whether the trigger is hearing the kick or the whole drum bus
			for (int c = 0; c < numChannels; ++c)
				buffer[i * numChannels + c] = det;
		} else if (gain != 1.0f) {
			for (int c = 0; c < numChannels; ++c)
				buffer[i * numChannels + c] *= gain;
		}

		// bucket the scope so a transient between two sample points cannot hide
		mScopeDetPeak = std::max(mScopeDetPeak, detDb);
		mScopeGrPeak = std::min(mScopeGrPeak, appliedDb);
		if (++mScopeCounter >= mScopeStride) {
			PushScopePoint(mScopeDetPeak, mScopeGrPeak);
			mScopeCounter = 0;
			mScopeDetPeak = -90.0f;
			mScopeGrPeak = 0.0f;
		}
	}

	mVisGrDb.store(mGrDb * amount, std::memory_order_relaxed);
	mVisDetDb.store(LinearToDb(mDetEnv), std::memory_order_relaxed);
	mVisEnvPhase.store(lastEnvPhase, std::memory_order_relaxed);
}

// ================================================================
// SERIALIZATION
// ================================================================

void AutoSidechainProcessor::Save(std::ostream& out) {
	// written before the inherited PARAMS block: AudioProcessor::Load stops at
	// PARAMS_END and Track::Load eats exactly one line after it (PROCESSOR_END), so
	// there is no room for extra lines on the far side
	out << "SC_SOURCE " << mSourceTrackId << "\n";
	out << "SC_SOURCE_NAME \"" << mSourceTrackName << "\"\n";
	AudioProcessor::Save(out);
}

void AutoSidechainProcessor::Load(std::istream& in) {
	std::string line;
	uint32_t loadedId = 0;

	while (std::getline(in, line)) {
		if (line.rfind("SC_SOURCE_NAME", 0) == 0) {
			size_t q1 = line.find('"');
			size_t q2 = line.find('"', q1 + 1);
			if (q1 != std::string::npos && q2 != std::string::npos)
				mSourceTrackName = line.substr(q1 + 1, q2 - q1 - 1);
		} else if (line.rfind("SC_SOURCE ", 0) == 0) {
			std::stringstream ss(line.substr(10));
			ss >> loadedId;
		} else {
			// anything else is the base class's territory. it tolerates the leading
			// line we just consumed, and reads on to PARAMS_END
			AudioProcessor::Load(in);
			break;
		}
	}

	// the referenced track may not exist yet (Track::Load runs per track, in order),
	// which is fine: the id is resolved lazily and the hub happily holds a slot for a
	// track that has not been read back in yet
	if (loadedId != 0) {
		const std::string savedName = mSourceTrackName;
		SetSourceTrackId(loadedId);
		mSourceTrackName = savedName; // the track it names is still being read back in
	}
	mAutoPickAttempted = true; // a loaded device must never silently repick its source
}

// ================================================================
// UI
// ================================================================

bool AutoSidechainProcessor::RenderCustomUI(const ImVec2& size) {
	const Theme& th = Theme::Instance();
	ImGuiStyle& style = ImGui::GetStyle();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	Project* project = SidechainHub::Instance().GetProject();

	const int mode = CurrentMode();

	// an "auto sidechain" dropped on a track with a kick in the project should just
	// work, so guess the obvious source once, on first sight, and never again
	if (!mAutoPickAttempted) {
		mAutoPickAttempted = true;
		if (mSourceTrackId == 0 && project) {
			for (auto& track : project->GetTracks()) {
				if (ToLower(track->GetName()).find("kick") != std::string::npos) {
					SetSourceTrackId(track->GetId());
					break;
				}
			}
		}
	}

	// ---- header row: source | mode | listen ----
	const float rowHeight = ImGui::GetFrameHeight();
	const float listenWidth = ImGui::CalcTextSize("Listen").x + style.FramePadding.x * 2.0f;
	const float modeWidth = ImGui::CalcTextSize("Follow").x + style.FramePadding.x * 2.0f + rowHeight;
	float sourceWidth = size.x - listenWidth - modeWidth - style.ItemSpacing.x * 2.0f;
	sourceWidth = std::max(sourceWidth, 90.0f);

	Track* sourceTrack = FindTrackById(project, mSourceTrackId);
	std::string sourceLabel;
	if (mSourceTrackId == 0)
		sourceLabel = "No source";
	else if (sourceTrack)
		sourceLabel = sourceTrack->GetName();
	else
		sourceLabel = (mSourceTrackName.empty() ? std::string("Unknown track") : mSourceTrackName) + " (missing)";

	ImGui::SetNextItemWidth(sourceWidth);
	if (ImGui::BeginCombo("##ScSource", sourceLabel.c_str())) {
		if (ImGui::Selectable("No source", mSourceTrackId == 0))
			SetSourceTrackId(0);
		if (project) {
			for (auto& track : project->GetTracks()) {
				ImGui::PushID((int)track->GetId());
				const bool selected = track->GetId() == mSourceTrackId;
				ImVec2 dotPos = ImGui::GetCursorScreenPos();
				ImGui::Dummy(ImVec2(12.0f, 1.0f));
				ImGui::SameLine();
				if (ImGui::Selectable(track->GetName().c_str(), selected))
					SetSourceTrackId(track->GetId());
				ImGui::GetWindowDrawList()->AddRectFilled(
					ImVec2(dotPos.x, dotPos.y + 4.0f),
					ImVec2(dotPos.x + 7.0f, dotPos.y + ImGui::GetTextLineHeight()),
					track->GetColor(), 1.0f);
				if (track->IsGroup()) {
					ImGui::SameLine();
					ImGui::TextDisabled("group");
				}
				ImGui::PopID();
			}
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Track whose signal drives the ducking");

	ImGui::SameLine();
	ImGui::SetNextItemWidth(modeWidth);
	if (ImGui::BeginCombo("##ScMode", kModeNames[mode], ImGuiComboFlags_NoArrowButton)) {
		for (int i = 0; i < kNumModes; ++i) {
			if (ImGui::Selectable(kModeNames[i], i == mode)) {
				const float oldValue = pMode->value;
				pMode->value = (float)i;
				pMode->CommitEditImmediate(oldValue);
				Reset();
			}
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Follow: sidechain compressor, tracks the source's dynamics\n"
						  "Duck: every source transient fires the shaped envelope below\n"
						  "Free: the envelope retriggers on the beat, no source needed");
	}

	ImGui::SameLine();
	if (mListen)
		ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
	if (ImGui::Button("Listen", ImVec2(listenWidth, 0.0f)))
		mListen = !mListen;
	if (mListen)
		ImGui::PopStyleColor();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Monitor the filtered detector signal instead of this track");

	// ---- graph ----
	// a knob row is label + knob + value; reserve it and give ALL the rest to the graph.
	// the device rack is a fixed-height strip, so anything the graph does not take is
	// dead space rather than something another device could use
	const float knobRowHeight = ImGui::GetTextLineHeight() * 2.0f + style.ItemInnerSpacing.y * 3.0f + 36.0f;
	float graphHeight = size.y - rowHeight - knobRowHeight - style.ItemSpacing.y * 2.0f;
	graphHeight = std::max(graphHeight, 40.0f);

	const ImVec2 graphPos = ImGui::GetCursorScreenPos();
	const float graphWidth = size.x;
	const ImVec2 graphEnd(graphPos.x + graphWidth, graphPos.y + graphHeight);
	ImGui::Dummy(ImVec2(graphWidth, graphHeight));
	const ImVec2 belowGraph = ImGui::GetCursorScreenPos();

	dl->AddRectFilled(graphPos, graphEnd, th.bgDeepest);

	// follow has no drawable shape, so it always shows the live scope
	const bool shapeMode = (mode != ModeFollow);
	const bool showScope = !shapeMode || mShowScope;

	if (showScope) {
		// switching view (or mode) mid-drag retires the handles without ever reporting
		// their release, which would leave the shape view's time axis frozen for good
		mDraggingHandle = false;

		// ---- live history: detector level behind, gain reduction in front ----
		const float detTop = -6.0f, detBottom = -60.0f;
		const float grSpan = 30.0f;
		const int write = mScopeWrite.load(std::memory_order_relaxed);

		for (int gridDb = 6; gridDb < (int)grSpan; gridDb += 6) {
			const float y = graphPos.y + (gridDb / grSpan) * graphHeight;
			dl->AddLine(ImVec2(graphPos.x, y), ImVec2(graphEnd.x, y), th.gridSub);
		}

		const float stepX = graphWidth / (float)kScopePoints;
		ImVec2 prevDet, prevGr;
		for (int i = 0; i < kScopePoints; ++i) {
			const ScopePoint& point = mScope[(write + i) % kScopePoints];
			const float x = graphPos.x + i * stepX;

			const float detNorm = std::clamp((point.detDb - detBottom) / (detTop - detBottom), 0.0f, 1.0f);
			const ImVec2 detPos(x, graphEnd.y - detNorm * graphHeight);
			const float grNorm = std::clamp(-point.grDb / grSpan, 0.0f, 1.0f);
			const ImVec2 grPos(x, graphPos.y + grNorm * graphHeight);

			if (i > 0) {
				dl->AddLine(prevDet, detPos, Theme::WithAlpha(th.graphCurveCool, 90), 1.0f);
				dl->AddQuadFilled(prevGr, grPos, ImVec2(grPos.x, graphPos.y), ImVec2(prevGr.x, graphPos.y),
								  Theme::WithAlpha(th.accent, 45));
				dl->AddLine(prevGr, grPos, th.accent, 1.5f);
			}
			prevDet = detPos;
			prevGr = grPos;
		}

		// threshold marker, on the detector's scale
		if (mode != ModeFree) {
			const float threshNorm = std::clamp((pThreshold->value - detBottom) / (detTop - detBottom), 0.0f, 1.0f);
			const float y = graphEnd.y - threshNorm * graphHeight;
			for (float x = graphPos.x; x < graphEnd.x; x += 8.0f)
				dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + 4.0f, graphEnd.x), y), Theme::WithAlpha(th.graphCurveCool, 190));
		}
	} else {
		// ---- envelope shape, drawn and draggable ----
		const float depth = pDepth->value;
		const float dbSpan = depth > -24.0f ? 24.0f : 48.0f;
		const float bpm = project ? (float)project->GetTransport().GetBpm() : 120.0f;

		float axisSec = (mode == ModeFree)
							? (float)(FreeCycleBeats() * 60.0 / std::max(1.0f, bpm))
							: (float)EnvelopeLengthSec();
		if (mDraggingHandle)
			axisSec = mDragAxisSec;
		axisSec = std::max(axisSec, 0.01f);

		const float pxPerSec = graphWidth / axisSec;
		auto timeToX = [&](double seconds) { return graphPos.x + (float)seconds * pxPerSec; };
		auto dbToY = [&](float db) { return graphPos.y + std::clamp(-db / dbSpan, 0.0f, 1.0f) * graphHeight; };

		for (int gridDb = 6; gridDb < (int)dbSpan; gridDb += 6) {
			const float y = dbToY(-(float)gridDb);
			dl->AddLine(ImVec2(graphPos.x, y), ImVec2(graphEnd.x, y), th.gridSub);
		}
		// beat rulings, so a duck can be lined up against the grid by eye
		const float secondsPerBeat = 60.0f / std::max(1.0f, bpm);
		for (float beat = secondsPerBeat; beat < axisSec; beat += secondsPerBeat)
			dl->AddLine(ImVec2(timeToX(beat), graphPos.y), ImVec2(timeToX(beat), graphEnd.y), th.gridBeat);

		const int kSamples = 160;
		ImVec2 prev;
		for (int i = 0; i <= kSamples; ++i) {
			const double t = axisSec * (double)i / (double)kSamples;
			const ImVec2 pos(timeToX(t), dbToY(depth * EnvelopeAmount(t)));
			if (i > 0) {
				dl->AddQuadFilled(prev, pos, ImVec2(pos.x, graphPos.y), ImVec2(prev.x, graphPos.y),
								  Theme::WithAlpha(th.accent, 45));
				dl->AddLine(prev, pos, th.accent, 2.0f);
			}
			prev = pos;
		}

		// where the envelope actually is right now
		const float phase = mVisEnvPhase.load(std::memory_order_relaxed);
		if (phase >= 0.0f) {
			const double t = (mode == ModeFree) ? phase * axisSec : phase * EnvelopeLengthSec();
			const float x = std::clamp(timeToX(t), graphPos.x, graphEnd.x);
			dl->AddLine(ImVec2(x, graphPos.y), ImVec2(x, graphEnd.y), Theme::WithAlpha(th.playhead, 190), 1.5f);
			dl->AddCircleFilled(ImVec2(x, dbToY(depth * EnvelopeAmount(t))), 3.0f, th.playhead);
		}

		// draggable handles. submitted after the graph fill so they win hover, and the
		// time axis is frozen while one is held or the curve would crawl out from
		// under the cursor as the drag rescales it
		auto handle = [&](const char* id, ImVec2 center, Parameter* param, bool vertical, float unitsPerPixel, const char* format) {
			const float radius = 4.5f;
			ImGui::SetCursorScreenPos(ImVec2(center.x - radius * 2.0f, center.y - radius * 2.0f));
			ImGui::InvisibleButton(id, ImVec2(radius * 4.0f, radius * 4.0f));

			const bool active = ImGui::IsItemActive();
			const bool hovered = ImGui::IsItemHovered();
			if (ImGui::IsItemActivated()) {
				param->BeginEditGesture();
				param->Select();
				mDraggingHandle = true;
				mDragAxisSec = axisSec;
			}
			if (active) {
				const float delta = vertical ? ImGui::GetIO().MouseDelta.y : ImGui::GetIO().MouseDelta.x;
				if (delta != 0.0f)
					param->value = std::clamp(param->value + delta * unitsPerPixel, param->minValue, param->maxValue);
				ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
			}
			if (ImGui::IsItemDeactivated()) {
				param->EndEditGesture();
				mDraggingHandle = false;
			}

			dl->AddCircleFilled(center, radius, (active || hovered) ? th.accentHover : th.accent);
			dl->AddCircle(center, radius, th.bgDeepest, 0, 1.5f);
			if (hovered || active)
				ImGui::SetTooltip(format, param->name.c_str(), param->value);
		};

		const double attackSec = pAttack->value * 0.001;
		const double holdSec = pHold->value * 0.001;
		const double releaseSec = pRelease->value * 0.001;
		const float msPerPixel = 1000.0f / pxPerSec;

		handle("##ScAttack", ImVec2(timeToX(attackSec), dbToY(depth)), pAttack, false, msPerPixel, "%s  %.1f ms");
		if (pHold->value > 0.0f || pAttack->value > 0.0f)
			handle("##ScHold", ImVec2(timeToX(attackSec + holdSec), dbToY(depth)), pHold, false, msPerPixel, "%s  %.1f ms");
		handle("##ScRelease", ImVec2(timeToX(attackSec + holdSec + releaseSec), dbToY(0.0f)), pRelease, false, msPerPixel, "%s  %.0f ms");

		// depth rides the flat bottom, curve rides the middle of the recovery
		const double depthHandleT = attackSec + holdSec * 0.5;
		handle("##ScDepth", ImVec2(timeToX(depthHandleT), dbToY(depth)), pDepth, true, -dbSpan / graphHeight, "%s  %.1f dB");
		const double curveT = attackSec + holdSec + releaseSec * 0.5;
		handle("##ScCurve", ImVec2(timeToX(curveT), dbToY(depth * EnvelopeAmount(curveT))), pCurve, true, -0.012f, "%s  %+.2f");
	}

	dl->AddRect(graphPos, graphEnd, th.border);

	// readouts + the scope/shape switch, drawn over the graph's corners
	char readout[48];
	snprintf(readout, sizeof(readout), "GR %.1f dB", mVisGrDb.load(std::memory_order_relaxed));
	dl->AddText(ImVec2(graphPos.x + 6.0f, graphPos.y + 4.0f), th.textMuted, readout);

	if (mSourceTrackId == 0 && mode != ModeFree) {
		dl->AddText(ImVec2(graphPos.x + 6.0f, graphEnd.y - ImGui::GetTextLineHeight() - 4.0f), th.textDim,
					"Pick a source track above");
	} else if (mSourceTrackId != 0 && !sourceTrack) {
		dl->AddText(ImVec2(graphPos.x + 6.0f, graphEnd.y - ImGui::GetTextLineHeight() - 4.0f), th.danger,
					"Source track no longer exists");
	}

	if (shapeMode) {
		const char* toggleLabel = mShowScope ? "Shape" : "Scope";
		const ImVec2 toggleSize = ImGui::CalcTextSize(toggleLabel);
		ImGui::SetCursorScreenPos(ImVec2(graphEnd.x - toggleSize.x - style.FramePadding.x * 2.0f - 4.0f, graphPos.y + 3.0f));
		if (ImGui::SmallButton(toggleLabel))
			mShowScope = !mShowScope;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(mShowScope ? "Edit the ducking shape" : "Watch the live reduction");
	}

	ImGui::SetCursorScreenPos(belowGraph);

	// ---- knob row: only what the current mode actually uses ----
	auto drawKnob = [&](Parameter* param, const char* tip) {
		param->Draw();
		if (ImGui::IsItemHovered() && tip)
			ImGui::SetTooltip("%s", tip);
		ImGui::SameLine();
	};

	if (mode == ModeFollow) {
		drawKnob(pThreshold, "Level the source has to pass before this track is pushed down");
		drawKnob(pRatio, "How hard the reduction bites past the threshold");
		drawKnob(pAttack, "How fast the reduction clamps down");
		drawKnob(pHold, "Time held at full reduction before releasing");
		drawKnob(pRelease, "How fast the level comes back up");
		drawKnob(pAmount, "Blend of the computed reduction, 0% is bypass");
	} else if (mode == ModeDuck) {
		drawKnob(pThreshold, "Source level that fires the envelope");
		drawKnob(pDepth, "How far down each duck pushes this track");
		drawKnob(pAttack, "Time to reach full reduction");
		drawKnob(pRelease, "Time to recover");
		drawKnob(pCurve, "Shape of the drop and recovery");
		drawKnob(pAmount, "Blend of the computed reduction, 0% is bypass");
	} else {
		// Free: the rate combo takes the slot the threshold would have used
		ImGui::BeginGroup();
		ImGui::TextUnformatted("Rate");
		const int rateIndex = std::clamp((int)std::lround(pRate->value), 0, kNumRates - 1);
		ImGui::SetNextItemWidth(ImGui::CalcTextSize("1 Bar").x + style.FramePadding.x * 2.0f + rowHeight);
		if (ImGui::BeginCombo("##ScRate", kRateNames[rateIndex], ImGuiComboFlags_NoArrowButton)) {
			for (int i = 0; i < kNumRates; ++i) {
				if (ImGui::Selectable(kRateNames[i], i == rateIndex)) {
					const float oldValue = pRate->value;
					pRate->value = (float)i;
					pRate->CommitEditImmediate(oldValue);
				}
			}
			ImGui::EndCombo();
		}
		ImGui::EndGroup();
		ImGui::SameLine();
		drawKnob(pDepth, "How far down each cycle pushes this track");
		drawKnob(pAttack, "Time to reach full reduction");
		drawKnob(pRelease, "Time to recover");
		drawKnob(pCurve, "Shape of the drop and recovery");
		drawKnob(pAmount, "Blend of the computed reduction, 0% is bypass");
	}

	// everything the mode's knob row left out
	if (ImGui::SmallButton("..."))
		ImGui::OpenPopup("ScMore");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Detector filters and the rest of this mode's controls");
	if (ImGui::BeginPopup("ScMore")) {
		ImGui::TextDisabled("Detector");
		pHighPass->Draw();
		ImGui::SameLine();
		pLowPass->Draw();
		ImGui::Separator();
		if (mode == ModeFollow) {
			ImGui::TextDisabled("Curve");
			pKnee->Draw();
		} else {
			ImGui::TextDisabled("Timing");
			pHold->Draw();
		}
		ImGui::EndPopup();
	}

	return true;
}
