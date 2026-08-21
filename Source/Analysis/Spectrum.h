#pragma once

// the frequency-domain analysis shared by every spectrum this DAW draws - the EQ's
// curve backdrop and the Analyzer's spectrum, spectrogram and per-band stereo bars
//
// it lives here rather than inside either device because the two have to agree: a
// spectrum drawn by one and a readout taken from the other must be the same numbers,
// and the fold onto the log axis is the part that is easy to get subtly wrong
//
// NOTE: nothing here holds state or allocates. every buffer is the caller's, which is
// what lets one caller transform two channels and keep both complex results around
// long enough to take their cross-spectrum
namespace spectrum {

	// in-place radix-2 decimation-in-time transform. `size` must be a power of two
	void Transform(double* real, double* imaginary, int size);

	// Hann-window `size` samples into real/imaginary (each `size` doubles) and
	// transform them in place
	void WindowedTransform(const float* samples, int size, double* real, double* imaginary);

	// magnitudes of bins 0..size/2 inclusive, so outMagnitude holds size/2+1 doubles,
	// scaled by 4/N - which undoes both the transform's length and the Hann window's
	// 0.5 coherent gain - a full-scale sine then reads 1.0 at its own bin
	void Magnitudes(const double* real, const double* imaginary, int size, double* outMagnitude);

	// center frequency of display point `index` of a `count`-point log axis. the
	// mapping, the drawing and any headless readout all have to agree on this or the
	// spectrum lands next to the curve it is supposed to sit under
	double LogAxisPointFrequency(int index, int count, double minFrequency, double maxFrequency);

	// fold the transform's linear bins onto that log axis, in dB
	//
	// the two disagree at both ends, in opposite directions, and taking a peak over a
	// span only answers one of them:
	//   low  - a display point is narrower than one bin, so a plain lookup repeats the
	//          same bin across dozens of points and the curve comes out in steps
	//   high - a display point spans dozens of bins, where the peak is the honest
	//          answer because a lone tone must not average itself away
	// so this interpolates between neighboring bins below the crossover and peaks above it
	void FoldToLogAxis(const double* magnitude, int transformSize, double sampleRate,
					   double minFrequency, double maxFrequency,
					   float* outDb, int pointCount);

} // namespace spectrum
