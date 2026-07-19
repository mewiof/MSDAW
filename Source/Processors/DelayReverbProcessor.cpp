#include "Parameters/SliderParameter.h"
#include "Parameters/KnobParameter.h"
#include "PrecompHeader.h"
#include "DelayReverbProcessor.h"
#include "ProcessorFactory.h"
#include "Theme.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include "imgui.h"

REGISTER_PROCESSOR(DelayReverbProcessor, "DelayReverb", false)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void DelayLine::Resize(int sizeSamples) {
	// find next power of 2
	int size = 1;
	while (size < sizeSamples)
		size *= 2;
	buffer.assign(size, 0.0f);
	mask = size - 1;
	writePos = 0;
}

void DelayLine::Write(float sample) {
	buffer[writePos] = sample;
	writePos = (writePos + 1) & mask;
}

float DelayLine::Read(float delayInSamples) const {
	float readPos = (float)writePos - delayInSamples;
	if (readPos < 0)
		readPos += (float)buffer.size();

	int iPart = (int)readPos;
	float fPart = readPos - iPart;

	// linear interpolation
	int iNext = (iPart + 1) & mask;
	iPart &= mask;

	return buffer[iPart] * (1.0f - fPart) + buffer[iNext] * fPart;
}

float OnePole::Process(float in, float coeff) {
	// simple lowpass filter
	z1 = (in * (1.0f - coeff)) + (z1 * coeff);
	return z1;
}

void CombFilter::Resize(int size) {
	delayLine.Resize(size);
	delayLine.mask = size - 1;
	delayLine.buffer.assign(size, 0.0f);
	delayLine.writePos = 0;
	damper.z1 = 0.0f;
}

float CombFilter::Process(float input, float dampingAmount) {
	float out = delayLine.buffer[delayLine.writePos];

	// apply damping to feedback
	float fbVal = damper.Process(out, dampingAmount);

	// write input and feedback
	delayLine.Write(input + fbVal * feedback);

	return out;
}

void AllPassFilter::Resize(int size) {
	delayLine.Resize(size);
	delayLine.buffer.assign(size, 0.0f);
	delayLine.mask = size - 1;
	delayLine.writePos = 0;
}

float AllPassFilter::Process(float input) {
	float bufOut = delayLine.buffer[delayLine.writePos];

	// allpass difference equation
	float out = bufOut - feedback * input;
	delayLine.Write(input + feedback * bufOut);

	return out;
}

// processor implementation

DelayReverbProcessor::DelayReverbProcessor() {
	// delay params
	pDelayTime = AddParameter(std::make_unique<KnobParameter>("Time", 250.0f, 1.0f, 2000.0f)); // ms
	pDelayFeedback = AddParameter(std::make_unique<KnobParameter>("Feed", 0.4f, 0.0f, 1.1f));
	pDelayPingPong = AddParameter(std::make_unique<SliderParameter>("Pong", 0.0f, 0.0f, 1.0f));
	pDelayLowCut = AddParameter(std::make_unique<KnobParameter>("LoCut", 100.0f, 20.0f, 1000.0f, ImGuiKnobVariant_Hertz));
	pDelayHighCut = AddParameter(std::make_unique<KnobParameter>("HiCut", 5000.0f, 1000.0f, 20000.0f, ImGuiKnobVariant_Hertz));
	pDelayMix = AddParameter(std::make_unique<KnobParameter>("Mix", 0.3f, 0.0f, 1.0f));

	// reverb params
	pRevSize = AddParameter(std::make_unique<KnobParameter>("Size", 1.0f, 0.5f, 2.0f));
	pRevDecay = AddParameter(std::make_unique<KnobParameter>("Decay", 0.85f, 0.0f, 0.98f));
	pRevDamp = AddParameter(std::make_unique<KnobParameter>("Damp", 0.2f, 0.0f, 1.0f));
	pRevMix = AddParameter(std::make_unique<KnobParameter>("Mix", 0.2f, 0.0f, 1.0f));
}

void DelayReverbProcessor::PrepareToPlay(double sampleRate) {
	mSampleRate = sampleRate;

	// 2 second buffer
	int maxDelaySamples = (int)(2.0 * sampleRate) + 1024;
	mDelayL.Resize(maxDelaySamples);
	mDelayR.Resize(maxDelaySamples);

	// reverb setup
	for (int i = 0; i < kNumCombs; ++i) {
		mRevL.combs[i].Resize((int)(kCombTuningsL[i] * sampleRate / 44100.0));
		mRevR.combs[i].Resize((int)(kCombTuningsR[i] * sampleRate / 44100.0));
	}
	for (int i = 0; i < kNumAllPass; ++i) {
		mRevL.allpasses[i].Resize((int)(kAllPassTuningsL[i] * sampleRate / 44100.0));
		mRevR.allpasses[i].Resize((int)(kAllPassTuningsR[i] * sampleRate / 44100.0));
	}

	Reset();
}

