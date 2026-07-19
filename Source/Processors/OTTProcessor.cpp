#include "Parameters/SliderParameter.h"
#include "PrecompHeader.h"
#include "OTTProcessor.h"
#include "ProcessorFactory.h"
#include "Theme.h"
#include <cmath>
#include <algorithm>
#include <cstdio>

REGISTER_PROCESSOR(OTTProcessor, "OTT", false)

// TODO: this fuck is shit

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// biquad implementation
void OTTProcessor::Biquad::CalcLowPass(float freq, float q, float sampleRate) {
	float w0 = (float)(2.0 * M_PI * freq / sampleRate);
	float alpha = std::sin(w0) / (2.0f * q);
	float cosw0 = std::cos(w0);

	b0 = (1.0f - cosw0) / 2.0f;
	b1 = 1.0f - cosw0;
	b2 = (1.0f - cosw0) / 2.0f;
	float a0 = 1.0f + alpha;
	a1 = (-2.0f * cosw0) / a0;
	a2 = (1.0f - alpha) / a0;
	b0 /= a0;
	b1 /= a0;
	b2 /= a0;
}

void OTTProcessor::Biquad::CalcHighPass(float freq, float q, float sampleRate) {
	float w0 = (float)(2.0 * M_PI * freq / sampleRate);
	float alpha = std::sin(w0) / (2.0f * q);
	float cosw0 = std::cos(w0);

	b0 = (1.0f + cosw0) / 2.0f;
	b1 = -(1.0f + cosw0);
	b2 = (1.0f + cosw0) / 2.0f;
	float a0 = 1.0f + alpha;
	a1 = (-2.0f * cosw0) / a0;
	a2 = (1.0f - alpha) / a0;
	b0 /= a0;
	b1 /= a0;
	b2 /= a0;
}

float OTTProcessor::Biquad::Process(float in) {
	float out = b0 * in + z1;
	z1 = b1 * in - a1 * out + z2;
	z2 = b2 * in - a2 * out;
	return out;
}

// compressor implementation
float OTTProcessor::CompressorBand::GetGainForSample(float sample, float timeScale, float sampleRate) {
	// 1. rms detection
	const float rmsWindow = 0.005f;
	float rmsCoeff = 1.0f - std::exp(-1.0f / (rmsWindow * sampleRate));
	float sq = sample * sample;

	rmsState += rmsCoeff * (sq - rmsState);
	if (rmsState < 1e-9f)
		rmsState = 1e-9f;

	float db = 10.0f * std::log10(rmsState);

	// 2. ott compression curve logic
	float targetGainDb = 0.0f;

	// downward compression
	const float highThresh = -16.0f;
	if (db > highThresh) {
		targetGainDb -= (db - highThresh) * 0.8f;
	}

	// upward compression
	const float lowThresh = -42.0f;
	if (db < lowThresh) {
		float diff = lowThresh - db;
		targetGainDb += std::min(diff * 0.6f, 36.0f);
	}

	// 3. attack / release
	float attackSec = 0.002f * timeScale;
	float releaseSec = 0.050f * timeScale;

	float attCoeff = 1.0f - std::exp(-1.0f / (attackSec * sampleRate));
	float relCoeff = 1.0f - std::exp(-1.0f / (releaseSec * sampleRate));

	float targetGainLin = std::pow(10.0f, targetGainDb / 20.0f);

	if (targetGainLin < gainReduction) {
		// attack (gain is dropping)
		gainReduction += attCoeff * (targetGainLin - gainReduction);
	} else {
		// release (gain is recovering)
		gainReduction += relCoeff * (targetGainLin - gainReduction);
	}

	visualGain = gainReduction;

	return gainReduction;
}

// main processor

OTTProcessor::OTTProcessor() {
	pDepth = AddParameter(std::make_unique<SliderParameter>("Depth", 1.0f, 0.0f, 1.0f));
	pTime = AddParameter(std::make_unique<SliderParameter>("Time", 1.0f, 0.1f, 10.0f));
	pInGain = AddParameter(std::make_unique<SliderParameter>("In Gain", 0.0f, -24.0f, 24.0f));
	pOutGain = AddParameter(std::make_unique<SliderParameter>("Out Gain", 0.0f, -24.0f, 24.0f));

	pLowGain = AddParameter(std::make_unique<SliderParameter>("Low Gain", 0.0f, -12.0f, 12.0f));
	pMidGain = AddParameter(std::make_unique<SliderParameter>("Mid Gain", 0.0f, -12.0f, 12.0f));
	pHighGain = AddParameter(std::make_unique<SliderParameter>("High Gain", 0.0f, -12.0f, 12.0f));
}

