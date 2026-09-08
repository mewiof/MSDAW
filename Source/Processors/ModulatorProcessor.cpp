#include "PrecompHeader.h"
#include "ModulatorProcessor.h"

#include "Parameters/KnobParameter.h"
#include "Parameters/SliderParameter.h"
#include "Parameters/ToggleParameter.h"
#include "ProcessorFactory.h"
#include "ProcessorHost.h"
#include "Processors/RackProcessor.h"
#include "Project.h"
#include "SidechainHub.h"
#include "Theme.h"
#include "Track.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <sstream>

REGISTER_PROCESSOR(ModulatorProcessor, "Modulator", false)

namespace {

	// ================================================================
	// TABLES
	// ================================================================

	// the device's header reads as the generator that is running, the way the reference
	// product labels the slot. the serialization id stays "Modulator" whatever it says
	const char* kModeNames[] = {"LFO", "Performer", "Stepper"};
	const char* kShapeNames[] = {"Sine", "Triangle", "Saw Down", "Saw Up", "Square", "Random"};
	const char* kCurveNames[] = {"Silent", "Hold", "Ramp Up", "Ramp Down", "Exp Rise", "Exp Fall",
								 "Triangle", "Hump", "Pulse", "Double", "Stair Up", "Stair Down"};

	// beat divisions for the synced rate, longest first
	const char* kDivisionNames[] = {"8 Bars", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/32"};
	const double kDivisionBeats[] = {32.0, 16.0, 8.0, 4.0, 2.0, 1.0, 2.0 / 3.0, 0.5, 1.0 / 3.0, 0.25, 0.125};
	constexpr int kNumDivisions = 11;
	constexpr int kDefaultDivision = 5; // 1/4

	constexpr float kTwoPi = 6.28318530718f;
	constexpr float kPi = 3.14159265359f;

	// ================================================================
	// GENERATORS
	// ================================================================

	// every generator reads 0..1, so one blend, one depth and one mapping range serve
	// all three. bipolar modulation is expressed by inverting a target's min/max
	float EvaluateShape(int shape, float phase) {
		phase -= std::floor(phase);
		switch (shape) {
		case ModulatorProcessor::ShapeTriangle:
			return phase < 0.5f ? phase * 2.0f : 2.0f - phase * 2.0f;
		case ModulatorProcessor::ShapeSawDown:
			return 1.0f - phase;
		case ModulatorProcessor::ShapeSawUp:
			return phase;
		case ModulatorProcessor::ShapeSquare:
			return phase < 0.5f ? 1.0f : 0.0f;
		case ModulatorProcessor::ShapeSine:
		default:
			return 0.5f - 0.5f * std::cos(phase * kTwoPi);
		}
	}

	float EvaluateCurve(int curve, float t) {
		t = std::clamp(t, 0.0f, 1.0f);
		switch (curve) {
		case ModulatorProcessor::CurveHold:
			return 1.0f;
		case ModulatorProcessor::CurveRampUp:
			return t;
		case ModulatorProcessor::CurveRampDown:
			return 1.0f - t;
		case ModulatorProcessor::CurveExpRise:
			return t * t;
		case ModulatorProcessor::CurveExpFall:
			return (1.0f - t) * (1.0f - t);
		case ModulatorProcessor::CurveTriangle:
			return t < 0.5f ? t * 2.0f : 2.0f - t * 2.0f;
		case ModulatorProcessor::CurveHump:
			return std::sin(t * kPi);
		case ModulatorProcessor::CurvePulse:
			return t < 0.5f ? 1.0f : 0.0f;
		case ModulatorProcessor::CurveDoublePulse:
			return (t < 0.25f || (t >= 0.5f && t < 0.75f)) ? 1.0f : 0.0f;
		case ModulatorProcessor::CurveStairUp:
			return std::min(std::floor(t * 4.0f), 3.0f) / 3.0f;
		case ModulatorProcessor::CurveStairDown:
			return 1.0f - std::min(std::floor(t * 4.0f), 3.0f) / 3.0f;
		case ModulatorProcessor::CurveSilent:
		default:
			return 0.0f;
		}
	}

	// the randomizer runs on the ui thread. the generator's own sample-and-hold keeps
	// its own state, so a button press can never nudge what is currently sounding
	float RandomUnit() {
		static uint32_t state = 0x2545f491u;
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		return (float)(state & 0xFFFFFFu) / (float)0xFFFFFF;
	}

	// a stand-in shape for drawing a Random lane: the generator's held values are not
	// known ahead of time, so the display shows a fixed stepped pattern that reads as
	// "one new value per step" without pretending to predict them
	float PreviewRandom(int step) {
		uint32_t h = (uint32_t)step * 2654435761u;
		h ^= h >> 15;
		return (float)(h & 0xFFFFu) / (float)0xFFFF;
	}

	// ================================================================
	// PROJECT ADDRESSING
	// ================================================================

	// every device in this list, nested racks included, in the order they are saved in.
	// the callback stops the walk by returning true
	bool VisitDevices(const std::vector<std::shared_ptr<AudioProcessor>>& devices,
					  const std::function<bool(const std::shared_ptr<AudioProcessor>&)>& visit) {
		for (const auto& device : devices) {
			if (!device)
				continue;
			if (visit(device))
				return true;
			if (auto* rack = dynamic_cast<RackProcessor*>(device.get())) {
				for (const auto& chain : rack->GetChains()) {
					if (chain && VisitDevices(chain->GetProcessors(), visit))
						return true;
				}
			}
		}
		return false;
	}

	// dotted indices into a track's chain: "3" is the fourth device on the track, and
	// "3.0.1" the second device of the first chain of the rack in that slot - a device
	// index followed by a (chain, device) hop per rack the path descends into. the file
	// format has no per-device ids, so this is the only stable way to name one
	std::string PathOfDevice(const std::vector<std::shared_ptr<AudioProcessor>>& devices,
							 const std::shared_ptr<AudioProcessor>& target) {
		for (size_t d = 0; d < devices.size(); ++d) {
			if (devices[d] == target)
				return std::to_string(d);
			if (auto* rack = dynamic_cast<RackProcessor*>(devices[d].get())) {
				auto& chains = rack->GetChains();
				for (size_t c = 0; c < chains.size(); ++c) {
					if (!chains[c])
						continue;
					const std::string deeper = PathOfDevice(chains[c]->GetProcessors(), target);
					if (!deeper.empty())
						return std::to_string(d) + "." + std::to_string(c) + "." + deeper;
				}
			}
		}
		return "";
	}

	std::shared_ptr<AudioProcessor> DeviceAtPath(const std::vector<std::shared_ptr<AudioProcessor>>& devices,
												 const std::string& path) {
		std::vector<int> indices;
		std::stringstream ss(path);
		std::string part;
		while (std::getline(ss, part, '.')) {
			if (part.empty())
				return nullptr;
			indices.push_back(std::atoi(part.c_str()));
		}
		// a device index, then a (chain, device) pair for every rack below it
		if (indices.empty() || indices.size() % 2 == 0)
			return nullptr;

		const std::vector<std::shared_ptr<AudioProcessor>>* list = &devices;
		std::shared_ptr<AudioProcessor> device = nullptr;
		for (size_t i = 0; i < indices.size();) {
			const int deviceIndex = indices[i++];
			if (!list || deviceIndex < 0 || deviceIndex >= (int)list->size())
				return nullptr;
			device = (*list)[deviceIndex];
			if (i >= indices.size())
				break;

			auto* rack = dynamic_cast<RackProcessor*>(device.get());
			const int chainIndex = indices[i++];
			if (!rack || chainIndex < 0 || chainIndex >= (int)rack->GetChains().size())
				return nullptr;
			list = &rack->GetChains()[chainIndex]->GetProcessors();
		}
		return device;
	}

	// the master track carries devices and a fader like any other, so it is a legitimate
	// modulation destination and belongs in every lookup
	std::shared_ptr<Track> FindTrack(Project* project, uint32_t trackId) {
		if (!project || trackId == 0)
			return nullptr;
		for (auto& track : project->GetTracks()) {
			if (track && track->GetId() == trackId)
				return track;
		}
		auto master = project->GetMasterTrack();
		if (master && master->GetId() == trackId)
			return master;
		return nullptr;
	}

	// the fader (and, on the master, the tempo) belong to the track rather than to any
	// device on it, so they are addressed by an empty device path
	Parameter* TrackParameterNamed(const std::shared_ptr<Track>& track, const std::string& name) {
		if (!track)
			return nullptr;
		Parameter* own[] = {track->GetVolumeParameter(), track->GetPanParameter(), track->GetBpmParameter()};
		for (Parameter* parameter : own) {
			if (parameter && parameter->name == name)
				return parameter;
		}
		return nullptr;
	}

	bool IsTrackParameter(const std::shared_ptr<Track>& track, const Parameter* parameter) {
		if (!track || !parameter)
			return false;
		return track->GetVolumeParameter() == parameter || track->GetPanParameter() == parameter ||
			   track->GetBpmParameter() == parameter;
	}

	// ================================================================
	// UI HELPERS
	// ================================================================

	bool ToggleButton(const char* label, bool active, const ImVec2& size = ImVec2(0, 0)) {
		const Theme& th = Theme::Instance();
		if (active) {
			ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.accent);
			ImGui::PushStyleColor(ImGuiCol_Text, th.textOnAccent);
		}
		const bool pressed = ImGui::Button(label, size);
		if (active)
			ImGui::PopStyleColor(3);
		return pressed;
	}

