#include "RenderWindow.h"

#include "m2ftg/DisplayModes.h"
#include "pxd/LJ/DeviceHooks.h"   // the D3D12 module-interop layer this window installs

#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"

#include "wil/win32_helpers.h"
#include "wil/resource.h"

#include <dxgi1_6.h>
#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "YAMPGeneral.h"
#include "DebugLog.h"
#include "input/Input.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

static constexpr DXGI_FORMAT OUTPUT_FORMAT = DXGI_FORMAT_B8G8R8A8_UNORM;


extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// The single RenderWindow instance, for WndProc (which has no user data pointer) to reach.
static RenderWindow* g_windowInstance = nullptr;

static LRESULT WINAPI WindowProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	LRESULT imguiResult = ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam);
	if (imguiResult != 0) return imguiResult;

	switch (Msg)
	{
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	case WM_SIZE:
		// Runs on the window thread; the render thread picks it up in BeginFrame.
		if (wParam != SIZE_MINIMIZED && g_windowInstance != nullptr)
		{
			g_windowInstance->RequestResize(LOWORD(lParam), HIWORD(lParam));
		}
		break;
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		// While an ImGui text field has focus the keystroke is text, not game input: typing a
		// server name into the netplay lobby must not also throw punches. Key-UP is still always
		// recorded, so a key held as a field takes focus cannot stick down.
		if (!ImGui::GetIO().WantTextInput)
		{
			gGeneral.SetKeyPressed(wParam, true);
		}
		break;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		gGeneral.SetKeyPressed(wParam, false);
		break;

	default:
		break;
	}
	return DefWindowProc(hWnd, Msg, wParam, lParam);
}

RenderWindow::RenderWindow(HINSTANCE instance, HINSTANCE dllInstance, int cmdShow)
{
	g_windowInstance = this;

	wil::unique_event startupEvent(wil::EventOptions::None);
	m_windowThread = std::thread([this, &startupEvent, instance, dllInstance, cmdShow] {

		WNDCLASSEX wndClass { sizeof(wndClass) };
		wndClass.hInstance = instance;
		wndClass.lpfnWndProc = WindowProc;
		wndClass.lpszClassName = L"YAKUZA_VF5FS";
		wndClass.hIcon = LoadIcon(dllInstance, MAKEINTRESOURCE(101));
		wndClass.hCursor = LoadCursor(nullptr, IDC_ARROW);

		const ATOM windowClass = RegisterClassEx(&wndClass);
		THROW_LAST_ERROR_IF(windowClass == 0);

		const YAMPSettings* settings = gGeneral.GetSettings();

		DWORD style;
		if (settings->m_fullscreen) // Borderless for now
		{
			style = WS_POPUP;

			RECT desktop;
			GetWindowRect(GetDesktopWindow(), &desktop);
			m_width = desktop.right - desktop.left;
			m_height = desktop.bottom - desktop.top;
		}
		else
		{
			style = WS_OVERLAPPEDWINDOW;
			m_width = settings->m_resX;
			m_height = settings->m_resY;

			// "Match window to render resolution": size the client area to what the module is
			// about to render at instead of the configured resolution, for a pixel-exact arcade
			// window. Resolved from the command line / setting rather than from the module,
			// because the window has to exist long before module_start runs the option parse -
			// ModuleArgs applies the same precedence, so the two agree.
			if (settings->m_m2WindowMatchesRender && gGeneral.IsModel2ArcadeGame())
			{
				// Sized to the full PICTURE, not to the raw framebuffer: a Model 2 board's 496x384
				// is displayed as 4:3 (non-square pixels), so a literal 496x384 window would
				// squash it. Taking the render height at the display aspect means the ordinary
				// aspect-correct viewport fills the whole client area - no letterboxing, and the
				// game blit itself stays exactly what it always was.
				const uint32_t mode = static_cast<uint32_t>(m2ftg::IntendedDisplayMode(settings->m_m2RenderMode));
				const uint32_t renderH = m2ftg::DisplayModeHeight(mode);
				m_height = renderH;
				m_width = static_cast<uint32_t>(renderH * (4.0f / 3.0f) + 0.5f);
			}
		}

        RECT clientArea { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
		AdjustWindowRect(&clientArea, style, FALSE);

		const std::wstring windowTitle = UTF8ToWchar(gGeneral.GetArcadeGameName());
		wil::unique_hwnd window(CreateWindowExW(0, L"YAKUZA_VF5FS", windowTitle.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
			clientArea.right - clientArea.left, clientArea.bottom - clientArea.top, nullptr, nullptr, instance, nullptr));
		THROW_LAST_ERROR_IF_NULL(window);

		ImGui_ImplWin32_Init(window.get());

		// --- D3D12 debug layer. MUST be enabled BEFORE device creation. It converts raw driver
		// faults (e.g. the null-store crash inside nvwgf2umx.dll from the m2ftg 2D ring draw) into
		// readable validation messages naming the exact bad parameter. GPU-based validation
		// additionally catches malformed vertex-buffer views / missing bindings at draw time.
		// Gated on the existing m_useD3DDebugLayer setting (already used for the 11-on-12 device).
		if (gGeneral.GetSettings()->m_useD3DDebugLayer)
		{
			wil::com_ptr<ID3D12Debug> debugController;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
			{
				debugController->EnableDebugLayer();
				// GPU-based validation catches draw-time resource/VBV errors but is very slow
				// (can time out the automated driver). Left off for the first pass; flip to TRUE
				// if the basic layer doesn't flag the bad call.
				wil::com_ptr<ID3D12Debug1> debugController1;
				if (SUCCEEDED(debugController->QueryInterface(IID_PPV_ARGS(debugController1.put()))))
				{
					// GBV is the one instrument never yet run to the scene: it names the exact
					// draw-time descriptor/resource fault the basic layer cannot see. It was tried
					// once and the layer died at ~frame 11 on the id=527 barrier desync, which is
					// FIXED now, so it should survive into StF's scene. Slow but decisive.
					debugController1->SetEnableGPUBasedValidation(FALSE); // GBV already gave us id=679; too slow for normal runs
				}
			}
		}

		// --- DRED: enable GPU auto-breadcrumbs + page-fault reporting BEFORE device creation, so a
		// device-removal reports the exact faulting GPU op + resource (independent of the debug layer).
		{
			wil::com_ptr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(dredSettings.put()))) && dredSettings)
			{
				dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				DebugLog("[DRED] enabled auto-breadcrumbs + page-fault reporting\n");
			}
		}

		// --- D3D12 device
		THROW_IF_FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_d3d12Device)));

		// The DRED breakpoint handler + the CreatePipelineState/CreateDescriptorHeap hooks -
		// the module-interop layer, now in pxd/LJ/DeviceHooks (its consumers always were).
		InstallDeviceHooks(m_d3d12Device.get());

		// --- Capture validation messages to a file. Since the x64dbg MCP can't read the debugger
		// log, we register an ID3D12InfoQueue1 callback that writes each message to disk immediately
		// (fflush per line) so even the message that precedes the driver crash is captured. Written
		// to "d3d12_debug.log" in the CWD (the exe dir). Also break on CORRUPTION (never benign).
		if (gGeneral.GetSettings()->m_useD3DDebugLayer)
		{
			wil::com_ptr<ID3D12InfoQueue1> infoQueue1;
			if (SUCCEEDED(m_d3d12Device->QueryInterface(IID_PPV_ARGS(infoQueue1.put()))))
			{
				DWORD cookie = 0;
				infoQueue1->RegisterMessageCallback(&D3D12DebugMessageCallback,
					D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
			}
			// NOTE: intentionally NOT calling SetBreakOnSeverity — a DebugBreak on the recurring
			// CORRUPTION messages halts the debuggee before it reaches the real driver fault. The
			// file callback above captures every message up to (and including) the crashing frame.
		}

		// --- Command queue
        {
		D3D12_COMMAND_QUEUE_DESC qdesc{};
		qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		THROW_IF_FAILED(m_d3d12Device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&m_cmdQueue)));

		// Dedicated list for the per-frame COMMON->RENDER_TARGET backbuffer barrier 11on12 omits.
		THROW_IF_FAILED(m_d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_bbBarrierAlloc)));
		THROW_IF_FAILED(m_d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_bbBarrierAlloc.get(), nullptr, IID_PPV_ARGS(&m_bbBarrierList)));
		THROW_IF_FAILED(m_bbBarrierList->Close());
        }

		// --- 11-on-12 device + context
        {
            // Keep the D3D11-on-12 debug flag OFF even when the D3D12 debug layer is on: the D3D11
            // validation halts on a benign PSSetShaderResources(null) in the ImGui/11-on-12 path
            // before the StF DX12 PSO is ever built, masking the D3D12 error we actually want.
            const UINT flags = 0;

		wil::com_ptr<ID3D11Device> device11;
		wil::com_ptr<ID3D11DeviceContext> context11;
		IUnknown* queues[] = { m_cmdQueue.get() };

		THROW_IF_FAILED(D3D11On12CreateDevice(
			m_d3d12Device.get(),
			flags,
			nullptr, 0,
			queues, _countof(queues),
			0,
			&device11, &context11, nullptr));

		m_device = std::move(device11);
		m_deviceContext = std::move(context11);
		m_d3d11on12 = m_device.query<ID3D11On12Device>();
        }

		// --- Create swap chain for the D3D12 queue
		{
		wil::com_ptr<IDXGIFactory6> factory;
		THROW_IF_FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));;

		DXGI_SWAP_CHAIN_DESC1 scd{};
		scd.Width = m_width;
		scd.Height = m_height;
		scd.Format = OUTPUT_FORMAT;
		scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		scd.BufferCount = kBufferCount;
		scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		scd.SampleDesc.Count = 1;

		wil::com_ptr<IDXGISwapChain1> sc1;
		THROW_IF_FAILED(factory->CreateSwapChainForHwnd(
			m_cmdQueue.get(), window.get(), &scd, nullptr, nullptr, &sc1));

		m_swapChain = std::move(sc1);
		// BeginFrame/EndFrame need GetCurrentBackBufferIndex every frame; resolve the interface once.
		m_swapChain3 = m_swapChain.query<IDXGISwapChain3>();
        }

        // --- ImGui init (DX11 backend)
		ImGui_ImplDX11_Init(m_device.get(), m_deviceContext.get());

		// --- Wrap backbuffers
		CreateWrappedBackbuffers();
		ShowWindow(window.get(), cmdShow);
		UpdateWindow(window.get());
		m_window = std::unique_ptr<std::remove_pointer_t<HWND>, hwnd_deleter>(window.release());
		// DirectInput needs a top-level window to set a cooperative level against; until it has
		// one, only XInput pads are visible (see source/input/DirectInputPad.cpp). This is the
		// window thread, but it runs before startupEvent below, which the thread that actually
		// polls waits on — so the handover is ordered and the input layer stays lock-free.
		Input::SetWindow(m_window.get());

		CreateRenderResources();
		EnumerateDisplayModes();
		CalculateViewport();

		m_ui.GetDefaultsFromSettings();
		startupEvent.SetEvent();

		BOOL ret;
		MSG msg;
        while ((ret = GetMessage(&msg, NULL, 0, 0)) != 0)
		{
			if (ret == -1)
			{
				// handle the error and possibly exit
			}
			else
			{
				TranslateMessage(&msg); 
				DispatchMessage(&msg); 
			}
		}

		m_shuttingDownWindow.store(true, std::memory_order_relaxed);

		// TODO: Pass the exit code back to WinMain via RenderWindow
		
	});
	startupEvent.wait();
}

