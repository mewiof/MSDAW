#include "PrecompHeader.h"
#include "PianoRollView.h"
#include "Clips/MIDIClip.h"
#include "Clips/AudioClip.h"
#include "Project.h"
#include "Undo/Actions.h"
#include "Theme.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>

// helper to check if a point is inside a rect
static bool IsPointInRect(const ImVec2& p, const ImVec2& min, const ImVec2& max) {
	return (p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y);
}

// helper to check rect overlap
static bool RectOverlap(const ImVec2& minA, const ImVec2& maxA, const ImVec2& minB, const ImVec2& maxB) {
	return (minA.x < maxB.x && maxA.x > minB.x && minA.y < maxB.y && maxA.y > minB.y);
}

static bool IsBlackKey(int noteNum) {
	int n = ((noteNum % 12) + 12) % 12;
	return (n == 1 || n == 3 || n == 6 || n == 8 || n == 10);
}

bool PianoRollView::IsNoteSelected(int index) {
	for (int i : mSelectedIndices) {
		if (i == index)
			return true;
	}
	return false;
}

void PianoRollView::SelectNote(int index, bool addToSelection) {
	if (!addToSelection) {
		mSelectedIndices.clear();
		mSelectedIndices.push_back(index);
	} else {
		// toggle
		auto it = std::find(mSelectedIndices.begin(), mSelectedIndices.end(), index);
		if (it != mSelectedIndices.end())
			mSelectedIndices.erase(it);
		else
			mSelectedIndices.push_back(index);
	}
}

void PianoRollView::StopPreview() {
	if (mLastPreviewNote != -1) {
		mContext.engine.SendMIDIEvent(0x80, mLastPreviewNote, 0);
		mLastPreviewNote = -1;
	}
}

void PianoRollView::CenterOnClip(MIDIClip* clip, float gridW, float gridH) {
	const float NOTE_HEIGHT = mNoteHeight * mContext.state.mainScale;
	const float PPB = mPixelsPerBeat * mContext.state.mainScale;

	int minNote = 127, maxNote = 0;
	double firstBeat = 0.0;
	bool any = false;
	if (clip) {
		for (const auto& n : clip->GetNotes()) {
			if (!any)
				firstBeat = n.startBeat;
			any = true;
			minNote = std::min(minNote, n.noteNumber);
			maxNote = std::max(maxNote, n.noteNumber);
			firstBeat = std::min(firstBeat, n.startBeat);
		}
	}

	// vertical: center on the pitch midpoint, or middle C when the clip is empty
	float midPitch = any ? (minNote + maxNote) * 0.5f : 60.0f;
	float contentH = 128.0f * NOTE_HEIGHT;
	float targetY = (127.0f - midPitch) * NOTE_HEIGHT - gridH * 0.5f;
	targetY = std::clamp(targetY, 0.0f, std::max(0.0f, contentH - gridH));

	// horizontal: bring the first note a little in from the left edge
	float targetX = any ? (float)(firstBeat * PPB) - gridW * 0.2f : 0.0f;
	if (targetX < 0.0f)
		targetX = 0.0f;

	mCenterTargetX = targetX;
	mCenterTargetY = targetY;
	mPendingCenter = true;
}

void PianoRollView::BeginGesture(MIDIClip* clip, const char* name) {
	if (!clip || mGestureActive)
		return;
	mGestureBefore = clip->GetNotes();
	mGestureName = name;
	mGestureActive = true;
}

void PianoRollView::EndGesture(MIDIClip* clip) {
	if (!mGestureActive)
		return;
	mGestureActive = false;
	if (!clip)
		return;

	auto clipShared = std::dynamic_pointer_cast<MIDIClip>(mContext.state.selectedClip);
	Project* project = mContext.GetProject();
	if (!clipShared || clipShared.get() != clip || !project)
		return;

	const auto& after = clip->GetNotes();
	if (after != mGestureBefore) {
		auto before = std::move(mGestureBefore);
		mContext.undoManager.Push(std::make_unique<NoteEditAction>(project, clipShared, std::move(before), after, mGestureName));
	}
	mGestureBefore.clear();
}

