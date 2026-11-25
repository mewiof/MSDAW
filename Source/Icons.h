#pragma once
#include "imgui.h"

// the glyphs the interface draws in place of words. every one is built from draw-list
// primitives on a square box, so it scales with the display and needs nothing from the
// font atlas - which holds no symbols, and where a typed "M" or "X" reads as a label
// rather than as a control
enum class Icon {
	// transport
	Play,
	Pause,
	Stop,
	Loop,
	Follow,
	Keyboard,
	Automation,
	// track toggles
	Speaker,
	SpeakerOff,
	Headphones,
	// generic actions
	Close,
	Plus,
	Minus,
	More,
	Reset,
	Dice,
	Search,
	// direction
	ChevronLeft,
	ChevronRight,
	ChevronUp,
	ChevronDown,
	CaretRight,
	CaretDown,
	// library and files
	Folder,
	AudioFile,
	MIDIFile,
	ProjectFile,
	Plug,
	Device,
	Rack,
	// device panels
	Window,
	Knob,
	List,
	Devices,
	Target,
	Clip,
	Note,
	Phi,
	Spin,

	Count
};

namespace Icons {

	// draws `icon` centered on `center`, filling a square `size` pixels across. the stroke
	// weight follows the size, so a glyph keeps its proportions from a badge to a button
	void Draw(ImDrawList* drawList, Icon icon, const ImVec2& center, float size, ImU32 color);

} // namespace Icons
