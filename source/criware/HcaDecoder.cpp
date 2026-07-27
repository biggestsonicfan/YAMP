#include "HcaDecoder.h"
#include "HcaTables.h"

#include <cstring>

// Stage provenance: each stage was first reconstructed from the CRI runtime in Yakuza 6
// (function addresses cited per stage), then corrected/completed against the ClHcaSharp
// reference decoder (C:\temp\clhca), whose output on the shipped StF data is the validated
// oracle. Where the two disagreed, the reference pipeline won — the original Y6 transcription
// missed the resolution-range gain factor and the per-type coded counts, which is what kept
// this decoder "recognizable but wrong" for several sessions.

namespace cri::hca
{
	using namespace tables;

	namespace
	{
		// Chunk tags are matched with the high bit of every byte cleared, exactly as the CRI
		// reader does (HCAHeader_Read @0x1436c5e28) — that is what lets an encrypted header
		// still be identified.
		constexpr uint32_t Tag(const uint8_t* p)
		{
			return (uint32_t(p[0] & 0x7F) << 24) | (uint32_t(p[1] & 0x7F) << 16) |
				(uint32_t(p[2] & 0x7F) << 8) | uint32_t(p[3] & 0x7F);
		}
		constexpr uint32_t kTagHCA = 0x48434100; // "HCA\0"
		constexpr uint32_t kTagFmt = 0x666D7400; // "fmt\0"
		constexpr uint32_t kTagComp = 0x636F6D70; // "comp"
		constexpr uint32_t kTagDec = 0x64656300; // "dec\0"
		constexpr uint32_t kTagVbr = 0x76627200; // "vbr\0"
		constexpr uint32_t kTagAth = 0x61746800; // "ath\0"
		constexpr uint32_t kTagLoop = 0x6C6F6F70; // "loop"
		constexpr uint32_t kTagCiph = 0x63697068; // "ciph"
		constexpr uint32_t kTagRva = 0x72766100; // "rva\0"
		constexpr uint32_t kTagComm = 0x636F6D6D; // "comm"

		uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
		uint32_t Be32(const uint8_t* p)
		{
			return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
		}
	}

	// MSB-first bit reader over one frame. Absolute bit cursor so short rewinds (Skip of a
	// negative count, used by the coefficient unpack) are trivial. Peeking past the end of the
	// frame yields 0, matching the reference reader.
	class BitReader
	{
	public:
		BitReader(const uint8_t* data, size_t size)
			: m_data(data), m_sizeBits(int64_t(size) * 8) {}

		// n <= 16
		uint32_t Peek(int n) const
		{
			if (m_bit < 0 || m_bit + n > m_sizeBits)
				return 0;
			const size_t byteIdx = size_t(m_bit >> 3);
			const int r = int(m_bit & 7);
			const size_t byteSize = size_t(m_sizeBits >> 3);
			uint64_t win = 0;
			for (int i = 0; i < 5; ++i)
			{
				win <<= 8;
				if (byteIdx + i < byteSize)
					win |= m_data[byteIdx + i];
			}
			return uint32_t(win >> (40 - r - n)) & ((n < 16) ? kQuantBitMask[n] : 0xFFFFu);
		}

		uint32_t Read(int n)
		{
			const uint32_t v = Peek(n);
			m_bit += n;
			return v;
		}

		void Skip(int n) { m_bit += n; }

	private:
		const uint8_t* m_data;
		int64_t m_sizeBits;
		int64_t m_bit = 0;
	};

