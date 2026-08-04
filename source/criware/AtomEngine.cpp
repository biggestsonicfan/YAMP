// XAudio2 2.9 needs a Win10 SDK target; the project globally targets Win7, so bump it for
// this TU only (must happen before any Windows header).
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0A00
#ifdef WINVER
#undef WINVER
#endif
#define WINVER 0x0A00

#include "AtomEngine.h"
#include "AdxDecoder.h"
#include "HcaDecoder.h"
#include "icri.h"
#include "../DebugLog.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "xaudio2.lib")

namespace fs = std::filesystem;

namespace cri::atom
{
	namespace
	{
		uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
		uint32_t Be32(const uint8_t* p)
		{
			return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
		}
		uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
		uint32_t Le32(const uint8_t* p)
		{
			return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
		}

		// ---- @UTF table reader ---------------------------------------------------------
		// Big-endian tagged tables (layout validated against stf_all.acb — see the audio
		// session notes). Header at +8: u16 version, u16 rowsOff, u32 stringsOff, u32
		// dataOff, u32 nameOff, u16 fieldCount, u16 rowSize, u32 rowCount; offsets are
		// relative to +8. Field flags: 0x10 = named, 0x20 = inline constant in the schema,
		// 0x40 = stored per row; low nibble = type.
		class UtfTable
		{
		public:
			bool Parse(const uint8_t* data, size_t size)
			{
				static constexpr uint8_t kTypeSize[12] = { 1, 1, 2, 2, 4, 4, 8, 8, 4, 8, 4, 8 };
				if (!data || size < 32 || std::memcmp(data, "@UTF", 4) != 0)
					return false;
				m_base = data + 8;
				m_size = size - 8;
				m_rowsOff = Be16(m_base + 2);
				m_strOff = Be32(m_base + 4);
				m_dataOff = Be32(m_base + 8);
				const uint32_t fieldCount = Be16(m_base + 16);
				m_rowSize = Be16(m_base + 18);
				m_rowCount = Be32(m_base + 20);

				const uint8_t* p = m_base + 24;
				const uint8_t* end = m_base + m_size;
				uint32_t rowOff = 0;
				for (uint32_t i = 0; i < fieldCount; ++i)
				{
					if (p >= end)
						return false;
					Field f;
					const uint8_t flags = *p++;
					f.type = flags & 0x0F;
					if (f.type > 0x0B)
						return false;
					if (flags & 0x10)
					{
						f.name = String(Be32(p));
						p += 4;
					}
					if (flags & 0x20) // inline constant
					{
						f.constant = p;
						p += (f.type == 0x0B) ? 8 : kTypeSize[f.type];
					}
					if (flags & 0x40) // per-row
					{
						f.rowOffset = int32_t(rowOff);
						rowOff += kTypeSize[f.type];
					}
					m_fields.push_back(f);
				}

				// The row STRIDE is the schema's, not the header's. @UTF version 1 (Like a Dragon
				// Gaiden's pre3 ACBs — fv2.acb/src2.acb) writes a rowLength that omits the three
				// trailing data columns of the ROOT table (PaddingArea, StreamAwbTocWork,
				// StreamAwbAfs2Header): it says 415 where the schema sums to 439. Rejecting that
				// mismatch is what made every pre3 ACB fail to parse, so the game loaded no cues
				// and ran silent while every other part of CRI came up fine.
				//
				// Measured on both generations, every table: the derived stride lands the row block
				// exactly on the string table in 13/13 cases, while the header field is right in 12
				// of 13. So the schema is the reliable source and this is not a pre3 special case —
				// v0 files (stf/fv/mr/omg) derive byte-identical values and are unaffected.
				if (m_rowCount != 0 && rowOff != m_rowSize)
				{
					DebugLog("[cri] @UTF v%u: header rowLength=%u but the schema sums to %u; "
						"using the schema\n", Be16(m_base), m_rowSize, rowOff);
				}
				if (rowOff != 0)
					m_rowSize = uint16_t(rowOff);

				// Still bounded: the row block has to fit inside the table.
				return m_rowCount == 0
					|| size_t(m_rowsOff) + size_t(m_rowCount) * m_rowSize <= m_size;
			}

			uint32_t RowCount() const { return m_rowCount; }

			// Integer fetch, widening any of u8..u32 (s-variants read as unsigned).
			bool GetU32(uint32_t row, const char* name, uint32_t& out) const
			{
				const uint8_t* v;
				uint8_t type;
				if (!Locate(row, name, v, type))
					return false;
				switch (type)
				{
				case 0: case 1: out = *v; return true;
				case 2: case 3: out = Be16(v); return true;
				case 4: case 5: out = Be32(v); return true;
				default: return false;
				}
			}

			const char* GetString(uint32_t row, const char* name) const
			{
				const uint8_t* v;
				uint8_t type;
				if (!Locate(row, name, v, type) || type != 0x0A)
					return nullptr;
				return String(Be32(v));
			}

			bool GetData(uint32_t row, const char* name, const uint8_t*& ptr, uint32_t& size) const
			{
				const uint8_t* v;
				uint8_t type;
				if (!Locate(row, name, v, type) || type != 0x0B)
					return false;
				ptr = m_base + m_dataOff + Be32(v);
				size = Be32(v + 4);
				return true;
			}

		private:
			struct Field
			{
				const char* name = "";
				uint8_t type = 0;
				const uint8_t* constant = nullptr;
				int32_t rowOffset = -1;
			};

			const char* String(uint32_t off) const
			{
				return reinterpret_cast<const char*>(m_base + m_strOff + off);
			}

			bool Locate(uint32_t row, const char* name, const uint8_t*& value, uint8_t& type) const
			{
				for (const Field& f : m_fields)
				{
					if (std::strcmp(f.name, name) != 0)
						continue;
					type = f.type;
					if (f.rowOffset >= 0)
					{
						if (row >= m_rowCount)
							return false;
						value = m_base + m_rowsOff + size_t(row) * m_rowSize + f.rowOffset;
						return true;
					}
					if (f.constant)
					{
						value = f.constant;
						return true;
					}
					return false; // storage "zero": treat as absent
				}
				return false;
			}

			const uint8_t* m_base = nullptr;
			size_t m_size = 0;
			uint32_t m_rowsOff = 0, m_strOff = 0, m_dataOff = 0;
			uint32_t m_rowSize = 0, m_rowCount = 0;
			std::vector<Field> m_fields;
		};

		// ---- AFS2 (.awb) ----------------------------------------------------------------
		// count u32 @8, align u16 @12, ids u16[count] @0x10, offsets u32[count+1] following;
		// entry data starts at align-up(offset[i]) and ends at raw offset[i+1].
		struct Afs2Toc
		{
			std::vector<uint16_t> ids;
			std::vector<uint32_t> offsets; // count+1 raw offsets
			uint16_t align = 32;

