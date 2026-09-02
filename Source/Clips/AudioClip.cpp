#include "PrecompHeader.h"
#include "AudioClip.h"
#include <fstream>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <iostream>

AudioClip::AudioClip() {
	mName = "Audio Clip";
}

struct ChunkHeader {
	char id[4];
	uint32_t size;
};

// riff header
struct RiffHeader {
	char riff[4];		  // "RIFF"
	uint32_t overallSize; //
	char wave[4];		  // "WAVE"
};

bool AudioClip::LoadFromFile(const std::string& path) {

	mFilePath = path; // store for serialization
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open()) {
		std::cout << "Failed to open audio file: " << path << "\n";
		return false;
	}

	// 1. read riff header
	RiffHeader riffHeader;
	file.read((char*)&riffHeader, sizeof(RiffHeader));

	if (std::strncmp(riffHeader.riff, "RIFF", 4) != 0 || std::strncmp(riffHeader.wave, "WAVE", 4) != 0) {
		std::cout << "Invalid WAV file format: " << path << "\n";
		return false;
	}

	// 2. iterate chunks to find 'fmt ' and 'data'
	ChunkHeader chunk;
	bool foundFmt = false;
	bool foundData = false;

	uint16_t formatType = 0;
	uint16_t bitsPerSample = 0;
	uint16_t blockAlign = 0;

	while (file.read((char*)&chunk, sizeof(ChunkHeader))) {
		if (std::strncmp(chunk.id, "fmt ", 4) == 0) {
			// read format chunk
			struct WavFmt {
				uint16_t wFormatTag;
				uint16_t nChannels;
				uint32_t nSamplesPerSec;
				uint32_t nAvgBytesPerSec;
				uint16_t nBlockAlign;
				uint16_t wBitsPerSample;
			} fmt;

			if (chunk.size < 16) {
				std::cout << "Error: fmt chunk too small\n";
				return false;
			}

			file.read((char*)&fmt, 16);

			formatType = fmt.wFormatTag;
			mChannels = fmt.nChannels;
			mSampleRate = (double)fmt.nSamplesPerSec;
			bitsPerSample = fmt.wBitsPerSample;
			blockAlign = fmt.nBlockAlign;

			foundFmt = true;

			if (chunk.size > 16) {
				file.seekg(chunk.size - 16, std::ios::cur);
			}
		} else if (std::strncmp(chunk.id, "data", 4) == 0) {
			foundData = true;
			break; // stop at the start of data
		} else {
			file.seekg(chunk.size, std::ios::cur);
		}
	}

	if (!foundFmt) {
		std::cout << "Error: No 'fmt ' chunk found in WAV.\n";
		return false;
	}
	if (!foundData) {
		std::cout << "Error: No 'data' chunk found in WAV.\n";
		return false;
	}

	uint32_t bytesPerSample = bitsPerSample / 8;
	if (bytesPerSample == 0)
		bytesPerSample = 1;

	uint32_t numSamples = chunk.size / bytesPerSample;
	mSamples.resize(numSamples);

	if (formatType == 1 || formatType == 0xFFFE) { // pcm
		if (bitsPerSample == 16) {
			std::vector<int16_t> temp(numSamples);
			file.read((char*)temp.data(), chunk.size);
			for (size_t i = 0; i < numSamples; ++i) {
				mSamples[i] = temp[i] / 32768.0f;
			}
		} else if (bitsPerSample == 24) {
			uint32_t numFrames = numSamples;
			size_t byteCount = numFrames * 3;
			std::vector<uint8_t> raw(byteCount);
			file.read((char*)raw.data(), byteCount);

			for (size_t i = 0; i < numFrames; ++i) {
				size_t idx = i * 3;
				int32_t val = (raw[idx + 0]) | (raw[idx + 1] << 8) | (raw[idx + 2] << 16);
				if (val & 0x800000)
					val |= 0xFF000000;
				mSamples[i] = val / 8388608.0f;
			}
		} else if (bitsPerSample == 8) {
			std::vector<uint8_t> temp(numSamples);
			file.read((char*)temp.data(), chunk.size);
			for (size_t i = 0; i < numSamples; ++i) {
				mSamples[i] = (temp[i] - 128) / 128.0f;
			}
		} else {
			std::cout << "Unsupported PCM bit depth: " << bitsPerSample << "\n";
			return false;
		}
	} else if (formatType == 3) { // ieee float
		if (bitsPerSample == 32) {
			file.read((char*)mSamples.data(), chunk.size);
		} else {
			std::cout << "Unsupported float bit depth: " << bitsPerSample << "\n";
			return false;
		}
	} else {
		std::cout << "Unsupported WAV format type: " << formatType << "\n";
		return false;
	}

	if (mChannels > 0)
		mTotalFileFrames = numSamples / mChannels;
	else
		mTotalFileFrames = 0;

	return true;
}

