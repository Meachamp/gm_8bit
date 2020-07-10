#pragma once
#include <cstdint>
#include <ivoicecodec.h>

namespace SteamVoice {
	enum {
		OP_SILENCE = 0,
		OP_CODEC_OPUSPLC = 6,
		OP_SAMPLERATE = 11
	};

	int DecompressIntoBuffer(IVoiceCodec* codec, char* compressedData, int compressedLen, char* decompressedOut, int maxDecompressed) {
		char* curRead = compressedData;
		char* maxRead = compressedData + compressedLen;
		char* curWrite = decompressedOut;
		char* maxWrite = decompressedOut + maxDecompressed;
		while (curRead < maxRead) {
			//Check to make sure we have one byte of buffer space remaining at least
			if (curRead + 1 >= maxRead)
				return -1;

			//Get the current packet opcode
			char opcode = *curRead;
			curRead++;

			switch (opcode) {
			case OP_SILENCE: {
				//Contains a number of silence samples to add to the decompressed data. Skip for now.
				if (curRead + 2 >= maxRead)
					return -1;

				curRead += 2;
				break;
			}
			case OP_SAMPLERATE: {
				//Contains the samplerate for the stream. Always 24000 as far as I can tell.
				if (curRead + 2 >= maxRead)
					return -1;

				uint16_t sampleRate = *(uint16_t*)curRead;
				sampleRate;
				curRead += 2;
				break;
			}
			case OP_CODEC_OPUSPLC: {
				//Contains length plus a number of steam opus frames
				if (curRead + 2 >= maxRead)
					return -1;

				uint16_t frameDataLen = *(uint16_t*)curRead;
				if (curRead + frameDataLen >= maxRead)
					return -1;

				int decompressedBytes = codec->Decompress(curRead, frameDataLen, curWrite, maxWrite-curWrite);
				if (decompressedBytes <= 0)
					return -1;

				curWrite += decompressedBytes;
				curRead += frameDataLen;
				break;
			}
			default:
				return -1;
			}
		}

		return curWrite - decompressedOut;
	}
}