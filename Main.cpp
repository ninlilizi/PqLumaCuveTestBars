#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>

// ---------------------------------------------------------------------------
// Embedded HLSL shaders
// ---------------------------------------------------------------------------

static const char* g_vsSource = R"HLSL(
struct VsOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VsOut main(uint vid : SV_VertexID)
{
    // Fullscreen triangle from vertex ID (0,1,2)
    VsOut o;
    o.uv  = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)HLSL";

static const char* g_psSource = R"HLSL(
cbuffer TestParams : register(b0)
{
    float startNits;
    float endNits;
    float2 viewportSize;
    int   numBars;
    int   outputMode;   // 0 = PQ direct, 1 = scRGB linear
    float labelNits;
    float pad;
};

// ---- 3x5 bitmap font for digits 0-9 ----
// Each glyph packed into 15 bits: row0[14:12] row1[11:9] row2[8:6] row3[5:3] row4[2:0]
// Within each 3-bit row: bit2=left, bit1=center, bit0=right.
static const uint kDigits[10] = {
    31599u, // 0
    11415u, // 1
    29671u, // 2
    29647u, // 3
    23497u, // 4
    31183u, // 5
    31215u, // 6
    29257u, // 7
    31727u, // 8
    31695u  // 9
};

static const int FONT_SCALE = 4;
static const int SEP_PX     = 2;

bool SampleGlyph(uint digit, int2 pos)
{
    if ((uint)pos.x >= 3u || (uint)pos.y >= 5u) return false;
    uint bitIdx = (4u - (uint)pos.y) * 3u + (2u - (uint)pos.x);
    return (kDigits[digit] >> bitIdx) & 1u;
}

// Renders "X.XXXXX" for a nits value (up to 5 decimal places).
// Returns true if screenPos falls on a lit font pixel.
bool SampleValue(float nits, int2 screenPos, int2 origin)
{
    int2 lp = screenPos - origin;
    int cellW = (3 + 1) * FONT_SCALE; // glyph width + 1 col spacing, scaled
    int cellH = 5 * FONT_SCALE;

    if (lp.y < 0 || lp.y >= cellH || lp.x < 0) return false;

    // Total character slots: we render up to "NNNNN.NNNNN" = 11 chars
    // But for small nits we use "0.XXXXX" = 7 chars. For larger values we need more.
    // Flexible: figure out integer part digits, always 5 decimal digits.
    int intPart = (int)nits;
    int fracVal = (int)round((nits - (float)intPart) * 100000.0);

    // Count integer digits (at least 1)
    int intDigits = 0;
    {
        int tmp = max(intPart, 0);
        if (tmp == 0) { intDigits = 1; }
        else { while (tmp > 0) { intDigits++; tmp /= 10; } }
    }

    // Total chars = intDigits + 1(dot) + 5(frac)
    int totalChars = intDigits + 1 + 5;

    int charIdx = lp.x / cellW;
    if (charIdx >= totalChars) return false;

    int fx = (lp.x % cellW) / FONT_SCALE;
    int fy = lp.y / FONT_SCALE;
    if (fx >= 3) return false; // spacing gap

    int2 fp = int2(fx, fy);

    // Determine which digit this character slot maps to
    if (charIdx < intDigits)
    {
        // Integer part digit
        int divisor = 1;
        for (int i = 0; i < (intDigits - 1 - charIdx); i++) divisor *= 10;
        int digit = (intPart / divisor) % 10;
        return SampleGlyph((uint)clamp(digit, 0, 9), fp);
    }
    else if (charIdx == intDigits)
    {
        // Decimal point: single pixel at bottom-center
        return (fp.x == 1 && fp.y == 4);
    }
    else
    {
        // Fractional digits
        int fracIdx = charIdx - intDigits - 1; // 0..4
        int divisor = 1;
        for (int i = 0; i < (4 - fracIdx); i++) divisor *= 10;
        int digit = (fracVal / divisor) % 10;
        return SampleGlyph((uint)clamp(digit, 0, 9), fp);
    }
}

