#pragma once

#include "icri.h"

class Cri final : public icri
{
public:
	virtual CriAtomExPlayerTag* criAtomExPlayer_Create(CriAtomExPlayerConfigTag*, void*, int) override;
	virtual void criAtomExPlayer_Destroy(CriAtomExPlayerTag*) override;
	virtual int criAtomExPlayer_GetStatus(CriAtomExPlayerTag*) override;
	virtual CriAtomExAcbTag* criAtomExAcb_LoadAcbData(void*, int, CriFsBinderHnObjTag*, const char*, void*, int) override;
	virtual int criAtomExAcb_CalculateWorkSizeForLoadAcbData(void*, int, CriFsBinderHnObjTag*, const char*) override;
	virtual void criAtomExAcb_Release(CriAtomExAcbTag*) override;
	virtual void criAtomExPlayer_AttachAisac(CriAtomExPlayerTag*, const char*) override;
	virtual void criAtomExPlayer_DetachAisac(CriAtomExPlayerTag*, const char*) override;
	virtual void criAtomExPlayer_SetCueName(CriAtomExPlayerTag*, CriAtomExAcbTag*, const char*) override;
	virtual void criAtomExPlayer_SetVolume(CriAtomExPlayerTag*, float) override;
	virtual int criAtomExPlayer_CalculateWorkSize(CriAtomExPlayerConfigTag*) override;
	virtual unsigned int criAtomExPlayer_Start(CriAtomExPlayerTag*) override;
	virtual void criAtomExPlayer_Stop(CriAtomExPlayerTag*) override;
	virtual void criAtomExPlayer_StopWithoutReleaseTime(CriAtomExPlayerTag*) override;
	virtual void criAtomExPlayer_ResetParameters(CriAtomExPlayerTag*) override;
	virtual void criAtomExPlayer_UpdateAll(CriAtomExPlayerTag*) override;
	virtual void criAtomExPlayer_SetPitch(CriAtomExPlayerTag*, float) override;
	virtual void criAtomExPlayer_SetPan3dAngle(CriAtomExPlayerTag*, float) override;
	virtual void criAtomExPlayer_Pause(CriAtomExPlayerTag*, int) override;
	virtual void criAtomExPlayer_SetAisacControlByName(CriAtomExPlayerTag*, const char*, float) override;
	virtual int criAtomExPlayer_IsPaused(CriAtomExPlayerTag*) override;
	virtual void criAtom_ExecuteMain() override;
	virtual void criAtomExPlayer_SetData(CriAtomExPlayerTag*, void*, int) override;
	virtual void criAtomExPlayer_SetFormat(CriAtomExPlayerTag*, unsigned int) override;
	virtual void criAtomExPlayer_SetNumChannels(CriAtomExPlayerTag*, int) override;
	virtual void criAtomExPlayer_SetSamplingRate(CriAtomExPlayerTag*, int) override;
	virtual void criAtomExPlayer_SetFile(CriAtomExPlayerTag*, CriFsBinderHnObjTag*, const char*) override;
	virtual void criAtomExPlayer_LimitLoopCount(CriAtomExPlayerTag*, int) override;
	virtual void criAtomExPlayer_SetVoicePriority(CriAtomExPlayerTag*, int) override;
	virtual int criManaPlayer_CalculateHandleWorkSize() override;
	virtual CriManaPlayerTag* criManaPlayer_Create(void*, int) override;
	virtual void criManaPlayer_Destroy(CriManaPlayerTag*) override;
	virtual void criManaPlayer_SetFile(CriManaPlayerTag*, CriFsBinderHnObjTag*, const char*) override;
	virtual void criManaPlayer_Start(CriManaPlayerTag*) override;
	virtual void criManaPlayer_Stop(CriManaPlayerTag*) override;
	virtual void criManaPlayer_StopAndWaitCompletion(CriManaPlayerTag*) override;
	virtual void criManaPlayer_Pause(CriManaPlayerTag*, int) override;
	virtual int criManaPlayer_IsPaused(CriManaPlayerTag*) override;
	virtual void criManaPlayer_GetTime(CriManaPlayerTag*, unsigned __int64*, unsigned __int64*) override;
	virtual CriManaPlayerStatus criManaPlayer_GetStatus(CriManaPlayerTag*) override;
	virtual int criManaPlayer_ReferFrame(CriManaPlayerTag*, CriManaFrameInfo*) override;
	virtual int criManaPlayer_IsFrameOnTime(CriManaPlayerTag*, CriManaFrameInfo*) override;
	virtual void criManaPlayer_CopyFrameToBuffersYUV(CriManaPlayerTag*, CriManaFrameInfo*, CriManaTextureBuffersYUV*) override;
	virtual void criManaPlayer_DiscardFrame(CriManaPlayerTag*, CriManaFrameInfo*) override;
	virtual float criManaPlayer_GetVolume(CriManaPlayerTag*) override;
	virtual void criManaPlayer_SetVolume(CriManaPlayerTag*, float) override;
	virtual int criManaPlayer_GetPlaybackWorkParam(CriManaPlayerTag*, CriManaPlaybackBasicWorkConfig*, CriManaPlaybackExWorkConfig*) override;
	virtual int criManaPlayer_CalculatePlaybackWorkSize(CriManaPlayerTag*, CriManaPlaybackBasicWorkConfig*, CriManaPlaybackExWorkConfig*) override;
	virtual void criManaPlayer_SetPlaybackWork(CriManaPlayerTag*, CriManaPlaybackBasicWorkConfig*, CriManaPlaybackExWorkConfig*, void*, int) override;
	virtual void criManaPlayer_FreePlaybackWork(CriManaPlayerTag*) override;
	virtual void criManaPlayer_Prepare(CriManaPlayerTag*) override;
	virtual void criManaPlayer_DecodeHeader(CriManaPlayerTag*) override;
	virtual void criMana_SyncMasterTimer() override;
	virtual void criMana_ExecuteMain() override;
	virtual void* alloc(size_t size, size_t align) override;
	virtual void free(void*) override;
	virtual int criAtomEx_CalculateWorkSizeForRegisterAcfData(void*, int) override;
	virtual void criAtomEx_RegisterAcfData(void*, int, void*, int) override;
	virtual void criAtomEx_UnregisterAcf() override;
	virtual int criAtomEx_CalculateWorkSizeForDspBusSetting(const char*) override;
	virtual int criAtomEx_CalculateWorkSizeForDspBusSettingFromAcfData(void*, int, const char*) override;
	virtual void criAtomEx_AttachDspBusSetting(const char*, void*, int) override;
	virtual CriAtomExVoicePoolTag* criAtomExVoicePool_AllocateStandardVoicePool(CriAtomExStandardVoicePoolConfigTag*, void*, int) override;
	virtual void criAtomExVoicePool_Free(CriAtomExVoicePoolTag*) override;
	virtual void criAtomDbas_Destroy(int) override;
	virtual void criAtomEx_DetachDspBusSetting() override;
	virtual void criAtomEx_ExecuteMain() override;
	virtual void criAtomExPlayer_SetBusSendLevelByName(CriAtomExPlayerTag*, const char*, float) override;
	virtual void criAtomExPlayer_SetBusSendLevelOffsetByName(CriAtomExPlayerTag*, const char*, float) override;
	virtual unsigned int criAtomExPlayer_Prepare(CriAtomExPlayerTag*) override;
	virtual void criAtomExPlayer_SetAisacControlById(CriAtomExPlayerTag*, unsigned int, float) override;
	virtual void criAtomExPlayer_Resume(CriAtomExPlayerTag*, CriAtomExResumeModeTag) override;
	virtual void criAtomExPlayer_Update(CriAtomExPlayerTag*, unsigned int) override;
	virtual void unmount(unsigned int) override;
	virtual void remount(unsigned int) override;
	virtual void criAtomExPlayer_SetVoicePoolIdentifier(CriAtomExPlayerTag*, unsigned int) override;
	virtual void criAtomExPlayer_SetDspParameter(CriAtomExPlayerTag*, int, float) override;
	virtual int criAtomExAcb_GetWaveformInfoByName(CriAtomExAcbTag*, const char*, CriAtomExWaveformInfoTag*) override;

