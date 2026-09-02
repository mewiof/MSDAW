#include "Parameters/SliderParameter.h"
#include "PrecompHeader.h"
#include "Track.h"

#include "Clips/MIDIClip.h"
#include "Clips/AudioClip.h"
#include "Clips/WarpEngine.h"
#include "Mixing.h"
#include "ProcessorIO.h"
#include "SidechainHub.h"
#include "Theme.h"
#include <cmath>
#include <algorithm>
#include <sstream>
#include <cstdlib>

float AutomationCurve::Evaluate(double beat) const {

	if (points.empty()) {
		if (targetParam)
			return targetParam->value;
		return 0.0f;
	}

	if (beat <= points.front().beat)
		return points.front().value;
	if (beat >= points.back().beat)
		return points.back().value;

	for (size_t i = 0; i < points.size() - 1; ++i) {
		if (beat >= points[i].beat && beat < points[i + 1].beat) {
			double t = (beat - points[i].beat) / (points[i + 1].beat - points[i].beat);
			float tension = points[i].tension;

			if (tension > 0.99f)
				tension = 0.99f;
			if (tension < -0.99f)
				tension = -0.99f;

			double curvedT = t;
			if (std::abs(tension) > 0.001f) {
				double exponent = std::pow(10.0, std::abs((double)tension));

				if (tension > 0.0f) {
					curvedT = 1.0 - std::pow(1.0 - t, exponent);
				} else {
					curvedT = std::pow(t, exponent);
				}
			}

			return points[i].value + (float)curvedT * (points[i + 1].value - points[i].value);
		}
	}
	return points.back().value;
}

// hands out Track::mId. ids only need to be unique within a session: loading a project
// adopts the saved ids and pushes the counter past them, so ids created afterwards can
// never collide with the ones a cross-track reference was saved against
static std::atomic<uint32_t> sNextTrackId{1};

static uint32_t AllocateTrackId() {
	return sNextTrackId.fetch_add(1);
}

static void ReserveTrackId(uint32_t id) {
	uint32_t expected = sNextTrackId.load();
	while (id >= expected && !sNextTrackId.compare_exchange_weak(expected, id + 1)) {
	}
}

Track::Track() {
	mId = AllocateTrackId();

	mVolumeParam = std::make_unique<SliderParameter>("Volume", 0.0f, -60.0f, 6.0f);
	mPanParam = std::make_unique<SliderParameter>("Pan", 0.0f, -1.0f, 1.0f);
	// cycle the curated on-theme palette instead of rolling muddy random grays
	// the counter is static so successive new tracks step through distinct hues
	static int sNextTrackColor = 0;
	mColor = Theme::Instance().TrackColor(sNextTrackColor++);
}
Track::~Track() {}

void Track::InitMasterTrackParameters(float initialBpm) {
	mBpmParam = std::make_unique<SliderParameter>("BPM", initialBpm, 20.0f, 300.0f);
}

void Track::PrepareToPlay(double sampleRate) {
	for (auto& proc : mProcessors) {
		proc->PrepareToPlay(sampleRate);
	}
}
void Track::Reset() {
	for (auto& proc : mProcessors) {
		proc->Reset();
	}
	// a panic releases everything the instruments were holding, so the sequencer no
	// longer believes anything of its own is sounding
	mSoundingNotes.reset();
	mPeakL.store(0.0f);
	mPeakR.store(0.0f);
}
void Track::AllNotesOff() {
	// only instruments hold MIDI notes, so only they need flushing on a loop wrap. use
	// AllNotesOff (a note release), NOT Reset: Reset is a hard panic that also cuts a
	// synth's own reverb/delay tail (VST2 sends cc 120 all-sound-off; built-in effects
	// clear their buffers), which would break a looped region's ambience. effects are
	// skipped entirely so their tails ring on across the loop point
	for (auto& proc : mProcessors) {
		if (proc->IsInstrument())
			proc->AllNotesOff();
	}
	mSoundingNotes.reset();
}
void Track::ClearAccumulator() {
	std::fill(mInputAccumulator.begin(), mInputAccumulator.end(), 0.0f);
}
void Track::AddToAccumulator(const float* input, int numFrames, int numChannels) {
	if (mInputAccumulator.size() < (size_t)(numFrames * numChannels)) {
		mInputAccumulator.resize(numFrames * numChannels, 0.0f);
	}
	for (int i = 0; i < numFrames * numChannels; ++i) {
		mInputAccumulator[i] += input[i];
	}
}
void Track::EvaluateAutomation(double currentBeat) {
	for (auto& curve : mAutomationCurves) {
		if (curve.targetParam) {
			float val = curve.Evaluate(currentBeat);
			curve.targetParam->value = val;
		}
	}
}