// ST.2084 PQ forward curve: linear [0,1] -> PQ [0,1]
// Input Y is normalized luminance (nits / 10000)
float3 ApplyPQ(float3 Y)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;

    float3 Ym1 = pow(max(Y, 0.0), m1);
    float3 num = c1 + c2 * Ym1;
    float3 den = 1.0 + c3 * Ym1;
    return pow(num / den, m2);
}

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 main(VsOut input) : SV_Target
{
    float2 screenCoord = input.pos.xy;

    float barH = viewportSize.y / (float)numBars;

    int barIdx = clamp((int)(screenCoord.y / barH), 0, numBars - 1);
    float posInBar = fmod(screenCoord.y, barH);
    bool isSep = (posInBar < (float)SEP_PX) || (posInBar >= barH - (float)SEP_PX);

    // Linearly interpolate luminance across bars
    float t = (numBars > 1) ? ((float)barIdx / (float)(numBars - 1)) : 0.0;
    float barNits = lerp(startNits, endNits, t);

    // Label rendering
    int cellH = 5 * FONT_SCALE;
    int labelY = (int)(barIdx * barH + (barH - (float)cellH) * 0.5);
    int totalChars = 12; // generous
    int labelW = totalChars * (3 + 1) * FONT_SCALE + 20;
    bool inLabel = (int)screenCoord.x < labelW;
    bool isText = SampleValue(barNits, (int2)screenCoord.xy, int2(10, labelY));

    float3 barColor, labelColor;

    if (outputMode == 0)
    {
        // HDR10 PQ direct: PQ-encode
        barColor   = ApplyPQ((barNits / 10000.0).xxx);
        labelColor = ApplyPQ((labelNits / 10000.0).xxx);
    }
    else
    {
        // FP16 scRGB: linear nits / 80
        barColor   = (barNits / 80.0).xxx;
        labelColor = (labelNits / 80.0).xxx;
    }

    // Compositing: separator > text > label bg > bar
    float3 result = barColor;
    result = inLabel ? (float3)0.0 : result;
    result = isText  ? labelColor  : result;
    result = isSep   ? (float3)0.0 : result;

    return float4(result, 1.0);
}
)HLSL";


// ---------------------------------------------------------------------------
// App constants
// ---------------------------------------------------------------------------

static const int   TOOLBAR_HEIGHT     = 40;
static const float DEFAULT_START_NITS = 0.005f;
static const float DEFAULT_END_NITS   = 0.00248f;
static const int   DEFAULT_NUM_BARS   = 20;
static const float DEFAULT_LABEL_NITS = 5.0f;

enum OutputMode
{
    MODE_HDR10_PQ  = 0,
    MODE_FP16_SCRGB = 1
};

// ---------------------------------------------------------------------------
// Constant buffer matching HLSL
// ---------------------------------------------------------------------------

struct alignas(16) TestParamsCB
{
    float startNits;
    float endNits;
    float viewportW;
    float viewportH;
    int   numBars;
    int   outputMode;
    float labelNits;
    float pad;
};

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

static HWND                  g_hWnd           = nullptr;
static HWND                  g_hRenderWnd     = nullptr;
static HWND                  g_hToolbar       = nullptr;
static HWND                  g_hEditStart     = nullptr;
static HWND                  g_hEditEnd       = nullptr;
static HWND                  g_hEditBars      = nullptr;
static HWND                  g_hComboMode     = nullptr;
static HFONT                 g_hFont          = nullptr;

static ID3D11Device*         g_device         = nullptr;
static ID3D11Device1*        g_device1        = nullptr;
static ID3D11DeviceContext*  g_context        = nullptr;
static IDXGISwapChain1*      g_swapChain      = nullptr;
static IDXGISwapChain3*      g_swapChain3     = nullptr;
static ID3D11RenderTargetView* g_rtv          = nullptr;
static ID3D11VertexShader*   g_vs             = nullptr;
static ID3D11PixelShader*    g_ps             = nullptr;
static ID3D11Buffer*         g_cbuffer        = nullptr;
static IDXGIFactory2*        g_factory        = nullptr;

static float    g_startNits   = DEFAULT_START_NITS;
static float    g_endNits     = DEFAULT_END_NITS;
static int      g_numBars     = DEFAULT_NUM_BARS;
static float    g_labelNits   = DEFAULT_LABEL_NITS;
static OutputMode g_mode      = MODE_HDR10_PQ;

static bool     g_fullscreen     = false;
static RECT     g_savedWindowRect = {};
static LONG     g_savedStyle      = 0;
static LONG     g_savedExStyle    = 0;

static bool     g_needsResize    = false;
static bool     g_initialized    = false;

