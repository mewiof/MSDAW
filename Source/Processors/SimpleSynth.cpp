#include "Parameters/SliderParameter.h"
#include "PrecompHeader.h"
#include "SimpleSynth.h"
#include "ProcessorFactory.h"
#include <cmath>
#include <algorithm>

REGISTER_PROCESSOR(SimpleSynth, "SimpleSynth", true)

SimpleSynth::SimpleSynth() {
	// initialize parameters
	pAttack = AddParameter(std::make_unique<SliderParameter>("Attack", 0.05f, 0.001f, 2.0f));
	pRelease = AddParameter(std::make_unique<SliderParameter>("Release", 0.2f, 0.001f, 5.0f));
	pGain = AddParameter(std::make_unique<SliderParameter>("Gain", 0.5f, 0.0f, 1.0f));

	for (auto& v : mVoices) {
		v.active = false;
	}
}

void SimpleSynth::PrepareToPlay(double sampleRate) {
	mSampleRate = sampleRate;
}

void SimpleSynth::Reset() {
	// kill voices
	for (auto& v : mVoices) {
		v.active = false;
		v.amp = 0.0f;
		v.isReleased = false;
	}
}

void SimpleSynth::Process(float* buffer, int numFrames, int numChannels,
						  std::vector<MIDIMessage>& mIDIMessages,
						  const ProcessContext& context) {
	(void)context;

	int currentEventIndex = 0;
	int numEvents = (int)mIDIMessages.size();

	for (int i = 0; i < numFrames; ++i) {
		// 1. process MIDI events
		while (currentEventIndex < numEvents && mIDIMessages[currentEventIndex].frameIndex == i) {
			const auto& msg = mIDIMessages[currentEventIndex];
			uint8_t statusType = msg.status & 0xF0;

			if (statusType == 0x90) { // note on
				if (msg.data2 > 0)
					NoteOn(msg.data1, msg.data2);
				else
					NoteOff(msg.data1);
			} else if (statusType == 0x80) { // note off
				NoteOff(msg.data1);
			}
			currentEventIndex++;
		}

		// 2. render voices
		float sampleL = 0.0f;
		float sampleR = 0.0f;

		for (auto& voice : mVoices) {
			if (voice.active) {
				float s = GetSample(voice);
				sampleL += s;
				sampleR += s;
			}
		}

		// 3. accumulate
		if (numChannels >= 2) {
			buffer[i * numChannels + 0] += sampleL;
			buffer[i * numChannels + 1] += sampleR;
		}
	}
}

void SimpleSynth::NoteOn(int note, int velocity) {
	int stealIndex = -1;
	for (int i = 0; i < kNumVoices; ++i) {
		if (!mVoices[i].active) {
			stealIndex = i;
			break;
		}
	}
	if (stealIndex == -1)
		stealIndex = 0;

	auto& v = mVoices[stealIndex];
	v.active = true;
	v.note = note;
	v.velocity = velocity / 127.0f;
	v.phase = 0.0;
	double freq = 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
	v.phaseDelta = freq / mSampleRate;
	v.amp = 0.0f;
	v.isReleased = false;
}

void SimpleSynth::NoteOff(int note) {
	for (auto& v : mVoices) {
		if (v.active && v.note == note) {
			v.isReleased = true;
		}
	}
}

void SimpleSynth::AllNotesOff() {
	// release every sounding voice into its normal amp-release rather than hard-killing
	// it (as Reset does), so a loop wrap ends notes smoothly instead of clicking
	for (auto& v : mVoices) {
		if (v.active)
			v.isReleased = true;
	}
}

float SimpleSynth::GetSample(SynthVoice& voice) {
	float osc = (float)(2.0 * voice.phase - 1.0);
	osc *= pGain->value * 0.2f;

	voice.phase += voice.phaseDelta;
	if (voice.phase >= 1.0)
		voice.phase -= 1.0;

	float attackTime = std::max(0.001f, pAttack->value);
	float releaseTime = std::max(0.001f, pRelease->value);

	float attackRate = 1.0f / (float)(attackTime * mSampleRate);
	float releaseRate = 1.0f / (float)(releaseTime * mSampleRate);

	if (voice.isReleased) {
		voice.amp -= releaseRate;
		if (voice.amp <= 0.0f) {
			voice.amp = 0.0f;
			voice.active = false;
		}
	} else {
		if (voice.amp < voice.velocity) {
			voice.amp += attackRate;
			if (voice.amp > voice.velocity)
				voice.amp = voice.velocity;
		}
	}
	return osc * voice.amp;
}