void Track::Process(float* buffer, int numFrames, int numChannels,
					std::vector<MIDIMessage>& mIDIMessages,
					const ProcessContext& context,
					bool accumulateToOutput,
					bool detectorOnly) {

	// automation processing
	if (context.isPlaying) {
		double currentBeat = (double)context.currentSample / context.sampleRate * (context.bpm / 60.0);
		EvaluateAutomation(currentBeat);
	}

	// accumulate group inputs
	if (mInputAccumulator.size() >= (size_t)(numFrames * numChannels)) {
		for (int i = 0; i < numFrames * numChannels; ++i) {
			buffer[i] += mInputAccumulator[i];
		}
	}

	// sequencer & audio playback
	if (context.isPlaying) {
		double samplesPerBeat = (context.sampleRate * 60.0) / context.bpm;
		int64_t trackStartSample = context.currentSample;
		int64_t trackEndSample = trackStartSample + numFrames;

		// note numbers the clip data says are still down once this block has been
		// rendered. reconciled against mSoundingNotes after the loop
		std::bitset<128> shouldSound;

		for (const auto& clipBase : mClips) {
			// a deactivated clip is inert: no notes, no audio. it still occupies its span
			// on the timeline, so overlap resolution and dragging are unaffected
			if (!clipBase->IsEnabled())
				continue;

			int64_t clipStartSample = (int64_t)(clipBase->GetStartBeat() * samplesPerBeat);
			int64_t clipDurationSamples = (int64_t)(clipBase->GetDuration() * samplesPerBeat);
			int64_t clipEndSample = clipStartSample + clipDurationSamples;

			if (clipEndSample <= trackStartSample || clipStartSample >= trackEndSample)
				continue;

			double offsetBeats = clipBase->GetOffset();

			// handle MIDIClip
			auto mIDIClip = std::dynamic_pointer_cast<MIDIClip>(clipBase);
			if (mIDIClip) {
				const auto& notes = mIDIClip->GetNotes();
				for (const auto& note : notes) {
					// clip data can carry an out-of-range pitch (a malformed MIDI import),
					// and the ledger below is indexed by it
					int noteNumber = std::clamp(note.noteNumber, 0, 127);

					// apply offset to note position
					double adjustedStart = note.startBeat - offsetBeats;
					if (adjustedStart < 0)
						continue; // note starts before current clip view

					int64_t noteOnAbs = clipStartSample + (int64_t)(adjustedStart * samplesPerBeat);
					int64_t noteOffAbs = noteOnAbs + (int64_t)(note.durationBeats * samplesPerBeat);

					// a note may run past the clip's end. once the playhead leaves the clip
					// the clip is skipped entirely (see the overlap test above), so a note-off
					// out there would never be emitted and the note would sound forever. gate
					// the note at the clip boundary like Ableton/FL: clamp its release to the
					// clip end. also covers a note ending exactly on the clip end that would
					// otherwise fall on the next (skipped) block
					bool clampedToClipEnd = noteOffAbs >= clipEndSample;
					if (clampedToClipEnd)
						noteOffAbs = clipEndSample;

					// clip start and note onset are truncated to samples separately, and the
					// playhead is converted through a different beat->sample path, so a note
					// lined up with the playhead can land a sample or two before the block start
					// on a fresh start/seek, chase such onsets (but only while the note is still
					// sounding, so a fully-past note is never turned on without a matching off)
					const int64_t kOnsetChaseSlopSamples = 4;
					bool fireOn;
					if (context.playheadJumped)
						fireOn = noteOnAbs >= trackStartSample - kOnsetChaseSlopSamples && noteOnAbs < trackEndSample && noteOffAbs > trackStartSample;
					else
						fireOn = noteOnAbs >= trackStartSample && noteOnAbs < trackEndSample;

					if (fireOn) {
						MIDIMessage msg;
						msg.status = 0x90;
						msg.data1 = (uint8_t)noteNumber;
						msg.data2 = (uint8_t)note.velocity;
						int64_t onFrame = noteOnAbs - trackStartSample;
						msg.frameIndex = (int)(onFrame > 0 ? onFrame : 0); // clamp a chased onset to the block start
						mIDIMessages.push_back(msg);
						mSoundingNotes.set(noteNumber);
					}

					// a note straddling the end of this block is still down when the block
					// is handed over, and is what keeps the reconcile below from releasing it
					if (noteOnAbs < trackEndSample && noteOffAbs > trackEndSample)
						shouldSound.set(noteNumber);

					// a note-off clamped to the clip end can land on clipEndSample == trackEndSample
					// (the block's exclusive upper edge), which the plain [start, end) test would drop
					// and then never revisit; accept the upper edge in that case and clamp the frame
					// into the block's valid range
					bool offInBlock = clampedToClipEnd
										  ? (noteOffAbs > trackStartSample && noteOffAbs <= trackEndSample)
										  : (noteOffAbs >= trackStartSample && noteOffAbs < trackEndSample);
					if (offInBlock) {
						MIDIMessage msg;
						msg.status = 0x80;
						msg.data1 = (uint8_t)noteNumber;
						msg.data2 = 0;
						int64_t offFrame = noteOffAbs - trackStartSample;
						if (offFrame > numFrames - 1)
							offFrame = numFrames - 1; // a boundary-aligned cut belongs to this block's last sample
						msg.frameIndex = (int)offFrame;
						mIDIMessages.push_back(msg);
						// clearing the bit here is what stops the reconcile below from
						// emitting a second, block-start note-off for the same release
						mSoundingNotes.reset(noteNumber);
					}
				}
			}

			// handle AudioClip
			auto audioClip = std::dynamic_pointer_cast<AudioClip>(clipBase);
			if (audioClip) {
				const auto& samples = audioClip->GetSamples();
				int clipChannels = audioClip->GetNumChannels();

				// output window into this block, shared by both playback paths
				int64_t overlapStart = std::max(trackStartSample, clipStartSample);
				int64_t overlapEnd = std::min(trackEndSample, clipEndSample);
				int bufferOffset = (int)(overlapStart - trackStartSample);
				int processCount = (int)(overlapEnd - overlapStart);
				int64_t outputSamplesSinceClipStart = overlapStart - clipStartSample;

				// clip file offset in output frames (same for both paths)
				double offsetSeconds = offsetBeats * (60.0 / context.bpm);
				double offsetOutputFrames = offsetSeconds * context.sampleRate;

				if (audioClip->UsesGranularEngine() && !samples.empty() && clipChannels > 0 && processCount > 0) {
					// warped, non-Re-Pitch: the granular engine decouples time from pitch. it is
					// position-addressable, so it fills this block straight from transport time
					// just like the linear path -- seek/loop/offline export stay deterministic
					WarpRenderParams wp;
					wp.mode = audioClip->GetWarpMode();
					wp.sampleRate = context.sampleRate;
					wp.speed = audioClip->ComputeTimeStretchRate(context.sampleRate, context.bpm);
					wp.pitchRead = audioClip->ComputePitchReadRate(context.sampleRate);
					double totalSemis = audioClip->GetTransposeSemitones() + audioClip->GetTransposeCents() / 100.0;
					wp.pitchRatio = std::pow(2.0, totalSemis / 12.0);
					wp.offsetOutputFrames = offsetOutputFrames;
					wp.grainSizeMs = audioClip->GetGrainSizeMs();
					wp.fluctuation = audioClip->GetFluctuation();
					wp.transientEnvelope = audioClip->GetTransientEnvelope();
					wp.formants = audioClip->GetFormants();

					RenderWarpedBlock(samples.data(), samples.size(), clipChannels,
									  outputSamplesSinceClipStart, processCount, numChannels,
									  &buffer[bufferOffset * numChannels], wp);
				} else {
					// unwarped or Re-Pitch: single-rate resample (also drives the waveform preview)
					double playbackRate = audioClip->ComputePlaybackRate(context.sampleRate, context.bpm);
					double offsetSourceFrames = offsetOutputFrames * playbackRate;
					double startReadFrame = (double)outputSamplesSinceClipStart * playbackRate + offsetSourceFrames;

					for (int i = 0; i < processCount; ++i) {
						double framePos = startReadFrame + ((double)i * playbackRate);
						int frameIndex = (int)framePos;

						// bounds check
						if (frameIndex < 0)
							continue;
						if ((size_t)((frameIndex + 1) * clipChannels) >= samples.size())
							break; // end of file

						double alpha = framePos - frameIndex;

						for (int c = 0; c < numChannels; ++c) {
							int srcC = c % clipChannels;
							int idx1 = frameIndex * clipChannels + srcC;
							int idx2 = idx1 + clipChannels;

							float val = samples[idx1] + (float)alpha * (samples[idx2] - samples[idx1]);
							buffer[(bufferOffset + i) * numChannels + c] += val;
						}
					}
				}
			}
		}

		// reconcile what is actually sounding against what the clip data now says. a
		// note-on whose matching note-off can no longer be computed - the note was dragged
		// to another pitch or deleted, its clip was deactivated, moved or shortened, the
		// tempo changed under it, an undo replaced the sequence, the playhead jumped away -
		// would otherwise sound forever, because every release is derived from the data
		// rather than from what was played. releasing it here costs one block of overhang
		for (size_t noteNumber = 0; noteNumber < mSoundingNotes.size(); ++noteNumber) {
			if (!mSoundingNotes.test(noteNumber) || shouldSound.test(noteNumber))
				continue;
			MIDIMessage msg;
			msg.status = 0x80;
			msg.data1 = (uint8_t)noteNumber;
			msg.data2 = 0;
			msg.frameIndex = 0;
			mIDIMessages.push_back(msg);
		}
		mSoundingNotes = shouldSound;
	}

	// stable, so two events landing on the same frame keep the order they were queued in
	// - a release always precedes a re-trigger queued after it
	std::stable_sort(mIDIMessages.begin(), mIDIMessages.end(), [](const MIDIMessage& a, const MIDIMessage& b) {
		return a.frameIndex < b.frameIndex;
	});

	for (auto& proc : mProcessors) {
		if (!proc->IsBypassed()) {
			proc->Process(buffer, numFrames, numChannels, mIDIMessages, context);
		}
	}

	// the fader: one dB gain and one balance law, shared with a rack chain's mixer
	float currentPeakL = 0.0f;
	float currentPeakR = 0.0f;
	ApplyGainAndPan(buffer, numFrames, numChannels, mVolumeParam->value, mPanParam->value,
					&currentPeakL, &currentPeakR);

	if (detectorOnly) {
		// nothing of this render reaches the mix, so let the meter fall to rest
		mPeakL.store(mPeakL.load() * 0.95f);
		mPeakR.store(mPeakR.load() * 0.95f);
	} else {
		float oldL = mPeakL.load();
		if (currentPeakL > oldL)
			mPeakL.store(currentPeakL);
		else
			mPeakL.store(oldL * 0.95f);

		float oldR = mPeakR.load();
		if (currentPeakR > oldR)
			mPeakR.store(currentPeakR);
		else
			mPeakR.store(oldR * 0.95f);
	}

	// feed the detector bus, post-fader, so "what the kick sounds like" is literally
	// what drives a sidechain on another track. no-op unless something subscribed to
	// this track, so ordinary tracks pay one map lookup per block
	SidechainHub& hub = SidechainHub::Instance();
	if (hub.IsSource(mId))
		hub.Publish(mId, buffer, numFrames, numChannels);
}