// ---------------------------------------------------------------------------
// Control IDs
// ---------------------------------------------------------------------------

#define IDC_EDIT_START  101
#define IDC_EDIT_END    102
#define IDC_EDIT_BARS   103
#define IDC_COMBO_MODE  104
#define IDC_LABEL_START 105
#define IDC_LABEL_END   106
#define IDC_LABEL_BARS  107
#define IDC_LABEL_MODE  108

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void ReleaseRTV();
static bool CreateSwapChainForMode(OutputMode mode);
static bool InitD3D();
static void Render();
static void ToggleFullscreen();
static void ParseControls();
static void ResizeSwapChain();
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

template<typename T>
static void SafeRelease(T*& ptr)
{
    if (ptr) { ptr->Release(); ptr = nullptr; }
}

static void ReleaseRTV()
{
    if (g_context) g_context->OMSetRenderTargets(0, nullptr, nullptr);
    SafeRelease(g_rtv);
}

static bool CreateRTV()
{
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr)) return false;
    hr = g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
    backBuffer->Release();
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
// CheckHDRSupport
// ---------------------------------------------------------------------------

static bool CheckHDRSupport()
{
    IDXGIAdapter1* adapter = nullptr;
    g_factory->EnumAdapters1(0, &adapter);
    if (!adapter) return false;

    IDXGIOutput* output = nullptr;
    adapter->EnumOutputs(0, &output);
    adapter->Release();
    if (!output) return false;

    IDXGIOutput6* output6 = nullptr;
    HRESULT hr = output->QueryInterface(__uuidof(IDXGIOutput6), (void**)&output6);
    output->Release();
    if (FAILED(hr) || !output6) return false;

    DXGI_OUTPUT_DESC1 desc;
    hr = output6->GetDesc1(&desc);
    output6->Release();
    if (FAILED(hr)) return false;

    return desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
}

// ---------------------------------------------------------------------------
// CreateSwapChainForMode
// ---------------------------------------------------------------------------

static bool CreateSwapChainForMode(OutputMode mode)
{
    ReleaseRTV();

    // If swap chain already exists, release it
    SafeRelease(g_swapChain3);
    SafeRelease(g_swapChain);

    RECT rc;
    GetClientRect(g_hRenderWnd, &rc);
    UINT width  = max((UINT)(rc.right - rc.left), 1u);
    UINT height = max((UINT)(rc.bottom - rc.top), 1u);

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width       = width;
    sd.Height      = height;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

    if (mode == MODE_HDR10_PQ)
    {
        sd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    else
    {
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }

    HRESULT hr = g_factory->CreateSwapChainForHwnd(
        g_device, g_hRenderWnd, &sd, nullptr, nullptr, &g_swapChain);
    if (FAILED(hr)) return false;

    hr = g_swapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&g_swapChain3);
    if (FAILED(hr)) return false;

    // Set color space
    if (mode == MODE_HDR10_PQ)
    {
        g_swapChain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
    }
    else
    {
        g_swapChain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    }

    g_mode = mode;
    return CreateRTV();
}

// ---------------------------------------------------------------------------
// InitD3D
// ---------------------------------------------------------------------------

