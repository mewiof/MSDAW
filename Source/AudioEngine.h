#pragma once
#include <RtAudio.h>
#include <vector>
#include <mutex>
#include <memory>
#include "Project.h"
#include "MIDITypes.h"
#include "PreviewPlayer.h"

class AudioEngine {
public:
	AudioEngine();
	~AudioEngine();

	bool Init();
	void Start();
	void Stop();

	// check if hardware stream is running
	bool IsStreamRunning() const;

	Project* GetProject() { return mProject.get(); }

	// inject live MIDI event
	void SendMIDIEvent(int status, int note, int velocity);

	// auditioning a file from the library sits beside the project rather than inside
	// it, so it plays with the transport stopped and leaves nothing behind
	PreviewPlayer& GetPreviewPlayer() { return mPreview; }

	int OnAudioCallback(void* outputBuffer, void* inputBuffer, unsigned int nBufferFrames,
						double streamTime, RtAudioStreamStatus status, void* userData);
private:
	RtAudio dac;
	bool isStreamOpen = false;
	double sampleRate = 48000.0;

	// the current loaded project
	std::unique_ptr<Project> mProject;

	PreviewPlayer mPreview;

	// thread safety for realtime MIDI injection
	std::mutex mMIDIMutex;
	std::vector<MIDIMessage> mPendingMIDIMessages;
};