void DelayReverbProcessor::Reset() {
	std::fill(mDelayL.buffer.begin(), mDelayL.buffer.end(), 0.0f);
	std::fill(mDelayR.buffer.begin(), mDelayR.buffer.end(), 0.0f);
	mHpStateL = 0.0f;
	mHpStateR = 0.0f;
	mLpL.z1 = mHpL.z1 = mLpR.z1 = mHpR.z1 = 0.0f;

	for (int i = 0; i < kNumCombs; ++i) {
		mRevL.combs[i].damper.z1 = 0;
		mRevR.combs[i].damper.z1 = 0;
		std::fill(mRevL.combs[i].delayLine.buffer.begin(), mRevL.combs[i].delayLine.buffer.end(), 0.0f);
		std::fill(mRevR.combs[i].delayLine.buffer.begin(), mRevR.combs[i].delayLine.buffer.end(), 0.0f);
	}
	for (int i = 0; i < kNumAllPass; ++i) {
		std::fill(mRevL.allpasses[i].delayLine.buffer.begin(), mRevL.allpasses[i].delayLine.buffer.end(), 0.0f);
		std::fill(mRevR.allpasses[i].delayLine.buffer.begin(), mRevR.allpasses[i].delayLine.buffer.end(), 0.0f);
	}
}

void DelayReverbProcessor::Process(float* buffer, int numFrames, int numChannels,
								   std::vector<MIDIMessage>& mIDIMessages,
								   const ProcessContext& context) {
	(void)mIDIMessages;
	(void)context;

	// 1. delay calculations
	float dTimeMs = pDelayTime->value;
	float dFeedback = pDelayFeedback->value;
	float dMix = pDelayMix->value;
	bool dPingPong = (pDelayPingPong->value > 0.5f);

	// feedback filter coeffs
	float fcLo = pDelayLowCut->value;
	float hpAlpha = 1.0f / (1.0f + (float)(2.0 * M_PI * fcLo / mSampleRate));

	float fcHi = pDelayHighCut->value;
	float lpAlpha = 1.0f - std::exp(-2.0f * (float)M_PI * fcHi / (float)mSampleRate);

	float delaySamples = (dTimeMs / 1000.0f) * (float)mSampleRate;

	// 2. reverb calculations
	float rDecay = pRevDecay->value;
	float rDamp = pRevDamp->value;
	float rMix = pRevMix->value;

	// update feedback for combs
	for (int i = 0; i < kNumCombs; ++i) {
		mRevL.combs[i].feedback = rDecay;
		mRevR.combs[i].feedback = rDecay;
	}

	for (int i = 0; i < numFrames; ++i) {
		float inL = buffer[i * numChannels + 0];
		float inR = (numChannels > 1) ? buffer[i * numChannels + 1] : inL;

		// delay processing
		float delayOutL = mDelayL.Read(delaySamples);
		float delayOutR = mDelayR.Read(delaySamples);

		float fbL = delayOutL;
		float fbR = delayOutR;

		// filters
		fbL += lpAlpha * (mLpL.z1 - fbL);
		mLpL.z1 = fbL;
		fbR += lpAlpha * (mLpR.z1 - fbR);
		mLpR.z1 = fbR;

		float hpOutL = hpAlpha * (mHpL.z1 + fbL - mHpStateL);
		mHpStateL = fbL;
		mHpL.z1 = hpOutL;
		fbL = hpOutL;

		float hpOutR = hpAlpha * (mHpR.z1 + fbR - mHpStateR);
		mHpStateR = fbR;
		mHpR.z1 = hpOutR;
		fbR = hpOutR;

		// saturation
		fbL = std::tanh(fbL);
		fbR = std::tanh(fbR);

		// buffer write
		if (dPingPong) {
			mDelayL.Write(inL + fbR * dFeedback);
			mDelayR.Write(inR + fbL * dFeedback);
		} else {
			mDelayL.Write(inL + fbL * dFeedback);
			mDelayR.Write(inR + fbR * dFeedback);
		}

		float wetDelayL = delayOutL;
		float wetDelayR = delayOutR;

		// reverb processing
		float revInL = inL + wetDelayL * 0.5f;
		float revInR = inR + wetDelayR * 0.5f;

		float combSumL = 0.0f;
		float combSumR = 0.0f;

		for (int k = 0; k < kNumCombs; ++k) {
			combSumL += mRevL.combs[k].Process(revInL, rDamp);
			combSumR += mRevR.combs[k].Process(revInR, rDamp);
		}

		combSumL *= 0.25f;
		combSumR *= 0.25f;

		for (int k = 0; k < kNumAllPass; ++k) {
			combSumL = mRevL.allpasses[k].Process(combSumL);
			combSumR = mRevR.allpasses[k].Process(combSumR);
		}

		float wetRevL = combSumL;
		float wetRevR = combSumR;

		// mix result
		float afterDelayL = inL + (wetDelayL - inL) * dMix;
		float afterDelayR = inR + (wetDelayR - inR) * dMix;

		float finalL = afterDelayL + wetRevL * rMix;
		float finalR = afterDelayR + wetRevR * rMix;

		buffer[i * numChannels + 0] = finalL;
		if (numChannels > 1) {
			buffer[i * numChannels + 1] = finalR;
		}
	}
}