static bool InitD3D()
{
    // Create DXGI factory
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&g_factory);
    if (FAILED(hr)) return false;

    // Disable Alt+Enter fullscreen toggle (we handle it ourselves)
    g_factory->MakeWindowAssociation(g_hWnd, DXGI_MWA_NO_ALT_ENTER);

    // Create device
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
        &featureLevel, 1, D3D11_SDK_VERSION,
        &g_device, nullptr, &g_context);
    if (FAILED(hr)) return false;

    hr = g_device->QueryInterface(__uuidof(ID3D11Device1), (void**)&g_device1);
    if (FAILED(hr)) return false;

    // Compile vertex shader
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* errBlob = nullptr;
    hr = D3DCompile(g_vsSource, strlen(g_vsSource), "VS", nullptr, nullptr,
        "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob)
        {
            MessageBoxA(g_hWnd, (char*)errBlob->GetBufferPointer(), "VS Compile Error", MB_OK);
            errBlob->Release();
        }
        return false;
    }
    hr = g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs);
    vsBlob->Release();
    if (errBlob) errBlob->Release();
    if (FAILED(hr)) return false;

    // Compile pixel shader
    ID3DBlob* psBlob = nullptr;
    errBlob = nullptr;
    hr = D3DCompile(g_psSource, strlen(g_psSource), "PS", nullptr, nullptr,
        "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob)
        {
            MessageBoxA(g_hWnd, (char*)errBlob->GetBufferPointer(), "PS Compile Error", MB_OK);
            errBlob->Release();
        }
        return false;
    }
    hr = g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps);
    psBlob->Release();
    if (errBlob) errBlob->Release();
    if (FAILED(hr)) return false;

    // Create constant buffer
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = sizeof(TestParamsCB);
    cbd.Usage           = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags       = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags  = D3D11_CPU_ACCESS_WRITE;
    hr = g_device->CreateBuffer(&cbd, nullptr, &g_cbuffer);
    if (FAILED(hr)) return false;

    // Create swap chain for initial mode
    if (!CreateSwapChainForMode(g_mode)) return false;

    // Check HDR
    if (!CheckHDRSupport())
    {
        MessageBoxW(g_hWnd,
            L"HDR does not appear to be enabled on the primary display.\n"
            L"The test pattern will render, but colors will not be correct.\n"
            L"Enable HDR in Windows Display Settings for accurate results.",
            L"HDR Not Detected", MB_ICONWARNING | MB_OK);
    }

    return true;
}

// ---------------------------------------------------------------------------
// ResizeSwapChain
// ---------------------------------------------------------------------------

static void ResizeSwapChain()
{
    if (!g_swapChain) return;

    ReleaseRTV();

    RECT rc;
    GetClientRect(g_hRenderWnd, &rc);
    UINT width  = max((UINT)(rc.right - rc.left), 1u);
    UINT height = max((UINT)(rc.bottom - rc.top), 1u);

    g_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    CreateRTV();
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

static void Render()
{
    if (!g_context || !g_rtv) return;

    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(g_context->Map(g_cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        RECT rc;
        GetClientRect(g_hRenderWnd, &rc);
        float vpW = max((float)(rc.right - rc.left), 1.0f);
        float vpH = max((float)(rc.bottom - rc.top), 1.0f);

        TestParamsCB cb;
        cb.startNits  = g_startNits;
        cb.endNits    = g_endNits;
        cb.viewportW  = vpW;
        cb.viewportH  = vpH;
        cb.numBars    = g_numBars;
        cb.outputMode = (int)g_mode;
        cb.labelNits  = g_labelNits;
        cb.pad        = 0.0f;

        memcpy(mapped.pData, &cb, sizeof(cb));
        g_context->Unmap(g_cbuffer, 0);
    }

    // Set pipeline
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);

    RECT rc;
    GetClientRect(g_hRenderWnd, &rc);
    D3D11_VIEWPORT vp = {};
    vp.Width    = max((float)(rc.right - rc.left), 1.0f);
    vp.Height   = max((float)(rc.bottom - rc.top), 1.0f);
    vp.MaxDepth = 1.0f;
    g_context->RSSetViewports(1, &vp);

    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    g_context->ClearRenderTargetView(g_rtv, clearColor);

    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_vs, nullptr, 0);
    g_context->PSSetShader(g_ps, nullptr, 0);
    g_context->PSSetConstantBuffers(0, 1, &g_cbuffer);

    g_context->Draw(3, 0);

    g_swapChain->Present(1, 0);
}

// ---------------------------------------------------------------------------
// ToggleFullscreen
// ---------------------------------------------------------------------------