	// ---- One method past the end of icri, for the pre3 Model 3 module ------------------------
	//
	// NOT an `override`: icri.h does not declare it, so this deliberately appends a new slot at
	// the END of the vtable (index 79 in the base layout, 80 once UseGaidenVtable's insert shifts
	// everything from 9 up). That is exactly the slot the Model 3 module calls and the m2ftg ones
	// never do.
	//
	// FOUND BY CRASHING. Fighting Vipers 2 died with rip=0 - a call through a null function
	// pointer - on one particular stage, after several minutes of play. The caller is
	// DLL 0x180040335, `call qword ptr [rax+0x280]`, i.e. icri slot 80; the vtable it read is this
	// object's, and g_criGaidenVtable is a static whose entries past the copied range were never
	// written, so slot 80 was a literal zero. Sweeping every icri call site in the module puts the
	// next-highest slot it uses at 76, so this ONE method is the whole of what was missing -
	// UseGaidenVtable's "the highest slot the module actually calls is 76" was measured on Sonic
	// the Fighters and does not hold for pre3.
	//
	// WHAT IT IS. The module uses it in exactly one place (DLL 0x180040050, the sound-cue setup):
	//
	//     if (GetCueInfoById(0, cueId, &info) && info.name && strcmp(info.name, "dspoff") == 0)
	//         reverb = 0.0f;                      // a cue named "dspoff" suppresses the send
	//     else
	//         reverb = interpolate(param, range);
	//     SetAisacControlByName(player, "BUS_reverb_vf3", reverb);
	//
	// so it is CriAtomExAcb_GetCueInfoById, and `info.name` is read at offset +0x10 of the struct.
	//
	// RETURNS 0, AND WRITES NOTHING. "No cue info" is an answer the module already handles - it
	// takes the interpolated branch, which is what every cue not named "dspoff" gets anyway. The
	// only behaviour lost is suppressing reverb on a cue actually called "dspoff", and the result
	// is identical on every machine, so netplay stays in step either way. Filling the struct out
	// properly would mean knowing its size, and the one field the module reads sits at +0x10 where
	// the public CriAtomExCueInfo puts `name` at +0x08 - so this build's layout is NOT the
	// documented one, and guessing at it risks writing past a stack buffer to gain a reverb tweak.
	virtual int criAtomExAcb_GetCueInfoById(CriAtomExAcbTag*, unsigned int, void*);

	// ---- Like a Dragon Gaiden's icri layout -------------------------------------------------
	//
	// The module calls this object through its C++ vtable by SLOT INDEX, and the Gaiden build
	// (2023-11-27) was compiled against a newer CRIWARE whose icri has ONE EXTRA virtual method,
	// inserted between criAtomExPlayer_SetCueName (slot 8) and criAtomExPlayer_SetVolume (slot 9).
	// Every slot from 9 up is therefore one further along than icri.h declares.
	//
	// Measured, not guessed: sweeping every call made through the module's icri global gives 32
	// distinct slots and 67 call sites in BOTH builds, and each slot is either identical (index
	// <= 8) or exactly +8 bytes (index >= 9). The symptom of ignoring it is that the module's
	// `alloc(size, align)` lands on YAMP's `free` — which is how this was found: a heap
	// check_bytes failure inside Cri::free while loading stf.acf.
	//
	// The inserted method is never called by this module (the sweep finds no call to the slot it
	// occupies), so a no-op stub in its place is sufficient and nothing else has to change.
	// Repoints this object at a patched vtable; call once, before handing it to module_start.
	void UseGaidenVtable();
};