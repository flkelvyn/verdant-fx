// Verdant FX — camada de integração gráfica (DXGI proxy + hook de Present)  v0.1.0
//
// STATUS: código-fonte NÃO COMPILADO e NÃO TESTADO NO JOGO. O ambiente de
// desenvolvimento não tem toolchain Windows nem o ETS2. Tudo aqui precisa ser
// compilado (MSVC x64) e validado em uma máquina Windows real. Ver docs/.
//
// Arquitetura (a mesma categoria usada por ReShade/SnowyMoon, implementação própria):
//   1. Este dxgi.dll fica em  ...\Euro Truck Simulator 2\bin\win_x64\ ;
//      o jogo (DirectX 11) o carrega no lugar do dxgi.dll do sistema.
//   2. Os exports CreateDXGIFactory* repassam para o dxgi.dll REAL do System32.
//   3. Na primeira chamada, criamos uma swapchain "dummy" só para ler a vtable e
//      substituir IDXGISwapChain::Present / Present1 / ResizeBuffers.
//   4. No Present: copia o backbuffer, roda o shader de nitidez e escreve de volta.
//
// Não faz: leitura/escrita na memória do jogo, assinaturas/offsets, rede,
// alteração de física/rede/gameplay, nem qualquer ocultação ou bypass de anti-cheat.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "config.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = "0.1.0";

// ------------------------------------------------------------------ estado global
HMODULE            g_self = nullptr;
HMODULE            g_realDxgi = nullptr;
fs::path           g_dir;              // <pasta do dxgi.dll>\VerdantFX
vfx::Settings      g_settings;
std::atomic<bool>  g_initDone{false};
std::atomic<bool>  g_hooksInstalled{false};
std::atomic<bool>  g_inDummy{false};
FILE*              g_log = nullptr;
bool               g_disabledByError = false;  // desativação segura após erro grave

// ------------------------------------------------------------------ log (sem log por quadro)
void Log(int level, const char* fmt, ...) {
    if (!g_log || level > g_settings.logLevel) return;
    SYSTEMTIME t; GetLocalTime(&t);
    std::fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt);
    std::vfprintf(g_log, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

void LoadConfig() {
    std::vector<std::string> warnings;
    vfx::LoadSettings(g_dir / "config.ini", g_settings, warnings);
    for (auto& w : warnings) Log(0, "config: %s", w.c_str());
    Log(1, "config: enabled=%d profile=%s intensity=%.2f halo=%.2f edge=%.2f noise=%.2f veg=%.2f taps=%d",
        g_settings.enabled, vfx::ProfileName(g_settings.profile), g_settings.intensity,
        g_settings.haloSuppression, g_settings.edgePreserve, g_settings.noiseControl,
        g_settings.vegetationFocus, g_settings.taps);
}

// ------------------------------------------------------------------ recursos D3D11
struct Pass {
    IDXGISwapChain*              sc = nullptr;
    ComPtr<ID3D11Device>         dev;
    ComPtr<ID3D11VertexShader>   vs;
    ComPtr<ID3D11PixelShader>    ps;
    ComPtr<ID3D11Buffer>         cb;
    ComPtr<ID3D11RasterizerState>   rs;
    ComPtr<ID3D11BlendState>        bs;
    ComPtr<ID3D11DepthStencilState> ds;
    ComPtr<ID3D11Texture2D>      copyTex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11Texture2D>      lastBackbuffer;  // identifica o buffer do RTV em cache
    ComPtr<ID3D11RenderTargetView>   rtv;
    DXGI_FORMAT                  fmt = DXGI_FORMAT_UNKNOWN;
    UINT                         w = 0, h = 0;
    bool                         srgb = false;
    bool                         shadersReady = false;
    bool                         shaderFailed = false;
};
Pass g_pass;

void ReleaseBackbufferViews() { g_pass.rtv.Reset(); g_pass.lastBackbuffer.Reset(); }
void ResetPass() { g_pass = Pass{}; }

bool IsSrgb(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
bool IsSupportedFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return true;
        default: return false;  // HDR (FP16 etc.) não é suportado nesta versão
    }
}