			bool Parse(const uint8_t* data, size_t size)
			{
				if (!data || size < 16 || std::memcmp(data, "AFS2", 4) != 0)
					return false;
				// The two table field widths are declared in the header, they are NOT fixed:
				// +5 = offset size, +6 = id size, each 2 or 4 bytes. Every m2ftg sheet is AFS2
				// version 1 with 2-byte ids, so hardcoding that worked until pre3 — Like a Dragon
				// Gaiden's Model 3 sheets are version 2 with **4-byte ids**. Reading those as 2
				// bytes corrupts the id list AND starts the offset table count*2 bytes early, so
				// every entry boundary is wrong: the correct waveform id then fetches a completely
				// different sound. That is the whole "audio plays but the sounds are wrong" bug —
				// the ACB side resolves perfectly (verified waveform 358 / MemoryAwbId 114 /
				// 2ch 44100 for FV2's SEGA voice), and only the AWB slicing was off.
				const uint8_t offSize = data[5];
				const uint8_t idSize = data[6];
				if ((offSize != 2 && offSize != 4) || (idSize != 2 && idSize != 4))
					return false;
				const uint32_t count = Le32(data + 8);
				align = Le16(data + 12);
				const size_t need = 0x10 + size_t(idSize) * count
					+ size_t(offSize) * (size_t(count) + 1);
				if (align == 0 || count > 0xFFFF || size < need)
					return false;
				ids.resize(count);
				offsets.resize(count + 1);
				auto field = [](const uint8_t* p, uint8_t width)
				{
					return width == 2 ? uint32_t(Le16(p)) : Le32(p);
				};
				for (uint32_t i = 0; i < count; ++i)
					ids[i] = uint16_t(field(data + 0x10 + size_t(idSize) * i, idSize));
				const size_t offTbl = 0x10 + size_t(idSize) * count;
				for (uint32_t i = 0; i <= count; ++i)
					offsets[i] = field(data + offTbl + size_t(offSize) * i, offSize);
				return true;
			}

			// Returns entry index for a waveform id, or -1.
			int Find(uint16_t id) const
			{
				for (size_t i = 0; i < ids.size(); ++i)
					if (ids[i] == id)
						return int(i);
				return -1;
			}

			bool Span(int index, size_t containerSize, size_t& beg, size_t& end) const
			{
				if (index < 0 || size_t(index) >= ids.size())
					return false;
				beg = (size_t(offsets[index]) + align - 1) / align * align;
				end = offsets[size_t(index) + 1];
				if (end > containerSize)
					end = containerSize;
				return beg < end;
			}
		};

		// ---- cue sheet (ACB) --------------------------------------------------------------
		struct WaveformRef
		{
			uint16_t id = 0;
			uint8_t streaming = 0;   // 0 = internal AwbFile blob, 1 = external .awb
			uint8_t encodeType = 0;  // 2 = HCA (everything shipped)
			uint8_t numChannels = 0;
			uint32_t samplingRate = 0;
			uint32_t numSamples = 0;
		};

		struct DecodedWave
		{
			std::vector<int16_t> pcm; // interleaved
			uint32_t channels = 0;
			uint32_t rate = 0;
			bool hasLoop = false;
			uint32_t loopBeginSample = 0;
			uint32_t loopLengthSamples = 0;
		};
		using WavePtr = std::shared_ptr<const DecodedWave>;

		struct Acb
		{
			uint64_t serial = 0;       // unique per load (cache keys must survive addr reuse)
			std::vector<uint8_t> data; // owned copy of the whole .acb
			std::unordered_map<std::string, WaveformRef> cues;
			// pre3 (Model 3) selects cues NUMERICALLY, never by name: its play routine branches on a
			// sign bit and calls the by-number setter, which is the very slot Gaiden's CRIWARE
			// inserted into icri. Both keys are kept because CueId and the CueTable row index are
			// distinct numbering schemes and only a live capture says which one a module passes.
			std::unordered_map<uint32_t, std::string> cueNameById;
			std::unordered_map<uint32_t, std::string> cueNameByIndex;

			const uint8_t* memAwb = nullptr; // internal AFS2 blob (into `data`)
			uint32_t memAwbSize = 0;
			Afs2Toc memToc;

			fs::path extAwbPath;   // resolved sibling .awb (empty if none needed/found)
			Afs2Toc extToc;
			size_t extSize = 0;

			bool released = false;
		};

		struct Player
		{
			enum class Source { None, Cue, File, Data };
			Source source = Source::None;

			// Pending cue selection (Source::Cue)
			Acb* acb = nullptr;          // null = search the registry
			std::string cueName;

			// Pending stream selection (Source::File / Source::Data)
			std::string filePath;        // as passed by the game; resolved at Start
			std::vector<uint8_t> dataBlob;
			int loopLimit = 0;           // -1 = infinite (criAtomExPlayer_LimitLoopCount)

			float volume = 1.0f;
			bool paused = false;
			int status = 0;              // 0 STOP, 1 PREP, 2 PLAYING, 3 PLAYEND, 4 ERROR
			bool looping = false;

			// Bumped on every Start/Stop/Destroy; an async decode only applies its result if
			// the generation still matches (stale results are cached but not played).
			uint64_t gen = 0;

			IXAudio2SourceVoice* voice = nullptr;
			uint32_t voiceChannels = 0;
			uint32_t voiceRate = 0;
			WavePtr wave;                // buffer the live voice plays from
			// Chunked (gapless) start: the first ~0.5 s plays from `waveChunk` while the full
			// stream decodes on the worker, which then queues the tail behind it.
			WavePtr waveChunk;
			bool pendingTail = false;
		};

		struct Engine
		{
			std::recursive_mutex lock;
			IXAudio2* xaudio = nullptr;
			IXAudio2MasteringVoice* master = nullptr;
			bool initTried = false;
			std::vector<Acb*> registry; // insertion order (NULL-acb cue search order)
			std::vector<Player*> players;
			uint64_t nextAcbSerial = 1;

			// Decoded-PCM cache (players hold shared_ptrs, so eviction never frees a buffer a
			// voice is still playing). Keeps rematch BGM starts instant.
			static constexpr size_t kCacheBudgetBytes = 128u << 20;
			std::unordered_map<std::string, WavePtr> cache;
			std::list<std::string> cacheLru; // front = most recent
			size_t cacheBytes = 0;

			// Async decode worker: large streams are decoded off the game thread so Start()
			// never stalls a frame (the player sits in PREP until the result is applied).
			struct DecodeJob
			{
				Player* player = nullptr;
				uint64_t gen = 0;
				std::vector<uint8_t> bytes;
				std::string cacheKey;
				std::string label;
				bool fromCuePath = false;
				int loopLimit = 0;
				// Nonzero = a chunked start is already audible; append the remainder from this
				// sample (per channel) instead of starting the voice fresh.
				uint32_t chunkSamples = 0;
			};
			std::mutex jobLock;
			std::condition_variable jobCv;
			std::deque<DecodeJob> jobs;
			bool workerStarted = false;

			bool EnsureInit()
			{
				if (xaudio)
					return true;
				if (initTried)
					return false;
				initTried = true;
				if (FAILED(XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)))
				{
					DebugLog("[cri] XAudio2Create FAILED - audio disabled\n");
					xaudio = nullptr;
					return false;
				}
				if (FAILED(xaudio->CreateMasteringVoice(&master)))
				{
					DebugLog("[cri] CreateMasteringVoice FAILED - audio disabled\n");
					xaudio->Release();
					xaudio = nullptr;
					return false;
				}
				DebugLog("[cri] XAudio2 initialized\n");
				return true;
			}
		};

		// Deliberately leaked: destroying XAudio2 during process teardown is a hang risk,
		// and the engine must outlive every game thread.
		Engine& E()
		{
			static Engine* e = new Engine();
			return *e;
		}

		// ---- ACB parsing --------------------------------------------------------------------
		struct AcbTables
		{
			UtfTable cue, cueName, wave;
			UtfTable synth, sequence, track, command;
			bool hasSynth = false, hasSequence = false, hasTrack = false, hasCommand = false;
		};

