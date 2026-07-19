#include "Parameters/SliderParameter.h"
#include "PrecompHeader.h"
#include "Track.h"

#include "Clips/MIDIClip.h"
#include "Clips/AudioClip.h"
#include "ProcessorFactory.h"
#include "Theme.h"
#include "Processors/VSTProcessor.h"
#include "Processors/VST3Processor.h"
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

Track::Track() {

	mVolumeParam = std::make_unique<SliderParameter>("Volume", 0.0f, -60.0f, 6.0f);
	mPanParam = std::make_unique<SliderParameter>("Pan", 0.0f, -1.0f, 1.0f);
	// cycle the curated on-theme palette instead of rolling muddy random grays.
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
	mPeakL.store(0.0f);
	mPeakR.store(0.0f);
}
void Track::AllNotesOff() {
	// only instruments hold MIDI notes; their Reset() is a pure note-off/panic. effects
	// clear filter/delay/reverb buffers in Reset(), so skip them to keep tails ringing
	for (auto& proc : mProcessors) {
		if (proc->IsInstrument())
			proc->Reset();
	}
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
void Track::AddProcessor(std::shared_ptr<AudioProcessor> processor) {
	InsertProcessor((int)mProcessors.size(), processor);
}
void Track::InsertProcessor(int index, std::shared_ptr<AudioProcessor> processor) {
	if (index < 0)
		index = 0;
	if (index > (int)mProcessors.size())
		index = (int)mProcessors.size();
	mProcessors.insert(mProcessors.begin() + index, processor);
}
void Track::RemoveProcessor(int index) {
	if (index >= 0 && index < (int)mProcessors.size()) {
		mProcessors.erase(mProcessors.begin() + index);
	}
}
void Track::MoveProcessor(int fromIndex, int toIndex) {
	if (fromIndex < 0 || fromIndex >= (int)mProcessors.size())
		return;
	if (toIndex < 0 || toIndex > (int)mProcessors.size())
		return;
	auto proc = mProcessors[fromIndex];
	if (toIndex > fromIndex)
		toIndex--;
	mProcessors.erase(mProcessors.begin() + fromIndex);
	mProcessors.insert(mProcessors.begin() + toIndex, proc);
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
					bool accumulateToOutput) {

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

		for (const auto& clipBase : mClips) {
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
					// apply offset to note position
					double adjustedStart = note.startBeat - offsetBeats;
					if (adjustedStart < 0)
						continue; // note starts before current clip view

					int64_t noteOnAbs = clipStartSample + (int64_t)(adjustedStart * samplesPerBeat);
					int64_t noteOffAbs = noteOnAbs + (int64_t)(note.durationBeats * samplesPerBeat);

					// clip start and note onset are truncated to samples separately, and the
					// playhead is converted through a different beat->sample path, so a note
					// lined up with the playhead can land a sample or two before the block start.
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
						msg.data1 = (uint8_t)note.noteNumber;
						msg.data2 = (uint8_t)note.velocity;
						int64_t onFrame = noteOnAbs - trackStartSample;
						msg.frameIndex = (int)(onFrame > 0 ? onFrame : 0); // clamp a chased onset to the block start
						mIDIMessages.push_back(msg);
					}
					if (noteOffAbs >= trackStartSample && noteOffAbs < trackEndSample) {
						MIDIMessage msg;
						msg.status = 0x80;
						msg.data1 = (uint8_t)note.noteNumber;
						msg.data2 = 0;
						msg.frameIndex = (int)(noteOffAbs - trackStartSample);
						mIDIMessages.push_back(msg);
					}
				}
			}

			// handle AudioClip
			auto audioClip = std::dynamic_pointer_cast<AudioClip>(clipBase);
			if (audioClip) {
				const auto& samples = audioClip->GetSamples();
				int clipChannels = audioClip->GetNumChannels();
				double clipSR = audioClip->GetSampleRate();
				if (clipSR <= 0.0)
					clipSR = 44100.0;

				// sample rate ratio
				double baseSpeed = clipSR / context.sampleRate;
				double playbackRate = baseSpeed;

				// warping logic
				if (audioClip->IsWarpingEnabled()) {
					double projectBpm = context.bpm;
					double clipBpm = audioClip->GetSegmentBpm();
					if (clipBpm <= 0.1)
						clipBpm = 120.0;

					double warpRatio = projectBpm / clipBpm;
					playbackRate *= warpRatio;
				}

				// transposition
				double semis = audioClip->GetTransposeSemitones();
				double cents = audioClip->GetTransposeCents();
				double totalSemis = semis + (cents / 100.0);

				if (std::abs(totalSemis) > 0.001) {
					double pitchRatio = std::pow(2.0, totalSemis / 12.0);
					playbackRate *= pitchRatio;
				}

				// calculate playback position
				int64_t overlapStart = max(trackStartSample, clipStartSample);
				int64_t overlapEnd = min(trackEndSample, clipEndSample);
				int bufferOffset = (int)(overlapStart - trackStartSample);
				int processCount = (int)(overlapEnd - overlapStart);

				// map output time to source frames, including offset
				int64_t outputSamplesSinceClipStart = overlapStart - clipStartSample;

				// calculate offset in source frames
				double offsetSeconds = offsetBeats * (60.0 / context.bpm); // timeline seconds offset
				double offsetOutputFrames = offsetSeconds * context.sampleRate;
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

	std::sort(mIDIMessages.begin(), mIDIMessages.end(), [](const MIDIMessage& a, const MIDIMessage& b) {
		return a.frameIndex < b.frameIndex;
	});

	for (auto& proc : mProcessors) {
		if (!proc->IsBypassed()) {
			proc->Process(buffer, numFrames, numChannels, mIDIMessages, context);
		}
	}

	float db = mVolumeParam->value;
	float gain = std::pow(10.0f, db / 20.0f);
	float pan = mPanParam->value;
	// stereo balance mode (0dB center)
	// imported clips must play at their original loudness when centered
	float gainL = gain;
	float gainR = gain;
	if (pan > 0.0f) {
		// panning right: attenuate left
		gainL *= (1.0f - pan);
	} else if (pan < 0.0f) {
		// panning left: attenuate right
		gainR *= (1.0f + pan);
	}

	float currentPeakL = 0.0f;
	float currentPeakR = 0.0f;

	if (numChannels >= 2) {
		for (int i = 0; i < numFrames; ++i) {
			float L = buffer[i * numChannels + 0] * gainL;
			float R = buffer[i * numChannels + 1] * gainR;
			buffer[i * numChannels + 0] = L;
			buffer[i * numChannels + 1] = R;
			if (std::abs(L) > currentPeakL)
				currentPeakL = std::abs(L);
			if (std::abs(R) > currentPeakR)
				currentPeakR = std::abs(R);
		}
	} else if (numChannels == 1) {
		for (int i = 0; i < numFrames; ++i) {
			float val = buffer[i] * gain;
			buffer[i] = val;
			if (std::abs(val) > currentPeakL)
				currentPeakL = std::abs(val);
		}
		currentPeakR = currentPeakL;
	}

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

std::vector<Parameter*> Track::GetAllParameters() {
	std::vector<Parameter*> params;
	params.push_back(mVolumeParam.get());
	params.push_back(mPanParam.get());
	if (mBpmParam) {
		params.push_back(mBpmParam.get());
	}
	for (auto& proc : mProcessors) {
		const auto& procParams = proc->GetParameters();
		for (auto& p : procParams) {
			params.push_back(p.get());
		}
	}
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
	if (mVolumeParam->name == name)
		return mVolumeParam.get();
	if (mPanParam->name == name)
		return mPanParam.get();
	if (mBpmParam && mBpmParam->name == name)
		return mBpmParam.get();
	for (auto& proc : mProcessors) {
		for (auto& p : proc->GetParameters()) {
			if (p->name == name)
				return p.get();
		}
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
	out << "NAME \"" << mName << "\"\n";
	out << "COLOR " << mColor << "\n";
	out << "VOL " << mVolumeParam->value << "\n";
	out << "PAN " << mPanParam->value << "\n";
	out << "MUTE " << (mMute ? 1 : 0) << "\n";
	out << "SOLO " << (mSolo ? 1 : 0) << "\n";
	out << "GROUP " << (mIsGroup ? 1 : 0) << "\n";

	for (auto& proc : mProcessors) {
		out << "PROCESSOR " << proc->GetProcessorId() << "\n";
		out << "PROC_SCALING " << (int)proc->GetEditorScalingMode() << "\n";
		proc->Save(out);
		out << "PROCESSOR_END\n";
	}

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

		if (token == "NAME") {
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
		} else if (token == "PARENT_IDX") {
			ss >> mLoadedParentIndex;
		} else if (token == "PROCESSOR") {
			std::string type;
			ss >> type;
			std::shared_ptr<AudioProcessor> proc = nullptr;

			proc = ProcessorFactory::Instance().Create(type);
			// VST is special
			if (!proc && type == "VST")
				proc = std::make_shared<VSTProcessor>("");
			else if (!proc && type == "VST3")
				proc = std::make_shared<VST3Processor>("", "");

			if (proc) {
				// optional per-plugin editor scaling override (written since the
				// high-DPI work; older projects omit it and rewind untouched)
				std::streampos posBefore = in.tellg();
				std::string maybeScaling;
				if (std::getline(in, maybeScaling)) {
					std::stringstream ss2(maybeScaling);
					std::string tk;
					ss2 >> tk;
					if (tk == "PROC_SCALING") {
						int m = 0;
						ss2 >> m;
						proc->SetEditorScalingMode((EditorScalingMode)m);
					} else if (posBefore != std::streampos(-1)) {
						in.seekg(posBefore); // not ours; let proc->Load consume it
					}
				}

				proc->Load(in);
				AddProcessor(proc);
				std::string endTag;
				if (std::getline(in, endTag)) {
				}
			}
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
