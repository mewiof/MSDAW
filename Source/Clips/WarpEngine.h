#pragma once
#include "AudioClip.h" // for WarpMode
#include <cstddef>
#include <cstdint>

// position-addressable granular time-stretch / pitch-shift for warped audio clips
// it decouples time (governed by `speed`) from pitch (governed by `pitchRead`) so a clip
// can follow the project tempo without shifting pitch, and be transposed without changing
// length -- the thing plain resampling (Re-Pitch) cannot do
//
// the engine is deliberately stateless: the output for any block depends only on
// (samples, outStartFrame, params), never on what was rendered before. that keeps the
// exact property Track::Process relies on -- seeking, looping and offline export all drive
// the same graph from transport position alone -- and it means the audio thread never
// allocates (all scratch is fixed-size and stack-local)
//
// honest scope: this is a time-domain granular approximation, not a phase vocoder. the
// modes differ by grain size / overlap / windowing plus a deterministic WSOLA-style align
// on the tonal modes (each grain aligns to its predecessor's unadjusted position, so the
// align stays position-addressable rather than drifting with block phase). Complex/ComplexPro
// are the smoothest tier, not a spectral engine; ComplexPro's formant control is a cheap
// spectral tilt, not true FFT formant preservation

struct WarpRenderParams {
	WarpMode mode = WarpMode::Beats;
	double sampleRate = 48000.0;	 // output/device rate, turns grain milliseconds into frames
	double speed = 1.0;				 // Rsr*Rwarp: source frames the grain anchor advances per output frame
	double pitchRead = 1.0;			 // Rsr*Rpitch: source frames read per output frame inside a grain
	double pitchRatio = 1.0;		 // Rpitch alone: the musical pitch shift, used only for formant tilt
	double offsetOutputFrames = 0.0; // clip file offset, expressed in output frames

	// per-mode knobs, already clamped by the caller
	double grainSizeMs = 80.0;		// Tones/Texture/Complex/ComplexPro grain length
	double fluctuation = 0.0;		// texture only, 0..1 deterministic per-grain source jitter
	double transientEnvelope = 0.5; // beats only, 0..1 grain fade shaping (0 keeps transients sharp)
	double formants = 1.0;			// ComplexPro only, 0..1 approximate formant compensation strength
};

// mixes `count` output frames into `dest` (interleaved, destChannels wide) for the clip
// segment starting at absolute output frame `outStartFrame` (frames since clip content
// start). adds into dest like the existing reader's `+=`, so callers zero/own the buffer.
void RenderWarpedBlock(const float* samples, size_t numSamples, int clipChannels,
					   int64_t outStartFrame, int count, int destChannels,
					   float* dest, const WarpRenderParams& params);