		// Reference dispatch shared by cues, synth items and sequence note-on events:
		// type 1 = waveform, 2/6 = synth, 3/7 = sequence. First resolvable item wins
		// (Y6 FUN_1436895d0 / FUN_14368ae70 / FUN_14368ac4c).
		bool ResolveRef(const AcbTables& t, uint32_t refType, uint32_t refIndex,
			uint32_t& waveformIndex, int depth = 0);

		bool ResolveSynth(const AcbTables& t, uint32_t index, uint32_t& waveformIndex, int depth)
		{
			// SynthTable.ReferenceItems = big-endian {u16 type, u16 index} pairs.
			const uint8_t* items;
			uint32_t size;
			if (!t.hasSynth || !t.synth.GetData(index, "ReferenceItems", items, size))
				return false;
			for (uint32_t i = 0; i + 4 <= size; i += 4)
			{
				if (ResolveRef(t, Be16(items + i), Be16(items + i + 2), waveformIndex, depth + 1))
					return true;
			}
			return false;
		}

		// Sequence cues (the whole VF5FS sheet set): SequenceTable row -> TrackIndex list ->
		// TrackTable.EventIndex -> CommandTable command stream. The stream is TLV
		// {u16 opcode, u8 size, payload}; opcode 2000/2003 (noteOn) carries
		// {u16 refType, u16 refIndex}.
		bool ResolveSequence(const AcbTables& t, uint32_t index, uint32_t& waveformIndex, int depth)
		{
			if (!t.hasSequence || !t.hasTrack || !t.hasCommand)
				return false;
			uint32_t numTracks = 0;
			const uint8_t* trackIdx;
			uint32_t trackIdxSize;
			if (!t.sequence.GetU32(index, "NumTracks", numTracks) ||
				!t.sequence.GetData(index, "TrackIndex", trackIdx, trackIdxSize))
				return false;
			if (numTracks > trackIdxSize / 2)
				numTracks = trackIdxSize / 2;

			for (uint32_t ti = 0; ti < numTracks; ++ti)
			{
				const uint16_t trackNo = Be16(trackIdx + ti * 2);
				uint32_t eventIndex = 0;
				if (!t.track.GetU32(trackNo, "EventIndex", eventIndex) || eventIndex == 0xFFFF)
					continue;
				const uint8_t* cmdBlob;
				uint32_t cmdSize;
				if (!t.command.GetData(eventIndex, "Command", cmdBlob, cmdSize))
					continue;
				uint32_t pos = 0;
				while (pos + 3 <= cmdSize)
				{
					const uint16_t op = Be16(cmdBlob + pos);
					const uint8_t len = cmdBlob[pos + 2];
					pos += 3;
					if (pos + len > cmdSize)
						break;
					if ((op == 2000 || op == 2003) && len >= 4) // noteOn / noteOnWithNo
					{
						if (ResolveRef(t, Be16(cmdBlob + pos), Be16(cmdBlob + pos + 2),
							waveformIndex, depth + 1))
							return true;
					}
					pos += len;
				}
			}
			return false;
		}

		bool ResolveRef(const AcbTables& t, uint32_t refType, uint32_t refIndex,
			uint32_t& waveformIndex, int depth)
		{
			if (depth > 8)
				return false;
			switch (refType)
			{
			case 1:
				waveformIndex = refIndex;
				return true;
			case 2: case 6:
				return ResolveSynth(t, refIndex, waveformIndex, depth);
			case 3: case 7:
				return ResolveSequence(t, refIndex, waveformIndex, depth);
			default:
				return false;
			}
		}

		// Locates the .acb file on disk whose bytes match the blob the game handed us, and
		// returns its sibling .awb. The game reads ACBs through its own file system, so
		// LoadAcbData only ever sees bytes — matching size + header bytes against the known
		// sound directories recovers the path (same trick the removed prototype used).
		fs::path FindExternalAwb(const std::vector<uint8_t>& acbData) try
		{
			wchar_t exePathBuf[MAX_PATH];
			GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
			const fs::path exeDir = fs::path(exePathBuf).parent_path();

			std::error_code cwdEc;
			const fs::path roots[] = { exeDir, fs::current_path(cwdEc) };
			// "image/sound" is where the Model 3 module (pre3) keeps its sheets — fv2.acb/.awb,
			// src2.acb/.awb — rather than the m2ftg "rom/sound". Both spellings of the root are
			// listed because the launcher runs with the game's media directory as the CWD while a
			// direct `YAMP.exe -fv2` runs from the exe directory, where it is under `pre3/`.
			const fs::path subdirs[] = {
				L"rom/sound", L"vf2/rom/sound", L"image/sound", L"pre3/image/sound",
			};
			std::error_code ec;
			for (const auto& root : roots)
			{
				for (const auto& sub : subdirs)
				{
					const fs::path dir = root / sub;
					if (!fs::is_directory(dir, ec))
						continue;
					for (const auto& entry : fs::directory_iterator(dir, ec))
					{
						if (!entry.is_regular_file(ec))
							continue;
						if (entry.path().extension() != L".acb")
							continue;
						if (entry.file_size(ec) != acbData.size())
							continue;
						std::ifstream f(entry.path(), std::ios::binary);
						uint8_t head[32] = {};
						f.read(reinterpret_cast<char*>(head), sizeof(head));
						if (!f || std::memcmp(head, acbData.data(),
							std::min(acbData.size(), sizeof(head))) != 0)
							continue;
						fs::path awb = entry.path();
						awb.replace_extension(L".awb");
						if (fs::is_regular_file(awb, ec))
							return awb;
					}
				}
			}
			return {};
		}
		catch (...)
		{
			// Filesystem oddities must never take the game down over audio.
			return {};
		}