void AudioClip::GenerateTestSignal(double sampleRate, double durationSecs) {

	mSampleRate = sampleRate;
	mChannels = 2;
	size_t numFrames = (size_t)(durationSecs * sampleRate);
	mSamples.resize(numFrames * mChannels);

	for (size_t i = 0; i < numFrames; ++i) {
		double t = (double)i / sampleRate;
		double freq = 220.0 + (660.0 * t / durationSecs);
		float val = (float)(0.5 * std::sin(2.0 * 3.14159 * freq * t));

		mSamples[i * 2 + 0] = val;
		mSamples[i * 2 + 1] = val;
	}

	mDuration = durationSecs * 2.0; // approx beats assumption
	mTotalFileFrames = numFrames;
}

double AudioClip::GetMaxDurationInBeats(double projectBpm) const {
	if (mTotalFileFrames == 0 || mSampleRate <= 0.0)
		return 0.0;

	// calculate pitch factor
	double totalSemis = mTransposeSemitones + (mTransposeCents / 100.0);
	double pitchRatio = std::pow(2.0, totalSemis / 12.0);

	double fileDurationSecs = (double)mTotalFileFrames / mSampleRate;

	if (mWarpingEnabled) {
		// when warped, the file represents a fixed number of beats defined by msegmentbpm
		// beatsinfile = seconds * (segmentbpm / 60)
		double beatsInFile = fileDurationSecs * (mSegmentBpm / 60.0);

		// granular modes time-stretch to the grid, so transposing no longer resizes the clip;
		// only Re-Pitch is varispeed, where pitch and length stay coupled like tape speed
		if (mWarpMode == WarpMode::RePitch)
			return beatsInFile / pitchRatio;
		return beatsInFile;
	} else {
		// warping disabled: file plays at native speed adjusted by pitch
		// playbackdurationsecs = filedurationsecs / pitchratio

		double playbackDurationSecs = fileDurationSecs / pitchRatio;
		return playbackDurationSecs * (projectBpm / 60.0);
	}
}
void AudioClip::ValidateDuration(double projectBpm) {
	// with no samples behind it there is nothing to clamp against: a clip whose file has
	// moved (or has not been read yet) reports zero beats of content, and clamping to that
	// would wipe the arrangement's clip lengths on the first tempo or transpose edit
	if (mTotalFileFrames == 0 || mSampleRate <= 0.0)
		return;

	// 1. calculate how many beats the total file represents at this bpm/pitch/warp setting
	double maxTotalBeats = GetMaxDurationInBeats(projectBpm);

	// 2. the visible duration cannot extend past the end of the file
	// (total file beats) - (start offset beats) = max visible beats
	double maxVisible = maxTotalBeats - mOffset;

	// 3. clamp, but never down to nothing. the offset can end up at or past the file's new
	// end (a hard transpose up, a tempo drop), and a zero-length clip is one the timeline
	// can neither draw nor grab - leave a sliver the user can still reach and delete
	if (maxVisible < kMinClipDurationBeats)
		maxVisible = kMinClipDurationBeats;

	if (mDuration > maxVisible) {
		mDuration = maxVisible;
	}
}

void AudioClip::RetimeForBpmChange(double oldBpm, double newBpm) {
	if (oldBpm <= 0.0 || newBpm <= 0.0)
		return;

	// a warped clip is time-stretched onto the grid (ComputePlaybackRate scales it by
	// project/segment bpm), so the same audio always covers the same beats and the tempo
	// has nothing to say about its length. an unwarped one plays at the file's own speed,
	// which makes its beat length a tempo reading of a fixed stretch of seconds: both the
	// window's length and how far into the file it starts have to move with the tempo.
	// merely clamping would truncate the clip on every tempo drop and never give the audio
	// back on the way up
	if (!mWarpingEnabled) {
		double scale = newBpm / oldBpm;
		mDuration *= scale;
		mOffset *= scale;
	}

	ValidateDuration(newBpm);
}