RenderWindow::~RenderWindow()
{
	// The DirectInput devices hold a cooperative level against this window, so they have to go
	// before it does — and doing it here covers every host, not just the ones with a tidy exit.
	Input::ShutdownPads();
	PostMessage(m_window.get(), WM_CLOSE, 0, 0);
	m_windowThread.join();
}

// Lost Judgment's OWN CRT filter, ported verbatim. Ground truth: LJ applies the CRT look in a
// single host-side draw (PSO 9775 char-select / 9155 fight captures; PS hash
// c2860f630d5602cc47d194b29340d27f, DXIL SM6.4) sampling ONLY the resolved 1024x768 StF frame —
// no scanline texture, no cbuffer; the mask is computed analytically and is anchored to SOURCE
// texels: one scanline per Model 2 line (uv.y*384, quartic ramp, max 12.96% darkening), one
// grille stripe per source column (uv.x*512, ^8 ramp, max 1.68%), a Bayer 4x4 dither jittering
// the scanline phase (grid = 2048x1536), all 4x supersampled along the pixel derivative. LJ's
// quad also CROPS 16px off each side of the 1024-wide frame (UV x = 16/1024..1008/1024 — the
// active 992px Model 2 image). That crop is deliberately NOT reproduced: it only makes sense for
// LJ's own 1024-wide layout, and once the render resolution became selectable it started eating
// into the picture instead of the padding. Reverse-engineered 2026-07-26 from the PIX C++ exports
// (C:\temp\stf-lj-pix).
static const char LJ_CRT_PS_HLSL[] = R"(
Texture2D backBufferTexture : register(t0);
SamplerState backBufferSampler : register(s0);

// The fraction of the source texture the module's picture actually occupies. The module always
// renders into a 1024x768 target but lays its viewport out at whatever its own resolution option
// selected, so at "Model 2 native" the frame is the top-left 496x384 and this is (0.4844, 0.5).
// Every ramp below is anchored to the PICTURE (512x384 logical), so all of them work in
// normalised picture space and only the texture fetch is scaled back into the sub-rect.
// gSrcScale = (1,1) reproduces the original shader exactly.
cbuffer CrtParams : register(b0)
{
	float2 gSrcScale;    // (u,v) fraction of the texture the picture occupies
	float2 gCrtPad;
};

