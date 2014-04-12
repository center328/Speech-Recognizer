#include <iostream>
#include <fstream>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <cassert>
#include <vector>
#include "audio.h"
#include "WavData.h"
#include "Frame.h"
#include "Word.h"

using namespace std;

namespace wtm {
namespace audio {

/**
 * Read Wav data from a file
 */
WavDataPtr WavData::readFromFile(const std::string& file) {
	WavHeader wavHeader;

	// Open file
	std::fstream fs;
	fs.open(file.c_str(), std::ios::in | std::ios::binary);

	if (!fs.good()) {
		fprintf(stderr, "Can't open the wave file\n");
		exit(EXIT_FAILURE);
	}

	// Read header
	fs.read((char*)(&wavHeader), sizeof(WavHeader));
	checkHeader(wavHeader);

	// Read raw data
	WavDataPtr wavData(new WavData(wavHeader));
	readRawData(fs, wavHeader, *wavData);
	fs.close();

	return wavData;
}

void WavData::init() {

	this->divideIntoFrames();
	this->divideIntoWords();
}

/**
 * Checks a set of restrictions
 */
void WavData::checkHeader(const WavHeader& wavHeader) {

	if (0 != strncmp(wavHeader.riff, "RIFF", sizeof(wavHeader.riff))
			|| 0 != strncmp(wavHeader.wave, "WAVE", sizeof(wavHeader.wave))) {
		fprintf(stderr, "Invalid RIFF/WAVE format\n");
		exit(EXIT_FAILURE);
	}

	if (1 != wavHeader.audioFormat) {
		fprintf(stderr, "Invalid WAV format: only PCM audio format is supported\n");
		exit(EXIT_FAILURE);
	}

	if (wavHeader.numOfChan > 2) {
		fprintf(stderr, "Invalid WAV format: only 1 or 2 channels audio is supported\n");
		exit(EXIT_FAILURE);
	}

	unsigned long bitsPerChannel = wavHeader.bitsPerSample / wavHeader.numOfChan;
	if (8 != bitsPerChannel && 16 != bitsPerChannel) {
		fprintf(stderr, "Invalid WAV format: only 8 and 16-bit per channel is supported\n");
		exit(EXIT_FAILURE);
	}

	if (wavHeader.subchunk2Size > LONG_MAX) {
		fprintf(stderr, "File too big\n");
		exit(EXIT_FAILURE);
	}
}

void WavData::readRawData(std::fstream& fs, const WavHeader& wavHeader, WavData& wavFile) {
	raw_t value, minValue = 0, maxValue = 0;
	uint8_t value8, valueLeft8, valueRight8;
	int16_t value16, valueLeft16, valueRight16;

	lenght_t bytesPerSample = static_cast<uint32_t>(wavHeader.bitsPerSample / 8);
	unsigned long numberOfSamplesXChannels = wavHeader.subchunk2Size /
			(wavHeader.numOfChan * bytesPerSample);

	unsigned long sampleNumber = 0;
	for (; sampleNumber < numberOfSamplesXChannels && !fs.eof(); sampleNumber++) {

		if (8 == wavHeader.bitsPerSample) {
			if (1 == wavHeader.numOfChan) {
				fs.read((char*)(&value8), sizeof(uint8_t));
				value = static_cast<raw_t>(value8);

			} else {
				fs.read((char*)(&valueLeft8), sizeof(uint8_t));
				fs.read((char*)(&valueRight8), sizeof(uint8_t));
				value = static_cast<raw_t>((abs(valueLeft8) + abs(valueRight8)) / 2);
			}
		} else {
			if (1 == wavHeader.numOfChan) {
				fs.read((char*)(&value16), sizeof(int16_t));
				value = static_cast<raw_t>(value16);

			} else {
				fs.read((char*)(&valueLeft16), sizeof(int16_t));
				fs.read((char*)(&valueRight16), sizeof(int16_t));
				value = static_cast<raw_t>((abs(valueLeft16) + abs(valueRight16)) / 2);
			}
		}

		if (maxValue < value) {
			maxValue = value;
		}

		if (minValue > value) {
			minValue = value;
		}

		wavFile.rawData->push_back(value);
	}
	assert(sampleNumber > 0);

	// Update values
	wavFile.setMinVal(minValue);
	wavFile.setMaxVal(maxValue);
	wavFile.setNumberOfSamples(sampleNumber);

	lenght_t bytesPerFrame = static_cast<lenght_t>(wavHeader.bytesPerSec * FRAME_LENGTH / 1000.0);
	wavFile.samplesPerFrame = static_cast<lenght_t>(bytesPerFrame / bytesPerSample);
	assert(wavFile.samplesPerFrame > 0);
}

void WavData::divideIntoFrames() {
	assert(header.subchunk2Size > 0);

	unsigned int samplesPerNonOverlap =
		static_cast<unsigned int>(samplesPerFrame * (1 - FRAME_OVERLAP));
	unsigned int framesCount =
		(header.subchunk2Size / (header.bitsPerSample / 8)) / samplesPerNonOverlap;

	frames->reserve(framesCount);

	lenght_t indexBegin = 0, indexEnd = 0;
	for (lenght_t i = 0, size = rawData->size(); i < framesCount; ++i) {

		indexBegin = i * samplesPerNonOverlap;
		indexEnd = indexBegin + samplesPerFrame;
		if (indexEnd < size) {

			Frame* frame = new Frame(*rawData, indexBegin, indexEnd);
			frames->push_back(frame);
		} else {
			break;
		}
	}
}

void WavData::divideIntoWords() {
	assert(frames->size() > 10);

	double maMin = 0;
	double maMax = 0;
	double maAvg = 0;
	double ma;

	// Let's use Moving Average value to avoid spikes
	unsigned short maShift = MOVING_AVERAGE_SIZE / 2;
	for (lenght_t i = maShift; i < frames->size() - maShift; ++i) {

		ma = 0;
		for (unsigned short iMa = i - maShift; iMa <= i + maShift; iMa++) {
			ma += frames->at(i)->calcRMS();
		}
		ma /= MOVING_AVERAGE_SIZE;
		frames->at(i)->setMaRms(ma);

		if (maMin > ma) {
			maMin = ma;
		}
		if (maMax < ma) {
			maMax = ma;
		}

		maAvg = (maAvg + ma) / 2;
	}

	// If max value greater than min value more then 50% then we have the "silence" threshold.
	// Otherwise, let's think that we have only one world.
	double threshold = 0;
	if (maMax * 0.5 > maMin) {
		threshold = min(maMin * 5 , maAvg * 0.3);

		// Divide frames into words
		std::vector<Frame*>* wordFrames = 0;
		for (vector<Frame*>::const_iterator frame = frames->begin();
				frame != frames->end(); ++frame) {

			// Got a sound
			if ((*frame)->getMaRms() > threshold) {

				if (!wordFrames) {
					wordFrames = new std::vector<Frame*>;
				}
				wordFrames->push_back(*frame);

			// Got a silence
			} else {
				if (wordFrames) {
					this->words->push_back(new Word(wordFrames));
					wordFrames = 0;
				}
			}
		}

	// Seems we have only one word
	} else {
		this->words->push_back(new Word(this->frames));
	}
}

void WavData::saveToFile(const std::string& file, const Word& word) const {

	// number of data bytes in the resulting wave file
	unsigned int waveSize = word.getFrames().size() * (1 - FRAME_OVERLAP) * sizeof(raw_t);
	unsigned int samplesPerNonOverlap =
			static_cast<unsigned int>(samplesPerFrame * (1 - FRAME_OVERLAP));

	// prepare a new header and write it to file stream
	WavHeader headerNew;
	strncpy(headerNew.riff, this->header.riff, 4);
	headerNew.chunkSize = waveSize + sizeof(WavHeader);
	strncpy(headerNew.wave, this->header.wave, 4);
	strncpy(headerNew.fmt, this->header.fmt, 4);
	headerNew.subchunk1Size = this->header.subchunk1Size;
	headerNew.audioFormat = this->header.audioFormat;
	headerNew.numOfChan = 1;
	headerNew.samplesPerSec = this->header.samplesPerSec;
	headerNew.bytesPerSec = this->header.samplesPerSec * sizeof(raw_t) / 8;
	headerNew.blockAlign = sizeof(raw_t) / 8;
	headerNew.bitsPerSample = sizeof(raw_t);
	strncpy(headerNew.data, this->header.data, 4);
	headerNew.subchunk2Size = waveSize;

	std::fstream fs;
	fs.open(file.c_str(), std::ios::out | std::ios::binary);
	fs.write((char*)&headerNew, sizeof(WavHeader));

	raw_t* data = new raw_t[waveSize / sizeof(raw_t)];

	int frameNumber = 0;
	for (vector<Frame*>::const_iterator frame = word.getFrames().begin();
			frame != word.getFrames().end(); ++frame) {

		for (lenght_t i = 0; i < samplesPerNonOverlap; i++) {
			data[frameNumber * samplesPerNonOverlap + i ] =
					this->rawData->at((*frame)->getStart() + i);
		}

		frameNumber++;
	}

	fs.write((char*)data, waveSize);
	fs.close();
	delete [] data;
}

} // namespace audio
} // namespace wtm
