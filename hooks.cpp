#include "hooks.h"
#include <windows.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <algorithm>
#include <iostream>
#include "MinHook.h"
#include <dbghelp.h>
#include <io.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

// Forward declarations for CRT hook types and pointers
typedef FILE* (*_wfopen_t)(const wchar_t*, const wchar_t*);
typedef size_t(*fread_t)(void*, size_t, size_t, FILE*);
typedef int(*fseek_t)(FILE*, long, int);
typedef int(*fclose_t)(FILE*);
extern _wfopen_t OriginalWfopen;
extern fread_t OriginalFread;
extern fseek_t OriginalFseek;
extern fclose_t OriginalFclose;

// Direct3D11 / Direct2D overlay variables
void Log(const char* format, ...);
typedef HRESULT(WINAPI* IDXGISwapChainPresent_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
IDXGISwapChainPresent_t OriginalPresent = nullptr;

typedef HRESULT(STDMETHODCALLTYPE* CreateTexture2D_t)(
    ID3D11Device* This,
    const D3D11_TEXTURE2D_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData,
    ID3D11Texture2D** ppTexture2D
);
CreateTexture2D_t OriginalCreateTexture2D = nullptr;

typedef void (STDMETHODCALLTYPE* CopySubresourceRegion_t)(
    ID3D11DeviceContext* This,
    ID3D11Resource* pDstResource,
    UINT DstSubresource,
    UINT DstX,
    UINT DstY,
    UINT DstZ,
    ID3D11Resource* pSrcResource,
    UINT SrcSubresource,
    const D3D11_BOX* pSrcBox
);
CopySubresourceRegion_t OriginalCopySubresourceRegion = nullptr;

typedef void (STDMETHODCALLTYPE* UpdateSubresource_t)(
    ID3D11DeviceContext* This,
    ID3D11Resource* pDstResource,
    UINT DstSubresource,
    const D3D11_BOX* pDstBox,
    const void* pSrcData,
    UINT SrcRowPitch,
    UINT SrcDepthPitch
);
UpdateSubresource_t OriginalUpdateSubresource = nullptr;

std::unordered_set<ID3D11Resource*> g_SwappedResources;
std::mutex g_SwappedResourcesMutex;

typedef HRESULT(WINAPI* D3D11CreateDeviceAndSwapChain_t)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**
);
D3D11CreateDeviceAndSwapChain_t OriginalD3D11CreateDeviceAndSwapChain = nullptr;

void* HookVMT(void* pInstance, int index, void* pHookFunc) {
    if (!pInstance) return nullptr;
    void** pVMT = *(void***)pInstance;
    if (!pVMT) return nullptr;
    void* pOriginal = pVMT[index];
    DWORD oldProtect;
    if (VirtualProtect(&pVMT[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        pVMT[index] = pHookFunc;
        VirtualProtect(&pVMT[index], sizeof(void*), oldProtect, &oldProtect);
    }
    return pOriginal;
}

void STDMETHODCALLTYPE HookedCopySubresourceRegion(
    ID3D11DeviceContext* This,
    ID3D11Resource* pDstResource,
    UINT DstSubresource,
    UINT DstX,
    UINT DstY,
    UINT DstZ,
    ID3D11Resource* pSrcResource,
    UINT SrcSubresource,
    const D3D11_BOX* pSrcBox
) {
    bool isSwapped = false;
    {
        std::lock_guard<std::mutex> lock(g_SwappedResourcesMutex);
        if (g_SwappedResources.count(pDstResource) > 0) {
            isSwapped = true;
        }
    }
    if (isSwapped) {
        return;
    }
    OriginalCopySubresourceRegion(This, pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox);
}

void STDMETHODCALLTYPE HookedUpdateSubresource(
    ID3D11DeviceContext* This,
    ID3D11Resource* pDstResource,
    UINT DstSubresource,
    const D3D11_BOX* pDstBox,
    const void* pSrcData,
    UINT SrcRowPitch,
    UINT SrcDepthPitch
) {
    bool isSwapped = false;
    {
        std::lock_guard<std::mutex> lock(g_SwappedResourcesMutex);
        if (g_SwappedResources.count(pDstResource) > 0) {
            isSwapped = true;
        }
    }
    if (isSwapped) {
        return;
    }
    OriginalUpdateSubresource(This, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
}

HRESULT WINAPI HookedD3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain** ppSwapChain,
    ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext
);

bool g_D3D11HookInitialized = false;
ULONGLONG g_StartTickCount = 0;
bool g_OverlayEnabled = true;
thread_local std::wstring g_LastLoadedTexFile = L"";
thread_local bool t_InHookCreateTexture2D = false;
std::wstring g_ActiveStageName = L"";
int g_StageTextureCounter = 0;
std::vector<int> g_ActiveStageTexIndices;
std::wstring g_CurrentStageTexName = L"";
std::wstring g_ActiveFieldName = L"";
int g_FieldTextureCounter = 0;
std::vector<int> g_ActiveFieldTexIndices;
std::wstring g_CurrentFieldTexName = L"";
bool g_EnableTextureLogging = false;
bool g_DisableD3D11Hooks = false;
extern std::wstring g_TextureLogPath;
extern std::vector<std::wstring> g_ActiveMods;
extern std::vector<std::wstring> g_ActivePlugins;
extern std::wstring g_BaseDir;
extern std::wstring g_ModsDirectory;
bool FileExists(const std::wstring& path);
std::string WideToAnsi(const std::wstring& wstr);
std::wstring AnsiToWide(const std::string& str);

// Direct2D/DirectWrite rendering resources
ID2D1Factory* g_pD2DFactory = nullptr;
ID2D1RenderTarget* g_pD2DRenderTarget = nullptr;
IDWriteFactory* g_pDWriteFactory = nullptr;
IDWriteTextFormat* g_pTextFormat = nullptr;
ID2D1SolidColorBrush* g_pBrush = nullptr;
IDXGISwapChain* g_pCurrentSwapChain = nullptr;

void CleanupD2D() {
    if (g_pBrush) { g_pBrush->Release(); g_pBrush = nullptr; }
    if (g_pTextFormat) { g_pTextFormat->Release(); g_pTextFormat = nullptr; }
    if (g_pDWriteFactory) { g_pDWriteFactory->Release(); g_pDWriteFactory = nullptr; }
    if (g_pD2DRenderTarget) { g_pD2DRenderTarget->Release(); g_pD2DRenderTarget = nullptr; }
    if (g_pD2DFactory) { g_pD2DFactory->Release(); g_pD2DFactory = nullptr; }
    g_pCurrentSwapChain = nullptr;
}

HRESULT InitD2D(IDXGISwapChain* pSwapChain) {
    static bool g_D2DInitFailed = false;
    if (g_D2DInitFailed) return E_FAIL;

    CleanupD2D();
    Log("[Loader] InitD2D: Starting D2D initialization...\n");

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), (void**)&g_pD2DFactory);
    if (FAILED(hr)) {
        Log("[Loader] InitD2D: Failed to create D2D1Factory. HRESULT: 0x%08X\n", hr);
        g_D2DInitFailed = true;
        return hr;
    }

    IDXGISurface* pBackBuffer = nullptr;
    hr = pSwapChain->GetBuffer(0, __uuidof(IDXGISurface), (void**)&pBackBuffer);
    if (FAILED(hr) || !pBackBuffer) {
        Log("[Loader] InitD2D: Failed to get SwapChain backbuffer. HRESULT: 0x%08X\n", hr);
        g_D2DInitFailed = true;
        return hr;
    }

    DXGI_SURFACE_DESC desc = {};
    pBackBuffer->GetDesc(&desc);
    Log("[Loader] InitD2D: Backbuffer format is %d, size: %dx%d\n", desc.Format, desc.Width, desc.Height);

    // Map sRGB formats to standard formats for D2D compatibility
    DXGI_FORMAT formatsToTry[3] = { desc.Format, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
    if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        formatsToTry[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
    } else if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        formatsToTry[1] = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    hr = E_FAIL;
    bool targetCreated = false;
    for (int fmtIdx = 0; fmtIdx < 3; ++fmtIdx) {
        DXGI_FORMAT currentFmt = formatsToTry[fmtIdx];
        
        D2D1_RENDER_TARGET_PROPERTIES propsList[] = {
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(currentFmt, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(currentFmt, D2D1_ALPHA_MODE_IGNORE)),
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(currentFmt, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(currentFmt, D2D1_ALPHA_MODE_IGNORE))
        };

        for (int i = 0; i < 4; ++i) {
            hr = g_pD2DFactory->CreateDxgiSurfaceRenderTarget(pBackBuffer, &propsList[i], &g_pD2DRenderTarget);
            if (SUCCEEDED(hr) && g_pD2DRenderTarget) {
                Log("[Loader] InitD2D: CreateDxgiSurfaceRenderTarget succeeded using format %d, attempt %d!\n", currentFmt, i);
                targetCreated = true;
                break;
            }
        }
        if (targetCreated) break;
    }

    pBackBuffer->Release();
    if (FAILED(hr) || !g_pD2DRenderTarget) {
        Log("[Loader] InitD2D: Failed to create DXGI Surface Render Target on all format and alpha attempts. HRESULT: 0x%08X\n", hr);
        g_D2DInitFailed = true;
        return hr;
    }

    g_D2DInitFailed = false; // Successfully initialized

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_pDWriteFactory);
    if (FAILED(hr)) {
        Log("[Loader] InitD2D: Failed to create DWriteFactory. HRESULT: 0x%08X\n", hr);
        g_D2DInitFailed = true;
        return hr;
    }

    hr = g_pDWriteFactory->CreateTextFormat(
        L"Consolas",
        nullptr,
        DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        22.0f,
        L"en-us",
        &g_pTextFormat
    );
    if (FAILED(hr)) {
        Log("[Loader] InitD2D: Failed to create TextFormat. HRESULT: 0x%08X\n", hr);
        g_D2DInitFailed = true;
        return hr;
    }

    hr = g_pD2DRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Red), &g_pBrush);
    if (FAILED(hr)) {
        Log("[Loader] InitD2D: Failed to create SolidColorBrush. HRESULT: 0x%08X\n", hr);
        g_D2DInitFailed = true;
        return hr;
    }

    g_pCurrentSwapChain = pSwapChain;
    Log("[Loader] InitD2D: Direct2D context initialized successfully!\n");
    return S_OK;
}

static HWND g_hOverlayWnd = nullptr;

LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            
            // Set text parameters
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 0, 0)); // Red text

            /* 
            Font setup
            Height (Size): The first argument 32 is the height in pixels. You can lower it (e.g., to 16 or 20) to make it smaller.
            Weight (Boldness): The fifth argument FW_BOLD makes it thick. You can change this to FW_NORMAL, FW_LIGHT, or FW_SEMIBOLD to alter the thickness.
            Italic / Underline / Strikeout: The sixth, seventh, and eighth arguments (FALSE, FALSE, FALSE) stand for: Italic, Underline, and Strikeout. Set any to TRUE to activate them (e.g., setting the first to TRUE makes it italicized).
            Color: In SetTextColor(hdc, RGB(255, 0, 0));, the RGB(255, 0, 0) is Red. You can change these values (from 0 to 255) to make it any color. E.g., RGB(255, 255, 255) for white, or RGB(0, 255, 0) for green.
            Font Family: The last argument "Arial" can be changed to any installed Windows system font, such as:
            "Segoe UI" (Standard clean Windows font)
            "Trebuchet MS" or "Verdana" (Modern looking)
            "Consolas" or "Courier New" (Retro/Terminal monospaced look)
            "Georgia" or "Times New Roman" (Serif look)
            */
            HFONT hFont = CreateFontA(
                20, 0, 0, 0, FW_LIGHT, TRUE, FALSE, FALSE,
                ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI"
            );
            HFONT oldFont = (HFONT)SelectObject(hdc, hFont);

            const char* text = "FFVIISE Mod Loader Active - by NfgOdin";
            TextOutA(hdc, 25, 25, text, (int)strlen(text));

            char modsText[128];
            snprintf(modsText, sizeof(modsText), "Active Mods Loaded: %zu", g_ActiveMods.size());
            TextOutA(hdc, 25, 50, modsText, (int)strlen(modsText));

            char pluginsText[128];
            snprintf(pluginsText, sizeof(pluginsText), "Active Plugins Loaded: %zu", g_ActivePlugins.size());
            TextOutA(hdc, 25, 75, pluginsText, (int)strlen(pluginsText));

            SelectObject(hdc, oldFont);
            DeleteObject(hFont);
            
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1; // Prevent background erasing to maintain transparency
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

DWORD WINAPI OverlayThread(LPVOID lpParam) {
    HWND hGameWnd = (HWND)lpParam;
    
    // Register class
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "FFVIISE_Overlay";
    RegisterClassExA(&wc);

    // Get game window rect
    RECT rect;
    GetWindowRect(hGameWnd, &rect);

    // Create transparent borderless overlay window
    g_hOverlayWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED,
        "FFVIISE_Overlay", "", WS_POPUP,
        rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        hGameWnd, NULL, wc.hInstance, NULL
    );

    if (!g_hOverlayWnd) return 0;

    // Use alpha keying to make background fully transparent
    SetLayeredWindowAttributes(g_hOverlayWnd, RGB(0, 0, 0), 255, LWA_COLORKEY);
    ShowWindow(g_hOverlayWnd, SW_SHOW);
    UpdateWindow(g_hOverlayWnd);

    ULONGLONG start = GetTickCount64();
    MSG msg;
    while (GetTickCount64() - start < 5000) {
        // Handle window positioning
        if (IsWindow(hGameWnd)) {
            RECT gameRect;
            GetWindowRect(hGameWnd, &gameRect);
            SetWindowPos(g_hOverlayWnd, HWND_TOPMOST, gameRect.left, gameRect.top, gameRect.right - gameRect.left, gameRect.bottom - gameRect.top, SWP_NOACTIVATE);
        } else {
            break; // game exited
        }

        // Process message queue
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        
        InvalidateRect(g_hOverlayWnd, NULL, TRUE); // Redraw
        Sleep(30);
    }

    DestroyWindow(g_hOverlayWnd);
    UnregisterClassA("FFVIISE_Overlay", wc.hInstance);
    g_OverlayEnabled = false;
    Log("[Loader] OverlayThread: Timer finished. Overlay destroyed.\n");
    return 0;
}

HRESULT WINAPI HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (g_OverlayEnabled) {
        if (g_StartTickCount == 0) {
            g_StartTickCount = GetTickCount64();
            Log("[Loader] HookedPresent: First call. Starting Overlay Thread...\n");
            
            DXGI_SWAP_CHAIN_DESC desc = {};
            if (SUCCEEDED(pSwapChain->GetDesc(&desc)) && desc.OutputWindow) {
                CreateThread(NULL, 0, OverlayThread, desc.OutputWindow, 0, NULL);
            }
        }
    }

    return OriginalPresent(pSwapChain, SyncInterval, Flags);
}

typedef HRESULT(WINAPI* CreateDXGIFactory_t)(const IID&, void**);
CreateDXGIFactory_t OriginalCreateDXGIFactory = nullptr;