void Track::AddClip(std::shared_ptr<Clip> clip) {
	mClips.push_back(clip);
	// clean up overlaps when a new clip is added
	ResolveOverlaps(clip);
}
void Track::RemoveClip(std::shared_ptr<Clip> clip) {
	auto it = std::remove(mClips.begin(), mClips.end(), clip);
	if (it != mClips.end()) {
		mClips.erase(it, mClips.end());
	}
}

void Track::ResolveOverlaps(std::shared_ptr<Clip> activeClip) {
	if (!activeClip)
		return;

	double aStart = activeClip->GetStartBeat();
	double aEnd = activeClip->GetEndBeat();

	// collect changes first to avoid modifying the vector while iterating
	std::vector<std::shared_ptr<Clip>> toRemove;
	std::vector<std::shared_ptr<Clip>> toAdd;

	for (auto& b : mClips) {
		if (b == activeClip)
			continue;

		double bStart = b->GetStartBeat();
		double bEnd = b->GetEndBeat();

		// check if they strictly overlap (touching edges doesn't count)
		if (aStart < bEnd && aEnd > bStart) {

			// case 1: active clip completely covers b -> delete b
			if (aStart <= bStart && aEnd >= bEnd) {
				toRemove.push_back(b);
			}
			// case 2: active clip is inside b -> split b into two
			else if (aStart > bStart && aEnd < bEnd) {
				// create the "right" side of the split
				std::shared_ptr<Clip> rightSide = nullptr;
				if (auto ac = std::dynamic_pointer_cast<AudioClip>(b))
					rightSide = std::make_shared<AudioClip>(*ac);
				else if (auto mc = std::dynamic_pointer_cast<MIDIClip>(b))
					rightSide = std::make_shared<MIDIClip>(*mc);

				if (rightSide) {
					// right side starts where a ends
					double timeConsumed = aEnd - bStart;
					rightSide->SetStartBeat(aEnd);
					rightSide->SetDuration(bEnd - aEnd);
					// adjust offset so content continues correctly
					rightSide->SetOffset(b->GetOffset() + timeConsumed);
					toAdd.push_back(rightSide);
				}

				// trim "left" side (b) to end where a starts
				b->SetDuration(aStart - bStart);
			}
			// case 3: active clip overlaps the tail of b -> trim b end
			else if (bStart < aStart && bEnd > aStart) {
				b->SetDuration(aStart - bStart);
			}
			// case 4: active clip overlaps the head of b -> trim b start
			else if (bStart >= aStart && bStart < aEnd) {
				double overlapAmount = aEnd - bStart;
				b->SetStartBeat(aEnd);
				b->SetDuration(b->GetDuration() - overlapAmount);
				// moving start right means we must offset into the content
				b->SetOffset(b->GetOffset() + overlapAmount);
			}
		}
	}

	// apply deletions
	for (auto& clip : toRemove) {
		RemoveClip(clip);
	}

	// apply additions (the right-hand sides of splits)
	for (auto& clip : toAdd) {
		mClips.push_back(clip);
	}
}

