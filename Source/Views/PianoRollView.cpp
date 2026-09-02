#include "PrecompHeader.h"
#include "PianoRollView.h"
#include "Clips/MIDIClip.h"
#include "Clips/AudioClip.h"
#include "Project.h"
#include "Undo/Actions.h"
#include "Theme.h"
#include "Views/TimelineView/TimelineUtils.h"
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

std::vector<PianoRollView::RollClip> PianoRollView::CollectClips(double& origin) {
	std::vector<RollClip> out;
	Project* project = mContext.GetProject();
	if (!project)
		return out;

	// resolved by walking the tracks rather than the selection vector, because a clip
	// needs its owning track's color and because a clip that has left the arrangement
	// (an undo, a delete) must not keep drawing here
	for (const auto& t : project->GetTracks()) {
		for (const auto& c : t->GetClips()) {
			if (!mContext.state.IsClipSelected(c))
				continue;
			if (auto mIDIClip = std::dynamic_pointer_cast<MIDIClip>(c))
				out.push_back({mIDIClip, t->GetColor(), mIDIClip->GetStartBeat(), false});
		}
	}
	if (out.empty())
		return out;

	std::sort(out.begin(), out.end(), [](const RollClip& a, const RollClip& b) {
		return a.viewOffset < b.viewOffset;
	});

	// the view's beat 0 is the earliest selected clip's start, so the grid and the
	// ruler can stay in arrangement bars whichever clip currently has the focus
	origin = out.front().viewOffset;
	for (auto& rc : out)
		rc.viewOffset -= origin;

	// the arrangement selection may focus an audio clip (or a MIDI clip that has since
	// gone); the roll then edits the earliest MIDI clip on show rather than nothing
	auto focus = std::dynamic_pointer_cast<MIDIClip>(mContext.state.selectedClip);
	bool focusFound = false;
	for (auto& rc : out) {
		if (focus && rc.clip == focus) {
			rc.focused = true;
			focusFound = true;
		}
	}
	if (!focusFound)
		out.front().focused = true;
	return out;
}

void PianoRollView::CenterOnClip(MIDIClip* clip, double viewOffset, float gridW, float gridH) {
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

	// horizontal: bring the first note a little in from the left edge. the clip may
	// sit anywhere along a multi-clip view, so its own offset comes along
	float targetX = (float)((viewOffset + (any ? firstBeat : 0.0)) * PPB) - gridW * 0.2f;
	if (targetX < 0.0f)
		targetX = 0.0f;

	mCenterTargetX = targetX;
	mCenterTargetY = targetY;
	mPendingCenter = true;
}

void PianoRollView::BeginGesture(const std::shared_ptr<MIDIClip>& clip, const char* name) {
	if (!clip || mGestureActive)
		return;
	mGestureBefore = clip->GetNotes();
	mGestureName = name;
	mGestureActive = true;
}

void PianoRollView::EndGesture(const std::shared_ptr<MIDIClip>& clip) {
	if (!mGestureActive)
		return;
	mGestureActive = false;

	Project* project = mContext.GetProject();
	if (!clip || !project)
		return;

	const auto& after = clip->GetNotes();
	if (after != mGestureBefore) {
		auto before = std::move(mGestureBefore);
		mContext.undoManager.Push(std::make_unique<NoteEditAction>(project, clip, std::move(before), after, mGestureName));
	}
	mGestureBefore.clear();
}