	// where a curve sits inside its cell at `t`. the inset is per axis because a palette
	// swatch wants breathing room on both, while a step in the lane must span its cell
	// edge to edge - a horizontal gap there is a gap in the envelope being drawn
	ImVec2 CurvePoint(const ImVec2& min, const ImVec2& size, int curve, float t, float scale, const ImVec2& inset) {
		const float v = EvaluateCurve(curve, t) * scale;
		return ImVec2(min.x + inset.x + t * std::max(size.x - inset.x * 2.0f, 1.0f),
					  min.y + size.y - inset.y - v * std::max(size.y - inset.y * 2.0f, 1.0f));
	}

	// `scale` is the step's own height: a Performer step is a shape AND a level, so the
	// cell has to draw what the step actually contributes rather than the bare shape
	void DrawCurveShape(ImDrawList* dl, const ImVec2& min, const ImVec2& size, int curve, ImU32 color,
						float thickness, float scale = 1.0f, const ImVec2& inset = ImVec2(2.0f, 2.0f)) {
		constexpr int kSegments = 16;
		ImVec2 points[kSegments + 1];
		for (int i = 0; i <= kSegments; ++i)
			points[i] = CurvePoint(min, size, curve, (float)i / (float)kSegments, scale, inset);
		dl->AddPolyline(points, kSegments + 1, color, ImDrawFlags_None, thickness);
	}

} // namespace

// ================================================================
// CONSTRUCTION
// ================================================================

