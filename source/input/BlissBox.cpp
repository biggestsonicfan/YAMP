#include "BlissBox.h"

#include "../DebugLog.h"
#include "../StringUtil.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <SetupAPI.h>

// hidsdi.h/hidpi.h are C headers and older Windows SDKs ship them without linkage guards, so the
// declarations would mangle as C++ and fail to link against hid.lib. Wrapping is harmless where
// the SDK already guards them (nested extern "C" is legal).
extern "C"
{
#include <hidsdi.h>
#include <hidpi.h>
}

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace Input::BlissBox
{
	namespace
	{
		constexpr uint16_t VENDOR_ID = 0x16D0;
		constexpr uint16_t PID_FIRST = 0x0D04;   // port 1; each further port increments by one

		// Get-command report IDs (the API document's "Report ID" column).
		constexpr uint8_t REPORT_ADAPTER_INFO = 0x11;
		constexpr uint8_t REPORT_PAYLOAD = 0x10;

		// Feature buffers are sized from the device's own HIDP_CAPS, but a device that refuses to
		// report caps still has to be talked to, and 64 covers every documented Get command (the
		// largest, EEPROM, is 49 bytes).
		constexpr size_t FEATURE_FALLBACK_BYTES = 64;

		// "A control transfer can and will fail from time to time ... simply try again. Make sure
		// to build in a time out" — the vendor's own instruction. Two extra attempts is enough to
		// ride out a normal V-USB collision without turning a genuinely dead device into a stall.
		constexpr int TRANSFER_ATTEMPTS = 3;

		// Poll cadence. The adapter's guaranteed interrupt poll is 16 ms and the control-transfer
		// path is documented as usually faster; asking every 4 ms samples it about as often as it
		// can answer without spinning the CPU. The adapter INFO command is a different matter — it
		// only changes when someone swaps a controller, so it runs on a slow cadence and stays off
		// the hot path.
		constexpr int POLL_INTERVAL_MS = 4;
		constexpr int INFO_INTERVAL_MS = 250;
		// A rescan walks the whole HID stack via SetupAPI (~100 ms, same cost DirectInputPad pays
		// for EnumDevices), so it is never automatic on a timer — only at startup, on request, and
		// when a port stops answering.
		constexpr int RESCAN_COOLDOWN_MS = 1000;

		// Digital thresholds on the 0..255 axis bytes. A Saturn d-pad decoded to analog by the
		// adapter lands on 0x00 / 0x80 / 0xFF, so anything near the ends is a press; the wide dead
		// band means a genuine analog Saturn 3D pad has to be pushed properly to register, rather
		// than a resting stick pinning a lever.
		constexpr uint8_t AXIS_LOW = 0x40;
		constexpr uint8_t AXIS_HIGH = 0xC0;

		// HID button numbers for a Saturn controller, from the vendor's global mapping sheet.
		// 1-based, matching the sheet's HID1..HID24 columns.
		constexpr int HID_SATURN_A = 1;
		constexpr int HID_SATURN_B = 2;
		constexpr int HID_SATURN_X = 3;
		constexpr int HID_SATURN_Y = 4;
		constexpr int HID_SATURN_START = 6;
		constexpr int HID_SATURN_Z = 7;
		constexpr int HID_SATURN_C = 8;
		constexpr int HID_SATURN_L = 9;
		constexpr int HID_SATURN_R = 10;

		struct OpenPort
		{
			HANDLE handle = INVALID_HANDLE_VALUE;
			int pid = 0;
			std::wstring path;
			size_t featureBytes = FEATURE_FALLBACK_BYTES;
			// Where the DOCUMENT'S byte 0 actually lives in the returned buffer. Measured per
			// device at open time rather than assumed - see ProbeDataOffset.
			size_t dataOffset = 0;
			uint64_t updates = 0;
			uint64_t errors = 0;
			int consecutiveErrors = 0;
			int msSinceInfo = INFO_INTERVAL_MS;   // force an info read on the first pass
		};

		// "-blissbox-dump": raw report bytes to the log file, for settling a byte layout against
		// hardware rather than against a document.
		bool DumpWanted()
		{
			static const bool wanted = wcsstr(GetCommandLineW(), L"-blissbox-dump") != nullptr;
			return wanted;
		}

		std::mutex s_lock;                          // guards s_published only
		PortState s_published[MAX_PORTS];

		// Previous payload per port, for the -blissbox-dump diff. Worker-thread only.
		Payload s_publishedPayloadForDump[MAX_PORTS];

		std::thread s_thread;
		std::atomic<bool> s_running{ false };
		std::atomic<bool> s_rescanWanted{ true };

		// ---------------------------------------------------------------------------------
		// HID plumbing
		// ---------------------------------------------------------------------------------

		// Raw feature read. `raw` receives the buffer exactly as the device returned it, so callers
		// that need to reason about the framing itself can.
		bool ReadFeatureRaw(HANDLE handle, size_t featureBytes, uint8_t reportId,
			std::vector<uint8_t>& raw)
		{
			raw.assign(featureBytes, 0);
			for (int attempt = 0; attempt < TRANSFER_ATTEMPTS; attempt++)
			{
				raw[0] = reportId;
				if (HidD_GetFeature(handle, raw.data(), static_cast<ULONG>(raw.size())) != FALSE)
				{
					return true;
				}
			}
			return false;
		}

		// WHERE THE DOCUMENT'S BYTE 0 ACTUALLY IS, decided per device instead of assumed.
		//
		// A numbered HID feature report is supposed to echo its report ID back in byte 0, putting
		// the payload at byte 1 - RetroArch's blissbox_feature_report_index says as much, and
		// allows for transports that hand back the body alone. The adapter on this bench does the
		// latter: assuming the prefix shifted every field by one byte, which put the left lever's
		// two axes where the code was reading button rows, so half the stick silently did nothing.
		//
		// The tell is the adapter-info report's PLAYER ID field, which the vendor documents as 4..7
		// ("4 = player 1"). Exactly one of the two framings puts a value in that range there, so
		// the device is simply asked. Falling back to the report-ID test only when neither
		// candidate validates, because an unconfigured adapter may genuinely have no player number.
		size_t ProbeDataOffset(HANDLE handle, size_t featureBytes)
		{
			std::vector<uint8_t> raw;
			if (!ReadFeatureRaw(handle, featureBytes, REPORT_ADAPTER_INFO, raw) || raw.size() < 6)
			{
				return 0;
			}
			auto validPlayer = [](uint8_t value) { return value >= 4 && value <= 7; };
			const bool unprefixedOk = validPlayer(raw[3]);   // data[3] at buffer[3]
			const bool prefixedOk = validPlayer(raw[4]);     // data[3] at buffer[4]
			size_t offset;
			if (unprefixedOk != prefixedOk)
			{
				offset = unprefixedOk ? 0 : 1;
			}
			else
			{
				offset = raw[0] == REPORT_ADAPTER_INFO ? 1 : 0;
			}
			DebugLogFile("[blissbox] framing probe: raw %02X %02X %02X %02X %02X %02X"
				" -> data starts at byte %zu\n",
				raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], offset);
			return offset;
		}

		bool ReadFeature(const OpenPort& port, uint8_t reportId, uint8_t* out, size_t outBytes)
		{
			std::vector<uint8_t> raw;
			if (!ReadFeatureRaw(port.handle, port.featureBytes, reportId, raw))
			{
				return false;
			}
			if (raw.size() <= port.dataOffset)
			{
				return false;
			}
			const size_t available = raw.size() - port.dataOffset;
			memcpy(out, raw.data() + port.dataOffset,
				outBytes < available ? outBytes : available);
			return true;
		}

		void ClosePort(OpenPort& port)
		{
			if (port.handle != INVALID_HANDLE_VALUE)
			{
				CloseHandle(port.handle);
				port.handle = INVALID_HANDLE_VALUE;
			}
		}

		// Walks the HID class for Bliss-Box ports and opens each one. Everything about this is
		// SetupAPI boilerplate except the VID/PID filter and the caps read.
		void Enumerate(std::vector<OpenPort>& ports)
		{
			for (OpenPort& port : ports)
			{
				ClosePort(port);
			}
			ports.clear();

			GUID hidGuid = {};
			HidD_GetHidGuid(&hidGuid);

			HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
				DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
			if (set == INVALID_HANDLE_VALUE)
			{
				return;
			}

			SP_DEVICE_INTERFACE_DATA iface = {};
			iface.cbSize = sizeof(iface);
			for (DWORD index = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, index, &iface);
				index++)
			{
				DWORD needed = 0;
				SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &needed, nullptr);
				if (needed == 0)
				{
					continue;
				}
				std::vector<uint8_t> storage(needed);
				auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
				detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
				if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, needed, nullptr, nullptr))
				{
					continue;
				}

				// SHARE_READ|SHARE_WRITE so this never fights whatever else has the pad open —
				// the adapter's ports also enumerate to DirectInput, and a player may well have
				// one bound there for another game.
				HANDLE handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
					FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
				if (handle == INVALID_HANDLE_VALUE)
				{
					continue;
				}

				HIDD_ATTRIBUTES attributes = {};
				attributes.Size = sizeof(attributes);
				const bool isBlissBox = HidD_GetAttributes(handle, &attributes) != FALSE
					&& attributes.VendorID == VENDOR_ID
					&& attributes.ProductID >= PID_FIRST
					&& attributes.ProductID < PID_FIRST + MAX_PORTS;
				if (!isBlissBox)
				{
					CloseHandle(handle);
					continue;
				}

				OpenPort port;
				port.handle = handle;
				port.pid = attributes.ProductID;
				port.path = detail->DevicePath;

				// The real feature-report length, so a short buffer never truncates a reply.
				PHIDP_PREPARSED_DATA preparsed = nullptr;
				if (HidD_GetPreparsedData(handle, &preparsed) != FALSE && preparsed != nullptr)
				{
					HIDP_CAPS caps = {};
					if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS
						&& caps.FeatureReportByteLength > 1)
					{
						port.featureBytes = caps.FeatureReportByteLength;
					}
					HidD_FreePreparsedData(preparsed);
				}

				port.dataOffset = ProbeDataOffset(port.handle, port.featureBytes);
				DebugLogFile("[blissbox] port PID 0x%04X opened, feature report %zu bytes,"
					" data offset %zu\n", port.pid, port.featureBytes, port.dataOffset);
				ports.push_back(std::move(port));
			}
			SetupDiDestroyDeviceInfoList(set);
		}

		// ---------------------------------------------------------------------------------
		// Decode
		// ---------------------------------------------------------------------------------

		bool HidButton(const Payload& payload, int oneBasedButton)
		{
			const int zero = oneBasedButton - 1;
			const int row = zero / 8;
			if (row < 0 || row >= 3)
			{
				return false;
			}
			return (payload.buttons[row] & (1u << (zero % 8))) != 0;
		}

		// The Saturn d-pad, which is the LEFT LEVER.
		//
		// THE AXES, NOT THE HAT, and that is a measured choice rather than a preference. The vendor's
		// mapping sheet says the adapter "will send d-pad only controllers (i.e snes) u,d,l,r buttons
		// to analog by default", so a Saturn pad's directions arrive on the main X/Y axes unless
		// somebody turns on the Analog-to-D-pad mode (modes bit 5).
		//
		// The HAT byte is carried in the payload and shown on the settings page, but deliberately not
		// decoded: the adapter on this bench (firmware 4.0, modes 0x04 — Analog-to-D-pad OFF) reports
		// hat 0x00 at rest, and 0x00 is UP in the ordinary 0-based HID hat encoding, where rest is
		// 0x08 or 0x0F. So either this firmware numbers its hat from 1, or it leaves the field at zero
		// when unused — and under the first reading a driver that trusted it would hold the left lever
		// full forward forever. One unconfirmed convention is not worth a permanently stuck lever when
		// the axes carry the same four switches and are unambiguous. Run YAMP with -blissbox-dump if
		// the hat ever needs settling.
		void DecodeDirections(const Payload& payload, TwinStickState& out)
		{
			// Plain and conventional, now that the framing is right: X horizontal, Y vertical,
			// low = left/up. This LOOKED wrong on the bench for a while - physical up came out as
			// the board's left - but the cause was the report framing above shifting every field
			// by a byte, not the axes. Fixing the framing put the lever's vertical travel back on
			// Y where it belongs, so there is nothing to transpose here.
			if (payload.x <= AXIS_LOW) out.leftLeft = true;
			if (payload.x >= AXIS_HIGH) out.leftRight = true;
			if (payload.y <= AXIS_LOW) out.leftUp = true;
			if (payload.y >= AXIS_HIGH) out.leftDown = true;
		}

		TwinStickState DecodeTwinStick(const Payload& payload)
		{
			TwinStickState out;
			DecodeDirections(payload, out);

			// The wiki's correspondence table, one line each. The right lever is the four face
			// buttons; the two weapon triggers and two dash thumb-buttons are the shoulders and
			// the remaining faces.
			out.rightUp = HidButton(payload, HID_SATURN_Y);
			out.rightDown = HidButton(payload, HID_SATURN_B);
			out.rightLeft = HidButton(payload, HID_SATURN_X);
			out.rightRight = HidButton(payload, HID_SATURN_Z);
			out.leftTrigger = HidButton(payload, HID_SATURN_L);
			out.leftThumb = HidButton(payload, HID_SATURN_R);
			out.rightTrigger = HidButton(payload, HID_SATURN_A);
			out.rightThumb = HidButton(payload, HID_SATURN_C);
			out.start = HidButton(payload, HID_SATURN_START);
			return out;
		}

		// ---------------------------------------------------------------------------------
		// The worker
		// ---------------------------------------------------------------------------------

		void PublishEmpty(int slot)
		{
			std::lock_guard<std::mutex> guard(s_lock);
			s_published[slot] = PortState{};
		}

		void PollOnce(OpenPort& port, int slot)
		{
			const bool dumpWanted = DumpWanted();
			PortState state;
			state.present = true;
			state.pid = port.pid;
			state.id = "blissbox:" + std::to_string(port.pid);

			// Adapter info on its own slow cadence — it answers "what is plugged in", which only
			// changes when a human changes it.
			{
				std::lock_guard<std::mutex> guard(s_lock);
				state.controllerType = s_published[slot].controllerType;
				state.modes = s_published[slot].modes;
				state.firmwareMajor = s_published[slot].firmwareMajor;
				state.firmwareMinor = s_published[slot].firmwareMinor;
				state.playerId = s_published[slot].playerId;
				state.updates = s_published[slot].updates;
				state.errors = s_published[slot].errors;
			}

			port.msSinceInfo += POLL_INTERVAL_MS;
			if (port.msSinceInfo >= INFO_INTERVAL_MS)
			{
				port.msSinceInfo = 0;
				uint8_t info[8] = {};
				if (ReadFeature(port, REPORT_ADAPTER_INFO, info, sizeof(info)))
				{
					// The raw head alongside the decode. The decode is only as good as the byte
					// numbering it assumes, and when a controller comes back as a type nobody's
					// table lists, the bytes themselves are the only thing worth arguing from.
					if (dumpWanted && info[0] != state.controllerType)
					{
						DebugLogFile("[blissbox] port PID 0x%04X info raw:"
							" %02X %02X %02X %02X %02X %02X %02X %02X\n", port.pid,
							info[0], info[1], info[2], info[3],
							info[4], info[5], info[6], info[7]);
					}
					// Logged on CHANGE, which is once at startup and once per controller swap —
					// this is the line that says whether the adapter can see the stick at all, and
					// it is the first thing worth knowing when a lever does nothing.
					if (info[0] != state.controllerType)
					{
						DebugLogFile("[blissbox] port PID 0x%04X: controller %u (%s), firmware %u.%u,"
							" modes 0x%02X\n",
							port.pid, static_cast<unsigned>(info[0]), ControllerTypeName(info[0]),
							static_cast<unsigned>(info[2]), static_cast<unsigned>(info[4]),
							static_cast<unsigned>(info[1]));
					}
					state.controllerType = info[0];
					state.modes = info[1];
					state.firmwareMajor = info[2];
					// "Byte 3: player ID (subtract 3 from result)" — the device numbers players
					// 4..7, so this lands on 1..4. Clamped rather than trusted: a firmware that
					// has never been given a player number returns 0, and 0 - 3 is not a port.
					state.playerId = info[3] >= 4 && info[3] <= 7 ? info[3] - 3 : 0;
					state.firmwareMinor = info[4];
				}
			}

			uint8_t raw[13] = {};
			if (ReadFeature(port, REPORT_PAYLOAD, raw, sizeof(raw)))
			{
				Payload payload;
				payload.playerId = raw[0];
				payload.buttons[0] = raw[1];
				payload.buttons[1] = raw[2];
				payload.buttons[2] = raw[3];
				payload.x = raw[4];
				payload.y = raw[5];
				payload.z = raw[6];
				payload.x2 = raw[7];
				payload.y2 = raw[8];
				payload.z2 = raw[9];
				payload.slider = raw[10];
				payload.dial = raw[11];
				payload.hat = raw[12];

				// -blissbox-dump prints every payload whose buttons or axes CHANGED. The whole
				// point is to confirm, against a stick somebody is holding, the two things this
				// driver takes from documentation: which HID button number each Saturn button
				// occupies, and whether the d-pad arrives on the axes or the hat.
				if (dumpWanted)
				{
					const Payload& was = s_publishedPayloadForDump[slot];
					if (memcmp(was.buttons, payload.buttons, sizeof(payload.buttons)) != 0
						|| was.x != payload.x || was.y != payload.y || was.hat != payload.hat)
					{
						DebugLogFile("[blissbox] port %d: player %02X rows %02X %02X %02X"
							"  x %02X y %02X z %02X  x2 %02X y2 %02X  hat %02X\n",
							slot + 1, payload.playerId,
							payload.buttons[0], payload.buttons[1], payload.buttons[2],
							payload.x, payload.y, payload.z, payload.x2, payload.y2, payload.hat);
					}
					s_publishedPayloadForDump[slot] = payload;
				}

				state.payload = payload;
				state.stick = DecodeTwinStick(payload);
				state.updates = port.updates = port.updates + 1;
				port.consecutiveErrors = 0;
			}
			else
			{
				state.errors = port.errors = port.errors + 1;
				port.consecutiveErrors++;
				// A port that has stopped answering entirely is unplugged, not merely unlucky:
				// V-USB drops the odd transfer, it does not drop fifty in a row. Ask for a rescan
				// so the handle is reopened (or dropped) rather than retried forever.
				if (port.consecutiveErrors > 50)
				{
					s_rescanWanted.store(true, std::memory_order_relaxed);
				}
				// Hold the last good payload: a dropped control transfer must not read as the
				// player letting go of every lever for one frame.
				std::lock_guard<std::mutex> guard(s_lock);
				state.payload = s_published[slot].payload;
				state.stick = s_published[slot].stick;
			}

			state.name = "Bliss-Box port " + std::to_string(port.pid - PID_FIRST + 1)
				+ " - " + ControllerTypeName(state.controllerType);

			std::lock_guard<std::mutex> guard(s_lock);
			s_published[slot] = std::move(state);
		}

		void WorkerMain()
		{
			std::vector<OpenPort> ports;
			DWORD lastRescan = 0;

			while (s_running.load(std::memory_order_relaxed))
			{
				const DWORD now = GetTickCount();
				// The cooldown applies even with nothing attached: a SetupAPI walk is ~100 ms of
				// the whole HID stack, and "no adapter yet" is the case that would otherwise spin
				// it forever on a machine that simply does not own one.
				if (s_rescanWanted.load(std::memory_order_relaxed)
					&& now - lastRescan >= RESCAN_COOLDOWN_MS)
				{
					s_rescanWanted.store(false, std::memory_order_relaxed);
					lastRescan = now;
					Enumerate(ports);
					for (int slot = 0; slot < MAX_PORTS; slot++)
					{
						PublishEmpty(slot);
					}
				}

				if (ports.empty())
				{
					// Nothing attached. Idle cheaply — a scan every second is plenty for a device
					// somebody has to walk over and plug in.
					std::this_thread::sleep_for(std::chrono::milliseconds(250));
					s_rescanWanted.store(true, std::memory_order_relaxed);
					continue;
				}

				for (size_t i = 0; i < ports.size() && i < MAX_PORTS; i++)
				{
					PollOnce(ports[i], static_cast<int>(i));
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
			}

			for (OpenPort& port : ports)
			{
				ClosePort(port);
			}
		}
	}

	const char* ControllerTypeName(uint8_t type)
	{
		switch (type)
		{
		case TYPE_NO_DETECTION:   return "none";
		case TYPE_SATURN_DIGITAL: return "Saturn (digital)";
		case TYPE_SATURN_ANALOG:  return "Saturn 3D pad";
		case TYPE_SATURN_GUN:     return "Saturn gun";
		case TYPE_SEARCHING:      return "searching...";
		default: break;
		}
		// Named types are the ones YAMP reasons about; anything else is reported honestly by
		// number rather than guessed at from the vendor's 80-entry enum.
		static thread_local char buffer[24];
		snprintf(buffer, sizeof(buffer), "type %u", static_cast<unsigned>(type));
		return buffer;
	}

	void Start()
	{
		if (s_running.exchange(true))
		{
			return;
		}
		s_rescanWanted.store(true, std::memory_order_relaxed);
		s_thread = std::thread(WorkerMain);
	}

	void Shutdown()
	{
		if (!s_running.exchange(false))
		{
			return;
		}
		if (s_thread.joinable())
		{
			s_thread.join();
		}
		std::lock_guard<std::mutex> guard(s_lock);
		for (PortState& state : s_published)
		{
			state = PortState{};
		}
	}

	void RequestRescan()
	{
		s_rescanWanted.store(true, std::memory_order_relaxed);
	}

	bool GetPort(int port, PortState& out)
	{
		if (port < 0 || port >= MAX_PORTS)
		{
			return false;
		}
		std::lock_guard<std::mutex> guard(s_lock);
		if (!s_published[port].present)
		{
			return false;
		}
		out = s_published[port];
		return true;
	}

	int PortsOpen()
	{
		std::lock_guard<std::mutex> guard(s_lock);
		int count = 0;
		for (const PortState& state : s_published)
		{
			count += state.present ? 1 : 0;
		}
		return count;
	}

	int FindTwinStick(int skipPort)
	{
		std::lock_guard<std::mutex> guard(s_lock);
		for (int port = 0; port < MAX_PORTS; port++)
		{
			if (port != skipPort && s_published[port].IsTwinStickCapable())
			{
				return port;
			}
		}
		return -1;
	}
}