void Track::CollectOwnParameters(std::vector<Parameter*>& out) {
	out.push_back(mVolumeParam.get());
	out.push_back(mPanParam.get());
	if (mBpmParam) {
		out.push_back(mBpmParam.get());
	}
}

std::vector<Parameter*> Track::GetAllParameters() {
	// ProcessorHost walks the chain, descending into any rack sitting on it - so
	// grouping a device never takes its parameters off the automation list
	std::vector<Parameter*> params;
	CollectParameters(params);
	return params;
}

AutomationCurve* Track::GetAutomationCurve(Parameter* param) {
	for (auto& curve : mAutomationCurves) {
		if (curve.targetParam == param)
			return &curve;
	}
	AutomationCurve newCurve;
	newCurve.targetParam = param;
	newCurve.paramName = param->name;
	mAutomationCurves.push_back(newCurve);
	return &mAutomationCurves.back();
}

Parameter* Track::FindParameter(const std::string& name) {
	// the same walk GetAllParameters does, so a curve rebinds onto anything the
	// automation lane was able to offer it - nested devices included
	for (Parameter* parameter : GetAllParameters()) {
		if (parameter->name == name)
			return parameter;
	}
	return nullptr;
}

void Track::AddAutomationPoint(Parameter* param, double beat, float value) {
	AutomationCurve* curve = GetAutomationCurve(param);
	bool found = false;
	for (auto& p : curve->points) {
		if (std::abs(p.beat - beat) < 0.001) {
			p.value = value;
			found = true;
			break;
		}
	}
	if (!found) {
		curve->points.push_back({beat, value, 0.0f});
	}
	SortAutomationPoints(param);
}

