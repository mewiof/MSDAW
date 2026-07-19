#include "Parameters/SliderParameter.h"
#include "Parameters/KnobParameter.h"
#include "PrecompHeader.h"
#include "EqProcessor.h"
#include "ProcessorFactory.h"
#include "Theme.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <string>
#include "imgui.h"
#include "imgui_internal.h"

REGISTER_PROCESSOR(EqProcessor, "EqEight", false)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// drawing helpers

static void DrawFilterIcon(FilterType type, const ImVec2& p, float w, float h, ImU32 color) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	float t = 1.5f;

	switch (type) {
	case FilterType::LowCut:
		dl->AddBezierCubic(
			ImVec2(p.x, p.y + h),
			ImVec2(p.x + w * 0.4f, p.y + h),
			ImVec2(p.x + w * 0.4f, p.y + h * 0.2f),
			ImVec2(p.x + w, p.y + h * 0.2f),
			color, t);
		break;
	case FilterType::LowShelf:
		dl->AddBezierCubic(
			ImVec2(p.x, p.y + h * 0.8f),
			ImVec2(p.x + w * 0.4f, p.y + h * 0.8f),
			ImVec2(p.x + w * 0.4f, p.y + h * 0.2f),
			ImVec2(p.x + w, p.y + h * 0.2f),
			color, t);
		break;
	case FilterType::Bell:
		dl->AddBezierCubic(
			ImVec2(p.x, p.y + h * 0.8f),
			ImVec2(p.x + w * 0.3f, p.y + h * 0.8f),
			ImVec2(p.x + w * 0.4f, p.y + h * 0.1f),
			ImVec2(p.x + w * 0.5f, p.y + h * 0.1f),
			color, t);
		dl->AddBezierCubic(
			ImVec2(p.x + w * 0.5f, p.y + h * 0.1f),
			ImVec2(p.x + w * 0.6f, p.y + h * 0.1f),
			ImVec2(p.x + w * 0.7f, p.y + h * 0.8f),
			ImVec2(p.x + w, p.y + h * 0.8f),
			color, t);
		break;
	case FilterType::HighShelf:
		dl->AddBezierCubic(
			ImVec2(p.x, p.y + h * 0.2f),
			ImVec2(p.x + w * 0.4f, p.y + h * 0.2f),
			ImVec2(p.x + w * 0.4f, p.y + h * 0.8f),
			ImVec2(p.x + w, p.y + h * 0.8f),
			color, t);
		break;
	case FilterType::HighCut:
		dl->AddBezierCubic(
			ImVec2(p.x, p.y + h * 0.2f),
			ImVec2(p.x + w * 0.4f, p.y + h * 0.2f),
			ImVec2(p.x + w * 0.4f, p.y + h),
			ImVec2(p.x + w, p.y + h),
			color, t);
		break;
	}
}

// implementation

EqProcessor::EqProcessor() {
	mBands.resize(kNumBands);
	for (int i = 0; i < kNumBands; ++i) {
		std::string prefix = "B" + std::to_string(i + 1) + " ";

		float defaultFreq = 100.0f;
		float defaultType = 2.0f;
		float defaultQ = 0.71f;
		float defaultOn = 0.0f;

		if (i == 0) {
			defaultFreq = 100.0f;
			defaultType = 1.0f;
			defaultOn = 1.0f;
		} else if (i == 1) {
			defaultFreq = 400.0f;
			defaultOn = 1.0f;
		} else if (i == 2) {
			defaultFreq = 1500.0f;
			defaultOn = 1.0f;
		} else if (i == 3) {
			defaultFreq = 6000.0f;
			defaultType = 3.0f;
			defaultOn = 1.0f;
		} else {
			defaultFreq = 10000.0f + (i - 4) * 2000.0f;
		}

		mBands[i].pActive = AddParameter(std::make_unique<SliderParameter>(prefix + "On", defaultOn, 0.0f, 1.0f));
		mBands[i].pType = AddParameter(std::make_unique<SliderParameter>(prefix + "Type", defaultType, 0.0f, 4.0f));
		mBands[i].pFreq = AddParameter(std::make_unique<KnobParameter>(prefix + "Freq", defaultFreq, 10.0f, 22000.0f, ImGuiKnobVariant_Hertz));
		mBands[i].pGain = AddParameter(std::make_unique<KnobParameter>(prefix + "Gain", 0.0f, -15.0f, 15.0f, ImGuiKnobVariant_Decibel));
		mBands[i].pQ = AddParameter(std::make_unique<KnobParameter>(prefix + "Q", defaultQ, 0.1f, 18.0f));
	}

	pGlobalGain = AddParameter(std::make_unique<KnobParameter>("Output", 0.0f, -24.0f, 24.0f, ImGuiKnobVariant_Decibel));
	pScale = AddParameter(std::make_unique<KnobParameter>("Scale", 100.0f, 0.0f, 200.0f, ImGuiKnobVariant_Percent));
	pAdaptQ = AddParameter(std::make_unique<SliderParameter>("AdaptQ", 0.0f, 0.0f, 1.0f));
	pMode = AddParameter(std::make_unique<SliderParameter>("Mode", 0.0f, 0.0f, 4.0f));

	mSelectedBandIndex = 0;
}

