#pragma once

#include <cstddef>
#include <cstdint>

namespace net
{
	// The desync-canary hash, in one place (2026-08-09; it existed as five per-file copies).
	// FNV-1a over DWORDS, not bytes: every peer is x86-64, so the word interpretation is
	// identical on both sides and four times cheaper - the same reasoning each copy carried in
	// its own words. The constants are the standard 32-bit FNV offset basis and prime.
	inline constexpr uint32_t FNV1A_BASIS = 2166136261u;
	inline constexpr uint32_t FNV1A_PRIME = 16777619u;

	// One step, for the strided/field-wise accumulations (pre3's register file and its
	// deliberately-sparse RAM sweep keep their own loops and stride policy).
	inline uint32_t Fnv1aWord(uint32_t h, uint32_t word)
	{
		return (h ^ word) * FNV1A_PRIME;
	}

	// A contiguous block, whole dwords (the trailing bytes of a non-multiple-of-4 length are
	// not read - same as every copy this replaces).
	inline uint32_t Fnv1aWords(const void* data, size_t bytes)
	{
		uint32_t h = FNV1A_BASIS;
		const auto* w = reinterpret_cast<const uint32_t*>(data);
		for (size_t i = 0; i < bytes / 4; ++i)
		{
			h = Fnv1aWord(h, w[i]);
		}
		return h;
	}
}