ModulatorProcessor::ModulatorProcessor() {
	// stepped parameters stay plain sliders: the custom UI draws them as combo boxes,
	// and automating one still lands on a sane value because the generator rounds
	pMode = AddParameter(std::make_unique<SliderParameter>("Mode", (float)ModeLFO, 0.0f, (float)(kNumModes - 1)));
	pDivision = AddParameter(std::make_unique<SliderParameter>("Division", (float)kDefaultDivision, 0.0f, (float)(kNumDivisions - 1)));
	pSteps = AddParameter(std::make_unique<SliderParameter>("Steps", (float)kMaxSteps, 1.0f, (float)kMaxSteps));
	pShapeA = AddParameter(std::make_unique<SliderParameter>("Shape A", (float)ShapeSine, 0.0f, (float)(kNumShapes - 1)));
	pShapeB = AddParameter(std::make_unique<SliderParameter>("Shape B", (float)ShapeSawDown, 0.0f, (float)(kNumShapes - 1)));

	pSync = AddParameter(std::make_unique<ToggleParameter>("Sync", 1.0f, 0.0f, 1.0f));
	pRestart = AddParameter(std::make_unique<ToggleParameter>("Restart", 1.0f, 0.0f, 1.0f));

	pRate = AddParameter(std::make_unique<KnobParameter>("Rate", 1.0f, 0.01f, 40.0f, ImGuiKnobVariant_Hertz));
	pXFade = AddParameter(std::make_unique<KnobParameter>("XFade", 0.0f, 0.0f, 100.0f, ImGuiKnobVariant_Percent));
	pDepth = AddParameter(std::make_unique<KnobParameter>("Depth", 100.0f, 0.0f, 100.0f, ImGuiKnobVariant_Percent));
	pOffset = AddParameter(std::make_unique<KnobParameter>("Offset", 0.0f, 0.0f, 100.0f, ImGuiKnobVariant_Percent));
	pSmooth = AddParameter(std::make_unique<KnobParameter>("Glide", 0.0f, 0.0f, 1000.0f, ImGuiKnobVariant_Milliseconds));
	pPhase = AddParameter(std::make_unique<KnobParameter>("Phase", 0.0f, 0.0f, 360.0f, ImGuiKnobVariant_Linear));
	pAttack = AddParameter(std::make_unique<KnobParameter>("Attack", 0.0f, 0.0f, 2000.0f, ImGuiKnobVariant_Milliseconds));
	pDecay = AddParameter(std::make_unique<KnobParameter>("Decay", 500.0f, 1.0f, 5000.0f, ImGuiKnobVariant_Milliseconds));
	pEnvAmount = AddParameter(std::make_unique<KnobParameter>("Env", 0.0f, 0.0f, 100.0f, ImGuiKnobVariant_Percent));

	// two lanes that already differ, so the crossfader means something the moment the
	// device is dropped instead of blending a pattern into a copy of itself. the
	// Performer's lanes differ by shape and so start at full height: a fresh one should
	// read as sixteen whole curves, not as a fade-in somebody has to flatten first
	for (int step = 0; step < kMaxSteps; ++step) {
		const float t = (float)step / (float)(kMaxSteps - 1);
		mLevels[0][step] = t;
		mLevels[1][step] = 1.0f - t;
		mHeights[0][step] = 1.0f;
		mHeights[1][step] = 1.0f;
		mCurves[0][step] = CurveRampDown;
		mCurves[1][step] = CurveHump;
	}
}

ModulatorProcessor::~ModulatorProcessor() {}

const char* ModulatorProcessor::GetName() const {
	return kModeNames[CurrentMode()];
}

int ModulatorProcessor::CurrentMode() const {
	return std::clamp((int)std::lround(pMode->value), 0, kNumModes - 1);
}

int ModulatorProcessor::StepCount() const {
	return std::clamp((int)std::lround(pSteps->value), 1, kMaxSteps);
}

void ModulatorProcessor::PrepareToPlay(double sampleRate) {
	mSampleRate = sampleRate;
	Reset();
}

void ModulatorProcessor::Reset() {
	mFreePhase = 0.0;
	mAnchorSample = 0;
	mAnchored = false;
	mEnvPosSec = -1.0;
	mSmoothed = 0.0f;
	mSmoothPrimed = false;
	for (int lane = 0; lane < kLanes; ++lane) {
		mRandomValue[lane] = 0.0f;
		mRandomStep[lane] = -1;
	}
}

// ================================================================
// GENERATOR
// ================================================================

double ModulatorProcessor::CycleSeconds(const ProcessContext& context) const {
	if (pSync->value >= 0.5f) {
		const int index = std::clamp((int)std::lround(pDivision->value), 0, kNumDivisions - 1);
		return kDivisionBeats[index] * 60.0 / std::max(1.0, context.bpm);
	}
	return 1.0 / (double)std::max(0.01f, pRate->value);
}

float ModulatorProcessor::NextRandom() {
	mRandomState ^= mRandomState << 13;
	mRandomState ^= mRandomState >> 17;
	mRandomState ^= mRandomState << 5;
	return (float)(mRandomState & 0xFFFFFFu) / (float)0xFFFFFF;
}

float ModulatorProcessor::LaneValue(int lane, float phase) {
	phase -= std::floor(phase);
	const int steps = StepCount();
	const int step = std::clamp((int)(phase * (float)steps), 0, steps - 1);

	switch (CurrentMode()) {
	case ModePerformer:
		// a step is a shape and a height, the way the reference product's performer is:
		// the same curve at half height is half the modulation, not a different curve
		return EvaluateCurve(mCurves[lane][step], phase * (float)steps - (float)step) *
			   std::clamp(mHeights[lane][step], 0.0f, 1.0f);
	case ModeStepper:
		return std::clamp(mLevels[lane][step], 0.0f, 1.0f);
	case ModeLFO:
	default: {
		const Parameter* shapeParam = (lane == 0) ? pShapeA : pShapeB;
		const int shape = std::clamp((int)std::lround(shapeParam->value), 0, kNumShapes - 1);
		if (shape != ShapeRandom)
			return EvaluateShape(shape, phase);

		// sample and hold: one fresh value per step, so Steps means the same thing here
		// as it does in the two stepped modes
		if (step != mRandomStep[lane]) {
			mRandomStep[lane] = step;
			mRandomValue[lane] = NextRandom();
		}
		return mRandomValue[lane];
	}
	}
}

float ModulatorProcessor::EnvelopeValue() const {
	if (mEnvPosSec < 0.0)
		return 0.0f;

	const double attack = (double)pAttack->value * 0.001;
	if (mEnvPosSec < attack)
		return attack > 0.0 ? (float)(mEnvPosSec / attack) : 1.0f;

	const double decay = std::max((double)pDecay->value * 0.001, 1.0e-6);
	const double t = (mEnvPosSec - attack) / decay;
	if (t >= 1.0)
		return 0.0f;
	// squared rather than linear, so the tail reads as a decay instead of a ramp
	return (float)((1.0 - t) * (1.0 - t));
}

void ModulatorProcessor::Retrigger(int64_t currentSample) {
	mAnchorSample = currentSample;
	mAnchored = true;
	mFreePhase = 0.0;
	mEnvPosSec = 0.0;
	for (int lane = 0; lane < kLanes; ++lane)
		mRandomStep[lane] = -1;
}

