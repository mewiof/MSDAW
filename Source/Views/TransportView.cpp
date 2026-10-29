#include "PrecompHeader.h"
#include "TransportView.h"
#include "Project.h"
#include "Transport.h"
#include "GridScale.h"
#include "Parameter.h"
#include "Theme.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

	enum class TransportIcon {
		Play,
		Pause,
		Stop
	};

	// a transport symbol painted onto the button just drawn. the font atlas holds no
	// transport glyphs, and a typed ">" or "||" reads as text rather than as a control.
	// sized off the text height, so it scales with the labels on the buttons beside it
	void DrawTransportIcon(TransportIcon icon) {
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		const ImVec2 center(std::floor((min.x + max.x) * 0.5f), std::floor((min.y + max.y) * 0.5f));
		const float half = std::floor(ImGui::GetTextLineHeight() * 0.3f);
		const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
		ImDrawList* drawList = ImGui::GetWindowDrawList();

		switch (icon) {
		case TransportIcon::Play:
			// shifted right of its bounding box's center: centered by its box, a play
			// triangle's weight sits on its flat back edge and it reads as left of center
			drawList->AddTriangleFilled(ImVec2(center.x - half * 0.7f, center.y - half), ImVec2(center.x - half * 0.7f, center.y + half),
										ImVec2(center.x + half, center.y), color);
			break;
		case TransportIcon::Pause: {
			const float bar = std::max(std::floor(half * 0.6f), 2.0f);
			drawList->AddRectFilled(ImVec2(center.x - half * 0.8f, center.y - half), ImVec2(center.x - half * 0.8f + bar, center.y + half), color);
			drawList->AddRectFilled(ImVec2(center.x + half * 0.8f - bar, center.y - half), ImVec2(center.x + half * 0.8f, center.y + half), color);
			break;
		}
		case TransportIcon::Stop:
			drawList->AddRectFilled(ImVec2(center.x - half * 0.85f, center.y - half * 0.85f), ImVec2(center.x + half * 0.85f, center.y + half * 0.85f), color);
			break;
		}
	}

	// a thin upright rule between two groups of controls on the bar
	void VerticalRule(ImU32 color) {
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float height = ImGui::GetFrameHeight();
		ImGui::Dummy(ImVec2(1.0f, height));
		ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x, pos.y + height * 0.15f), ImVec2(pos.x, pos.y + height * 0.85f), color);
	}

} // namespace