void EqProcessor::PrepareToPlay(double sampleRate) {
	mSampleRate = sampleRate;
	mStates.clear();
	for (int i = 0; i < kNumBands; ++i)
		RecalculateCoeffs(i);
}

void EqProcessor::Reset() {
	for (auto& ch : mStates) {
		for (auto& band : ch) {
			band.z1 = 0;
			band.z2 = 0;
		}
	}
}

void EqProcessor::RecalculateCoeffs(int i) {
	auto& b = mBands[i];

	if (b.pActive->value < 0.5f) {
		b.coeffs = {1, 0, 0, 0, 0};
		return;
	}

	double Fs = mSampleRate;
	if (Fs < 1.0)
		Fs = 48000.0;

	double f0 = (double)b.pFreq->value;
	double Q = (double)b.pQ->value;
	double baseDbGain = (double)b.pGain->value;

	FilterType type = (FilterType)(int)b.pType->value;

	double scaleFactor = (double)pScale->value / 100.0;

	double dbGain = baseDbGain;
	if (type == FilterType::LowShelf || type == FilterType::Bell || type == FilterType::HighShelf) {
		dbGain *= scaleFactor;
	}

	if (pAdaptQ->value > 0.5f && type == FilterType::Bell) {
	}

	double A = std::pow(10.0, dbGain / 40.0);
	double w0 = 2.0 * M_PI * f0 / Fs;
	double alpha = std::sin(w0) / (2.0 * Q);
	double cosw0 = std::cos(w0);

	double a0 = 1.0;
	double& b0 = b.coeffs.b0;
	double& b1 = b.coeffs.b1;
	double& b2 = b.coeffs.b2;
	double& a1 = b.coeffs.a1;
	double& a2 = b.coeffs.a2;

	switch (type) {
	case FilterType::LowCut:
		b0 = (1 + cosw0) / 2;
		b1 = -(1 + cosw0);
		b2 = (1 + cosw0) / 2;
		a0 = 1 + alpha;
		a1 = -2 * cosw0;
		a2 = 1 - alpha;
		break;
	case FilterType::LowShelf:
		b0 = A * ((A + 1) - (A - 1) * cosw0 + 2 * std::sqrt(A) * alpha);
		b1 = 2 * A * ((A - 1) - (A + 1) * cosw0);
		b2 = A * ((A + 1) - (A - 1) * cosw0 - 2 * std::sqrt(A) * alpha);
		a0 = (A + 1) + (A - 1) * cosw0 + 2 * std::sqrt(A) * alpha;
		a1 = -2 * ((A - 1) + (A + 1) * cosw0);
		a2 = (A + 1) + (A - 1) * cosw0 - 2 * std::sqrt(A) * alpha;
		break;
	case FilterType::Bell:
		b0 = 1 + alpha * A;
		b1 = -2 * cosw0;
		b2 = 1 - alpha * A;
		a0 = 1 + alpha / A;
		a1 = -2 * cosw0;
		a2 = 1 - alpha / A;
		break;
	case FilterType::HighShelf:
		b0 = A * ((A + 1) + (A - 1) * cosw0 + 2 * std::sqrt(A) * alpha);
		b1 = -2 * A * ((A - 1) + (A + 1) * cosw0);
		b2 = A * ((A + 1) - (A - 1) * cosw0 - 2 * std::sqrt(A) * alpha);
		a0 = (A + 1) - (A - 1) * cosw0 + 2 * std::sqrt(A) * alpha;
		a1 = 2 * ((A - 1) - (A + 1) * cosw0);
		a2 = (A + 1) + (A - 1) * cosw0 - 2 * std::sqrt(A) * alpha;
		break;
	case FilterType::HighCut:
		b0 = (1 - cosw0) / 2;
		b1 = 1 - cosw0;
		b2 = (1 - cosw0) / 2;
		a0 = 1 + alpha;
		a1 = -2 * cosw0;
		a2 = 1 - alpha;
		break;
	}

	b0 /= a0;
	b1 /= a0;
	b2 /= a0;
	a1 /= a0;
	a2 /= a0;
}

