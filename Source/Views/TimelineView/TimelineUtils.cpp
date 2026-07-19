#include "PrecompHeader.h"
#include "TimelineUtils.h"
#include "Theme.h"
#include <cmath>
#include <algorithm>

namespace TimelineUtils {

	void RenderWaveform(ImDrawList* drawList, const std::vector<float>& samples, int channels,
						double sourceFramesPerPixel, double offsetFrames,
						const ImVec2& rectMin, const ImVec2& rectMax,
						ImU32 color, bool forceMono) {
		if (samples.empty())
			return;

		size_t totalFileFrames = samples.size() / channels;

		// viewport culling
		ImVec2 clipMin = drawList->GetClipRectMin();
		ImVec2 clipMax = drawList->GetClipRectMax();

		float startPx = std::max(rectMin.x, clipMin.x);
		float endPx = std::min(rectMax.x, clipMax.x);

		if (startPx >= endPx)
			return;

		bool drawStereo = (channels == 2 && !forceMono);

		// map an absolute source-frame index to its screen x. rectMin.x is the only
		// moving term as the clip scrolls, so every bin/sample translates smoothly
		auto FrameToX = [&](double frame) -> float {
			return rectMin.x + (float)((frame - offsetFrames) / sourceFramesPerPixel);
		};

		// visible span expressed in absolute source frames
		double fStartD = (double)(startPx - rectMin.x) * sourceFramesPerPixel + offsetFrames;
		double fEndD = (double)(endPx - rectMin.x) * sourceFramesPerPixel + offsetFrames;

		auto DrawSegment = [&](float midY, float halfH, int channelIdx) {
			if (fEndD <= 0.0)
				return;

			long long visStart = (long long)std::floor(fStartD);
			long long visEnd = (long long)std::ceil(fEndD);
			if (visStart < 0)
				visStart = 0;
			if (visEnd > (long long)totalFileFrames)
				visEnd = (long long)totalFileFrames;
			if (visStart >= visEnd)
				return;

			if (sourceFramesPerPixel > 1.0) {
				// fixed peak bins anchored to an ABSOLUTE sample grid (multiples of
				// binFrames), NOT to the moving screen columns. each bin covers a constant
				// source-frame range, so its min/max never changes as the clip scrolls
				// sub-pixel (follow mode) - the waveform slides but keeps its exact shape
				size_t binFrames = (size_t)std::max(1.0, std::round(sourceFramesPerPixel));
				size_t scanStep = (size_t)std::max(1.0, (double)binFrames / 10.0);

				size_t firstBin = (size_t)visStart / binFrames;
				size_t lastBin = ((size_t)visEnd + binFrames - 1) / binFrames;

				for (size_t b = firstBin; b < lastBin; ++b) {
					size_t idxStart = b * binFrames;
					size_t idxEnd = idxStart + binFrames;
					if (idxStart >= totalFileFrames)
						break;
					if (idxEnd > totalFileFrames)
						idxEnd = totalFileFrames;

					float minVal = 0.0f, maxVal = 0.0f;
					bool firstSample = true;

					for (size_t k = idxStart; k < idxEnd; k += scanStep) {
						float v = samples[k * channels + channelIdx];
						if (firstSample) {
							minVal = maxVal = v;
							firstSample = false;
						} else {
							if (v < minVal)
								minVal = v;
							if (v > maxVal)
								maxVal = v;
						}
					}

					if (firstSample)
						continue;

					float x0 = FrameToX((double)idxStart);
					float x1 = FrameToX((double)idxEnd);
					if (x1 < x0 + 1.0f)
						x1 = x0 + 1.0f;

					float y1 = midY + minVal * halfH;
					float y2 = midY + maxVal * halfH;
					if (std::abs(y2 - y1) < 1.0f) {
						y1 -= 0.5f;
						y2 += 0.5f;
					}

					drawList->AddRectFilled(ImVec2(x0, y1), ImVec2(x1, y2), color);
				}
			} else {
				// one sample spans >= 1px: trace a poly-line through the actual samples,
				// each pinned to its own source-frame position so it scrolls smoothly
				long long f0 = visStart > 0 ? visStart - 1 : 0; // 1-sample margin at left edge
				ImVec2 prevPos;
				bool first = true;

				for (long long f = f0; f < visEnd; ++f) {
					float val = samples[(size_t)f * channels + channelIdx];
					ImVec2 currentPos(FrameToX((double)f), midY + val * halfH);

					if (!first)
						drawList->AddLine(prevPos, currentPos, color, 1.5f);
					else
						first = false;
					prevPos = currentPos;
				}
			}
		};

		if (drawStereo) {
			float totalH = rectMax.y - rectMin.y;
			float channelH = totalH * 0.5f;
			float halfH = channelH * 0.45f;

			drawList->AddLine(ImVec2(rectMin.x, rectMin.y + channelH), ImVec2(rectMax.x, rectMin.y + channelH), Theme::WithAlpha(Theme::Instance().bgDeepest, 60));

			DrawSegment(rectMin.y + channelH * 0.5f, halfH, 0); // left
			DrawSegment(rectMin.y + channelH * 1.5f, halfH, 1); // right
		} else {
			float midY = (rectMin.y + rectMax.y) * 0.5f;
			float halfH = (rectMax.y - rectMin.y) * 0.45f;
			DrawSegment(midY, halfH, 0);
		}
	}

} // namespace TimelineUtils