// visualization and ui

void DelayReverbProcessor::RecalcVisCurve() {
	mVisCurve.clear();
	mVisCurve.resize(100);

	float dTime = pDelayTime->value;
	float dFeed = pDelayFeedback->value;
	float rDecay = pRevDecay->value;
	float rMix = pRevMix->value;
	float dMix = pDelayMix->value;

	float timePerPixel = 2000.0f / 100.0f; // ms per point

	for (int i = 0; i < 100; ++i) {
		float t = i * timePerPixel;
		float amp = 0.0f;

		if (i == 0)
			amp += 1.0f;

		if (dTime > 0.1f) {
			float tapIdx = t / dTime;
			float dist = std::abs(tapIdx - std::round(tapIdx));
			if (dist < 0.1f && tapIdx > 0.5f) {
				float tapNum = std::round(tapIdx);
				float tapAmp = std::pow(dFeed, tapNum) * dMix;
				amp += tapAmp;
			}
		}

		if (t > 0) {
			float decayFactor = -5.0f * (1.0f - rDecay);
			float env = std::exp(decayFactor * (t / 1000.0f));
			amp += env * rMix * 0.5f;
		}

		mVisCurve[i] = std::min(amp, 1.0f);
	}
}

bool DelayReverbProcessor::RenderCustomUI(const ImVec2& size) {
	float graphWidth = size.x * 0.55f;
	float controlsWidth = size.x - graphWidth - 10.0f;
	float height = size.y;

	// draw graph
	const Theme& th = Theme::Instance();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();

	drawList->AddRectFilled(p, ImVec2(p.x + graphWidth, p.y + height), th.bgDeepest);
	drawList->AddRect(p, ImVec2(p.x + graphWidth, p.y + height), th.border);

	RecalcVisCurve();
	float stepX = graphWidth / (float)mVisCurve.size();
	ImVec2 prevPos;
	for (size_t i = 0; i < mVisCurve.size(); ++i) {
		float val = mVisCurve[i];
		float h = val * (height - 10);
		ImVec2 curPos(p.x + i * stepX, p.y + height - h - 5);
		if (i > 0) {
			drawList->AddLine(prevPos, curPos, Theme::WithAlpha(th.graphCurveCool, 200), 2.0f);
			drawList->AddQuadFilled(prevPos, curPos, ImVec2(curPos.x, p.y + height), ImVec2(prevPos.x, p.y + height), Theme::WithAlpha(th.graphCurveCool, 50));
		}
		prevPos = curPos;
	}
	drawList->AddText(ImVec2(p.x + 5, p.y + 5), th.textDim, "Response");

	// draw controls
	ImGui::SetCursorScreenPos(ImVec2(p.x + graphWidth + 10, p.y));
	ImGui::BeginGroup();

	ImGui::BeginGroup();
	ImGui::TextDisabled("Delay");
	pDelayTime->Draw();
	ImGui::Dummy(ImVec2(0, 5));
	pDelayFeedback->Draw();
	ImGui::Dummy(ImVec2(0, 5));
	pDelayMix->Draw();
	ImGui::EndGroup();

	ImGui::SameLine();
	ImGui::Dummy(ImVec2(5, 0));
	ImGui::SameLine();

	ImGui::BeginGroup();
	ImGui::TextDisabled("Reverb");
	pRevDecay->Draw();
	ImGui::Dummy(ImVec2(0, 5));
	pRevSize->Draw();
	ImGui::Dummy(ImVec2(0, 5));
	pRevMix->Draw();
	ImGui::EndGroup();

	ImGui::SameLine();
	ImGui::Dummy(ImVec2(5, 0));
	ImGui::SameLine();
	ImGui::BeginGroup();
	ImGui::TextDisabled("EQ/Misc");
	pDelayLowCut->Draw();
	ImGui::Dummy(ImVec2(0, 5));
	pDelayHighCut->Draw();
	ImGui::Dummy(ImVec2(0, 5));
	bool pp = pDelayPingPong->value > 0.5f;
	if (ImGui::Checkbox("Pong", &pp))
		pDelayPingPong->value = pp ? 1.0f : 0.0f;
	ImGui::EndGroup();

	ImGui::EndGroup();

	return true;
}