typedef HRESULT(WINAPI* IDXGIFactoryCreateSwapChain_t)(IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain);
IDXGIFactoryCreateSwapChain_t OriginalCreateSwapChain = nullptr;

// Hook IDXGIFactory2::CreateSwapChainForHwnd (index 15)
typedef HRESULT(WINAPI* IDXGIFactory2CreateSwapChainForHwnd_t)(
    IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain
);
IDXGIFactory2CreateSwapChainForHwnd_t OriginalCreateSwapChainForHwnd = nullptr;

HRESULT WINAPI HookedCreateSwapChainForHwnd(
    IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain
) {
    Log("[Loader] IDXGIFactory2::CreateSwapChainForHwnd intercepted!\n");
    HRESULT hr = OriginalCreateSwapChainForHwnd(pFactory, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        IDXGISwapChain* pSwapChain = (IDXGISwapChain*)*ppSwapChain;
        if (!OriginalPresent) {
            Log("[Loader] SwapChain created via CreateSwapChainForHwnd! Hooking Present via VMT...\n");
            OriginalPresent = (IDXGISwapChainPresent_t)HookVMT(pSwapChain, 8, HookedPresent);
            Log("[Loader] D3D11 SwapChain Present Hooked via VMT! Original: %p\n", OriginalPresent);
        }
    }
    return hr;
}

HRESULT WINAPI HookedCreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) {
    Log("[Loader] IDXGIFactory::CreateSwapChain intercepted!\n");
    HRESULT hr = OriginalCreateSwapChain(pFactory, pDevice, pDesc, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        IDXGISwapChain* pSwapChain = *ppSwapChain;
        if (!OriginalPresent) {
            Log("[Loader] SwapChain created via IDXGIFactory! Hooking Present via VMT...\n");
            OriginalPresent = (IDXGISwapChainPresent_t)HookVMT(pSwapChain, 8, HookedPresent);
            Log("[Loader] D3D11 SwapChain Present Hooked via VMT! Original: %p\n", OriginalPresent);
        }
    }
    return hr;
}

void LogTexture(const char* format, ...) {
    if (!g_EnableTextureLogging) return;

    FILE* f = NULL;
    _wfopen_s(&f, g_TextureLogPath.c_str(), L"a");
    if (f) {
        va_list args;
        va_start(args, format);
        vfprintf(f, format, args);
        va_end(args);
        fclose(f);
    }
}

struct DDS_PIXELFORMAT {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwFourCC;
    DWORD dwRGBBitCount;
    DWORD dwRBitMask;
    DWORD dwGBitMask;
    DWORD dwBBitMask;
    DWORD dwABitMask;
};

struct DDS_HEADER {
    DWORD           dwSize;
    DWORD           dwFlags;
    DWORD           dwHeight;
    DWORD           dwWidth;
    DWORD           dwPitchOrLinearSize;
    DWORD           dwDepth;
    DWORD           dwMipMapCount;
    DWORD           dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    DWORD           dwCaps;
    DWORD           dwCaps2;
    DWORD           dwCaps3;
    DWORD           dwCaps4;
    DWORD           dwReserved2;
};

struct DDS_HEADER_DXT10 {
    DXGI_FORMAT dxgiFormat;
    DWORD       resourceDimension;
    UINT        miscFlag;
    UINT        arraySize;
    UINT        miscFlags2;
};

#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3) \
    ((DWORD)(BYTE)(ch0) | ((DWORD)(BYTE)(ch1) << 8) | \
    ((DWORD)(BYTE)(ch2) << 16) | ((DWORD)(BYTE)(ch3) << 24))
#endif

#define DDS_FOURCC      0x00000004  // DDPF_FOURCC
#define DDS_RGB         0x00000040  // DDPF_RGB

HRESULT LoadTextureFromPng(ID3D11Device* pDevice, const std::wstring& filePath, UINT bindFlags, ID3D11Texture2D** ppTexture) {
    IWICImagingFactory* pWICFactory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pWICFactory));
    if (FAILED(hr)) {
        CoInitialize(nullptr);
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pWICFactory));
        if (FAILED(hr)) return hr;
    }

    IWICBitmapDecoder* pDecoder = nullptr;
    hr = pWICFactory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder);
    if (FAILED(hr)) {
        pWICFactory->Release();
        return hr;
    }

    IWICBitmapFrameDecode* pFrame = nullptr;
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) {
        pDecoder->Release();
        pWICFactory->Release();
        return hr;
    }

    UINT width = 0, height = 0;
    pFrame->GetSize(&width, &height);

    IWICFormatConverter* pConverter = nullptr;
    hr = pWICFactory->CreateFormatConverter(&pConverter);
    if (FAILED(hr)) {
        pFrame->Release();
        pDecoder->Release();
        pWICFactory->Release();
        return hr;
    }

    hr = pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        pConverter->Release();
        pFrame->Release();
        pDecoder->Release();
        pWICFactory->Release();
        return hr;
    }

    UINT rowPitch = width * 4;
    UINT imageSize = rowPitch * height;
    std::vector<BYTE> pixels(imageSize);
    hr = pConverter->CopyPixels(nullptr, rowPitch, imageSize, pixels.data());
    if (FAILED(hr)) {
        pConverter->Release();
        pFrame->Release();
        pDecoder->Release();
        pWICFactory->Release();
        return hr;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bindFlags;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = pixels.data();
    initData.SysMemPitch = rowPitch;
    initData.SysMemSlicePitch = imageSize;

    hr = pDevice->CreateTexture2D(&desc, &initData, ppTexture);

    pConverter->Release();
    pFrame->Release();
    pDecoder->Release();
    pWICFactory->Release();
    return hr;
}

HRESULT LoadTextureFromDds(ID3D11Device* pDevice, const std::wstring& filePath, UINT bindFlags, ID3D11Texture2D** ppTexture) {
    FILE* f = nullptr;
    _wfopen_s(&f, filePath.c_str(), L"rb");
    if (!f) return E_FAIL;

    DWORD dwMagic = 0;
    fread(&dwMagic, 1, 4, f);
    if (dwMagic != MAKEFOURCC('D', 'D', 'S', ' ')) {
        fclose(f);
        return E_FAIL;
    }

    DDS_HEADER header = {};
    fread(&header, 1, sizeof(DDS_HEADER), f);

    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool isDXT10 = false;
    DDS_HEADER_DXT10 header10 = {};

    if ((header.ddspf.dwFlags & DDS_FOURCC) && (header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', '1', '0'))) {
        fread(&header10, 1, sizeof(DDS_HEADER_DXT10), f);
        format = header10.dxgiFormat;
        isDXT10 = true;
    } else if (header.ddspf.dwFlags & DDS_FOURCC) {
        switch (header.ddspf.dwFourCC) {
            case MAKEFOURCC('D', 'X', 'T', '1'):
                format = DXGI_FORMAT_BC1_UNORM;
                break;
            case MAKEFOURCC('D', 'X', 'T', '3'):
                format = DXGI_FORMAT_BC2_UNORM;
                break;
            case MAKEFOURCC('D', 'X', 'T', '5'):
                format = DXGI_FORMAT_BC3_UNORM;
                break;
            case MAKEFOURCC('A', 'T', 'I', '2'):
                format = DXGI_FORMAT_BC5_UNORM;
                break;
            default:
                break;
        }
    } else if (header.ddspf.dwFlags & DDS_RGB) {
        if (header.ddspf.dwRGBBitCount == 32) {
            if (header.ddspf.dwRBitMask == 0x00ff0000 && header.ddspf.dwGBitMask == 0x0000ff00 && header.ddspf.dwBBitMask == 0x000000ff) {
                format = DXGI_FORMAT_B8G8R8A8_UNORM;
            } else if (header.ddspf.dwRBitMask == 0x000000ff && header.ddspf.dwGBitMask == 0x0000ff00 && header.ddspf.dwBBitMask == 0x00ff0000) {
                format = DXGI_FORMAT_R8G8B8A8_UNORM;
            }
        }
    }

    if (format == DXGI_FORMAT_UNKNOWN) {
        fclose(f);
        return E_FAIL;
    }

    UINT width = header.dwWidth;
    UINT height = header.dwHeight;
    UINT mipLevels = max(1u, header.dwMipMapCount);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = mipLevels;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bindFlags;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    long currentOffset = ftell(f);
    fseek(f, 0, SEEK_END);
    long endOffset = ftell(f);
    fseek(f, currentOffset, SEEK_SET);

    size_t dataSize = endOffset - currentOffset;
    std::vector<BYTE> rawData(dataSize);
    fread(rawData.data(), 1, dataSize, f);
    fclose(f);

    std::vector<D3D11_SUBRESOURCE_DATA> initData(mipLevels);
    size_t offset = 0;
    UINT w = width;
    UINT h = height;

    for (UINT i = 0; i < mipLevels; ++i) {
        size_t rowPitch = 0;
        size_t slicePitch = 0;

        if (format == DXGI_FORMAT_BC1_UNORM || format == DXGI_FORMAT_BC1_UNORM_SRGB ||
            format == DXGI_FORMAT_BC2_UNORM || format == DXGI_FORMAT_BC2_UNORM_SRGB ||
            format == DXGI_FORMAT_BC3_UNORM || format == DXGI_FORMAT_BC3_UNORM_SRGB ||
            format == DXGI_FORMAT_BC4_UNORM || format == DXGI_FORMAT_BC4_SNORM ||
            format == DXGI_FORMAT_BC5_UNORM || format == DXGI_FORMAT_BC5_SNORM ||
            format == DXGI_FORMAT_BC6H_UF16 || format == DXGI_FORMAT_BC6H_SF16 ||
            format == DXGI_FORMAT_BC7_UNORM || format == DXGI_FORMAT_BC7_UNORM_SRGB) {
            
            size_t numBlocksWide = max(1u, (w + 3) / 4);
            size_t numBlocksHigh = max(1u, (h + 3) / 4);
            size_t bytesPerBlock = (format == DXGI_FORMAT_BC1_UNORM || format == DXGI_FORMAT_BC1_UNORM_SRGB || format == DXGI_FORMAT_BC4_UNORM || format == DXGI_FORMAT_BC4_SNORM) ? 8 : 16;
            rowPitch = numBlocksWide * bytesPerBlock;
            slicePitch = rowPitch * numBlocksHigh;
        } else {
            size_t bpp = 32;
            rowPitch = (w * bpp + 7) / 8;
            slicePitch = rowPitch * h;
        }

        if (offset + slicePitch > dataSize) {
            return E_FAIL;
        }

        initData[i].pSysMem = rawData.data() + offset;
        initData[i].SysMemPitch = (UINT)rowPitch;
        initData[i].SysMemSlicePitch = (UINT)slicePitch;

        offset += slicePitch;
        w = max(1u, w / 2);
        h = max(1u, h / 2);
    }

    return pDevice->CreateTexture2D(&desc, initData.data(), ppTexture);
}

std::wstring ResolveTextureOverride(const std::wstring& assetName) {
    if (assetName.empty()) return L"";
    
    std::wstring baseName = assetName;
    size_t dotPos = baseName.rfind(L'.');
    if (dotPos != std::wstring::npos) {
        baseName = baseName.substr(0, dotPos);
    }

    for (const auto& modFolder : g_ActiveMods) {
        // 1. Check under /field/<fieldName>/
        if (!g_ActiveFieldName.empty()) {
            std::wstring fieldPngPath = g_BaseDir + L"\\" + g_ModsDirectory + L"\\" + modFolder + L"\\field\\" + g_ActiveFieldName + L"\\" + baseName + L".png";
            if (FileExists(fieldPngPath)) return fieldPngPath;

            std::wstring fieldDdsPath = g_BaseDir + L"\\" + g_ModsDirectory + L"\\" + modFolder + L"\\field\\" + g_ActiveFieldName + L"\\" + baseName + L".dds";
            if (FileExists(fieldDdsPath)) return fieldDdsPath;
        }

        // 2. Check under /textures/
        std::wstring pngPath = g_BaseDir + L"\\" + g_ModsDirectory + L"\\" + modFolder + L"\\textures\\" + baseName + L".png";
        if (FileExists(pngPath)) return pngPath;

        std::wstring ddsPath = g_BaseDir + L"\\" + g_ModsDirectory + L"\\" + modFolder + L"\\textures\\" + baseName + L".dds";
        if (FileExists(ddsPath)) return ddsPath;

        // 3. Check under /battle/ (specifically for battle stages/assets)
        std::wstring battlePngPath = g_BaseDir + L"\\" + g_ModsDirectory + L"\\" + modFolder + L"\\battle\\" + baseName + L".png";
        if (FileExists(battlePngPath)) return battlePngPath;

        std::wstring battleDdsPath = g_BaseDir + L"\\" + g_ModsDirectory + L"\\" + modFolder + L"\\battle\\" + baseName + L".dds";
        if (FileExists(battleDdsPath)) return battleDdsPath;
    }
    return L"";
}

HRESULT LoadOverrideTexture(ID3D11Device* pDevice, const std::wstring& filePath, UINT bindFlags, ID3D11Texture2D** ppTexture) {
    std::wstring ext = filePath.substr(filePath.rfind(L'.'));
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

    if (ext == L".png") {
        return LoadTextureFromPng(pDevice, filePath, bindFlags, ppTexture);
    } else if (ext == L".dds") {
        return LoadTextureFromDds(pDevice, filePath, bindFlags, ppTexture);
    }
    return E_FAIL;
}

HRESULT STDMETHODCALLTYPE HookedCreateTexture2D(
    ID3D11Device* This,
    const D3D11_TEXTURE2D_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData,
    ID3D11Texture2D** ppTexture2D
) {
    if (t_InHookCreateTexture2D) {
        return OriginalCreateTexture2D(This, pDesc, pInitialData, ppTexture2D);
    }
    t_InHookCreateTexture2D = true;

    std::wstring assetName = L"";
    if (pDesc) {
        if (!g_LastLoadedTexFile.empty()) {
            if (pDesc->Usage == 2 || pDesc->Usage == 3) {
                assetName = g_LastLoadedTexFile;
            } else if (pDesc->Usage == 0 || pDesc->Usage == 1) {
                assetName = g_LastLoadedTexFile;
                g_LastLoadedTexFile = L""; // Clear character texture tracking!
            }
        } else if (!g_ActiveStageName.empty() && !g_ActiveStageTexIndices.empty()) {
            if (pDesc->Usage == 2 && pDesc->BindFlags == 8 && g_StageTextureCounter < g_ActiveStageTexIndices.size()) {
                int actualIndex = g_ActiveStageTexIndices[g_StageTextureCounter];
                wchar_t stageTexName[64];
                swprintf_s(stageTexName, L"%s_T%02d_00", g_ActiveStageName.c_str(), actualIndex);
                g_CurrentStageTexName = stageTexName;
                g_StageTextureCounter++;
                assetName = g_CurrentStageTexName;
            } else if (pDesc->Usage == 0 && !g_CurrentStageTexName.empty() && pDesc->BindFlags == 40) {
                assetName = g_CurrentStageTexName;
                g_CurrentStageTexName = L""; // Consume and clear immediately to prevent matching subsequent static textures!
            }
        } else if (!g_ActiveFieldName.empty() && !g_ActiveFieldTexIndices.empty()) {
            if (pDesc->Usage == 2 && pDesc->BindFlags == 8 && g_FieldTextureCounter < g_ActiveFieldTexIndices.size()) {
                int actualIndex = g_ActiveFieldTexIndices[g_FieldTextureCounter];
                wchar_t fieldTexName[128];
                swprintf_s(fieldTexName, L"%s_%02d_00", g_ActiveFieldName.c_str(), actualIndex);
                g_CurrentFieldTexName = fieldTexName;
                g_FieldTextureCounter++;
                assetName = g_CurrentFieldTexName;
            } else if (pDesc->Usage == 0 && !g_CurrentFieldTexName.empty() && pDesc->BindFlags == 40) {
                assetName = g_CurrentFieldTexName;
                g_CurrentFieldTexName = L""; // Consume and clear immediately
            }
        }

        if (!assetName.empty()) {
            LogTexture("[D3D11] CreateTexture2D: Asset=%S, Width=%u, Height=%u, MipLevels=%u, ArraySize=%u, Format=%u, SampleDescCount=%u, SampleDescQuality=%u, Usage=%u, BindFlags=%u, CPUAccessFlags=%u, MiscFlags=%u\n",
                assetName.c_str(), pDesc->Width, pDesc->Height, pDesc->MipLevels, pDesc->ArraySize, pDesc->Format,
                pDesc->SampleDesc.Count, pDesc->SampleDesc.Quality, pDesc->Usage, pDesc->BindFlags,
                pDesc->CPUAccessFlags, pDesc->MiscFlags);
        } else {
            LogTexture("[D3D11] CreateTexture2D: Asset=UNKNOWN, Width=%u, Height=%u, MipLevels=%u, ArraySize=%u, Format=%u, SampleDescCount=%u, SampleDescQuality=%u, Usage=%u, BindFlags=%u, CPUAccessFlags=%u, MiscFlags=%u\n",
                pDesc->Width, pDesc->Height, pDesc->MipLevels, pDesc->ArraySize, pDesc->Format,
                pDesc->SampleDesc.Count, pDesc->SampleDesc.Quality, pDesc->Usage, pDesc->BindFlags,
                pDesc->CPUAccessFlags, pDesc->MiscFlags);
        }
    }
    HRESULT hr = OriginalCreateTexture2D(This, pDesc, pInitialData, ppTexture2D);
    if (SUCCEEDED(hr) && ppTexture2D && *ppTexture2D && !assetName.empty()) {
        std::wstring overridePath = ResolveTextureOverride(assetName);
        if (!overridePath.empty()) {
            if (pDesc->Usage == 0) {
                ID3D11Texture2D* pOverrideTex = nullptr;
                HRESULT hrLoad = LoadOverrideTexture(This, overridePath, pDesc->BindFlags, &pOverrideTex);
                if (SUCCEEDED(hrLoad) && pOverrideTex) {
                    (*ppTexture2D)->Release();
                    *ppTexture2D = pOverrideTex;
                    
                    {
                        std::lock_guard<std::mutex> lock(g_SwappedResourcesMutex);
                        g_SwappedResources.insert(pOverrideTex);
                    }
                    
                    Log("[Loader] Swapped static texture %S in CreateTexture2D: Override=%p\n", assetName.c_str(), pOverrideTex);
                } else {
                    Log("[Loader] ERROR: LoadOverrideTexture failed for path %S: 0x%08X\n", overridePath.c_str(), hrLoad);
                }
            }
        }
    }
    t_InHookCreateTexture2D = false;
    return hr;
}

HRESULT WINAPI HookedD3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain** ppSwapChain,
    ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext
) {
    if (g_DisableD3D11Hooks) {
        Log("[Loader] D3D11 Hooks disabled in mods_loader.ini. Bypassing D3D11 hooks.\n");
        return OriginalD3D11CreateDeviceAndSwapChain(
            pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
            SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext
        );
    }

    Log("[Loader] D3D11CreateDeviceAndSwapChain intercepted! ppSwapChain: %p, ppDevice: %p\n", ppSwapChain, ppDevice);
    HRESULT hr = OriginalD3D11CreateDeviceAndSwapChain(
        pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
        SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext
    );
    Log("[Loader] Original D3D11CreateDeviceAndSwapChain returned: 0x%08X\n", hr);
    
    if (SUCCEEDED(hr)) {
        if (ppDevice && *ppDevice) {
            ID3D11Device* pDevice = *ppDevice;
            if (!OriginalCreateTexture2D) {
                Log("[Loader] Hooking ID3D11Device::CreateTexture2D via VMT...\n");
                OriginalCreateTexture2D = (CreateTexture2D_t)HookVMT(pDevice, 5, HookedCreateTexture2D);
                Log("[Loader] ID3D11Device::CreateTexture2D Hooked via VMT! Original: %p\n", OriginalCreateTexture2D);
            }
        }
        if (ppImmediateContext && *ppImmediateContext) {
            ID3D11DeviceContext* pContext = *ppImmediateContext;
            if (!OriginalCopySubresourceRegion) {
                Log("[Loader] Hooking ID3D11DeviceContext::CopySubresourceRegion via VMT...\n");
                OriginalCopySubresourceRegion = (CopySubresourceRegion_t)HookVMT(pContext, 46, HookedCopySubresourceRegion);
                Log("[Loader] ID3D11DeviceContext::CopySubresourceRegion Hooked via VMT! Original: %p\n", OriginalCopySubresourceRegion);
            }
            if (!OriginalUpdateSubresource) {
                Log("[Loader] Hooking ID3D11DeviceContext::UpdateSubresource via VMT...\n");
                OriginalUpdateSubresource = (UpdateSubresource_t)HookVMT(pContext, 48, HookedUpdateSubresource);
                Log("[Loader] ID3D11DeviceContext::UpdateSubresource Hooked via VMT! Original: %p\n", OriginalUpdateSubresource);
            }
        }
        if (ppSwapChain && *ppSwapChain) {
            IDXGISwapChain* pSwapChain = *ppSwapChain;
            if (!OriginalPresent) {
                Log("[Loader] Hooking IDXGISwapChain::Present via VMT...\n");
                OriginalPresent = (IDXGISwapChainPresent_t)HookVMT(pSwapChain, 8, HookedPresent);
                Log("[Loader] IDXGISwapChain::Present Hooked via VMT! Original: %p\n", OriginalPresent);
            }
        } else if (ppDevice && *ppDevice) {
            ID3D11Device* pDevice = *ppDevice;
            Log("[Loader] Device created without SwapChain. Hooking DXGI Factory...\n");
            
            IDXGIDevice* pDxgiDevice = nullptr;
            if (SUCCEEDED(pDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDxgiDevice)) && pDxgiDevice) {
                IDXGIAdapter* pDxgiAdapter = nullptr;
                if (SUCCEEDED(pDxgiDevice->GetAdapter(&pDxgiAdapter)) && pDxgiAdapter) {
                    IDXGIFactory* pDxgiFactory = nullptr;
                    if (SUCCEEDED(pDxgiAdapter->GetParent(__uuidof(IDXGIFactory), (void**)&pDxgiFactory)) && pDxgiFactory) {
                        if (!OriginalCreateSwapChain) {
                            Log("[Loader] Hooking IDXGIFactory::CreateSwapChain via VMT...\n");
                            OriginalCreateSwapChain = (IDXGIFactoryCreateSwapChain_t)HookVMT(pDxgiFactory, 10, HookedCreateSwapChain);
                            Log("[Loader] IDXGIFactory::CreateSwapChain Hooked via VMT! Original: %p\n", OriginalCreateSwapChain);

                            IDXGIFactory2* pDxgiFactory2 = nullptr;
                            if (SUCCEEDED(pDxgiFactory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&pDxgiFactory2)) && pDxgiFactory2) {
                                Log("[Loader] Hooking IDXGIFactory2::CreateSwapChainForHwnd via VMT...\n");
                                OriginalCreateSwapChainForHwnd = (IDXGIFactory2CreateSwapChainForHwnd_t)HookVMT(pDxgiFactory2, 15, HookedCreateSwapChainForHwnd);
                                Log("[Loader] IDXGIFactory2::CreateSwapChainForHwnd Hooked via VMT! Original: %p\n", OriginalCreateSwapChainForHwnd);
                                pDxgiFactory2->Release();
                            }
                        }
                        pDxgiFactory->Release();
                    }
                    pDxgiAdapter->Release();
                }
                pDxgiDevice->Release();
            }
        }
    }
    return hr;
}


