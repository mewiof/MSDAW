#include "PrecompHeader.h"
#include "TransportView.h"
#include "Project.h"
#include "Transport.h"
#include "GridScale.h"
#include "Parameter.h"
#include "Theme.h"
#include <cmath>

void TransportView::Render(const ImVec2& pos, float width, float height) {
	const Theme& th = Theme::Instance();
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(width, height));
	ImGui::Begin("Transport", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

	Project* project = mContext.GetProject();
	Transport* transport = project ? &project->GetTransport() : nullptr;

	if (transport) {
		bool isPlaying = transport->IsPlaying();
		if (ImGui::Button(isPlaying ? " || " : " > ", ImVec2(40 * mContext.state.mainScale, 0))) {
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
		if (ImGui::Button("Stop", ImVec2(40 * mContext.state.mainScale, 0)))
			transport->Stop(); // resets to 0
		ImGui::SameLine();

		// loop toggle
		bool isLooping = transport->IsLoopEnabled();
		if (isLooping) {
			ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.accentHover);
			ImGui::PushStyleColor(ImGuiCol_Text, th.textOnAccent);
		}
		if (ImGui::Button("Loop", ImVec2(40 * mContext.state.mainScale, 0))) {
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
		if (ImGui::Button("Follow", ImVec2(50 * mContext.state.mainScale, 0))) {
			mContext.state.followPlayback = !follow;
		}
		if (follow)
			ImGui::PopStyleColor(3);

		ImGui::SameLine();
		ImGui::Text("| Time: %.2f", (double)transport->GetPosition() / transport->GetSampleRate());
		ImGui::SameLine();

		// bpm - reuse the master track's BPM parameter so tempo edits are undoable and
		// round-trip through the transport (Project::ProcessBlock syncs mBpmParam->value)
		ImGui::AlignTextToFramePadding();
		ImGui::Text("BPM");
		ImGui::SameLine();
		if (std::shared_ptr<Track> master = project->GetMasterTrack())
			if (Parameter* bpmParam = master->GetBpmParameter())
				bpmParam->DrawCompact(58 * mContext.state.mainScale, "%.1f");

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
			mGridNumParam->DrawCompact(30 * mContext.state.mainScale, "%.0f");
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::Text("/");
			ImGui::SameLine();
			mGridDenParam->DrawCompact(30 * mContext.state.mainScale, "%.0f");
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
		if (ImGui::Button("Auto", ImVec2(44 * mContext.state.mainScale, 0)))
			mContext.state.timelineGridAuto = !gridAuto;
		if (gridAuto)
			ImGui::PopStyleColor(3);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Let the grid follow the zoom, in the timeline and the piano roll");

		// computer MIDI keyboard toggle
		ImGui::SameLine();
		ImGui::Dummy(ImVec2(20, 0)); // spacer
		ImGui::SameLine();

		bool mIDIKey = mContext.state.isComputerMIDIKeyboardEnabled;
		if (mIDIKey) {
			// amber active state (ableton-like), dark text for contrast on the accent
			ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.accentHover);
			ImGui::PushStyleColor(ImGuiCol_Text, th.textOnAccent);
		}

		if (ImGui::Button("KEY", ImVec2(40 * mContext.state.mainScale, 0))) {
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
		if (ImGui::Button("Show Auto", ImVec2(80 * mContext.state.mainScale, 0)) && lastParam)
			Parameter::RequestAutomation(lastParam);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered()) {
			if (lastParam)
				ImGui::SetTooltip("Show automation for last edited parameter:\n%s", lastParam->name.c_str());
			else
				ImGui::SetTooltip("Show automation for the last edited parameter\n(edit a knob or slider first)");
		}
	}
	ImGui::SameLine(ImGui::GetWindowWidth() - 150 * mContext.state.mainScale);
	ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(mContext.engine.IsStreamRunning() ? th.success : th.danger),
					   mContext.engine.IsStreamRunning() ? "Audio: ON" : "Audio: OFF");
	ImGui::End();
}