		bool ParseAcb(Acb& acb)
		{
			UtfTable root;
			if (!root.Parse(acb.data.data(), acb.data.size()))
				return false;

			const uint8_t* blob;
			uint32_t blobSize;
			AcbTables t;
			if (!root.GetData(0, "CueTable", blob, blobSize) || !t.cue.Parse(blob, blobSize))
				return false;
			if (!root.GetData(0, "CueNameTable", blob, blobSize) || !t.cueName.Parse(blob, blobSize))
				return false;
			if (!root.GetData(0, "WaveformTable", blob, blobSize) || !t.wave.Parse(blob, blobSize))
				return false;
			t.hasSynth =
				root.GetData(0, "SynthTable", blob, blobSize) && t.synth.Parse(blob, blobSize);
			t.hasSequence =
				root.GetData(0, "SequenceTable", blob, blobSize) && t.sequence.Parse(blob, blobSize);
			t.hasTrack =
				root.GetData(0, "TrackTable", blob, blobSize) && t.track.Parse(blob, blobSize);
			// Sequence note-on streams live in TrackEventTable on newer sheets, CommandTable
			// on this era's (v1.28 and the old/simple v1.03 schema).
			t.hasCommand =
				root.GetData(0, "TrackEventTable", blob, blobSize) && blobSize > 0 &&
				t.command.Parse(blob, blobSize);
			if (!t.hasCommand)
				t.hasCommand =
					root.GetData(0, "CommandTable", blob, blobSize) && t.command.Parse(blob, blobSize);

			// Internal AFS2 blob with the in-memory SFX.
			if (root.GetData(0, "AwbFile", blob, blobSize) && blobSize > 16)
			{
				if (acb.memToc.Parse(blob, blobSize))
				{
					acb.memAwb = blob;
					acb.memAwbSize = blobSize;
				}
			}

			// Resolve every cue eagerly: name -> waveform row. Covers the old/simple v1.03
			// schema (StF/VF2/FV/MR: direct + synth refs, waveform "Id") and the v1.28 schema
			// (VF5FS: sequence refs, split MemoryAwbId/StreamAwbId).
			uint32_t streamedCues = 0, unresolved = 0;
			for (uint32_t r = 0; r < t.cueName.RowCount(); ++r)
			{
				const char* name = t.cueName.GetString(r, "CueName");
				uint32_t cueIndex;
				if (!name || !t.cueName.GetU32(r, "CueIndex", cueIndex))
					continue;
				uint32_t refType, refIndex;
				if (!t.cue.GetU32(cueIndex, "ReferenceType", refType) ||
					!t.cue.GetU32(cueIndex, "ReferenceIndex", refIndex))
					continue;

				uint32_t waveIndex = 0;
				if (!ResolveRef(t, refType, refIndex, waveIndex))
				{
					if (++unresolved <= 8)
						DebugLog("[cri] cue '%s': unresolved (ReferenceType %u)\n", name, refType);
					continue;
				}

				WaveformRef w;
				uint32_t v;
				if (t.wave.GetU32(waveIndex, "Streaming", v)) w.streaming = uint8_t(v);
				if (t.wave.GetU32(waveIndex, "Id", v))
					w.id = uint16_t(v);
				else if (t.wave.GetU32(waveIndex, w.streaming ? "StreamAwbId" : "MemoryAwbId", v))
					w.id = uint16_t(v);
				else
					continue;
				if (t.wave.GetU32(waveIndex, "EncodeType", v)) w.encodeType = uint8_t(v);
				if (t.wave.GetU32(waveIndex, "NumChannels", v)) w.numChannels = uint8_t(v);
				if (t.wave.GetU32(waveIndex, "SamplingRate", v)) w.samplingRate = v;
				if (t.wave.GetU32(waveIndex, "NumSamples", v)) w.numSamples = v;
				if (w.streaming)
					++streamedCues;
				acb.cues.emplace(name, w);
				acb.cueNameByIndex.emplace(cueIndex, name);
				{
					uint32_t cueId;
					if (t.cue.GetU32(cueIndex, "CueId", cueId))
						acb.cueNameById.emplace(cueId, name);
				}
			}
			if (unresolved)
				DebugLog("[cri] %u cues unresolved\n", unresolved);

			// External .awb only matters when some cue streams.
			if (streamedCues > 0)
			{
				acb.extAwbPath = FindExternalAwb(acb.data);
				if (acb.extAwbPath.empty())
					DebugLog("[cri] WARNING: %u streamed cues but no matching external .awb found\n", streamedCues);
				else
				{
					std::ifstream f(acb.extAwbPath, std::ios::binary | std::ios::ate);
					acb.extSize = size_t(f.tellg());
					f.seekg(0);
					std::vector<uint8_t> head(std::min<size_t>(acb.extSize, 0x10000));
					f.read(reinterpret_cast<char*>(head.data()), head.size());
					if (!acb.extToc.Parse(head.data(), head.size()))
					{
						DebugLog("[cri] WARNING: external awb TOC parse failed: %ls\n", acb.extAwbPath.c_str());
						acb.extAwbPath.clear();
					}
				}
			}
			return true;
		}

		// ---- waveform fetch + decode ----------------------------------------------------------
		bool FetchHcaBytes(Acb& acb, const WaveformRef& w, std::vector<uint8_t>& out)
		{
			if (w.streaming == 0)
			{
				if (!acb.memAwb)
					return false;
				const int idx = acb.memToc.Find(w.id);
				size_t beg, end;
				if (!acb.memToc.Span(idx, acb.memAwbSize, beg, end))
					return false;
				out.assign(acb.memAwb + beg, acb.memAwb + end);
				return true;
			}
			if (acb.extAwbPath.empty())
				return false;
			const int idx = acb.extToc.Find(w.id);
			size_t beg, end;
			if (!acb.extToc.Span(idx, acb.extSize, beg, end))
				return false;
			std::ifstream f(acb.extAwbPath, std::ios::binary);
			if (!f)
				return false;
			out.resize(end - beg);
			f.seekg(std::streamoff(beg));
			f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
			return bool(f);
		}

		bool DecodeWholeHca(const std::vector<uint8_t>& hca, DecodedWave& out)
		{
			cri::hca::Decoder dec;
			if (!dec.Init(hca.data(), hca.size()))
				return false;
			const cri::hca::Info& info = dec.GetInfo();
			out.channels = info.channelCount;
			out.rate = info.samplingRate;

			const size_t frameSamples = size_t(1024) * info.channelCount;
			std::vector<float> frame(frameSamples);
			out.pcm.reserve(size_t(info.blockCount) * frameSamples);
			for (uint32_t b = 0; b < info.blockCount; ++b)
			{
				const size_t off = size_t(info.headerSize) + size_t(b) * info.blockSize;
				if (off + info.blockSize > hca.size())
					break;
				if (!dec.DecodeFrame(&hca[off], info.blockSize, frame.data()))
					continue; // damaged frame: skip (keeps loop math on block granularity)
				for (size_t i = 0; i < frameSamples; ++i)
				{
					// Same conversion as the validated reference decoder.
					int s = int(32768.0f * frame[i]);
					if (s > 32767) s = 32767;
					else if (s < -32767) s = -32767;
					out.pcm.push_back(int16_t(s));
				}
			}
			if (out.pcm.empty())
				return false;

			if (info.hasLoop && info.loopEnd >= info.loopStart)
			{
				// Loop points are block indexes (1024 samples per block), end inclusive.
				out.hasLoop = true;
				out.loopBeginSample = info.loopStart * 1024u;
				out.loopLengthSamples = (info.loopEnd + 1 - info.loopStart) * 1024u;
				const uint32_t total = uint32_t(out.pcm.size() / info.channelCount);
				if (out.loopBeginSample >= total)
					out.hasLoop = false;
				else if (out.loopBeginSample + out.loopLengthSamples > total)
					out.loopLengthSamples = total - out.loopBeginSample;
			}
			return true;
		}

		bool DecodeWholeAdx(const std::vector<uint8_t>& adx, DecodedWave& out)
		{
			cri::adx::Info info;
			if (!cri::adx::ParseHeader(adx.data(), adx.size(), info))
				return false;
			out.channels = info.channelCount;
			out.rate = info.samplingRate;
			out.pcm.resize(size_t(info.sampleCount) * info.channelCount);
			const uint32_t got = cri::adx::DecodeAll(info, adx.data(), adx.size(), out.pcm.data());
			if (got == 0)
				return false;
			out.pcm.resize(size_t(got) * info.channelCount);
			if (info.hasLoop && info.loopStartSample < got)
			{
				out.hasLoop = true;
				out.loopBeginSample = info.loopStartSample;
				const uint32_t end = info.loopEndSample < got ? info.loopEndSample : got;
				out.loopLengthSamples = end - info.loopStartSample;
			}
			return true;
		}

