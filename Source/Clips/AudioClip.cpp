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
		// maxduration = (beatsinfile) / pitchratio
		// beatsinfile = seconds * (segmentbpm / 60)

		double beatsInFile = fileDurationSecs * (mSegmentBpm / 60.0);
		return beatsInFile / pitchRatio;
	} else {
		// warping disabled: file plays at native speed adjusted by pitch
		// playbackdurationsecs = filedurationsecs / pitchratio

		double playbackDurationSecs = fileDurationSecs / pitchRatio;
		return playbackDurationSecs * (projectBpm / 60.0);
	}
}
void AudioClip::ValidateDuration(double projectBpm) {
	// 1. calculate how many beats the total file represents at this bpm/pitch/warp setting
	double maxTotalBeats = GetMaxDurationInBeats(projectBpm);

	// 2. the visible duration cannot extend past the end of the file
	// (total file beats) - (start offset beats) = max visible beats
	double maxVisible = maxTotalBeats - mOffset;

	if (maxVisible < 0.0)
		maxVisible = 0.0;

	// 3. clamp
	if (mDuration > maxVisible) {
		mDuration = maxVisible;
	}
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

AudioClipWarpState AudioClip::CaptureWarpState() const {
	AudioClipWarpState s;
	s.warpingEnabled = mWarpingEnabled;
	s.warpMode = mWarpMode;
	s.segmentBpm = mSegmentBpm;
	s.transposeSemitones = mTransposeSemitones;
	s.transposeCents = mTransposeCents;
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
		}
	}
}