void ModulatorProcessor::Process(float* buffer, int numFrames, int numChannels,
								 std::vector<MIDIMessage>& mIDIMessages,
								 const ProcessContext& context) {
	// NOTE: the buffer is deliberately never read or written. a modulator is in the
	// chain for what it does to other devices' parameters, not to the audio
	(void)buffer;
	(void)numChannels;

	if (numFrames <= 0)
		return;

	RefreshBindings();

	const double sampleRate = std::max(1.0, context.sampleRate);
	const double blockSeconds = (double)numFrames / sampleRate;
	const bool restart = pRestart->value >= 0.5f;

	if (restart) {
		// a seek, a fresh start, or the first block this device ever sees puts the cycle
		// back to its beginning. a note-on does too, so a modulator sitting ahead of an
		// instrument can behave like one of its envelopes
		if (context.playheadJumped || !mAnchored)
			Retrigger(context.currentSample);
		for (const auto& message : mIDIMessages) {
			if ((message.status & 0xF0) == 0x90 && message.data2 > 0) {
				Retrigger(context.currentSample);
				break;
			}
		}
	}

	const double cycleSeconds = std::max(CycleSeconds(context), 1.0e-4);

	double phase;
	if (pSync->value >= 0.5f && context.isPlaying) {
		// chased off the transport rather than accumulated, so a seek lands on the right
		// point of the cycle and an offline render matches what was heard
		const double elapsed = (double)(context.currentSample - (restart ? mAnchorSample : 0)) / sampleRate;
		phase = elapsed / cycleSeconds;
		phase -= std::floor(phase);
		mFreePhase = phase; // so stopping the transport continues from where it stopped
	} else {
		phase = mFreePhase;
	}

	// the free accumulator runs whether or not the transport does, so a knob wired to a
	// modulator still moves while the user is dialing it in
	mFreePhase += blockSeconds / cycleSeconds;
	mFreePhase -= std::floor(mFreePhase);

	phase += (double)pPhase->value / 360.0;
	phase -= std::floor(phase);

	const float laneA = LaneValue(0, (float)phase);
	const float laneB = LaneValue(1, (float)phase);
	const float blend = std::clamp(pXFade->value / 100.0f, 0.0f, 1.0f);
	const float raw = laneA + (laneB - laneA) * blend;

	if (mEnvPosSec >= 0.0)
		mEnvPosSec += blockSeconds;
	const float envAmount = std::clamp(pEnvAmount->value / 100.0f, 0.0f, 1.0f);
	const float envScale = 1.0f - envAmount + envAmount * EnvelopeValue();

	const float depth = std::clamp(pDepth->value / 100.0f, 0.0f, 1.0f);
	const float offset = std::clamp(pOffset->value / 100.0f, 0.0f, 1.0f);
	const float wanted = std::clamp(offset + depth * envScale * raw, 0.0f, 1.0f);

	// NOTE: the slew runs once per block, like everything else here, so a glide shorter
	// than a block (a few ms) rounds to no glide at all
	const float glideSeconds = pSmooth->value * 0.001f;
	if (!mSmoothPrimed || glideSeconds <= 0.0f) {
		mSmoothed = wanted;
		mSmoothPrimed = true;
	} else {
		const float alpha = std::clamp(1.0f - std::exp(-(float)blockSeconds / glideSeconds), 0.0f, 1.0f);
		mSmoothed += (wanted - mSmoothed) * alpha;
	}

	mVisValue = mSmoothed;
	mVisPhase = (float)phase;

	ApplyTargets(mSmoothed);
}

// ================================================================
// TARGETS
// ================================================================

void ModulatorProcessor::RefreshBindings() {
	const uint32_t generation = ProcessorHost::ChainGeneration();
	if (generation == mBoundGeneration)
		return;
	mBoundGeneration = generation;

	Project* project = SidechainHub::Instance().GetProject();

	for (auto& target : mTargets) {
		target.resolved = nullptr;

		auto track = FindTrack(project, target.trackId);

		// a saved address only becomes a live binding once the track it names has been
		// read back in, which is why the first refresh after a load does the resolving
		if (target.pending) {
			if (!project)
				continue; // nothing to resolve against yet; try again next time
			target.pending = false;
			if (!track)
				continue;
			target.track = track;
			target.trackName = track->GetName();
			if (!target.pendingPath.empty()) {
				auto device = DeviceAtPath(track->GetProcessors(), target.pendingPath);
				target.device = device;
				if (device)
					target.deviceName = device->GetName();
			}
		}

		// a track or a device carried off by an undo is still alive because the history
		// holds it, and must stop being driven until it comes back
		if (!track || track != target.track.lock())
			continue;

		auto device = target.device.lock();
		if (!device) {
			target.resolved = TrackParameterNamed(track, target.paramName);
			continue;
		}

		const bool onTrack = VisitDevices(track->GetProcessors(),
										  [&](const std::shared_ptr<AudioProcessor>& candidate) { return candidate == device; });
		if (!onTrack)
			continue;

		for (const auto& parameter : device->GetParameters()) {
			if (parameter->name == target.paramName) {
				target.resolved = parameter.get();
				break;
			}
		}
	}
}

void ModulatorProcessor::ApplyTargets(float value) {
	for (const auto& target : mTargets) {
		if (!target.enabled || !target.resolved)
			continue;
		// min > max is a deliberate inversion, so the span is signed
		const float mapped = target.minValue + value * (target.maxValue - target.minValue);
		const float low = std::min(target.resolved->minValue, target.resolved->maxValue);
		const float high = std::max(target.resolved->minValue, target.resolved->maxValue);
		target.resolved->value = std::clamp(mapped, low, high);
	}
}