// Configuration variables
std::wstring g_ModsDirectory = L"mods";
std::wstring g_PluginsDirectory = L"plugins";
std::wstring g_SplashScreens = L"custom";
std::wstring g_BaseDir = L"";
std::vector<std::wstring> g_ActiveMods;
std::vector<std::wstring> g_ActivePlugins;
bool g_EnableLogging = true;
std::wstring g_LogFileName = L"mods_loader_log.txt";
std::wstring g_LogPath = L"mods_loader_log.txt";
std::wstring g_TextureLogFileName = L"d3d11_texture_log.txt";
std::wstring g_TextureLogPath = L"d3d11_texture_log.txt";
std::wstring g_IniPath = L"mods_loader.ini";

bool DirectoryExists(const std::wstring& path) {
    DWORD dwAttrib = GetFileAttributesW(path.c_str());
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && (dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

std::vector<std::wstring> GetSubdirectories(const std::wstring& dirPath) {
    std::vector<std::wstring> subdirs;
    std::wstring searchPath = dirPath + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                wcscmp(fd.cFileName, L".") != 0 &&
                wcscmp(fd.cFileName, L"..") != 0) {
                subdirs.push_back(fd.cFileName);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    return subdirs;
}

struct FileInfo {
    std::wstring name;
    DWORD size;
};

std::vector<FileInfo> GetFilesInDirectory(const std::wstring& dirPath) {
    std::vector<FileInfo> files;
    std::wstring searchPath = dirPath + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                FileInfo info;
                info.name = fd.cFileName;
                info.size = fd.nFileSizeLow;
                files.push_back(info);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    return files;
}

// Logging Helper
void Log(const char* format, ...) {
    if (!g_EnableLogging) return;
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);
    
    FILE* f = NULL;
    _wfopen_s(&f, g_LogPath.c_str(), L"a");
    if (f) {
        va_list args;
        va_start(args, format);
        vfprintf(f, format, args);
        va_end(args);
        fclose(f);
    }
}




// Structs to hold LGP metadata
struct LgpEntry {
    std::string name;
    std::wstring diskName;
    DWORD dataStart; // Offset of the DATA-ENTRY_HEADER in the LGP
    DWORD size;      // Size of the raw file data
    std::wstring modFolder; // Name of the mod folder this entry resides in
    DWORD originalDataStart = 0; // If from physical LGP, the original offset
};

struct RedirectState {
    std::wstring archivePath;
    bool isLgp = false;
    std::vector<LgpEntry> entries;

    // Virtual LGP Archive state (when physical LGP is missing)
    bool isVirtualLgp = false;
    DWORD virtualFileOffset = 0; // Simulated pointer in the virtual LGP file
    std::vector<BYTE> virtualArchiveData; // Holds the header and TOC in memory
    std::wstring tempFilePath; // Path to the dummy file on disk
    FILE* physicalLgpFile = nullptr; // Stream to read original assets from physical LGP directly

    // Active redirection state for LGP file handle
    bool isRedirecting = false;
    std::wstring overrideFilePath;
    FILE* overrideFile = nullptr;
    DWORD overrideVirtualOffset = 0; // Read offset in the loose file
    DWORD overrideSize = 0;          // Size of the loose file
    DWORD fakeHeaderOffset = 0;      // If game is reading the 24-byte DATA-ENTRY_HEADER
    BYTE fakeHeader[24];
    std::string activeEntryName; // Keep track of the currently active entry for redirection
};

int CharToLookupValue(char c) {
    if (c == '.') return -1;
    if (c == '_') return 10;
    if (c == '-') return 11;
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= 'A' && c <= 'Z') return c - 'A';
    return 0; // fallback
}

int FilenameToLookupIndex(const std::string& filename) {
    if (filename.empty()) return 1;
    int lv1 = CharToLookupValue(filename[0]);
    int lv2 = (filename.size() > 1) ? CharToLookupValue(filename[1]) : 0;
    
    if (lv1 < 0) lv1 = 0;
    if (lv2 < 0) lv2 = 0;
    if (lv1 >= 30) lv1 = 29;
    if (lv2 >= 30) lv2 = 29;
    
    return lv1 * 30 + lv2 + 1; // Returns index 1 to 900
}

