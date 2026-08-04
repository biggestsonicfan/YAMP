#include "Cri.h"
#include "../DebugLog.h"
#include "AtomEngine.h"

#include <cstring>
#include <iterator>


// icri implementation. The CriAtomEx cue-playback surface (players, ACBs, ExecuteMain,
// alloc) is backed by the clean-room AtomEngine (ACB/@UTF + AFS2 + validated HCA decoder +
// XAudio2); everything the games never exercise (CriMana video, DSP buses, Aisac) stays
// stubbed as no-ops.
CriAtomExPlayerTag* Cri::criAtomExPlayer_Create(CriAtomExPlayerConfigTag*, void*, int)
{
	return cri::atom::PlayerCreate();
}

void Cri::criAtomExPlayer_Destroy(CriAtomExPlayerTag* player)
{
	cri::atom::PlayerDestroy(player);
}

int Cri::criAtomExPlayer_GetStatus(CriAtomExPlayerTag* player)
{
	return cri::atom::PlayerGetStatus(player);
}

CriAtomExAcbTag* Cri::criAtomExAcb_LoadAcbData(void* acbData, int acbDataSize, CriFsBinderHnObjTag*, const char*, void*, int)
{
	// Games pass a null binder/path; the engine resolves the external .awb itself by
	// matching the blob against the rom/sound .acb files (sibling-file convention).
	return cri::atom::LoadAcbData(acbData, acbDataSize);
}

int Cri::criAtomExAcb_CalculateWorkSizeForLoadAcbData(void*, int, CriFsBinderHnObjTag*, const char*)
{
	// The engine keeps its own copy of the data; the game-side work buffer is unused but
	// must be a sane allocatable size.
	return 0x100;
}

void Cri::criAtomExAcb_Release(CriAtomExAcbTag* acb)
{
	cri::atom::ReleaseAcb(acb);
}

void Cri::criAtomExPlayer_AttachAisac(CriAtomExPlayerTag*, const char*)
{
}

void Cri::criAtomExPlayer_DetachAisac(CriAtomExPlayerTag*, const char*)
{
}

void Cri::criAtomExPlayer_SetCueName(CriAtomExPlayerTag* player, CriAtomExAcbTag* acb, const char* cueName)
{
	// A null acb handle is the common case: the cue is looked up across ALL loaded ACBs in
	// registration order (criAtomExAcb_FindAcbByCueName semantics).
	cri::atom::PlayerSetCueName(player, acb, cueName);
}

void Cri::criAtomExPlayer_SetVolume(CriAtomExPlayerTag* player, float volume)
{
	cri::atom::PlayerSetVolume(player, volume); // linear amplitude at this interface
}

int Cri::criAtomExPlayer_CalculateWorkSize(CriAtomExPlayerConfigTag*)
{
	// Player state lives in the engine; the game-side work buffer is unused but must be a
	// sane allocatable size (the game allocs it through icri::alloc and hands it to Create).
	return 0x100;
}

unsigned int Cri::criAtomExPlayer_Start(CriAtomExPlayerTag* player)
{
	return cri::atom::PlayerStart(player);
}

void Cri::criAtomExPlayer_Stop(CriAtomExPlayerTag* player)
{
	cri::atom::PlayerStop(player);
}

void Cri::criAtomExPlayer_StopWithoutReleaseTime(CriAtomExPlayerTag* player)
{
	cri::atom::PlayerStop(player);
}

void Cri::criAtomExPlayer_ResetParameters(CriAtomExPlayerTag* player)
{
	cri::atom::PlayerResetParameters(player);
}

void Cri::criAtomExPlayer_UpdateAll(CriAtomExPlayerTag* player)
{
	cri::atom::PlayerUpdateAll(player);
}

void Cri::criAtomExPlayer_SetPitch(CriAtomExPlayerTag*, float)
{
}

void Cri::criAtomExPlayer_SetPan3dAngle(CriAtomExPlayerTag*, float)
{
}

