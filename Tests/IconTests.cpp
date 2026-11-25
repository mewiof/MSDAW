#include "PrecompHeader.h"
#include <gtest/gtest.h>
#include <cmath>

#include "Icons.h"
#include "imgui_internal.h" // ImDrawListSharedData, for a draw list with no context behind it

// ================================================================
// EVERY GLYPH DRAWS
// ================================================================

// a glyph the interface names but Icons::Draw has no case for compiles cleanly and
// draws nothing, which on screen is a button with no face. these draw each one into a
// bare draw list - no imgui context, no font - and count what it put there

namespace {

	// a draw list over its own shared data, the minimum imgui needs to turn shapes into
	// vertices. circle and curve tessellation are set to the values imgui's default style
	// gives them, which NewFrame would otherwise copy in
	struct BareDrawList {
		ImDrawListSharedData shared;
		ImDrawList drawList;

		BareDrawList()
			: drawList(&shared) {
			const ImGuiStyle defaults;
			shared.SetCircleTessellationMaxError(defaults.CircleTessellationMaxError);
			shared.CurveTessellationTol = defaults.CurveTessellationTol;
			drawList._ResetForNewFrame();
		}
	};

	const ImU32 kInk = IM_COL32(255, 255, 255, 255);

} // namespace

TEST(Icons, EveryIconPutsGeometryOnScreen) {
	BareDrawList bare;
	for (int i = 0; i < (int)Icon::Count; ++i) {
		const int before = bare.drawList.VtxBuffer.Size;
		Icons::Draw(&bare.drawList, (Icon)i, ImVec2(100.0f, 100.0f), 16.0f, kInk);
		EXPECT_GT(bare.drawList.VtxBuffer.Size, before) << "icon " << i << " drew nothing";
	}
}

TEST(Icons, AGlyphStaysInsideTheBoxItIsGiven) {
	// a toolbar packs its buttons edge to edge, so a glyph spilling out of its box draws
	// over the neighbor's. the anti-aliased fringe is allowed its pixel
	const float size = 20.0f;
	const ImVec2 center(200.0f, 200.0f);
	for (int i = 0; i < (int)Icon::Count; ++i) {
		BareDrawList bare;
		Icons::Draw(&bare.drawList, (Icon)i, center, size, kInk);
		for (const ImDrawVert& vertex : bare.drawList.VtxBuffer) {
			EXPECT_LE(std::abs(vertex.pos.x - center.x), size * 0.5f + 1.5f) << "icon " << i;
			EXPECT_LE(std::abs(vertex.pos.y - center.y), size * 0.5f + 1.5f) << "icon " << i;
		}
	}
}

TEST(Icons, NothingIsDrawnWithNoRoomOrNoInk) {
	BareDrawList bare;
	Icons::Draw(&bare.drawList, Icon::Play, ImVec2(10.0f, 10.0f), 0.0f, kInk);
	Icons::Draw(&bare.drawList, Icon::Play, ImVec2(10.0f, 10.0f), 16.0f, IM_COL32(255, 255, 255, 0));
	Icons::Draw(nullptr, Icon::Play, ImVec2(10.0f, 10.0f), 16.0f, kInk);
	EXPECT_EQ(bare.drawList.VtxBuffer.Size, 0);
}