		// Decodes only the first `maxSamples` (per channel) of an HCA stream — the audible head
		// of a chunked start — but fills the loop metadata for the FULL stream so the caller
		// can decide whether the loop region lies safely inside the (future) tail buffer.
		bool DecodeHcaPrefix(const std::vector<uint8_t>& hca, uint32_t maxSamples, DecodedWave& out)
		{
			cri::hca::Decoder dec;
			if (!dec.Init(hca.data(), hca.size()))
				return false;
			const cri::hca::Info& info = dec.GetInfo();
			out.channels = info.channelCount;
			out.rate = info.samplingRate;

			const size_t frameSamples = size_t(1024) * info.channelCount;
			std::vector<float> frame(frameSamples);
			uint32_t frames = (maxSamples + 1023) / 1024;
			if (frames > info.blockCount)
				frames = info.blockCount;
			out.pcm.reserve(size_t(frames) * frameSamples);
			for (uint32_t b = 0; b < frames; ++b)
			{
				const size_t off = size_t(info.headerSize) + size_t(b) * info.blockSize;
				if (off + info.blockSize > hca.size())
					break;
				if (!dec.DecodeFrame(&hca[off], info.blockSize, frame.data()))
					continue;
				for (size_t i = 0; i < frameSamples; ++i)
				{
					int s = int(32768.0f * frame[i]);
					if (s > 32767) s = 32767;
					else if (s < -32767) s = -32767;
					out.pcm.push_back(int16_t(s));
				}
			}
			if (out.pcm.empty())
				return false;

			// Full-stream loop metadata (same math as DecodeWholeHca, against the total length).
			if (info.hasLoop && info.loopEnd >= info.loopStart)
			{
				out.hasLoop = true;
				out.loopBeginSample = info.loopStart * 1024u;
				out.loopLengthSamples = (info.loopEnd + 1 - info.loopStart) * 1024u;
				const uint32_t total = info.blockCount * 1024u;
				if (out.loopBeginSample >= total)
					out.hasLoop = false;
				else if (out.loopBeginSample + out.loopLengthSamples > total)
					out.loopLengthSamples = total - out.loopBeginSample;
			}
			return true;
		}

		// Streams the game hands us by file/blob are ADX (all of VF5FS's bgm/voice) but sniff
		// the magic so an HCA file works too.
		bool DecodeAuto(const std::vector<uint8_t>& bytes, DecodedWave& out)
		{
			if (bytes.size() >= 4 && bytes[0] == 0x80 && bytes[1] == 0x00)
				return DecodeWholeAdx(bytes, out);
			if (bytes.size() >= 4 && (bytes[0] & 0x7F) == 'H' && (bytes[1] & 0x7F) == 'C' &&
				(bytes[2] & 0x7F) == 'A')
				return DecodeWholeHca(bytes, out);
			return false;
		}

		// SetFile paths come from the game's own path building; resolve them against the exe
		// dir / cwd first, then fall back to a filename lookup across the known sound roots.
		fs::path ResolveStreamFile(const std::string& raw) try
		{
			std::error_code ec;
			fs::path asGiven = fs::u8path(raw);
			if (asGiven.is_absolute() && fs::is_regular_file(asGiven, ec))
				return asGiven;

			wchar_t exePathBuf[MAX_PATH];
			GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
			const fs::path exeDir = fs::path(exePathBuf).parent_path();
			std::error_code cwdEc;
			const fs::path roots[] = { exeDir, fs::current_path(cwdEc) };
			for (const auto& root : roots)
				if (fs::is_regular_file(root / asGiven, ec))
					return root / asGiven;

			// Filename index over the sound trees (built once).
			static std::unordered_map<std::wstring, fs::path> index;
			static bool indexBuilt = false;
			if (!indexBuilt)
			{
				indexBuilt = true;
				const fs::path soundRoots[] = {
					exeDir / L"vf5fs/vf5fs_media/rom/sound",
					exeDir / L"rom/sound",
					exeDir / L"vf2/rom/sound",
				};
				for (const auto& sr : soundRoots)
				{
					if (!fs::is_directory(sr, ec))
						continue;
					for (auto it = fs::recursive_directory_iterator(sr, ec);
						it != fs::recursive_directory_iterator(); it.increment(ec))
					{
						if (ec)
							break;
						if (!it->is_regular_file(ec))
							continue;
						std::wstring key = it->path().filename().wstring();
						for (auto& c : key)
							c = wchar_t(towlower(c));
						index.emplace(std::move(key), it->path());
					}
				}
				DebugLog("[cri] stream-file index built: %zu files\n", index.size());
			}
			std::wstring key = asGiven.filename().wstring();
			for (auto& c : key)
				c = wchar_t(towlower(c));
			auto it = index.find(key);
			return it != index.end() ? it->second : fs::path{};
		}
		catch (...)
		{
			return {};
		}

		void DestroyVoice(Player& p)
		{
			if (p.voice)
			{
				p.voice->Stop(0);
				p.voice->FlushSourceBuffers();
				p.voice->DestroyVoice();
				p.voice = nullptr;
			}
		}

		// ---- decoded-wave cache (engine lock held) --------------------------------------------
		WavePtr CacheGet(Engine& e, const std::string& key)
		{
			auto it = e.cache.find(key);
			if (it == e.cache.end())
				return nullptr;
			for (auto l = e.cacheLru.begin(); l != e.cacheLru.end(); ++l)
			{
				if (*l == key)
				{
					e.cacheLru.splice(e.cacheLru.begin(), e.cacheLru, l);
					break;
				}
			}
			return it->second;
		}

		void CachePut(Engine& e, const std::string& key, const WavePtr& wave)
		{
			if (key.empty() || !wave)
				return;
			auto it = e.cache.find(key);
			if (it != e.cache.end())
				return; // already there (a duplicate decode of the same stream is harmless)
			e.cache.emplace(key, wave);
			e.cacheLru.push_front(key);
			e.cacheBytes += wave->pcm.size() * sizeof(int16_t);
			while (e.cacheBytes > Engine::kCacheBudgetBytes && e.cacheLru.size() > 1)
			{
				const std::string& victim = e.cacheLru.back();
				auto v = e.cache.find(victim);
				if (v != e.cache.end())
				{
					e.cacheBytes -= v->second->pcm.size() * sizeof(int16_t);
					e.cache.erase(v);
				}
				e.cacheLru.pop_back();
			}
		}

		// (Re)creates the player's source voice for the given format, or stops+flushes the
		// existing one when it already matches. Engine lock must be held.
		bool PrepareVoice(Engine& e, Player& p, uint32_t channels, uint32_t rate)
		{
			if (p.voice && (p.voiceChannels != channels || p.voiceRate != rate))
				DestroyVoice(p);
			if (!p.voice)
			{
				WAVEFORMATEX fmt = {};
				fmt.wFormatTag = WAVE_FORMAT_PCM;
				fmt.nChannels = WORD(channels);
				fmt.nSamplesPerSec = rate;
				fmt.wBitsPerSample = 16;
				fmt.nBlockAlign = WORD(channels * 2);
				fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
				if (FAILED(e.xaudio->CreateSourceVoice(&p.voice, &fmt)))
				{
					DebugLog("[cri] CreateSourceVoice FAILED (ch=%u rate=%u)\n", channels, rate);
					p.voice = nullptr;
					return false;
				}
				p.voiceChannels = channels;
				p.voiceRate = rate;
			}
			else
			{
				p.voice->Stop(0);
				p.voice->FlushSourceBuffers();
			}
			return true;
		}