void Cri::criAtomExPlayer_Pause(CriAtomExPlayerTag* player, int pause)
{
	cri::atom::PlayerPause(player, pause);
}

void Cri::criAtomExPlayer_SetAisacControlByName(CriAtomExPlayerTag*, const char*, float)
{
}

int Cri::criAtomExPlayer_IsPaused(CriAtomExPlayerTag* player)
{
	return cri::atom::PlayerIsPaused(player);
}

void Cri::criAtom_ExecuteMain()
{
	cri::atom::ExecuteMain();
}

void Cri::criAtomExPlayer_SetData(CriAtomExPlayerTag* player, void* data, int size)
{
	cri::atom::PlayerSetData(player, data, size);
}

void Cri::criAtomExPlayer_SetFormat(CriAtomExPlayerTag* player, unsigned int format)
{
	cri::atom::PlayerSetFormat(player, format);
}

void Cri::criAtomExPlayer_SetNumChannels(CriAtomExPlayerTag* player, int channels)
{
	cri::atom::PlayerSetNumChannels(player, channels);
}

void Cri::criAtomExPlayer_SetSamplingRate(CriAtomExPlayerTag* player, int rate)
{
	cri::atom::PlayerSetSamplingRate(player, rate);
}

void Cri::criAtomExPlayer_SetFile(CriAtomExPlayerTag* player, CriFsBinderHnObjTag*, const char* path)
{
	// VF5FS BGM/voice: streamed ADX files ("<search_path>/<name>"); the engine resolves the
	// path against the exe dir and the known sound roots.
	cri::atom::PlayerSetFile(player, path);
}

void Cri::criAtomExPlayer_LimitLoopCount(CriAtomExPlayerTag* player, int count)
{
	cri::atom::PlayerLimitLoopCount(player, count);
}

void Cri::criAtomExPlayer_SetVoicePriority(CriAtomExPlayerTag*, int)
{
}

int Cri::criManaPlayer_CalculateHandleWorkSize()
{
	return 0;
}

CriManaPlayerTag* Cri::criManaPlayer_Create(void*, int)
{
	return nullptr;
}

void Cri::criManaPlayer_Destroy(CriManaPlayerTag*)
{
}

void Cri::criManaPlayer_SetFile(CriManaPlayerTag*, CriFsBinderHnObjTag*, const char*)
{
}

void Cri::criManaPlayer_Start(CriManaPlayerTag*)
{
}

void Cri::criManaPlayer_Stop(CriManaPlayerTag*)
{
}

void Cri::criManaPlayer_StopAndWaitCompletion(CriManaPlayerTag*)
{
}

void Cri::criManaPlayer_Pause(CriManaPlayerTag*, int)
{
}

int Cri::criManaPlayer_IsPaused(CriManaPlayerTag*)
{
	return 0;
}

void Cri::criManaPlayer_GetTime(CriManaPlayerTag*, unsigned __int64*, unsigned __int64*)
{
}

CriManaPlayerStatus Cri::criManaPlayer_GetStatus(CriManaPlayerTag*)
{
	return CriManaPlayerStatus();
}

int Cri::criManaPlayer_ReferFrame(CriManaPlayerTag*, CriManaFrameInfo*)
{
	return 0;
}

int Cri::criManaPlayer_IsFrameOnTime(CriManaPlayerTag*, CriManaFrameInfo*)
{
	return 0;
}

void Cri::criManaPlayer_CopyFrameToBuffersYUV(CriManaPlayerTag*, CriManaFrameInfo*, CriManaTextureBuffersYUV*)
{
}

void Cri::criManaPlayer_DiscardFrame(CriManaPlayerTag*, CriManaFrameInfo*)
{
}

float Cri::criManaPlayer_GetVolume(CriManaPlayerTag*)
{
	return 0.0f;
}

void Cri::criManaPlayer_SetVolume(CriManaPlayerTag*, float)
{
}

int Cri::criManaPlayer_GetPlaybackWorkParam(CriManaPlayerTag*, CriManaPlaybackBasicWorkConfig*, CriManaPlaybackExWorkConfig*)
{
	return 0;
}

