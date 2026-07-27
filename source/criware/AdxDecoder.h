#pragma once

// CRI ADX decoder (standard 4-bit ADPCM, encoding type 3) — the format VF5FS streams its BGM
// and voice files in (SetFile path). Header is big-endian; each 18-byte block holds a 16-bit
// scale plus 32 4-bit deltas for one channel; blocks alternate channels. The two prediction
// coefficients derive from the header's highpass cutoff. Validated sample-exact against
// ffmpeg's decoder on the shipped VF5FS files.

#include <cstdint>
#include <cstddef>

namespace cri::adx
{
	struct Info
	{
		uint16_t dataOffset = 0;   // samples start at dataOffset + 4
		uint8_t encoding = 0;      // 3 = standard (only supported type; 2 = fixed-coef, unused)
		uint8_t blockSize = 0;     // 18 for every shipped file
		uint8_t bitsPerSample = 0; // 4
		uint8_t channelCount = 0;
		uint32_t samplingRate = 0;
		uint32_t sampleCount = 0;  // per channel
		uint16_t highpassFreq = 0;
		uint8_t version = 0;       // 3 or 4
		uint8_t flags = 0;         // 0x08/0x09 = encrypted (rejected)

		bool hasLoop = false;
		uint32_t loopStartSample = 0;
		uint32_t loopEndSample = 0; // exclusive

		int16_t coef1 = 0;         // derived from highpassFreq/samplingRate
		int16_t coef2 = 0;
	};

	// Parses the header (and computes the prediction coefficients). Returns false on a bad
	// magic, unsupported encoding, or encryption.
	bool ParseHeader(const uint8_t* data, size_t size, Info& out);

	// Decodes the whole stream into interleaved s16 PCM. `out` must hold
	// info.sampleCount * info.channelCount samples. Returns samples decoded per channel.
	uint32_t DecodeAll(const Info& info, const uint8_t* data, size_t size, int16_t* out);
}