void PopulateVirtualLgp(const std::wstring& archiveRelPath, RedirectState& state) {
    Log("[Loader] PopulateVirtualLgp started for rel path: %S\n", archiveRelPath.c_str());
    
    std::wstring archiveLower = archiveRelPath;
    std::transform(archiveLower.begin(), archiveLower.end(), archiveLower.begin(), ::towlower);
    if (archiveLower.find(L"battle\\battle") != std::wstring::npos) {
        g_ActiveStageName = L"";
        g_StageTextureCounter = 0;
        g_CurrentStageTexName = L"";
        Log("[Loader] Reset stage sequencer for new battle.\n");
    }
    
    struct MergedFileInfo {
        std::wstring diskName;
        DWORD size;
        std::wstring modFolder;
        DWORD originalDataStart = 0;
    };
    std::unordered_map<std::wstring, MergedFileInfo> mergedFiles;

    // 1. Read base physical LGP if it exists
    std::wstring originalLgpPath = g_BaseDir + L"\\ff7\\workingdir\\data\\" + archiveRelPath + L".lgp";
    FILE* fLgp = OriginalWfopen(originalLgpPath.c_str(), L"rb");
    if (fLgp) {
        BYTE header[16];
        if (OriginalFread(header, 1, 16, fLgp) == 16) {
            DWORD numOriginalFiles = *(DWORD*)&header[12];
            std::vector<BYTE> tocBuffer(numOriginalFiles * 27);
            if (OriginalFread(tocBuffer.data(), 1, numOriginalFiles * 27, fLgp) == numOriginalFiles * 27) {
                for (DWORD i = 0; i < numOriginalFiles; ++i) {
                    BYTE* entryPtr = &tocBuffer[i * 27];
                    char name[21] = { 0 };
                    memcpy(name, entryPtr, 20);
                    
                    std::string entryName = name;
                    std::wstring wEntryName(entryName.begin(), entryName.end());
                    std::wstring lowerName = wEntryName;
                    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::towlower);
                    
                    DWORD dataStart = *(DWORD*)&entryPtr[20];
                    
                    OriginalFseek(fLgp, dataStart + 20, SEEK_SET);
                    DWORD sizeVal = 0;
                    OriginalFread(&sizeVal, 1, 4, fLgp);
                    
                    MergedFileInfo info;
                    info.diskName = wEntryName;
                    info.size = sizeVal;
                    info.modFolder = L"";
                    info.originalDataStart = dataStart;
                    mergedFiles[lowerName] = info;
                }
            }
        }
        OriginalFclose(fLgp);
    }
    
    // 2. Scan active mods in reverse order (lowest priority first) so higher priority overrides them
    for (auto it = g_ActiveMods.rbegin(); it != g_ActiveMods.rend(); ++it) {
        std::wstring modFolder = *it;
        std::wstring archiveFolder = g_BaseDir + L"\\" + g_ModsDirectory + L"\\" + modFolder + L"\\" + archiveRelPath;
        if (DirectoryExists(archiveFolder)) {
            std::vector<FileInfo> files = GetFilesInDirectory(archiveFolder);
            for (const auto& file : files) {
                std::wstring lowerName = file.name;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::towlower);
                
                MergedFileInfo info;
                info.diskName = file.name;
                info.size = file.size;
                info.modFolder = modFolder;
                info.originalDataStart = 0;
                mergedFiles[lowerName] = info;
            }
        }
    }
    
    DWORD numFiles = (DWORD)mergedFiles.size();
    Log("[Loader] PopulateVirtualLgp: Found %d merged files across mods.\n", numFiles);

    state.isVirtualLgp = true;
    state.entries.resize(numFiles);

    // Initialize entries
    DWORD idx = 0;
    for (const auto& pair : mergedFiles) {
        std::wstring wName = pair.first;
        std::string name(wName.begin(), wName.end());

        LgpEntry entry;
        entry.name = name;
        entry.diskName = pair.second.diskName;
        entry.size = pair.second.size;
        entry.modFolder = pair.second.modFolder;
        entry.originalDataStart = pair.second.originalDataStart;
        state.entries[idx++] = entry;
    }

    // Sort entries by hash index ascending, and alphabetically within the same hash bucket
    std::sort(state.entries.begin(), state.entries.end(), [](const LgpEntry& a, const LgpEntry& b) {
        int hashA = FilenameToLookupIndex(a.name);
        int hashB = FilenameToLookupIndex(b.name);
        if (hashA != hashB) {
            return hashA < hashB;
        }
        return a.name < b.name;
    });

    // Build virtual LGP Header (16 bytes), TOC (numFiles * 27 bytes), Lookup Table (3600 bytes), Conflict Table (2 bytes)
    DWORD metadataSize = 16 + numFiles * 27 + 3600 + 2;
    state.virtualArchiveData.resize(metadataSize);
    
    // Creator: "\0\0SQUARESOFT" (12 bytes, right-aligned)
    const char creator[12] = { 0, 0, 'S', 'Q', 'U', 'A', 'R', 'E', 'S', 'O', 'F', 'T' };
    memcpy(state.virtualArchiveData.data(), creator, 12);
    
    // File count: numFiles (4 bytes)
    *(DWORD*)&state.virtualArchiveData[12] = numFiles;

    // Build TOC
    DWORD currentDataStart = metadataSize;
    for (DWORD i = 0; i < numFiles; ++i) {
        state.entries[i].dataStart = currentDataStart;

        // Populate TOC entry (27 bytes)
        BYTE* tocEntry = &state.virtualArchiveData[16 + i * 27];
        memset(tocEntry, 0, 27);
        memcpy(tocEntry, state.entries[i].name.c_str(), min(state.entries[i].name.size(), (size_t)20));
        *(DWORD*)&tocEntry[20] = currentDataStart;
        tocEntry[24] = 0x0E; // Standard LGP check code (14)

        currentDataStart += 24 + state.entries[i].size;
    }

    // Build Lookup (Hash) Table (3600 bytes) at offset (16 + numFiles * 27)
    BYTE* lookupTableStart = &state.virtualArchiveData[16 + numFiles * 27];
    memset(lookupTableStart, 0, 3600);
    
    std::vector<unsigned short> lookupIndex(900, 0);
    std::vector<unsigned short> lookupCount(900, 0);
    
    for (DWORD i = 0; i < numFiles; ++i) {
        int indexVal = FilenameToLookupIndex(state.entries[i].name); // 1-based index (1 to 900)
        if (indexVal >= 0 && indexVal < 900) {
            lookupCount[indexVal]++;
            if (lookupIndex[indexVal] == 0) {
                lookupIndex[indexVal] = (unsigned short)(i + 1); // 1-based TOC index
            }
        }
    }
    
    for (int i = 0; i < 900; ++i) {
        *(unsigned short*)&lookupTableStart[i * 4] = lookupIndex[i];
        *(unsigned short*)&lookupTableStart[i * 4 + 2] = lookupCount[i];
    }

    // Build Conflict Table (2 bytes of zero at the end of lookup table)
    BYTE* conflictTableStart = &state.virtualArchiveData[16 + numFiles * 27 + 3600];
    *(unsigned short*)conflictTableStart = 0; // 0 conflicts

    Log("[Loader] PopulateVirtualLgp completed successfully.\n");
}

std::unordered_map<FILE*, RedirectState> g_RedirectStates;
std::recursive_mutex g_Mutex;

// Original CRT Function Pointers
typedef FILE* (*fopen_t)(const char*, const char*);
typedef FILE* (*_wfopen_t)(const wchar_t*, const wchar_t*);
typedef size_t(*fread_t)(void*, size_t, size_t, FILE*);
typedef int(*fseek_t)(FILE*, long, int);
typedef int(*_fseeki64_t)(FILE*, __int64, int);
typedef int(*fclose_t)(FILE*);
typedef void(*clearerr_t)(FILE*);
typedef long(*ftell_t)(FILE*);
typedef __int64(*_ftelli64_t)(FILE*);
typedef int(*fgetpos_t)(FILE*, fpos_t*);
typedef int(*fsetpos_t)(FILE*, const fpos_t*);
typedef int(*fgetc_t)(FILE*);
typedef errno_t (__cdecl *_get_stream_buffer_pointers_t)(FILE*, char***, char***, int**);
typedef int (*ungetc_t)(int, FILE*);
typedef int (*_fileno_t)(FILE*);
typedef int (*fflush_t)(FILE*);

fopen_t OriginalFopen = nullptr;
_wfopen_t OriginalWfopen = nullptr;
fread_t OriginalFread = nullptr;
fseek_t OriginalFseek = nullptr;
_fseeki64_t OriginalFseeki64 = nullptr;
fclose_t OriginalFclose = nullptr;
clearerr_t OriginalClearerr = nullptr;
ftell_t OriginalFtell = nullptr;
_ftelli64_t OriginalFtelli64 = nullptr;
fgetpos_t OriginalFgetpos = nullptr;
fsetpos_t OriginalFsetpos = nullptr;
fgetc_t OriginalFgetc = nullptr;
_get_stream_buffer_pointers_t OriginalGetStreamBufferPointers = nullptr;
ungetc_t OriginalUngetc = nullptr;
_fileno_t OriginalFileno = nullptr;
fflush_t OriginalFflush = nullptr;



// Helper to check if a file exists
bool FileExists(const std::wstring& path) {
    DWORD dwAttrib = GetFileAttributesW(path.c_str());
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

std::wstring AnsiToWide(const std::string& str) {
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_ACP, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_ACP, 0, str.c_str(), (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

std::string WideToAnsi(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::wstring GetFullPathSafe(const std::wstring& path) {
    wchar_t absPath[4096];
    DWORD res = GetFullPathNameW(path.c_str(), 4096, absPath, NULL);
    if (res > 0 && res < 4096) {
        return std::wstring(absPath, res);
    }
    if (res >= 4096) {
        std::vector<wchar_t> buffer(res + 1);
        DWORD res2 = GetFullPathNameW(path.c_str(), (DWORD)buffer.size(), buffer.data(), NULL);
        if (res2 > 0 && res2 < buffer.size()) {
            return std::wstring(buffer.data(), res2);
        }
    }
    return path;
}

bool ContainsKeywordAnsi(const char* str) {
    if (!str) return false;
    std::string s = str;
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
    }
    return (s.find("ff7") != std::string::npos ||
            s.find("workingdir") != std::string::npos ||
            s.find(".lgp") != std::string::npos ||
            s.find("logo") != std::string::npos ||
            s.find("start") != std::string::npos);
}

bool ContainsKeywordWide(const wchar_t* str) {
    if (!str) return false;
    std::wstring s = str;
    for (wchar_t& c : s) {
        if (c >= L'A' && c <= L'Z') c += (L'a' - L'A');
    }
    return (s.find(L"ff7") != std::wstring::npos ||
            s.find(L"workingdir") != std::wstring::npos ||
            s.find(L".lgp") != std::wstring::npos ||
            s.find(L"logo") != std::wstring::npos ||
            s.find(L"start") != std::wstring::npos);
}


// Extract embedded RCDATA resource to a temporary file
std::wstring ExtractResourceToTempFile(int resourceID, const std::wstring& suffix) {
    HMODULE hMod = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&ExtractResourceToTempFile, &hMod);
    if (!hMod) {
        hMod = GetModuleHandleA("d3d11.dll");
    }
    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(resourceID), MAKEINTRESOURCEW(10));
    if (!hRes) {
        Log("[Loader] ERROR: FindResourceW failed for ID %d\n", resourceID);
        return L"";
    }

    HGLOBAL hGlob = LoadResource(hMod, hRes);
    DWORD size = SizeofResource(hMod, hRes);
    void* pData = LockResource(hGlob);
    if (!pData) {
        Log("[Loader] ERROR: LockResource failed for ID %d\n", resourceID);
        return L"";
    }

    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring tempFilePath = std::wstring(tempDir) + L"ffviise_temp_" + suffix;

    HANDLE hFile = CreateFileW(tempFilePath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD bytesWritten = 0;
        WriteFile(hFile, pData, size, &bytesWritten, NULL);
        CloseHandle(hFile);
        Log("[Loader] Successfully extracted resource %d to temp file: %S (%d bytes)\n", resourceID, tempFilePath.c_str(), size);
        return tempFilePath;
    } else {
        Log("[Loader] ERROR: CreateFileW failed for temp file: %S. Error: %d\n", tempFilePath.c_str(), GetLastError());
    }
    return L"";
}

// Forward declarations of Hooked APIs
FILE* HookedFopen(const char* filename, const char* mode);
FILE* HookedWfopen(const wchar_t* filename, const wchar_t* mode);
size_t HookedFread(void* buffer, size_t size, size_t count, FILE* stream);
int HookedFseek(FILE* stream, long offset, int origin);
int HookedFseeki64(FILE* stream, __int64 offset, int origin);
int HookedFclose(FILE* stream);
long HookedFtell(FILE* stream);
__int64 HookedFtelli64(FILE* stream);
int HookedFgetpos(FILE* stream, fpos_t* pos);
int HookedFsetpos(FILE* stream, const fpos_t* pos);
int HookedFgetc(FILE* stream);
errno_t __cdecl HookedGetStreamBufferPointers(FILE* stream, char*** base, char*** ptr, int** count);
int HookedUngetc(int c, FILE* stream);
int HookedFileno(FILE* stream);
int HookedFflush(FILE* stream);
void HookedClearerr(FILE* stream);


void PreInitializeLogging() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring exeStr = exePath;
    size_t exeSlash = exeStr.rfind(L'\\');
    std::wstring iniPath = (exeSlash != std::wstring::npos) ? exeStr.substr(0, exeSlash) + L"\\mods_loader.ini" : L"mods_loader.ini";
    g_BaseDir = (exeSlash != std::wstring::npos) ? exeStr.substr(0, exeSlash) : L"";

    // Read EnableLogging (true/false)
    wchar_t loggingStr[32] = L"true";
    GetPrivateProfileStringW(L"Loader", L"EnableLogging", L"true", loggingStr, 32, iniPath.c_str());
    std::wstring logStrLower = loggingStr;
    std::transform(logStrLower.begin(), logStrLower.end(), logStrLower.begin(), ::towlower);
    g_EnableLogging = (logStrLower == L"true");

    // Read ModsDirectory
    wchar_t modsDir[MAX_PATH] = L"mods";
    GetPrivateProfileStringW(L"Loader", L"ModsDirectory", L"mods", modsDir, MAX_PATH, iniPath.c_str());
    g_ModsDirectory = modsDir;

    // Read PluginsDirectory
    wchar_t pluginsDir[MAX_PATH] = L"plugins";
    GetPrivateProfileStringW(L"Loader", L"PluginsDirectory", L"plugins", pluginsDir, MAX_PATH, iniPath.c_str());
    g_PluginsDirectory = pluginsDir;

    // Read SplashScreens (default/custom/mods)
    wchar_t splashScreens[32] = L"custom";
    GetPrivateProfileStringW(L"Loader", L"SplashScreens", L"custom", splashScreens, 32, iniPath.c_str());
    g_SplashScreens = splashScreens;
    std::transform(g_SplashScreens.begin(), g_SplashScreens.end(), g_SplashScreens.begin(), ::towlower);

    // Read LogFile
    wchar_t logFile[MAX_PATH] = L"mods_loader_log.txt";
    GetPrivateProfileStringW(L"Loader", L"LogFile", L"mods_loader_log.txt", logFile, MAX_PATH, iniPath.c_str());
    g_LogFileName = logFile;

    // Resolve absolute log path
    g_LogPath = (exeSlash != std::wstring::npos) ? exeStr.substr(0, exeSlash) + L"\\" + g_LogFileName : g_LogFileName;
    g_IniPath = iniPath;

    // Read EnableTextureLogging (true/false)
    wchar_t texLoggingStr[32] = L"false";
    GetPrivateProfileStringW(L"Loader", L"EnableTextureLogging", L"false", texLoggingStr, 32, iniPath.c_str());
    std::wstring texLogStrLower = texLoggingStr;
    std::transform(texLogStrLower.begin(), texLogStrLower.end(), texLogStrLower.begin(), ::towlower);
    g_EnableTextureLogging = (texLogStrLower == L"true");

    // Read TextureLogFile
    wchar_t texLogFile[MAX_PATH] = L"d3d11_texture_log.txt";
    GetPrivateProfileStringW(L"Loader", L"TextureLogFile", L"d3d11_texture_log.txt", texLogFile, MAX_PATH, iniPath.c_str());
    g_TextureLogFileName = texLogFile;
    g_TextureLogPath = (exeSlash != std::wstring::npos) ? exeStr.substr(0, exeSlash) + L"\\" + g_TextureLogFileName : g_TextureLogFileName;

    // Read DisableD3D11Hooks
    wchar_t disableHooksStr[32] = L"false";
    GetPrivateProfileStringW(L"Loader", L"DisableD3D11Hooks", L"false", disableHooksStr, 32, iniPath.c_str());
    std::wstring disableHooksLower = disableHooksStr;
    std::transform(disableHooksLower.begin(), disableHooksLower.end(), disableHooksLower.begin(), ::towlower);
    g_DisableD3D11Hooks = (disableHooksLower == L"true");

    if (g_EnableLogging) {
        FILE* f = NULL;
        _wfopen_s(&f, g_LogPath.c_str(), L"w");
        if (f) {
            fprintf(f, "[Loader] === Log Initialized (Fresh Start) ===\n");
            fclose(f);
        }
    }

    if (g_EnableTextureLogging) {
        FILE* fTex = NULL;
        _wfopen_s(&fTex, g_TextureLogPath.c_str(), L"w");
        if (fTex) fclose(fTex);
    }
}