// uv is NORMALISED picture space - 0..1 across whatever the module rendered - which is what every
// ramp below is anchored to. Only the fetch scales back into the source sub-rect, so the scanline
// and grille frequencies stay one-per-source-line at any render size.
//
// LJ's own quad samples 16/1024..1008/1024, trimming the padding its 1024-wide layout leaves
// around the 992-wide image. That trim is NOT reproduced here: at any other render size the module
// fills from the left edge, so the same trim eats into the picture instead of the padding (it
// clipped the F of "FREE PLAY" at Model 2 native). Showing the whole frame is the intent.
float3 CrtTap(float2 uv)
{
	float3 c = backBufferTexture.Sample(backBufferSampler, uv * gSrcScale).rgb;

	// Bayer 4x4 phase dither, keyed on a 2048x1536 grid (4x the 512x384 logical grid)
	static const float kBayer[16] = {
		 0.0,  8.0,  2.0, 10.0,
		12.0,  4.0, 14.0,  6.0,
		 3.0, 11.0,  1.0,  9.0,
		15.0,  7.0, 13.0,  5.0,
	};
	int xi = (int)(frac(round(uv.x * 2048.0) * 0.25) * 4.0);
	int yi = (int)(frac(round(uv.y * 1536.0) * 0.25) * 4.0);
	float dither = kBayer[yi * 4 + xi] / 255.0;

	// Horizontal scanlines: one per source line, quartic ramp clamped at phase 0.6
	float py = frac(dither + uv.y * 384.0);
	float m4 = min(py, 0.6);
	m4 *= m4; m4 *= m4;
	float scan = 1.0 - m4;

	// Vertical aperture grille: one stripe per source column, ^8 ramp (much subtler)
	float px = frac(uv.x * 512.0);
	float m8 = min(px, 0.6);
	m8 *= m8; m8 *= m8; m8 *= m8;
	float grille = saturate(1.0 - m8);

	return c * (scan * grille);
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
	// The vertex UVs are pre-scaled to the module's sub-rect; undo that so the ramps see the
	// picture as 0..1 whatever the module rendered at.
	float2 uvc = uv / gSrcScale;
	float2 d = float2(ddx_coarse(uvc.x), ddy_coarse(uvc.y));
	float3 s = CrtTap(uvc) + CrtTap(uvc + 0.25 * d) + CrtTap(uvc + 0.5 * d) + CrtTap(uvc + 0.75 * d);
	return float4(s * 0.25, 1.0);
}
)";

bool RenderWindow::EnsureCrtResources()
{
	if (m_crtCompileFailed) return false;

	HRESULT hr = S_OK;
	wil::com_ptr<ID3DBlob> errors;
	if (!m_psCrt)
	{
		wil::com_ptr<ID3DBlob> code;
		hr = D3DCompile(LJ_CRT_PS_HLSL, sizeof(LJ_CRT_PS_HLSL) - 1, "lj_crt_ps", nullptr, nullptr,
			"main", "ps_5_0", 0, 0, code.put(), errors.put());
		if (SUCCEEDED(hr))
		{
			hr = m_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, m_psCrt.put());
		}
	}

	if (SUCCEEDED(hr))
	{
		// LJ sizes its CRT target to the WINDOW (menu captures ran 1776x956, the unpaused capture
		// happened to be a 1920x1080 window), recomputing the game quad per frame. Do the same,
		// but clamp the target up to >= 1080p tall: below that the 384 scanlines alias into
		// broken segments (LJ never runs that small, so this divergence has no LJ counterpart).
		const float upscale = m_height < 1080 ? 1080.0f / m_height : 1.0f;
		const UINT targetW = static_cast<UINT>(m_width * upscale + 0.5f);
		const UINT targetH = static_cast<UINT>(m_height * upscale + 0.5f);
		if (!m_crtTex || m_crtTexW != targetW || m_crtTexH != targetH)
		{
			m_crtSRV.reset();
			m_crtRTV.reset();
			m_crtTex.reset();

			D3D11_TEXTURE2D_DESC td {};
			td.Width = targetW;
			td.Height = targetH;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = OUTPUT_FORMAT;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			hr = m_device->CreateTexture2D(&td, nullptr, m_crtTex.put());
			if (SUCCEEDED(hr))
			{
				hr = m_device->CreateRenderTargetView(m_crtTex.get(), nullptr, m_crtRTV.put());
			}
			if (SUCCEEDED(hr))
			{
				hr = m_device->CreateShaderResourceView(m_crtTex.get(), nullptr, m_crtSRV.put());
			}
			if (SUCCEEDED(hr))
			{
				m_crtTexW = targetW;
				m_crtTexH = targetH;
			}
		}
	}

	if (FAILED(hr))
	{
		m_crtCompileFailed = true;
		m_psCrt.reset();
		m_crtTex.reset();
		m_crtRTV.reset();
		m_crtSRV.reset();
		DebugLog("[crt] shader setup failed hr=0x%08lX: %s\n", static_cast<unsigned long>(hr),
			errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no compiler output)");
		return false;
	}
	return true;
}

void RenderWindow::ClearBackbuffer()
{
	const FLOAT clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	ID3D11RenderTargetView* rtv = m_backBufferRTV.get();
	m_deviceContext->OMSetRenderTargets(1, &rtv, nullptr);
	m_deviceContext->ClearRenderTargetView(rtv, clearColor);
}

void RenderWindow::SetModuleSourceRect(uint32_t width, uint32_t height)
{
	// Clamped: a mode larger than the output texture is already clipped by the module itself, and
	// a UV scale above 1 would only sample past the edge (clamped) and squash the picture.
	const float u = width == 0 ? 1.0f
		: (std::min)(1.0f, static_cast<float>(width) / static_cast<float>(m2ftg::OUTPUT_TEXTURE_WIDTH));
	const float v = height == 0 ? 1.0f
		: (std::min)(1.0f, static_cast<float>(height) / static_cast<float>(m2ftg::OUTPUT_TEXTURE_HEIGHT));
	if (u == m_srcUScale && v == m_srcVScale)
	{
		return;
	}
	m_srcUScale = u;
	m_srcVScale = v;
	UpdateBlitVertices();
}

