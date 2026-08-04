#pragma once

// Clean-room CRI Atom playback engine backing Cri's icri implementation.
//
// Scope: the cue-based path Sonic the Fighters (and VF2/FV/MR) actually uses —
//   criAtomExAcb_LoadAcbData -> player SetCueName(acb-or-NULL, name) -> Start/GetStatus/
//   SetVolume/Pause/Stop — with HCA waveforms resolved from the ACB's @UTF tables:
//   in-memory SFX from the ACB's internal "AwbFile" AFS2 blob, streamed BGM from the
//   sibling .awb on disk (located by matching the loaded ACB bytes against rom/sound/*.acb).
// Semantics follow the CRI runtime in Yakuza 6 (memory 26k: registry insertion order for
// NULL-acb cue lookup, synth ReferenceItems fallback list, first non-zero wins).
//
// Output: XAudio2 (one mastering voice; one source voice per player, recreated on format
// change). Waveforms are fully decoded to PCM16 at Start() — the largest BGM is ~25 MB of
// PCM, and the validated decoder does ~6k frames in well under a second.
// HCA loop points map to the XAUDIO2_BUFFER loop region (encoder delay is not trimmed yet;
// loops are accurate to within the ~45 ms delay — refine if audibly off).

struct CriAtomExAcbTag;
struct CriAtomExPlayerTag;
struct CriAtomExWaveformInfoTag;

#include <cstddef>

namespace cri::atom
{
	// Allocator handed to the game through icri::alloc/free — MUST return real memory
	// (the game builds player/acb work buffers with it).
	void* Alloc(size_t size, size_t align);
	void Free(void* p);

	// ACB registry. Data is copied; the handle stays valid until Release.
	CriAtomExAcbTag* LoadAcbData(const void* data, int size);
	void ReleaseAcb(CriAtomExAcbTag* acb);
	int GetWaveformInfoByName(CriAtomExAcbTag* acb, const char* cueName, CriAtomExWaveformInfoTag* out);

	// Players.
	CriAtomExPlayerTag* PlayerCreate();
	void PlayerDestroy(CriAtomExPlayerTag* p);
	void PlayerSetCueName(CriAtomExPlayerTag* p, CriAtomExAcbTag* acb, const char* cueName);
	// Numeric cue selection (pre3/Model 3 uses this instead of the by-name path).
	void PlayerSetCueId(CriAtomExPlayerTag* p, CriAtomExAcbTag* acb, unsigned int id);
	// Non-cue sources (the VF5FS BGM/voice path): a stream file on disk (ADX or HCA,
	// auto-detected) or an in-memory blob. Format/channels/rate hints from the game are
	// accepted but the file's own header is authoritative.
	void PlayerSetFile(CriAtomExPlayerTag* p, const char* path);
	void PlayerSetData(CriAtomExPlayerTag* p, const void* data, int size);
	void PlayerSetFormat(CriAtomExPlayerTag* p, unsigned int format);
	void PlayerSetNumChannels(CriAtomExPlayerTag* p, int channels);
	void PlayerSetSamplingRate(CriAtomExPlayerTag* p, int rate);
	void PlayerLimitLoopCount(CriAtomExPlayerTag* p, int count); // -1 = loop forever
	void PlayerSetVolume(CriAtomExPlayerTag* p, float linearVolume);
	unsigned int PlayerStart(CriAtomExPlayerTag* p);
	void PlayerStop(CriAtomExPlayerTag* p);
	void PlayerPause(CriAtomExPlayerTag* p, int pause);
	void PlayerResume(CriAtomExPlayerTag* p, int mode);
	int PlayerIsPaused(CriAtomExPlayerTag* p);
	// CRIATOMEXPLAYER_STATUS_*: 0 STOP, 1 PREP, 2 PLAYING, 3 PLAYEND, 4 ERROR
	int PlayerGetStatus(CriAtomExPlayerTag* p);
	void PlayerResetParameters(CriAtomExPlayerTag* p);
	void PlayerUpdateAll(CriAtomExPlayerTag* p);

	// Called once per frame by the game (criAtom_ExecuteMain).
	void ExecuteMain();
}
