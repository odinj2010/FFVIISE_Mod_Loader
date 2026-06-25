#include "hooks.h"
#include <windows.h>
#include <string>
#include <unordered_map>
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

// Direct3D11 / Direct2D overlay variables
void Log(const char* format, ...);
typedef HRESULT(WINAPI* IDXGISwapChainPresent_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
IDXGISwapChainPresent_t OriginalPresent = nullptr;

typedef HRESULT(WINAPI* D3D11CreateDeviceAndSwapChain_t)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**
);
D3D11CreateDeviceAndSwapChain_t OriginalD3D11CreateDeviceAndSwapChain = nullptr;

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
                16, 0, 0, 0, FW_LIGHT, TRUE, FALSE, FALSE,
                ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Verdana"
            );
            HFONT oldFont = (HFONT)SelectObject(hdc, hFont);

            const char* text = "FFVIISE File Loader Active - by NfgOdin";
            TextOutA(hdc, 25, 25, text, (int)strlen(text));

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
            void** pVMT = *(void***)pSwapChain;
            void* pPresent = pVMT[8];
            Log("[Loader] SwapChain created via CreateSwapChainForHwnd! Hooking Present (%p)...\n", pPresent);

            MH_STATUS status = MH_CreateHook(pPresent, (LPVOID)&HookedPresent, (LPVOID*)&OriginalPresent);
            if (status == MH_OK) {
                MH_STATUS enableStatus = MH_EnableHook(pPresent);
                Log("[Loader] D3D11 SwapChain Present Hooked successfully! Status: %d\n", enableStatus);
            } else {
                Log("[Loader] Failed to hook Present. Error: %d\n", status);
            }
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
            void** pVMT = *(void***)pSwapChain;
            void* pPresent = pVMT[8];
            Log("[Loader] SwapChain created via IDXGIFactory! Hooking Present (%p)...\n", pPresent);

            MH_STATUS status = MH_CreateHook(pPresent, (LPVOID)&HookedPresent, (LPVOID*)&OriginalPresent);
            if (status == MH_OK) {
                MH_STATUS enableStatus = MH_EnableHook(pPresent);
                Log("[Loader] D3D11 SwapChain Present Hooked successfully! Status: %d\n", enableStatus);
            } else {
                Log("[Loader] Failed to hook Present. Error: %d\n", status);
            }
        }
    }
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
    Log("[Loader] D3D11CreateDeviceAndSwapChain intercepted! ppSwapChain: %p, ppDevice: %p\n", ppSwapChain, ppDevice);
    HRESULT hr = OriginalD3D11CreateDeviceAndSwapChain(
        pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
        SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext
    );
    Log("[Loader] Original D3D11CreateDeviceAndSwapChain returned: 0x%08X\n", hr);
    
    if (SUCCEEDED(hr)) {
        if (ppSwapChain && *ppSwapChain) {
            IDXGISwapChain* pSwapChain = *ppSwapChain;
            if (!OriginalPresent) {
                void** pVMT = *(void***)pSwapChain;
                void* pPresent = pVMT[8];
                Log("[Loader] Game SwapChain created directly! Hooking Present (%p)...\n", pPresent);

                MH_STATUS status = MH_CreateHook(pPresent, (LPVOID)&HookedPresent, (LPVOID*)&OriginalPresent);
                if (status == MH_OK) {
                    MH_STATUS enableStatus = MH_EnableHook(pPresent);
                    Log("[Loader] D3D11 SwapChain Present Hooked successfully! Status: %d\n", enableStatus);
                } else {
                    Log("[Loader] Failed to hook Present. Error: %d\n", status);
                }
            }
        } else if (ppDevice && *ppDevice) {
            ID3D11Device* pDevice = *ppDevice;
            Log("[Loader] Device created without SwapChain. Attempting to query DXGI Factory to hook SwapChain creation...\n");
            
            IDXGIDevice* pDxgiDevice = nullptr;
            if (SUCCEEDED(pDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDxgiDevice)) && pDxgiDevice) {
                IDXGIAdapter* pDxgiAdapter = nullptr;
                if (SUCCEEDED(pDxgiDevice->GetAdapter(&pDxgiAdapter)) && pDxgiAdapter) {
                    IDXGIFactory* pDxgiFactory = nullptr;
                    if (SUCCEEDED(pDxgiAdapter->GetParent(__uuidof(IDXGIFactory), (void**)&pDxgiFactory)) && pDxgiFactory) {
                        if (!OriginalCreateSwapChain) {
                            void** pVMT = *(void***)pDxgiFactory;
                            void* pCreateSwapChain = pVMT[10];
                            Log("[Loader] Hooking IDXGIFactory::CreateSwapChain (%p) dynamically...\n", pCreateSwapChain);
                            
                            MH_STATUS status = MH_CreateHook(pCreateSwapChain, (LPVOID)&HookedCreateSwapChain, (LPVOID*)&OriginalCreateSwapChain);
                            if (status == MH_OK) {
                                MH_STATUS enableStatus = MH_EnableHook(pCreateSwapChain);
                                Log("[Loader] IDXGIFactory::CreateSwapChain Hooked successfully! Status: %d\n", enableStatus);
                            } else {
                                Log("[Loader] Failed to hook CreateSwapChain. Error: %d\n", status);
                            }

                            // Also try to hook IDXGIFactory2::CreateSwapChainForHwnd (index 15)
                            IDXGIFactory2* pDxgiFactory2 = nullptr;
                            if (SUCCEEDED(pDxgiFactory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&pDxgiFactory2)) && pDxgiFactory2) {
                                void** pVMT2 = *(void***)pDxgiFactory2;
                                void* pCreateSwapChainForHwnd = pVMT2[15];
                                Log("[Loader] Hooking IDXGIFactory2::CreateSwapChainForHwnd (%p) dynamically...\n", pCreateSwapChainForHwnd);

                                MH_STATUS status2 = MH_CreateHook(pCreateSwapChainForHwnd, (LPVOID)&HookedCreateSwapChainForHwnd, (LPVOID*)&OriginalCreateSwapChainForHwnd);
                                if (status2 == MH_OK) {
                                    MH_STATUS enableStatus2 = MH_EnableHook(pCreateSwapChainForHwnd);
                                    Log("[Loader] IDXGIFactory2::CreateSwapChainForHwnd Hooked successfully! Status: %d\n", enableStatus2);
                                } else {
                                    Log("[Loader] Failed to hook CreateSwapChainForHwnd. Error: %d\n", status2);
                                }
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
bool g_EnableLogging = true;
std::wstring g_LogFileName = L"mods_loader_log.txt";
std::wstring g_LogPath = L"mods_loader_log.txt";
std::wstring g_IniPath = L"mods_loader.ini";

bool DirectoryExists(const std::wstring& path) {
    DWORD dwAttrib = GetFileAttributesW(path.c_str());
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && (dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
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

    // Active redirection state for LGP file handle
    bool isRedirecting = false;
    std::wstring overrideFilePath;
    FILE* overrideFile = nullptr;
    DWORD overrideVirtualOffset = 0; // Read offset in the loose file
    DWORD overrideSize = 0;          // Size of the loose file
    DWORD fakeHeaderOffset = 0;      // If game is reading the 24-byte DATA-ENTRY_HEADER
    BYTE fakeHeader[24];
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

void PopulateVirtualLgp(const std::wstring& archiveFolder, RedirectState& state) {
    Log("[Loader] PopulateVirtualLgp started for folder: %S\n", archiveFolder.c_str());
    std::vector<FileInfo> files = GetFilesInDirectory(archiveFolder);
    
    DWORD numFiles = (DWORD)files.size();
    Log("[Loader] PopulateVirtualLgp: Found %d files in folder.\n", numFiles);

    state.isVirtualLgp = true;
    state.entries.resize(numFiles);

    // Initialize entries with diskName, lowercase LGP name, and size
    for (DWORD i = 0; i < numFiles; ++i) {
        std::wstring wName = files[i].name;
        std::string name(wName.begin(), wName.end());
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);

        LgpEntry entry;
        entry.name = name;
        entry.diskName = files[i].name;
        entry.size = files[i].size;
        state.entries[i] = entry;
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
        int idx = FilenameToLookupIndex(state.entries[i].name); // 1-based index (1 to 900)
        if (idx >= 0 && idx < 900) {
            lookupCount[idx]++;
            if (lookupIndex[idx] == 0) {
                lookupIndex[idx] = (unsigned short)(i + 1); // 1-based TOC index
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

void LoadConfiguration() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring exeStr = exePath;
    size_t exeSlash = exeStr.rfind(L'\\');
    std::wstring iniPath = (exeSlash != std::wstring::npos) ? exeStr.substr(0, exeSlash) + L"\\mods_loader.ini" : L"mods_loader.ini";

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

    // Read LogFile
    wchar_t logFile[MAX_PATH] = L"mods_loader_log.txt";
    GetPrivateProfileStringW(L"Loader", L"LogFile", L"mods_loader_log.txt", logFile, MAX_PATH, iniPath.c_str());
    g_LogFileName = logFile;

    // Resolve absolute log path
    g_LogPath = (exeSlash != std::wstring::npos) ? exeStr.substr(0, exeSlash) + L"\\" + g_LogFileName : g_LogFileName;
    g_IniPath = iniPath;
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

// Hooked fopen
FILE* HookedFopen(const char* filename, const char* mode) {
    if (filename) {
        wchar_t wFilename[MAX_PATH];
        size_t convertedChars = 0;
        mbstowcs_s(&convertedChars, wFilename, filename, MAX_PATH);

        wchar_t absPath[MAX_PATH];
        if (GetFullPathNameW(wFilename, MAX_PATH, absPath, NULL) != 0) {
            std::wstring pathStr = absPath;
            std::replace(pathStr.begin(), pathStr.end(), L'/', L'\\');
            std::wstring originalPath = pathStr;
            std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::towlower);

            size_t dataPos = pathStr.find(L"\\ff7\\workingdir\\data\\");
            if (dataPos != std::wstring::npos) {
                std::wstring overridePath = originalPath.substr(0, dataPos) + L"\\" + g_ModsDirectory + L"\\" + originalPath.substr(dataPos + 21);
                if (FileExists(overridePath)) {
                    char cOverridePath[MAX_PATH];
                    size_t wConverted = 0;
                    wcstombs_s(&wConverted, cOverridePath, overridePath.c_str(), MAX_PATH);
                    
                    Log("[Loader] Redirecting fopen: %s -> %s\n", filename, cOverridePath);
                    return OriginalFopen(cOverridePath, mode);
                }
            }
        }
    }
    
    Log("[Loader] fopen called: %s (mode: %s)\n", filename ? filename : "NULL", mode ? mode : "NULL");
    FILE* f = OriginalFopen(filename, mode);
    if (!f && filename) {
        std::string pathStr = filename;
        std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::tolower);

        if (pathStr.find(".lgp") != std::string::npos) {
            wchar_t wFilename[MAX_PATH];
            size_t convertedChars = 0;
            mbstowcs_s(&convertedChars, wFilename, filename, MAX_PATH);

            wchar_t absPath[MAX_PATH];
            if (GetFullPathNameW(wFilename, MAX_PATH, absPath, NULL) != 0) {
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

                std::wstring archiveFolder = gameDir + L"\\" + g_ModsDirectory + L"\\" + archiveRelPath;
                if (DirectoryExists(archiveFolder)) {
                    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
                    RedirectState state;
                    state.archivePath = baseDir;
                    state.isLgp = true;
                    PopulateVirtualLgp(archiveFolder, state);
                    
                    DWORD totalVirtualSize = (DWORD)state.virtualArchiveData.size();
                    for (const auto& entry : state.entries) {
                        totalVirtualSize += 24 + entry.size;
                    }
                    totalVirtualSize += 14; // for the "FINAL FANTASY7" footer
                    
                    std::wstring tempPath = gameDir + L"\\" + g_ModsDirectory + L"\\" + archiveRelPath + L".tmp";
                    Log("[Loader] Creating temp file via Win32: %S\n", tempPath.c_str());
                    HANDLE hFile = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hFile != INVALID_HANDLE_VALUE) {
                        DWORD bytesWritten = 0;
                        BOOL writeRes = WriteFile(hFile, state.virtualArchiveData.data(), (DWORD)state.virtualArchiveData.size(), &bytesWritten, NULL);
                        if (writeRes && bytesWritten == state.virtualArchiveData.size()) {
                            Log("[Loader] Temp file header/TOC written successfully (%u bytes).\n", bytesWritten);
                            LARGE_INTEGER li;
                            li.QuadPart = totalVirtualSize;
                            if (SetFilePointerEx(hFile, li, NULL, FILE_BEGIN)) {
                                if (SetEndOfFile(hFile)) {
                                    Log("[Loader] Temp file sized to %u successfully via Win32.\n", totalVirtualSize);
                                    
                                    // Seek to end minus 14 and write the footer
                                    LARGE_INTEGER footerOffset;
                                    footerOffset.QuadPart = totalVirtualSize - 14;
                                    if (SetFilePointerEx(hFile, footerOffset, NULL, FILE_BEGIN)) {
                                        DWORD footerWritten = 0;
                                        WriteFile(hFile, "FINAL FANTASY7", 14, &footerWritten, NULL);
                                    }
                                } else {
                                    Log("[Loader] ERROR: SetEndOfFile failed. Error: %d\n", GetLastError());
                                }
                            } else {
                                Log("[Loader] ERROR: SetFilePointerEx failed. Error: %d\n", GetLastError());
                            }
                        } else {
                            Log("[Loader] ERROR: WriteFile failed for temp file. Error: %d\n", GetLastError());
                        }
                        CloseHandle(hFile);
                    } else {
                        Log("[Loader] ERROR: CreateFileW failed for %S. Error: %d\n", tempPath.c_str(), GetLastError());
                    }
                    
                    Log("[Loader] Re-opening temp file for read: %S\n", tempPath.c_str());
                    f = OriginalWfopen(tempPath.c_str(), L"rb");
                    if (f) {
                        Log("[Loader] Faking missing LGP archive '%s' using folder '%S' (size: %d, tempFile: %S)\n", filename, archiveFolder.c_str(), totalVirtualSize, tempPath.c_str());
                        state.tempFilePath = tempPath;
                        g_RedirectStates[f] = state;
                    } else {
                        Log("[Loader] ERROR: Failed to re-open temp file for read: %S (errno: %d)\n", tempPath.c_str(), errno);
                    }
                } else {
                    Log("[Loader] Directory does not exist: %S\n", archiveFolder.c_str());
                }
            }
        }
    }

    if (f && filename) {
        wchar_t wFilename[MAX_PATH];
        size_t convertedChars = 0;
        mbstowcs_s(&convertedChars, wFilename, filename, MAX_PATH);

        wchar_t absPath[MAX_PATH];
        if (GetFullPathNameW(wFilename, MAX_PATH, absPath, NULL) != 0) {
            std::wstring pathStr = absPath;
            std::replace(pathStr.begin(), pathStr.end(), L'/', L'\\');
            std::wstring originalPath = pathStr;
            std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::towlower);

            if (pathStr.find(L".lgp") != std::wstring::npos) {
                std::lock_guard<std::recursive_mutex> lock(g_Mutex);
                if (g_RedirectStates.find(f) == g_RedirectStates.end()) {
                    RedirectState state;
                    state.archivePath = originalPath;
                    state.isLgp = true;
                    ParseLgpTOC(f, state);
                    
                    OriginalFseek(f, 0, SEEK_SET);
                    g_RedirectStates[f] = state;
                }
            }
        }
    }
    Log("[Loader] fopen returned: %p for %s\n", f, filename ? filename : "NULL");
    return f;
}


// Hooked _wfopen
FILE* HookedWfopen(const wchar_t* filename, const wchar_t* mode) {
    if (filename) {
        wchar_t absPath[MAX_PATH];
        if (GetFullPathNameW(filename, MAX_PATH, absPath, NULL) != 0) {
            std::wstring pathStr = absPath;
            std::replace(pathStr.begin(), pathStr.end(), L'/', L'\\');
            std::wstring originalPath = pathStr;
            std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::towlower);

            size_t dataPos = pathStr.find(L"\\ff7\\workingdir\\data\\");
            if (dataPos != std::wstring::npos) {
                std::wstring overridePath = originalPath.substr(0, dataPos) + L"\\" + g_ModsDirectory + L"\\" + originalPath.substr(dataPos + 21);
                if (FileExists(overridePath)) {
                    Log("[Loader] Redirecting _wfopen: %S -> %S\n", filename, overridePath.c_str());
                    return OriginalWfopen(overridePath.c_str(), mode);
                }
            }
        }
    }

    Log("[Loader] _wfopen called: %S (mode: %S)\n", filename ? filename : L"NULL", mode ? mode : L"NULL");
    FILE* f = OriginalWfopen(filename, mode);
    if (!f && filename) {
        std::wstring pathStr = filename;
        std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::towlower);

        if (pathStr.find(L".lgp") != std::wstring::npos) {
            wchar_t absPath[MAX_PATH];
            if (GetFullPathNameW(filename, MAX_PATH, absPath, NULL) != 0) {
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

                std::wstring archiveFolder = gameDir + L"\\" + g_ModsDirectory + L"\\" + archiveRelPath;
                if (DirectoryExists(archiveFolder)) {
                    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
                    RedirectState state;
                    state.archivePath = baseDir;
                    state.isLgp = true;
                    PopulateVirtualLgp(archiveFolder, state);
                    
                    DWORD totalVirtualSize = (DWORD)state.virtualArchiveData.size();
                    for (const auto& entry : state.entries) {
                        totalVirtualSize += 24 + entry.size;
                    }
                    totalVirtualSize += 14; // for the "FINAL FANTASY7" footer
                    
                    std::wstring tempPath = gameDir + L"\\" + g_ModsDirectory + L"\\" + archiveRelPath + L".tmp";
                    Log("[Loader] Creating temp file via Win32: %S\n", tempPath.c_str());
                    HANDLE hFile = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hFile != INVALID_HANDLE_VALUE) {
                        DWORD bytesWritten = 0;
                        BOOL writeRes = WriteFile(hFile, state.virtualArchiveData.data(), (DWORD)state.virtualArchiveData.size(), &bytesWritten, NULL);
                        if (writeRes && bytesWritten == state.virtualArchiveData.size()) {
                            Log("[Loader] Temp file header/TOC written successfully (%u bytes).\n", bytesWritten);
                            LARGE_INTEGER li;
                            li.QuadPart = totalVirtualSize;
                            if (SetFilePointerEx(hFile, li, NULL, FILE_BEGIN)) {
                                if (SetEndOfFile(hFile)) {
                                    Log("[Loader] Temp file sized to %u successfully via Win32.\n", totalVirtualSize);
                                    
                                    // Seek to end minus 14 and write the footer
                                    LARGE_INTEGER footerOffset;
                                    footerOffset.QuadPart = totalVirtualSize - 14;
                                    if (SetFilePointerEx(hFile, footerOffset, NULL, FILE_BEGIN)) {
                                        DWORD footerWritten = 0;
                                        WriteFile(hFile, "FINAL FANTASY7", 14, &footerWritten, NULL);
                                    }
                                } else {
                                    Log("[Loader] ERROR: SetEndOfFile failed. Error: %d\n", GetLastError());
                                }
                            } else {
                                Log("[Loader] ERROR: SetFilePointerEx failed. Error: %d\n", GetLastError());
                            }
                        } else {
                            Log("[Loader] ERROR: WriteFile failed for temp file. Error: %d\n", GetLastError());
                        }
                        CloseHandle(hFile);
                    } else {
                        Log("[Loader] ERROR: CreateFileW failed for %S. Error: %d\n", tempPath.c_str(), GetLastError());
                    }
                    
                    Log("[Loader] Re-opening temp file for read: %S\n", tempPath.c_str());
                    f = OriginalWfopen(tempPath.c_str(), L"rb");
                    if (f) {
                        Log("[Loader] Faking missing LGP archive '%S' using folder '%S' (size: %d, tempFile: %S)\n", filename, archiveFolder.c_str(), totalVirtualSize, tempPath.c_str());
                        state.tempFilePath = tempPath;
                        g_RedirectStates[f] = state;
                    } else {
                        Log("[Loader] ERROR: Failed to re-open temp file for read: %S (errno: %d)\n", tempPath.c_str(), errno);
                    }
                } else {
                    Log("[Loader] Directory does not exist: %S\n", archiveFolder.c_str());
                }
            }
        }
    }

    if (f && filename) {
        wchar_t absPath[MAX_PATH];
        if (GetFullPathNameW(filename, MAX_PATH, absPath, NULL) != 0) {
            std::wstring pathStr = absPath;
            std::replace(pathStr.begin(), pathStr.end(), L'/', L'\\');
            std::wstring originalPath = pathStr;
            std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::towlower);

            if (pathStr.find(L".lgp") != std::wstring::npos) {
                std::lock_guard<std::recursive_mutex> lock(g_Mutex);
                if (g_RedirectStates.find(f) == g_RedirectStates.end()) {
                    RedirectState state;
                    state.archivePath = originalPath;
                    state.isLgp = true;
                    ParseLgpTOC(f, state);
                    
                    OriginalFseek(f, 0, SEEK_SET);
                    g_RedirectStates[f] = state;
                }
            }
        }
    }
    Log("[Loader] _wfopen returned: %p for %S\n", f, filename ? filename : L"NULL");
    return f;
}


void UpdateRedirection(FILE* stream, RedirectState& state, DWORD targetOffset) {
    state.virtualFileOffset = targetOffset;
    state.isRedirecting = false;
    if (state.overrideFile) {
        OriginalFclose(state.overrideFile);
        state.overrideFile = nullptr;
    }

    for (const auto& entry : state.entries) {
        if (targetOffset >= entry.dataStart && targetOffset < entry.dataStart + 24 + entry.size) {
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

            std::wstring overridePath = gameDir + L"\\" + g_ModsDirectory + L"\\" + archiveRelPath + L"\\" + entry.diskName;
            
            if (state.isVirtualLgp || FileExists(overridePath)) {
                state.isRedirecting = true;
                state.overrideFilePath = overridePath;
                
                state.overrideFile = OriginalWfopen(overridePath.c_str(), L"rb");
                if (state.overrideFile) {
                    OriginalFseek(state.overrideFile, 0, SEEK_END);
                    state.overrideSize = (DWORD)OriginalFtelli64(state.overrideFile);
                    OriginalFseek(state.overrideFile, 0, SEEK_SET);

                    memset(state.fakeHeader, 0, 24);
                    memcpy(state.fakeHeader, entry.name.c_str(), min(entry.name.size(), (size_t)20));
                    *(DWORD*)&state.fakeHeader[20] = state.overrideSize;

                    DWORD relOffset = targetOffset - entry.dataStart;
                    if (relOffset < 24) {
                        state.fakeHeaderOffset = relOffset;
                        state.overrideVirtualOffset = 0;
                    } else {
                        state.fakeHeaderOffset = 24;
                        state.overrideVirtualOffset = relOffset - 24;
                    }
                    Log("[Loader] [Redirect] Virtual LGP redirected: entry %s -> %S (virtualOffset: %d, relOffset: %d, overrideSize: %d)\n", entry.name.c_str(), overridePath.c_str(), state.virtualFileOffset, relOffset, state.overrideSize);
                    break;
                } else {
                    Log("[Loader] ERROR: [Redirect] Failed to open override file: %S (errno: %d)\n", overridePath.c_str(), errno);
                }
            }
        }
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
            DWORD totalRequested = (DWORD)(size * count);
            DWORD totalBytesRead = 0;
            DWORD bytesToRead = totalRequested;
            BYTE* destBuffer = (BYTE*)buffer;

            if (state.isRedirecting && state.overrideFile) {
                if (state.fakeHeaderOffset < 24) {
                    DWORD headerBytesAvailable = 24 - state.fakeHeaderOffset;
                    DWORD chunk = min(bytesToRead, headerBytesAvailable);
                    memcpy(destBuffer, state.fakeHeader + state.fakeHeaderOffset, chunk);
                    state.fakeHeaderOffset += chunk;
                    destBuffer += chunk;
                    bytesToRead -= chunk;
                    totalBytesRead += chunk;
                }

                if (bytesToRead > 0 && state.overrideVirtualOffset < state.overrideSize) {
                    OriginalFseek(state.overrideFile, state.overrideVirtualOffset, SEEK_SET);
                    size_t actualRead = OriginalFread(destBuffer, 1, bytesToRead, state.overrideFile);
                    state.overrideVirtualOffset += (DWORD)actualRead;
                    totalBytesRead += (DWORD)actualRead;
                }

                state.virtualFileOffset += totalBytesRead;
                // Keep the dummy stream's file pointer in sync
                OriginalFseek(stream, state.virtualFileOffset, SEEK_SET);
                if (OriginalClearerr) OriginalClearerr(stream);

                // If we hit EOF on the override file, force EOF on the dummy stream
                if (state.overrideVirtualOffset >= state.overrideSize) {
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
        if (state.isRedirecting && state.overrideFile) {
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
        if (state.isRedirecting && state.overrideFile) {
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
