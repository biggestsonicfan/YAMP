#include "AdxDecoder.h"

#include <cmath>

namespace cri::adx
{
	namespace
	{
		uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
		uint32_t Be32(const uint8_t* p)
		{
			return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
		}
	}

	bool ParseHeader(const uint8_t* data, size_t size, Info& out)
	{
		if (!data || size < 20 || Be16(data) != 0x8000)
			return false;

		out = Info{};
		out.dataOffset = Be16(data + 2);
		out.encoding = data[4];
		out.blockSize = data[5];
		out.bitsPerSample = data[6];
		out.channelCount = data[7];
		out.samplingRate = Be32(data + 8);
		out.sampleCount = Be32(data + 12);
		out.highpassFreq = Be16(data + 16);
		out.version = data[18];
		out.flags = data[19];

		if (out.encoding != 3 || out.blockSize < 3 || out.bitsPerSample != 4)
			return false;
		if (out.channelCount < 1 || out.channelCount > 8)
			return false;
		if (out.samplingRate == 0 || out.sampleCount == 0)
			return false;
		if (out.flags & 0x08) // encrypted (0x08/0x09) — nothing shipped uses it
			return false;
		if (size < size_t(out.dataOffset) + 4)
			return false;

		// Loop metadata sits in the header tail; field layout differs between v3 and v4
		// (v4 inserts per-channel history words at 0x18 first).
		if (out.version == 3 && out.dataOffset >= 0x28)
		{
			if (Be32(data + 0x18) != 0)
			{
				out.hasLoop = true;
				out.loopStartSample = Be32(data + 0x1C);
				out.loopEndSample = Be32(data + 0x24);
			}
		}
		else if (out.version == 4 && out.dataOffset >= 0x34)
		{
			if (Be32(data + 0x24) != 0)
			{
				out.hasLoop = true;
				out.loopStartSample = Be32(data + 0x28);
				out.loopEndSample = Be32(data + 0x30);
			}
		}
		if (out.hasLoop &&
			(out.loopEndSample <= out.loopStartSample || out.loopEndSample > out.sampleCount))
			out.hasLoop = false;

		// Prediction coefficients from the highpass cutoff (the standard ADX derivation).
		const double z = std::cos(2.0 * 3.14159265358979323846 *
			double(out.highpassFreq) / double(out.samplingRate));
		const double a = 1.41421356237309504880 - z;
		const double b = 1.41421356237309504880 - 1.0;
		const double c = (a - std::sqrt((a + b) * (a - b))) / b;
		out.coef1 = int16_t(std::lround(c * 8192.0));
		out.coef2 = int16_t(std::lround(c * c * -4096.0));
		return true;
	}

	uint32_t DecodeAll(const Info& info, const uint8_t* data, size_t size, int16_t* out)
	{
		if (!data || !out)
			return 0;
		const size_t dataStart = size_t(info.dataOffset) + 4;
		if (size <= dataStart)
			return 0;

		const uint32_t ch = info.channelCount;
		const uint32_t samplesPerBlock = uint32_t(info.blockSize - 2) * 2;
		const uint32_t frames =
			(info.sampleCount + samplesPerBlock - 1) / samplesPerBlock;

		int32_t hist1[8] = {};
		int32_t hist2[8] = {};

		uint32_t done = 0;
		for (uint32_t f = 0; f < frames; ++f)
		{
			const uint32_t remaining = info.sampleCount - done;
			const uint32_t n = remaining < samplesPerBlock ? remaining : samplesPerBlock;
			for (uint32_t c = 0; c < ch; ++c)
			{
				const size_t blockOff = dataStart + (size_t(f) * ch + c) * info.blockSize;
				if (blockOff + info.blockSize > size)
					return done;
				const uint8_t* p = data + blockOff;
				const int32_t scale = Be16(p);
				p += 2;

				int32_t s1 = hist1[c];
				int32_t s2 = hist2[c];
				int16_t* dst = out + (size_t(done) * ch) + c;
				for (uint32_t i = 0; i < n; ++i)
				{
					// 4-bit deltas, high nibble first; predictor >> 12 with the delta
					// pre-shifted so rounding matches the CRI/ffmpeg arithmetic exactly.
					const uint8_t nib = (i & 1) ? (p[i >> 1] & 0x0F) : (p[i >> 1] >> 4);
					const int32_t d = int32_t(nib << 28) >> 28; // sign-extend 4 bits
					int64_t s = (int64_t(d) << 12) * scale +
						int64_t(info.coef1) * s1 + int64_t(info.coef2) * s2;
					s >>= 12;
					if (s > 32767) s = 32767;
					else if (s < -32768) s = -32768;
					s2 = s1;
					s1 = int32_t(s);
					dst[size_t(i) * ch] = int16_t(s);
				}
				hist1[c] = s1;
				hist2[c] = s2;
			}
			done += n;
		}
		return done;
	}
}
