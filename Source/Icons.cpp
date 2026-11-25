#include "PrecompHeader.h"
#include "Icons.h"
#include <cmath>
#include <initializer_list>

namespace {

	constexpr float kPi = 3.14159265358979f;

	// one glyph's coordinate frame. every position below is a fraction of the box,
	// measured from its center (-0.5 .. 0.5 spans the box), so each glyph reads in the
	// same units whatever size it is drawn at
	struct Glyph {
		ImDrawList* drawList;
		ImVec2 center;
		float size;
		ImU32 color;
		float stroke;

		ImVec2 P(float x, float y) const { return ImVec2(center.x + x * size, center.y + y * size); }
		float Len(float fraction) const { return fraction * size; }

		void Line(float x0, float y0, float x1, float y1, float weight = 1.0f) const {
			drawList->AddLine(P(x0, y0), P(x1, y1), color, stroke * weight);
		}

		void Polyline(std::initializer_list<ImVec2> points) const {
			for (const ImVec2& point : points)
				drawList->PathLineTo(P(point.x, point.y));
			drawList->PathStroke(color, ImDrawFlags_None, stroke);
		}

		// imgui's anti-aliased fill grows its soft edge along each edge's normal, which
		// points outward only for clockwise points - wound the other way, the fringe eats
		// into the shape. the glyphs are written in whatever order reads best, and this
		// puts them right
		void Fill(std::initializer_list<ImVec2> fractions) const {
			ImVec2 points[8];
			int count = 0;
			for (const ImVec2& fraction : fractions) {
				if (count < 8)
					points[count++] = P(fraction.x, fraction.y);
			}
			float area = 0.0f;
			for (int i = 0; i < count; ++i) {
				const ImVec2& a = points[i];
				const ImVec2& b = points[(i + 1) % count];
				area += a.x * b.y - b.x * a.y;
			}
			if (area < 0.0f) {
				for (int i = 0; i < count / 2; ++i)
					std::swap(points[i], points[count - 1 - i]);
			}
			drawList->AddConvexPolyFilled(points, count, color);
		}

		void Rect(float x0, float y0, float x1, float y1, float rounding = 0.0f) const {
			drawList->AddRect(P(x0, y0), P(x1, y1), color, Len(rounding), ImDrawFlags_None, stroke);
		}

		void RectFilled(float x0, float y0, float x1, float y1, float rounding = 0.0f) const {
			drawList->AddRectFilled(P(x0, y0), P(x1, y1), color, Len(rounding));
		}

		void Circle(float x, float y, float radius) const {
			drawList->AddCircle(P(x, y), Len(radius), color, 0, stroke);
		}

		void Dot(float x, float y, float radius) const {
			drawList->AddCircleFilled(P(x, y), Len(radius), color);
		}

		void Arc(float x, float y, float radius, float fromTurn, float toTurn) const {
			drawList->PathArcTo(P(x, y), Len(radius), fromTurn * kPi, toTurn * kPi);
			drawList->PathStroke(color, ImDrawFlags_None, stroke);
		}

		// an arc with an arrowhead at its end, pointing the way the arc runs
		void ArcArrow(float x, float y, float radius, float fromTurn, float toTurn) const {
			Arc(x, y, radius, fromTurn, toTurn);
			const float angle = toTurn * kPi;
			const float direction = toTurn > fromTurn ? 1.0f : -1.0f;
			const ImVec2 radial(std::cos(angle), std::sin(angle));
			const ImVec2 tangent(-radial.y * direction, radial.x * direction);
			const ImVec2 end(x + radial.x * radius, y + radial.y * radius);
			const float head = 0.15f;
			Fill({ImVec2(end.x + radial.x * head, end.y + radial.y * head),
				  ImVec2(end.x - radial.x * head, end.y - radial.y * head),
				  ImVec2(end.x + tangent.x * head * 1.3f, end.y + tangent.y * head * 1.3f)});
		}

		// the loudspeaker shared by the mute glyphs: a box and its cone
		void SpeakerBody() const {
			RectFilled(-0.40f, -0.12f, -0.22f, 0.12f);
			Fill({ImVec2(-0.22f, -0.12f), ImVec2(0.0f, -0.33f), ImVec2(0.0f, 0.33f), ImVec2(-0.22f, 0.12f)});
		}
	};

} // namespace

namespace Icons {

