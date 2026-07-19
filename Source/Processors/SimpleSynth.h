#pragma once
#include "AudioProcessor.h"
#include <array>

struct SynthVoice {
	bool active = false;
	int note = 0;
	float velocity = 0.0f;
	double phase = 0.0;
	double phaseDelta = 0.0;

	// simple envelope
	float amp = 0.0f;
	bool isReleased = false;
};

// basic sawtooth synth with polyphony
class SimpleSynth : public AudioProcessor {
public:
	SimpleSynth();
	~SimpleSynth() override = default;

	const char* GetName() const override { return "Simple Synth"; }
	std::string GetProcessorId() const override { return "SimpleSynth"; }
	bool IsInstrument() const override { return true; }

	void PrepareToPlay(double sampleRate) override;

	void Reset() override;
	void AllNotesOff() override;

	void Process(float* buffer, int numFrames, int numChannels,
				 std::vector<MIDIMessage>& mIDIMessages,
				 const ProcessContext& context) override;
private:
	double mSampleRate = 48000.0;
	static const int kNumVoices = 16;
	std::array<SynthVoice, kNumVoices> mVoices;

	// processor parameters
	Parameter* pAttack = nullptr;
	Parameter* pRelease = nullptr;
	Parameter* pGain = nullptr;

	void NoteOn(int note, int velocity);
	void NoteOff(int note);
	float GetSample(SynthVoice& voice);
};
