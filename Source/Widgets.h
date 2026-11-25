#pragma once
#include "Icons.h"
#include "imgui.h"
#include <cstddef>

// the flat controls the panels are built from. imgui's own button is a word in a
// rounded box, which is what made the app read as a debug overlay; a DAW's controls
// are glyphs and short captions that stay gray until they are engaged. everything here
// takes its colors from Theme and its size from the caller, so a toolbar, a track
// header and a device all share one look at whatever size their rows give them
//
// every color goes through ImGui::GetColorU32, so a control inside BeginDisabled fades
// with the rest of its row
namespace Widgets {

	// how a button shows itself at rest
	enum class Frame {
		Ghost, // a muted glyph, nothing else until the pointer reaches it: panel corners, row tools
		Plain, // the glyph in the full text tone with no box: the transport's own controls
		Boxed  // a filled box: the track and chain toggles, which read as one set
	};

	// a button that is only a glyph. `tooltip` is shown on hover when given
	bool IconButton(const char* id, Icon icon, const ImVec2& size, Frame frame = Frame::Ghost, const char* tooltip = nullptr);

	// a two-state glyph button. engaged, it fills with `activeColor` and the glyph turns
	// whichever of the light and dark ink reads on that fill
	bool IconToggle(const char* id, Icon icon, bool active, const ImVec2& size, ImU32 activeColor,
					Frame frame = Frame::Boxed, const char* tooltip = nullptr);

	// the same two states with a caption in place of the glyph (Map, Sync, Freeze). a
	// zero size component is fitted to the caption / the frame height. the accent is the
	// engaged color unless the caller names another
	bool TextToggle(const char* label, bool active, const ImVec2& size = ImVec2(0.0f, 0.0f), ImU32 activeColor = 0,
					const char* tooltip = nullptr);

	// a boxed button carrying a glyph and a caption side by side ("+ Chain")
	bool IconTextButton(const char* label, Icon icon, const ImVec2& size = ImVec2(0.0f, 0.0f), const char* tooltip = nullptr);

	// a text field with a magnifier inside its left edge. width comes from the usual
	// SetNextItemWidth; flags pass straight to the input
	bool SearchField(const char* id, char* buffer, size_t bufferSize, const char* hint, ImGuiInputTextFlags flags = 0);

	// a section's caption - the small strong face in the muted tone, with an optional
	// glyph before it - sized to sit on a frame-height row
	void Caption(const char* label, Icon icon = Icon::Count);

	// a list entry: a full-width selectable carrying a glyph and a name, with an optional
	// short tag in the caption size pinned to its right edge (a plugin's format). `id`
	// names the row; zero colors mean the usual ones. returns what Selectable returns
	bool ListRow(const char* id, Icon icon, const char* label, bool selected, ImU32 labelColor = 0, ImU32 iconColor = 0,
				 const char* tag = nullptr, ImGuiSelectableFlags flags = 0);

	// the glyph and name a list draws for an entry imgui has already laid out (a tree
	// node submitted with a hidden label), starting at `origin` on its text line
	void ListRowFace(const ImVec2& origin, float right, Icon icon, const char* label, ImU32 labelColor, ImU32 iconColor,
					 const char* tag = nullptr);

	// imgui's combo, with its arrow sitting in the field it opens rather than in a box of
	// the button tone - which made every dropdown read as a field with a button glued on.
	// pair with ImGui::EndCombo exactly as BeginCombo
	bool BeginCombo(const char* id, const char* preview, ImGuiComboFlags flags = 0);

	// a floating panel (the piano roll, settings, history): imgui's own window, its title
	// set in the strong face the way every other name in the app is. pair with ImGui::End
	bool BeginFloatingWindow(const char* name, bool* open = nullptr, ImGuiWindowFlags flags = 0);

	// the typefaces as a scope: push, draw, ImGui::PopFont()
	void PushStrongFont();				  // names, at the current size
	void PushSmallFont();				  // captions, in the current face
	void PushSmallStrongFont();			  // small headings

} // namespace Widgets
