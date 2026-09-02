#pragma once
#include <cmath>

// ================================================================
// BLOCK MIXING HELPERS
// ================================================================

// applies a dB gain and a -1..1 stereo balance to an interleaved block in place, and
// reports the peak it leaves behind on each channel (either output may be null)
//
// the one place the balance law lives, shared by a track's fader and a rack chain's:
// center is exactly 0 dB, because an imported clip has to play back at the loudness it
// was recorded at, and panning attenuates the opposite channel rather than boosting one
inline void ApplyGainAndPan(float* buffer, int numFrames, int numChannels,
							float gainDb, float pan, float* outPeakL = nullptr, float* outPeakR = nullptr) {
	const float gain = std::pow(10.0f, gainDb / 20.0f);
	float gainL = gain;
	float gainR = gain;
	if (pan > 0.0f)
		gainL *= (1.0f - pan);
	else if (pan < 0.0f)
		gainR *= (1.0f + pan);

	float peakL = 0.0f;
	float peakR = 0.0f;

	if (numChannels >= 2) {
		for (int i = 0; i < numFrames; ++i) {
			float left = buffer[i * numChannels + 0] * gainL;
			float right = buffer[i * numChannels + 1] * gainR;
			buffer[i * numChannels + 0] = left;
			buffer[i * numChannels + 1] = right;
			if (std::abs(left) > peakL)
				peakL = std::abs(left);
			if (std::abs(right) > peakR)
				peakR = std::abs(right);
		}
	} else if (numChannels == 1) {
		for (int i = 0; i < numFrames; ++i) {
			float value = buffer[i] * gain;
			buffer[i] = value;
			if (std::abs(value) > peakL)
				peakL = std::abs(value);
		}
		peakR = peakL;
	}

	if (outPeakL)
		*outPeakL = peakL;
	if (outPeakR)
		*outPeakR = peakR;
}