void PianoRollView::Render() {
	auto midiClipShared = std::dynamic_pointer_cast<MIDIClip>(mContext.state.selectedClip);
	if (!midiClipShared) {
		// the piano roll is closed for this frame: make sure a held preview note
		// does not get stuck sounding forever
		StopPreview();
		return;
	}

	MIDIClip* mIDIClip = midiClipShared.get();
	Project* project = mContext.GetProject();
	Transport* transport = project ? &project->GetTransport() : nullptr;
	ImGuiIO& io = ImGui::GetIO();
	const Theme& th = Theme::Instance();
	const float scale = mContext.state.mainScale;

	ImGui::SetNextWindowSize(ImVec2(720 * scale, 500 * scale), ImGuiCond_FirstUseEver);
	bool windowOpen = ImGui::Begin("Piano Roll", nullptr);
	if (!windowOpen) {
		StopPreview();
		ImGui::End();
		return;
	}

	// scoped project lock: locks only when a project exists, so mutations never
	// race the audio thread reading the same note vector in Track::ProcessBlock
	auto lockProject = [&]() -> std::unique_lock<std::mutex> {
		if (project)
			return std::unique_lock<std::mutex>(project->GetMutex());
		return std::unique_lock<std::mutex>();
	};

	// push one undo entry describing the difference from a pre-edit snapshot
	auto pushNoteEdit = [&](const std::vector<MIDINote>& before, const char* name) {
		if (!project)
			return;
		const auto& after = mIDIClip->GetNotesEx();
		if (after != before)
			mContext.undoManager.Push(std::make_unique<NoteEditAction>(project, midiClipShared, before, after, name));
	};

	// ---- constants (all DPI-scaled). NOTE_HEIGHT and PPB are computed after the
	// zoom block below, so a Ctrl+Wheel zoom takes full effect on the same frame
	const float KEY_WIDTH = 40.0f * scale;
	const float RULER_H = 22.0f * scale;
	const float VELO_H = 80.0f * scale;
	const float PAD = 8.0f * scale;
	const float MARQUEE_THRESHOLD = 3.0f * scale;

	// ---- toolbar ----
	ImGui::Text("Editing Clip: %s", mIDIClip->GetName().c_str());

	ImGui::SameLine();
	ImGui::Dummy(ImVec2(10 * scale, 0));
	ImGui::SameLine();
	ImGui::Text("Grid");
	ImGui::SameLine();

	int clipNum = mIDIClip->GetGridNumerator();
	int clipDen = mIDIClip->GetGridDenominator();
	bool gridChanged = false;

	ImGui::SetNextItemWidth(30 * scale);
	if (ImGui::DragInt("##PRGridNum", &clipNum, 0.2f, 1, 64))
		gridChanged = true;
	ImGui::SameLine();
	ImGui::Text("/");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(30 * scale);
	if (ImGui::DragInt("##PRGridDen", &clipDen, 0.2f, 1, 128))
		gridChanged = true;

	if (gridChanged) {
		if (clipNum < 1)
			clipNum = 1;
		if (clipDen < 1)
			clipDen = 1;
		mIDIClip->SetGrid(clipNum, clipDen);
	}

	double snapGrid = (double)clipNum / (double)clipDen;
	if (snapGrid <= 0.0)
		snapGrid = 0.25;

	ImGui::SameLine();
	ImGui::Dummy(ImVec2(10 * scale, 0));
	ImGui::SameLine();
	ImGui::Checkbox("Velocity", &mVelocityLaneOpen);
	ImGui::SameLine();
	ImGui::Checkbox("Minimap", &mMinimapEnabled);

	ImGui::SameLine();
	ImGui::Dummy(ImVec2(10 * scale, 0));
	ImGui::SameLine();
	ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(th.textMuted), "(Ctrl+A select | Del remove | Arrows move | Shift+Up/Down octave | Ctrl+Wheel zoom)");

	ImGui::Separator();

	// ---- content sizing ----
	double maxDuration = std::max(mIDIClip->GetDuration(), 4.0);
	for (const auto& n : mIDIClip->GetNotesEx()) {
		if (n.startBeat + n.durationBeats > maxDuration)
			maxDuration = n.startBeat + n.durationBeats;
	}
	double totalBeats = maxDuration + 1.0;

	// ---- pane geometry ----
	ImVec2 origin = ImGui::GetCursorScreenPos();
	ImVec2 avail = ImGui::GetContentRegionAvail();
	float availW = avail.x;
	float availH = avail.y;
	float gridW = std::max(50.0f, availW - KEY_WIDTH);
	float gridH = std::max(50.0f, availH - RULER_H - (mVelocityLaneOpen ? VELO_H : 0.0f));

	// auto-center when the selected clip changes identity
	auto curClip = std::static_pointer_cast<Clip>(midiClipShared);
	if (mLastCenteredClip.expired() || mLastCenteredClip.lock() != curClip) {
		CenterOnClip(mIDIClip, gridW, gridH);
		mLastCenteredClip = curClip;
	}

	// ---- Ctrl+Wheel zoom, resolved BEFORE the grid child so the child's content
	// size and scroll (declared just below via SetNextWindow*) reflect THIS frame's
	// zoom. previously the grid drew at the new zoom while the horizontal scrollbar
	// grab still used the previous frame's content size, so the grab jittered ----
	float prNextScrollX = -1.0f; // < 0 == leave that scroll axis untouched
	float prNextScrollY = -1.0f;
	{
		ImVec2 gridWinPos(origin.x + KEY_WIDTH, origin.y + RULER_H);
		if (mGridHoveredLast && io.MouseWheel != 0.0f && io.KeyCtrl) {
			if (io.KeyShift) {
				// vertical zoom, anchored on the row under the cursor
				float nhOld = mNoteHeight * scale;
				double rowAtMouse = (double)(io.MousePos.y - (gridWinPos.y - mScrollY)) / nhOld;
				mNoteHeight = std::clamp(mNoteHeight * std::pow(1.15f, io.MouseWheel), 6.0f, 48.0f);
				float nhNew = mNoteHeight * scale;
				prNextScrollY = std::max(0.0f, (float)(rowAtMouse * nhNew) - (io.MousePos.y - gridWinPos.y));
			} else {
				// horizontal zoom, anchored on the beat under the cursor
				float ppbOld = mPixelsPerBeat * scale;
				double beatAtMouse = (double)(io.MousePos.x - (gridWinPos.x - mScrollX)) / ppbOld;
				mPixelsPerBeat = std::clamp(mPixelsPerBeat * std::pow(1.15f, io.MouseWheel), 8.0f, 800.0f);
				float ppbNew = mPixelsPerBeat * scale;
				prNextScrollX = std::max(0.0f, (float)(beatAtMouse * ppbNew) - (io.MousePos.x - gridWinPos.x));
			}
		}
	}

	// post-zoom sizing (reflects this frame's zoom)
	const float NOTE_HEIGHT = mNoteHeight * scale;
	const float PPB = mPixelsPerBeat * scale;
	float contentW = (float)totalBeats * PPB;
	float contentH = 128.0f * NOTE_HEIGHT;

	auto& notes = mIDIClip->GetNotesEx();

	// keep selection indices valid: an external undo/redo can replace the whole
	// note vector out from under us
	mSelectedIndices.erase(std::remove_if(mSelectedIndices.begin(), mSelectedIndices.end(),
										  [&](int i) { return i < 0 || i >= (int)notes.size(); }),
						   mSelectedIndices.end());

	const ImU32 kBgColor = th.bgPanel;
	const ImU32 kDivider = th.divider;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

	// =====================================================================
	// center grid child -- the ONLY child that owns real ImGui scroll. it is
	// rendered first so the frozen ruler / keys / velocity siblings can read
	// its scroll this same frame and draw at a matching offset (zero lag)
	// =====================================================================
	ImGui::SetCursorScreenPos(ImVec2(origin.x + KEY_WIDTH, origin.y + RULER_H));
	// declare the content size (and, on a zoom, the scroll) up front so the child's
	// scrollbar is sized and positioned from this frame's zoom with zero lag
	ImGui::SetNextWindowContentSize(ImVec2(contentW, contentH));
	if (prNextScrollX >= 0.0f || prNextScrollY >= 0.0f)
		ImGui::SetNextWindowScroll(ImVec2(prNextScrollX, prNextScrollY)); // < 0 axis == untouched
	ImGui::BeginChild("prGrid", ImVec2(gridW, gridH), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoScrollWithMouse);
	{
		// consume a pending auto-center / minimap jump (takes effect next frame)
		if (mPendingCenter) {
			ImGui::SetScrollX(mCenterTargetX);
			ImGui::SetScrollY(mCenterTargetY);
			mPendingCenter = false;
		}

		mScrollX = ImGui::GetScrollX();
		mScrollY = ImGui::GetScrollY();

		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 canvas = ImGui::GetCursorScreenPos(); // top-left of content == gridPos - scroll
		ImVec2 gridWinPos = ImGui::GetWindowPos();
		ImVec2 gridWinSize = ImGui::GetWindowSize();

		ImGui::InvisibleButton("##pr_canvas", ImVec2(contentW, contentH));
		bool gridHovered = ImGui::IsItemHovered();
		mGridHoveredLast = gridHovered; // feeds next frame's pre-child zoom gate

		ImVec2 mousePos = io.MousePos;
		double mouseBeat = (double)(mousePos.x - canvas.x) / PPB;
		if (mouseBeat < 0)
			mouseBeat = 0;
		int mouseRow = (int)std::floor((mousePos.y - canvas.y) / NOTE_HEIGHT);
		int mouseNoteNum = 127 - mouseRow;

		bool isMouseClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
		bool isMouseDoubleClicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
		bool isMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);

		// ---- wheel scrolling. Ctrl(+Shift) zoom is resolved before the grid child
		// (see above); here we only handle plain / Shift scrolling. NoScrollWithMouse
		// on the child means nothing scrolls behind our back ----
		float maxScrollX = std::max(0.0f, contentW - gridW);
		float maxScrollY = std::max(0.0f, contentH - gridH);
		if (gridHovered && io.MouseWheel != 0.0f && !io.KeyCtrl) {
			if (io.KeyShift)
				ImGui::SetScrollX(std::clamp(mScrollX - io.MouseWheel * 60.0f * scale, 0.0f, maxScrollX));
			else
				ImGui::SetScrollY(std::clamp(mScrollY - io.MouseWheel * 3.0f * NOTE_HEIGHT, 0.0f, maxScrollY));
		}
		if (gridHovered && io.MouseWheelH != 0.0f && !io.KeyCtrl)
			ImGui::SetScrollX(std::clamp(mScrollX - io.MouseWheelH * 60.0f * scale, 0.0f, maxScrollX));

		// visible ranges (limit drawing to what is on screen)
		int firstRow = std::max(0, (int)std::floor(mScrollY / NOTE_HEIGHT));
		int lastRow = std::min(127, (int)std::ceil((mScrollY + gridH) / NOTE_HEIGHT));
		double startBeatVis = mScrollX / PPB;
		double endBeatVis = (mScrollX + gridW) / PPB;

		// a. row backgrounds + horizontal grid lines
		for (int row = firstRow; row <= lastRow; ++row) {
			int noteNum = 127 - row;
			float y = canvas.y + row * NOTE_HEIGHT;
			ImU32 rowColor = IsBlackKey(noteNum) ? th.rowBlack : th.rowWhite;
			dl->AddRectFilled(ImVec2(canvas.x, y), ImVec2(canvas.x + contentW, y + NOTE_HEIGHT), rowColor);
			dl->AddLine(ImVec2(canvas.x, y + NOTE_HEIGHT), ImVec2(canvas.x + contentW, y + NOTE_HEIGHT), th.divider);
		}

		// b. vertical beat / bar lines
		int bStart = std::max(0, (int)std::floor(startBeatVis));
		int bEnd = (int)std::ceil(endBeatVis);
		for (int b = bStart; b <= bEnd; ++b) {
			float x = canvas.x + (float)b * PPB;
			bool isBar = (b % 4 == 0);
			dl->AddLine(ImVec2(x, canvas.y), ImVec2(x, canvas.y + contentH), isBar ? th.gridBar : th.gridBeat);
		}
		// subdivisions
		if (snapGrid * PPB >= 10.0f && snapGrid < 1.0) {
			int iStart = std::max(0, (int)std::floor(startBeatVis / snapGrid));
			int iEnd = (int)std::ceil(endBeatVis / snapGrid);
			for (int i = iStart; i <= iEnd; ++i) {
				double b = i * snapGrid;
				if (std::abs(std::fmod(b + 0.001, 1.0)) > 0.002) {
					float x = canvas.x + (float)(b * PPB);
					dl->AddLine(ImVec2(x, canvas.y), ImVec2(x, canvas.y + contentH), th.gridSub);
				}
			}
		}

		// b2. clip-length shade: anything past the clip's end (x=0 is the clip start,
		// same space as the playhead below) is gated off at playback, so wash it darker
		// to show it lies outside the playable region. the crisp boundary line is drawn
		// on top of the notes further down so a note crossing it clearly shows the cut
		float clipEndX = canvas.x + (float)(mIDIClip->GetDuration() * PPB);
		if (clipEndX < canvas.x + contentW)
			dl->AddRectFilled(ImVec2(clipEndX, canvas.y), ImVec2(canvas.x + contentW, canvas.y + contentH), Theme::WithAlpha(th.bgDeepest, 90));

		// ---- minimap rect (computed before interaction so it can steal input) ----
		float sbw = ImGui::GetStyle().ScrollbarSize;
		float mmW = 160.0f * scale;
		float mmH = 90.0f * scale;
		ImVec2 mmMin(gridWinPos.x + gridWinSize.x - mmW - PAD - sbw, gridWinPos.y + PAD);
		ImVec2 mmMax(mmMin.x + mmW, mmMin.y + mmH);
		bool overMinimap = mMinimapEnabled && IsPointInRect(mousePos, mmMin, mmMax);

		// minimap drag re-centers the view (only when no grid gesture is active,
		// so a marquee / note drag that wanders over it does not hijack scroll)
		if (overMinimap && isMouseDown && mInteraction == InteractionMode::None) {
			float fx = std::clamp((mousePos.x - mmMin.x) / mmW, 0.0f, 1.0f);
			float fy = std::clamp((mousePos.y - mmMin.y) / mmH, 0.0f, 1.0f);
			float tx = std::clamp(fx * contentW - gridW * 0.5f, 0.0f, std::max(0.0f, contentW - gridW));
			float ty = std::clamp(fy * contentH - gridH * 0.5f, 0.0f, std::max(0.0f, contentH - gridH));
			ImGui::SetScrollX(tx);
			ImGui::SetScrollY(ty);
		}

		// ---- note interaction (skipped while the minimap has the mouse) ----
		if (gridHovered && !overMinimap) {
			// hit test (front to back for z-order)
			int hitIndex = -1;
			bool hitResizeRight = false;
			for (int i = (int)notes.size() - 1; i >= 0; --i) {
				const auto& note = notes[i];
				float nx = canvas.x + (float)(note.startBeat * PPB);
				float ny = canvas.y + ((127 - note.noteNumber) * NOTE_HEIGHT);
				float nw = (float)(note.durationBeats * PPB);
				if (mousePos.x >= nx && mousePos.x <= nx + nw && mousePos.y >= ny && mousePos.y <= ny + NOTE_HEIGHT) {
					hitIndex = i;
					if (mousePos.x >= nx + nw - 8.0f * scale)
						hitResizeRight = true;
					break;
				}
			}

			if (hitIndex != -1)
				ImGui::SetMouseCursor(hitResizeRight ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_Hand);

			if (isMouseClicked) {
				if (hitIndex != -1) {
					if (io.KeyCtrl) {
						SelectNote(hitIndex, true);
					} else if (io.KeyShift) {
						if (!IsNoteSelected(hitIndex))
							SelectNote(hitIndex, true);
					} else {
						if (!IsNoteSelected(hitIndex))
							SelectNote(hitIndex, false);
					}

					// begin a move / resize drag (one undo entry per drag)
					mInteraction = hitResizeRight ? InteractionMode::ResizingNotes : InteractionMode::MovingNotes;
					BeginGesture(mIDIClip, hitResizeRight ? "Resize notes" : "Move notes");
					mDragInitialStates.clear();
					for (int idx : mSelectedIndices) {
						if (idx >= 0 && idx < (int)notes.size()) {
							NoteDragState s;
							s.originalStart = notes[idx].startBeat;
							s.originalDuration = notes[idx].durationBeats;
							s.originalNoteNum = notes[idx].noteNumber;
							mDragInitialStates[idx] = s;
						}
					}
				} else if (isMouseDoubleClicked) {
					// create a note (immediate undo entry)
					std::vector<MIDINote> before = notes;
					MIDINote newNote;
					newNote.noteNumber = std::clamp(mouseNoteNum, 0, 127);
					newNote.startBeat = std::round(mouseBeat / snapGrid) * snapGrid;
					if (newNote.startBeat < 0)
						newNote.startBeat = 0;
					newNote.durationBeats = snapGrid;
					newNote.velocity = std::clamp(mContext.state.mIDIVelocity, 1, 127);
					{
						auto lk = lockProject();
						notes.push_back(newNote);
					}
					mSelectedIndices.clear();
					mSelectedIndices.push_back((int)notes.size() - 1);
					pushNoteEdit(before, "Add note");
				} else {
					// start a marquee selection
					if (!io.KeyCtrl && !io.KeyShift)
						mSelectedIndices.clear();
					mInteraction = InteractionMode::Selecting;
					mMarqueeStart = ImVec2(mousePos.x - canvas.x, mousePos.y - canvas.y);
					mMarqueeEnd = mMarqueeStart;
				}
			}
		}

		// ---- continue active grid drags ----
		if (isMouseDown && mInteraction == InteractionMode::Selecting) {
			mMarqueeEnd = ImVec2(mousePos.x - canvas.x, mousePos.y - canvas.y);
			ImVec2 sMin(canvas.x + std::min(mMarqueeStart.x, mMarqueeEnd.x), canvas.y + std::min(mMarqueeStart.y, mMarqueeEnd.y));
			ImVec2 sMax(canvas.x + std::max(mMarqueeStart.x, mMarqueeEnd.x), canvas.y + std::max(mMarqueeStart.y, mMarqueeEnd.y));

			if (!io.KeyCtrl && !io.KeyShift)
				mSelectedIndices.clear();
			for (int i = 0; i < (int)notes.size(); ++i) {
				const auto& note = notes[i];
				float nx = canvas.x + (float)(note.startBeat * PPB);
				float ny = canvas.y + ((127 - note.noteNumber) * NOTE_HEIGHT);
				float nw = (float)(note.durationBeats * PPB);
				if (RectOverlap(sMin, sMax, ImVec2(nx, ny), ImVec2(nx + nw, ny + NOTE_HEIGHT))) {
					if (!IsNoteSelected(i))
						mSelectedIndices.push_back(i);
				}
			}
		} else if (isMouseDown && mInteraction == InteractionMode::MovingNotes) {
			ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
			double deltaBeats = delta.x / PPB;
			int deltaSemis = -(int)(delta.y / NOTE_HEIGHT);
			auto lk = lockProject();
			for (auto& pair : mDragInitialStates) {
				int idx = pair.first;
				if (idx < 0 || idx >= (int)notes.size())
					continue;
				auto& note = notes[idx];
				double newStart = pair.second.originalStart + deltaBeats;
				newStart = std::round(newStart / snapGrid) * snapGrid;
				if (newStart < 0)
					newStart = 0;
				note.startBeat = newStart;
				note.noteNumber = std::clamp(pair.second.originalNoteNum + deltaSemis, 0, 127);
			}
		} else if (isMouseDown && mInteraction == InteractionMode::ResizingNotes) {
			ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
			double deltaBeats = delta.x / PPB;
			auto lk = lockProject();
			for (auto& pair : mDragInitialStates) {
				int idx = pair.first;
				if (idx < 0 || idx >= (int)notes.size())
					continue;
				auto& note = notes[idx];
				double newDur = pair.second.originalDuration + deltaBeats;
				newDur = std::round(newDur / snapGrid) * snapGrid;
				if (newDur < snapGrid)
					newDur = snapGrid;
				note.durationBeats = newDur;
			}
		}

		// c. notes
		for (size_t i = 0; i < notes.size(); ++i) {
			const auto& note = notes[i];
			float x = canvas.x + (float)(note.startBeat * PPB);
			float w = (float)(note.durationBeats * PPB);
			float y = canvas.y + ((127 - note.noteNumber) * NOTE_HEIGHT);
			// cull off-screen notes vertically / horizontally
			if (y + NOTE_HEIGHT < canvas.y + mScrollY || y > canvas.y + mScrollY + gridH)
				continue;
			if (x + w < canvas.x + mScrollX || x > canvas.x + mScrollX + gridW)
				continue;

			bool isSelected = IsNoteSelected((int)i);
			// tint the note body by velocity so dynamics are visible at a glance:
			// dim amber at low velocity, bright amber at high
			float vf = std::clamp(note.velocity / 127.0f, 0.0f, 1.0f);
			ImU32 fillColor = isSelected ? th.noteFillSelected : Theme::Lerp(Theme::WithAlpha(th.accentMuted, 235), th.noteFill, vf);
			ImU32 borderColor = isSelected ? th.noteBorderSelected : th.noteBorder;
			dl->AddRectFilled(ImVec2(x, y + 1), ImVec2(x + w, y + NOTE_HEIGHT - 1), fillColor, 4.0f);
			dl->AddRect(ImVec2(x, y + 1), ImVec2(x + w, y + NOTE_HEIGHT - 1), borderColor, 4.0f);
		}

		// clip-end boundary: a crisp line at the clip's playable length, drawn over the
		// notes so a note that runs past it visibly shows where playback will cut it off
		// (matching the note-off clamp in Track::Process)
		if (clipEndX >= canvas.x + mScrollX && clipEndX <= canvas.x + mScrollX + gridW)
			dl->AddLine(ImVec2(clipEndX, canvas.y), ImVec2(clipEndX, canvas.y + contentH), Theme::WithAlpha(th.borderStrong, 235), std::max(1.0f, 2.0f * scale));

		// d. marquee (only once dragged past a threshold, to avoid click flicker)
		if (mInteraction == InteractionMode::Selecting) {
			if (std::abs(mMarqueeEnd.x - mMarqueeStart.x) > MARQUEE_THRESHOLD || std::abs(mMarqueeEnd.y - mMarqueeStart.y) > MARQUEE_THRESHOLD) {
				ImVec2 mn(canvas.x + std::min(mMarqueeStart.x, mMarqueeEnd.x), canvas.y + std::min(mMarqueeStart.y, mMarqueeEnd.y));
				ImVec2 mx(canvas.x + std::max(mMarqueeStart.x, mMarqueeEnd.x), canvas.y + std::max(mMarqueeStart.y, mMarqueeEnd.y));
				dl->AddRectFilled(mn, mx, th.selectionFill);
				dl->AddRect(mn, mx, th.selectionStroke);
			}
		}

		// e. playhead
		if (transport) {
			double currentBeat = (double)transport->GetPosition() / transport->GetSampleRate() * (transport->GetBpm() / 60.0);
			double relBeat = currentBeat - mIDIClip->GetStartBeat();
			if (relBeat >= 0) {
				float phX = canvas.x + (float)(relBeat * PPB);
				dl->AddLine(ImVec2(phX, canvas.y), ImVec2(phX, canvas.y + contentH), Theme::WithAlpha(th.playhead, 200), 2.0f);
			}
		}

		// f. minimap overlay (drawn last, pinned to the visible top-right)
		if (mMinimapEnabled) {
			dl->AddRectFilled(mmMin, mmMax, th.bgOverlay, 3.0f);
			dl->AddRect(mmMin, mmMax, Theme::WithAlpha(th.textDim, 220), 3.0f);
			for (const auto& n : notes) {
				float fx = (float)(n.startBeat / totalBeats);
				float fw = (float)std::max(1.0, (n.durationBeats / totalBeats) * mmW);
				float fy = (float)((127 - n.noteNumber) / 128.0);
				float nx = mmMin.x + fx * mmW;
				float ny = mmMin.y + fy * mmH;
				dl->AddRectFilled(ImVec2(nx, ny), ImVec2(std::min(nx + fw, mmMax.x), ny + std::max(1.0f, mmH / 128.0f)), Theme::WithAlpha(th.noteFill, 220));
			}
			// viewport indicator
			float vx0 = mmMin.x + (mScrollX / contentW) * mmW;
			float vx1 = mmMin.x + ((mScrollX + gridW) / contentW) * mmW;
			float vy0 = mmMin.y + (mScrollY / contentH) * mmH;
			float vy1 = mmMin.y + ((mScrollY + gridH) / contentH) * mmH;
			dl->AddRect(ImVec2(vx0, vy0), ImVec2(std::min(vx1, mmMax.x), std::min(vy1, mmMax.y)), Theme::WithAlpha(th.text, 220));
		}
	}
	ImGui::EndChild();

	// =====================================================================
	// top-left corner cell
	// =====================================================================
	ImGui::SetCursorScreenPos(origin);
	ImGui::BeginChild("prCorner", ImVec2(KEY_WIDTH, RULER_H), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs);
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetWindowPos();
		dl->AddRectFilled(p, ImVec2(p.x + KEY_WIDTH, p.y + RULER_H), kBgColor);
	}
	ImGui::EndChild();

	// =====================================================================
	// ruler (frozen in Y, x-synced) -- click / drag sets the GLOBAL playhead
	// =====================================================================
	ImGui::SetCursorScreenPos(ImVec2(origin.x + KEY_WIDTH, origin.y));
	ImGui::BeginChild("prRuler", ImVec2(gridW, RULER_H), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs);
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 rp = ImGui::GetWindowPos();
		dl->AddRectFilled(rp, ImVec2(rp.x + gridW, rp.y + RULER_H), th.bgPanel);
		dl->AddLine(ImVec2(rp.x, rp.y + RULER_H - 1), ImVec2(rp.x + gridW, rp.y + RULER_H - 1), th.borderStrong);

		double startBeatVis = mScrollX / PPB;
		double endBeatVis = (mScrollX + gridW) / PPB;
		int bStart = std::max(0, (int)std::floor(startBeatVis));
		int bEnd = (int)std::ceil(endBeatVis);
		for (int b = bStart; b <= bEnd; ++b) {
			float x = rp.x + (float)b * PPB - mScrollX;
			bool isBar = (b % 4 == 0);
			if (isBar) {
				dl->AddLine(ImVec2(x, rp.y + 6), ImVec2(x, rp.y + RULER_H), th.textMuted);
				char buf[16];
				snprintf(buf, sizeof(buf), "%d", b / 4 + 1);
				dl->AddText(ImVec2(x + 3, rp.y + 3), th.textMuted, buf);
			} else {
				dl->AddLine(ImVec2(x, rp.y + RULER_H - 8), ImVec2(x, rp.y + RULER_H), th.textDim);
			}
		}

		// clip-end marker: mirror the grid's boundary in the ruler so the playable
		// length stays visible even when the boundary itself is scrolled off, and
		// shade the ruler past it to match the grid wash
		float clipEndRX = rp.x + (float)(mIDIClip->GetDuration() * PPB) - mScrollX;
		if (clipEndRX < rp.x + gridW) {
			float shadeX0 = std::max(rp.x, clipEndRX);
			dl->AddRectFilled(ImVec2(shadeX0, rp.y), ImVec2(rp.x + gridW, rp.y + RULER_H), Theme::WithAlpha(th.bgDeepest, 90));
		}
		if (clipEndRX >= rp.x && clipEndRX <= rp.x + gridW)
			dl->AddLine(ImVec2(clipEndRX, rp.y), ImVec2(clipEndRX, rp.y + RULER_H), Theme::WithAlpha(th.borderStrong, 235), std::max(1.0f, 2.0f * scale));

		// playhead marker
		if (transport) {
			double currentBeat = (double)transport->GetPosition() / transport->GetSampleRate() * (transport->GetBpm() / 60.0);
			double relBeat = currentBeat - mIDIClip->GetStartBeat();
			if (relBeat >= 0) {
				float x = rp.x + (float)(relBeat * PPB) - mScrollX;
				dl->AddTriangleFilled(ImVec2(x - 4, rp.y), ImVec2(x + 4, rp.y), ImVec2(x, rp.y + 8), th.playhead);
			}
		}

		ImGui::InvisibleButton("##pr_ruler_hit", ImVec2(gridW, RULER_H));
		if (ImGui::IsItemActive() && transport) {
			double relBeat = (double)(io.MousePos.x - rp.x + mScrollX) / PPB;
			if (relBeat < 0)
				relBeat = 0;
			relBeat = std::round(relBeat / snapGrid) * snapGrid;
			double absoluteBeat = mIDIClip->GetStartBeat() + relBeat;
			int64_t sample = (int64_t)(absoluteBeat * (60.0 / transport->GetBpm()) * transport->GetSampleRate());
			transport->SetPosition(sample);
			// also set the global start position: TogglePlayStop rewinds here on
			// stop and starts here on play, so the picked spot is where space plays from
			mContext.state.selectionStart = absoluteBeat;
			mContext.state.selectionEnd = absoluteBeat;
			transport->SetLoopRange(0, 0);
		}
	}
	ImGui::EndChild();

	// notes currently under the playhead during playback: their keys light up
	// like they're being pressed. reads the note vector (no mutation) same as the
	// grid render above, so no project lock is needed
	std::set<int> playingNotes;
	if (transport && transport->IsPlaying()) {
		double currentBeat = (double)transport->GetPosition() / transport->GetSampleRate() * (transport->GetBpm() / 60.0);
		double relBeat = currentBeat - mIDIClip->GetStartBeat();
		for (const auto& n : notes) {
			if (relBeat >= n.startBeat && relBeat < n.startBeat + n.durationBeats)
				playingNotes.insert(n.noteNumber);
		}
	}

	// =====================================================================
	// piano keys column (frozen in X, y-synced) -- click to preview
	// =====================================================================
	ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + RULER_H));
	ImGui::BeginChild("prKeys", ImVec2(KEY_WIDTH, gridH), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs);
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 kp = ImGui::GetWindowPos();

		int firstRow = std::max(0, (int)std::floor(mScrollY / NOTE_HEIGHT));
		int lastRow = std::min(127, (int)std::ceil((mScrollY + gridH) / NOTE_HEIGHT));
		for (int row = firstRow; row <= lastRow; ++row) {
			int noteNum = 127 - row;
			float y = kp.y + row * NOTE_HEIGHT - mScrollY;
			bool black = IsBlackKey(noteNum);
			bool preview = (mLastPreviewNote == noteNum);
			bool computerKey = (mContext.state.activeMIDINotes.find(noteNum) != mContext.state.activeMIDINotes.end());
			bool playing = (playingNotes.find(noteNum) != playingNotes.end());

			ImU32 keyColor = (preview || computerKey || playing) ? th.keyPressed : (black ? th.keyBlack : th.keyWhite);
			dl->AddRectFilled(ImVec2(kp.x, y), ImVec2(kp.x + KEY_WIDTH, y + NOTE_HEIGHT), keyColor);
			dl->AddRect(ImVec2(kp.x, y), ImVec2(kp.x + KEY_WIDTH, y + NOTE_HEIGHT), Theme::WithAlpha(th.divider, 100));
			if (noteNum % 12 == 0) {
				char buf[8];
				snprintf(buf, sizeof(buf), "C%d", (noteNum / 12) - 1);
				dl->AddText(ImVec2(kp.x + 3, y + 1), th.keyText, buf);
			}
		}

		ImGui::InvisibleButton("##pr_keys_hit", ImVec2(KEY_WIDTH, gridH));
		bool keysActive = ImGui::IsItemActive();
		if (keysActive) {
			int row = (int)std::floor((io.MousePos.y - kp.y + mScrollY) / NOTE_HEIGHT);
			int noteNum = 127 - row;
			if (noteNum >= 0 && noteNum <= 127) {
				if (noteNum != mLastPreviewNote) {
					StopPreview();
					mContext.engine.SendMIDIEvent(0x90, noteNum, 100);
					mLastPreviewNote = noteNum;
				}
			}
		} else {
			// mouse released or moved off the keys -> release preview
			StopPreview();
		}
	}
	ImGui::EndChild();

	// =====================================================================
	// velocity lane (collapsible, x-synced) -- drag bars to set velocity
	// =====================================================================
	if (mVelocityLaneOpen) {
		ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + RULER_H + gridH));
		ImGui::BeginChild("prVel", ImVec2(availW, VELO_H), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs);
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImVec2 vp = ImGui::GetWindowPos();
			dl->AddRectFilled(vp, ImVec2(vp.x + availW, vp.y + VELO_H), th.bgWindow);
			dl->AddLine(vp, ImVec2(vp.x + availW, vp.y), kDivider, 1.0f);

			// gutter label
			dl->AddText(ImVec2(vp.x + 3, vp.y + 3), th.textMuted, "Vel");
			dl->AddText(ImVec2(vp.x + 3, vp.y + VELO_H - 16), th.textDim, "0");

			float laneTop = vp.y + PAD;
			float laneBot = vp.y + VELO_H - PAD;
			float gutterX = vp.x + KEY_WIDTH;

			ImGui::InvisibleButton("##pr_vel_hit", ImVec2(availW, VELO_H));
			bool velHovered = ImGui::IsItemHovered();
			bool velDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
			ImVec2 mp = io.MousePos;

			// map a mouse-y to a velocity value
			auto velFromY = [&](float my) {
				float frac = (laneBot - my) / std::max(1.0f, laneBot - laneTop);
				return std::clamp((int)std::round(frac * 127.0f), 0, 127);
			};

			// begin a velocity drag when clicking near a note's dot (nearest in x),
			// so overlapping notes stay individually reachable
			if (velHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mp.x >= gutterX) {
				int hit = -1;
				float bestDist = 6.0f * scale;
				for (int i = 0; i < (int)notes.size(); ++i) {
					float dx = std::abs((gutterX + (float)(notes[i].startBeat * PPB) - mScrollX) - mp.x);
					if (dx < bestDist) {
						bestDist = dx;
						hit = i;
					}
				}
				if (hit != -1) {
					if (!IsNoteSelected(hit))
						SelectNote(hit, false);
					mInteraction = InteractionMode::EditingVelocity;
					BeginGesture(mIDIClip, "Edit velocity");
				}
			}

			// apply velocity to all selected notes while dragging
			if (mInteraction == InteractionMode::EditingVelocity && velDown) {
				int vel = velFromY(mp.y);
				auto lk = lockProject();
				for (int i : mSelectedIndices) {
					if (i >= 0 && i < (int)notes.size())
						notes[i].velocity = vel;
				}
			}

			// draw a thin stem + dot per note (like Ableton) so dense / overlapping
			// notes stay legible instead of fat bars covering each other
			for (size_t i = 0; i < notes.size(); ++i) {
				const auto& note = notes[i];
				float x = gutterX + (float)(note.startBeat * PPB) - mScrollX;
				if (x < gutterX - 4.0f * scale || x > vp.x + availW)
					continue;
				float velY = laneBot - (note.velocity / 127.0f) * (laneBot - laneTop);
				bool sel = IsNoteSelected((int)i);
				ImU32 stemCol = sel ? th.veloStemSelected : th.veloStem;
				ImU32 dotCol = sel ? th.veloDotSelected : th.veloDot;
				dl->AddLine(ImVec2(x, laneBot), ImVec2(x, velY), stemCol, std::max(1.0f, 1.5f * scale));
				dl->AddCircleFilled(ImVec2(x, velY), 3.5f * scale, dotCol);
			}
		}
		ImGui::EndChild();
	}

	ImGui::PopStyleVar(2);

	// draw a divider to the right of the keys column
	{
		ImDrawList* wdl = ImGui::GetWindowDrawList();
		wdl->AddLine(ImVec2(origin.x + KEY_WIDTH, origin.y + RULER_H), ImVec2(origin.x + KEY_WIDTH, origin.y + RULER_H + gridH), kDivider, 1.0f);
	}

	// =====================================================================
	// keyboard shortcuts (window scope; not while a mouse drag is in progress)
	// =====================================================================
	bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	if (focused && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
			mSelectedIndices.clear();
			for (int i = 0; i < (int)notes.size(); ++i)
				mSelectedIndices.push_back(i);
		}

		if ((ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) && !mSelectedIndices.empty()) {
			std::vector<MIDINote> before = notes;
			std::vector<int> sorted = mSelectedIndices;
			std::sort(sorted.rbegin(), sorted.rend());
			{
				auto lk = lockProject();
				for (int idx : sorted) {
					if (idx >= 0 && idx < (int)notes.size())
						notes.erase(notes.begin() + idx);
				}
			}
			mSelectedIndices.clear();
			pushNoteEdit(before, "Delete notes");
		}

		// arrow-key nudging: grid cell horizontally, semitone / octave vertically
		if (!mSelectedIndices.empty()) {
			double dBeat = 0.0;
			int dSemi = 0;
			if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
				dBeat = -snapGrid;
			if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
				dBeat = snapGrid;
			if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
				dSemi = io.KeyShift ? 12 : 1;
			if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
				dSemi = io.KeyShift ? -12 : -1;

			if (dBeat != 0.0 || dSemi != 0) {
				std::vector<MIDINote> before = notes;
				{
					auto lk = lockProject();
					for (int i : mSelectedIndices) {
						if (i < 0 || i >= (int)notes.size())
							continue;
						if (dBeat != 0.0)
							notes[i].startBeat = std::max(0.0, notes[i].startBeat + dBeat);
						if (dSemi != 0)
							notes[i].noteNumber = std::clamp(notes[i].noteNumber + dSemi, 0, 127);
					}
				}
				pushNoteEdit(before, dSemi != 0 ? "Nudge pitch" : "Nudge time");
			}
		}
	}

	// =====================================================================
	// end-of-drag: finalize gesture undo + reset interaction state
	// =====================================================================
	if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
		if (mGestureActive)
			EndGesture(mIDIClip);
		mInteraction = InteractionMode::None;
		mDragInitialStates.clear();
	}

	ImGui::End();
}
