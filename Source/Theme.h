#pragma once
#include "imgui.h"

// the single source of truth for every color and typeface the DAW draws. before this,
// colors were split between ImGui's built-in dark theme and ~150 ad-hoc IM_COL32
// literals scattered across the views, so nothing was consistent and nothing could be
// retimed. now every draw site reads a named field off Theme::Instance()
//
// it is a global singleton (like AppConfig) rather than living on EditorContext
// because processor editors (AudioProcessor::RenderCustomUI) and Parameter::Draw
// have no context reference. all reads happen on the UI thread, so no locking
//
// default look: exact neutral grays in the tonal order of the reference product's
// mid-dark themes - dark chrome, darker bars, mid panels, lighter title strips and
// near-black graph wells. color is reserved for state: one orange accent for what is
// selected, active or being touched, plus the track toggles' own on-colors. a control
// at rest is always gray
class Theme {
public:
	static Theme& Instance();

	// ================================================================
	// SURFACES (darkest to lightest)
	// ================================================================

	ImU32 bgDeepest;		  // graph/plot interiors, meter wells
	ImU32 bgChrome;			  // the menu bar and whatever shows between windows
	ImU32 inputBg;			  // recessed control backgrounds (value boxes, text fields)
	ImU32 bgWindow;			  // the dark ground devices and the arrangement's empty canvas sit on
	ImU32 bgHeader;			  // bars and sticky strips: transport, tab strip, ruler, section heads
	ImU32 bgPanel;			  // ordinary panels: library, clip view, track rows, floating windows
	ImU32 bgPanelAlt;		  // raised surfaces: popups, group rows, the master row
	ImU32 bgHover;			  // hovered frame/row
	ImU32 bgActive;			  // selected row (neutral, never the accent)
	ImU32 bgDeviceEffect;	  // device rack slot: effect
	ImU32 bgDeviceInstrument; // device rack slot: instrument
	ImU32 bgDeviceRack;		  // a rack's own shell, around the panels it holds
	ImU32 bgRackInner;		  // recessed ground the rack's macro / chain / device panels sit on
	ImU32 deviceTitle;		  // the strip along a device's top that carries its name
	ImU32 deviceTitleSelected; // ... while the device is part of the selection
	ImU32 bgOverlay;		  // translucent dark box floating over the canvas
	ImU32 bgLane;			  // timeline lane behind an ordinary track
	ImU32 bgLaneSelected;	  // ... behind the selected track
	ImU32 bgLaneGroup;		  // ... behind a group, which holds no clips of its own

	// ================================================================
	// CONTROLS
	// ================================================================

	// a button sits a step above every surface it can be placed on, the raised ones
	// (popups, group rows) included - sharing a tone with any of them makes it vanish
	ImU32 button;
	ImU32 buttonHover;
	ImU32 buttonActive;		// pressed
	ImU32 ghostHover;		// wash under an unboxed icon button the pointer is over
	ImU32 ghostPress;		// ... while it is held
	ImU32 valueFill;		// level fill behind a value box's number, dark enough that the number stays readable on it
	ImU32 valueFillActive;	// ... while it is being dragged: the accent's hue, held just as dark
	ImU32 knobTrack;		// the full sweep of a dial, under its value arc
	ImU32 knobValue;		// the value arc and pointer of a dial at rest

	// the on-state of the three track toggles. each is its own color so a glance down
	// the track list tells muted, soloed and automated tracks apart
	ImU32 muteActive;
	ImU32 soloActive;
	ImU32 automationActive;

	// ================================================================
	// LINES & BORDERS
	// ================================================================

	ImU32 border;		// the dark groove around a control or a panel
	ImU32 borderStrong; // a light rule that has to read over dark and mid grounds alike
	ImU32 divider;		// near-black separator between rows/sections
	ImU32 gridSub;		// subdivision grid line (faintest)
	ImU32 gridBeat;		// beat grid line
	ImU32 gridBar;		// bar grid line (strongest)

	// ================================================================
	// TEXT
	// ================================================================