static void ToggleFullscreen()
{
    g_fullscreen = !g_fullscreen;

    if (g_fullscreen)
    {
        // Save window state
        g_savedStyle   = GetWindowLong(g_hWnd, GWL_STYLE);
        g_savedExStyle = GetWindowLong(g_hWnd, GWL_EXSTYLE);
        GetWindowRect(g_hWnd, &g_savedWindowRect);

        // Hide toolbar
        ShowWindow(g_hToolbar, SW_HIDE);

        // Go borderless fullscreen
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(MonitorFromWindow(g_hWnd, MONITOR_DEFAULTTOPRIMARY), &mi);

        int monW = mi.rcMonitor.right - mi.rcMonitor.left;
        int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

        SetWindowLong(g_hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLong(g_hWnd, GWL_EXSTYLE, 0);
        SetWindowPos(g_hWnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top, monW, monH,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER);

        // Render child fills entire window in fullscreen
        SetWindowPos(g_hRenderWnd, nullptr, 0, 0, monW, monH, SWP_NOZORDER);
    }
    else
    {
        // Restore window
        SetWindowLong(g_hWnd, GWL_STYLE, g_savedStyle);
        SetWindowLong(g_hWnd, GWL_EXSTYLE, g_savedExStyle);
        SetWindowPos(g_hWnd, nullptr,
            g_savedWindowRect.left, g_savedWindowRect.top,
            g_savedWindowRect.right - g_savedWindowRect.left,
            g_savedWindowRect.bottom - g_savedWindowRect.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOZORDER);

        // Show toolbar, reposition render child below it
        ShowWindow(g_hToolbar, SW_SHOW);
        RECT rc;
        GetClientRect(g_hWnd, &rc);
        int w = rc.right - rc.left;
        int rh = max((int)(rc.bottom - rc.top) - TOOLBAR_HEIGHT, 1);
        SetWindowPos(g_hRenderWnd, nullptr, 0, TOOLBAR_HEIGHT, w, rh, SWP_NOZORDER);
    }

    ResizeSwapChain();
}

// ---------------------------------------------------------------------------
// ParseControls
// ---------------------------------------------------------------------------

static void ParseControls()
{
    wchar_t buf[64];

    GetWindowTextW(g_hEditStart, buf, 64);
    double v = wcstod(buf, nullptr);
    g_startNits = (float)max(0.0, min(v, 10000.0));

    GetWindowTextW(g_hEditEnd, buf, 64);
    v = wcstod(buf, nullptr);
    g_endNits = (float)max(0.0, min(v, 10000.0));

    GetWindowTextW(g_hEditBars, buf, 64);
    int bars = _wtoi(buf);
    g_numBars = max(2, min(bars, 100));

    // Check combo box
    int sel = (int)SendMessageW(g_hComboMode, CB_GETCURSEL, 0, 0);
    OutputMode newMode = (sel == 1) ? MODE_FP16_SCRGB : MODE_HDR10_PQ;
    if (newMode != g_mode)
    {
        CreateSwapChainForMode(newMode);
    }
}

// ---------------------------------------------------------------------------
// WndProc
// ---------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            RECT rc;
            GetClientRect(hWnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            if (!g_fullscreen)
            {
                if (g_hToolbar)
                    SetWindowPos(g_hToolbar, nullptr, 0, 0, w, TOOLBAR_HEIGHT, SWP_NOZORDER);
                if (g_hRenderWnd)
                {
                    int rh = max(h - TOOLBAR_HEIGHT, 1);
                    SetWindowPos(g_hRenderWnd, nullptr, 0, TOOLBAR_HEIGHT, w, rh, SWP_NOZORDER);
                }
            }
            else
            {
                if (g_hRenderWnd)
                    SetWindowPos(g_hRenderWnd, nullptr, 0, 0, w, h, SWP_NOZORDER);
            }

            if (g_swapChain)
                g_needsResize = true;
        }
        return 0;

    case WM_COMMAND:
        if (g_initialized)
        {
            int id   = LOWORD(wParam);
            int code = HIWORD(wParam);

            if ((id == IDC_EDIT_START || id == IDC_EDIT_END || id == IDC_EDIT_BARS)
                && (code == EN_CHANGE || code == EN_KILLFOCUS))
            {
                ParseControls();
            }
            if (id == IDC_COMBO_MODE && code == CBN_SELCHANGE)
            {
                ParseControls();
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_F11)
        {
            ToggleFullscreen();
        }
        else if (wParam == VK_RETURN)
        {
            // If focus is on an edit control, parse and move focus away
            HWND focused = GetFocus();
            if (focused == g_hEditStart || focused == g_hEditEnd || focused == g_hEditBars)
            {
                ParseControls();
                SetFocus(hWnd);
            }
        }
        else if (wParam == VK_ESCAPE && g_fullscreen)
        {
            ToggleFullscreen();
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// CreateToolbar
// ---------------------------------------------------------------------------

static void CreateToolbar(HWND hWnd, HINSTANCE hInst)
{
    // Create a child window to hold toolbar controls
    g_hToolbar = CreateWindowExW(0, L"STATIC", nullptr,
        WS_CHILD | WS_VISIBLE,
        0, 0, 1200, TOOLBAR_HEIGHT,
        hWnd, nullptr, hInst, nullptr);

    g_hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    int x = 8;
    int y = 10;
    int editW = 80;
    int editH = 22;
    int labelH = 20;
    int gap = 12;

    // Parent controls to g_hWnd so WM_COMMAND notifications reach our WndProc.
    // Toolbar STATIC is just a visual background band.
    auto MakeLabel = [&](const wchar_t* text, int id, int w) -> HWND
    {
        HWND h = CreateWindowExW(0, L"STATIC", text,
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            x, y + 1, w, labelH, hWnd, (HMENU)(INT_PTR)id, hInst, nullptr);
        SendMessageW(h, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        x += w + 4;
        return h;
    };

    auto MakeEdit = [&](const wchar_t* text, int id) -> HWND
    {
        HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            x, y, editW, editH, hWnd, (HMENU)(INT_PTR)id, hInst, nullptr);
        SendMessageW(h, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        x += editW + gap;
        return h;
    };

    wchar_t buf[64];

    MakeLabel(L"Start:", IDC_LABEL_START, 38);
    swprintf_s(buf, L"%.5f", DEFAULT_START_NITS);
    g_hEditStart = MakeEdit(buf, IDC_EDIT_START);

    MakeLabel(L"End:", IDC_LABEL_END, 28);
    swprintf_s(buf, L"%.5f", DEFAULT_END_NITS);
    g_hEditEnd = MakeEdit(buf, IDC_EDIT_END);

    MakeLabel(L"Bars:", IDC_LABEL_BARS, 32);
    swprintf_s(buf, L"%d", DEFAULT_NUM_BARS);
    g_hEditBars = MakeEdit(buf, IDC_EDIT_BARS);

    MakeLabel(L"Mode:", IDC_LABEL_MODE, 36);
    g_hComboMode = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        x, y - 2, 140, 200, hWnd, (HMENU)(INT_PTR)IDC_COMBO_MODE, hInst, nullptr);
    SendMessageW(g_hComboMode, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(g_hComboMode, CB_ADDSTRING, 0, (LPARAM)L"HDR10 PQ");
    SendMessageW(g_hComboMode, CB_ADDSTRING, 0, (LPARAM)L"FP16 scRGB");
    SendMessageW(g_hComboMode, CB_SETCURSEL, 0, 0);
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow)
{
    // DPI awareness
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"PQLuminanceTestClass";
    RegisterClassExW(&wc);

    // Create window
    RECT rc = { 0, 0, 1280, 800 };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);

    g_hWnd = CreateWindowExW(0, L"PQLuminanceTestClass",
        L"PQ Luminance Test Bars",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);

    if (!g_hWnd) return 1;

    // Create toolbar controls
    CreateToolbar(g_hWnd, hInst);

    // Create render child window (swap chain target, sits below toolbar)
    {
        RECT rc;
        GetClientRect(g_hWnd, &rc);
        int w = rc.right - rc.left;
        int rh = max((int)(rc.bottom - rc.top) - TOOLBAR_HEIGHT, 1);
        g_hRenderWnd = CreateWindowExW(0, L"STATIC", nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, TOOLBAR_HEIGHT, w, rh,
            g_hWnd, nullptr, hInst, nullptr);
    }

    // Initialize D3D
    if (!InitD3D())
    {
        MessageBoxW(g_hWnd, L"Failed to initialize Direct3D 11.", L"Error", MB_ICONERROR);
        return 1;
    }

    g_initialized = true;

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    // Message loop
    MSG msg = {};
    while (true)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) goto done;

            // Allow Tab navigation and Enter in edit controls
            if (!IsDialogMessageW(g_hWnd, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        if (g_needsResize)
        {
            ResizeSwapChain();
            g_needsResize = false;
        }

        Render();
    }

done:
    // Cleanup
    SafeRelease(g_cbuffer);
    SafeRelease(g_ps);
    SafeRelease(g_vs);
    ReleaseRTV();
    SafeRelease(g_swapChain3);
    SafeRelease(g_swapChain);
    SafeRelease(g_device1);
    SafeRelease(g_context);
    SafeRelease(g_device);
    SafeRelease(g_factory);
    if (g_hFont) DeleteObject(g_hFont);

    return (int)msg.wParam;
}