void LoadConfiguration() {
    // Load active mods order
    void LoadModsLoadOrder();
    LoadModsLoadOrder();
}

void LoadModsLoadOrder() {
    g_ActiveMods.clear();
    std::wstring modsPath = g_BaseDir + L"\\" + g_ModsDirectory;
    if (!DirectoryExists(modsPath)) {
        Log("[Loader] Mods directory does not exist: %S\n", modsPath.c_str());
        return;
    }

    std::wstring loadOrderFilePath = modsPath + L"\\load_order.txt";
    std::vector<std::wstring> discovered = GetSubdirectories(modsPath);
    
    if (!FileExists(loadOrderFilePath)) {
        Log("[Loader] load_order.txt not found. Creating and populating in alphabetical order...\n");
        // Sort discovered subdirectories alphabetically
        std::sort(discovered.begin(), discovered.end());
        
        FILE* f = NULL;
        _wfopen_s(&f, loadOrderFilePath.c_str(), L"w, ccs=UTF-8");
        if (f) {
            for (const auto& mod : discovered) {
                fwprintf(f, L"%s\n", mod.c_str());
                g_ActiveMods.push_back(mod);
                Log("[Loader] Discovered and auto-added mod to load order: %S\n", mod.c_str());
            }
            fclose(f);
        }
    } else {
        Log("[Loader] Reading load order from: %S\n", loadOrderFilePath.c_str());
        FILE* f = NULL;
        _wfopen_s(&f, loadOrderFilePath.c_str(), L"r, ccs=UTF-8");
        if (!f) {
            _wfopen_s(&f, loadOrderFilePath.c_str(), L"r, ccs=UNICODE");
        }
        if (!f) {
            _wfopen_s(&f, loadOrderFilePath.c_str(), L"r"); // fallback to ANSI
        }
        
        std::vector<std::wstring> existingMods;
        if (f) {
            wchar_t line[MAX_PATH];
            while (fgetws(line, MAX_PATH, f)) {
                std::wstring modName = line;
                // Trim whitespace and newlines
                modName.erase(modName.find_last_not_of(L" \t\r\n") + 1);
                modName.erase(0, modName.find_first_not_of(L" \t\r\n"));
                
                if (!modName.empty() && modName[0] != L';') { // allow comments starting with ';'
                    std::wstring fullModPath = modsPath + L"\\" + modName;
                    if (DirectoryExists(fullModPath)) {
                        existingMods.push_back(modName);
                        g_ActiveMods.push_back(modName);
                        Log("[Loader] Active mod loaded: %S\n", modName.c_str());
                    } else {
                        Log("[Loader] WARNING: Mod folder specified in load_order.txt does not exist: %S\n", modName.c_str());
                    }
                }
            }
            fclose(f);
        }
        
        // Find new mods that are not in the existing load_order.txt list
        std::vector<std::wstring> newMods;
        for (const auto& discMod : discovered) {
            bool found = false;
            for (const auto& existMod : existingMods) {
                if (_wcsicmp(discMod.c_str(), existMod.c_str()) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                newMods.push_back(discMod);
            }
        }
        
        if (!newMods.empty()) {
            std::sort(newMods.begin(), newMods.end());
            Log("[Loader] Found %d new mods not in load_order.txt. Appending to bottom...\n", (int)newMods.size());
            
            // Re-open in append mode
            FILE* fAppend = NULL;
            _wfopen_s(&fAppend, loadOrderFilePath.c_str(), L"a, ccs=UTF-8");
            if (!fAppend) {
                _wfopen_s(&fAppend, loadOrderFilePath.c_str(), L"a");
            }
            
            if (fAppend) {
                for (const auto& mod : newMods) {
                    fwprintf(fAppend, L"%s\n", mod.c_str());
                    g_ActiveMods.push_back(mod);
                    Log("[Loader] Appended new mod to load order: %S\n", mod.c_str());
                }
                fclose(fAppend);
            }
        }
    }
}

void GetModuleAndOffset(void* address, wchar_t* outModuleName, DWORD& outOffset) {
    outModuleName[0] = L'\0';
    outOffset = 0;

    HMODULE hModule = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)address, &hModule)) {
        wchar_t modulePath[MAX_PATH];
        if (GetModuleFileNameW(hModule, modulePath, MAX_PATH)) {
            std::wstring pathStr = modulePath;
            size_t slashPos = pathStr.rfind(L'\\');
            std::wstring name = (slashPos != std::wstring::npos) ? pathStr.substr(slashPos + 1) : pathStr;
            wcscpy_s(outModuleName, MAX_PATH, name.c_str());
            outOffset = (DWORD)((BYTE*)address - (BYTE*)hModule);
        }
    }
}

typedef BOOL(WINAPI* MiniDumpWriteDump_t)(
    HANDLE hProcess,
    DWORD ProcessId,
    HANDLE hFile,
    MINIDUMP_TYPE DumpType,
    PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
    PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
    PMINIDUMP_CALLBACK_INFORMATION CallbackParam
);

void WriteMinidump(EXCEPTION_POINTERS* exceptionInfo, const std::wstring& dumpPath) {
    HMODULE hDbgHelp = LoadLibraryW(L"dbghelp.dll");
    if (hDbgHelp) {
        MiniDumpWriteDump_t pfnMiniDumpWriteDump = (MiniDumpWriteDump_t)GetProcAddress(hDbgHelp, "MiniDumpWriteDump");
        if (pfnMiniDumpWriteDump) {
            HANDLE hFile = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei;
                mei.ThreadId = GetCurrentThreadId();
                mei.ExceptionPointers = exceptionInfo;
                mei.ClientPointers = TRUE;

                pfnMiniDumpWriteDump(
                    GetCurrentProcess(),
                    GetCurrentProcessId(),
                    hFile,
                    MiniDumpNormal,
                    &mei,
                    NULL,
                    NULL
                );
                CloseHandle(hFile);
            }
        }
        FreeLibrary(hDbgHelp);
    }
}

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* exceptionInfo) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring exeStr = exePath;
    size_t exeSlash = exeStr.rfind(L'\\');
    std::wstring baseDir = (exeSlash != std::wstring::npos) ? exeStr.substr(0, exeSlash) : L"";

    std::wstring logPath = baseDir + L"\\mods_loader_crash.txt";
    std::wstring dumpPath = baseDir + L"\\mods_loader_crash.dmp";

    FILE* f = NULL;
    _wfopen_s(&f, logPath.c_str(), L"w");
    if (f) {
        DWORD exceptionCode = exceptionInfo->ExceptionRecord->ExceptionCode;
        void* exceptionAddress = exceptionInfo->ExceptionRecord->ExceptionAddress;

        wchar_t moduleName[MAX_PATH];
        DWORD offset = 0;
        GetModuleAndOffset(exceptionAddress, moduleName, offset);

        fprintf(f, "==================================================\n");
        fprintf(f, "           MOD LOADER CRASH REPORT                \n");
        fprintf(f, "==================================================\n\n");
        fprintf(f, "Exception Code:    0x%08X\n", exceptionCode);
        fprintf(f, "Exception Address: %p\n", exceptionAddress);
        if (moduleName[0] != L'\0') {
            fprintf(f, "Faulting Module:   %S\n", moduleName);
            fprintf(f, "Relative Offset:   0x%X\n\n", offset);
        } else {
            fprintf(f, "Faulting Module:   Unknown\n\n");
        }

#ifdef _M_X64
        fprintf(f, "Registers (x64):\n");
        fprintf(f, "RAX: 0x%016I64X   RBX: 0x%016I64X   RCX: 0x%016I64X\n", exceptionInfo->ContextRecord->Rax, exceptionInfo->ContextRecord->Rbx, exceptionInfo->ContextRecord->Rcx);
        fprintf(f, "RDX: 0x%016I64X   RSI: 0x%016I64X   RDI: 0x%016I64X\n", exceptionInfo->ContextRecord->Rdx, exceptionInfo->ContextRecord->Rsi, exceptionInfo->ContextRecord->Rdi);
        fprintf(f, "RBP: 0x%016I64X   RSP: 0x%016I64X   RIP: 0x%016I64X\n", exceptionInfo->ContextRecord->Rbp, exceptionInfo->ContextRecord->Rsp, exceptionInfo->ContextRecord->Rip);
        fprintf(f, "R8:  0x%016I64X   R9:  0x%016I64X   R10: 0x%016I64X\n", exceptionInfo->ContextRecord->R8,  exceptionInfo->ContextRecord->R9,  exceptionInfo->ContextRecord->R10);
        fprintf(f, "R11: 0x%016I64X   R12: 0x%016I64X   R13: 0x%016I64X\n", exceptionInfo->ContextRecord->R11, exceptionInfo->ContextRecord->R12, exceptionInfo->ContextRecord->R13);
        fprintf(f, "R14: 0x%016I64X   R15: 0x%016I64X\n", exceptionInfo->ContextRecord->R14, exceptionInfo->ContextRecord->R15);
#else
        fprintf(f, "Registers (x86):\n");
        fprintf(f, "EAX: 0x%08X   EBX: 0x%08X   ECX: 0x%08X\n", exceptionInfo->ContextRecord->Eax, exceptionInfo->ContextRecord->Ebx, exceptionInfo->ContextRecord->Ecx);
        fprintf(f, "EDX: 0x%08X   ESI: 0x%08X   EDI: 0x%08X\n", exceptionInfo->ContextRecord->Edx, exceptionInfo->ContextRecord->Esi, exceptionInfo->ContextRecord->Edi);
        fprintf(f, "EBP: 0x%08X   ESP: 0x%08X   EIP: 0x%08X\n", exceptionInfo->ContextRecord->Ebp, exceptionInfo->ContextRecord->Esp, exceptionInfo->ContextRecord->Eip);
#endif
        fclose(f);
    }

    WriteMinidump(exceptionInfo, dumpPath);
    return EXCEPTION_CONTINUE_SEARCH;
}

void LoadPlugins() {
    g_ActivePlugins.clear();
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring exeStr = exePath;
    size_t exeSlash = exeStr.rfind(L'\\');
    std::wstring baseDir = (exeSlash != std::wstring::npos) ? exeStr.substr(0, exeSlash) : L"";

    std::wstring pluginsPath = baseDir + L"\\" + g_PluginsDirectory;
    if (!DirectoryExists(pluginsPath)) {
        Log("[Plugins] Directory does not exist: %S\n", pluginsPath.c_str());
        return;
    }

    Log("[Plugins] Scanning for plugins in: %S\n", pluginsPath.c_str());
    std::wstring searchPath = pluginsPath + L"\\*.dll";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::wstring dllName = fd.cFileName;
            std::wstring fullPath = pluginsPath + L"\\" + dllName;
            
            Log("[Plugins] Found plugin: %S. Loading...\n", dllName.c_str());
            HMODULE hPlugin = LoadLibraryW(fullPath.c_str());
            if (hPlugin) {
                Log("[Plugins] Successfully loaded plugin: %S (Base: %p)\n", dllName.c_str(), hPlugin);
                g_ActivePlugins.push_back(dllName);
            } else {
                DWORD error = GetLastError();
                Log("[Plugins] Failed to load plugin: %S. Error code: %d\n", dllName.c_str(), error);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    } else {
        Log("[Plugins] No plugins found.\n");
    }
}

void InitializeHooks() {
    PreInitializeLogging();
    LoadConfiguration();
    
    Log("[Loader] Initializing hooks via MinHook...\n");
    SetUnhandledExceptionFilter(CrashHandler);

    if (MH_Initialize() != MH_OK) {
        Log("[Loader] MH_Initialize failed!\n");
        return;
    }

    HMODULE hUcrt = GetModuleHandleW(L"ucrtbase.dll");
    if (!hUcrt) {
        hUcrt = LoadLibraryW(L"ucrtbase.dll");
    }

    if (hUcrt) {
        fopen_t targetFopen = (fopen_t)GetProcAddress(hUcrt, "fopen");
        _wfopen_t targetWfopen = (_wfopen_t)GetProcAddress(hUcrt, "_wfopen");
        fread_t targetFread = (fread_t)GetProcAddress(hUcrt, "fread");
        fseek_t targetFseek = (fseek_t)GetProcAddress(hUcrt, "fseek");
        _fseeki64_t targetFseeki64 = (_fseeki64_t)GetProcAddress(hUcrt, "_fseeki64");
        fclose_t targetFclose = (fclose_t)GetProcAddress(hUcrt, "fclose");
        OriginalClearerr = (clearerr_t)GetProcAddress(hUcrt, "clearerr");
        ftell_t targetFtell = (ftell_t)GetProcAddress(hUcrt, "ftell");
        _ftelli64_t targetFtelli64 = (_ftelli64_t)GetProcAddress(hUcrt, "_ftelli64");
        fgetpos_t targetFgetpos = (fgetpos_t)GetProcAddress(hUcrt, "fgetpos");
        fsetpos_t targetFsetpos = (fsetpos_t)GetProcAddress(hUcrt, "fsetpos");
        fgetc_t targetFgetc = (fgetc_t)GetProcAddress(hUcrt, "fgetc");
        _get_stream_buffer_pointers_t targetGetStreamBufferPointers = (_get_stream_buffer_pointers_t)GetProcAddress(hUcrt, "_get_stream_buffer_pointers");
        ungetc_t targetUngetc = (ungetc_t)GetProcAddress(hUcrt, "ungetc");
        _fileno_t targetFileno = (_fileno_t)GetProcAddress(hUcrt, "_fileno");
        fflush_t targetFflush = (fflush_t)GetProcAddress(hUcrt, "fflush");

        if (targetFopen) MH_CreateHook(targetFopen, (LPVOID)&HookedFopen, (LPVOID*)&OriginalFopen);
        if (targetWfopen) MH_CreateHook(targetWfopen, (LPVOID)&HookedWfopen, (LPVOID*)&OriginalWfopen);
        if (targetFread) MH_CreateHook(targetFread, (LPVOID)&HookedFread, (LPVOID*)&OriginalFread);
        if (targetFseek) MH_CreateHook(targetFseek, (LPVOID)&HookedFseek, (LPVOID*)&OriginalFseek);
        if (targetFseeki64) MH_CreateHook(targetFseeki64, (LPVOID)&HookedFseeki64, (LPVOID*)&OriginalFseeki64);
        if (targetFclose) MH_CreateHook(targetFclose, (LPVOID)&HookedFclose, (LPVOID*)&OriginalFclose);
        if (OriginalClearerr) MH_CreateHook(OriginalClearerr, (LPVOID)&HookedClearerr, (LPVOID*)&OriginalClearerr);
        if (targetFtell) MH_CreateHook(targetFtell, (LPVOID)&HookedFtell, (LPVOID*)&OriginalFtell);
        if (targetFtelli64) MH_CreateHook(targetFtelli64, (LPVOID)&HookedFtelli64, (LPVOID*)&OriginalFtelli64);
        if (targetFgetpos) MH_CreateHook(targetFgetpos, (LPVOID)&HookedFgetpos, (LPVOID*)&OriginalFgetpos);
        if (targetFsetpos) MH_CreateHook(targetFsetpos, (LPVOID)&HookedFsetpos, (LPVOID*)&OriginalFsetpos);
        if (targetFgetc) MH_CreateHook(targetFgetc, (LPVOID)&HookedFgetc, (LPVOID*)&OriginalFgetc);
        if (targetGetStreamBufferPointers) MH_CreateHook(targetGetStreamBufferPointers, (LPVOID)&HookedGetStreamBufferPointers, (LPVOID*)&OriginalGetStreamBufferPointers);
        if (targetUngetc) MH_CreateHook(targetUngetc, (LPVOID)&HookedUngetc, (LPVOID*)&OriginalUngetc);
        if (targetFileno) MH_CreateHook(targetFileno, (LPVOID)&HookedFileno, (LPVOID*)&OriginalFileno);
        if (targetFflush) MH_CreateHook(targetFflush, (LPVOID)&HookedFflush, (LPVOID*)&OriginalFflush);

        // Hook D3D11CreateDeviceAndSwapChain to intercept the game's actual SwapChain creation
        Log("[Loader] Hooking D3D11CreateDeviceAndSwapChain...\n");
        HMODULE hD3D11 = GetModuleHandleW(L"d3d11.dll");
        if (!hD3D11) {
            hD3D11 = LoadLibraryW(L"d3d11.dll");
        }
        if (hD3D11) {
            typedef HRESULT(WINAPI* D3D11CreateDeviceAndSwapChain_t)(
                IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                const D3D_FEATURE_LEVEL*, UINT, UINT,
                const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
                ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**
            );
            void* targetCreate = (void*)GetProcAddress(hD3D11, "D3D11CreateDeviceAndSwapChain");
            if (targetCreate) {
                MH_STATUS status = MH_CreateHook(targetCreate, (LPVOID)&HookedD3D11CreateDeviceAndSwapChain, (LPVOID*)&OriginalD3D11CreateDeviceAndSwapChain);
                if (status == MH_OK) {
                    Log("[Loader] Hooked D3D11CreateDeviceAndSwapChain export successfully.\n");
                } else {
                    Log("[Loader] Failed to hook D3D11CreateDeviceAndSwapChain. Status: %d\n", status);
                }
            } else {
                Log("[Loader] Failed to locate D3D11CreateDeviceAndSwapChain export in d3d11.dll\n");
            }
        } else {
            Log("[Loader] Failed to load/locate d3d11.dll\n");
        }



        MH_STATUS enableStatus = MH_EnableHook(MH_ALL_HOOKS);
        if (enableStatus == MH_OK) {
            Log("[Loader] Hooks initialized successfully via MinHook detour! ucrtbase: %p\n", hUcrt);
        } else {
            Log("[Loader] MH_EnableHook failed! Status: %d\n", enableStatus);
        }
        
        LoadPlugins();
    } else {
        Log("[Loader] Failed to locate ucrtbase.dll!\n");
    }
}