	void Draw(ImDrawList* drawList, Icon icon, const ImVec2& center, float size, ImU32 color) {
		if (!drawList || size <= 0.0f || (color & IM_COL32_A_MASK) == 0)
			return;

		// centered on a pixel's middle, so a one-pixel stroke through the center lands on
		// one column of pixels instead of smearing across two
		const ImVec2 snapped(std::floor(center.x) + 0.5f, std::floor(center.y) + 0.5f);
		const Glyph g{drawList, snapped, size, color, std::max(1.0f, size * 0.09f)};

		switch (icon) {
		// ---- transport ----
		case Icon::Play:
			// its weight sits on the flat back edge, so it is drawn right of the box's
			// center to read as centered
			g.Fill({ImVec2(-0.28f, -0.36f), ImVec2(0.38f, 0.0f), ImVec2(-0.28f, 0.36f)});
			break;
		case Icon::Pause:
			g.RectFilled(-0.30f, -0.34f, -0.09f, 0.34f);
			g.RectFilled(0.09f, -0.34f, 0.30f, 0.34f);
			break;
		case Icon::Stop:
			g.RectFilled(-0.29f, -0.29f, 0.29f, 0.29f);
			break;
		case Icon::Loop: {
			// two rounded runs chasing each other around a flat loop
			const float radius = 0.13f;
			drawList->PathLineTo(g.P(-0.38f, 0.07f));
			drawList->PathArcTo(g.P(-0.38f + radius, -0.20f + radius), g.Len(radius), kPi, kPi * 1.5f);
			drawList->PathLineTo(g.P(0.14f, -0.20f));
			drawList->PathStroke(color, ImDrawFlags_None, g.stroke);
			g.Fill({ImVec2(0.12f, -0.33f), ImVec2(0.36f, -0.20f), ImVec2(0.12f, -0.07f)});

			drawList->PathLineTo(g.P(0.38f, -0.07f));
			drawList->PathArcTo(g.P(0.38f - radius, 0.20f - radius), g.Len(radius), 0.0f, kPi * 0.5f);
			drawList->PathLineTo(g.P(-0.14f, 0.20f));
			drawList->PathStroke(color, ImDrawFlags_None, g.stroke);
			g.Fill({ImVec2(-0.12f, 0.07f), ImVec2(-0.12f, 0.33f), ImVec2(-0.36f, 0.20f)});
			break;
		}
		case Icon::Follow:
			// a playhead - its flag and its line - with the view running after it
			g.Fill({ImVec2(-0.42f, -0.40f), ImVec2(-0.10f, -0.40f), ImVec2(-0.26f, -0.16f)});
			g.Line(-0.26f, -0.20f, -0.26f, 0.40f);
			g.Line(-0.06f, 0.10f, 0.22f, 0.10f, 1.2f);
			g.Fill({ImVec2(0.16f, -0.08f), ImVec2(0.42f, 0.10f), ImVec2(0.16f, 0.28f)});
			break;
		case Icon::Keyboard:
			// three white keys and the two black ones between them
			g.Rect(-0.42f, -0.30f, 0.42f, 0.30f, 0.04f);
			g.Line(-0.14f, 0.0f, -0.14f, 0.30f);
			g.Line(0.14f, 0.0f, 0.14f, 0.30f);
			g.RectFilled(-0.22f, -0.30f, -0.06f, 0.04f);
			g.RectFilled(0.06f, -0.30f, 0.22f, 0.04f);
			break;
		case Icon::Automation:
			// a breakpoint envelope, its two inner nodes marked
			g.Polyline({ImVec2(-0.42f, 0.24f), ImVec2(-0.12f, -0.20f), ImVec2(0.12f, 0.12f), ImVec2(0.42f, -0.18f)});
			g.Dot(-0.12f, -0.20f, 0.09f);
			g.Dot(0.12f, 0.12f, 0.09f);
			break;

		// ---- track toggles ----
		case Icon::Speaker:
			g.SpeakerBody();
			g.Arc(0.0f, 0.0f, 0.17f, -0.28f, 0.28f);
			g.Arc(0.0f, 0.0f, 0.32f, -0.28f, 0.28f);
			break;
		case Icon::SpeakerOff:
			g.SpeakerBody();
			g.Line(0.14f, -0.14f, 0.40f, 0.14f);
			g.Line(0.14f, 0.14f, 0.40f, -0.14f);
			break;
		case Icon::Headphones:
			g.Arc(0.0f, 0.06f, 0.33f, 1.0f, 2.0f);
			g.RectFilled(-0.42f, 0.02f, -0.23f, 0.36f, 0.05f);
			g.RectFilled(0.23f, 0.02f, 0.42f, 0.36f, 0.05f);
			break;

		// ---- generic actions ----
		case Icon::Close:
			g.Line(-0.27f, -0.27f, 0.27f, 0.27f);
			g.Line(-0.27f, 0.27f, 0.27f, -0.27f);
			break;
		case Icon::Plus:
			g.Line(-0.32f, 0.0f, 0.32f, 0.0f);
			g.Line(0.0f, -0.32f, 0.0f, 0.32f);
			break;
		case Icon::Minus:
			g.Line(-0.32f, 0.0f, 0.32f, 0.0f);
			break;
		case Icon::More:
			g.Dot(-0.28f, 0.0f, 0.075f);
			g.Dot(0.0f, 0.0f, 0.075f);
			g.Dot(0.28f, 0.0f, 0.075f);
			break;
		case Icon::Reset:
			g.ArcArrow(0.0f, 0.0f, 0.30f, -0.70f, 0.95f);
			break;
		case Icon::Dice:
			g.Rect(-0.36f, -0.36f, 0.36f, 0.36f, 0.10f);
			g.Dot(-0.17f, -0.17f, 0.065f);
			g.Dot(0.17f, -0.17f, 0.065f);
			g.Dot(0.0f, 0.0f, 0.065f);
			g.Dot(-0.17f, 0.17f, 0.065f);
			g.Dot(0.17f, 0.17f, 0.065f);
			break;
		case Icon::Search:
			g.Circle(-0.07f, -0.07f, 0.25f);
			g.Line(0.11f, 0.11f, 0.36f, 0.36f, 1.4f);
			break;

		// ---- direction ----
		case Icon::ChevronLeft:
			g.Polyline({ImVec2(0.12f, -0.28f), ImVec2(-0.14f, 0.0f), ImVec2(0.12f, 0.28f)});
			break;
		case Icon::ChevronRight:
			g.Polyline({ImVec2(-0.12f, -0.28f), ImVec2(0.14f, 0.0f), ImVec2(-0.12f, 0.28f)});
			break;
		case Icon::ChevronUp:
			g.Polyline({ImVec2(-0.28f, 0.12f), ImVec2(0.0f, -0.14f), ImVec2(0.28f, 0.12f)});
			break;
		case Icon::ChevronDown:
			g.Polyline({ImVec2(-0.28f, -0.12f), ImVec2(0.0f, 0.14f), ImVec2(0.28f, -0.12f)});
			break;
		case Icon::CaretRight:
			g.Fill({ImVec2(-0.13f, -0.25f), ImVec2(0.20f, 0.0f), ImVec2(-0.13f, 0.25f)});
			break;
		case Icon::CaretDown:
			g.Fill({ImVec2(-0.25f, -0.13f), ImVec2(0.25f, -0.13f), ImVec2(0.0f, 0.20f)});
			break;

		// ---- library and files ----
		case Icon::Folder:
			g.Polyline({ImVec2(-0.42f, 0.32f), ImVec2(-0.42f, -0.30f), ImVec2(-0.12f, -0.30f), ImVec2(-0.02f, -0.19f),
						ImVec2(0.42f, -0.19f), ImVec2(0.42f, 0.32f), ImVec2(-0.42f, 0.32f)});
			g.Line(-0.42f, -0.07f, 0.42f, -0.07f);
			break;
		case Icon::AudioFile: {
			// a waveform's bars, loud in the middle
			const float heights[5] = {0.14f, 0.28f, 0.42f, 0.24f, 0.12f};
			for (int bar = 0; bar < 5; ++bar) {
				const float x = -0.34f + 0.17f * (float)bar;
				g.Line(x, -heights[bar], x, heights[bar], 1.15f);
			}
			break;
		}
		case Icon::MIDIFile:
			// notes as a piano roll draws them
			g.RectFilled(-0.40f, -0.30f, -0.06f, -0.16f, 0.03f);
			g.RectFilled(-0.18f, -0.07f, 0.20f, 0.07f, 0.03f);
			g.RectFilled(0.06f, 0.16f, 0.40f, 0.30f, 0.03f);
			break;
		case Icon::ProjectFile:
			g.Polyline({ImVec2(0.10f, -0.40f), ImVec2(-0.32f, -0.40f), ImVec2(-0.32f, 0.40f), ImVec2(0.32f, 0.40f),
						ImVec2(0.32f, -0.18f), ImVec2(0.10f, -0.40f), ImVec2(0.10f, -0.18f), ImVec2(0.32f, -0.18f)});
			g.Line(-0.16f, 0.06f, 0.16f, 0.06f);
			g.Line(-0.16f, 0.22f, 0.16f, 0.22f);
			break;
		case Icon::Plug:
			g.Line(-0.12f, -0.42f, -0.12f, -0.20f);
			g.Line(0.12f, -0.42f, 0.12f, -0.20f);
			g.RectFilled(-0.26f, -0.20f, 0.26f, 0.12f, 0.06f);
			g.Line(0.0f, 0.12f, 0.0f, 0.42f);
			break;
		case Icon::Device:
			// a device's face: two dials over a fader
			g.Rect(-0.38f, -0.32f, 0.38f, 0.32f, 0.06f);
			g.Dot(-0.15f, -0.07f, 0.09f);
			g.Dot(0.15f, -0.07f, 0.09f);
			g.Line(-0.20f, 0.16f, 0.20f, 0.16f);
			break;
		case Icon::Rack:
			g.Rect(-0.38f, -0.38f, 0.38f, -0.14f, 0.04f);
			g.Rect(-0.38f, -0.12f, 0.38f, 0.12f, 0.04f);
			g.Rect(-0.38f, 0.14f, 0.38f, 0.38f, 0.04f);
			break;

		// ---- device panels ----
		case Icon::Window:
			// the plugin's own editor: a window with a title bar
			g.Rect(-0.40f, -0.32f, 0.40f, 0.32f, 0.04f);
			g.RectFilled(-0.40f, -0.32f, 0.40f, -0.14f, 0.04f);
			break;
		case Icon::Knob: {
			g.Circle(0.0f, 0.0f, 0.32f);
			const float angle = -0.30f * kPi;
			g.Line(0.0f, 0.0f, std::cos(angle) * 0.32f, std::sin(angle) * 0.32f);
			break;
		}
		case Icon::List:
			for (int row = 0; row < 3; ++row) {
				const float y = -0.24f + 0.24f * (float)row;
				g.Dot(-0.32f, y, 0.06f);
				g.Line(-0.16f, y, 0.38f, y);
			}
			break;
		case Icon::Devices:
			g.Rect(-0.42f, -0.30f, -0.04f, 0.30f, 0.04f);
			g.Rect(0.04f, -0.30f, 0.42f, 0.30f, 0.04f);
			break;
		case Icon::Target:
			g.Circle(0.0f, 0.0f, 0.26f);
			g.Dot(0.0f, 0.0f, 0.07f);
			g.Line(0.0f, -0.44f, 0.0f, -0.30f);
			g.Line(0.0f, 0.30f, 0.0f, 0.44f);
			g.Line(-0.44f, 0.0f, -0.30f, 0.0f);
			g.Line(0.30f, 0.0f, 0.44f, 0.0f);
			break;
		case Icon::Clip:
			g.Rect(-0.40f, -0.28f, 0.40f, 0.28f, 0.04f);
			g.RectFilled(-0.40f, -0.28f, 0.40f, -0.10f, 0.04f);
			break;
		case Icon::Note: {
			// an eighth note: head, stem and flag
			const ImVec2 head(-0.12f, 0.22f);
			g.Dot(head.x, head.y, 0.16f);
			g.Line(head.x + 0.15f, head.y, head.x + 0.15f, -0.32f);
			drawList->AddBezierQuadratic(g.P(head.x + 0.15f, -0.32f), g.P(0.30f, -0.18f), g.P(head.x + 0.21f, 0.02f), color, g.stroke);
			break;
		}
		case Icon::Phi:
			g.Circle(0.0f, 0.0f, 0.24f);
			g.Line(0.0f, -0.40f, 0.0f, 0.40f);
			break;
		case Icon::Spin:
			// an arrow chasing its own tail
			g.ArcArrow(0.0f, 0.0f, 0.30f, 0.35f, 1.95f);
			break;

		case Icon::Count:
			break;
		}
	}

} // namespace Icons