bool ModulatorProcessor::AddTarget(Project* project, const Parameter* parameter) {
	if (!project || !parameter)
		return false;

	std::shared_ptr<Track> ownerTrack;
	std::shared_ptr<AudioProcessor> ownerDevice;

	auto search = [&](const std::shared_ptr<Track>& track) {
		if (!track)
			return false;
		if (IsTrackParameter(track, parameter)) {
			ownerTrack = track;
			return true;
		}
		std::shared_ptr<AudioProcessor> found;
		VisitDevices(track->GetProcessors(), [&](const std::shared_ptr<AudioProcessor>& device) {
			for (const auto& candidate : device->GetParameters()) {
				if (candidate.get() == parameter) {
					found = device;
					return true;
				}
			}
			return false;
		});
		if (!found)
			return false;
		ownerTrack = track;
		ownerDevice = found;
		return true;
	};

	for (auto& track : project->GetTracks()) {
		if (search(track))
			break;
	}
	if (!ownerTrack)
		search(project->GetMasterTrack());
	if (!ownerTrack)
		return false;

	// driving its own controls is a loop with no useful reading, and would make the
	// modulator's own knobs impossible to turn
	if (ownerDevice.get() == this)
		return false;

	const uint32_t trackId = ownerTrack->GetId();
	for (auto& existing : mTargets) {
		if (existing.trackId == trackId && existing.paramName == parameter->name &&
			existing.device.lock() == ownerDevice) {
			// re-mapping the same parameter reopens it at its full range rather than
			// stacking a second entry that fights the first
			existing.minValue = parameter->minValue;
			existing.maxValue = parameter->maxValue;
			existing.enabled = true;
			mBoundGeneration = 0;
			return true;
		}
	}

	Target target;
	target.trackId = trackId;
	target.track = ownerTrack;
	target.device = ownerDevice;
	target.paramName = parameter->name;
	target.trackName = ownerTrack->GetName();
	target.deviceName = ownerDevice ? ownerDevice->GetName() : ownerTrack->GetName();
	target.minValue = parameter->minValue;
	target.maxValue = parameter->maxValue;
	mTargets.push_back(std::move(target));

	mBoundGeneration = 0; // force a re-resolve on the next block
	return true;
}

void ModulatorProcessor::RemoveTarget(int index) {
	if (index < 0 || index >= (int)mTargets.size())
		return;
	mTargets.erase(mTargets.begin() + index);
	mBoundGeneration = 0;
}

bool ModulatorProcessor::IsParameterTargeted(const Parameter* parameter) {
	if (!parameter)
		return false;
	RefreshBindings();
	for (const auto& target : mTargets) {
		if (target.enabled && target.resolved == parameter)
			return true;
	}
	return false;
}

// ================================================================
// UNDO SUPPORT
// ================================================================

ModulatorProcessor::State ModulatorProcessor::CaptureState() const {
	State state;
	for (int lane = 0; lane < kLanes; ++lane) {
		for (int step = 0; step < kMaxSteps; ++step) {
			state.levels[lane][step] = mLevels[lane][step];
			state.heights[lane][step] = mHeights[lane][step];
			state.curves[lane][step] = mCurves[lane][step];
		}
	}
	state.targets = mTargets;
	return state;
}

void ModulatorProcessor::ApplyState(const State& state) {
	for (int lane = 0; lane < kLanes; ++lane) {
		for (int step = 0; step < kMaxSteps; ++step) {
			mLevels[lane][step] = state.levels[lane][step];
			mHeights[lane][step] = state.heights[lane][step];
			mCurves[lane][step] = state.curves[lane][step];
		}
	}
	mTargets = state.targets;
	for (auto& target : mTargets)
		target.resolved = nullptr;
	mBoundGeneration = 0;
}

void ModulatorProcessor::BeginPatternEdit() {
	if (mPatternEditing)
		return;
	mPatternBefore = CaptureState();
	mPatternEditing = true;
}

void ModulatorProcessor::EndPatternEdit(const char* name) {
	if (!mPatternEditing)
		return;
	mPatternEditing = false;

	State after = CaptureState();
	// a click that repaints the step it landed on changes nothing, and an undo entry
	// that restores what is already there is noise in the history
	bool changed = false;
	for (int lane = 0; lane < kLanes && !changed; ++lane) {
		for (int step = 0; step < kMaxSteps; ++step) {
			if (mPatternBefore.levels[lane][step] != after.levels[lane][step] ||
				mPatternBefore.heights[lane][step] != after.heights[lane][step] ||
				mPatternBefore.curves[lane][step] != after.curves[lane][step]) {
				changed = true;
				break;
			}
		}
	}
	if (!changed)
		return;

	mPatternAfter = std::move(after);
	mPatternEditName = name;
	mPatternEditReady = true;
}

bool ModulatorProcessor::TakePatternEdit(State& before, State& after, std::string& name) {
	if (!mPatternEditReady)
		return false;
	before = mPatternBefore;
	after = mPatternAfter;
	name = mPatternEditName;
	mPatternEditReady = false;
	return true;
}

void ModulatorProcessor::CopyStateFrom(const AudioProcessor& other) {
	const auto* source = dynamic_cast<const ModulatorProcessor*>(&other);
	if (!source)
		return;

	for (int lane = 0; lane < kLanes; ++lane) {
		for (int step = 0; step < kMaxSteps; ++step) {
			mLevels[lane][step] = source->mLevels[lane][step];
			mHeights[lane][step] = source->mHeights[lane][step];
			mCurves[lane][step] = source->mCurves[lane][step];
		}
	}

	// a duplicated modulator drives the same things the original did, exactly the way a
	// duplicated Auto Sidechain keeps pointing at the same source track
	mTargets = source->mTargets;
	for (auto& target : mTargets)
		target.resolved = nullptr;

	mBrush = source->mBrush;
	mSnapToGrid = source->mSnapToGrid;
	mBoundGeneration = 0;
}

// ================================================================
// SERIALIZATION
// ================================================================

void ModulatorProcessor::Save(std::ostream& out) {
	// everything of the modulator's own goes ahead of the inherited PARAMS block:
	// AudioProcessor::Load stops at PARAMS_END and the caller eats exactly one line
	// after it, so there is no room for extra lines on the far side
	for (int lane = 0; lane < kLanes; ++lane) {
		out << "MOD_LEVELS " << lane;
		for (int step = 0; step < kMaxSteps; ++step)
			out << " " << mLevels[lane][step];
		out << "\n";

		out << "MOD_HEIGHTS " << lane;
		for (int step = 0; step < kMaxSteps; ++step)
			out << " " << mHeights[lane][step];
		out << "\n";

		out << "MOD_CURVES " << lane;
		for (int step = 0; step < kMaxSteps; ++step)
			out << " " << mCurves[lane][step];
		out << "\n";
	}

	for (const auto& target : mTargets) {
		std::string path;
		if (target.pending) {
			// never resolved (the project it names was not loaded), so the address it
			// arrived with is still the best thing to write back out
			path = target.pendingPath;
		} else if (auto device = target.device.lock()) {
			auto track = target.track.lock();
			if (!track)
				continue;
			path = PathOfDevice(track->GetProcessors(), device);
			if (path.empty())
				continue; // the device it named has left the project
		}
		out << "MOD_TARGET " << target.trackId << " \"" << path << "\" \"" << target.paramName
			<< "\" " << target.minValue << " " << target.maxValue << " " << (target.enabled ? 1 : 0) << "\n";
	}

	AudioProcessor::Save(out);
}