void EqProcessor::Process(float* buffer, int numFrames, int numChannels, std::vector<MIDIMessage>& mIDIMessages, const ProcessContext& context) {
	(void)mIDIMessages;
	(void)context;

	for (int i = 0; i < kNumBands; ++i)
		RecalculateCoeffs(i);

	if (mStates.size() != (size_t)numChannels) {
		mStates.resize(numChannels);
		for (auto& ch : mStates)
			ch.resize(kNumBands);
	}

	float outputGain = std::pow(10.0f, pGlobalGain->value / 20.0f);
	EqMode mode = (EqMode)(int)pMode->value;

	for (int c = 0; c < numChannels; ++c) {
		bool processChannel = true;
		if (numChannels == 2) {
			if (mode == EqMode::Left && c == 1)
				processChannel = false;
			if (mode == EqMode::Right && c == 0)
				processChannel = false;
		}

		if (!processChannel) {
			continue;
		}

		for (int i = 0; i < numFrames; ++i) {
			double sample = buffer[i * numChannels + c];

			for (int b = 0; b < kNumBands; ++b) {
				const auto& co = mBands[b].coeffs;
				auto& s = mStates[c][b];

				double y = co.b0 * sample + s.z1;
				s.z1 = co.b1 * sample - co.a1 * y + s.z2;
				s.z2 = co.b2 * sample - co.a2 * y;
				sample = y;
			}

			sample *= outputGain;
			buffer[i * numChannels + c] = (float)sample;
		}
	}
}

std::complex<double> EqProcessor::GetBiquadResponse(const BiquadCoeffs& c, double freq) {
	double w = 2.0 * M_PI * freq / mSampleRate;
	std::complex<double> z1 = std::polar(1.0, -w);
	std::complex<double> z2 = std::polar(1.0, -2.0 * w);
	std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
	std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;
	if (std::abs(den) < 1e-9)
		return {1.0, 0.0};
	return num / den;
}

float EqProcessor::GetMagnitudeForFreq(double freq) {
	std::complex<double> response(1.0, 0.0);
	for (int i = 0; i < kNumBands; ++i) {
		if (mBands[i].pActive->value > 0.5f) {
			response *= GetBiquadResponse(mBands[i].coeffs, freq);
		}
	}
	return (float)std::abs(response);
}