// The blit is a single oversized triangle whose UVs run 0..2; scaling them by the sub-rect makes
// the drawn area cover exactly the part of the texture the module rendered into.
void RenderWindow::UpdateBlitVertices()
{
	if (!m_vb || !m_deviceContext)
	{
		return;
	}
	const struct { DirectX::XMFLOAT2 Position; DirectX::XMFLOAT2 Texcoord0; } verts[] = {
		{ { -1.0f, -3.0f }, { 0.0f,                 2.0f * m_srcVScale } },
		{ { -1.0f,  1.0f }, { 0.0f,                 0.0f } },
		{ {  3.0f,  1.0f }, { 2.0f * m_srcUScale,   0.0f } },
	};
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (SUCCEEDED(m_deviceContext->Map(m_vb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
	{
		memcpy(mapped.pData, verts, sizeof(verts));
		m_deviceContext->Unmap(m_vb.get(), 0);
	}
}

void RenderWindow::BlitGameFrame(ID3D11ShaderResourceView* src, bool alphaBlend)
{
	const FLOAT clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

	// Live setting: route the frame through LJ's CRT pass first (never on the alpha-blended
	// overlay layer — LJ applies the CRT once, to the completed composite).
	// ...and only for the Model 2 boards: the CRT filter models an arcade cabinet's display, which
	// is meaningless for the VF5FS builds (modern widescreen 3D). The setting lives in the shared
	// [StF] section, so it has to be gated on the game rather than on the flag alone.
	const bool useCrt = !alphaBlend && gGeneral.IsModel2ArcadeGame()
		&& gGeneral.GetSettings()->m_m2CrtFilter && EnsureCrtResources();
	D3D11_VIEWPORT finalViewport = m_viewport;
	bool clearBackbuffer = m_requiresClear && !alphaBlend;
	if (useCrt)
	{
		// PASS 1 — replicate LJ: evaluate the CRT shader into the window-shaped intermediate
		// target, game quad = the aspect-corrected game rect (boxing baked into the frame as
		// black, from the clear). The target may be upscaled from the window (see
		// EnsureCrtResources), so scale the viewport with it.
		m_deviceContext->ClearRenderTargetView(m_crtRTV.get(), clearColor);
		m_deviceContext->OMSetRenderTargets(1, m_crtRTV.addressof(), nullptr);
		m_deviceContext->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
		m_deviceContext->VSSetShader(m_vs.get(), nullptr, 0);
		const UINT crtOffsets[1] = { 0 };
		m_deviceContext->IASetInputLayout(m_inputLayout.get());
		m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		m_deviceContext->IASetVertexBuffers(0, 1, m_vb.addressof(), &m_vbStride, crtOffsets);
		m_deviceContext->PSSetShader(m_psCrt.get(), nullptr, 0);
		m_deviceContext->PSSetShaderResources(0, 1, &src);
		m_deviceContext->PSSetSamplers(0, 1, m_blitSampler.addressof());
		if (m_crtCb)
		{
			D3D11_MAPPED_SUBRESOURCE cb{};
			if (SUCCEEDED(m_deviceContext->Map(m_crtCb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cb)))
			{
				const float params[4] = { m_srcUScale, m_srcVScale, 0.0f, 0.0f };
				memcpy(cb.pData, params, sizeof(params));
				m_deviceContext->Unmap(m_crtCb.get(), 0);
			}
			m_deviceContext->PSSetConstantBuffers(0, 1, m_crtCb.addressof());
		}

		const float upscale = static_cast<float>(m_crtTexH) / m_height;
		D3D11_VIEWPORT crtViewport = m_viewport;
		crtViewport.TopLeftX *= upscale;
		crtViewport.TopLeftY *= upscale;
		crtViewport.Width *= upscale;
		crtViewport.Height *= upscale;
		m_deviceContext->RSSetViewports(1, &crtViewport);
		m_deviceContext->RSSetState(nullptr);
		m_deviceContext->Draw(3, 0);

		ID3D11ShaderResourceView* nullSrv = nullptr;
		m_deviceContext->PSSetShaderResources(0, 1, &nullSrv);

		// PASS 2 shows the finished frame 1:1 over the whole window (boxing already baked in).
		src = m_crtSRV.get();
		finalViewport.TopLeftX = 0.0f;
		finalViewport.TopLeftY = 0.0f;
		finalViewport.Width = static_cast<float>(m_width);
		finalViewport.Height = static_cast<float>(m_height);
		finalViewport.MinDepth = 0.0f;
		finalViewport.MaxDepth = 1.0f;
		clearBackbuffer = false;
	}

	if (clearBackbuffer)
	{
		m_deviceContext->ClearRenderTargetView(m_backBufferRTV.get(), clearColor);
	}

	m_deviceContext->OMSetRenderTargets(1, m_backBufferRTV.addressof(), nullptr);
	if (alphaBlend)
	{
		// StF layer composite (mirrors the LJ host): the 496x384 2D output carries a proper alpha
		// mask (HUD pixels a=255, empty a=0) and must be blended OVER the 1024x768 3D layer.
		static wil::com_ptr<ID3D11BlendState> s_alphaBlend;
		if (!s_alphaBlend)
		{
			D3D11_BLEND_DESC bd{};
			bd.RenderTarget[0].BlendEnable = TRUE;
			bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			m_device->CreateBlendState(&bd, s_alphaBlend.put());
		}
		m_deviceContext->OMSetBlendState(s_alphaBlend.get(), nullptr, 0xFFFFFFFF);
	}
	else
		m_deviceContext->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

	m_deviceContext->VSSetShader(m_vs.get(), nullptr, 0);

	const UINT Offsets[1] = { 0 };
	m_deviceContext->IASetInputLayout(m_inputLayout.get());
	m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	// After the CRT pass the source is the full-size intermediate, not the module's output, so the
	// source sub-rect UVs must NOT be applied a second time - that would magnify a corner of the
	// finished frame over the whole window.
	m_deviceContext->IASetVertexBuffers(0, 1,
		useCrt ? m_vbFull.addressof() : m_vb.addressof(), &m_vbStride, Offsets);

	m_deviceContext->PSSetShader(m_ps.get(), nullptr, 0);
	m_deviceContext->PSSetShaderResources(0, 1, &src);
	// Bind an explicit linear-clamp sampler: the blit used to rely on whatever sampler the ImGui
	// backend left bound at s0 (same linear-clamp state, but by accident rather than by contract).
	// LJ's CRT pass uses the same filtering (MIN_MAG_MIP_LINEAR, clamp).
	m_deviceContext->PSSetSamplers(0, 1, m_blitSampler.addressof());

	m_deviceContext->RSSetViewports(1, &finalViewport);
	m_deviceContext->RSSetState(nullptr);

	m_deviceContext->Draw(3, 0);

	if (useCrt)
	{
		// Unbind the CRT frame SRV so next frame's pass-1 RTV bind doesn't hazard against it.
		ID3D11ShaderResourceView* nullSrv = nullptr;
		m_deviceContext->PSSetShaderResources(0, 1, &nullSrv);
	}
}

// The +0x98 pointer StF hands us can DANGLE (immediate resource release frees the object while the
// slot still points at it). QueryInterface both validates it IS a live ID3D12Resource AND AddRefs it,
// keeping it alive for the duration of the composite. Guarded so a truly-freed object just skips the
// frame instead of crashing. Returns a ref'd resource in *out (caller Releases) or false.
static bool SafeAcquireResource(ID3D12Resource* p, ID3D12Resource** out)
{
	*out = nullptr;
	if (!p) return false;
	__try
	{
		ID3D12Resource* r = nullptr;
		if (p->QueryInterface(__uuidof(ID3D12Resource), reinterpret_cast<void**>(&r)) == S_OK && r)
		{
			*out = r;
			return true;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return false;
}

// Composite a DX12-native texture (StF's output render target) onto the swapchain. StF renders in
// native D3D12, so its output has no D3D11 SRV; wrap the ID3D12Resource through 11on12 into a D3D11
// shader-resource, then reuse the D3D11 fullscreen-triangle blit. The wrap + SRV are cached by
// resource pointer (StF reuses the same RT across frames).
ID3D12Resource* GetModuleRenderTarget(int index);
int GetModuleRenderTargetCount();
int GetModuleRenderTargetState(int index);
ID3D12Resource* GetModuleResolveDst();

void RenderWindow::BlitDX12Texture(ID3D12Resource* texRaw)
{
	if (!m_d3d11on12) return;

	// (The old [rt-rb] every-600-frames readback diagnostic lived here. Removed 2026-07-26: the
	// black-screen investigation it served is long solved, and it crashed after window resizes —
	// it called GetDesc() on raw tracked-RT pointers, which include freed pre-resize backbuffers.)

	// *** TWO-LAYER HOST COMPOSITE (fight-capture diff, 2026-07-26) ***
	// The Model 2 3D scene (characters/arena) renders into the MSAA RT and is RESOLVED into the
	// 1024x768 RT — which StF's OWN 4-quad composite never consumes. In Lost Judgment the HOST
	// engine samples that resolved layer and composites it into the scene itself (fight capture:
	// LJ engine PSO 9155 samples the resolve dst; StF's 496x384 output is used as an alpha-masked
	// OVERLAY). Mirror that here: blit the 1024x768 3D layer first (opaque), then alpha-blend the
	// 496x384 2D output (HUD/tile layers; alpha mask verified: HUD a=255, empty a=0) over it.
	// PRIMARY SOURCE = texRaw (execute_info.output_texid -> cgs_tex -> sbgl_resource+0x98, resolved
	// by GameLoop): the module's own "completed frame" pointer — it does the double-buffer flip, so
	// it never points at the in-progress buffer. (Blitting "last resolve dst" instead flashed
	// mid-frame/intermediate passes during transitions.) The old "display_tex+0x98 is never rendered
	// to" note predates the render fixes and no longer holds. Fallbacks: the latest MSAA resolve dst,
	// then the 496x384 output (pre-first-resolve boot frames only).
	constexpr D3D12_RESOURCE_STATES kShaderRead =
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	ID3D12Resource* rt3d = texRaw ? texRaw : GetModuleResolveDst();
	D3D12_RESOURCE_STATES st3d = kShaderRead;
	D3D12_RESOURCE_STATES st3dTracked = kShaderRead;
	bool st3dFound = false;
	ID3D12Resource* rt2d = nullptr; D3D12_RESOURCE_STATES st2d = D3D12_RESOURCE_STATE_COMMON;
	const int nrt = GetModuleRenderTargetCount();
	for (int i = 0; i < nrt; ++i)
	{
		ID3D12Resource* raw = GetModuleRenderTarget(i);
		if (!raw) continue;
		// The tracked-RT list holds raw pointers and includes old swapchain backbuffers, which
		// DIE on window resize — validate + ref before touching (freed entries just get skipped).
		ID3D12Resource* r = nullptr;
		if (!SafeAcquireResource(raw, &r)) continue;
		const D3D12_RESOURCE_DESC d = r->GetDesc();
		r->Release();
		const D3D12_RESOURCE_STATES rs = static_cast<D3D12_RESOURCE_STATES>(GetModuleRenderTargetState(i));
		// Only RTs StF leaves SHADER-READABLE (0xC0) can get an SRV.
		const bool readable = (rs & kShaderRead) != 0;
		if (readable && d.Width == 496 && d.Height == 384) { rt2d = raw; st2d = rs; }
		// Remember the TRACKED state of the primary source. st3d is hardcoded to 0xC0 below because
		// StF's module leaves its RTs shader-readable; other modules need not. Handing
		// CreateWrappedResource a state the resource is not actually in means AcquireWrappedResources
		// emits the wrong barrier (or none), so the sample reads a resource still in RENDER_TARGET —
		// which renders and presents happily and shows BLACK.
		if (raw == texRaw) { st3dTracked = rs; st3dFound = true; }
	}
	// VF5FS only, so StF's verified-working path is bit-identical: trust the tracked state of the
	// primary source rather than assuming StF's 0xC0.
	if (gGeneral.GetGameId() == YAMPGeneral::GameId::VF5FS_LJ && st3dFound)
	{
		st3d = st3dTracked;
	}
	if (!rt3d && !rt2d) return; // nothing shader-readable yet — skip (don't composite the black display_tex)

	// Per-layer 11on12 wrap + SRV cache (two live layers, headroom for RT churn).
	struct LayerCache
	{
		ID3D12Resource* tex = nullptr;
		wil::com_ptr<ID3D11Resource> wrapped;
		wil::com_ptr<ID3D11ShaderResourceView> srv;
	};
	static LayerCache s_layerCache[4];

	auto blitLayer = [&](ID3D12Resource* srcRt, D3D12_RESOURCE_STATES srcState, bool alphaBlend)
	{
		ID3D12Resource* tex = nullptr;
		if (!SafeAcquireResource(srcRt, &tex)) { DebugLogFile("[blit] skip: src not a live resource\n"); return; }
		struct RelGuard { ID3D12Resource* r; ~RelGuard() { if (r) r->Release(); } } relGuard{ tex };

		LayerCache* c = nullptr;
		for (auto& e : s_layerCache) if (e.tex == tex) { c = &e; break; }
		if (!c)
		{
			for (auto& e : s_layerCache) if (!e.tex) { c = &e; break; }
			if (!c) { c = &s_layerCache[0]; c->srv.reset(); c->wrapped.reset(); }
			c->tex = tex;
			D3D11_RESOURCE_FLAGS flags{};
			flags.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			HRESULT hrw = m_d3d11on12->CreateWrappedResource(
				tex, &flags, srcState, srcState, IID_PPV_ARGS(c->wrapped.put()));
			DebugLogFile("[blit] CreateWrappedResource hr=0x%08lX wrapped=%p tex=%p\n",
				static_cast<unsigned long>(hrw), static_cast<void*>(c->wrapped.get()), static_cast<void*>(tex));
		}
		if (!c->wrapped) { c->tex = nullptr; return; }

		ID3D11Resource* wr[] = { c->wrapped.get() };
		m_d3d11on12->AcquireWrappedResources(wr, 1);
		if (!c->srv)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
			sd.Format = tex->GetDesc().Format;
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MostDetailedMip = 0;
			sd.Texture2D.MipLevels = 1;
			m_device->CreateShaderResourceView(c->wrapped.get(), &sd, c->srv.put());
		}
		if (c->srv) BlitGameFrame(c->srv.get(), alphaBlend);
		m_d3d11on12->ReleaseWrappedResources(wr, 1);
	};

	// LJ ground truth (both captures, fight AND char select): the host samples ONLY the resolved
	// 1024x768 layer — it already contains the COMPLETE frame (3D scene + HUD sprites); the 496x384
	// output is never displayed (0 SRVs reference it in either LJ capture). Blitting both doubles
	// the HUD. Show the resolve layer alone; fall back to the 496x384 only before the first resolve.
	if (rt3d)      blitLayer(rt3d, st3d, false);
	else if (rt2d) blitLayer(rt2d, st2d, false);

}

void RenderWindow::NewImGuiFrame()
{
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
}

void RenderWindow::RenderImGui()
{
	m_ui.Draw();

	ImGui::Render();

	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void RenderWindow::CreateRenderResources()
{
	ID3D11Device* device = m_device.get();

	// Create shaders
	static const uint8_t vs_main[] =
	{
		 68,  88,  66,  67,   0, 158, 
		 39, 114, 142,  75, 234,  29, 
		 89, 242, 219,   6,  14, 240, 
		 60,  96,   1,   0,   0,   0, 
		 56,   2,   0,   0,   5,   0, 
		  0,   0,  52,   0,   0,   0, 
		128,   0,   0,   0, 212,   0, 
		  0,   0,  44,   1,   0,   0, 
		188,   1,   0,   0,  82,  68, 
		 69,  70,  68,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		 28,   0,   0,   0,   0,   4, 
		254, 255,   0, 129,   0,   0, 
		 28,   0,   0,   0,  77, 105, 
		 99, 114, 111, 115, 111, 102, 
		116,  32,  40,  82,  41,  32, 
		 72,  76,  83,  76,  32,  83, 
		104,  97, 100, 101, 114,  32, 
		 67, 111, 109, 112, 105, 108, 
		101, 114,  32,  49,  48,  46, 
		 49,   0,  73,  83,  71,  78, 
		 76,   0,   0,   0,   2,   0, 
		  0,   0,   8,   0,   0,   0, 
		 56,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  3,   0,   0,   0,   0,   0, 
		  0,   0,   3,   3,   0,   0, 
		 65,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  3,   0,   0,   0,   1,   0, 
		  0,   0,   3,   3,   0,   0, 
		 80,  79,  83,  73,  84,  73, 
		 79,  78,   0,  84,  69,  88, 
		 67,  79,  79,  82,  68,   0, 
		171, 171,  79,  83,  71,  78, 
		 80,   0,   0,   0,   2,   0, 
		  0,   0,   8,   0,   0,   0, 
		 56,   0,   0,   0,   0,   0, 
		  0,   0,   1,   0,   0,   0, 
		  3,   0,   0,   0,   0,   0, 
		  0,   0,  15,   0,   0,   0, 
		 68,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  3,   0,   0,   0,   1,   0, 
		  0,   0,   3,  12,   0,   0, 
		 83,  86,  95,  80,  79,  83, 
		 73,  84,  73,  79,  78,   0, 
		 84,  69,  88,  67,  79,  79, 
		 82,  68,   0, 171, 171, 171, 
		 83,  72,  68,  82, 136,   0, 
		  0,   0,  64,   0,   1,   0, 
		 34,   0,   0,   0,  95,   0, 
		  0,   3,  50,  16,  16,   0, 
		  0,   0,   0,   0,  95,   0, 
		  0,   3,  50,  16,  16,   0, 
		  1,   0,   0,   0, 103,   0, 
		  0,   4, 242,  32,  16,   0, 
		  0,   0,   0,   0,   1,   0, 
		  0,   0, 101,   0,   0,   3, 
		 50,  32,  16,   0,   1,   0, 
		  0,   0,  54,   0,   0,   5, 
		 50,  32,  16,   0,   0,   0, 
		  0,   0,  70,  16,  16,   0, 
		  0,   0,   0,   0,  54,   0, 
		  0,   8, 194,  32,  16,   0, 
		  0,   0,   0,   0,   2,  64, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		128,  63,   0,   0, 128,  63, 
		 54,   0,   0,   5,  50,  32, 
		 16,   0,   1,   0,   0,   0, 
		 70,  16,  16,   0,   1,   0, 
		  0,   0,  62,   0,   0,   1, 
		 83,  84,  65,  84, 116,   0, 
		  0,   0,   4,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   4,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  1,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  3,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0
	};
	HRESULT hr = device->CreateVertexShader(vs_main, sizeof(vs_main), nullptr, m_vs.addressof());
	THROW_IF_FAILED(hr);

	const D3D11_INPUT_ELEMENT_DESC inputElements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	hr = device->CreateInputLayout(inputElements, std::size(inputElements), vs_main, sizeof(vs_main), m_inputLayout.addressof());
	THROW_IF_FAILED(hr);

	static const uint8_t ps_main[] =
	{
		 68,  88,  66,  67, 106, 118, 
		 18, 107, 193, 156, 216,  42, 
		  8, 105, 148, 229, 179, 147, 
		188, 143,   1,   0,   0,   0, 
		 88,   2,   0,   0,   5,   0, 
		  0,   0,  52,   0,   0,   0, 
		228,   0,   0,   0,  60,   1, 
		  0,   0, 112,   1,   0,   0, 
		220,   1,   0,   0,  82,  68, 
		 69,  70, 168,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   2,   0,   0,   0, 
		 28,   0,   0,   0,   0,   4, 
		255, 255,   0, 129,   0,   0, 
		128,   0,   0,   0,  92,   0, 
		  0,   0,   3,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   1,   0, 
		  0,   0,   1,   0,   0,   0, 
		110,   0,   0,   0,   2,   0, 
		  0,   0,   5,   0,   0,   0, 
		  4,   0,   0,   0, 255, 255, 
		255, 255,   0,   0,   0,   0, 
		  1,   0,   0,   0,  13,   0, 
		  0,   0,  98,  97,  99, 107, 
		 66, 117, 102, 102, 101, 114, 
		 83,  97, 109, 112, 108, 101, 
		114,   0,  98,  97,  99, 107, 
		 66, 117, 102, 102, 101, 114, 
		 84, 101, 120, 116, 117, 114, 
		101,   0,  77, 105,  99, 114, 
		111, 115, 111, 102, 116,  32, 
		 40,  82,  41,  32,  72,  76, 
		 83,  76,  32,  83, 104,  97, 
		100, 101, 114,  32,  67, 111, 
		109, 112, 105, 108, 101, 114, 
		 32,  49,  48,  46,  49,   0, 
		 73,  83,  71,  78,  80,   0, 
		  0,   0,   2,   0,   0,   0, 
		  8,   0,   0,   0,  56,   0, 
		  0,   0,   0,   0,   0,   0, 
		  1,   0,   0,   0,   3,   0, 
		  0,   0,   0,   0,   0,   0, 
		 15,   0,   0,   0,  68,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   3,   0, 
		  0,   0,   1,   0,   0,   0, 
		  3,   3,   0,   0,  83,  86, 
		 95,  80,  79,  83,  73,  84, 
		 73,  79,  78,   0,  84,  69, 
		 88,  67,  79,  79,  82,  68, 
		  0, 171, 171, 171,  79,  83, 
		 71,  78,  44,   0,   0,   0, 
		  1,   0,   0,   0,   8,   0, 
		  0,   0,  32,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   3,   0,   0,   0, 
		  0,   0,   0,   0,  15,   0, 
		  0,   0,  83,  86,  95,  84, 
		 65,  82,  71,  69,  84,   0, 
		171, 171,  83,  72,  68,  82, 
		100,   0,   0,   0,  64,   0, 
		  0,   0,  25,   0,   0,   0, 
		 90,   0,   0,   3,   0,  96, 
		 16,   0,   0,   0,   0,   0, 
		 88,  24,   0,   4,   0, 112, 
		 16,   0,   0,   0,   0,   0, 
		 85,  85,   0,   0,  98,  16, 
		  0,   3,  50,  16,  16,   0, 
		  1,   0,   0,   0, 101,   0, 
		  0,   3, 242,  32,  16,   0, 
		  0,   0,   0,   0,  69,   0, 
		  0,   9, 242,  32,  16,   0, 
		  0,   0,   0,   0,  70,  16, 
		 16,   0,   1,   0,   0,   0, 
		 70, 126,  16,   0,   0,   0, 
		  0,   0,   0,  96,  16,   0, 
		  0,   0,   0,   0,  62,   0, 
		  0,   1,  83,  84,  65,  84, 
		116,   0,   0,   0,   2,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   2,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   1,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  1,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0
	};
	hr = device->CreatePixelShader(ps_main, sizeof(ps_main), nullptr, m_ps.addressof());
	THROW_IF_FAILED(hr);

	// Create a vertex buffer
	{
		using namespace DirectX;

		static const struct
		{
			XMFLOAT2 Position;
			XMFLOAT2 Texcoord0;
		} vertexBuffer[] = {
			{ XMFLOAT2(-1.0f, -3.0f), XMFLOAT2(0.0f, 2.0f) },
			{ XMFLOAT2(-1.0f, 1.0f), XMFLOAT2(0.0f, 0.0f) },
			{ XMFLOAT2(3.0f, 1.0f), XMFLOAT2(2.0f, 0.0f) },
		};
		// DYNAMIC rather than IMMUTABLE: the UVs carry the module source sub-rect, which changes
		// when the module's own render-resolution option does (see SetModuleSourceRect).
		D3D11_BUFFER_DESC desc;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.ByteWidth = sizeof(vertexBuffer);
		desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		desc.MiscFlags = 0;

		const D3D11_SUBRESOURCE_DATA initData { vertexBuffer };
		hr = device->CreateBuffer(&desc, &initData, m_vb.addressof());
		THROW_IF_FAILED(hr);

		// The unscaled companion, for passes that sample a full-size texture rather than the
		// module's output.
		D3D11_BUFFER_DESC fullDesc = desc;
		fullDesc.Usage = D3D11_USAGE_IMMUTABLE;
		fullDesc.CPUAccessFlags = 0;
		hr = device->CreateBuffer(&fullDesc, &initData, m_vbFull.addressof());
		THROW_IF_FAILED(hr);

		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.ByteWidth = 16;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		hr = device->CreateBuffer(&cbDesc, nullptr, m_crtCb.addressof());
		THROW_IF_FAILED(hr);

		m_vbStride = sizeof(vertexBuffer[0]);
		UpdateBlitVertices();
	}

	// Linear-clamp sampler for the game blit (see the PSSetSamplers note in BlitGameFrame)
	{
		D3D11_SAMPLER_DESC desc {};
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
		desc.MaxLOD = D3D11_FLOAT32_MAX;
		hr = device->CreateSamplerState(&desc, m_blitSampler.addressof());
		THROW_IF_FAILED(hr);
	}
}

void RenderWindow::EnumerateDisplayModes()
{
	wil::com_ptr<IDXGIDevice> dxgiDevice;
	HRESULT hr = m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
	wil::com_ptr<IDXGIAdapter> adapter;
	hr = dxgiDevice->GetAdapter(adapter.addressof());
	THROW_IF_FAILED(hr);

	wil::com_ptr<IDXGIOutput> output;
	hr = adapter->EnumOutputs(0, output.addressof());
	THROW_IF_FAILED(hr);

	UINT numModes = 0;
	hr = output->GetDisplayModeList(OUTPUT_FORMAT, 0, &numModes, nullptr);
	THROW_IF_FAILED(hr);

	auto displayModes = std::make_unique<DXGI_MODE_DESC[]>(numModes);
	output->GetDisplayModeList(OUTPUT_FORMAT, 0, &numModes, displayModes.get());

	std::vector<std::tuple<uint32_t, uint32_t, float>> displayModesVector;
	for (auto* mode = displayModes.get(); mode < displayModes.get() + numModes; ++mode)
	{
		if (mode->ScanlineOrdering != DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE || mode->Scaling != DXGI_MODE_SCALING_UNSPECIFIED) continue;

		// We don't want resolutions smaller than 800x600, as the settings window is 600x600
		if (mode->Width < 800 || mode->Height < 600) continue;

		displayModesVector.push_back({mode->Width, mode->Height, static_cast<float>(mode->RefreshRate.Numerator) / mode->RefreshRate.Denominator});
	}

	// Refresh rates seem to be unsorted, fix it
	std::sort(displayModesVector.begin(), displayModesVector.end());
	for (const auto& mode : displayModesVector)
	{
		m_ui.AddResolution(std::get<0>(mode), std::get<1>(mode), std::get<2>(mode));
	}
}

void RenderWindow::CalculateViewport()
{
	if (m_fillViewport)
	{
		// "Fill Window": stretch the game over the whole backbuffer, no boxing.
		m_viewport.TopLeftX = 0;
		m_viewport.TopLeftY = 0;
		m_viewport.Width = static_cast<float>(m_width);
		m_viewport.Height = static_cast<float>(m_height);
		m_viewport.MinDepth = 0.0f;
		m_viewport.MaxDepth = 1.0f;
		m_requiresClear = false;
		return;
	}

	const float gameAR = m_gameAspect; // per-game (16:9 default; StF sets 4:3 via SetGameAspectRatio)
	const float windowAR = static_cast<float>(m_width) / m_height;
	if (gameAR >= windowAR)
	{
		// Letterbox - or nothing, if window is precisely 16:9
		const UINT targetHeight = static_cast<UINT>(m_width / gameAR);

		m_viewport.TopLeftX = 0;
		m_viewport.TopLeftY = (m_height - targetHeight) / 2;
		m_viewport.Width = m_width;
		m_viewport.Height = targetHeight;
	}
	else
	{
		// Pillarbox
		const UINT targetWidth = static_cast<UINT>(m_height * gameAR);

		m_viewport.TopLeftX = (m_width - targetWidth) / 2;
		m_viewport.TopLeftY = 0;
		m_viewport.Width = targetWidth;
		m_viewport.Height = m_height;
	}

	m_viewport.MinDepth = 0.0f;
	m_viewport.MaxDepth = 1.0f;

	m_requiresClear = m_viewport.TopLeftX != 0 || m_viewport.TopLeftY != 0;
}

// Defined in HostCdevice.cpp: the module's barrier corrector must never rewrite transitions on the
// swapchain backbuffers (those are driven by 11on12 + TransitionBackbufferToRenderTarget).
void RegisterWatchResourceNow(ID3D12Resource* r);

void RenderWindow::CreateWrappedBackbuffers()
{
	// clean previous
	for (UINT i = 0; i < kBufferCount; ++i) {
		m_backBufferRTVs[i].reset();
		m_wrappedBackbuffers[i].reset();
		m_backbuffers[i].reset();
	}

	// fetch 12 backbuffers, wrap for 11, and build the RTV each frame will select
	for (UINT i = 0; i < kBufferCount; ++i) {
		THROW_IF_FAILED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backbuffers[i])));

		D3D11_RESOURCE_FLAGS flags{};
		flags.BindFlags = D3D11_BIND_RENDER_TARGET;

		THROW_IF_FAILED(m_d3d11on12->CreateWrappedResource(
			m_backbuffers[i].get(),
			&flags,
			D3D12_RESOURCE_STATE_RENDER_TARGET,   // before DX11 uses it
			D3D12_RESOURCE_STATE_PRESENT,         // after DX11 work
			IID_PPV_ARGS(&m_wrappedBackbuffers[i])));

		THROW_IF_FAILED(m_device->CreateRenderTargetView(
			m_wrappedBackbuffers[i].get(), nullptr, m_backBufferRTVs[i].put()));

		// Exclude them from the module barrier corrector (see RegisterWatchResourceNow above).
		RegisterWatchResourceNow(m_backbuffers[i].get());
	}

	m_backBufferRTV = m_backBufferRTVs[0]; // boot value; BeginFrame selects the live one
}

void RenderWindow::ResizeOn12(UINT w, UINT h)
{
	// ResizeBuffers fails on ANY outstanding backbuffer reference: drop the RTVs + the 11on12
	// wrappers + our ID3D12Resource refs, flush whatever the D3D11 context still holds deferred,
	// and wait for the D3D12 queue to finish with the old buffers before resizing.
	m_backBufferRTV.reset();
	for (UINT i = 0; i < kBufferCount; ++i) {
		m_backBufferRTVs[i].reset();
		m_wrappedBackbuffers[i].reset();
		m_backbuffers[i].reset();
	}
	m_deviceContext->ClearState();
	m_deviceContext->Flush();

	if (!m_resizeFence)
	{
		THROW_IF_FAILED(m_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_resizeFence.put())));
	}
	const uint64_t fenceValue = ++m_resizeFenceValue;
	THROW_IF_FAILED(m_cmdQueue->Signal(m_resizeFence.get(), fenceValue));
	if (m_resizeFence->GetCompletedValue() < fenceValue)
	{
		wil::unique_event gpuIdle(wil::EventOptions::None);
		THROW_IF_FAILED(m_resizeFence->SetEventOnCompletion(fenceValue, gpuIdle.get()));
		gpuIdle.wait();
	}

	// The closed m_bbBarrierList still holds a reference to the backbuffer it last transitioned;
	// reset it (safe now — the GPU is idle) so ResizeBuffers sees no outstanding refs.
	THROW_IF_FAILED(m_bbBarrierAlloc->Reset());
	THROW_IF_FAILED(m_bbBarrierList->Reset(m_bbBarrierAlloc.get(), nullptr));
	THROW_IF_FAILED(m_bbBarrierList->Close());

	const HRESULT hr = m_swapChain->ResizeBuffers(kBufferCount, w, h, OUTPUT_FORMAT, 0);
	DebugLogFile("[resize] ResizeBuffers %ux%u hr=0x%08lX\n", w, h, static_cast<unsigned long>(hr));
	THROW_IF_FAILED(hr);
	CreateWrappedBackbuffers();
}