std::wstring ResolveModPath(const std::wstring& relativePath);

// Internal LGP Parser helper
void ParseLgpTOC(FILE* f, RedirectState& state) {
    // LGP Header is 16 bytes: 12 bytes creator, 4 bytes file count
    BYTE header[16];
    OriginalFseek(f, 0, SEEK_SET);
    if (OriginalFread(header, 1, 16, f) < 16) return;

    DWORD numFiles = *(DWORD*)&header[12];
    state.entries.resize(numFiles);

    // Read Table of Contents (TOC) - each entry is 27 bytes
    std::vector<BYTE> tocBuffer(numFiles * 27);
    if (OriginalFread(tocBuffer.data(), 1, numFiles * 27, f) < numFiles * 27) return;

    for (DWORD i = 0; i < numFiles; ++i) {
        BYTE* entryPtr = &tocBuffer[i * 27];
        char name[21] = { 0 };
        memcpy(name, entryPtr, 20);
        
        LgpEntry entry;
        entry.name = name;
        entry.diskName = std::wstring(entry.name.begin(), entry.name.end());
        entry.dataStart = *(DWORD*)&entryPtr[20];
        
        // Get the file size by seeking to dataStart and reading size
        OriginalFseek(f, entry.dataStart + 20, SEEK_SET);
        DWORD sizeVal = 0;
        OriginalFread(&sizeVal, 1, 4, f);
        entry.size = sizeVal;

        state.entries[i] = entry;
    }
}

std::wstring GetFilenameFromPath(const std::wstring& path) {
    size_t lastSlash = path.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        return path.substr(lastSlash + 1);
    }
    return path;
}

std::wstring ResolveModPath(const std::wstring& relativePath) {
    if (relativePath.empty()) return L"";
    for (const auto& modFolder : g_ActiveMods) {
        std::wstring checkPath = g_BaseDir + L"\\" + g_ModsDirectory + L"\\" + modFolder + L"\\" + relativePath;
        if (FileExists(checkPath)) {
            return checkPath;
        }
    }
    return L"";
}

std::wstring GetTempPathForArchive(const std::wstring& gameDir, const std::wstring& archiveRelPath) {
    std::wstring safeName = archiveRelPath;
    for (wchar_t& c : safeName) {
        if (c == L'\\' || c == L'/') {
            c = L'_';
        }
    }
    return gameDir + L"\\" + g_ModsDirectory + L"\\" + safeName + L".tmp";
}

std::wstring ResolveModDirectoryPath(const std::wstring& relativePath) {
    if (relativePath.empty()) return L"";
    for (const auto& modFolder : g_ActiveMods) {
        std::wstring checkPath = g_BaseDir + L"\\" + g_ModsDirectory + L"\\" + modFolder + L"\\" + relativePath;
        if (DirectoryExists(checkPath)) {
            return checkPath;
        }
    }
    return L"";
}

std::wstring GetSplashOverridePath(const std::wstring& pathStr, const std::wstring& originalPath, int resourceID, const std::wstring& tempSuffix) {
    if (g_SplashScreens == L"custom") {
        // Always load custom built-in resource directly
        std::wstring tempFile = ExtractResourceToTempFile(resourceID, tempSuffix);
        if (!tempFile.empty()) {
            Log("[Loader] Splash redirection using custom embedded resource: %S\n", tempFile.c_str());
            return tempFile;
        }
        return L"";
    }

    // Otherwise, SplashScreens == "default". Check mods folder first
    std::wstring filename = GetFilenameFromPath(originalPath);
    
    // Try to find relative path from base directory
    std::wstring relativePath = L"";
    if (!g_BaseDir.empty() && originalPath.size() > g_BaseDir.size() &&
        _wcsnicmp(originalPath.c_str(), g_BaseDir.c_str(), g_BaseDir.size()) == 0) {
        relativePath = originalPath.substr(g_BaseDir.size());
        if (!relativePath.empty() && (relativePath[0] == L'\\' || relativePath[0] == L'/')) {
            relativePath = relativePath.substr(1);
        }
    }
    
    // 1. Check mirrored path in mods: mods/relative_path
    if (!relativePath.empty()) {
        std::wstring mirrorPath = ResolveModPath(relativePath);
        if (!mirrorPath.empty()) {
            Log("[Loader] Splash redirection found in mods (mirrored): %S\n", mirrorPath.c_str());
            return mirrorPath;
        }
    }
    
    // 2. Check flat path in mods: mods/filename
    std::wstring flatPath = ResolveModPath(filename);
    if (!flatPath.empty()) {
        Log("[Loader] Splash redirection found in mods (flat): %S\n", flatPath.c_str());
        return flatPath;
    }
    
    // If not found in mods folder, fall back to default game image (no redirection)
    Log("[Loader] Splash override not found in mods folder. Using default game image: %S\n", originalPath.c_str());
    return L"";
}

// Hooked fopen
FILE* HookedFopen(const char* filename, const char* mode) {
    if (filename && !ContainsKeywordAnsi(filename)) {
        return OriginalFopen(filename, mode);
    }

    if (filename) {
        std::wstring wFilename = AnsiToWide(filename);
        std::wstring absPath = GetFullPathSafe(wFilename);
        if (!absPath.empty()) {
            std::wstring pathStr = absPath;
            std::replace(pathStr.begin(), pathStr.end(), L'/', L'\\');
            std::wstring originalPath = pathStr;
            std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::towlower);

            // Track texture filename if opening a loose .tex file
            if (pathStr.find(L".tex") != std::wstring::npos) {
                size_t lastSlash = pathStr.rfind(L'\\');
                if (lastSlash != std::wstring::npos) {
                    g_LastLoadedTexFile = pathStr.substr(lastSlash + 1);
                } else {
                    g_LastLoadedTexFile = pathStr;
                }
            }

            std::wstring splashOverride = L"";
            if (pathStr.find(L"dotemu-logo.png") != std::wstring::npos) {
                splashOverride = GetSplashOverridePath(pathStr, originalPath, 101, L"dotemu.png");
            } else if (pathStr.find(L"finelogo.png") != std::wstring::npos) {
                splashOverride = GetSplashOverridePath(pathStr, originalPath, 102, L"finelogo.png");
            } else if (pathStr.find(L"press_start.png") != std::wstring::npos) {
                splashOverride = GetSplashOverridePath(pathStr, originalPath, 103, L"press_start.png");
            }

            if (!splashOverride.empty()) {
                std::string cTempFile = WideToAnsi(splashOverride);
                Log("[Loader] Redirecting fopen: %s -> %s\n", filename, cTempFile.c_str());
                FILE* fRedirect = OriginalFopen(cTempFile.c_str(), mode);
                Log("[Loader] Redirecting fopen returned: %p\n", fRedirect);
                return fRedirect;
            }

            size_t dataPos = pathStr.find(L"\\ff7\\workingdir\\data\\");
            if (dataPos != std::wstring::npos) {
                std::wstring relPath = originalPath.substr(dataPos + 21);
                std::wstring overridePath = ResolveModPath(relPath);
                if (!overridePath.empty()) {
                    std::string cOverridePath = WideToAnsi(overridePath);
                    Log("[Loader] Redirecting fopen: %s -> %s\n", filename, cOverridePath.c_str());
                    FILE* fRedirect = OriginalFopen(cOverridePath.c_str(), mode);
                    Log("[Loader] Redirecting fopen returned: %p\n", fRedirect);
                    return fRedirect;
                }
            }
        }
    }
    
    Log("[Loader] fopen called: %s (mode: %s)\n", filename ? filename : "NULL", mode ? mode : "NULL");
    FILE* f = OriginalFopen(filename, mode);
    if (filename) {
        std::string pathStr = filename;
        std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::tolower);

        if (pathStr.find(".lgp") != std::string::npos) {
            std::wstring wFilename = AnsiToWide(filename);
            std::wstring absPath = GetFullPathSafe(wFilename);
            if (!absPath.empty()) {
                std::wstring baseDir = absPath;
                std::replace(baseDir.begin(), baseDir.end(), L'/', L'\\');
                std::wstring archiveRelPath = L"";
                size_t dataPosInArchive = baseDir.find(L"\\ff7\\workingdir\\data\\");
                if (dataPosInArchive != std::wstring::npos) {
                    archiveRelPath = baseDir.substr(dataPosInArchive + 21);
                } else {
                    size_t lastSlash = baseDir.rfind(L'\\');
                    archiveRelPath = (lastSlash != std::wstring::npos) ? baseDir.substr(lastSlash + 1) : baseDir;
                }
                size_t dotPos = archiveRelPath.rfind(L'.');
                if (dotPos != std::wstring::npos) archiveRelPath = archiveRelPath.substr(0, dotPos);

                std::wstring archiveFolder = ResolveModDirectoryPath(archiveRelPath);
                if (!archiveFolder.empty()) {
                    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
                    
                    // Close the original physical LGP file handle first if we opened it
                    if (f) {
                        OriginalFclose(f);
                        f = nullptr;
                    }
                    
                    std::wstring gameDir = L"";
                    size_t dataPos = baseDir.find(L"\\ff7\\workingdir");
                    if (dataPos != std::wstring::npos) {
                        gameDir = baseDir.substr(0, dataPos);
                    } else {
                        wchar_t exePath[MAX_PATH];
                        GetModuleFileNameW(NULL, exePath, MAX_PATH);
                        std::wstring exeStr = exePath;
                        size_t exeSlash = exeStr.rfind(L'\\');
                        if (exeSlash != std::wstring::npos) gameDir = exeStr.substr(0, exeSlash);
                    }

                    RedirectState state;
                    state.archivePath = baseDir;
                    state.isLgp = true;
                    PopulateVirtualLgp(archiveRelPath, state);
                    
                    DWORD totalVirtualSize = (DWORD)state.virtualArchiveData.size();
                    for (const auto& entry : state.entries) {
                        totalVirtualSize += 24 + entry.size;
                    }
                    totalVirtualSize += 14; // footer
                    
                    std::wstring tempPath = GetTempPathForArchive(gameDir, archiveRelPath);
                    Log("[Loader] Creating temp file via Win32: %S\n", tempPath.c_str());
                    HANDLE hFile = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hFile != INVALID_HANDLE_VALUE) {
                        DWORD bytesWritten = 0;
                        BOOL writeRes = WriteFile(hFile, state.virtualArchiveData.data(), (DWORD)state.virtualArchiveData.size(), &bytesWritten, NULL);
                        if (writeRes && bytesWritten == state.virtualArchiveData.size()) {
                            LARGE_INTEGER li;
                            li.QuadPart = totalVirtualSize;
                            if (SetFilePointerEx(hFile, li, NULL, FILE_BEGIN)) {
                                if (SetEndOfFile(hFile)) {
                                    LARGE_INTEGER footerOffset;
                                    footerOffset.QuadPart = totalVirtualSize - 14;
                                    if (SetFilePointerEx(hFile, footerOffset, NULL, FILE_BEGIN)) {
                                        DWORD footerWritten = 0;
                                        WriteFile(hFile, "FINAL FANTASY7", 14, &footerWritten, NULL);
                                    }
                                }
                            }
                        }
                        CloseHandle(hFile);
                    }
                    
                    f = OriginalWfopen(tempPath.c_str(), L"rb");
                    if (f) {
                        Log("[Loader] Faking merged LGP archive '%s' using folder '%S' (size: %d, tempFile: %S)\n", filename, archiveFolder.c_str(), totalVirtualSize, tempPath.c_str());
                        state.tempFilePath = tempPath;
                        
                        // Open the physical LGP archive stream for fallbacks
                        std::wstring physicalLgpPath = gameDir + L"\\ff7\\workingdir\\data\\" + archiveRelPath + L".lgp";
                        state.physicalLgpFile = OriginalWfopen(physicalLgpPath.c_str(), L"rb");
                        
                        g_RedirectStates[f] = state;
                    }
                } else {
                    // No mod overrides, parse standard TOC
                    if (f) {
                        std::lock_guard<std::recursive_mutex> lock(g_Mutex);
                        if (g_RedirectStates.find(f) == g_RedirectStates.end()) {
                            RedirectState state;
                            state.archivePath = baseDir;
                            state.isLgp = true;
                            ParseLgpTOC(f, state);
                            OriginalFseek(f, 0, SEEK_SET);
                            g_RedirectStates[f] = state;
                        }
                    }
                }
            }
        }
    }
    Log("[Loader] fopen returned: %p for %s\n", f, filename ? filename : "NULL");
    return f;
}