void Track::RemoveAutomationPoint(Parameter* param, int index) {
	AutomationCurve* curve = GetAutomationCurve(param);
	if (index >= 0 && index < (int)curve->points.size()) {
		curve->points.erase(curve->points.begin() + index);
	}
}

void Track::SortAutomationPoints(Parameter* param) {
	AutomationCurve* curve = GetAutomationCurve(param);
	std::sort(curve->points.begin(), curve->points.end(),
			  [](const AutomationPoint& a, const AutomationPoint& b) { return a.beat < b.beat; });
}

std::vector<AutomationPoint> Track::GetAutomationPoints(Parameter* param) {
	AutomationCurve* curve = GetAutomationCurve(param);
	return curve->points;
}

void Track::SetAutomationPoints(Parameter* param, const std::vector<AutomationPoint>& points) {
	AutomationCurve* curve = GetAutomationCurve(param);
	curve->points = points;
}

bool Track::HasInstrument() const {
	for (const auto& proc : mProcessors) {
		if (proc->IsInstrument())
			return true;
	}
	return false;
}

void Track::RebindAutomation() {
	for (auto& curve : mAutomationCurves) {
		curve.targetParam = FindParameter(curve.paramName);
	}
	if (mSelectedAutomationParam) {
		mSelectedAutomationParam = FindParameter(mSelectedAutomationParam->name);
	}
}