void OTTProcessor::PrepareToPlay(double sampleRate) {
	mSampleRate = sampleRate;
	mChannels.clear();
}

void OTTProcessor::Reset() {
	mChannels.clear();
}

void OTTProcessor::Process(float* buffer, int numFrames, int numChannels,
						   std::vector<MIDIMessage>& mIDIMessages,
						   const ProcessContext& context) {
	(void)mIDIMessages;
	(void)context;

	if (mChannels.size() != (size_t)numChannels) {
		mChannels.resize(numChannels);

		for (auto& ch : mChannels) {
			// setup crossover filters
			ch.lpLow.CalcLowPass(kFreqLow, 0.707f, (float)mSampleRate);
			ch.hpLow.CalcHighPass(kFreqLow, 0.707f, (float)mSampleRate);

			ch.lpHigh.CalcLowPass(kFreqHigh, 0.707f, (float)mSampleRate);
			ch.hpHigh.CalcHighPass(kFreqHigh, 0.707f, (float)mSampleRate);
		}
	}

	float inGain = std::pow(10.0f, pInGain->value / 20.0f);
	float outGain = std::pow(10.0f, pOutGain->value / 20.0f);

	float gL = std::pow(10.0f, pLowGain->value / 20.0f);
	float gM = std::pow(10.0f, pMidGain->value / 20.0f);
	float gH = std::pow(10.0f, pHighGain->value / 20.0f);

	float depth = pDepth->value;
	float timeScale = pTime->value;

	for (int i = 0; i < numFrames; ++i) {
		for (int c = 0; c < numChannels; ++c) {
			float inSample = buffer[i * numChannels + c] * inGain;

			auto& ch = mChannels[c];

			// crossover
			float lowBand = ch.lpLow.Process(inSample);
			float midHigh = ch.hpLow.Process(inSample);

			float midBand = ch.lpHigh.Process(midHigh);
			float highBand = ch.hpHigh.Process(midHigh);

			// compression
			float gainL = ch.bands[0].GetGainForSample(lowBand, timeScale, (float)mSampleRate);
			float gainM = ch.bands[1].GetGainForSample(midBand, timeScale, (float)mSampleRate);
			float gainH = ch.bands[2].GetGainForSample(highBand, timeScale, (float)mSampleRate);

			float outL = lowBand * gainL * gL;
			float outM = midBand * gainM * gM;
			float outH = highBand * gainH * gH;

			float wetSignal = outL + outM + outH;
			float output = (wetSignal * depth) + (inSample * (1.0f - depth));

			buffer[i * numChannels + c] = output * outGain;
		}
	}
}

bool OTTProcessor::RenderCustomUI(const ImVec2& size) {
	const Theme& th = Theme::Instance();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();

	drawList->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), th.bgDeepest);
	drawList->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), th.border);

	if (mChannels.empty()) {
		ImGui::Text("Audio engine not running");
		return true;
	}

	float gains[3] = {0, 0, 0};
	int count = 0;
	for (const auto& ch : mChannels) {
		gains[0] += ch.bands[0].visualGain;
		gains[1] += ch.bands[1].visualGain;
		gains[2] += ch.bands[2].visualGain;
		count++;
	}
	if (count > 0) {
		gains[0] /= count;
		gains[1] /= count;
		gains[2] /= count;
	}

	float barWidth = (size.x - 40) / 3.0f;
	float centerY = p.y + size.y / 2.0f;
	const char* labels[] = {"Low", "Mid", "High"};

	for (int b = 0; b < 3; ++b) {
		float x = p.x + 10 + b * (barWidth + 10);
		float g = gains[b];

		float db = 20.0f * std::log10(g + 0.0001f);
		float pxHeight = -db * (size.y / 48.0f);

		ImVec2 barStart(x, centerY);
		ImVec2 barEnd(x + barWidth, centerY + pxHeight);

		ImU32 col = (db >= 0) ? Theme::WithAlpha(th.success, 200) : Theme::WithAlpha(th.danger, 200);

		drawList->AddRectFilled(barStart, barEnd, col);
		drawList->AddText(ImVec2(x, p.y + size.y - 20), th.textMuted, labels[b]);

		char valBuf[16];
		snprintf(valBuf, 16, "%.1fdb", db);
		drawList->AddText(ImVec2(x, p.y + 5), th.textDim, valBuf);
	}

	drawList->AddLine(ImVec2(p.x, centerY), ImVec2(p.x + size.x, centerY), Theme::WithAlpha(th.text, 100));
	ImGui::InvisibleButton("OttVis", size);

	return true;
}