		// Binds a decoded wave to the player's voice and (unless pause-preloaded) starts it.
		// Engine lock must be held. Loop policy: the stream's own loop points win; a non-cue
		// player with LimitLoopCount(-1) loops the whole buffer.
		void ApplyWave(Engine& e, Player& p, const WavePtr& wave, const char* label,
			bool fromCuePath, int loopLimit)
		{
			bool loop = wave->hasLoop;
			uint32_t loopBegin = wave->loopBeginSample;
			uint32_t loopLen = wave->loopLengthSamples;
			if (!loop && !fromCuePath && loopLimit < 0)
			{
				loop = true;
				loopBegin = 0;
				loopLen = uint32_t(wave->pcm.size() / wave->channels);
			}

			if (!PrepareVoice(e, p, wave->channels, wave->rate))
			{
				p.status = 4;
				return;
			}

			p.wave = wave;
			p.waveChunk.reset();
			p.pendingTail = false;
			XAUDIO2_BUFFER buf = {};
			buf.AudioBytes = UINT32(wave->pcm.size() * sizeof(int16_t));
			buf.pAudioData = reinterpret_cast<const BYTE*>(wave->pcm.data());
			buf.Flags = XAUDIO2_END_OF_STREAM;
			if (loop)
			{
				buf.LoopBegin = loopBegin;
				buf.LoopLength = loopLen;
				buf.LoopCount = XAUDIO2_LOOP_INFINITE;
			}
			if (FAILED(p.voice->SubmitSourceBuffer(&buf)))
			{
				DebugLog("[cri] SubmitSourceBuffer FAILED (%s)\n", label);
				p.status = 4;
				return;
			}
			p.voice->SetVolume(p.volume);
			p.looping = loop;
			// Preload protocol: the game may Pause(1) BEFORE Start; output begins on Pause(0).
			if (!p.paused)
				p.voice->Start(0);
			p.status = 2;
			DebugLog("[cri] Start: %s (ch=%u rate=%u, %zu samples%s)\n", label,
				wave->channels, wave->rate, wave->pcm.size() / wave->channels,
				loop ? ", looped" : "");
		}

		bool PlayerAlive(Engine& e, Player* p)
		{
			return std::find(e.players.begin(), e.players.end(), p) != e.players.end();
		}

		void DecodeWorkerLoop()
		{
			Engine& e = E();
			for (;;)
			{
				Engine::DecodeJob job;
				{
					std::unique_lock<std::mutex> ql(e.jobLock);
					e.jobCv.wait(ql, [&] { return !e.jobs.empty(); });
					job = std::move(e.jobs.front());
					e.jobs.pop_front();
				}

				auto wave = std::make_shared<DecodedWave>();
				const bool ok = DecodeAuto(job.bytes, *wave);

				std::lock_guard<std::recursive_mutex> g(e.lock);
				if (ok)
					CachePut(e, job.cacheKey, wave);
				// Only touch the player if it still exists and nothing superseded this start.
				if (!PlayerAlive(e, job.player) || job.player->gen != job.gen)
					continue;
				Player& p = *job.player;
				if (!ok)
				{
					DebugLog("[cri] async decode FAILED (%s)\n", job.label.c_str());
					if (p.voice)
						p.voice->Stop(0);
					p.pendingTail = false;
					p.status = 4;
					continue;
				}
				if (job.chunkSamples > 0 && p.voice)
				{
					// Chunked start: the head is already audible — queue the remainder behind
					// it. The loop region was verified to sit entirely inside this tail.
					const uint32_t total = uint32_t(wave->pcm.size() / wave->channels);
					if (job.chunkSamples >= total)
					{
						p.wave = wave;
						p.pendingTail = false;
						continue; // the head already covered the whole stream
					}
					XAUDIO2_BUFFER buf = {};
					buf.pAudioData = reinterpret_cast<const BYTE*>(
						wave->pcm.data() + size_t(job.chunkSamples) * wave->channels);
					buf.AudioBytes =
						UINT32((size_t(total) - job.chunkSamples) * wave->channels * sizeof(int16_t));
					buf.Flags = XAUDIO2_END_OF_STREAM;
					if (wave->hasLoop && wave->loopBeginSample >= job.chunkSamples)
					{
						buf.LoopBegin = wave->loopBeginSample - job.chunkSamples;
						buf.LoopLength = wave->loopLengthSamples;
						buf.LoopCount = XAUDIO2_LOOP_INFINITE;
					}
					if (FAILED(p.voice->SubmitSourceBuffer(&buf)))
					{
						DebugLog("[cri] tail SubmitSourceBuffer FAILED (%s)\n", job.label.c_str());
						p.status = 4;
					}
					else
					{
						p.wave = wave; // keep waveChunk alive too until the head finishes playing
						p.looping = buf.LoopCount != 0;
						p.pendingTail = false;
						DebugLog("[cri] Start tail: %s (+%u samples%s)\n", job.label.c_str(),
							total - job.chunkSamples, p.looping ? ", looped" : "");
					}
					continue;
				}
				ApplyWave(e, p, wave, job.label.c_str(), job.fromCuePath, job.loopLimit);
			}
		}

		void QueueDecodeJob(Engine& e, Engine::DecodeJob&& job)
		{
			{
				std::lock_guard<std::mutex> ql(e.jobLock);
				if (!e.workerStarted)
				{
					e.workerStarted = true;
					std::thread(DecodeWorkerLoop).detach();
				}
				e.jobs.push_back(std::move(job));
			}
			e.jobCv.notify_one();
		}