bool EqProcessor::RenderCustomUI(const ImVec2& size) {
	const float leftPanelW = 90.0f;
	const float rightPanelW = 90.0f;
	const float graphH = size.y - 30.0f;
	const float centerW = size.x - leftPanelW - rightPanelW;

	if (centerW < 50.0f)
		return false;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

	const Theme& th = Theme::Instance();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();

	drawList->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), th.bgPanelAlt);

	// left panel
	ImGui::SetCursorScreenPos(ImVec2(p.x + 5, p.y + 10));
	ImGui::BeginGroup();

	if (mSelectedBandIndex >= 0 && mSelectedBandIndex < kNumBands) {
		auto& band = mBands[mSelectedBandIndex];

		ImGui::PushID("L_Freq");
		band.pFreq->Draw();
		ImGui::PopID();

		ImGui::Dummy(ImVec2(0, 10));

		ImGui::PushID("L_Gain");
		band.pGain->Draw();
		ImGui::PopID();

		ImGui::Dummy(ImVec2(0, 10));

		ImGui::PushID("L_Q");
		band.pQ->Draw();
		ImGui::PopID();

	} else {
		ImGui::TextDisabled("None");
	}
	ImGui::EndGroup();

	// center panel
	float graphX = p.x + leftPanelW;
	float graphY = p.y;
	float graphW = centerW;
	float tabsH = 30.0f;
	float actualGraphH = size.y - tabsH;

	drawList->AddRectFilled(ImVec2(graphX, graphY), ImVec2(graphX + graphW, graphY + actualGraphH), th.bgDeepest);

	float minFreq = 10.0f;
	float maxFreq = 22000.0f;
	float minLog = std::log10(minFreq);
	float scaleX = graphW / (std::log10(maxFreq) - minLog);

	const float freqPoints[] = {100, 1000, 10000};
	for (float f : freqPoints) {
		float x = graphX + (std::log10(f) - minLog) * scaleX;
		drawList->AddLine(ImVec2(x, graphY), ImVec2(x, graphY + actualGraphH), th.border);
		char buf[16];
		if (f >= 1000)
			snprintf(buf, 16, "%.0fk", f / 1000);
		else
			snprintf(buf, 16, "%.0f", f);
		drawList->AddText(ImVec2(x + 2, graphY + actualGraphH - 12), th.textDim, buf);
	}

	float maxDb = 15.0f;
	float rangeDb = 30.0f;
	float scaleY = actualGraphH / rangeDb;
	float zeroY = graphY + (maxDb * scaleY);

	drawList->AddLine(ImVec2(graphX, zeroY), ImVec2(graphX + graphW, zeroY), th.borderStrong);
	drawList->AddLine(ImVec2(graphX, zeroY - 6 * scaleY), ImVec2(graphX + graphW, zeroY - 6 * scaleY), th.border);
	drawList->AddLine(ImVec2(graphX, zeroY + 6 * scaleY), ImVec2(graphX + graphW, zeroY + 6 * scaleY), th.border);

	ImVec2 prevPos;
	for (int x = 0; x <= (int)graphW; x += 2) {
		float logFreq = minLog + ((float)x / scaleX);
		float freq = std::pow(10.0f, logFreq);
		float mag = GetMagnitudeForFreq(freq);
		float db = 20.0f * std::log10(mag);

		float y = zeroY - (db * scaleY);
		y = std::clamp(y, graphY, graphY + actualGraphH);

		ImVec2 curPos(graphX + x, y);
		if (x > 0) {
			drawList->AddLine(prevPos, curPos, th.graphCurve, 1.5f);
			drawList->AddLine(ImVec2(prevPos.x, prevPos.y), ImVec2(curPos.x, zeroY), Theme::WithAlpha(th.graphCurve, 20), 1.0f);
		}
		prevPos = curPos;
	}

	ImGui::SetCursorScreenPos(ImVec2(graphX, graphY));
	ImGui::InvisibleButton("GraphInteract", ImVec2(graphW, actualGraphH));
	bool graphHovered = ImGui::IsItemHovered();
	bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
	ImVec2 mousePos = ImGui::GetMousePos();

	if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && graphHovered) {
		float bestDist = 20.0f;
		int bestIdx = -1;

		float mouseFreq = std::pow(10.0f, minLog + (mousePos.x - graphX) / scaleX);
		float mouseDb = (zeroY - mousePos.y) / scaleY;

		for (int i = 0; i < kNumBands; ++i) {
			if (mBands[i].pActive->value < 0.5f)
				continue;

			float bx = graphX + (std::log10(mBands[i].pFreq->value) - minLog) * scaleX;
			float by = zeroY - (mBands[i].pGain->value * scaleY);

			float dist = std::sqrt(std::pow(mousePos.x - bx, 2) + std::pow(mousePos.y - by, 2));
			if (dist < bestDist) {
				bestDist = dist;
				bestIdx = i;
			}
		}

		if (bestIdx != -1) {
			mSelectedBandIndex = bestIdx;
			mDraggingNode = true;
		}
	}

	if (!mouseDown)
		mDraggingNode = false;

	if (mDraggingNode && mSelectedBandIndex != -1) {
		float newLogFreq = minLog + ((mousePos.x - graphX) / scaleX);
		float newGain = (zeroY - mousePos.y) / scaleY;

		mBands[mSelectedBandIndex].pFreq->value = std::clamp(std::pow(10.0f, newLogFreq), 10.0f, 22000.0f);

		FilterType t = (FilterType)(int)mBands[mSelectedBandIndex].pType->value;
		if (t == FilterType::Bell || t == FilterType::LowShelf || t == FilterType::HighShelf) {
			mBands[mSelectedBandIndex].pGain->value = std::clamp(newGain, -15.0f, 15.0f);
		}

		if (ImGui::GetIO().KeyAlt) {
			float dy = ImGui::GetMouseDragDelta(0).y;
			mBands[mSelectedBandIndex].pQ->value = std::clamp(mBands[mSelectedBandIndex].pQ->value + dy * 0.05f, 0.1f, 18.0f);
		}
	}

	for (int i = 0; i < kNumBands; ++i) {
		if (mBands[i].pActive->value < 0.5f)
			continue;

		float bx = graphX + (std::log10(mBands[i].pFreq->value) - minLog) * scaleX;
		float by = zeroY - (mBands[i].pGain->value * scaleY);

		bool isSel = (i == mSelectedBandIndex);
		ImU32 col = isSel ? th.accentHover : Theme::WithAlpha(th.accent, 180);

		drawList->AddCircleFilled(ImVec2(bx, by), 4.0f, col);
		drawList->AddCircle(ImVec2(bx, by), 5.0f, th.divider);

		if (isSel) {
			char buf[4];
			sprintf(buf, "%d", i + 1);
			drawList->AddText(ImVec2(bx + 6, by - 6), th.text, buf);
		}
	}

	float tabW = graphW / kNumBands;
	ImGui::SetCursorScreenPos(ImVec2(graphX, graphY + actualGraphH));

	for (int i = 0; i < kNumBands; ++i) {
		ImGui::PushID(i);
		ImVec2 tabP = ImGui::GetCursorScreenPos();

		bool isSel = (i == mSelectedBandIndex);
		bool isActive = mBands[i].pActive->value > 0.5f;

		ImU32 bgCol = isSel ? th.bgActive : th.bgPanel;
		drawList->AddRectFilled(tabP, ImVec2(tabP.x + tabW, tabP.y + tabsH), bgCol);
		drawList->AddRect(tabP, ImVec2(tabP.x + tabW, tabP.y + tabsH), th.divider);

		ImGui::InvisibleButton("TabHit", ImVec2(tabW, tabsH));
		if (ImGui::IsItemClicked()) {
			mSelectedBandIndex = i;
		}

		ImVec2 toggleP(tabP.x + 3, tabP.y + 3);
		ImU32 toggleCol = isActive ? th.accent : th.border;
		drawList->AddRectFilled(toggleP, ImVec2(toggleP.x + 8, toggleP.y + 8), toggleCol);
		if (ImGui::IsItemClicked() && ImGui::GetIO().KeyCtrl) {
			mBands[i].pActive->value = isActive ? 0.0f : 1.0f;
		}

		char numBuf[4];
		sprintf(numBuf, "%d", i + 1);
		drawList->AddText(ImVec2(tabP.x + tabW * 0.5f - 4, tabP.y + 2), th.textMuted, numBuf);

		FilterType fType = (FilterType)(int)mBands[i].pType->value;
		DrawFilterIcon(fType, ImVec2(tabP.x + tabW * 0.5f - 6, tabP.y + 15), 12, 10, isActive ? th.text : th.textDim);

		if (isSel && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
			ImGui::OpenPopup("TypeSel");
		}
		if (ImGui::BeginPopup("TypeSel")) {
			if (ImGui::MenuItem("Low Cut"))
				mBands[i].pType->value = 0.0f;
			if (ImGui::MenuItem("Low Shelf"))
				mBands[i].pType->value = 1.0f;
			if (ImGui::MenuItem("Bell"))
				mBands[i].pType->value = 2.0f;
			if (ImGui::MenuItem("High Shelf"))
				mBands[i].pType->value = 3.0f;
			if (ImGui::MenuItem("High Cut"))
				mBands[i].pType->value = 4.0f;
			ImGui::EndPopup();
		}

		ImGui::SameLine();
		ImGui::PopID();
	}

	// right panel
	ImGui::SetCursorScreenPos(ImVec2(graphX + graphW + 5, p.y + 5));
	ImGui::BeginGroup();

	const char* modes[] = {"Stereo", "L", "R", "M", "S"};
	int curMode = (int)pMode->value;
	ImGui::SetNextItemWidth(80);
	if (ImGui::Combo("##Mode", &curMode, modes, 5)) {
		pMode->value = (float)curMode;
	}

	ImGui::Button("Edit A", ImVec2(80, 18));
	ImGui::Dummy(ImVec2(0, 5));

	bool adapt = pAdaptQ->value > 0.5f;
	if (ImGui::Checkbox("Adapt. Q", &adapt))
		pAdaptQ->value = adapt ? 1.0f : 0.0f;

	ImGui::Dummy(ImVec2(0, 10));

	ImGui::PushID("R_Scale");
	pScale->Draw();
	ImGui::PopID();

	ImGui::Dummy(ImVec2(0, 10));

	ImGui::PushID("R_Gain");
	pGlobalGain->Draw();
	ImGui::PopID();

	ImGui::EndGroup();

	ImGui::PopStyleVar(2);
	return true;
}