void ModulatorProcessor::Load(std::istream& in) {
	mTargets.clear();

	std::string line;
	while (std::getline(in, line)) {
		std::stringstream ss(line);
		std::string token;
		ss >> token;

		if (token == "MOD_LEVELS") {
			int lane = -1;
			ss >> lane;
			if (lane >= 0 && lane < kLanes) {
				for (int step = 0; step < kMaxSteps; ++step)
					ss >> mLevels[lane][step];
			}
		} else if (token == "MOD_HEIGHTS") {
			int lane = -1;
			ss >> lane;
			if (lane >= 0 && lane < kLanes) {
				for (int step = 0; step < kMaxSteps; ++step)
					ss >> mHeights[lane][step];
			}
		} else if (token == "MOD_CURVES") {
			int lane = -1;
			ss >> lane;
			if (lane >= 0 && lane < kLanes) {
				for (int step = 0; step < kMaxSteps; ++step)
					ss >> mCurves[lane][step];
			}
		} else if (token == "MOD_TARGET") {
			const size_t q1 = line.find('"');
			if (q1 == std::string::npos)
				continue;
			const size_t q2 = line.find('"', q1 + 1);
			const size_t q3 = line.find('"', q2 + 1);
			const size_t q4 = line.find('"', q3 + 1);
			if (q4 == std::string::npos)
				continue;

			Target target;
			ss >> target.trackId;
			target.pendingPath = line.substr(q1 + 1, q2 - q1 - 1);
			target.paramName = line.substr(q3 + 1, q4 - q3 - 1);
			target.pending = true;

			int enabled = 1;
			std::stringstream rest(line.substr(q4 + 1));
			rest >> target.minValue >> target.maxValue >> enabled;
			target.enabled = (enabled != 0);
			mTargets.push_back(std::move(target));
		} else if (token == "PARAMS_BEGIN") {
			AudioProcessor::Load(in);
			break;
		}
	}

	mBoundGeneration = 0;
}

// ================================================================
// UI
// ================================================================

bool ModulatorProcessor::RenderCustomUI(const ImVec2& size) {
	const ImGuiStyle& style = ImGui::GetStyle();

	RenderHeaderRow();

	// a knob row is label + dial + value; reserve it and give everything left to the
	// lanes, the same split the other custom editors in the rack use
	const float knobRowHeight = ImGui::GetTextLineHeight() * 2.0f + style.ItemInnerSpacing.y * 3.0f + 30.0f;
	float laneAreaHeight = size.y - ImGui::GetFrameHeight() - knobRowHeight - style.ItemSpacing.y * 2.0f;
	laneAreaHeight = std::max(laneAreaHeight, 40.0f);

	const float paletteWidth = (CurrentMode() == ModePerformer) ? ImGui::GetTextLineHeight() * 3.0f : 0.0f;
	if (paletteWidth > 0.0f) {
		RenderCurvePalette(ImVec2(paletteWidth, laneAreaHeight));
		ImGui::SameLine();
	}

	const float laneWidth = size.x - (paletteWidth > 0.0f ? paletteWidth + style.ItemSpacing.x : 0.0f);
	const float laneHeight = std::max((laneAreaHeight - style.ItemSpacing.y) * 0.5f, 16.0f);

	ImGui::BeginGroup();
	RenderLanePanel(0, ImVec2(laneWidth, laneHeight));
	RenderLanePanel(1, ImVec2(laneWidth, laneHeight));
	ImGui::EndGroup();

	RenderKnobRow(size.x, knobRowHeight);
	return true;
}