void Track::Save(std::ostream& out, int trackIndex) {
	out << "TRACK_BEGIN\n";
	out << "ID " << mId << "\n";
	out << "NAME \"" << mName << "\"\n";
	out << "COLOR " << mColor << "\n";
	out << "VOL " << mVolumeParam->value << "\n";
	out << "PAN " << mPanParam->value << "\n";
	out << "MUTE " << (mMute ? 1 : 0) << "\n";
	out << "SOLO " << (mSolo ? 1 : 0) << "\n";
	out << "GROUP " << (mIsGroup ? 1 : 0) << "\n";
	out << "COLLAPSED " << (mIsCollapsed ? 1 : 0) << "\n";

	for (auto& proc : mProcessors)
		ProcessorIO::SaveProcessor(out, *proc);

	for (auto& clip : mClips) {
		std::string type = "UNKNOWN";
		if (std::dynamic_pointer_cast<AudioClip>(clip))
			type = "AUDIO";
		else if (std::dynamic_pointer_cast<MIDIClip>(clip))
			type = "MIDI";

		out << "CLIP_GRID_NEXT " << clip->GetGridNumerator() << " " << clip->GetGridDenominator() << "\n";
		out << "CLIP_BEGIN " << type << "\n";
		clip->Save(out);
		out << "CLIP_END\n";
	}

	for (auto& curve : mAutomationCurves) {
		if (curve.points.empty())
			continue;
		out << "AUTO_BEGIN " << "\"" << curve.paramName << "\"\n";
		for (auto& p : curve.points) {
			out << "PT " << p.beat << " " << p.value << " " << p.tension << "\n";
		}
		out << "AUTO_END\n";
	}

	out << "TRACK_END\n";
}

