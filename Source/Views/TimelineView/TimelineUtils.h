#pragma once
#include "imgui.h"
#include <vector>

namespace TimelineUtils {
	// renders audio waveform into the given drawList within rectMin/rectMax
	// sourceFramesPerPixel: defines density
	// offsetFrames: how many source frames into the file to start drawing (supports clip offset)
	void RenderWaveform(ImDrawList* drawList,
						const std::vector<float>& samples,
						int channels,
						double sourceFramesPerPixel,
						double offsetFrames,
						const ImVec2& rectMin,
						const ImVec2& rectMax,
						ImU32 color,
						bool forceMono = false);

	// the chain badge that marks a clip whose notes another clip shares. two linked
	// rings, drawn from a single anchor so the arrangement and the piano roll's clip
	// chips say "linked" with the same mark
	void DrawLinkBadge(ImDrawList* drawList, const ImVec2& topLeft, float height, ImU32 color);

	// how wide DrawLinkBadge comes out at that height, so a caller can right-align it
	float LinkBadgeWidth(float height);
} //namespace TimelineUtils