void PianoRollView::Render() {
	// multi-clip editing: every MIDI clip in the arrangement selection is drawn on one
	// shared grid at the beat it occupies in the arrangement. the focused clip is the
	// only one that takes edits - clicking another one hands it the focus
	double viewOrigin = 0.0;
	std::vector<RollClip> rollClips = CollectClips(viewOrigin);
	if (rollClips.empty()) {
		// the piano roll is closed for this frame: make sure a held preview note does
		// not get stuck sounding forever, and that a pan the close interrupted does
		// not pick itself back up the next time the roll opens
		StopPreview();
		mPanning = false;
		return;
	}

	// which clip on show holds the focus, where its own beat 0 lands in the view, and
	// the arrangement beat it sits at. clip-local note times convert to view space with
	// the first and to song time with the second; the two differ by the view origin.
	// re-runnable, because a toolbar chip can move the focus part-way down this function
	// and everything after it has to be drawn from the same frame's answer
	const RollClip* focusEntry = nullptr;
	std::shared_ptr<MIDIClip> midiClipShared;
	MIDIClip* mIDIClip = nullptr;
	double focusOffset = 0.0;
	double focusStart = 0.0;
	auto resolveFocus = [&]() {
		focusEntry = &rollClips.front();
		for (const auto& rc : rollClips) {
			if (rc.focused)
				focusEntry = &rc;
		}
		midiClipShared = focusEntry->clip;
		mIDIClip = midiClipShared.get();
		focusOffset = focusEntry->viewOffset;
		focusStart = mIDIClip->GetStartBeat();
	};
	resolveFocus();

	Project* project = mContext.GetProject();
	Transport* transport = project ? &project->GetTransport() : nullptr;
	ImGuiIO& io = ImGui::GetIO();
	const Theme& th = Theme::Instance();
	const float scale = mContext.state.mainScale;

	ImGui::SetNextWindowSize(ImVec2(720 * scale, 500 * scale), ImGuiCond_FirstUseEver);
	bool windowOpen = ImGui::Begin("Piano Roll", nullptr);
	if (!windowOpen) {
		StopPreview();
		mPanning = false;
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
	// one chip per clip on show, tinted with its track color: it names what is on the
	// grid and is the way to hand the focus to another clip without leaving the roll.
	// a plain button would paint itself before reporting its click, so the chip you
	// pressed would stay dim for a frame - hence the bare hitboxes here and the paint
	// pass below, which reads the focus this frame's press already moved
	struct ChipRect {
		ImVec2 min;
		ImVec2 max;
		std::shared_ptr<MIDIClip> clip;
		ImU32 color;
		bool hovered;
	};
	std::vector<ChipRect> chipRects;
	bool focusSwitched = false;
	float chipPadX = ImGui::GetStyle().FramePadding.x;

	ImGui::AlignTextToFramePadding();
	ImGui::Text("Editing");
	for (const auto& rc : rollClips) {
		ImGui::SameLine();
		ImGui::PushID(rc.clip.get());
		ImVec2 labelSize = ImGui::CalcTextSize(rc.clip->GetName().c_str());
		// a linked clip widens its chip by the chain badge: the notes on this grid are
		// also played somewhere else, and every edit below lands there too
		float badgeSpace = rc.clip->IsSequenceShared()
							   ? TimelineUtils::LinkBadgeWidth(labelSize.y * 0.72f) + 4.0f * scale
							   : 0.0f;
		ImGui::InvisibleButton("##chip", ImVec2(labelSize.x + badgeSpace + chipPadX * 2.0f, labelSize.y + 2.0f * scale));
		if (ImGui::IsItemActivated() && !rc.focused) {
			mContext.state.selectedClip = rc.clip;
			mSelectedIndices.clear(); // note indices belong to the clip they came from
			focusSwitched = true;
		}
		chipRects.push_back({ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), rc.clip, rc.color, ImGui::IsItemHovered()});
		ImGui::PopID();
	}

	// the whole roll below is built from the focus resolved at the top of this function,
	// so a chip press has to be folded back in before any of it is measured or drawn
	if (focusSwitched) {
		rollClips = CollectClips(viewOrigin);
		resolveFocus();
	}

	{
		ImDrawList* chipDrawList = ImGui::GetWindowDrawList();
		for (const auto& chip : chipRects) {
			bool chipFocused = (mContext.state.selectedClip == chip.clip);
			ImU32 fill = chipFocused ? chip.color : Theme::WithAlpha(chip.color, chip.hovered ? 160 : 90);
			chipDrawList->AddRectFilled(chip.min, chip.max, fill, 3.0f * scale);
			chipDrawList->AddText(ImVec2(chip.min.x + chipPadX, chip.min.y + 1.0f * scale),
								  chipFocused ? th.clipText : th.textMuted, chip.clip->GetName().c_str());
			if (chip.clip->IsSequenceShared()) {
				const float badgeHeight = (chip.max.y - chip.min.y) * 0.62f;
				TimelineUtils::DrawLinkBadge(chipDrawList,
											 ImVec2(chip.max.x - chipPadX - TimelineUtils::LinkBadgeWidth(badgeHeight),
													chip.min.y + (chip.max.y - chip.min.y - badgeHeight) * 0.5f),
											 badgeHeight, th.clipLinked);
			}
		}
	}

	ImGui::SameLine();
	ImGui::Dummy(ImVec2(10 * scale, 0));
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::Text("Grid");
	ImGui::SameLine();

	// per-clip snap grid, edited with the same custom parameter widget as the timeline
	// grid (drag / type-to-enter / click-away deselect) instead of a bare DragInt. the
	// clip owns the value, so re-seed the widgets whenever the edited clip changes
	auto gridClip = std::static_pointer_cast<Clip>(midiClipShared);
	if (mLastGridClip.expired() || mLastGridClip.lock() != gridClip) {
		mGridNumParam->value = (float)mIDIClip->GetGridNumerator();
		mGridDenParam->value = (float)mIDIClip->GetGridDenominator();
		mLastGridClip = gridClip;
	}

	mGridNumParam->DrawCompact(30 * scale, "%.0f");
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::Text("/");
	ImGui::SameLine();
	mGridDenParam->DrawCompact(30 * scale, "%.0f");

	// the clip is the source of truth; write the (integer) widget values back each frame
	int clipNum = std::max(1, (int)std::lround(mGridNumParam->value));
	int clipDen = std::max(1, (int)std::lround(mGridDenParam->value));
	mIDIClip->SetGrid(clipNum, clipDen);

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
	ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(th.textMuted), "(Ctrl+A select | Del remove | Arrows move | Shift+Up/Down octave | Ctrl+Wheel zoom | click a clip to edit it)");

	ImGui::Separator();

	// ---- content sizing ----
	// the grid spans every clip on show, not just the focused one
	double maxViewBeat = 4.0;
	for (const auto& rc : rollClips) {
		maxViewBeat = std::max(maxViewBeat, rc.viewOffset + rc.clip->GetDuration());
		for (const auto& n : rc.clip->GetNotes())
			maxViewBeat = std::max(maxViewBeat, rc.viewOffset + n.startBeat + n.durationBeats);
	}
	double totalBeats = maxViewBeat + 1.0;

	// ---- pane geometry ----
	ImVec2 origin = ImGui::GetCursorScreenPos();
	ImVec2 avail = ImGui::GetContentRegionAvail();
	float availW = avail.x;
	float availH = avail.y;
	float gridW = std::max(50.0f, availW - KEY_WIDTH);
	float gridH = std::max(50.0f, availH - RULER_H - (mVelocityLaneOpen ? VELO_H : 0.0f));

	// auto-center when the focused clip changes identity, or when the view's beat 0
	// moves under it because another clip joined or left the selection
	auto curClip = std::static_pointer_cast<Clip>(midiClipShared);
	if (mLastCenteredClip.expired() || mLastCenteredClip.lock() != curClip || mLastCenterOrigin != viewOrigin) {
		CenterOnClip(mIDIClip, focusOffset, gridW, gridH);
		mLastCenteredClip = curClip;
		mLastCenterOrigin = viewOrigin;
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

	// ---- middle-button pan: hold the wheel down and drag the grid around under the
	// cursor, both axes at once. resolved here rather than inside the child for the
	// same reason the zoom is - a scroll set from in there only lands on the next
	// frame, and a view that trails the mouse by a frame reads as sticky ----
	if (mPanning)
		mPanning = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
	else if (mGridHoveredLast && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
		mPanning = true;
	if (mPanning) {
		prNextScrollX = std::max(0.0f, mScrollX - io.MouseDelta.x);
		prNextScrollY = std::max(0.0f, mScrollY - io.MouseDelta.y);
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

	// ================================================================
	// CENTER GRID
	// ================================================================
	// the ONLY child that owns real ImGui scroll. it is rendered first so the
	// frozen ruler / keys / velocity siblings can read its scroll this same
	// frame and draw at a matching offset (zero lag)
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
		double mouseViewBeat = (double)(mousePos.x - canvas.x) / PPB;
		if (mouseViewBeat < 0)
			mouseViewBeat = 0;
		// note times are stored relative to their own clip, so everything the mouse
		// says has to be brought back out of view space and into the focused clip's
		double mouseBeat = mouseViewBeat - focusOffset;

		// notes snap on the ARRANGEMENT grid - the one the vertical lines are drawn on.
		// snapping clip-locally would put the notes of a clip that does not start on a
		// grid line onto a grid of its own, invisibly offset from the lines under them
		auto snapClipBeat = [&](double clipBeat) {
			return std::round((clipBeat + focusStart) / snapGrid) * snapGrid - focusStart;
		};

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

		// b. vertical beat / bar lines, laid out on ARRANGEMENT beats rather than on
		// view beats: the view's x=0 is wherever the earliest clip on show happens to
		// start, and bar 1 has to keep meaning bar 1 of the song
		double arrStartVis = viewOrigin + startBeatVis;
		double arrEndVis = viewOrigin + endBeatVis;
		int bStart = std::max(0, (int)std::floor(arrStartVis));
		int bEnd = (int)std::ceil(arrEndVis);
		for (int b = bStart; b <= bEnd; ++b) {
			float x = canvas.x + (float)((b - viewOrigin) * PPB);
			bool isBar = (b % 4 == 0);
			dl->AddLine(ImVec2(x, canvas.y), ImVec2(x, canvas.y + contentH), isBar ? th.gridBar : th.gridBeat);
		}
		// subdivisions
		if (snapGrid * PPB >= 10.0f && snapGrid < 1.0) {
			int iStart = std::max(0, (int)std::floor(arrStartVis / snapGrid));
			int iEnd = (int)std::ceil(arrEndVis / snapGrid);
			for (int i = iStart; i <= iEnd; ++i) {
				double b = i * snapGrid;
				if (std::abs(std::fmod(b + 0.001, 1.0)) > 0.002) {
					float x = canvas.x + (float)((b - viewOrigin) * PPB);
					dl->AddLine(ImVec2(x, canvas.y), ImVec2(x, canvas.y + contentH), th.gridSub);
				}
			}
		}

		// b2. clip bands. anything outside every clip on show is gated off at playback,
		// so wash it darker to show it lies outside the playable region; the gaps
		// between clips get the same treatment as the tail past the last one. the crisp
		// boundary lines are drawn on top of the notes further down so a note crossing
		// one clearly shows the cut
		{
			double covered = 0.0;
			for (const auto& rc : rollClips) {
				double clipStart = rc.viewOffset;
				if (clipStart > covered)
					dl->AddRectFilled(ImVec2(canvas.x + (float)(covered * PPB), canvas.y),
									  ImVec2(canvas.x + (float)(clipStart * PPB), canvas.y + contentH),
									  Theme::WithAlpha(th.bgDeepest, 90));
				covered = std::max(covered, clipStart + rc.clip->GetDuration());
			}
			float tailX = canvas.x + (float)(covered * PPB);
			if (tailX < canvas.x + contentW)
				dl->AddRectFilled(ImVec2(tailX, canvas.y), ImVec2(canvas.x + contentW, canvas.y + contentH), Theme::WithAlpha(th.bgDeepest, 90));
		}

		// a clip that does not hold the focus is tinted in its own track color, so a run
		// of clips from several tracks reads as separate parts instead of one long
		// sequence. the focused one stays untinted: it is the material being edited
		for (const auto& rc : rollClips) {
			if (rc.focused)
				continue;
			float bandX0 = canvas.x + (float)(rc.viewOffset * PPB);
			float bandX1 = canvas.x + (float)((rc.viewOffset + rc.clip->GetDuration()) * PPB);
			if (bandX1 < canvas.x + mScrollX || bandX0 > canvas.x + mScrollX + gridW)
				continue;
			dl->AddRectFilled(ImVec2(bandX0, canvas.y), ImVec2(bandX1, canvas.y + contentH), Theme::WithAlpha(rc.color, 26));
		}

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
			// a click anywhere over another clip's band hands it the focus, the way
			// Ableton switches which clip of a multi-clip edit is the editable one.
			// checked before the note hit test so the notes drawn there are click
			// targets for the switch rather than for an edit that could not apply
			// where bands overlap - clips on different tracks covering the same bars -
			// the focused clip keeps priority, or its own notes would sit unreachable
			// under someone else's band. the toolbar chips are the way into those
			bool overFocusBand = (mouseViewBeat >= focusOffset && mouseViewBeat <= focusOffset + mIDIClip->GetDuration());
			std::shared_ptr<MIDIClip> focusTarget;
			for (const auto& rc : rollClips) {
				if (rc.focused || overFocusBand)
					continue;
				double bandStart = rc.viewOffset;
				double bandEnd = bandStart + rc.clip->GetDuration();
				if (mouseViewBeat >= bandStart && mouseViewBeat <= bandEnd)
					focusTarget = rc.clip;
			}
			if (focusTarget)
				ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

			// hit test (front to back for z-order)
			int hitIndex = -1;
			bool hitResizeRight = false;
			for (int i = (int)notes.size() - 1; i >= 0 && !focusTarget; --i) {
				const auto& note = notes[i];
				float nx = canvas.x + (float)((focusOffset + note.startBeat) * PPB);
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

			if (isMouseClicked && focusTarget) {
				mContext.state.selectedClip = focusTarget;
				// note indices belong to the clip they were taken from
				mSelectedIndices.clear();
			} else if (isMouseClicked) {
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
					BeginGesture(midiClipShared, hitResizeRight ? "Resize notes" : "Move notes");
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
					newNote.startBeat = snapClipBeat(mouseBeat);
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
				float nx = canvas.x + (float)((focusOffset + note.startBeat) * PPB);
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
				double newStart = snapClipBeat(pair.second.originalStart + deltaBeats);
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

		// c. notes of the clips that do not hold the focus. they are context, not
		// editable material, so they carry their own track color rather than the amber
		// note palette - which is exactly what says "this run belongs to another clip"
		for (const auto& rc : rollClips) {
			if (rc.focused)
				continue;
			for (const auto& note : rc.clip->GetNotes()) {
				float x = canvas.x + (float)((rc.viewOffset + note.startBeat) * PPB);
				float w = (float)(note.durationBeats * PPB);
				float y = canvas.y + ((127 - note.noteNumber) * NOTE_HEIGHT);
				if (y + NOTE_HEIGHT < canvas.y + mScrollY || y > canvas.y + mScrollY + gridH)
					continue;
				if (x + w < canvas.x + mScrollX || x > canvas.x + mScrollX + gridW)
					continue;
				dl->AddRectFilled(ImVec2(x, y + 1), ImVec2(x + w, y + NOTE_HEIGHT - 1), Theme::WithAlpha(rc.color, 150), 4.0f);
				dl->AddRect(ImVec2(x, y + 1), ImVec2(x + w, y + NOTE_HEIGHT - 1), Theme::WithAlpha(rc.color, 220), 4.0f);
			}
		}

		// c2. notes of the focused clip - the only ones that take edits
		for (size_t i = 0; i < notes.size(); ++i) {
			const auto& note = notes[i];
			float x = canvas.x + (float)((focusOffset + note.startBeat) * PPB);
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

		// clip boundaries: a crisp line at each clip's start and playable end, drawn over
		// the notes so a note that runs past one visibly shows where playback will cut it
		// off (matching the note-off clamp in Track::Process), and so the seam between
		// two adjacent clips stays readable
		for (const auto& rc : rollClips) {
			float edges[2] = {canvas.x + (float)(rc.viewOffset * PPB),
							  canvas.x + (float)((rc.viewOffset + rc.clip->GetDuration()) * PPB)};
			for (float edgeX : edges) {
				if (edgeX >= canvas.x + mScrollX && edgeX <= canvas.x + mScrollX + gridW)
					dl->AddLine(ImVec2(edgeX, canvas.y), ImVec2(edgeX, canvas.y + contentH), Theme::WithAlpha(th.borderStrong, 235), std::max(1.0f, 2.0f * scale));
			}
		}

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
			double relBeat = currentBeat - viewOrigin;
			if (relBeat >= 0) {
				float phX = canvas.x + (float)(relBeat * PPB);
				dl->AddLine(ImVec2(phX, canvas.y), ImVec2(phX, canvas.y + contentH), Theme::WithAlpha(th.playhead, 200), 2.0f);
			}
		}

		// f. minimap overlay (drawn last, pinned to the visible top-right)
		if (mMinimapEnabled) {
			dl->AddRectFilled(mmMin, mmMax, th.bgOverlay, 3.0f);
			dl->AddRect(mmMin, mmMax, Theme::WithAlpha(th.textDim, 220), 3.0f);
			for (const auto& rc : rollClips) {
				for (const auto& n : rc.clip->GetNotes()) {
					float fx = (float)((rc.viewOffset + n.startBeat) / totalBeats);
					float fw = (float)std::max(1.0, (n.durationBeats / totalBeats) * mmW);
					float fy = (float)((127 - n.noteNumber) / 128.0);
					float nx = mmMin.x + fx * mmW;
					float ny = mmMin.y + fy * mmH;
					ImU32 dotColor = rc.focused ? Theme::WithAlpha(th.noteFill, 220) : Theme::WithAlpha(rc.color, 200);
					dl->AddRectFilled(ImVec2(nx, ny), ImVec2(std::min(nx + fw, mmMax.x), ny + std::max(1.0f, mmH / 128.0f)), dotColor);
				}
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

	// ================================================================
	// TOP-LEFT CORNER CELL
	// ================================================================
	ImGui::SetCursorScreenPos(origin);
	ImGui::BeginChild("prCorner", ImVec2(KEY_WIDTH, RULER_H), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs);
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetWindowPos();
		dl->AddRectFilled(p, ImVec2(p.x + KEY_WIDTH, p.y + RULER_H), kBgColor);
	}
	ImGui::EndChild();

	// ================================================================
	// RULER
	// ================================================================
	// frozen in Y, x-synced; click / drag sets the GLOBAL playhead
	ImGui::SetCursorScreenPos(ImVec2(origin.x + KEY_WIDTH, origin.y));
	ImGui::BeginChild("prRuler", ImVec2(gridW, RULER_H), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs);
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 rp = ImGui::GetWindowPos();
		dl->AddRectFilled(rp, ImVec2(rp.x + gridW, rp.y + RULER_H), th.bgPanel);
		dl->AddLine(ImVec2(rp.x, rp.y + RULER_H - 1), ImVec2(rp.x + gridW, rp.y + RULER_H - 1), th.borderStrong);

		// bar numbers come from ARRANGEMENT beats, so the ruler reads the same as the
		// arrangement's own ruler whichever clip is focused
		double startBeatVis = mScrollX / PPB;
		double endBeatVis = (mScrollX + gridW) / PPB;
		int bStart = std::max(0, (int)std::floor(viewOrigin + startBeatVis));
		int bEnd = (int)std::ceil(viewOrigin + endBeatVis);
		for (int b = bStart; b <= bEnd; ++b) {
			float x = rp.x + (float)((b - viewOrigin) * PPB) - mScrollX;
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

		// wash the ruler over every stretch no clip covers, matching the grid, so the
		// playable regions stay legible even when their boundaries scroll off
		{
			double covered = 0.0;
			auto shade = [&](double from, double to) {
				float x0 = std::max(rp.x, rp.x + (float)(from * PPB) - mScrollX);
				float x1 = std::min(rp.x + gridW, rp.x + (float)(to * PPB) - mScrollX);
				if (x1 > x0)
					dl->AddRectFilled(ImVec2(x0, rp.y), ImVec2(x1, rp.y + RULER_H), Theme::WithAlpha(th.bgDeepest, 90));
			};
			for (const auto& rc : rollClips) {
				if (rc.viewOffset > covered)
					shade(covered, rc.viewOffset);
				covered = std::max(covered, rc.viewOffset + rc.clip->GetDuration());
			}
			shade(covered, (double)totalBeats);
		}

		// a name strip along the bottom of the ruler, one per clip in its track color:
		// with several clips on the grid this is what says where each one begins, ends
		// and which of them is the editable one
		float stripH = std::max(3.0f, 4.0f * scale);
		for (const auto& rc : rollClips) {
			float x0 = rp.x + (float)(rc.viewOffset * PPB) - mScrollX;
			float x1 = rp.x + (float)((rc.viewOffset + rc.clip->GetDuration()) * PPB) - mScrollX;
			if (x1 < rp.x || x0 > rp.x + gridW)
				continue;
			dl->AddRectFilled(ImVec2(std::max(x0, rp.x), rp.y + RULER_H - stripH),
							  ImVec2(std::min(x1, rp.x + gridW), rp.y + RULER_H),
							  rc.focused ? rc.color : Theme::WithAlpha(rc.color, 120));
			if (x0 >= rp.x && x0 <= rp.x + gridW)
				dl->AddLine(ImVec2(x0, rp.y), ImVec2(x0, rp.y + RULER_H), Theme::WithAlpha(th.borderStrong, 235), std::max(1.0f, 2.0f * scale));
			if (x1 >= rp.x && x1 <= rp.x + gridW)
				dl->AddLine(ImVec2(x1, rp.y), ImVec2(x1, rp.y + RULER_H), Theme::WithAlpha(th.borderStrong, 235), std::max(1.0f, 2.0f * scale));
		}

		// playhead marker
		if (transport) {
			double currentBeat = (double)transport->GetPosition() / transport->GetSampleRate() * (transport->GetBpm() / 60.0);
			double relBeat = currentBeat - viewOrigin;
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
			double absoluteBeat = std::round((viewOrigin + relBeat) / snapGrid) * snapGrid;
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
		for (const auto& rc : rollClips) {
			double relBeat = currentBeat - rc.clip->GetStartBeat();
			for (const auto& n : rc.clip->GetNotes()) {
				if (relBeat >= n.startBeat && relBeat < n.startBeat + n.durationBeats)
					playingNotes.insert(n.noteNumber);
			}
		}
	}

	// ================================================================
	// PIANO KEYS COLUMN
	// ================================================================
	// frozen in X, y-synced; click to preview
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

	// ================================================================
	// VELOCITY LANE
	// ================================================================
	// collapsible, x-synced; drag bars to set velocity
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
					float dx = std::abs((gutterX + (float)((focusOffset + notes[i].startBeat) * PPB) - mScrollX) - mp.x);
					if (dx < bestDist) {
						bestDist = dx;
						hit = i;
					}
				}
				if (hit != -1) {
					if (!IsNoteSelected(hit))
						SelectNote(hit, false);
					mInteraction = InteractionMode::EditingVelocity;
					BeginGesture(midiClipShared, "Edit velocity");
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

			// stems for the clips that do not hold the focus, in their track color: they
			// place the focused clip's dynamics against what surrounds it without
			// pretending to be draggable
			for (const auto& rc : rollClips) {
				if (rc.focused)
					continue;
				for (const auto& note : rc.clip->GetNotes()) {
					float x = gutterX + (float)((rc.viewOffset + note.startBeat) * PPB) - mScrollX;
					if (x < gutterX - 4.0f * scale || x > vp.x + availW)
						continue;
					float velY = laneBot - (note.velocity / 127.0f) * (laneBot - laneTop);
					dl->AddLine(ImVec2(x, laneBot), ImVec2(x, velY), Theme::WithAlpha(rc.color, 120), std::max(1.0f, 1.5f * scale));
					dl->AddCircleFilled(ImVec2(x, velY), 2.5f * scale, Theme::WithAlpha(rc.color, 200));
				}
			}

			// draw a thin stem + dot per note (like Ableton) so dense / overlapping
			// notes stay legible instead of fat bars covering each other
			for (size_t i = 0; i < notes.size(); ++i) {
				const auto& note = notes[i];
				float x = gutterX + (float)((focusOffset + note.startBeat) * PPB) - mScrollX;
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

	// ================================================================
	// KEYBOARD SHORTCUTS
	// ================================================================
	// window scope; not while a mouse drag is in progress
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

	// ================================================================
	// END OF DRAG
	// ================================================================
	// finalize the gesture undo and reset interaction state
	if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
		if (mGestureActive)
			EndGesture(midiClipShared);
		mInteraction = InteractionMode::None;
		mDragInitialStates.clear();
	}

	ImGui::End();

	// after End, so it wins over the hand/resize cursors the notes under the pointer
	// set while it is dragged across them
	if (mPanning)
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
}