bool CompileShaders(ID3D11Device* dev) {
    using PFN_D3DCompileFromFile = decltype(&D3DCompileFromFile);
    HMODULE lib = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!lib) { Log(0, "d3dcompiler_47.dll indisponível; efeito desativado."); return false; }
    auto compile = reinterpret_cast<PFN_D3DCompileFromFile>(GetProcAddress(lib, "D3DCompileFromFile"));
    if (!compile) { Log(0, "D3DCompileFromFile ausente."); return false; }

    fs::path file = g_dir / "shaders" / "vegetation_sharpen.hlsl";
    auto build = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& blob) {
        ComPtr<ID3DBlob> err;
        HRESULT hr = compile(file.c_str(), nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                             blob.GetAddressOf(), err.GetAddressOf());
        if (FAILED(hr)) {
            Log(0, "falha ao compilar %s (hr=0x%08lX): %s", entry, static_cast<unsigned long>(hr),
                err ? static_cast<const char*>(err->GetBufferPointer()) : "(sem mensagem)");
            return false;
        }
        return true;
    };
    ComPtr<ID3DBlob> vsb, psb;
    if (!build("VSMain", "vs_5_0", vsb) || !build("PSMain", "ps_5_0", psb)) return false;
    if (FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_pass.vs))) return false;
    if (FAILED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_pass.ps))) return false;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 32;  // 2 x float4
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_pass.cb))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, &g_pass.rs))) return false;
    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bld, &g_pass.bs))) return false;
    D3D11_DEPTH_STENCIL_DESC dsd{};  // depth/stencil desligados
    if (FAILED(dev->CreateDepthStencilState(&dsd, &g_pass.ds))) return false;
    return true;
}

bool EnsureTargets(ID3D11Device* dev, ID3D11Texture2D* bb, const D3D11_TEXTURE2D_DESC& d) {
    if (!g_pass.copyTex || g_pass.w != d.Width || g_pass.h != d.Height || g_pass.fmt != d.Format) {
        g_pass.copyTex.Reset(); g_pass.srv.Reset();
        D3D11_TEXTURE2D_DESC cd{};
        cd.Width = d.Width; cd.Height = d.Height; cd.MipLevels = 1; cd.ArraySize = 1;
        cd.Format = d.Format; cd.SampleDesc.Count = 1; cd.Usage = D3D11_USAGE_DEFAULT;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&cd, nullptr, &g_pass.copyTex))) return false;
        if (FAILED(dev->CreateShaderResourceView(g_pass.copyTex.Get(), nullptr, &g_pass.srv))) return false;
        g_pass.w = d.Width; g_pass.h = d.Height; g_pass.fmt = d.Format; g_pass.srgb = IsSrgb(d.Format);
        Log(1, "recursos recriados: %ux%u formato=%d", d.Width, d.Height, static_cast<int>(d.Format));
    }
    if (!g_pass.rtv || g_pass.lastBackbuffer.Get() != bb) {
        g_pass.rtv.Reset();
        if (FAILED(dev->CreateRenderTargetView(bb, nullptr, &g_pass.rtv))) return false;
        g_pass.lastBackbuffer = bb;
    }
    return true;
}