void TransportView::Render(const ImVec2& pos, float width, float height) {
	const Theme& th = Theme::Instance();
	const ImGuiStyle& style = ImGui::GetStyle();
	const float scale = mContext.state.mainScale;
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(width, height));
	ImGui::Begin("Transport", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

	// the bar holds one row of controls, centered top to bottom rather than hung off the
	// window padding, which left a wider strip under the row than over it
	ImGui::SetCursorPosY(std::max((height - ImGui::GetFrameHeight()) * 0.5f, 0.0f));

	Project* project = mContext.GetProject();
	Transport* transport = project ? &project->GetTransport() : nullptr;

	if (transport) {
		bool isPlaying = transport->IsPlaying();
		bool playPressed = ImGui::Button("##PlayPause", ImVec2(40 * scale, 0));
		DrawTransportIcon(isPlaying ? TransportIcon::Pause : TransportIcon::Play);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(isPlaying ? "Pause (Space)" : "Play from the marker (Space)");
		if (playPressed) {
			if (isPlaying) {
				// pause logic (ableton style): stop and return to insert marker
				transport->Pause();
				double startBeat = mContext.state.selectionStart;
				int64_t startSample = (int64_t)(startBeat * (60.0 / transport->GetBpm()) * transport->GetSampleRate());
				transport->SetPosition(startSample);
			} else {
				// play logic (ableton style): play from insert marker
				double startBeat = mContext.state.selectionStart;
				int64_t startSample = (int64_t)(startBeat * (60.0 / transport->GetBpm()) * transport->GetSampleRate());
				transport->SetPosition(startSample);
				transport->Play();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("##Stop", ImVec2(40 * scale, 0)))
			transport->Stop(); // resets to 0
		DrawTransportIcon(TransportIcon::Stop);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Stop and return to the start");
		ImGui::SameLine();

		// loop toggle
		bool isLooping = transport->IsLoopEnabled();
		if (isLooping) {
			ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.accentHover);
			ImGui::PushStyleColor(ImGuiCol_Text, th.textOnAccent);
		}
		if (ImGui::Button("Loop", ImVec2(40 * scale, 0))) {
			transport->SetLoopEnabled(!isLooping);
		}
		if (isLooping)
			ImGui::PopStyleColor(3);

		ImGui::SameLine();

		// follow toggle
		bool follow = mContext.state.followPlayback;
		if (follow) {
			ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.accentHover);
			ImGui::PushStyleColor(ImGuiCol_Text, th.textOnAccent);
		}
		if (ImGui::Button("Follow", ImVec2(50 * scale, 0))) {
			mContext.state.followPlayback = !follow;
		}
		if (follow)
			ImGui::PopStyleColor(3);

		ImGui::SameLine();
		VerticalRule(th.border);
		ImGui::SameLine();

		// a recessed read-only display of a fixed width, digits right-aligned like a counter,
		// so everything after it on the bar stays put as the seconds tick past another digit.
		// unlike the BPM box beside it, it has no outline: nothing here takes input
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Time");
		ImGui::SameLine();
		char timeText[32];
		snprintf(timeText, sizeof(timeText), "%.2f", (double)transport->GetPosition() / transport->GetSampleRate());
		const float timeTextWidth = ImGui::CalcTextSize(timeText).x;
		const ImVec2 timeSize(std::max(ImGui::CalcTextSize("0000.00").x, timeTextWidth) + style.FramePadding.x * 2.0f, ImGui::GetFrameHeight());
		const ImVec2 timePos = ImGui::GetCursorScreenPos();
		ImGui::Dummy(timeSize);
		ImDrawList* timeDrawList = ImGui::GetWindowDrawList();
		timeDrawList->AddRectFilled(timePos, ImVec2(timePos.x + timeSize.x, timePos.y + timeSize.y), th.inputBg, style.FrameRounding);
		timeDrawList->AddText(ImVec2(timePos.x + timeSize.x - style.FramePadding.x - timeTextWidth, timePos.y + style.FramePadding.y), th.text, timeText);
		ImGui::SameLine();

		// bpm - reuse the master track's BPM parameter so tempo edits are undoable and
		// round-trip through the transport (Project::ProcessBlock syncs mBpmParam->value)
		ImGui::AlignTextToFramePadding();
		ImGui::Text("BPM");
		ImGui::SameLine();
		if (std::shared_ptr<Track> master = project->GetMasterTrack())
			if (Parameter* bpmParam = master->GetBpmParameter())
				bpmParam->DrawCompact(58 * scale, "%.1f");

		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::Text("Grid");
		ImGui::SameLine();

		const bool gridAuto = mContext.state.timelineGridAuto;
		if (gridAuto) {
			// the fields are what the grid falls back to, not what it is doing: show
			// the division the zoom actually landed on, and take the fields out of play
			ImGui::BeginDisabled();
			const double cell = mContext.state.timelineGrid;
			if (cell >= 1.0)
				ImGui::Text("%g / 1", cell);
			else
				ImGui::Text("1 / %g", 1.0 / std::max(cell, 0.000001));
			ImGui::EndDisabled();
		} else {
			mGridNumParam->DrawCompact(30 * scale, "%.0f");
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::Text("/");
			ImGui::SameLine();
			mGridDenParam->DrawCompact(30 * scale, "%.0f");
		}

		// derive the snap ratio from the (integer) grid fields every frame so undo/redo of
		// the fields propagates back into the timeline grid
		int gridNum = std::max(1, (int)std::lround(mGridNumParam->value));
		int gridDen = std::max(1, (int)std::lround(mGridDenParam->value));
		mContext.state.timelineGridNumerator = gridNum;
		mContext.state.timelineGridDenominator = gridDen;
		if (gridAuto) {
			// the transport draws before the timeline does, so this frame's lines and
			// this frame's snapping both come from the division decided here
			mContext.state.timelineGrid = GridScale::Adaptive(mContext.state.pixelsPerBeat, mContext.state.mainScale);
		} else {
			mContext.state.timelineGrid = (double)gridNum / (double)gridDen;
		}

		ImGui::SameLine();
		if (gridAuto) {
			ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.accent);
			ImGui::PushStyleColor(ImGuiCol_Text, th.textOnAccent);
		}
		if (ImGui::Button("Auto", ImVec2(44 * scale, 0)))
			mContext.state.timelineGridAuto = !gridAuto;
		if (gridAuto)
			ImGui::PopStyleColor(3);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Let the grid follow the zoom, in the timeline and the piano roll");

		// computer MIDI keyboard toggle
		ImGui::SameLine();
		VerticalRule(th.border);
		ImGui::SameLine();

		bool mIDIKey = mContext.state.isComputerMIDIKeyboardEnabled;
		if (mIDIKey) {
			// amber active state (ableton-like), dark text for contrast on the accent
			ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.accentHover);
			ImGui::PushStyleColor(ImGuiCol_Text, th.textOnAccent);
		}

		if (ImGui::Button("KEY", ImVec2(40 * scale, 0))) {
			mContext.state.isComputerMIDIKeyboardEnabled = !mIDIKey;
		}

		if (mIDIKey) {
			ImGui::PopStyleColor(3);
		}

		// optional: show current octave/vel on hover or beside it
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Computer MIDI Keyboard (M)\nOctave: %d (Z/X)\nVelocity: %d (C/V)",
							  mContext.state.mIDIOctave, mContext.state.mIDIVelocity);
		}

		// jump to the automation lane of the last edited knob/parameter
		ImGui::SameLine();
		Parameter* lastParam = Parameter::GetLastTouchedParameter();
		ImGui::BeginDisabled(lastParam == nullptr);
		if (ImGui::Button("Show Auto", ImVec2(80 * scale, 0)) && lastParam)
			Parameter::RequestAutomation(lastParam);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered()) {
			if (lastParam)
				ImGui::SetTooltip("Show automation for last edited parameter:\n%s", lastParam->name.c_str());
			else
				ImGui::SetTooltip("Show automation for the last edited parameter\n(edit a knob or slider first)");
		}
	}
	// the stream state is pinned to the bar's right edge, lined up under the cpu/ram readout
	// in the menu bar. a window too narrow for that leaves it just after the controls
	const bool streamRunning = mContext.engine.IsStreamRunning();
	const char* streamLabel = streamRunning ? "Audio: ON" : "Audio: OFF";
	const float streamX = ImGui::GetWindowWidth() - style.WindowPadding.x - ImGui::CalcTextSize(streamLabel).x;
	const float afterControlsX = transport ? ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + style.ItemSpacing.x : 0.0f;
	ImGui::SameLine(std::max(streamX, afterControlsX));
	ImGui::AlignTextToFramePadding();
	ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(streamRunning ? th.success : th.danger), "%s", streamLabel);
	ImGui::End();
}