// Apply a window resize posted by WndProc: resize the swapchain to the new client size, rewrap
// the backbuffers for 11on12, and recompute the aspect-corrected game viewport. Runs on the
// render thread (top of BeginFrame), between frames, so no wrapped resource is acquired.
void RenderWindow::ApplyPendingResize()
{
	const uint64_t packed = m_pendingResize.exchange(0, std::memory_order_acquire);
	if (packed == 0) return;

	const UINT w = static_cast<UINT>(packed >> 32);
	const UINT h = static_cast<UINT>(packed & 0xFFFFFFFF);
	if (w == 0 || h == 0 || (w == m_width && h == m_height)) return;

	m_width = w;
	m_height = h;
	ResizeOn12(w, h);
	CalculateViewport();
}

// Issue the COMMON->RENDER_TARGET barrier d3d11on12 omits on Acquire, on m_cmdQueue. The queue serializes
// this ahead of 11on12's later flush (the composite/ImGui draws), so the backbuffer is RENDER_TARGET when
// they run. StF's Present leaves it back in PRESENT(==COMMON) for the next frame, so before is always 0x0.
void RenderWindow::TransitionBackbufferToRenderTarget(UINT idx)
{
	THROW_IF_FAILED(m_bbBarrierAlloc->Reset());
	THROW_IF_FAILED(m_bbBarrierList->Reset(m_bbBarrierAlloc.get(), nullptr));
	D3D12_RESOURCE_BARRIER b{};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource   = m_backbuffers[idx].get();
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
	m_bbBarrierList->ResourceBarrier(1, &b);
	THROW_IF_FAILED(m_bbBarrierList->Close());
	ID3D12CommandList* lists[] = { m_bbBarrierList.get() };
	m_cmdQueue->ExecuteCommandLists(1, lists);
}

void RenderWindow::BeginFrame()
{
	// Between frames and before any wrapped-resource Acquire: safe point to follow a window resize.
	ApplyPendingResize();

	if (!m_swapChain3) return;
	const UINT idx = m_swapChain3->GetCurrentBackBufferIndex();

	// Put the backbuffer into RENDER_TARGET BEFORE handing it to 11on12 (which won't do it itself).
	TransitionBackbufferToRenderTarget(idx);
	ID3D11Resource* const res[] = { m_wrappedBackbuffers[idx].get() };
	m_d3d11on12->AcquireWrappedResources(res, 1);
	m_backBufferRTV = m_backBufferRTVs[idx];
}

void RenderWindow::EndFrame()
{
	if (!m_swapChain3) return;
	const UINT idx = m_swapChain3->GetCurrentBackBufferIndex();

	ID3D11Resource* const res[] = { m_wrappedBackbuffers[idx].get() };

	// Pair with AcquireWrappedResources from BeginFrame
	m_d3d11on12->ReleaseWrappedResources(res, 1);

	// Hand the recorded D3D11 work to the D3D12 queue before the caller Presents.
	m_deviceContext->Flush();
}
