#include "PrecompHeader.h"
#include "Spectrum.h"
#include <algorithm>
#include <cmath>

namespace {

	constexpr double kPi = 3.14159265358979323846;

	double LinearToDb(double linear) {
		return 20.0 * std::log10(std::max(linear, 1.0e-9));
	}

} // namespace

namespace spectrum {

	void Transform(double* real, double* imaginary, int size) {
		// bit-reversal permutation first, so the butterflies below can run in place
		for (int i = 1, j = 0; i < size; ++i) {
			int bit = size >> 1;
			for (; j & bit; bit >>= 1)
				j ^= bit;
			j ^= bit;
			if (i < j) {
				std::swap(real[i], real[j]);
				std::swap(imaginary[i], imaginary[j]);
			}
		}

		// NOTE: the twiddle is advanced by repeated complex multiply rather than a trig
		// call per element. the drift that accumulates over one stage is far below the
		// resolution of anything drawn from this
		for (int length = 2; length <= size; length <<= 1) {
			const double angle = -2.0 * kPi / (double)length;
			const double stepReal = std::cos(angle);
			const double stepImaginary = std::sin(angle);
			for (int start = 0; start < size; start += length) {
				double twiddleReal = 1.0;
				double twiddleImaginary = 0.0;
				for (int k = 0; k < length / 2; ++k) {
					const int low = start + k;
					const int high = low + length / 2;
					const double productReal = real[high] * twiddleReal - imaginary[high] * twiddleImaginary;
					const double productImaginary = real[high] * twiddleImaginary + imaginary[high] * twiddleReal;
					real[high] = real[low] - productReal;
					imaginary[high] = imaginary[low] - productImaginary;
					real[low] += productReal;
					imaginary[low] += productImaginary;

					const double nextReal = twiddleReal * stepReal - twiddleImaginary * stepImaginary;
					twiddleImaginary = twiddleReal * stepImaginary + twiddleImaginary * stepReal;
					twiddleReal = nextReal;
				}
			}
		}
	}

	void WindowedTransform(const float* samples, int size, double* real, double* imaginary) {
		for (int i = 0; i < size; ++i) {
			const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * (double)i / (double)(size - 1));
			real[i] = (double)samples[i] * window;
			imaginary[i] = 0.0;
		}

		Transform(real, imaginary, size);
	}

	void Magnitudes(const double* real, const double* imaginary, int size, double* outMagnitude) {
		const double scale = 4.0 / (double)size;
		for (int k = 0; k <= size / 2; ++k)
			outMagnitude[k] = std::sqrt(real[k] * real[k] + imaginary[k] * imaginary[k]) * scale;
	}

	double LogAxisPointFrequency(int index, int count, double minFrequency, double maxFrequency) {
		const double ratio = std::log(maxFrequency / minFrequency);
		return minFrequency * std::exp(ratio * ((double)index + 0.5) / (double)count);
	}

	void FoldToLogAxis(const double* magnitude, int transformSize, double sampleRate,
					   double minFrequency, double maxFrequency,
					   float* outDb, int pointCount) {
		const double rate = sampleRate > 1.0 ? sampleRate : 48000.0;
		const double binHz = rate / (double)transformSize;
		const double ratio = std::log(maxFrequency / minFrequency);

		for (int point = 0; point < pointCount; ++point) {
			const double low = minFrequency * std::exp(ratio * (double)point / (double)pointCount);
			const double high = minFrequency * std::exp(ratio * (double)(point + 1) / (double)pointCount);

			const int first = std::max((int)std::floor(low / binHz), 1);
			const int last = std::min((int)std::ceil(high / binHz), transformSize / 2 - 1);

			// NOTE: the first few points sit below bin 1's center, where the transform has
			// nothing to say, so they all report it. that is two percent of the width at
			// the extreme left edge and no window short enough to be responsive fixes it
			double level = 0.0;
			if (last - first < 2) {
				const double exact = LogAxisPointFrequency(point, pointCount, minFrequency, maxFrequency) / binHz;
				const int lower = std::clamp((int)std::floor(exact), 1, transformSize / 2 - 2);
				const double t = std::clamp(exact - (double)lower, 0.0, 1.0);
				level = magnitude[lower] * (1.0 - t) + magnitude[lower + 1] * t;
			} else {
				for (int k = first; k <= last; ++k)
					level = std::max(level, magnitude[k]);
			}

			outDb[point] = (float)LinearToDb(level);
		}
	}

} // namespace spectrum