int Cri::criManaPlayer_CalculatePlaybackWorkSize(CriManaPlayerTag*, CriManaPlaybackBasicWorkConfig*, CriManaPlaybackExWorkConfig*)
{
	return 0;
}

void Cri::criManaPlayer_SetPlaybackWork(CriManaPlayerTag*, CriManaPlaybackBasicWorkConfig*, CriManaPlaybackExWorkConfig*, void*, int)
{
}

void Cri::criManaPlayer_FreePlaybackWork(CriManaPlayerTag*)
{
}

void Cri::criManaPlayer_Prepare(CriManaPlayerTag*)
{
}

void Cri::criManaPlayer_DecodeHeader(CriManaPlayerTag*)
{
}

void Cri::criMana_SyncMasterTimer()
{
}

void Cri::criMana_ExecuteMain()
{
}

void* Cri::alloc(size_t size, size_t align)
{
	// MUST return real memory: the game allocates its player/acb work buffers through this
	// (a null return was one of the known silent-audio causes).
	return cri::atom::Alloc(size, align);
}

void Cri::free(void* p)
{
	cri::atom::Free(p);
}

namespace
{
	// The slot Gaiden's CRIWARE inserted. Never reached from the StF module (see Cri.h), so it
	// only has to exist and return harmlessly if some other module in that generation does call
	// it. Deliberately takes only `this`: with nothing known about its real signature, a callee
	// that touches no argument is the safe shape under the Microsoft x64 convention, where the
	// CALLER cleans up the stack.
	// The method Gaiden's CRIWARE inserted into icri, one slot after criAtomExPlayer_SetCueName:
	// its by-NUMBER counterpart. Gaiden's Sonic the Fighters module never calls it, which is why a
	// no-op stub was enough for that game. pre3 (Model 3) calls almost nothing else - its play
	// routine branches on a sign bit and takes the numeric path for every ordinary sound - so with
	// a stub here Fighting Vipers 2 selected no cue at all and ran silent while the rest of CRI
	// came up perfectly. Signature matches SetCueName's (player, acb, x) with a number for x.
	void __fastcall CriGaidenSetCueId(void*, CriAtomExPlayerTag* player, CriAtomExAcbTag* acb,
		unsigned int cueId)
	{
		cri::atom::PlayerSetCueId(player, acb, cueId);
	}

	// Storage for the patched vtable. Static rather than heap so its lifetime outlives the
	// stack-allocated Cri the hosts hand to module_start.
	void* g_criGaidenVtable[96];
}

void Cri::UseGaidenVtable()
{
	// icri declares 78 methods plus a virtual destructor = 79 slots, and Cri appends one more of
	// its own past the end of icri (criAtomExAcb_GetCueInfoById) = 80; +1 for the inserted one.
	//
	// kSlots WAS 79, on the reasoning that "the highest slot the Gaiden module actually calls is
	// 76, comfortably inside this". That was measured on Sonic the Fighters and is NOT true of the
	// Model 3 module, which calls slot 80 - and because this array is a static, the slot it read
	// was a zero nobody had written. Fighting Vipers 2 crashed with rip=0 on one stage as a result.
	// Anything appended to Cri past icri has to be counted here or it is invisible to the module.
	constexpr size_t kInsertAt = 9;   // between criAtomExPlayer_SetCueName and _SetVolume
	constexpr size_t kSlots = 80;
	static_assert(kSlots + 1 <= std::size(g_criGaidenVtable), "grow g_criGaidenVtable");

	void** const vt = *reinterpret_cast<void***>(this);
	std::memcpy(g_criGaidenVtable, vt, kInsertAt * sizeof(void*));
	g_criGaidenVtable[kInsertAt] = reinterpret_cast<void*>(&CriGaidenSetCueId);
	std::memcpy(g_criGaidenVtable + kInsertAt + 1, vt + kInsertAt,
		(kSlots - kInsertAt) * sizeof(void*));

	*reinterpret_cast<void***>(this) = g_criGaidenVtable;

	// EVERY slot must hold a real function before the module is handed this object. A hole is a
	// call through null the moment some game reaches the code that uses it - which is exactly how
	// the slot-80 bug presented: silent for two of the three games, then a crash several minutes
	// into a match, on one stage of the third. The arithmetic above is easy to get wrong (it
	// already was, once), so it is checked rather than trusted.
	size_t holes = 0;
	for (size_t i = 0; i <= kSlots; i++)
	{
		if (g_criGaidenVtable[i] == nullptr)
		{
			holes++;
			DebugLogFile("[cri] WARNING: icri vtable slot %zu is NULL - any game that calls it "
				"will crash with rip=0\n", i);
		}
	}
	DebugLogFile("[cri] using the Like a Dragon Gaiden icri layout (SetCueId at slot %zu, "
		"%zu slots, %zu holes)\n", kInsertAt, kSlots + 1, holes);
}