// ------------------------------------------------------------------ passe de pós-processamento
void RunPass(IDXGISwapChain* sc) {
    ComPtr<ID3D11Device> dev;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(dev.GetAddressOf())))) return;  // não é D3D11
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(bb.GetAddressOf())))) return;
    D3D11_TEXTURE2D_DESC d{}; bb->GetDesc(&d);

    if (g_pass.sc != sc || g_pass.dev.Get() != dev.Get()) { ResetPass(); g_pass.sc = sc; g_pass.dev = dev; }
    if (g_pass.shaderFailed) return;
    if (d.SampleDesc.Count > 1 || !IsSupportedFormat(d.Format)) {
        Log(1, "backbuffer não suportado (MSAA=%u formato=%d); passe ignorado.", d.SampleDesc.Count, static_cast<int>(d.Format));
        g_pass.shaderFailed = true;
        return;
    }
    if (!g_pass.shadersReady) {
        if (!CompileShaders(dev.Get())) { g_pass.shaderFailed = true; return; }
        g_pass.shadersReady = true;
        Log(1, "shaders compilados.");
    }
    if (!EnsureTargets(dev.Get(), bb.Get(), d)) { g_pass.shaderFailed = true; Log(0, "falha ao criar recursos."); return; }

    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);

    // --- salva o estado que vamos alterar
    ComPtr<ID3D11RenderTargetView> oldRtv; ComPtr<ID3D11DepthStencilView> oldDsv;
    ctx->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; D3D11_VIEWPORT oldVp[16];
    ctx->RSGetViewports(&nvp, oldVp);
    ComPtr<ID3D11RasterizerState> oldRs; ctx->RSGetState(&oldRs);
    ComPtr<ID3D11BlendState> oldBs; FLOAT oldBf[4]; UINT oldMask; ctx->OMGetBlendState(&oldBs, oldBf, &oldMask);
    ComPtr<ID3D11DepthStencilState> oldDs; UINT oldRef; ctx->OMGetDepthStencilState(&oldDs, &oldRef);
    ComPtr<ID3D11InputLayout> oldIl; ctx->IAGetInputLayout(&oldIl);
    D3D11_PRIMITIVE_TOPOLOGY oldTopo; ctx->IAGetPrimitiveTopology(&oldTopo);
    ComPtr<ID3D11VertexShader> oldVs; ctx->VSGetShader(&oldVs, nullptr, nullptr);
    ComPtr<ID3D11PixelShader> oldPs;  ctx->PSGetShader(&oldPs, nullptr, nullptr);
    ComPtr<ID3D11GeometryShader> oldGs; ctx->GSGetShader(&oldGs, nullptr, nullptr);
    ComPtr<ID3D11HullShader> oldHs;     ctx->HSGetShader(&oldHs, nullptr, nullptr);
    ComPtr<ID3D11DomainShader> oldDomS; ctx->DSGetShader(&oldDomS, nullptr, nullptr);
    ComPtr<ID3D11ShaderResourceView> oldSrv; ctx->PSGetShaderResources(0, 1, &oldSrv);
    ComPtr<ID3D11Buffer> oldCb; ctx->PSGetConstantBuffers(0, 1, &oldCb);

    // --- copia o backbuffer e aplica o efeito
    ctx->CopyResource(g_pass.copyTex.Get(), bb.Get());

    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(g_pass.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float* p = static_cast<float*>(m.pData);
        p[0] = g_settings.intensity;       p[1] = g_settings.haloSuppression;
        p[2] = g_settings.edgePreserve;    p[3] = g_settings.noiseControl;
        p[4] = g_settings.vegetationFocus; p[5] = g_pass.srgb ? 1.0f : 0.0f;
        p[6] = static_cast<float>(g_settings.taps); p[7] = 0.0f;
        ctx->Unmap(g_pass.cb.Get(), 0);
    }

    D3D11_VIEWPORT vp{0, 0, static_cast<float>(d.Width), static_cast<float>(d.Height), 0.0f, 1.0f};
    ID3D11RenderTargetView* rtv = g_pass.rtv.Get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_pass.rs.Get());
    const FLOAT bf[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(g_pass.bs.Get(), bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_pass.ds.Get(), 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_pass.vs.Get(), nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(g_pass.ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = g_pass.srv.Get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11Buffer* cb = g_pass.cb.Get();
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->Draw(3, 0);

    // --- restaura o estado
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    ID3D11RenderTargetView* orv = oldRtv.Get();
    ctx->OMSetRenderTargets(1, &orv, oldDsv.Get());
    ctx->RSSetViewports(nvp, oldVp);
    ctx->RSSetState(oldRs.Get());
    ctx->OMSetBlendState(oldBs.Get(), oldBf, oldMask);
    ctx->OMSetDepthStencilState(oldDs.Get(), oldRef);
    ctx->IASetInputLayout(oldIl.Get());
    ctx->IASetPrimitiveTopology(oldTopo);
    ctx->VSSetShader(oldVs.Get(), nullptr, 0);
    ctx->PSSetShader(oldPs.Get(), nullptr, 0);
    ctx->GSSetShader(oldGs.Get(), nullptr, 0);
    ctx->HSSetShader(oldHs.Get(), nullptr, 0);
    ctx->DSSetShader(oldDomS.Get(), nullptr, 0);
    ID3D11ShaderResourceView* osrv = oldSrv.Get();
    ctx->PSSetShaderResources(0, 1, &osrv);
    ID3D11Buffer* ocb = oldCb.Get();
    ctx->PSSetConstantBuffers(0, 1, &ocb);
}

// ------------------------------------------------------------------ atalhos (Ctrl+Alt+V liga/desliga, Ctrl+Alt+R recarrega)
void PollHotkeys() {
    static bool wasV = false, wasR = false;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) { wasV = wasR = false; return; }
    const bool mod = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    const bool v = mod && (GetAsyncKeyState('V') & 0x8000);
    const bool r = mod && (GetAsyncKeyState('R') & 0x8000);
    if (v && !wasV) { g_settings.enabled = !g_settings.enabled; Log(1, "toggle: enabled=%d", g_settings.enabled); }
    if (r && !wasR) { LoadConfig(); }
    wasV = v; wasR = r;
}

// ------------------------------------------------------------------ hooks de vtable
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PFN_Resize = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
PFN_Present g_origPresent = nullptr;
PFN_Present1 g_origPresent1 = nullptr;
PFN_Resize g_origResize = nullptr;
thread_local int t_depth = 0;  // evita processar duas vezes se Present chamar Present1 internamente

void BeforePresent(IDXGISwapChain* sc, UINT flags) {
    if (t_depth > 0 || (flags & DXGI_PRESENT_TEST) || g_disabledByError) return;
    PollHotkeys();
    if (!g_settings.enabled) return;
    RunPass(sc);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    BeforePresent(sc, flags);
    ++t_depth;
    HRESULT hr = g_origPresent(sc, sync, flags);
    --t_depth;
    return hr;
}
HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
    BeforePresent(sc, flags);
    ++t_depth;
    HRESULT hr = g_origPresent1(sc, sync, flags, pp);
    --t_depth;
    return hr;
}
HRESULT STDMETHODCALLTYPE HookResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    if (g_pass.sc == sc) ReleaseBackbufferViews();  // o RTV segura o backbuffer; precisa ser liberado
    return g_origResize(sc, n, w, h, f, fl);
}

