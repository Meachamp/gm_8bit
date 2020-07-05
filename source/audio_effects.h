#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>

namespace AudioEffects {
	void BitCrush(uint16* sampleBuffer, int samples, float quant) {
		for (int i = 0; i < samples; i++) {
			//Signed shorts range from -32768 to 32767
			//Let's quantize that a bit
			float f = (float)sampleBuffer[i];
			f /= quant;
			sampleBuffer[i] = (uint16)f;
			sampleBuffer[i] *= quant;
			sampleBuffer[i] *= 1.5;
		}
	}

	static uint16 tempBuf[10 * 1024];
	void Desample(uint16* inBuffer, int& samples) {
		assert(samples / 2 + 1 <= sizeof(tempBuf));
		int outIdx = 0;
		for (int i = 0; i < samples; i++) {
			if (i % 2 == 1) continue;

			tempBuf[outIdx] = inBuffer[i];
			outIdx++;
		}
		std::memcpy(inBuffer, tempBuf, outIdx * 2);
	}
}