		Acb* FindCueAcb(Acb* preferred, const char* name, const WaveformRef*& outRef)
		{
			Engine& e = E();
			if (preferred)
			{
				auto it = preferred->cues.find(name);
				if (it != preferred->cues.end())
				{
					outRef = &it->second;
					return preferred;
				}
				return nullptr;
			}
			// NULL acb: first registered sheet containing the cue (criAtomExAcb_FindAcbByCueName).
			for (Acb* acb : e.registry)
			{
				auto it = acb->cues.find(name);
				if (it != acb->cues.end())
				{
					outRef = &it->second;
					return acb;
				}
			}
			return nullptr;
		}
	}

	// ---- public API ------------------------------------------------------------------------

	void* Alloc(size_t size, size_t align)
	{
		if (align < 16)
			align = 16;
		return _aligned_malloc(size ? size : 1, align);
	}

	void Free(void* p)
	{
		_aligned_free(p);
	}

	CriAtomExAcbTag* LoadAcbData(const void* data, int size)
	{
		if (!data || size <= 0)
			return nullptr;
		auto acb = std::make_unique<Acb>();
		acb->data.assign(static_cast<const uint8_t*>(data),
			static_cast<const uint8_t*>(data) + size);
		if (!ParseAcb(*acb))
		{
			DebugLog("[cri] LoadAcbData: parse FAILED (%d bytes)\n", size);
			return nullptr;
		}
		std::lock_guard<std::recursive_mutex> g(E().lock);
		acb->serial = E().nextAcbSerial++;
		E().registry.push_back(acb.get());
		DebugLog("[cri] LoadAcbData: %d bytes, %zu cues, memAwb=%zu entries, extAwb=%zu entries (%ls)\n",
			size, acb->cues.size(), acb->memToc.ids.size(), acb->extToc.ids.size(),
			acb->extAwbPath.empty() ? L"none" : acb->extAwbPath.filename().c_str());
		return reinterpret_cast<CriAtomExAcbTag*>(acb.release());
	}

	void ReleaseAcb(CriAtomExAcbTag* handle)
	{
		if (!handle)
			return;
		Acb* acb = reinterpret_cast<Acb*>(handle);
		std::lock_guard<std::recursive_mutex> g(E().lock);
		auto& reg = E().registry;
		reg.erase(std::remove(reg.begin(), reg.end(), acb), reg.end());
		// Any player still pointing here loses its pending selection.
		for (Player* p : E().players)
			if (p->acb == acb)
				p->acb = nullptr;
		delete acb;
	}

	int GetWaveformInfoByName(CriAtomExAcbTag* handle, const char* cueName, CriAtomExWaveformInfoTag* out)
	{
		if (!cueName || !out)
			return 0;
		std::lock_guard<std::recursive_mutex> g(E().lock);
		const WaveformRef* ref = nullptr;
		if (!FindCueAcb(reinterpret_cast<Acb*>(handle), cueName, ref))
			return 0;
		std::memset(out, 0, sizeof(*out));
		out->wave_id = ref->id;
		out->format = (ref->encodeType == 2) ? 3u : 1u; // CRIATOM_FORMAT_HCA / _ADX
		out->sampling_rate = int(ref->samplingRate);
		out->num_channels = ref->numChannels;
		out->num_samples = ref->numSamples;
		out->streaming_flag = ref->streaming;
		return 1;
	}

	CriAtomExPlayerTag* PlayerCreate()
	{
		Player* p = new Player();
		std::lock_guard<std::recursive_mutex> g(E().lock);
		E().players.push_back(p);
		return reinterpret_cast<CriAtomExPlayerTag*>(p);
	}

	void PlayerDestroy(CriAtomExPlayerTag* handle)
	{
		if (!handle)
			return;
		Player* p = reinterpret_cast<Player*>(handle);
		std::lock_guard<std::recursive_mutex> g(E().lock);
		++p->gen;
		DestroyVoice(*p);
		// The decode worker re-checks membership under this same lock before dereferencing a
		// job's player, so erase + delete here is race-free.
		auto& v = E().players;
		v.erase(std::remove(v.begin(), v.end(), p), v.end());
		delete p;
	}

	// Numeric cue selection, resolved to a name and handed to the by-name path so cue lookup,
	// ACB fallback and every diagnostic stay in one place. Tries CueId first (what CRI's
	// SetCueId means) and falls back to the CueTable row index, reporting which one hit so the
	// ambiguity can be settled from a log rather than guessed at.
	void PlayerSetCueId(CriAtomExPlayerTag* handle, CriAtomExAcbTag* acb, uint32_t id)
	{
		const std::string* name = nullptr;
		const char* how = nullptr;
		auto look = [&](Acb* a)
		{
			if (!a || name) return;
			auto i = a->cueNameById.find(id);
			if (i != a->cueNameById.end()) { name = &i->second; how = "CueId"; return; }
			auto j = a->cueNameByIndex.find(id);
			if (j != a->cueNameByIndex.end()) { name = &j->second; how = "CueIndex"; }
		};
		look(reinterpret_cast<Acb*>(acb));
		for (Acb* a : E().registry) look(a);

		static int logged = 0;
		if (++logged <= 12)
		{
			DebugLog("[cri] SetCueId(%u) -> %s (by %s)\n", id,
				name ? name->c_str() : "*** NO SUCH CUE ***", how ? how : "-");
		}
		if (!name) return;
		PlayerSetCueName(handle, acb, name->c_str());
	}

	void PlayerSetCueName(CriAtomExPlayerTag* handle, CriAtomExAcbTag* acb, const char* cueName)
	{
		if (!handle)
			return;
		Player* p = reinterpret_cast<Player*>(handle);
		std::lock_guard<std::recursive_mutex> g(E().lock);
		p->source = Player::Source::Cue;
		p->acb = reinterpret_cast<Acb*>(acb);
		p->cueName = cueName ? cueName : "";
	}

	void PlayerSetFile(CriAtomExPlayerTag* handle, const char* path)
	{
		if (!handle)
			return;
		Player* p = reinterpret_cast<Player*>(handle);
		std::lock_guard<std::recursive_mutex> g(E().lock);
		p->source = Player::Source::File;
		p->filePath = path ? path : "";
		p->dataBlob.clear();
		DebugLog("[cri] SetFile: '%s'\n", p->filePath.c_str());
	}

	void PlayerSetData(CriAtomExPlayerTag* handle, const void* data, int size)
	{
		if (!handle)
			return;
		Player* p = reinterpret_cast<Player*>(handle);
		std::lock_guard<std::recursive_mutex> g(E().lock);
		p->source = Player::Source::Data;
		p->filePath.clear();
		if (data && size > 0)
			p->dataBlob.assign(static_cast<const uint8_t*>(data),
				static_cast<const uint8_t*>(data) + size);
		else
			p->dataBlob.clear();
	}

	void PlayerSetFormat(CriAtomExPlayerTag*, unsigned int)
	{
		// Hint only (1 = ADX for the VF5FS BGM ports); the stream header is authoritative.
	}

	void PlayerSetNumChannels(CriAtomExPlayerTag*, int)
	{
	}

	void PlayerSetSamplingRate(CriAtomExPlayerTag*, int)
	{
	}

	void PlayerLimitLoopCount(CriAtomExPlayerTag* handle, int count)
	{
		if (!handle)
			return;
		Player* p = reinterpret_cast<Player*>(handle);
		std::lock_guard<std::recursive_mutex> g(E().lock);
		p->loopLimit = count;
	}

	void PlayerSetVolume(CriAtomExPlayerTag* handle, float linearVolume)
	{
		if (!handle)
			return;
		Player* p = reinterpret_cast<Player*>(handle);
		std::lock_guard<std::recursive_mutex> g(E().lock);
		if (!(linearVolume >= 0.0f))
			linearVolume = 0.0f;
		if (linearVolume > 4.0f)
			linearVolume = 4.0f;
		// TEMP diagnostics: log volume changes (not the steady per-frame repeats).
		if (linearVolume != p->volume)
			DebugLog("[cri] SetVolume: player %p %.4f -> %.4f\n", (void*)p, p->volume, linearVolume);
		p->volume = linearVolume;
		if (p->voice)
			p->voice->SetVolume(linearVolume);
	}

	unsigned int PlayerStart(CriAtomExPlayerTag* handle)
	{
		if (!handle)
			return 0;
		Player* p = reinterpret_cast<Player*>(handle);
		std::lock_guard<std::recursive_mutex> g(E().lock);
		Engine& e = E();
		if (!e.EnsureInit())
		{
			p->status = 4;
			return 0;
		}

		// Gather the raw stream bytes + a cache key without doing any decode work yet.
		std::vector<uint8_t> bytes;
		std::string cacheKey;
		std::string label;
		const bool fromCuePath = p->source == Player::Source::Cue;
		if (p->source == Player::Source::Cue && !p->cueName.empty())
		{
			const WaveformRef* ref = nullptr;
			Acb* acb = FindCueAcb(p->acb, p->cueName.c_str(), ref);
			if (!acb)
			{
				DebugLog("[cri] Start: cue '%s' not found in any loaded ACB\n", p->cueName.c_str());
				p->status = 4;
				return 0;
			}
			char key[64];
			std::snprintf(key, sizeof(key), "A:%llu:%u:%u",
				(unsigned long long)acb->serial, ref->id, ref->streaming);
			cacheKey = key;
			label = "cue '" + p->cueName + "'";
			if (!CacheGet(e, cacheKey) && !FetchHcaBytes(*acb, *ref, bytes))
			{
				DebugLog("[cri] Start: cue '%s' (id %u, enc %u, %s) fetch FAILED\n", p->cueName.c_str(),
					ref->id, ref->encodeType, ref->streaming ? "stream" : "memory");
				p->status = 4;
				return 0;
			}
		}
		else if (p->source == Player::Source::File && !p->filePath.empty())
		{
			const fs::path path = ResolveStreamFile(p->filePath);
			if (!path.empty())
			{
				cacheKey = "F:" + path.u8string();
				if (!CacheGet(e, cacheKey))
				{
					std::ifstream f(path, std::ios::binary | std::ios::ate);
					if (f)
					{
						bytes.resize(size_t(f.tellg()));
						f.seekg(0);
						f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
						if (!f)
							bytes.clear();
					}
				}
			}
			if (cacheKey.empty() || (!CacheGet(e, cacheKey) && bytes.empty()))
			{
				DebugLog("[cri] Start: file '%s' -> '%ls' load FAILED\n", p->filePath.c_str(),
					path.empty() ? L"(unresolved)" : path.c_str());
				p->status = 4;
				return 0;
			}
			label = "file '" + p->filePath + "'";
		}
		else if (p->source == Player::Source::Data && !p->dataBlob.empty())
		{
			bytes = p->dataBlob; // no stable identity for a raw blob: not cached
			label = "data blob";
		}
		else
		{
			p->status = 4;
			return 0;
		}

		// Supersede any in-flight async decode for this player.
		++p->gen;

		if (const WavePtr cached = CacheGet(e, cacheKey))
		{
			ApplyWave(e, *p, cached, label.c_str(), fromCuePath, p->loopLimit);
			return p->status == 4 ? 0 : 1;
		}

		// Small streams (all the SFX) and ADX (cheap to decode at any size) go inline — they
		// take a few ms at most and the sound must land on the exact frame.
		constexpr size_t kSyncDecodeRawBytes = 64 * 1024;
		const bool isAdx = bytes.size() >= 2 && bytes[0] == 0x80 && bytes[1] == 0x00;
		if (bytes.size() <= kSyncDecodeRawBytes || isAdx)
		{
			auto wave = std::make_shared<DecodedWave>();
			if (!DecodeAuto(bytes, *wave))
			{
				DebugLog("[cri] Start: %s decode FAILED\n", label.c_str());
				p->status = 4;
				return 0;
			}
			CachePut(e, cacheKey, wave);
			ApplyWave(e, *p, wave, label.c_str(), fromCuePath, p->loopLimit);
			return p->status == 4 ? 0 : 1;
		}

		Engine::DecodeJob job;
		job.player = p;
		job.gen = p->gen;
		job.cacheKey = std::move(cacheKey);
		job.label = std::move(label);
		job.fromCuePath = fromCuePath;
		job.loopLimit = p->loopLimit;

		// Gapless chunked start for big HCA cues (LJ starts fight BGM with NO gap after the
		// gong): synchronously decode ~0.5 s — a couple dozen frames, instant with the
		// optimized decoder — start it NOW, and let the worker append the remainder. Only
		// safe when the loop region sits entirely past the head (true for every StF BGM;
		// whole-track loops fall back to the PREP path below).
		if (fromCuePath)
		{
			// ~2 s head at 48 kHz: still a handful of milliseconds to decode, and a generous
			// runway for the worker to land the tail before the head runs dry.
			auto chunk = std::make_shared<DecodedWave>();
			if (DecodeHcaPrefix(bytes, 96 * 1024, *chunk))
			{
				const uint32_t chunkSamples = uint32_t(chunk->pcm.size() / chunk->channels);
				const bool loopSafe = !chunk->hasLoop || chunk->loopBeginSample >= chunkSamples;
				if (loopSafe && PrepareVoice(e, *p, chunk->channels, chunk->rate))
				{
					XAUDIO2_BUFFER head = {};
					head.AudioBytes = UINT32(chunk->pcm.size() * sizeof(int16_t));
					head.pAudioData = reinterpret_cast<const BYTE*>(chunk->pcm.data());
					// no END_OF_STREAM: the tail buffer follows from the worker
					if (SUCCEEDED(p->voice->SubmitSourceBuffer(&head)))
					{
						p->voice->SetVolume(p->volume);
						p->wave.reset();
						p->waveChunk = chunk;
						p->looping = chunk->hasLoop;
						p->pendingTail = true;
						if (!p->paused)
							p->voice->Start(0);
						p->status = 2;
						DebugLog("[cri] Start: %s (chunked head %u samples, ch=%u rate=%u)\n",
							job.label.c_str(), chunkSamples, chunk->channels, chunk->rate);
						job.chunkSamples = chunkSamples;
						job.bytes = std::move(bytes);
						QueueDecodeJob(e, std::move(job));
						return 1;
					}
				}
			}
		}

		// Fallback: fully asynchronous with the player in PREP until the wave lands.
		if (p->voice)
		{
			p->voice->Stop(0); // silence the previous sound while the new one decodes
			p->voice->FlushSourceBuffers();
		}
		p->status = 1; // PREP
		job.bytes = std::move(bytes);
		QueueDecodeJob(e, std::move(job));
		return 1; // playback id (opaque nonzero)
	}

	void PlayerStop(CriAtomExPlayerTag* handle)
	{
		if (!handle)
			return;
		Player* p = reinterpret_cast<Player*>(handle);
		std::lock_guard<std::recursive_mutex> g(E().lock);
		++p->gen; // cancels any in-flight async decode for this player
		if (p->voice)
		{
			p->voice->Stop(0);
			p->voice->FlushSourceBuffers();
		}
		p->paused = false;
		p->status = 0;
	}

	void PlayerPause(CriAtomExPlayerTag* handle, int pause)
	{
		if (!handle)
			return;
		Player* p = reinterpret_cast<Player*>(handle);
		std::lock_guard<std::recursive_mutex> g(E().lock);
		if (p->paused != (pause != 0))
			DebugLog("[cri] Pause: player %p -> %d\n", (void*)p, pause); // TEMP diagnostics
		if (p->voice && p->status == 2)
		{
			if (pause)
				p->voice->Stop(0);
			else
				p->voice->Start(0);
		}
		p->paused = pause != 0;
	}

	// criAtomExPlayer_Resume(player, mode). StF's pause path (FUN_180043020, driven by the host
	// pause status bit0) pauses every playing handle with Pause(p, 1) and resumes with
	// Resume(p, CRIATOMEX_RESUME_PAUSED_PLAYBACK) — this was an empty stub, so the first pause
	// muted the game permanently. Our engine models both the pause and the preload hold with the
	// single `paused` flag, so every resume mode reduces to releasing that hold.
	void PlayerResume(CriAtomExPlayerTag* handle, int /*mode*/)
	{
		PlayerPause(handle, 0);
	}

	int PlayerIsPaused(CriAtomExPlayerTag* handle)
	{
		if (!handle)
			return 0;
		Player* p = reinterpret_cast<Player*>(handle);
		std::lock_guard<std::recursive_mutex> g(E().lock);
		return p->paused ? 1 : 0;
	}

	int PlayerGetStatus(CriAtomExPlayerTag* handle)
	{
		if (!handle)
			return 4;
		Player* p = reinterpret_cast<Player*>(handle);
		std::lock_guard<std::recursive_mutex> g(E().lock);
		// pendingTail: a chunked start whose remainder hasn't been queued yet — the head
		// running dry must not read as PLAYEND.
		if (p->status == 2 && p->voice && !p->looping && !p->paused && !p->pendingTail)
		{
			XAUDIO2_VOICE_STATE state;
			p->voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
			if (state.BuffersQueued == 0)
				p->status = 3; // PLAYEND
		}
		return p->status;
	}

	void PlayerResetParameters(CriAtomExPlayerTag* handle)
	{
		if (!handle)
			return;
		Player* p = reinterpret_cast<Player*>(handle);
		std::lock_guard<std::recursive_mutex> g(E().lock);
		p->volume = 1.0f;
		if (p->voice)
			p->voice->SetVolume(1.0f);
	}

	void PlayerUpdateAll(CriAtomExPlayerTag* handle)
	{
		// Parameters are applied immediately in the setters; nothing deferred.
		(void)handle;
	}

	void ExecuteMain()
	{
		// Playback state is polled in PlayerGetStatus; no per-frame work needed.
	}
}