// See the long note on the declaration: returning 0 is "no cue info", which the module already
// has a path for, and writing nothing into the caller's struct is the only safe answer while its
// layout is unconfirmed.
int Cri::criAtomExAcb_GetCueInfoById(CriAtomExAcbTag*, unsigned int, void*)
{
	return 0;
}

int Cri::criAtomEx_CalculateWorkSizeForRegisterAcfData(void*, int)
{
	return 0;
}

void Cri::criAtomEx_RegisterAcfData(void*, int, void*, int)
{
}

void Cri::criAtomEx_UnregisterAcf()
{
}

int Cri::criAtomEx_CalculateWorkSizeForDspBusSetting(const char*)
{
	return 0;
}

int Cri::criAtomEx_CalculateWorkSizeForDspBusSettingFromAcfData(void*, int, const char*)
{
	return 0;
}

void Cri::criAtomEx_AttachDspBusSetting(const char*, void*, int)
{
}

CriAtomExVoicePoolTag* Cri::criAtomExVoicePool_AllocateStandardVoicePool(CriAtomExStandardVoicePoolConfigTag*, void*, int)
{
	// Voices are managed inside AtomEngine; hand back a distinct non-null token in case the
	// game checks the pool handle for success.
	static int dummyPool;
	return reinterpret_cast<CriAtomExVoicePoolTag*>(&dummyPool);
}

void Cri::criAtomExVoicePool_Free(CriAtomExVoicePoolTag*)
{
}

void Cri::criAtomDbas_Destroy(int)
{
}

void Cri::criAtomEx_DetachDspBusSetting()
{
}

void Cri::criAtomEx_ExecuteMain()
{
	cri::atom::ExecuteMain();
}

void Cri::criAtomExPlayer_SetBusSendLevelByName(CriAtomExPlayerTag*, const char*, float)
{
}

void Cri::criAtomExPlayer_SetBusSendLevelOffsetByName(CriAtomExPlayerTag*, const char*, float)
{
}

unsigned int Cri::criAtomExPlayer_Prepare(CriAtomExPlayerTag*)
{
	return 0;
}

void Cri::criAtomExPlayer_SetAisacControlById(CriAtomExPlayerTag*, unsigned int, float)
{
}

void Cri::criAtomExPlayer_Resume(CriAtomExPlayerTag* player, CriAtomExResumeModeTag mode)
{
	cri::atom::PlayerResume(player, static_cast<int>(mode));
}

void Cri::criAtomExPlayer_Update(CriAtomExPlayerTag*, unsigned int)
{
}

void Cri::unmount(unsigned int)
{
}

void Cri::remount(unsigned int)
{
}

void Cri::criAtomExPlayer_SetVoicePoolIdentifier(CriAtomExPlayerTag*, unsigned int)
{
}

void Cri::criAtomExPlayer_SetDspParameter(CriAtomExPlayerTag*, int, float)
{
}

int Cri::criAtomExAcb_GetWaveformInfoByName(CriAtomExAcbTag* acb, const char* cueName, CriAtomExWaveformInfoTag* info)
{
	return cri::atom::GetWaveformInfoByName(acb, cueName, info);
}
