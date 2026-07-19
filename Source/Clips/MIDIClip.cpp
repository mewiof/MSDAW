#include "PrecompHeader.h"
#include "MIDIClip.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <map>

MIDIClip::MIDIClip() {
	mName = "MIDI Clip";
	// allocate a fresh sequence
	mSequence = std::make_shared<MIDISequence>();
}

// helpers for big-endian parsing
static uint16_t ReadBE16(std::ifstream& f) {
	uint8_t b[2];
	f.read((char*)b, 2);
	return (b[0] << 8) | b[1];
}

static uint32_t ReadBE32(std::ifstream& f) {
	uint8_t b[4];
	f.read((char*)b, 4);
	return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
}

// a note that is currently sounding while parsing: we must remember the
// attack velocity seen at note-on, since the matching note-off carries the
// (usually meaningless) release velocity instead
struct ParsingActiveNote {
	uint32_t startTick;
	uint8_t onVelocity;
};

static uint32_t ReadVLQ(std::ifstream& f, uint32_t& bytesRead) {
	uint32_t value = 0;
	uint8_t byte;
	do {
		f.read((char*)&byte, 1);
		bytesRead++;
		value = (value << 7) | (byte & 0x7F);
	} while (byte & 0x80);
	return value;
}

bool MIDIClip::LoadFromFile(const std::string& path) {

	if (!mSequence)
		mSequence = std::make_shared<MIDISequence>();

	std::ifstream file(path, std::ios::binary);
	if (!file.is_open()) {
		std::cout << "Failed to open MIDI file: " << path << "\n";
		return false;
	}

	// 1. read header chunk (mthd)
	char chunkId[4];
	file.read(chunkId, 4);
	if (memcmp(chunkId, "MThd", 4) != 0)
		return false;

	uint32_t headerSize = ReadBE32(file);
	uint16_t formatType = ReadBE16(file);
	uint16_t numTracks = ReadBE16(file);
	uint16_t timeDivision = ReadBE16(file);

	if (headerSize > 6)
		file.seekg(headerSize - 6, std::ios::cur);

	if ((timeDivision & 0x8000) != 0) {
		std::cout << "SMPTE time division not supported yet.\n";
		return false;
	}

	mSequence->notes.clear();
	double maxBeat = 0.0;

	// 2. read tracks (mtrk)
	for (int t = 0; t < numTracks; ++t) {
		file.read(chunkId, 4);
		while (memcmp(chunkId, "MTrk", 4) != 0 && file.good()) {
			uint32_t skipSize = ReadBE32(file);
			file.seekg(skipSize, std::ios::cur);
			file.read(chunkId, 4);
		}
		if (!file.good())
			break;

		uint32_t trackSize = ReadBE32(file);
		uint32_t bytesRead = 0;
		uint32_t currentTick = 0;
		uint8_t runningStatus = 0;

		std::map<int, std::map<int, ParsingActiveNote>> activeNotes;

		while (bytesRead < trackSize) {
			uint32_t vlqBytes = 0;
			uint32_t deltaTime = ReadVLQ(file, vlqBytes);
			bytesRead += vlqBytes;
			currentTick += deltaTime;

			uint8_t status = 0;
			file.read((char*)&status, 1);

			if (status & 0x80) {
				bytesRead++;
				runningStatus = status;
			} else {
				file.seekg(-1, std::ios::cur);
				status = runningStatus;
			}

			uint8_t type = status & 0xF0;
			uint8_t channel = status & 0x0F;

			if (type == 0x80 || type == 0x90) { // note off or note on
				uint8_t note, velocity;
				file.read((char*)&note, 1);
				file.read((char*)&velocity, 1);
				bytesRead += 2;

				bool isNoteOn = (type == 0x90) && (velocity > 0);
				bool isNoteOff = (type == 0x80) || ((type == 0x90) && (velocity == 0));

				if (isNoteOn) {
					activeNotes[channel][note] = {currentTick, velocity};
				} else if (isNoteOff) {
					if (activeNotes[channel].count(note)) {
						ParsingActiveNote active = activeNotes[channel][note];
						uint32_t startTick = active.startTick;
						activeNotes[channel].erase(note);

						MIDINote newNote;
						newNote.noteNumber = note;
						// use the attack velocity captured at note-on, clamped to a valid
						// audible range (a stray 0 would be silent)
						newNote.velocity = std::clamp((int)active.onVelocity, 1, 127);
						newNote.startBeat = (double)startTick / (double)timeDivision;
						newNote.durationBeats = (double)(currentTick - startTick) / (double)timeDivision;

						mSequence->notes.push_back(newNote);

						double endBeat = newNote.startBeat + newNote.durationBeats;
						if (endBeat > maxBeat)
							maxBeat = endBeat;
					}
				}
			} else if (type == 0xC0 || type == 0xD0) {
				char dummy;
				file.read(&dummy, 1);
				bytesRead++;
			} else if (type == 0xB0 || type == 0xE0 || type == 0xA0) {
				char dummy[2];
				file.read(dummy, 2);
				bytesRead += 2;
			} else if (status == 0xF0 || status == 0xF7) {
				uint32_t lenBytes = 0;
				uint32_t len = ReadVLQ(file, lenBytes);
				bytesRead += lenBytes;
				file.seekg(len, std::ios::cur);
				bytesRead += len;
			} else if (status == 0xFF) {
				uint8_t metaType;
				file.read((char*)&metaType, 1);
				bytesRead++;

				uint32_t lenBytes = 0;
				uint32_t len = ReadVLQ(file, lenBytes);
				bytesRead += lenBytes;

				file.seekg(len, std::ios::cur);
				bytesRead += len;

				if (metaType == 0x2F) {
					break;
				}
			}
		}
	}

	mDuration = maxBeat;
	if (mDuration < 1.0)
		mDuration = 4.0;

	return true;
}

void MIDIClip::MakeUnique() {
	if (mSequence) {
		// deep copy the sequence
		auto newSeq = std::make_shared<MIDISequence>(*mSequence);
		mSequence = newSeq;
		mName += " (Unique)";
	}
}

void MIDIClip::Save(std::ostream& out) {
	Clip::Save(out); // saves offset
	for (const auto& n : mSequence->notes) {
		out << "NOTE " << n.noteNumber << " " << n.velocity << " " << n.startBeat << " " << n.durationBeats << "\n";
	}
}

void MIDIClip::Load(std::istream& in) {
	if (!mSequence)
		mSequence = std::make_shared<MIDISequence>();
	mSequence->notes.clear();

	std::string line;
	while (std::getline(in, line)) {
		if (line == "CLIP_END")
			break;

		// parse base fields
		if (line.rfind("CLIP_NAME ", 0) == 0) {
			size_t q1 = line.find('"');
			size_t q2 = line.find('"', q1 + 1);
			if (q1 != std::string::npos && q2 != std::string::npos) {
				mName = line.substr(q1 + 1, q2 - q1 - 1);
			}
		} else if (line.rfind("START ", 0) == 0) {
			mStartBeat = std::stod(line.substr(6));
		} else if (line.rfind("DUR ", 0) == 0) {
			mDuration = std::stod(line.substr(4));
		} else if (line.rfind("OFFSET ", 0) == 0) {
			mOffset = std::stod(line.substr(7));
		}

		// parse MIDI fields
		if (line.rfind("NOTE ", 0) == 0) {
			MIDINote n;
			if (sscanf_s(line.c_str(), "NOTE %d %d %lf %lf", &n.noteNumber, &n.velocity, &n.startBeat, &n.durationBeats) == 4) {
				mSequence->notes.push_back(n);
			}
		}
	}
}