	ImU32 text;			// primary
	ImU32 textMuted;	// secondary labels, icons at rest
	ImU32 textDim;		// hints, disabled
	ImU32 textOnAccent; // dark text drawn on top of an accent fill

	// ================================================================
	// ACCENT - orange, used sparingly
	// ================================================================

	ImU32 accent;
	ImU32 accentHover;
	ImU32 accentActive;		 // pressed/deeper
	ImU32 accentMuted;		 // subtle fills
	ImU32 accentTranslucent; // selection wash / focus fill

	// ================================================================
	// STATUS & SEMANTIC
	// ================================================================

	ImU32 playhead;		   // the line the transport is at
	ImU32 recording;	   // red (armed/record)
	ImU32 marker;		   // the insert marker playback starts from
	ImU32 selectionFill;   // translucent wash under a selection region
	ImU32 selectionStroke; // bright outline around a selection
	ImU32 dropLine;		   // active drag-drop insert indicator
	ImU32 ghost;		   // neutral translucent preview (dragged content)
	ImU32 success;		   // green
	ImU32 danger;		   // red
	ImU32 graphCurve;	   // signature readout curve (eq) = accent
	ImU32 graphCurveCool;  // secondary readout curve (delay/reverb)
	ImU32 spectrumFill;	   // translucent body of a spectrum analyzer
	ImU32 spectrumEdge;	   // the line along the top of that body
	ImU32 spectrumFillAlt; // second overlaid spectrum (right / side channel)
	ImU32 spectrumEdgeAlt;
	ImU32 spectrogramFloor; // spectrogram ramp: at or under the noise floor
	ImU32 spectrogramMid;	// ... halfway up the scale
	ImU32 spectrogramPeak;	// ... the loudest thing on screen

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
	ImU32 clipTextDim; // name on a deactivated clip
	ImU32 clipBorder;
	ImU32 clipDisabled; // body of a deactivated clip, in place of the track color
	ImU32 clipLinked;	// chain badge + border of a clip whose notes another clip shares
	ImU32 waveform;	 // default waveform stroke fallback
	ImU32 waveBgMid; // subtle green backing (mid/side view)

	// how far a clip's body sinks from its track color toward the lane it sits on. the
	// title strip carries the color at full strength; the body only tints the lane, so
	// an arrangement full of clips stays mostly gray and its notes and waveforms read
	float clipBodyTint;
	float clipBodyTintSelected;

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
	// TYPOGRAPHY
	// ================================================================

	// text is small and dense on purpose: a DAW shows hundreds of labels at once, and a
	// face sized for reading prose makes every control look a size too big. names - a
	// track, a device, a clip, a section - are set in the strong face, values and
	// everything else in the regular one
	ImFont* fontRegular = nullptr;
	ImFont* fontStrong = nullptr;
	float fontSize = 0.0f;		// body text in pixels, display scale included
	float fontSizeSmall = 0.0f; // captions: ruler numbers, meter labels, badges

	// loads both faces at the display scale and makes the regular one the default.
	// called once, before the first frame
	void LoadFonts(float scale);

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
	// the WCAG contrast ratio between two colors' rgb, alpha ignored: 1 for the same
	// lightness, up to 21 for black against white. symmetric in its arguments
	static float ContrastRatio(ImU32 a, ImU32 b);
	// whichever of two text tones has the higher contrast against an opaque fill - for
	// text drawn on a color the user picked, which can land anywhere from dark to light
	static ImU32 ContrastingText(ImU32 fill, ImU32 light, ImU32 dark);

	// green -> yellow -> red ramp for the cpu/ram meter. 0 = good, 1 = pegged
	ImU32 HeatColor(float t) const;
	// green -> red ramp for a per-channel peak meter, norm in 0..1
	ImU32 MeterColor(float norm) const;
	// floor -> mid -> peak ramp for one spectrogram cell, t in 0..1
	ImU32 SpectrogramColor(float t) const;
	// curated on-theme track color chosen by index, cycling a fixed palette
	// (replaces the old rand() that produced muddy grays)
	ImU32 TrackColor(int index) const;
private:
	// the one place the concrete default palette is defined
	Theme();
};