bool PatchVtable(void** vtbl, size_t index, void* hook, void** orig) {
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[index], sizeof(void*), PAGE_READWRITE, &old)) return false;
    if (!*orig) *orig = vtbl[index];
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
    return true;
}

LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

// Slots: IDXGISwapChain::Present = 8, ResizeBuffers = 13; IDXGISwapChain1::Present1 = 22.
void InstallHooks() {
    if (g_hooksInstalled.exchange(true)) return;
    g_inDummy = true;
    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = DummyProc; wc.hInstance = g_self; wc.lpszClassName = L"VerdantFXDummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 8, 8, nullptr, nullptr, g_self, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1; sd.BufferDesc.Width = 8; sd.BufferDesc.Height = 8;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ComPtr<IDXGISwapChain> sc; ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) {
        Log(0, "swapchain dummy falhou (hr=0x%08lX); efeito desativado com segurança.", static_cast<unsigned long>(hr));
        g_disabledByError = true;
    } else {
        void** vtbl = *reinterpret_cast<void***>(sc.Get());
        bool ok = PatchVtable(vtbl, 8, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_origPresent));
        ok &= PatchVtable(vtbl, 13, reinterpret_cast<void*>(&HookResize), reinterpret_cast<void**>(&g_origResize));
        ComPtr<IDXGISwapChain1> sc1;
        if (SUCCEEDED(sc.As(&sc1))) {
            void** vtbl1 = *reinterpret_cast<void***>(sc1.Get());
            PatchVtable(vtbl1, 22, reinterpret_cast<void*>(&HookPresent1), reinterpret_cast<void**>(&g_origPresent1));
        }
        Log(1, "hooks instalados (ok=%d, feature level=0x%x).", ok, static_cast<unsigned>(fl));
        if (!ok) g_disabledByError = true;
    }
    sc.Reset(); ctx.Reset(); dev.Reset();
    if (hwnd) DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, g_self);
    g_inDummy = false;
}

