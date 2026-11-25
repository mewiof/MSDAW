#include "PrecompHeader.h"
#include "TransportView.h"
#include "Project.h"
#include "Transport.h"
#include "GridScale.h"
#include "Parameter.h"
#include "Theme.h"
#include "Widgets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

	// a thin upright rule between two groups of controls on the bar
	void VerticalRule(ImU32 color) {
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float height = ImGui::GetFrameHeight();
		ImGui::Dummy(ImVec2(1.0f, height));
		ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x, pos.y + height * 0.15f), ImVec2(pos.x, pos.y + height * 0.85f), color);
	}

	// a field's name, set small and muted beside it: the value is what is read, the name
	// only says which one it is
	void FieldCaption(const char* caption) {
		Widgets::PushSmallFont();
		const float rowHeight = ImGui::GetFrameHeight();
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 size = ImGui::CalcTextSize(caption);
		ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x, pos.y + (rowHeight - size.y) * 0.5f),
											ImGui::GetColorU32(Theme::Instance().textMuted), caption);
		ImGui::Dummy(ImVec2(size.x, rowHeight));
		ImGui::PopFont();
	}

} // namespace

void TransportView::Render(const ImVec2& pos, float width, float height) {
	const Theme& th = Theme::Instance();
	const ImGuiStyle& style = ImGui::GetStyle();
	const float scale = mContext.state.mainScale;
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(width, height));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, th.bgHeader);
	ImGui::Begin("Transport", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
	ImGui::PopStyleColor();

	// the bar holds one row of controls, centered top to bottom rather than hung off the
	// window padding, which left a wider strip under the row than over it
	ImGui::SetCursorPosY(std::max((height - ImGui::GetFrameHeight()) * 0.5f, 0.0f));

	Project* project = mContext.GetProject();
	Transport* transport = project ? &project->GetTransport() : nullptr;

	// the transport's buttons are glyphs a little wider than they are tall, the shape the
	// reference product gives them; the toggles beside them are square
	const float frameHeight = ImGui::GetFrameHeight();
	const ImVec2 transportButton(std::floor(frameHeight * 1.3f), frameHeight);
	const ImVec2 toggleButton(frameHeight, frameHeight);

	if (transport) {
		bool isPlaying = transport->IsPlaying();
		// while it runs, the button offers the pause it would do, lit in the accent so the
		// bar says the transport is going even with the arrangement scrolled away
		bool playPressed = isPlaying
							   ? Widgets::IconToggle("##PlayPause", Icon::Pause, true, transportButton, th.accent, Widgets::Frame::Plain, "Pause (Space)")
							   : Widgets::IconButton("##PlayPause", Icon::Play, transportButton, Widgets::Frame::Plain, "Play from the marker (Space)");
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
		ImGui::SameLine(0.0f, 0.0f);
		if (Widgets::IconButton("##Stop", Icon::Stop, transportButton, Widgets::Frame::Plain, "Stop and return to the start"))
			transport->Stop(); // resets to 0
		ImGui::SameLine();

		// loop toggle
		bool isLooping = transport->IsLoopEnabled();
		if (Widgets::IconToggle("##Loop", Icon::Loop, isLooping, toggleButton, th.accent, Widgets::Frame::Plain,
								"Loop the selected time range"))
			transport->SetLoopEnabled(!isLooping);

		ImGui::SameLine(0.0f, 2.0f * scale);

		// follow toggle
		bool follow = mContext.state.followPlayback;
		if (Widgets::IconToggle("##Follow", Icon::Follow, follow, toggleButton, th.accent, Widgets::Frame::Plain,
								"Follow: keep the playhead in view while playing"))
			mContext.state.followPlayback = !follow;

		ImGui::SameLine();
		VerticalRule(th.border);
		ImGui::SameLine();

		// a recessed read-only display of a fixed width, digits right-aligned like a counter,
		// so everything after it on the bar stays put as the seconds tick past another digit.
		// unlike the BPM box beside it, it has no outline: nothing here takes input
		FieldCaption("Time");
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		char timeText[32];
		snprintf(timeText, sizeof(timeText), "%.2f", (double)transport->GetPosition() / transport->GetSampleRate());
		const float timeTextWidth = ImGui::CalcTextSize(timeText).x;
		const ImVec2 timeSize(std::max(ImGui::CalcTextSize("0000.00").x, timeTextWidth) + style.FramePadding.x * 2.0f, frameHeight);
		const ImVec2 timePos = ImGui::GetCursorScreenPos();
		ImGui::Dummy(timeSize);
		ImDrawList* timeDrawList = ImGui::GetWindowDrawList();
		timeDrawList->AddRectFilled(timePos, ImVec2(timePos.x + timeSize.x, timePos.y + timeSize.y), th.inputBg, style.FrameRounding);
		timeDrawList->AddText(ImVec2(timePos.x + timeSize.x - style.FramePadding.x - timeTextWidth, timePos.y + style.FramePadding.y), th.text, timeText);
		ImGui::SameLine();

		// bpm - reuse the master track's BPM parameter so tempo edits are undoable and
		// round-trip through the transport (Project::ProcessBlock syncs mBpmParam->value)
		FieldCaption("BPM");
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		if (std::shared_ptr<Track> master = project->GetMasterTrack())
			if (Parameter* bpmParam = master->GetBpmParameter())
				bpmParam->DrawCompact(58 * scale, "%.2f");

		ImGui::SameLine();
		FieldCaption("Grid");
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);

		const bool gridAuto = mContext.state.timelineGridAuto;
		if (gridAuto) {
			// the fields are what the grid falls back to, not what it is doing: show
			// the division the zoom actually landed on, and take the fields out of play
			ImGui::BeginDisabled();
			ImGui::AlignTextToFramePadding();
			const double cell = mContext.state.timelineGrid;
			if (cell >= 1.0)
				ImGui::Text("%g / 1", cell);
			else
				ImGui::Text("1 / %g", 1.0 / std::max(cell, 0.000001));
			ImGui::EndDisabled();
		} else {
			mGridNumParam->DrawCompact(30 * scale, "%.0f");
			ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("/");
			ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
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
		if (Widgets::TextToggle("Auto##AdaptiveGrid", gridAuto, ImVec2(0.0f, frameHeight), 0,
								"Let the grid follow the zoom, in the timeline and the piano roll"))
			mContext.state.timelineGridAuto = !gridAuto;

		// computer MIDI keyboard toggle
		ImGui::SameLine();
		VerticalRule(th.border);
		ImGui::SameLine();

		bool mIDIKey = mContext.state.isComputerMIDIKeyboardEnabled;
		if (Widgets::IconToggle("##ComputerKeyboard", Icon::Keyboard, mIDIKey, toggleButton, th.accent, Widgets::Frame::Plain))
			mContext.state.isComputerMIDIKeyboardEnabled = !mIDIKey;
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Computer MIDI Keyboard (M)\nOctave: %d (Z/X)\nVelocity: %d (C/V)",
							  mContext.state.mIDIOctave, mContext.state.mIDIVelocity);
		}

		// jump to the automation lane of the last edited knob/parameter
		ImGui::SameLine(0.0f, 2.0f * scale);
		Parameter* lastParam = Parameter::GetLastTouchedParameter();
		ImGui::BeginDisabled(lastParam == nullptr);
		if (Widgets::IconButton("##ShowAutomation", Icon::Automation, toggleButton, Widgets::Frame::Plain) && lastParam)
			Parameter::RequestAutomation(lastParam);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			if (lastParam)
				ImGui::SetTooltip("Show automation for last edited parameter:\n%s", lastParam->name.c_str());
			else
				ImGui::SetTooltip("Show automation for the last edited parameter\n(edit a knob or slider first)");
		}
	}

	// the stream state is pinned to the bar's right edge, lined up under the cpu/ram readout
	// in the menu bar: a lamp and a caption, lit green while the engine runs. a window too
	// narrow for that leaves it just after the controls
	const bool streamRunning = mContext.engine.IsStreamRunning();
	const char* streamLabel = "Audio";
	Widgets::PushSmallFont();
	const float lampDiameter = std::floor(ImGui::GetTextLineHeight() * 0.55f);
	const float statusWidth = lampDiameter + style.ItemInnerSpacing.x + ImGui::CalcTextSize(streamLabel).x;
	const float streamX = ImGui::GetWindowWidth() - style.WindowPadding.x - statusWidth;
	const float afterControlsX = transport ? ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + style.ItemSpacing.x : 0.0f;
	ImGui::SameLine(std::max(streamX, afterControlsX));
	const ImVec2 statusPos = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(statusWidth, frameHeight));
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(streamRunning ? "Audio engine running" : "Audio engine stopped: no output device");
	const float middleY = statusPos.y + frameHeight * 0.5f;
	ImDrawList* statusDrawList = ImGui::GetWindowDrawList();
	statusDrawList->AddCircleFilled(ImVec2(statusPos.x + lampDiameter * 0.5f, middleY), lampDiameter * 0.5f,
									streamRunning ? th.success : th.danger);
	statusDrawList->AddText(ImVec2(statusPos.x + lampDiameter + style.ItemInnerSpacing.x, middleY - ImGui::GetTextLineHeight() * 0.5f),
							th.textMuted, streamLabel);
	ImGui::PopFont();
	ImGui::End();
}