void AudioClip::RetimeForWarpChange(double oldMaxBeats, double projectBpm) {
	// a warp or pitch edit changes how fast the file is read, so the slice the clip was cut
	// to now takes a different number of beats to play. both ends of that window move with
	// it: scaling the offset is what keeps the clip starting on the same moment of audio,
	// and scaling the duration is what stretches or shrinks it on the grid instead of
	// clamping the tail off on the way up and never handing it back on the way down.
	// a granular warp mode holds the file to the grid, so its reach does not move and the
	// scale falls out as exactly one
	double newMaxBeats = GetMaxDurationInBeats(projectBpm);
	if (oldMaxBeats > 0.0 && newMaxBeats > 0.0) {
		double scale = newMaxBeats / oldMaxBeats;
		mDuration *= scale;
		mOffset *= scale;
	}

	ValidateDuration(projectBpm);
}

void AudioClip::FlipSamples() {
	if (mChannels <= 0)
		return;
	const size_t frames = mSamples.size() / (size_t)mChannels;
	if (frames < 2)
		return;

	for (size_t i = 0, j = frames - 1; i < j; ++i, --j) {
		for (int c = 0; c < mChannels; ++c)
			std::swap(mSamples[i * mChannels + c], mSamples[j * mChannels + c]);
	}
}

void AudioClip::Reverse(double projectBpm) {
	FlipSamples();

	// the window has to travel to the other end of the file with the audio it was cut
	// around, or a clip pointing at bar three would come back playing bar one backwards.
	// the reach is read in the clip's own beat units, the same ones the offset is in, so
	// warp and pitch are already accounted for
	double maxBeats = GetMaxDurationInBeats(projectBpm);
	if (maxBeats > 0.0)
		mOffset = std::max(0.0, maxBeats - mOffset - mDuration);

	mReversed = !mReversed;
}

double AudioClip::ComputePlaybackRate(double deviceSampleRate, double projectBpm) const {
	double clipSR = (mSampleRate > 0.0) ? mSampleRate : 44100.0;
	double device = (deviceSampleRate > 0.0) ? deviceSampleRate : 48000.0;

	// base resample: play the file's samples at the device rate
	double rate = clipSR / device;

	// warp: stretch so the file's own tempo (segment bpm) lands on the project tempo
	if (mWarpingEnabled) {
		double seg = (mSegmentBpm > 0.1) ? mSegmentBpm : 120.0;
		rate *= projectBpm / seg;
	}

	// pitch: resampling changes pitch, so transpose is just a further rate scale
	double totalSemis = mTransposeSemitones + (mTransposeCents / 100.0);
	if (std::abs(totalSemis) > 0.001) {
		rate *= std::pow(2.0, totalSemis / 12.0);
	}

	return rate;
}

double AudioClip::ComputeTimeStretchRate(double deviceSampleRate, double projectBpm) const {
	double clipSR = (mSampleRate > 0.0) ? mSampleRate : 44100.0;
	double device = (deviceSampleRate > 0.0) ? deviceSampleRate : 48000.0;

	// resample times warp, deliberately without the pitch factor: this is the rate the
	// grain anchor marches through the source at, so it sets duration but not pitch
	double rate = clipSR / device;
	if (mWarpingEnabled) {
		double seg = (mSegmentBpm > 0.1) ? mSegmentBpm : 120.0;
		rate *= projectBpm / seg;
	}
	return rate;
}

double AudioClip::ComputePitchReadRate(double deviceSampleRate) const {
	double clipSR = (mSampleRate > 0.0) ? mSampleRate : 44100.0;
	double device = (deviceSampleRate > 0.0) ? deviceSampleRate : 48000.0;

	// resample times pitch, deliberately without the warp factor: reading the source at this
	// rate inside a grain transposes it without changing how fast we advance through the file
	double rate = clipSR / device;
	double totalSemis = mTransposeSemitones + (mTransposeCents / 100.0);
	if (std::abs(totalSemis) > 0.001) {
		rate *= std::pow(2.0, totalSemis / 12.0);
	}
	return rate;
}

AudioClipWarpState AudioClip::CaptureWarpState() const {
	AudioClipWarpState s;
	s.warpingEnabled = mWarpingEnabled;
	s.warpMode = mWarpMode;
	s.segmentBpm = mSegmentBpm;
	s.transposeSemitones = mTransposeSemitones;
	s.transposeCents = mTransposeCents;
	s.grainSizeMs = mGrainSizeMs;
	s.fluctuation = mFluctuation;
	s.transientEnvelope = mTransientEnvelope;
	s.formants = mFormants;
	s.duration = mDuration;
	s.offset = mOffset;
	return s;
}