void ModulatorProcessor::RenderHeaderRow() {
	const ImGuiStyle& style = ImGui::GetStyle();
	const int mode = CurrentMode();
	const bool synced = pSync->value >= 0.5f;

	// a stepped parameter changed from a combo is one instant edit, not a drag
	auto commit = [](Parameter* parameter, float value) {
		const float oldValue = parameter->value;
		parameter->value = value;
		parameter->CommitEditImmediate(oldValue);
	};

	auto comboParam = [&](const char* id, Parameter* parameter, const char* const* names, int count, float width, const char* tooltip) {
		const int current = std::clamp((int)std::lround(parameter->value), 0, count - 1);
		ImGui::SetNextItemWidth(width);
		if (ImGui::BeginCombo(id, names[current], ImGuiComboFlags_NoArrowButton)) {
			for (int i = 0; i < count; ++i) {
				if (ImGui::Selectable(names[i], i == current))
					commit(parameter, (float)i);
			}
			ImGui::EndCombo();
		}
		if (tooltip && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", tooltip);
	};

	const float modeWidth = ImGui::CalcTextSize("Performer").x + style.FramePadding.x * 4.0f;
	comboParam("##ModMode", pMode, kModeNames, kNumModes, modeWidth,
			   "LFO: a waveform per lane\n"
			   "Performer: every step plays a curve\n"
			   "Stepper: every step holds a level");

	ImGui::SameLine();
	if (ToggleButton("Sync", synced))
		commit(pSync, synced ? 0.0f : 1.0f);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Lock the cycle to the project tempo");

	ImGui::SameLine();
	const float rateWidth = ImGui::CalcTextSize("8 Bars").x + style.FramePadding.x * 4.0f;
	if (synced)
		comboParam("##ModDivision", pDivision, kDivisionNames, kNumDivisions, rateWidth, "Length of one cycle");
	else
		pRate->DrawCompact(rateWidth, nullptr);

	ImGui::SameLine();
	const bool restart = pRestart->value >= 0.5f;
	if (ToggleButton("Restart", restart))
		commit(pRestart, restart ? 0.0f : 1.0f);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Put the cycle back to its start on play, on a seek and on every note");

	if (mode == ModeLFO) {
		const float shapeWidth = ImGui::CalcTextSize("Triangle").x + style.FramePadding.x * 4.0f;
		ImGui::SameLine();
		comboParam("##ModShapeA", pShapeA, kShapeNames, kNumShapes, shapeWidth, "Waveform of the upper lane");
		ImGui::SameLine();
		comboParam("##ModShapeB", pShapeB, kShapeNames, kNumShapes, shapeWidth, "Waveform of the lower lane");
	} else {
		ImGui::SameLine();
		ImGui::TextUnformatted("Steps");
		ImGui::SameLine();
		pSteps->DrawCompact(ImGui::CalcTextSize("16").x + style.FramePadding.x * 4.0f, "%.0f");
	}

	if (mode != ModeLFO) {
		ImGui::SameLine();
		if (ToggleButton("Snap", mSnapToGrid))
			mSnapToGrid = !mSnapToGrid;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Quantize a dragged step height to eighths");
	}

	ImGui::SameLine();
	if (ImGui::Button("Rnd"))
		RandomizePattern();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Roll a new pattern for both lanes");
}

void ModulatorProcessor::RandomizePattern() {
	const int mode = CurrentMode();

	if (mode == ModeLFO) {
		// the shapes are ordinary parameters, so rolling them is an ordinary edit and
		// records itself
		Parameter* shapes[] = {pShapeA, pShapeB};
		for (Parameter* shape : shapes) {
			const float oldValue = shape->value;
			shape->value = (float)std::clamp((int)(RandomUnit() * (float)kNumShapes), 0, kNumShapes - 1);
			shape->CommitEditImmediate(oldValue);
		}
		return;
	}

	BeginPatternEdit();
	const int steps = StepCount();
	for (int lane = 0; lane < kLanes; ++lane) {
		for (int step = 0; step < steps; ++step) {
			if (mode == ModeStepper) {
				mLevels[lane][step] = RandomUnit();
			} else {
				mCurves[lane][step] = std::clamp((int)(RandomUnit() * (float)kNumCurves), 0, kNumCurves - 1);
				// kept off the floor: a roll that flattens half the steps reads as a
				// broken randomizer rather than as a pattern
				mHeights[lane][step] = 0.5f + 0.5f * RandomUnit();
			}
		}
	}
	EndPatternEdit("Randomize modulator");
}

void ModulatorProcessor::RenderCurvePalette(const ImVec2& size) {
	const Theme& th = Theme::Instance();
	const ImGuiStyle& style = ImGui::GetStyle();
	ImDrawList* dl = ImGui::GetWindowDrawList();

	constexpr int kColumns = 2;
	const int rows = (kNumCurves + kColumns - 1) / kColumns;
	const float cellWidth = std::max((size.x - style.ItemSpacing.x * (kColumns - 1)) / (float)kColumns, 8.0f);
	const float cellHeight = std::max((size.y - style.ItemSpacing.y * (rows - 1)) / (float)rows, 8.0f);

	ImGui::BeginGroup();
	for (int curve = 0; curve < kNumCurves; ++curve) {
		ImGui::PushID(curve);
		if (curve % kColumns != 0)
			ImGui::SameLine();

		const ImVec2 cellPos = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##Curve", ImVec2(cellWidth, cellHeight));
		if (ImGui::IsItemClicked())
			mBrush = curve;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s\nDraw across the steps to give them this curve", kCurveNames[curve]);

		const bool selected = (mBrush == curve);
		const ImVec2 cellEnd(cellPos.x + cellWidth, cellPos.y + cellHeight);
		dl->AddRectFilled(cellPos, cellEnd, th.bgDeepest);
		dl->AddRect(cellPos, cellEnd, selected ? th.accent : th.border);
		DrawCurveShape(dl, cellPos, ImVec2(cellWidth, cellHeight), curve,
					   selected ? th.accent : th.graphCurveCool, 1.0f);
		ImGui::PopID();
	}
	ImGui::EndGroup();
}

void ModulatorProcessor::RenderLanePanel(int lane, const ImVec2& size) {
	const Theme& th = Theme::Instance();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const int mode = CurrentMode();
	const int steps = StepCount();
	const float width = std::max(size.x, 1.0f);
	const float height = std::max(size.y, 1.0f);

	ImGui::PushID(lane);
	const ImVec2 panelPos = ImGui::GetCursorScreenPos();
	// right-drag paints the palette's curve onto a Performer step, so the two things a
	// step carries - its height and its shape - each get a gesture of their own
	ImGui::InvisibleButton("##Lane", ImVec2(width, height),
						   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	const ImVec2 panelEnd(panelPos.x + width, panelPos.y + height);

	dl->AddRectFilled(panelPos, panelEnd, th.bgDeepest);

	// ---- interaction ----
	// a level or a curve is a plain member the audio thread reads, exactly like a
	// parameter value: the drag writes it live and reports one undo entry when it ends
	if (mode != ModeLFO) {
		if (ImGui::IsItemActivated()) {
			BeginPatternEdit();
			mDragLane = lane;
			// the button the gesture opened with decides what it writes, latched so that
			// letting go of one mid-drag cannot turn it into the other
			mHeightOnly = ImGui::IsMouseDown(ImGuiMouseButton_Right);
		}
		if (ImGui::IsItemActive() && mDragLane == lane) {
			const ImVec2 mouse = ImGui::GetIO().MousePos;
			const float x = std::clamp((mouse.x - panelPos.x) / width, 0.0f, 0.9999f);
			const int step = std::clamp((int)(x * (float)steps), 0, steps - 1);

			float level = 1.0f - std::clamp((mouse.y - panelPos.y) / height, 0.0f, 1.0f);
			if (mSnapToGrid)
				level = std::round(level * 8.0f) / 8.0f;
			StepValues(lane)[step] = level;

			// drawing in Performer stamps the palette's curve as it goes: the brush
			// writes the whole step, height and shape both, because half a brush is what
			// makes picking a curve out of the palette look like it did nothing.
			// right-drag is the way to reshape an envelope without restyling it
			if (mode == ModePerformer && !mHeightOnly)
				mCurves[lane][step] = mBrush;
		}
	}
	// outside the mode check on purpose: a gesture that started in a stepped mode has to
	// be closed even if the mode was switched out from under it
	if (ImGui::IsItemDeactivated() && mDragLane == lane) {
		EndPatternEdit(mode == ModeStepper ? "Modulator steps" : "Modulator pattern");
		mDragLane = -1;
		mHeightOnly = false;
	}

	if (mode == ModePerformer && ImGui::IsItemHovered() && !ImGui::IsItemActive())
		ImGui::SetTooltip("Draw to set a step's height and give it the palette's curve\n"
						  "Right-drag to change heights only");

	// ---- content ----
	const float cellWidth = width / (float)steps;
	if (mode == ModeLFO) {
		const Parameter* shapeParam = (lane == 0) ? pShapeA : pShapeB;
		const int shape = std::clamp((int)std::lround(shapeParam->value), 0, kNumShapes - 1);

		for (int i = 1; i < 4; ++i) {
			const float x = panelPos.x + width * (float)i * 0.25f;
			dl->AddLine(ImVec2(x, panelPos.y), ImVec2(x, panelEnd.y), th.gridSub);
		}

		if (shape == ShapeRandom) {
			for (int step = 0; step < steps; ++step) {
				const float v = PreviewRandom(step);
				const float x0 = panelPos.x + (float)step * cellWidth;
				const float y = panelEnd.y - 2.0f - v * (height - 4.0f);
				dl->AddLine(ImVec2(x0, y), ImVec2(x0 + cellWidth, y), th.graphCurve, 1.5f);
			}
		} else {
			const int segments = std::max((int)width / 2, 8);
			std::vector<ImVec2> points((size_t)segments + 1);
			for (int i = 0; i <= segments; ++i) {
				const float t = (float)i / (float)segments;
				points[(size_t)i] = ImVec2(panelPos.x + t * width,
										   panelEnd.y - 2.0f - EvaluateShape(shape, t) * (height - 4.0f));
			}
			dl->AddPolyline(points.data(), segments + 1, th.graphCurve, ImDrawFlags_None, 1.5f);
		}
	} else {
		const float* values = StepValues(lane);
		const ImVec2 cellSize(cellWidth, height);
		const ImVec2 cellInset(0.0f, 2.0f); // edge to edge across, so neighbours meet

		for (int step = 0; step < steps; ++step) {
			const float x0 = panelPos.x + (float)step * cellWidth;
			const float x1 = x0 + cellWidth;
			if (step > 0)
				dl->AddLine(ImVec2(x0, panelPos.y), ImVec2(x0, panelEnd.y), th.gridSub);

			const float level = std::clamp(values[step], 0.0f, 1.0f);
			const float top = panelEnd.y - level * height;
			if (mode == ModeStepper) {
				dl->AddRectFilled(ImVec2(x0 + 1.0f, top), ImVec2(x1 - 1.0f, panelEnd.y), th.accentMuted);
				dl->AddLine(ImVec2(x0 + 1.0f, top), ImVec2(x1 - 1.0f, top), th.graphCurve, 1.5f);
				continue;
			}

			// the ceiling the shape is drawn against, which is also what a drag is
			// grabbing - without it a flattened step looks like an empty cell
			dl->AddLine(ImVec2(x0 + 1.0f, top), ImVec2(x1 - 1.0f, top), th.accentMuted, 1.0f);
			DrawCurveShape(dl, ImVec2(x0, panelPos.y), cellSize, mCurves[lane][step],
						   th.graphCurve, 1.5f, level, cellInset);

			// join this step's last point to the next one's first. the sixteen curves are
			// one envelope, and without the risers they read as sixteen unrelated
			// drawings. dimmer than the curves themselves, because a jump between two
			// steps is the shape of the pattern rather than anything the generator sweeps
			if (step + 1 < steps) {
				const float nextLevel = std::clamp(values[step + 1], 0.0f, 1.0f);
				const ImVec2 endHere = CurvePoint(ImVec2(x0, panelPos.y), cellSize, mCurves[lane][step], 1.0f, level, cellInset);
				const ImVec2 startNext = CurvePoint(ImVec2(x1, panelPos.y), cellSize, mCurves[lane][step + 1], 0.0f, nextLevel, cellInset);
				dl->AddLine(endHere, startNext, Theme::WithAlpha(th.graphCurve, 90), 1.0f);
			}
		}
	}

	// ---- readout ----
	// the cycle position and the blended output, so the lane shows what is actually
	// being written rather than only what was drawn
	const float playheadX = panelPos.x + std::clamp(mVisPhase, 0.0f, 1.0f) * width;
	dl->AddLine(ImVec2(playheadX, panelPos.y), ImVec2(playheadX, panelEnd.y), th.playhead, 1.0f);
	const float valueY = panelEnd.y - std::clamp(mVisValue, 0.0f, 1.0f) * height;
	dl->AddLine(ImVec2(panelPos.x, valueY), ImVec2(panelEnd.x, valueY), Theme::WithAlpha(th.accent, 90), 1.0f);

	dl->AddRect(panelPos, panelEnd, th.border);
	ImGui::PopID();
}

void ModulatorProcessor::RenderKnobRow(float width, float height) {
	const ImGuiStyle& style = ImGui::GetStyle();

	Parameter* knobs[] = {pXFade, pDepth, pOffset, pSmooth, pPhase, pAttack, pDecay, pEnvAmount};
	const char* labels[] = {"XFade", "Depth", "Offset", "Glide", "Phase", "Att", "Dec", "Env"};
	constexpr int kCount = 8;

	const float cellWidth = std::max((width - style.ItemSpacing.x * (kCount - 1)) / (float)kCount, 16.0f);
	const float radius = std::min(KnobParameter::RadiusForHeight(height), KnobParameter::kDefaultRadius);

	for (int i = 0; i < kCount; ++i) {
		if (i > 0)
			ImGui::SameLine();
		ImGui::BeginGroup();
		if (radius > 0.0f)
			static_cast<KnobParameter*>(knobs[i])->DrawSized(radius, cellWidth, labels[i]);
		else
			knobs[i]->DrawCompact(cellWidth, nullptr);
		ImGui::EndGroup();
	}
}