// Hooked _wfopen
FILE* HookedWfopen(const wchar_t* filename, const wchar_t* mode) {
    if (filename && !ContainsKeywordWide(filename)) {
        return OriginalWfopen(filename, mode);
    }

    if (filename) {
        std::wstring absPath = GetFullPathSafe(filename);
        if (!absPath.empty()) {
            std::wstring pathStr = absPath;
            std::replace(pathStr.begin(), pathStr.end(), L'/', L'\\');
            std::wstring originalPath = pathStr;
            std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::towlower);

            // Track texture filename if opening a loose .tex file
            if (pathStr.find(L".tex") != std::wstring::npos) {
                size_t lastSlash = pathStr.rfind(L'\\');
                if (lastSlash != std::wstring::npos) {
                    g_LastLoadedTexFile = pathStr.substr(lastSlash + 1);
                } else {
                    g_LastLoadedTexFile = pathStr;
                }
            }

            std::wstring splashOverride = L"";
            if (pathStr.find(L"dotemu-logo.png") != std::wstring::npos) {
                splashOverride = GetSplashOverridePath(pathStr, originalPath, 101, L"dotemu.png");
            } else if (pathStr.find(L"finelogo.png") != std::wstring::npos) {
                splashOverride = GetSplashOverridePath(pathStr, originalPath, 102, L"finelogo.png");
            } else if (pathStr.find(L"press_start.png") != std::wstring::npos) {
                splashOverride = GetSplashOverridePath(pathStr, originalPath, 103, L"press_start.png");
            }

            if (!splashOverride.empty()) {
                Log("[Loader] Redirecting _wfopen: %S -> %S\n", filename, splashOverride.c_str());
                FILE* fRedirect = OriginalWfopen(splashOverride.c_str(), mode);
                Log("[Loader] Redirecting _wfopen returned: %p\n", fRedirect);
                return fRedirect;
            }

            size_t dataPos = pathStr.find(L"\\ff7\\workingdir\\data\\");
            if (dataPos != std::wstring::npos) {
                std::wstring relPath = originalPath.substr(dataPos + 21);
                std::wstring overridePath = ResolveModPath(relPath);
                if (!overridePath.empty()) {
                    Log("[Loader] Redirecting _wfopen: %S -> %S\n", filename, overridePath.c_str());
                    FILE* fRedirect = OriginalWfopen(overridePath.c_str(), mode);
                    Log("[Loader] Redirecting _wfopen returned: %p\n", fRedirect);
                    return fRedirect;
                }
            }
        }
    }

    Log("[Loader] _wfopen called: %S (mode: %S)\n", filename ? filename : L"NULL", mode ? mode : L"NULL");
    FILE* f = OriginalWfopen(filename, mode);
    if (filename) {
        std::wstring pathStr = filename;
        std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::towlower);

        if (pathStr.find(L".lgp") != std::wstring::npos) {
            std::wstring absPath = GetFullPathSafe(filename);
            if (!absPath.empty()) {
                std::wstring baseDir = absPath;
                std::replace(baseDir.begin(), baseDir.end(), L'/', L'\\');
                std::wstring archiveRelPath = L"";
                size_t dataPosInArchive = baseDir.find(L"\\ff7\\workingdir\\data\\");
                if (dataPosInArchive != std::wstring::npos) {
                    archiveRelPath = baseDir.substr(dataPosInArchive + 21);
                } else {
                    size_t lastSlash = baseDir.rfind(L'\\');
                    archiveRelPath = (lastSlash != std::wstring::npos) ? baseDir.substr(lastSlash + 1) : baseDir;
                }
                size_t dotPos = archiveRelPath.rfind(L'.');
                if (dotPos != std::wstring::npos) archiveRelPath = archiveRelPath.substr(0, dotPos);

                std::wstring archiveFolder = ResolveModDirectoryPath(archiveRelPath);
                if (!archiveFolder.empty()) {
                    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
                    
                    // Close the original physical LGP file handle first if we opened it
                    if (f) {
                        OriginalFclose(f);
                        f = nullptr;
                    }
                    
                    std::wstring gameDir = L"";
                    size_t dataPos = baseDir.find(L"\\ff7\\workingdir");
                    if (dataPos != std::wstring::npos) {
                        gameDir = baseDir.substr(0, dataPos);
                    } else {
                        wchar_t exePath[MAX_PATH];
                        GetModuleFileNameW(NULL, exePath, MAX_PATH);
                        std::wstring exeStr = exePath;
                        size_t exeSlash = exeStr.rfind(L'\\');
                        if (exeSlash != std::wstring::npos) gameDir = exeStr.substr(0, exeSlash);
                    }

                    RedirectState state;
                    state.archivePath = baseDir;
                    state.isLgp = true;
                    PopulateVirtualLgp(archiveRelPath, state);
                    
                    DWORD totalVirtualSize = (DWORD)state.virtualArchiveData.size();
                    for (const auto& entry : state.entries) {
                        totalVirtualSize += 24 + entry.size;
                    }
                    totalVirtualSize += 14; // footer
                    
                    std::wstring tempPath = GetTempPathForArchive(gameDir, archiveRelPath);
                    Log("[Loader] Creating temp file via Win32: %S\n", tempPath.c_str());
                    HANDLE hFile = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hFile != INVALID_HANDLE_VALUE) {
                        DWORD bytesWritten = 0;
                        BOOL writeRes = WriteFile(hFile, state.virtualArchiveData.data(), (DWORD)state.virtualArchiveData.size(), &bytesWritten, NULL);
                        if (writeRes && bytesWritten == state.virtualArchiveData.size()) {
                            LARGE_INTEGER li;
                            li.QuadPart = totalVirtualSize;
                            if (SetFilePointerEx(hFile, li, NULL, FILE_BEGIN)) {
                                if (SetEndOfFile(hFile)) {
                                    LARGE_INTEGER footerOffset;
                                    footerOffset.QuadPart = totalVirtualSize - 14;
                                    if (SetFilePointerEx(hFile, footerOffset, NULL, FILE_BEGIN)) {
                                        DWORD footerWritten = 0;
                                        WriteFile(hFile, "FINAL FANTASY7", 14, &footerWritten, NULL);
                                    }
                                }
                            }
                        }
                        CloseHandle(hFile);
                    }
                    
                    f = OriginalWfopen(tempPath.c_str(), L"rb");
                    if (f) {
                        Log("[Loader] Faking merged LGP archive '%S' using folder '%S' (size: %d, tempFile: %S)\n", filename, archiveFolder.c_str(), totalVirtualSize, tempPath.c_str());
                        state.tempFilePath = tempPath;
                        
                        // Open the physical LGP archive stream for fallbacks
                        std::wstring physicalLgpPath = gameDir + L"\\ff7\\workingdir\\data\\" + archiveRelPath + L".lgp";
                        state.physicalLgpFile = OriginalWfopen(physicalLgpPath.c_str(), L"rb");
                        
                        g_RedirectStates[f] = state;
                    }
                } else {
                    // No mod overrides, parse standard TOC
                    if (f) {
                        std::lock_guard<std::recursive_mutex> lock(g_Mutex);
                        if (g_RedirectStates.find(f) == g_RedirectStates.end()) {
                            RedirectState state;
                            state.archivePath = baseDir;
                            state.isLgp = true;
                            ParseLgpTOC(f, state);
                            OriginalFseek(f, 0, SEEK_SET);
                            g_RedirectStates[f] = state;
                        }
                    }
                }
            }
        }
    }
    Log("[Loader] _wfopen returned: %p for %S\n", f, filename ? filename : L"NULL");
    return f;
}




const LgpEntry* FindLgpEntry(const std::vector<LgpEntry>& entries, DWORD targetOffset) {
    if (entries.empty()) return nullptr;
    
    // Binary search for the first entry that starts AFTER targetOffset
    auto it = std::upper_bound(entries.begin(), entries.end(), targetOffset,
        [](DWORD offset, const LgpEntry& entry) {
            return offset < entry.dataStart;
        });
        
    // The candidate entry is the one right before the upper bound
    if (it != entries.begin()) {
        const LgpEntry& entry = *(it - 1);
        return &entry;
    }
    return nullptr;
}

void UpdateRedirection(FILE* stream, RedirectState& state, DWORD targetOffset) {
    state.virtualFileOffset = targetOffset;

    const LgpEntry* foundEntry = nullptr;

    // 1. Check if the targetOffset is the start of any entry (either header or data start)
    const LgpEntry* candidate = FindLgpEntry(state.entries, targetOffset);
    if (candidate) {
        if (targetOffset == candidate->dataStart || targetOffset == candidate->dataStart + 24) {
            foundEntry = candidate;
        }
    }

    // 2. If it's a random seek offset:
    if (!foundEntry) {
        if (state.isRedirecting) {
            // Keep the currently active redirection active ONLY if the seek is within the redirected file's enlarged bounds.
            if (candidate && candidate->name == state.activeEntryName) {
                if (targetOffset >= candidate->dataStart && targetOffset < candidate->dataStart + 24 + state.overrideSize) {
                    foundEntry = candidate;
                }
            }
        } else {
            // Find which entry the offset belongs to using original non-overlapping LGP boundaries.
            if (candidate) {
                if (targetOffset >= candidate->dataStart && targetOffset < candidate->dataStart + 24 + candidate->size) {
                    foundEntry = candidate;
                }
            }
        }
    }

    // 3. Handle redirection status update based on the identified entry
    if (foundEntry) {
        const LgpEntry& entry = *foundEntry;
        
        // Track the last loaded texture file name for correlation
        std::wstring entryNameLower = entry.diskName;
        std::transform(entryNameLower.begin(), entryNameLower.end(), entryNameLower.begin(), ::towlower);
        
        std::wstring archivePathLower = state.archivePath;
        std::transform(archivePathLower.begin(), archivePathLower.end(), archivePathLower.begin(), ::towlower);
        if (archivePathLower.find(L"battle.lgp") != std::wstring::npos) {
            if (entry.name.length() >= 2) {
                char prefix1 = entry.name[0];
                char prefix2 = entry.name[1];
                if (prefix1 >= 'a' && prefix1 <= 'z' && prefix2 >= 'a' && prefix2 <= 'z') {
                    int stageIndex = (prefix1 - 'a') * 26 + (prefix2 - 'a');
                    if (stageIndex >= 0 && stageIndex <= 89) {
                        wchar_t stageBuf[32];
                        swprintf_s(stageBuf, L"STAGE%02d", stageIndex);
                        std::wstring stageName = stageBuf;
                        bool isMasterFile = (entry.name.length() >= 4 && entry.name.substr(entry.name.length() - 2) == "aa");
                        if (stageName != g_ActiveStageName || isMasterFile) {
                            g_ActiveStageName = stageName;
                            g_StageTextureCounter = 0;
                            g_CurrentStageTexName = L"";
                            g_LastLoadedTexFile = L""; // Clear character texture tracking
                            
                            g_ActiveFieldName = L"";
                            g_FieldTextureCounter = 0;
                            g_CurrentFieldTexName = L"";
                            g_ActiveFieldTexIndices.clear();
                            
                            // Find all texture indices for this stage in active mods
                            g_ActiveStageTexIndices.clear();
                            for (const auto& mod : g_ActiveMods) {
                                std::wstring modBattleDir = g_ModsDirectory + L"\\" + mod + L"\\battle";
                                for (int i = 0; i < 99; i++) {
                                    wchar_t fileBuf[MAX_PATH];
                                    swprintf_s(fileBuf, L"%s\\%s_T%02d_00.dds", modBattleDir.c_str(), g_ActiveStageName.c_str(), i);
                                    if (FileExists(fileBuf)) {
                                        g_ActiveStageTexIndices.push_back(i);
                                    }
                                    swprintf_s(fileBuf, L"%s\\%s_T%02d_00.png", modBattleDir.c_str(), g_ActiveStageName.c_str(), i);
                                    if (FileExists(fileBuf)) {
                                        g_ActiveStageTexIndices.push_back(i);
                                    }
                                }
                            }
                            // Sort and remove duplicates
                            std::sort(g_ActiveStageTexIndices.begin(), g_ActiveStageTexIndices.end());
                            g_ActiveStageTexIndices.erase(std::unique(g_ActiveStageTexIndices.begin(), g_ActiveStageTexIndices.end()), g_ActiveStageTexIndices.end());
                            Log("[Loader] Active battle stage set to: %S (due to entry %s, isMaster=%d) - Found %d override textures\n", g_ActiveStageName.c_str(), entry.name.c_str(), isMasterFile, (int)g_ActiveStageTexIndices.size());
                        }
                    }
                }
            }
        } else if (archivePathLower.find(L"flevel.lgp") != std::wstring::npos) {
            if (entryNameLower.find(L".") == std::wstring::npos) {
                std::wstring fieldName = entryNameLower;
                if (fieldName != g_ActiveFieldName) {
                    g_ActiveFieldName = fieldName;
                    g_FieldTextureCounter = 0;
                    g_CurrentFieldTexName = L"";
                    
                    // Find all texture indices for this field in active mods
                    g_ActiveFieldTexIndices.clear();
                    for (const auto& mod : g_ActiveMods) {
                        std::wstring modFieldDir = g_BaseDir + L"\\" + g_ModsDirectory + L"\\" + mod + L"\\field\\" + g_ActiveFieldName;
                        for (int i = 0; i < 99; i++) {
                            wchar_t fileBuf[MAX_PATH];
                            swprintf_s(fileBuf, L"%s\\%s_%02d_00.png", modFieldDir.c_str(), g_ActiveFieldName.c_str(), i);
                            if (FileExists(fileBuf)) {
                                g_ActiveFieldTexIndices.push_back(i);
                            }
                            swprintf_s(fileBuf, L"%s\\%s_%02d_00.dds", modFieldDir.c_str(), g_ActiveFieldName.c_str(), i);
                            if (FileExists(fileBuf)) {
                                g_ActiveFieldTexIndices.push_back(i);
                            }
                        }
                    }
                    // Sort and remove duplicates
                    std::sort(g_ActiveFieldTexIndices.begin(), g_ActiveFieldTexIndices.end());
                    g_ActiveFieldTexIndices.erase(std::unique(g_ActiveFieldTexIndices.begin(), g_ActiveFieldTexIndices.end()), g_ActiveFieldTexIndices.end());
                    Log("[Loader] Active field map set to: %S - Found %d override textures\n", g_ActiveFieldName.c_str(), (int)g_ActiveFieldTexIndices.size());
                }
            }
        } else {
            if (entryNameLower.find(L".tex") != std::wstring::npos) {
                g_LastLoadedTexFile = entryNameLower;
            }
        }
        
        // If we are switching to a different entry, close the previous override file
        if (entry.name != state.activeEntryName) {
            if (state.overrideFile) {
                OriginalFclose(state.overrideFile);
                state.overrideFile = nullptr;
            }
            state.isRedirecting = false;
            state.activeEntryName = entry.name;
        }

        std::wstring baseDir = state.archivePath;
        std::replace(baseDir.begin(), baseDir.end(), L'/', L'\\');
        std::wstring gameDir = L"";
        size_t dataPos = baseDir.find(L"\\ff7\\workingdir");
        if (dataPos != std::wstring::npos) {
            gameDir = baseDir.substr(0, dataPos);
        } else {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            std::wstring exeStr = exePath;
            size_t exeSlash = exeStr.rfind(L'\\');
            if (exeSlash != std::wstring::npos) gameDir = exeStr.substr(0, exeSlash);
        }

        std::wstring archiveRelPath = L"";
        size_t dataPosInArchive = baseDir.find(L"\\ff7\\workingdir\\data\\");
        if (dataPosInArchive != std::wstring::npos) {
            archiveRelPath = baseDir.substr(dataPosInArchive + 21);
        } else {
            size_t lastSlash = baseDir.rfind(L'\\');
            archiveRelPath = (lastSlash != std::wstring::npos) ? baseDir.substr(lastSlash + 1) : baseDir;
        }
        size_t dotPos = archiveRelPath.rfind(L'.');
        if (dotPos != std::wstring::npos) archiveRelPath = archiveRelPath.substr(0, dotPos);

        bool isPhysicalFallback = entry.modFolder.empty() && state.isVirtualLgp;
        if (isPhysicalFallback) {
            // Close any active loose override file
            if (state.overrideFile) {
                OriginalFclose(state.overrideFile);
                state.overrideFile = nullptr;
            }
            
            // Ensure physical LGP file stream is open
            if (!state.physicalLgpFile) {
                std::wstring physicalLgpPath = gameDir + L"\\ff7\\workingdir\\data\\" + archiveRelPath + L".lgp";
                state.physicalLgpFile = OriginalWfopen(physicalLgpPath.c_str(), L"rb");
            }
            
            if (state.physicalLgpFile) {
                state.isRedirecting = true;
                state.overrideSize = entry.size;
                
                memset(state.fakeHeader, 0, 24);
                memcpy(state.fakeHeader, entry.name.c_str(), min(entry.name.size(), (size_t)20));
                *(DWORD*)&state.fakeHeader[20] = entry.size;
                
                DWORD relOffset = targetOffset - entry.dataStart;
                if (relOffset < 24) {
                    state.fakeHeaderOffset = relOffset;
                    state.overrideVirtualOffset = entry.originalDataStart + 24;
                } else {
                    state.fakeHeaderOffset = 24;
                    state.overrideVirtualOffset = entry.originalDataStart + relOffset;
                }
                Log("[Loader] [Redirect] Virtual LGP redirected (Physical Fallback): entry %s -> physical LGP (virtualOffset: %d, relOffset: %d, size: %d, physOffset: %d)\n",
                    entry.name.c_str(), state.virtualFileOffset, relOffset, entry.size, state.overrideVirtualOffset);
            } else {
                Log("[Loader] ERROR: [Redirect] Failed to open physical LGP fallback: %S\n", archiveRelPath.c_str());
                state.isRedirecting = false;
            }
        } else {
            std::wstring overridePath = L"";
            if (state.isVirtualLgp) {
                overridePath = gameDir + L"\\" + g_ModsDirectory + L"\\" + entry.modFolder + L"\\" + archiveRelPath + L"\\" + entry.diskName;
            } else {
                overridePath = ResolveModPath(archiveRelPath + L"\\" + entry.diskName);
            }
            
            if (!overridePath.empty()) {
                state.isRedirecting = true;
                state.overrideFilePath = overridePath;
                
                if (!state.overrideFile) {
                    state.overrideFile = OriginalWfopen(overridePath.c_str(), L"rb");
                    if (state.overrideFile) {
                        OriginalFseek(state.overrideFile, 0, SEEK_END);
                        state.overrideSize = (DWORD)OriginalFtelli64(state.overrideFile);
                        OriginalFseek(state.overrideFile, 0, SEEK_SET);
                        
                        memset(state.fakeHeader, 0, 24);
                        memcpy(state.fakeHeader, entry.name.c_str(), min(entry.name.size(), (size_t)20));
                        *(DWORD*)&state.fakeHeader[20] = state.overrideSize;
                    } else {
                        Log("[Loader] ERROR: [Redirect] Failed to open override file: %S (errno: %d)\n", overridePath.c_str(), errno);
                        state.isRedirecting = false;
                    }
                }
                
                if (state.overrideFile) {
                    DWORD relOffset = targetOffset - entry.dataStart;
                    if (relOffset < 24) {
                        state.fakeHeaderOffset = relOffset;
                        state.overrideVirtualOffset = 0;
                    } else {
                        state.fakeHeaderOffset = 24;
                        state.overrideVirtualOffset = relOffset - 24;
                    }
                    Log("[Loader] [Redirect] Virtual LGP redirected: entry %s -> %S (virtualOffset: %d, relOffset: %d, overrideSize: %d)\n", entry.name.c_str(), overridePath.c_str(), state.virtualFileOffset, relOffset, state.overrideSize);
                }
            } else {
                // No override for this entry, ensure we are not redirecting
                if (state.overrideFile) {
                    OriginalFclose(state.overrideFile);
                    state.overrideFile = nullptr;
                }
                state.isRedirecting = false;
            }
        }
    } else {
        // Target offset fell outside of any entry's original boundaries
        if (state.overrideFile) {
            OriginalFclose(state.overrideFile);
            state.overrideFile = nullptr;
        }
        state.isRedirecting = false;
        state.activeEntryName.clear();
    }
}

