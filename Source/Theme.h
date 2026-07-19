#pragma once
#include "imgui.h"

// the single source of truth for every color the DAW draws. before this, colors
// were split between ImGui's built-in dark theme and ~150 ad-hoc IM_COL32 literals
// scattered across the views, so nothing was consistent and nothing could be
// retimed. now every draw site reads a named field off Theme::Instance().
//
// it is a global singleton (like AppConfig) rather than living on EditorContext
// because processor editors (AudioProcessor::RenderCustomUI) and Parameter::Draw
// have no context reference. all reads happen on the UI thread, so no locking.
//
// default look: neutral dark gray, Ableton-like, with a single amber/gold accent
// used sparingly - accent appears only on selection, active toggles, focus, and
// the value being touched, never on every knob/slider at rest
class Theme {
public:
	static Theme& Instance();

	// ================================================================
	// SURFACES (darkest to lightest)
	// ================================================================

	ImU32 bgDeepest;		  // graph/plot interiors, meter wells
	ImU32 inputBg;			  // recessed control backgrounds (slider tracks, text boxes)
	ImU32 bgWindow;			  // main application window
	ImU32 bgPanel;			  // track rows, ruler, ordinary panels
	ImU32 bgPanelAlt;		  // raised buttons, popups, group rows
	ImU32 bgHeader;			  // sticky header strips
	ImU32 bgHover;			  // hovered frame/row/button
	ImU32 bgActive;			  // selected row (neutral, never the accent)
	ImU32 bgDeviceEffect;	  // device rack slot: effect
	ImU32 bgDeviceInstrument; // device rack slot: instrument
	ImU32 bgOverlay;		  // translucent dark box floating over the canvas

	// ================================================================
	// LINES & BORDERS
	// ================================================================

	ImU32 border;
	ImU32 borderStrong;
	ImU32 divider;	// near-black separator between rows/sections
	ImU32 gridSub;	// subdivision grid line (faintest)
	ImU32 gridBeat; // beat grid line
	ImU32 gridBar;	// bar grid line (strongest)

	// ================================================================
	// TEXT
	// ================================================================

	ImU32 text;			// primary
	ImU32 textMuted;	// secondary labels
	ImU32 textDim;		// hints, disabled
	ImU32 textOnAccent; // dark text drawn on top of an accent fill

	// ================================================================
	// ACCENT - amber/gold, used sparingly
	// ================================================================

	ImU32 accent;
	ImU32 accentHover;
	ImU32 accentActive;		 // pressed/deeper
	ImU32 accentMuted;		 // subtle fills
	ImU32 accentTranslucent; // selection wash / focus fill

	// ================================================================
	// STATUS & SEMANTIC
	// ================================================================

	ImU32 playhead;		   // red
	ImU32 recording;	   // red (armed/record)
	ImU32 marker;		   // green insert/loop cursor
	ImU32 selectionFill;   // translucent wash under a selection region
	ImU32 selectionStroke; // bright outline around a selection
	ImU32 dropLine;		   // active drag-drop insert indicator
	ImU32 ghost;		   // neutral translucent preview (dragged content)
	ImU32 success;		   // green
	ImU32 danger;		   // red
	ImU32 graphCurve;	   // signature readout curve (eq) = accent
	ImU32 graphCurveCool;  // secondary readout curve (delay/reverb)

	// ================================================================
	// METERS (peak level ramp)
	// ================================================================

	ImU32 meterBg;
	ImU32 meterLow;	 // green
	ImU32 meterMid;	 // yellow
	ImU32 meterHigh; // red

	// ================================================================
	// CLIPS & WAVEFORMS
	// ================================================================

	ImU32 clipText;
	ImU32 clipBorder;
	ImU32 waveform;	  // default waveform stroke fallback
	ImU32 waveBgMono; // dark backing behind a mono waveform
	ImU32 waveBgMid;  // subtle green backing (mid/side view)

	// ================================================================
	// PIANO ROLL
	// ================================================================

	ImU32 keyWhite;
	ImU32 keyBlack;
	ImU32 keyText;
	ImU32 keyPressed; // key lit by preview/computer keyboard
	ImU32 rowWhite;	  // grid row behind a white key
	ImU32 rowBlack;	  // grid row behind a black key
	ImU32 noteFill;	  // warm amber note body
	ImU32 noteFillSelected;
	ImU32 noteBorder;
	ImU32 noteBorderSelected;
	ImU32 veloStem;
	ImU32 veloStemSelected;
	ImU32 veloDot;
	ImU32 veloDotSelected;

	// ================================================================
	// AUTOMATION
	// ================================================================

	ImU32 automationLine;
	ImU32 automationPoint;
	ImU32 automationPointSelected;
	ImU32 automationGhost; // faint automation of neighboring lanes

	// ================================================================
	// STYLE + HELPERS
	// ================================================================

	// pushes the palette into ImGui's Colors[] so built-in widgets (buttons,
	// frames, headers, scrollbars, tabs...) match. also owns ScaleAllSizes and
	// the rounding/spacing defaults. replaces the StyleColorsDark block in Main
	void ApplyImGuiStyle(float scale) const;

	// repack alpha (0-255) while keeping rgb - lets call sites reuse one named
	// color at several opacities instead of inventing a field per translucency
	static ImU32 WithAlpha(ImU32 color, int alpha);
	// straight rgba lerp between two packed colors, t in 0..1
	static ImU32 Lerp(ImU32 a, ImU32 b, float t);

	// green -> yellow -> red ramp for the cpu/ram meter. 0 = good, 1 = pegged
	ImU32 HeatColor(float t) const;
	// green -> red ramp for a per-channel peak meter, norm in 0..1
	ImU32 MeterColor(float norm) const;
	// curated on-theme track color chosen by index, cycling a fixed palette
	// (replaces the old rand() that produced muddy grays)
	ImU32 TrackColor(int index) const;
private:
	// the one place the concrete default palette is defined
	Theme();
};