void Track::Load(std::istream& in) {
	int pendingGridNum = 1;
	int pendingGridDen = 4;

	std::string line;
	while (std::getline(in, line)) {
		if (line == "TRACK_END")
			break;

		std::stringstream ss(line);
		std::string token;
		ss >> token;

		if (token == "ID") {
			// projects saved before cross-track references existed have no ID line;
			// those tracks keep the fresh id the constructor handed out
			uint32_t loadedId = 0;
			ss >> loadedId;
			if (loadedId != 0) {
				mId = loadedId;
				ReserveTrackId(loadedId);
			}
		} else if (token == "NAME") {
			size_t q1 = line.find('"');
			size_t q2 = line.find('"', q1 + 1);
			if (q1 != std::string::npos && q2 != std::string::npos)
				mName = line.substr(q1 + 1, q2 - q1 - 1);
		} else if (token == "COLOR") {
			ss >> mColor;
		} else if (token == "VOL") {
			ss >> mVolumeParam->value;
		} else if (token == "PAN") {
			ss >> mPanParam->value;
		} else if (token == "MUTE") {
			int val;
			ss >> val;
			mMute = (val != 0);
		} else if (token == "SOLO") {
			int val;
			ss >> val;
			mSolo = (val != 0);
		} else if (token == "GROUP") {
			int val;
			ss >> val;
			mIsGroup = (val != 0);
		} else if (token == "COLLAPSED") {
			int val;
			ss >> val;
			mIsCollapsed = (val != 0);
		} else if (token == "PARENT_IDX") {
			ss >> mLoadedParentIndex;
		} else if (token == "PROCESSOR") {
			std::string type;
			ss >> type;
			if (auto proc = ProcessorIO::LoadProcessor(in, type))
				AddProcessor(std::move(proc));
		} else if (token == "CLIP_GRID_NEXT") {
			ss >> pendingGridNum >> pendingGridDen;
		} else if (token == "CLIP_BEGIN") {
			std::string type;
			ss >> type;

			std::shared_ptr<Clip> clip = nullptr;
			if (type == "AUDIO") {
				clip = std::make_shared<AudioClip>();
			} else if (type == "MIDI") {
				clip = std::make_shared<MIDIClip>();
			}

			if (clip) {
				clip->SetGrid(pendingGridNum, pendingGridDen);
				clip->Load(in);
				AddClip(clip);
			} else {
				std::string skip;
				while (std::getline(in, skip)) {
					if (skip == "CLIP_END")
						break;
				}
			}
			pendingGridNum = 1;
			pendingGridDen = 4;
		} else if (token == "AUTO_BEGIN") {
			size_t q1 = line.find('"');
			size_t q2 = line.find('"', q1 + 1);
			if (q1 != std::string::npos && q2 != std::string::npos) {
				std::string pName = line.substr(q1 + 1, q2 - q1 - 1);
				AutomationCurve curve;
				curve.paramName = pName;
				curve.targetParam = nullptr;

				while (std::getline(in, line)) {
					if (line == "AUTO_END")
						break;
					if (line.rfind("PT ", 0) == 0) {
						AutomationPoint pt;
						std::stringstream pts(line.substr(3));
						pts >> pt.beat >> pt.value >> pt.tension;
						curve.points.push_back(pt);
					}
				}
				mAutomationCurves.push_back(curve);
			}
		}
	}
}