// Hooked fseek
int HookedFseek(FILE* stream, long offset, int origin) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    auto it = g_RedirectStates.find(stream);
    bool isLgp = false;
    if (it != g_RedirectStates.end()) {
        RedirectState& state = it->second;
        isLgp = state.isLgp;

        if (state.isLgp) {
            DWORD targetOffset = offset;
            if (origin == SEEK_CUR) {
                targetOffset = state.virtualFileOffset + offset;
            } else if (origin == SEEK_END) {
                if (state.isVirtualLgp) {
                    DWORD totalVirtualSize = (DWORD)state.virtualArchiveData.size();
                    for (const auto& entry : state.entries) {
                        totalVirtualSize += 24 + entry.size;
                    }
                    targetOffset = totalVirtualSize + offset;
                } else {
                    OriginalFseek(stream, 0, SEEK_END);
                    targetOffset = (DWORD)_ftelli64(stream) + offset;
                }
            }

            UpdateRedirection(stream, state, targetOffset);
        }
    }
    
    if (isLgp && it != g_RedirectStates.end()) {
        int res = OriginalFseek(stream, it->second.virtualFileOffset, SEEK_SET);
        if (OriginalClearerr) {
            OriginalClearerr(stream);
        }
        return res;
    }
    return OriginalFseek(stream, offset, origin);
}

// Hooked _fseeki64
int HookedFseeki64(FILE* stream, __int64 offset, int origin) {
    return HookedFseek(stream, (long)offset, origin);
}

// Hooked fread
size_t HookedFread(void* buffer, size_t size, size_t count, FILE* stream) {
    if (size == 0 || count == 0) return 0;
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    auto it = g_RedirectStates.find(stream);
    if (it != g_RedirectStates.end()) {
        RedirectState& state = it->second;

        if (state.isLgp) {
            UpdateRedirection(stream, state, state.virtualFileOffset);

            DWORD totalRequested = (DWORD)(size * count);
            DWORD totalBytesRead = 0;
            DWORD bytesToRead = totalRequested;
            BYTE* destBuffer = (BYTE*)buffer;

            if (state.isRedirecting) {
                if (state.fakeHeaderOffset < 24) {
                    DWORD headerBytesAvailable = 24 - state.fakeHeaderOffset;
                    DWORD chunk = min(bytesToRead, headerBytesAvailable);
                    memcpy(destBuffer, state.fakeHeader + state.fakeHeaderOffset, chunk);
                    state.fakeHeaderOffset += chunk;
                    destBuffer += chunk;
                    bytesToRead -= chunk;
                    totalBytesRead += chunk;
                }

                if (bytesToRead > 0) {
                    FILE* srcFile = state.overrideFile ? state.overrideFile : state.physicalLgpFile;
                    if (srcFile) {
                        OriginalFseek(srcFile, state.overrideVirtualOffset, SEEK_SET);
                        size_t actualRead = OriginalFread(destBuffer, 1, bytesToRead, srcFile);
                        state.overrideVirtualOffset += (DWORD)actualRead;
                        totalBytesRead += (DWORD)actualRead;
                    }
                }

                state.virtualFileOffset += totalBytesRead;
                // Keep the dummy stream's file pointer in sync
                OriginalFseek(stream, state.virtualFileOffset, SEEK_SET);
                if (OriginalClearerr) OriginalClearerr(stream);

                // If we hit EOF on the override file, force EOF on the dummy stream
                if (state.overrideFile && state.overrideVirtualOffset >= state.overrideSize) {
                    OriginalFseek(stream, 0, SEEK_END);
                    char dummyChar;
                    OriginalFread(&dummyChar, 1, 1, stream);
                }

                return totalBytesRead / size;
            }
        }
    }

    size_t res = OriginalFread(buffer, size, count, stream);
    if (it != g_RedirectStates.end()) {
        it->second.virtualFileOffset += (DWORD)(res * size);
    }
    return res;
}

// Hooked fclose
int HookedFclose(FILE* stream) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    auto it = g_RedirectStates.find(stream);
    if (it != g_RedirectStates.end()) {
        Log("[Loader] fclose: Handle: %p (VirtualLgp: %d, Redirecting: %d)\n", stream, it->second.isVirtualLgp, it->second.isRedirecting);
        if (it->second.overrideFile) {
            OriginalFclose(it->second.overrideFile);
        }
        if (it->second.physicalLgpFile) {
            OriginalFclose(it->second.physicalLgpFile);
        }
        std::wstring tempFile = it->second.tempFilePath;
        g_RedirectStates.erase(it);
        int res = OriginalFclose(stream);
        if (!tempFile.empty()) {
            _wremove(tempFile.c_str());
        }
        return res;
    }
    return OriginalFclose(stream);
}

// Hooked ftell
long HookedFtell(FILE* stream) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    auto it = g_RedirectStates.find(stream);
    if (it != g_RedirectStates.end()) {
        return (long)it->second.virtualFileOffset;
    }
    return OriginalFtell(stream);
}

// Hooked _ftelli64
__int64 HookedFtelli64(FILE* stream) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    auto it = g_RedirectStates.find(stream);
    if (it != g_RedirectStates.end()) {
        return (__int64)it->second.virtualFileOffset;
    }
    return OriginalFtelli64(stream);
}

// Hooked fgetpos
int HookedFgetpos(FILE* stream, fpos_t* pos) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    auto it = g_RedirectStates.find(stream);
    if (it != g_RedirectStates.end()) {
        *pos = it->second.virtualFileOffset;
        return 0;
    }
    return OriginalFgetpos(stream, pos);
}

// Hooked fsetpos
int HookedFsetpos(FILE* stream, const fpos_t* pos) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    auto it = g_RedirectStates.find(stream);
    bool isLgp = false;
    if (it != g_RedirectStates.end()) {
        RedirectState& state = it->second;
        isLgp = state.isLgp;
        
        DWORD targetOffset = (DWORD)*pos;
        if (state.isLgp) {
            UpdateRedirection(stream, state, targetOffset);
        }
    }

    if (isLgp && it != g_RedirectStates.end()) {
        fpos_t actualPos = it->second.virtualFileOffset;
        int res = OriginalFsetpos(stream, &actualPos);
        if (OriginalClearerr) {
            OriginalClearerr(stream);
        }
        return res;
    }
    return OriginalFsetpos(stream, pos);
}

// Hooked fgetc
int HookedFgetc(FILE* stream) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    auto it = g_RedirectStates.find(stream);
    if (it != g_RedirectStates.end()) {
        unsigned char c = 0;
        size_t readCount = HookedFread(&c, 1, 1, stream);
        if (readCount > 0) {
            return c;
        } else {
            return EOF;
        }
    }
    return OriginalFgetc(stream);
}

// Hooked _get_stream_buffer_pointers
errno_t __cdecl HookedGetStreamBufferPointers(FILE* stream, char*** base, char*** ptr, int** count) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    auto it = g_RedirectStates.find(stream);
    if (it != g_RedirectStates.end()) {
        RedirectState& state = it->second;
        if (state.isRedirecting && state.overrideFile && !state.isLgp) {
            Log("[Loader] [_get_stream_buffer_pointers] Redirecting stream %p -> override file %p\n", stream, state.overrideFile);
            return OriginalGetStreamBufferPointers(state.overrideFile, base, ptr, count);
        }
    }
    return OriginalGetStreamBufferPointers(stream, base, ptr, count);
}

// Hooked ungetc
int HookedUngetc(int c, FILE* stream) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    auto it = g_RedirectStates.find(stream);
    if (it != g_RedirectStates.end()) {
        RedirectState& state = it->second;
        if (state.isRedirecting && state.overrideFile) {
            int res = OriginalUngetc(c, state.overrideFile);
            state.virtualFileOffset--;
            if (state.overrideVirtualOffset > 0) {
                state.overrideVirtualOffset--;
            }
            Log("[Loader] [Ungetc] Redirected stream %p to override file %p. Pos updated to %d\n", stream, state.overrideFile, state.virtualFileOffset);
            return res;
        } else {
            int res = OriginalUngetc(c, stream);
            state.virtualFileOffset--;
            Log("[Loader] [Ungetc] Stream %p. Pos updated to %d\n", stream, state.virtualFileOffset);
            return res;
        }
    }
    return OriginalUngetc(c, stream);
}

// Hooked _fileno
int HookedFileno(FILE* stream) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    auto it = g_RedirectStates.find(stream);
    if (it != g_RedirectStates.end()) {
        RedirectState& state = it->second;
        if (state.isRedirecting && state.overrideFile && !state.isLgp) {
            int fd = OriginalFileno(state.overrideFile);
            Log("[Loader] [_fileno] Redirecting stream %p -> override file fd %d\n", stream, fd);
            return fd;
        }
    }
    return OriginalFileno(stream);
}

// Hooked fflush
int HookedFflush(FILE* stream) {
    if (!stream) return OriginalFflush(nullptr);
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    auto it = g_RedirectStates.find(stream);
    if (it != g_RedirectStates.end()) {
        RedirectState& state = it->second;
        if (state.isRedirecting && state.overrideFile) {
            return OriginalFflush(state.overrideFile);
        }
    }
    return OriginalFflush(stream);
}

// Hooked clearerr
void HookedClearerr(FILE* stream) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    auto it = g_RedirectStates.find(stream);
    if (it != g_RedirectStates.end()) {
        RedirectState& state = it->second;
        if (state.isRedirecting && state.overrideFile) {
            Log("[Loader] [Clearerr] Redirecting stream %p -> override file %p\n", stream, state.overrideFile);
            OriginalClearerr(state.overrideFile);
        }
    }
    OriginalClearerr(stream);
}