void AudioClip::ApplyWarpState(const AudioClipWarpState& state) {
	mWarpingEnabled = state.warpingEnabled;
	mWarpMode = state.warpMode;
	mSegmentBpm = state.segmentBpm;
	mTransposeSemitones = state.transposeSemitones;
	mTransposeCents = state.transposeCents;
	mGrainSizeMs = state.grainSizeMs;
	mFluctuation = state.fluctuation;
	mTransientEnvelope = state.transientEnvelope;
	mFormants = state.formants;
	mDuration = state.duration;
	mOffset = state.offset;
}

void AudioClip::Save(std::ostream& out) {
	Clip::Save(out); // saves offset
	out << "PATH \"" << mFilePath << "\"\n";
	out << "WARP " << (mWarpingEnabled ? 1 : 0) << "\n";
	out << "WARP_MODE " << (int)mWarpMode << "\n";
	out << "SEG_BPM " << mSegmentBpm << "\n";
	out << "TRANSPOSE " << mTransposeSemitones << "\n";
	out << "TRANSPOSE_FINE " << mTransposeCents << "\n";
	out << "GRAIN_MS " << mGrainSizeMs << "\n";
	out << "FLUX " << mFluctuation << "\n";
	out << "TRANSIENT_ENV " << mTransientEnvelope << "\n";
	out << "FORMANTS " << mFormants << "\n";
	out << "REVERSED " << (mReversed ? 1 : 0) << "\n";
}

void AudioClip::Load(std::istream& in) {
	std::string line;
	while (std::getline(in, line)) {
		if (line == "CLIP_END")
			break;

		// parse base fields
		if (line.rfind("CLIP_NAME ", 0) == 0) {
			size_t q1 = line.find('"');
			size_t q2 = line.find('"', q1 + 1);
			if (q1 != std::string::npos && q2 != std::string::npos) {
				mName = line.substr(q1 + 1, q2 - q1 - 1);
			}
		} else if (line.rfind("START ", 0) == 0) {
			mStartBeat = std::stod(line.substr(6));
		} else if (line.rfind("DUR ", 0) == 0) {
			mDuration = std::stod(line.substr(4));
		} else if (line.rfind("OFFSET ", 0) == 0) {
			mOffset = std::stod(line.substr(7));
		} else if (line.rfind("ENABLED ", 0) == 0) {
			mEnabled = (std::stoi(line.substr(8)) != 0);
		}

		// parse audio fields
		if (line.rfind("PATH ", 0) == 0) {
			size_t q1 = line.find('"');
			size_t q2 = line.find('"', q1 + 1);
			if (q1 != std::string::npos && q2 != std::string::npos) {
				mFilePath = line.substr(q1 + 1, q2 - q1 - 1);
				if (!mFilePath.empty()) {
					LoadFromFile(mFilePath);
				}
			}
		} else if (line.rfind("WARP_MODE ", 0) == 0) {
			int mode = std::stoi(line.substr(10));
			if (mode >= 0 && mode <= 5)
				mWarpMode = (WarpMode)mode;
		} else if (line.rfind("WARP ", 0) == 0) {
			int val = std::stoi(line.substr(5));
			mWarpingEnabled = (val != 0);
		} else if (line.rfind("SEG_BPM ", 0) == 0) {
			mSegmentBpm = std::stod(line.substr(8));
		} else if (line.rfind("TRANSPOSE_FINE ", 0) == 0) {
			mTransposeCents = std::stod(line.substr(15));
		} else if (line.rfind("TRANSPOSE ", 0) == 0) {
			mTransposeSemitones = std::stod(line.substr(10));
		} else if (line.rfind("GRAIN_MS ", 0) == 0) {
			mGrainSizeMs = std::stod(line.substr(9));
		} else if (line.rfind("FLUX ", 0) == 0) {
			mFluctuation = std::stod(line.substr(5));
		} else if (line.rfind("TRANSIENT_ENV ", 0) == 0) {
			mTransientEnvelope = std::stod(line.substr(14));
		} else if (line.rfind("FORMANTS ", 0) == 0) {
			mFormants = std::stod(line.substr(9));
		} else if (line.rfind("REVERSED ", 0) == 0) {
			mReversed = (std::stoi(line.substr(9)) != 0);
		}
	}

	// LoadFromFile read the file forwards, so a reversed clip has to be flipped back
	// here. done after the loop rather than on the REVERSED line, because that line and
	// the PATH that reloads the samples can be parsed in either order. only the buffer:
	// the offset just parsed is already the mirrored one
	if (mReversed)
		FlipSamples();
}
