#include "PrecompHeader.h"
#include "ClipView.h"
#include "Clips/AudioClip.h"
#include "Clips/MIDIClip.h"
#include "Undo/Actions.h"
#include <string>
#include <algorithm>
#include <memory>
#include <mutex>

void ClipView::Render(const ImVec2& pos, float width, float height) {
	ImVec2 defaultPadding = ImGui::GetStyle().WindowPadding;
	// prevent double-padding issues with full-size children
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(width, height));
	ImGui::Begin("Clip View", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

	// restore padding for content
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, defaultPadding);
	ImGui::BeginChild("ClipContent", ImVec2(width, height), false, ImGuiWindowFlags_None);
	ImGui::PopStyleVar();

	auto clip = mContext.state.selectedClip;

	if (!clip) {
		float txtW = ImGui::CalcTextSize("No Clip Selected").x;
		ImGui::SetCursorPos(ImVec2(width * 0.5f - txtW * 0.5f, height * 0.5f - 10.0f));
		ImGui::TextDisabled("No Clip Selected");
		ImGui::EndChild();
		ImGui::End();
		ImGui::PopStyleVar();
		return;
	}

	// basic info header
	ImGui::Text("Clip: %s", clip->GetName().c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("(%.2f beats)", clip->GetDuration());
	ImGui::SameLine();

	// activation toggle. the undo step snapshots the owning track, so it needs the
	// selected track rather than the clip alone
	bool clipEnabled = clip->IsEnabled();
	if (ImGui::Checkbox("Active", &clipEnabled)) {
		Project* clipProject = mContext.GetProject();
		std::shared_ptr<Track> ownerTrack = nullptr;
		if (clipProject) {
			auto& tracks = clipProject->GetTracks();
			int idx = mContext.state.selectedTrackIndex;
			if (idx >= 0 && idx < (int)tracks.size())
				ownerTrack = tracks[idx];
		}
		ToggleClipEnabled(clipProject, mContext.undoManager, ownerTrack, clip);
	}
	ImGui::Separator();

	if (auto ac = std::dynamic_pointer_cast<AudioClip>(clip)) {
		Project* project = mContext.GetProject();
		double projectBpm = project ? project->GetTransport().GetBpm() : 120.0;

		// all warp/pitch fields are read by the audio thread, so every mutation runs
		// under the project lock; the undo step records the before -> current diff
		auto locked = [&](auto&& fn) {
			if (project) {
				std::lock_guard<std::mutex> lock(project->GetMutex());
				fn();
			} else {
				fn();
			}
		};
		auto pushUndo = [&](const AudioClipWarpState& before, const char* name) {
			if (project)
				mContext.undoManager.Push(std::make_unique<AudioClipWarpAction>(project, ac, before, ac->CaptureWarpState(), name));
		};
		// drag widgets fire every frame; snapshot on activation, commit one step on release
		auto beginDrag = [&]() {
			if (ImGui::IsItemActivated()) {
				mWarpGestureBefore = ac->CaptureWarpState();
				mWarpGestureClip = ac;
				mWarpGestureActive = true;
			}
		};
		auto endDrag = [&](const char* name) {
			if (ImGui::IsItemDeactivatedAfterEdit() && mWarpGestureActive && mWarpGestureClip == ac) {
				pushUndo(mWarpGestureBefore, name);
			}
			if (ImGui::IsItemDeactivated()) {
				mWarpGestureActive = false;
				mWarpGestureClip.reset();
			}
		};

		// audio clip controls
		ImGui::Columns(3, "AudioClipCols", false);

		// column 1: warping
		{
			ImGui::Text("Warping");
			ImGui::Dummy(ImVec2(0, 5));

			bool warping = ac->IsWarpingEnabled();
			if (ImGui::Checkbox("Warp", &warping)) {
				AudioClipWarpState before = ac->CaptureWarpState();
				locked([&]() {
					ac->SetWarpingEnabled(warping);
					// enabling warp anchors the clip to the grid at the current tempo:
					// segment bpm := project bpm makes the warp ratio exactly 1, so there
					// is no speed/pitch jump at that instant
					if (warping)
						ac->SetSegmentBpm(projectBpm);
					ac->ValidateDuration(projectBpm);
				});
				pushUndo(before, warping ? "Enable warp" : "Disable warp");
			}

			if (warping) {
				const char* warpModes[] = {"Beats", "Tones", "Texture", "Re-Pitch", "Complex", "Complex Pro"};
				int currentMode = (int)ac->GetWarpMode();
				ImGui::SetNextItemWidth(100);
				if (ImGui::Combo("Mode", &currentMode, warpModes, IM_ARRAYSIZE(warpModes))) {
					if (currentMode >= 0 && currentMode <= 5) {
						AudioClipWarpState before = ac->CaptureWarpState();
						locked([&]() { ac->SetWarpMode((WarpMode)currentMode); });
						pushUndo(before, "Change warp mode");
					}
				}

				double bpm = ac->GetSegmentBpm();
				float fBpm = (float)bpm;
				ImGui::SetNextItemWidth(100);
				bool bpmEdited = ImGui::DragFloat("Seg. BPM", &fBpm, 0.1f, 20.0f, 999.0f, "%.2f");
				beginDrag();
				if (bpmEdited) {
					locked([&]() {
						ac->SetSegmentBpm((double)fBpm);
						ac->ValidateDuration(projectBpm);
					});
				}
				endDrag("Set clip BPM");

				// half/double-time: the usual fix when the detected tempo is an octave off
				if (ImGui::SmallButton("/2")) {
					AudioClipWarpState before = ac->CaptureWarpState();
					locked([&]() {
						ac->SetSegmentBpm(ac->GetSegmentBpm() * 0.5);
						ac->ValidateDuration(projectBpm);
					});
					pushUndo(before, "Halve clip BPM");
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("x2")) {
					AudioClipWarpState before = ac->CaptureWarpState();
					locked([&]() {
						ac->SetSegmentBpm(ac->GetSegmentBpm() * 2.0);
						ac->ValidateDuration(projectBpm);
					});
					pushUndo(before, "Double clip BPM");
				}

				// per-mode controls, mirroring Ableton's per-warp-mode knobs
				WarpMode mode = ac->GetWarpMode();
				if (mode == WarpMode::Beats) {
					double env = ac->GetTransientEnvelope();
					float fEnv = (float)env;
					ImGui::SetNextItemWidth(100);
					bool edited = ImGui::DragFloat("Envelope", &fEnv, 0.005f, 0.0f, 1.0f, "%.2f");
					beginDrag();
					if (edited)
						locked([&]() { ac->SetTransientEnvelope((double)fEnv); });
					endDrag("Set transient envelope");
				} else if (mode != WarpMode::RePitch) {
					// Tones/Texture/Complex/ComplexPro share a grain-size knob
					double gs = ac->GetGrainSizeMs();
					float fGs = (float)gs;
					ImGui::SetNextItemWidth(100);
					bool edited = ImGui::DragFloat("Grain Size", &fGs, 0.5f, 20.0f, 300.0f, "%.0f ms");
					beginDrag();
					if (edited)
						locked([&]() { ac->SetGrainSizeMs((double)fGs); });
					endDrag("Set grain size");

					if (mode == WarpMode::Texture) {
						double fl = ac->GetFluctuation();
						float fFl = (float)fl;
						ImGui::SetNextItemWidth(100);
						bool e2 = ImGui::DragFloat("Fluctuation", &fFl, 0.005f, 0.0f, 1.0f, "%.2f");
						beginDrag();
						if (e2)
							locked([&]() { ac->SetFluctuation((double)fFl); });
						endDrag("Set fluctuation");
					}
					if (mode == WarpMode::ComplexPro) {
						double fm = ac->GetFormants();
						float fFm = (float)fm;
						ImGui::SetNextItemWidth(100);
						bool e2 = ImGui::DragFloat("Formants", &fFm, 0.005f, 0.0f, 1.0f, "%.2f");
						beginDrag();
						if (e2)
							locked([&]() { ac->SetFormants((double)fFm); });
						endDrag("Set formants");
					}
				}

				// honest one-liner: only Re-Pitch couples tempo to pitch
				if (mode == WarpMode::RePitch)
					ImGui::TextDisabled("Tempo change shifts pitch (tape)");
				else
					ImGui::TextDisabled("Pitch preserved on tempo change");
			} else {
				ImGui::TextDisabled("Warping Disabled");
				ImGui::TextDisabled("Audio plays at native speed");
			}
		}
		ImGui::NextColumn();

		// column 2: pitch
		{
			ImGui::Text("Pitch / Transpose");
			ImGui::Dummy(ImVec2(0, 5));

			// Re-Pitch derives pitch from playback speed, so transpose is meaningless there
			// (Ableton disables it too); grey the controls out while that mode is active
			bool repitch = ac->IsWarpingEnabled() && ac->GetWarpMode() == WarpMode::RePitch;
			if (repitch)
				ImGui::TextDisabled("Disabled in Re-Pitch");
			ImGui::BeginDisabled(repitch);

			double semi = ac->GetTransposeSemitones();
			float fSemi = (float)semi;
			ImGui::SetNextItemWidth(100);
			bool semiEdited = ImGui::DragFloat("Semitones", &fSemi, 0.1f, -48.0f, 48.0f, "%.0f st");
			beginDrag();
			if (semiEdited) {
				locked([&]() {
					ac->SetTransposeSemitones((double)fSemi);
					ac->ValidateDuration(projectBpm);
				});
			}
			endDrag("Transpose clip");

			double cents = ac->GetTransposeCents();
			float fCents = (float)cents;
			ImGui::SetNextItemWidth(100);
			bool centsEdited = ImGui::DragFloat("Detune", &fCents, 0.1f, -100.0f, 100.0f, "%.0f ct");
			beginDrag();
			if (centsEdited) {
				locked([&]() {
					ac->SetTransposeCents((double)fCents);
					ac->ValidateDuration(projectBpm);
				});
			}
			endDrag("Transpose clip");

			// add a helper reset button
			if (ImGui::Button("Reset Pitch")) {
				AudioClipWarpState before = ac->CaptureWarpState();
				locked([&]() {
					ac->SetTransposeSemitones(0.0);
					ac->SetTransposeCents(0.0);
					ac->ValidateDuration(projectBpm);
				});
				pushUndo(before, "Reset pitch");
			}

			ImGui::EndDisabled();
		}
		ImGui::NextColumn();

		// column 3: file info
		{
			ImGui::TextDisabled("File Info");
			ImGui::Dummy(ImVec2(0, 5));
			ImGui::Text("Sample Rate: %.0f Hz", ac->GetSampleRate());
			ImGui::Text("Channels: %d", ac->GetNumChannels());
			double durSec = 0.0;
			if (ac->GetSampleRate() > 0)
				durSec = (double)ac->GetTotalFileFrames() / ac->GetSampleRate();
			ImGui::Text("Length: %.2f sec", durSec);
			ImGui::Text("Frames: %llu", ac->GetTotalFileFrames());
		}

		ImGui::Columns(1);
	} else if (auto mc = std::dynamic_pointer_cast<MIDIClip>(clip)) {
		// MIDI clip controls
		ImGui::Text("MIDI Properties");
		ImGui::Separator();
		ImGui::Text("Notes: %zu", mc->GetNotes().size());
		// placeholder for MIDI specific tools
		ImGui::TextDisabled("Use Piano Roll to edit notes.");
	}

	ImGui::EndChild();
	ImGui::End();
	ImGui::PopStyleVar();
}
