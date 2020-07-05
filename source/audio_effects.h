#pragma once

namespace AudioEffects {
	void BitCrush(char* sampleBuffer, int samples, float quant) {
		for (int i = 0; i < samples; i++) {
			short* ptr = (short*)&sampleBuffer + i;

			//Signed shorts range from -32768 to 32767
			//Let's quantize that a bit
			float f = (float)*ptr;
			f /= quant;
			*ptr = (short)f;
			*ptr *= quant;
			*ptr *= 1.5;
		}
	}
}