	uint16_t Crc16(const uint8_t* data, size_t size)
	{
		// Table-less form of HCACommon_CalculateCrc @0x1436c89f4 (poly 0x8005).
		uint16_t crc = 0;
		for (size_t i = 0; i < size; ++i)
		{
			crc ^= uint16_t(data[i]) << 8;
			for (int b = 0; b < 8; ++b)
				crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x8005) : uint16_t(crc << 1);
		}
		return crc;
	}

	bool ParseHeader(const uint8_t* data, size_t size, Info& out)
	{
		if (!data || size < 8 || Tag(data) != kTagHCA)
			return false;

		out = Info{};
		out.version = Be16(data + 4);
		out.headerSize = Be16(data + 6);
		if (out.version != 0x0101 && out.version != 0x0102 && out.version != 0x0103 &&
			out.version != 0x0200 && out.version != 0x0300)
			return false;
		if (size < out.headerSize)
			return false;
		if (Crc16(data, out.headerSize) != 0)
			return false;

		const uint8_t* p = data + 8;
		const uint8_t* end = data + out.headerSize;
		if (p + 16 > end || Tag(p) != kTagFmt)
			return false;

		// 'fmt': channels (8) | sampling rate (24) | block count (32) | delay (16) | padding (16)
		out.channelCount = p[4];
		out.samplingRate = (uint32_t(p[5]) << 16) | (uint32_t(p[6]) << 8) | p[7];
		out.blockCount = Be32(p + 8);
		out.encoderDelay = Be16(p + 12);
		out.encoderPadding = Be16(p + 14);
		p += 16;

		if (p + 16 <= end && Tag(p) == kTagComp)
		{
			// 'comp': blockSize u16, then 8 bytes: minRes, maxRes, trackCount, channelConfig,
			// totalBands, baseBands, stereoBands, bandsPerHfrGroup, msStereo, reserved.
			out.blockSize = Be16(p + 4);
			out.minResolution = p[6];
			out.maxResolution = p[7];
			out.trackCount = p[8];
			out.channelConfig = p[9];
			out.totalBands = p[10];
			out.baseBands = p[11];
			out.stereoBands = p[12];
			out.bandsPerHfrGroup = p[13];
			out.msStereo = p[14];
			p += 16;
		}
		else if (p + 12 <= end && Tag(p) == kTagDec)
		{
			// 'dec': band counts stored as (value + 1), track/config packed in a nibble byte,
			// and a stereo-type flag selecting whether baseBands is independent.
			out.blockSize = Be16(p + 4);
			out.minResolution = p[6];
			out.maxResolution = p[7];
			out.totalBands = uint32_t(p[8]) + 1;
			out.baseBands = uint32_t(p[9]) + 1;
			out.trackCount = uint8_t(p[10] >> 4);
			out.channelConfig = uint8_t(p[10] & 0x0F);
			const uint8_t stereoType = p[11];
			if (stereoType == 0)
				out.baseBands = out.totalBands;
			out.stereoBands = out.totalBands - out.baseBands;
			out.bandsPerHfrGroup = 0;
			p += 12;
		}
		else
			return false;

		if (p + 8 <= end && Tag(p) == kTagVbr)
			p += 8; // VBR streams are not produced by AtomCraft for these titles
		if (p + 6 <= end && Tag(p) == kTagAth)
		{
			out.athType = Be16(p + 4);
			p += 6;
		}
		else
			out.athType = (out.version < 0x0200) ? 1 : 0;
		if (p + 16 <= end && Tag(p) == kTagLoop)
		{
			out.hasLoop = true;
			out.loopStart = Be32(p + 4);
			out.loopEnd = Be32(p + 8);
			out.loopPreRoll = Be16(p + 12);
			out.loopPostRoll = Be16(p + 14);
			p += 16;
		}
		if (p + 6 <= end && Tag(p) == kTagCiph)
		{
			out.cipherType = Be16(p + 4);
			p += 6;
		}
		if (p + 8 <= end && Tag(p) == kTagRva)
		{
			const uint32_t bits = Be32(p + 4);
			std::memcpy(&out.rvaVolume, &bits, sizeof(float));
			p += 8;
		}
		if (p + 5 <= end && Tag(p) == kTagComm)
			p += 5 + p[4];

		// Validation limits (FUN_1436c6640 / reference header checks)
		if (out.channelCount < 1 || out.channelCount > 16)
			return false;
		if (out.samplingRate < 1 || out.samplingRate > 0x7FFFFF)
			return false;
		if (out.blockCount == 0)
			return false;
		if (out.blockSize < 8)
			return false;
		if (out.version <= 0x0200)
		{
			if (out.minResolution != 1 || out.maxResolution != 15)
				return false;
		}
		else if (out.minResolution > out.maxResolution || out.maxResolution > 15)
			return false;
		if (out.trackCount == 0)
			out.trackCount = 1;
		if (out.trackCount > out.channelCount)
			return false;
		if (out.totalBands > 128 || out.baseBands > 128 || out.stereoBands > 128 ||
			out.baseBands + out.stereoBands > 128 || out.bandsPerHfrGroup > 128)
			return false;

		// High-frequency-reconstruction group count (0 whenever bandsPerHfrGroup is 0, which
		// is every 'dec' stream).
		const uint32_t hfrBands =
			out.totalBands - out.baseBands - out.stereoBands;
		out.hfrGroupCount = (out.bandsPerHfrGroup >= 1)
			? (hfrBands / out.bandsPerHfrGroup + ((hfrBands % out.bandsPerHfrGroup) ? 1 : 0))
			: 0;
		return true;
	}

	Decoder::~Decoder() = default;

	bool Decoder::Init(const uint8_t* header, size_t headerSize)
	{
		if (!ParseHeader(header, headerSize, m_info))
			return false;
		if (m_info.cipherType != 0)
			return false; // keyed streams unsupported: none of the shipped assets use them

		m_channelCount = m_info.channelCount;
		for (uint32_t i = 0; i < m_channelCount; ++i)
			m_channels[i] = Channel{};
		m_random = 1;

		// ATH curve: type 0 is flat zero; type 1 samples the base curve at f = i * rate / 8192,
		// saturating at 0xFF past the table end.
		if (m_info.athType == 1)
		{
			uint32_t acc = 0;
			for (uint32_t i = 0; i < 128; ++i)
			{
				acc += m_info.samplingRate;
				const uint32_t index = acc >> 13;
				m_athCurve[i] = (index >= 656) ? 0xFF : kAthBaseCurve[index];
			}
		}
		else if (m_info.athType == 0)
			std::memset(m_athCurve, 0, sizeof(m_athCurve));
		else
			return false;

		// Channel typing: stereo primary/secondary pairs only exist when the stream actually
		// has stereo (intensity) bands; otherwise every channel is discrete. Layouts per
		// channels-per-track follow the CRI table (only the 2-channel case occurs in our data).
		uint8_t types[16] = {};
		const uint32_t channelsPerTrack = m_channelCount / m_info.trackCount;
		if (m_info.stereoBands > 0 && channelsPerTrack > 1)
		{
			switch (channelsPerTrack)
			{
			case 2:
			case 3:
				types[0] = 1; types[1] = 2;
				break;
			case 4:
				types[0] = 1; types[1] = 2;
				if (m_info.channelConfig == 0) { types[2] = 1; types[3] = 2; }
				break;
			case 5:
				types[0] = 1; types[1] = 2;
				if (m_info.channelConfig <= 2) { types[3] = 1; types[4] = 2; }
				break;
			case 6:
			case 7:
				types[0] = 1; types[1] = 2; types[4] = 1; types[5] = 2;
				break;
			case 8:
				types[0] = 1; types[1] = 2; types[4] = 1; types[5] = 2;
				types[6] = 1; types[7] = 2;
				break;
			default:
				break;
			}
		}
		for (uint32_t i = 0; i < m_channelCount; ++i)
		{
			m_channels[i].type = types[i];
			// A stereo-secondary channel only codes the base bands; its stereo bands are
			// reconstructed from the primary. Everyone else codes base + stereo bands.
			m_channels[i].codedCount = (types[i] == 2)
				? m_info.baseBands
				: m_info.baseBands + m_info.stereoBands;
		}
		return true;
	}

	void Decoder::ResetState()
	{
		for (uint32_t i = 0; i < m_channelCount; ++i)
			std::memset(m_channels[i].imdctPrev, 0, sizeof(m_channels[i].imdctPrev));
		m_random = 1;
	}

	// Delta-coded scale factors (FUN_1436c55f0). 3-bit delta width; 0 = whole channel silent;
	// >= 6 = every entry an absolute 6-bit value; else a 6-bit seed then deltas, where an
	// all-ones delta escapes to a fresh absolute value.
	bool Decoder::UnpackScaleFactors(Channel& ch, BitReader& br)
	{
		uint32_t csCount = ch.codedCount;
		uint32_t extraCount = 0;
		// v3.0 streams append the HFR group scales here for non-secondary channels; v<=2.0
		// carries them in the intensity slot instead (see UnpackIntensity).
		if (ch.type != 2 && m_info.hfrGroupCount > 0 && m_info.version > 0x0200)
		{
			extraCount = m_info.hfrGroupCount;
			csCount += extraCount;
			if (csCount > 128)
				return false;
		}

		const uint32_t deltaBits = br.Read(3);
		if (deltaBits >= 6)
		{
			for (uint32_t i = 0; i < csCount; ++i)
				ch.scale[i] = uint8_t(br.Read(6));
		}
		else if (deltaBits > 0)
		{
			const uint32_t maxDelta = (1u << deltaBits) - 1;
			uint32_t value = br.Read(6);
			ch.scale[0] = uint8_t(value);
			for (uint32_t i = 1; i < csCount; ++i)
			{
				const uint32_t d = br.Read(int(deltaBits));
				if (d == maxDelta)
					value = br.Read(6); // escape: absolute
				else
				{
					const int test = int(value) + int(d) - int(maxDelta >> 1);
					if (test < 0 || test >= 64)
						return false;
					value = uint32_t(test) & 0x3F;
				}
				ch.scale[i] = uint8_t(value);
			}
		}
		else
			std::memset(ch.scale, 0, 128);

		// Relocate the extra (HFR) scales to the fixed tail slots the reconstruction reads.
		for (uint32_t i = 0; i < extraCount; ++i)
			ch.scale[128 - 1 - i] = ch.scale[csCount - i];
		return true;
	}

	// Per-sub-frame intensity indexes (stereo secondary) or, for v<=2.0 non-secondary
	// channels, the HFR group scales. Faithful quirk: for v<=2.0, an intensity prefix of 15
	// consumes NO bits and leaves the previous frame's indexes 1..7 in place.
	void Decoder::UnpackIntensity(Channel& ch, BitReader& br)
	{
		if (ch.type == 2)
		{
			if (m_info.version <= 0x0200)
			{
				const uint8_t value = uint8_t(br.Peek(4));
				ch.intensity[0] = value;
				if (value < 15)
				{
					br.Skip(4);
					for (int i = 1; i < 8; ++i)
						ch.intensity[i] = uint8_t(br.Read(4));
				}
			}
			else
			{
				const uint8_t value = uint8_t(br.Peek(4));
				if (value < 15)
				{
					br.Skip(4);
					const uint32_t deltaBits = br.Read(2);
					ch.intensity[0] = value;
					if (deltaBits == 3)
					{
						for (int i = 1; i < 8; ++i)
							ch.intensity[i] = uint8_t(br.Read(4));
					}
					else
					{
						const uint8_t bMax = uint8_t((2u << deltaBits) - 1);
						const int bits = int(deltaBits) + 1;
						uint8_t v = value;
						for (int i = 1; i < 8; ++i)
						{
							const uint8_t d = uint8_t(br.Read(bits));
							if (d == bMax)
								v = uint8_t(br.Read(4));
							else
								v = uint8_t(v - (bMax >> 1) + d); // > 15 is malformed; clamp-free like CRI
							ch.intensity[i] = v;
						}
					}
				}
				else
				{
					br.Skip(4);
					for (int i = 0; i < 8; ++i)
						ch.intensity[i] = 7;
				}
			}
		}
		else if (m_info.version <= 0x0200 && m_info.hfrGroupCount > 0)
		{
			for (uint32_t i = 0; i < m_info.hfrGroupCount; ++i)
				ch.scale[128 - m_info.hfrGroupCount + i] = uint8_t(br.Read(6));
		}
	}

	// Resolution per coefficient (FUN_1436c4e10 tail): the scale factor is weighed against the
	// frame noise floor (+ ATH curve), mapped through the invert table, then clamped to the
	// header's resolution range. Zero-resolution slots are candidates for noise substitution.
	void Decoder::CalculateResolution(Channel& ch, int packedNoiseLevel)
	{
		uint32_t noiseCount = 0;
		uint32_t validCount = 0;
		for (uint32_t i = 0; i < ch.codedCount; ++i)
		{
			uint8_t res = 0;
			const uint8_t sf = ch.scale[i];
			if (sf > 0)
			{
				const int noiseLevel = int(m_athCurve[i]) + ((packedNoiseLevel + int(i)) >> 8);
				const int pos = noiseLevel + 1 - ((5 * int(sf)) >> 1);
				if (pos < 0)
					res = 15;
				else if (pos <= 65)
					res = kScaleToResolution[pos];
				else
					res = 0;
				if (res > m_info.maxResolution)
					res = m_info.maxResolution;
				else if (res < m_info.minResolution)
					res = m_info.minResolution;

				if (res < 1)
					ch.noises[noiseCount++] = uint8_t(i);
				else
					ch.noises[128 - 1 - validCount++] = uint8_t(i);
			}
			ch.resolution[i] = res;
		}
		ch.noiseCount = noiseCount;
		ch.validCount = validCount;
		std::memset(ch.resolution + ch.codedCount, 0, 128 - ch.codedCount);
	}

	// Dequantizer gain — TWO factors: the scale-factor step AND the resolution range
	// normalization. (The missing range factor was the long-standing "structured coefficient
	// error": correct overall RMS, wildly wrong individual coefficients.)
	void Decoder::CalculateGain(Channel& ch)
	{
		for (uint32_t i = 0; i < ch.codedCount; ++i)
			ch.gain[i] = kDequantizerScaling[ch.scale[i]] * kDequantizerRange[ch.resolution[i]];
	}

	// Coefficient unpack + dequantize for ONE sub-frame (FUN_1436c4608; the shipped build
	// unrolls this 8x). Reads the peek width, then rewinds the bits the code didn't use.
	void Decoder::Dequantize(Channel& ch, BitReader& br)
	{
		for (uint32_t i = 0; i < ch.codedCount; ++i)
		{
			const uint8_t res = ch.resolution[i];
			const int bits = kQuantMaxBits[res];
			const uint32_t code = br.Read(bits);

			float qc;
			if (res > 7)
			{
				const int v = (1 - int((code & 1) << 1)) * int(code >> 1);
				if (v == 0)
					br.Skip(-1);
				qc = float(v);
			}
			else
			{
				const size_t idx = size_t(res) * 16 + code;
				br.Skip(int(kQuantConsumedBits[idx]) - bits);
				qc = kQuantValue[idx];
			}
			ch.coef[i] = ch.gain[i] * qc;
		}
		std::memset(ch.coef + ch.codedCount, 0, (128 - ch.codedCount) * sizeof(float));
	}

	// Noise substitution: zero-resolution coefficients borrow a randomly chosen valid
	// coefficient, rescaled by the scale-factor difference. Only reachable when
	// minResolution == 0 (never in the shipped v<=2.0 data, where it is forced to 1).
	void Decoder::ReconstructNoise(Channel& ch)
	{
		if (m_info.minResolution > 0)
			return;
		if (ch.validCount == 0 || ch.noiseCount == 0)
			return;
		if (m_info.msStereo != 0 && ch.type == 1)
			return;

		for (uint32_t i = 0; i < ch.noiseCount; ++i)
		{
			m_random = int(uint32_t(m_random) * 0x343FDu + 0x269EC3u); // CRI LCG, wrapping
			const uint32_t randomIndex =
				128 - ch.validCount + ((uint32_t(m_random & 0x7FFF) * ch.validCount) >> 15);

			const uint32_t noiseIndex = ch.noises[i];
			const uint32_t validIndex = ch.noises[randomIndex];

			int sc = int(ch.scale[noiseIndex]) - int(ch.scale[validIndex]) + 62;
			sc &= ~(sc >> 31); // clamp negatives to 0
			ch.coef[noiseIndex] = kScaleConversion[sc] * ch.coef[validIndex];
		}
	}

	// High-frequency reconstruction: bands above base+stereo are mirrored down from the coded
	// region, rescaled by the HFR group scale. Only reachable for 'comp' streams with a
	// nonzero bandsPerHfrGroup (none shipped).
	void Decoder::ReconstructHighFrequency(Channel& ch)
	{
		if (m_info.bandsPerHfrGroup == 0)
			return;
		if (ch.type == 2)
			return;

		const uint32_t groupLimit = (m_info.version <= 0x0200)
			? m_info.hfrGroupCount
			: (m_info.hfrGroupCount >> 1);
		const uint32_t startBand = m_info.stereoBands + m_info.baseBands;
		uint32_t highBand = startBand;
		int lowBand = int(startBand) - 1;

		for (uint32_t group = 0; group < m_info.hfrGroupCount; ++group)
		{
			const int lowBandSub = (group < groupLimit) ? 1 : 0;
			for (uint32_t i = 0; i < m_info.bandsPerHfrGroup; ++i)
			{
				if (highBand >= m_info.totalBands || lowBand < 0)
					break;
				int sc = ch.scale[128 - m_info.hfrGroupCount + group];
				sc &= ~(sc >> 31);
				ch.coef[highBand] = kScaleConversion[sc] * ch.coef[lowBand];
				++highBand;
				lowBand -= lowBandSub;
			}
		}
		if (highBand > 0)
			ch.coef[highBand - 1] = 0.0f;
	}

	// Joint (intensity) stereo: both output channels derive their stereo bands from the
	// PRIMARY's coefficients scaled by a ratio pair (partner = 2 - ratio; FUN_1436c539c and
	// the Y6 wrappers agree on this form). Dead path in the shipped data (stereoBands == 0).
	void Decoder::ApplyIntensityStereo(uint32_t pair, uint32_t subFrame)
	{
		Channel& a = m_channels[pair];
		Channel& b = m_channels[pair + 1];
		if (a.type != 1)
			return;
		const float ratio = kIntensityRatio[b.intensity[subFrame] & 0x0F];
		const float partner = 2.0f - ratio;
		for (uint32_t i = m_info.baseBands; i < m_info.baseBands + m_info.stereoBands; ++i)
		{
			const float c = a.coef[i];
			a.coef[i] = c * ratio;
			b.coef[i] = c * partner;
		}
	}

	// Mid/side stereo over the stereo bands (only for msStereo streams; none shipped).
	void Decoder::ApplyMsStereo(uint32_t pair)
	{
		if (m_info.msStereo == 0)
			return;
		Channel& a = m_channels[pair];
		Channel& b = m_channels[pair + 1];
		if (a.type != 1)
			return;
		constexpr float kInvSqrt2 = 0.70710676908493042f; // 0x3F3504F3, CRI's constant
		for (uint32_t i = m_info.baseBands; i < m_info.baseBands + m_info.stereoBands; ++i)
		{
			const float l = a.coef[i];
			const float r = b.coef[i];
			a.coef[i] = (l + r) * kInvSqrt2;
			b.coef[i] = (l - r) * kInvSqrt2;
		}
	}

	// HCADCT_Transform @0x1436c8bfc — a 128-point DCT built as 7 butterfly stages followed by
	// 7 rotation stages, ping-ponging between two scratch buffers. Stage s consumes the
	// 64-entry sin/cos row s (the shipped code walks the same tables with a negative stride,
	// which is why the Y6 extraction saw them "backwards").
	void Decoder::Dct4(const float* in, float* out)
	{
		float bufA[128];
		float bufB[128];

		// Phase 1: butterflies. Group count doubles, group half-width halves.
		std::memcpy(bufA, in, sizeof(bufA));
		const float* src = bufA;
		float* dst = bufB;
		uint32_t count = 1;
		uint32_t stride = 64;
		for (int s = 0; s < 7; ++s)
		{
			const float* sp = src;
			float* dp = dst;
			for (uint32_t g = 0; g < count; ++g)
			{
				float* hi = dp + stride;
				for (uint32_t j = 0; j < stride; ++j)
				{
					dp[j] = sp[0] + sp[1];
					hi[j] = sp[0] - sp[1];
					sp += 2;
				}
				dp += stride * 2;
			}
			stride >>= 1;
			count <<= 1;
			const float* nextSrc = dst;
			dst = const_cast<float*>(src);
			src = nextSrc;
		}

		// Phase 2: rotations. One half written forward, the mirrored half backward:
		//   fwd = a*sin - b*cos ; bwd = a*cos + b*sin
		// (verified identical between the Y6 transcription — "variant A" — and the reference;
		// the sin/cos row advance is `half` per group, continuous across the stage).
		uint32_t groups = 128;
		uint32_t half = 1;
		for (int s = 0; s < 7; ++s)
		{
			groups >>= 1;
			const float* sn = kDctSin[s];
			const float* cs = kDctCos[s];

			const float* sp = src;
			float* fwd = dst;
			float* bwd = dst + (half * 2 - 1);
			for (uint32_t g = 0; g < groups; ++g)
			{
				const float* hi = sp + half;
				for (uint32_t j = 0; j < half; ++j)
				{
					fwd[j] = sp[j] * sn[j] - hi[j] * cs[j];
					bwd[-int(j)] = sp[j] * cs[j] + hi[j] * sn[j];
				}
				sn += half;
				cs += half;
				sp += half * 2;
				fwd += half * 2;
				bwd += half * 2;
			}
			half <<= 1;
			const float* nextSrc = dst;
			dst = const_cast<float*>(src);
			src = nextSrc;
		}
		std::memcpy(out, src, 128 * sizeof(float));
	}

	// Windowed 50%-overlap fold: each sub-frame emits 128 samples built from the current
	// DCT's second half plus the previous tail, then regenerates the tail from the first
	// half. The window carries all normalization — no extra scale anywhere.
	void Decoder::Imdct(Channel& ch, float* out, uint32_t stride)
	{
		float dct[128];
		Dct4(ch.coef, dct);

		constexpr uint32_t half = 64;
		for (uint32_t i = 0; i < half; ++i)
		{
			out[i * stride] =
				kImdctWindow[i] * dct[i + half] + ch.imdctPrev[i];
			out[(i + half) * stride] =
				kImdctWindow[i + half] * dct[127 - i] - ch.imdctPrev[i + half];

			ch.imdctPrev[i] = kImdctWindow[127 - i] * dct[half - i - 1];
			ch.imdctPrev[i + half] = kImdctWindow[half - i - 1] * dct[i];
		}
	}

	// One frame = 8 sub-frames of 128 samples per channel. The frame header (scales,
	// resolutions, intensity) is parsed once; the same bitstream then carries 8 consecutive
	// coefficient sets, consumed sub-frame-major / channel-minor.
	bool Decoder::DecodeFrame(const uint8_t* frame, size_t frameSize, float* out)
	{
		if (!frame || !out || frameSize < m_info.blockSize)
			return false;

		BitReader br(frame, m_info.blockSize);
		if (br.Read(16) != 0xFFFF)
			return false;
		if (Crc16(frame, m_info.blockSize) != 0)
			return false;

		const int noiseLevel = int(br.Read(9));
		const int evalBoundary = int(br.Read(7));
		const int packedNoiseLevel = (noiseLevel << 8) - evalBoundary;

		for (uint32_t c = 0; c < m_channelCount; ++c)
		{
			Channel& ch = m_channels[c];
			if (!UnpackScaleFactors(ch, br))
				return false;
			UnpackIntensity(ch, br);
			CalculateResolution(ch, packedNoiseLevel);
			CalculateGain(ch);
		}

		for (uint32_t sub = 0; sub < kSubFrames; ++sub)
		{
			for (uint32_t c = 0; c < m_channelCount; ++c)
				Dequantize(m_channels[c], br);

			for (uint32_t c = 0; c < m_channelCount; ++c)
			{
				ReconstructNoise(m_channels[c]);
				ReconstructHighFrequency(m_channels[c]);
			}
			if (m_info.stereoBands > 0)
			{
				for (uint32_t c = 0; c + 1 < m_channelCount; ++c)
				{
					ApplyIntensityStereo(c, sub);
					ApplyMsStereo(c);
				}
			}

			float* dst = out + size_t(sub) * 128 * m_channelCount;
			for (uint32_t c = 0; c < m_channelCount; ++c)
				Imdct(m_channels[c], dst + c, m_channelCount);
		}
		return true;
	}
}