// ------------------------------------------------------------------ proxy dos exports reais
void EnsureInit() {
    if (g_initDone.exchange(true)) return;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_self, path, MAX_PATH);
    g_dir = fs::path(path).parent_path() / "VerdantFX";
    std::error_code ec; fs::create_directories(g_dir / "logs", ec);
    g_log = _wfopen((g_dir / "logs" / "verdantfx.log").c_str(), L"w");
    LoadConfig();
    Log(1, "Verdant FX %s iniciado (pid=%lu).", kVersion, GetCurrentProcessId());

    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    g_realDxgi = LoadLibraryW((fs::path(sys) / "dxgi.dll").c_str());
    if (!g_realDxgi) Log(0, "não foi possível carregar o dxgi.dll do sistema.");
}

template <typename Fn> Fn Real(const char* name) {
    return g_realDxgi ? reinterpret_cast<Fn>(GetProcAddress(g_realDxgi, name)) : nullptr;
}

void AfterFactory() {
    if (!g_inDummy && !g_hooksInstalled && g_settings.enabled) InstallHooks();
}

} // namespace

// ------------------------------------------------------------------ exports (ver dxgi.def)
extern "C" {

HRESULT WINAPI VFX_CreateDXGIFactory(REFIID riid, void** pp) {
    EnsureInit();
    auto fn = Real<HRESULT(WINAPI*)(REFIID, void**)>("CreateDXGIFactory");
    if (!fn) return E_FAIL;
    HRESULT hr = fn(riid, pp);
    if (SUCCEEDED(hr)) AfterFactory();
    return hr;
}
HRESULT WINAPI VFX_CreateDXGIFactory1(REFIID riid, void** pp) {
    EnsureInit();
    auto fn = Real<HRESULT(WINAPI*)(REFIID, void**)>("CreateDXGIFactory1");
    if (!fn) return E_FAIL;
    HRESULT hr = fn(riid, pp);
    if (SUCCEEDED(hr)) AfterFactory();
    return hr;
}
HRESULT WINAPI VFX_CreateDXGIFactory2(UINT flags, REFIID riid, void** pp) {
    EnsureInit();
    auto fn = Real<HRESULT(WINAPI*)(UINT, REFIID, void**)>("CreateDXGIFactory2");
    if (!fn) return E_FAIL;
    HRESULT hr = fn(flags, riid, pp);
    if (SUCCEEDED(hr)) AfterFactory();
    return hr;
}
HRESULT WINAPI VFX_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** pp) {
    EnsureInit();
    auto fn = Real<HRESULT(WINAPI*)(UINT, REFIID, void**)>("DXGIGetDebugInterface1");
    return fn ? fn(flags, riid, pp) : E_FAIL;
}
HRESULT WINAPI VFX_DXGIDeclareAdapterRemovalSupport() {
    EnsureInit();
    auto fn = Real<HRESULT(WINAPI*)()>("DXGIDeclareAdapterRemovalSupport");
    return fn ? fn() : E_FAIL;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);  // nada além disto sob o loader lock
    } else if (reason == DLL_PROCESS_DETACH && g_log) {
        std::fclose(g_log);
        g_log = nullptr;
    }
    return TRUE;
}
