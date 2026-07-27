#pragma once

// HCA (CRI ADX2 "High Compression Audio") decoder — clean-room reimplementation. Originally
// reconstructed from the CRI runtime statically linked into Yakuza 6 (Ghidra, image base
// 0x140000000; per-stage source functions cited in the .cpp), then finished and validated
// stage-for-stage against the ClHcaSharp reference decoder, whose output on the shipped StF
// assets was listener-verified. Constants live in HcaTables.h (bit-exact).
//
// Format summary (verified against the shipped stf/vf2/fv/mr AWB assets):
//   - Container : AFS2 (.awb) or in-ACB "AwbFile" blob; each entry is a standalone HCA stream.
//   - Header    : 'HCA\0' + chunk list, all tags compared with the high bit masked off (0x7F)
//                 so the header survives header-encryption. CRC16 over the header must be 0.
//   - Frames    : fixed blockSize bytes each, blockCount total, 1024 samples/channel per frame
//                 (8 sub-frames x 128 samples internally), CRC16 over the frame must be 0.
//   - Our data  : ciph type 0 (unencrypted), so no key handling is implemented here. All
//                 shipped streams are v1.03 'dec' / v2.00 'comp' with zero stereo bands, no
//                 HFR groups and minResolution 1, so the intensity/MS-stereo, high-frequency
//                 and noise-substitution stages below are implemented but never exercised.

#include <cstdint>
#include <cstddef>

namespace cri::hca
{
	// ---- header ----------------------------------------------------------------------------
	// Field set + validation limits from HCAHeader_Read @0x1436c5e28 and FUN_1436c6640.

	struct Info
	{
		uint16_t version = 0;       // 0x0101..0x0300 accepted
		uint16_t headerSize = 0;

		uint8_t channelCount = 0;   // 1..16
		uint32_t samplingRate = 0;  // 1..0x7FFFFF
		uint32_t blockCount = 0;
		uint16_t encoderDelay = 0;
		uint16_t encoderPadding = 0;

		uint16_t blockSize = 0;     // 8..0xFFFF
		uint8_t minResolution = 0;  // 1 for v<=2.0 (validated)
		uint8_t maxResolution = 0;  // 15 for v<=2.0 (validated)
		uint8_t trackCount = 0;     // 0 -> 1
		uint8_t channelConfig = 0;

		// Band layout. In the 'dec' chunk these are stored as (value + 1); totalBands is the
		// per-channel coefficient count (128 for the shipped 48 kHz streams).
		uint32_t totalBands = 0;
		uint32_t baseBands = 0;
		uint32_t stereoBands = 0;
		uint32_t bandsPerHfrGroup = 0; // always 0 for 'dec' streams
		uint32_t hfrGroupCount = 0;    // derived
		uint8_t msStereo = 0;

		uint16_t athType = 0;       // absent chunk: 1 for v<2.0, else 0

		bool hasLoop = false;
		uint32_t loopStart = 0;     // block index
		uint32_t loopEnd = 0;       // block index (inclusive)
		uint16_t loopPreRoll = 0;
		uint16_t loopPostRoll = 0;

		uint16_t cipherType = 0;    // 0 = none (all our assets), 1 = static, 56 = keyed
		float rvaVolume = 1.0f;     // 'rva' chunk, defaults to 1.0 when absent

		// Derived
		uint32_t SampleCount() const { return blockCount * 1024u; }
	};

	// Parses an HCA header. Returns false if the magic, version, CRC or any validation limit
	// fails. `size` must cover at least headerSize bytes.
	bool ParseHeader(const uint8_t* data, size_t size, Info& out);

	// CRC16 used by the header and every frame (HCACommon_CalculateCrc @0x1436c89f4): poly
	// 0x8005, init 0, no reflection, no final xor. Valid data CRCs to 0 including its checksum.
	uint16_t Crc16(const uint8_t* data, size_t size);

	// ---- decoder ---------------------------------------------------------------------------

	class BitReader; // defined in the .cpp

	class Decoder
	{
	public:
		Decoder() = default;
		~Decoder();

		Decoder(const Decoder&) = delete;
		Decoder& operator=(const Decoder&) = delete;

		// Binds the stream. `header` must remain valid only for this call.
		bool Init(const uint8_t* header, size_t headerSize);

		const Info& GetInfo() const { return m_info; }

		// A frame decodes to kSubFrames * 128 = 1024 samples per channel.
		static constexpr uint32_t kSubFrames = 8;
		static constexpr uint32_t kSamplesPerFrame = kSubFrames * 128;

		// Decodes one frame (exactly info.blockSize bytes) into `out`, which must hold
		// kSamplesPerFrame * channelCount floats, interleaved, nominally in [-1, 1].
		// Returns false on a malformed frame (bad sync/CRC or bitstream overrun).
		bool DecodeFrame(const uint8_t* frame, size_t frameSize, float* out);

		// Clears inter-frame state (IMDCT overlap tails + noise RNG), e.g. when seeking.
		void ResetState();

	private:
		// Per-channel working state (one sub-frame of spectra at a time — the bitstream is
		// consumed sub-frame-major, so nothing needs to persist across sub-frames except the
		// IMDCT overlap tail).
		struct Channel
		{
			uint8_t type = 0;            // 0 discrete, 1 stereo primary, 2 stereo secondary
			uint32_t codedCount = 0;     // coefficients actually in the bitstream (per type!)

			uint8_t scale[128] = {};     // scale factors (tail doubles as HFR group scales)
			uint8_t resolution[128] = {};
			uint8_t intensity[8] = {};   // per-sub-frame joint-stereo index
			float gain[128] = {};        // dequantizer gain = scaling[scale] * range[resolution]

			// Noise substitution bookkeeping: indexes of zero-resolution coefficients grow
			// from the front, valid ones from the back (mirrors the CRI layout).
			uint8_t noises[128] = {};
			uint32_t noiseCount = 0;
			uint32_t validCount = 0;

			float coef[128] = {};        // current sub-frame spectra
			float imdctPrev[128] = {};   // IMDCT overlap tail
		};

		Info m_info;
		Channel m_channels[16];
		uint32_t m_channelCount = 0;
		uint8_t m_athCurve[128] = {};
		int m_random = 1;

		bool UnpackScaleFactors(Channel& ch, BitReader& br);
		void UnpackIntensity(Channel& ch, BitReader& br);
		void CalculateResolution(Channel& ch, int packedNoiseLevel);
		void CalculateGain(Channel& ch);
		void Dequantize(Channel& ch, BitReader& br);
		void ReconstructNoise(Channel& ch);
		void ReconstructHighFrequency(Channel& ch);
		void ApplyIntensityStereo(uint32_t pair, uint32_t subFrame);
		void ApplyMsStereo(uint32_t pair);
		static void Dct4(const float* in, float* out);
		void Imdct(Channel& ch, float* out, uint32_t stride);
	};
}
