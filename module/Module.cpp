#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#include "Module.h"
#include "UiControlServer.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <algorithm>
#include <psapi.h>
#include <d3d11.h>
#include <dxgi.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

namespace fm {

    static bool mem_readable(const void* ptr, SIZE_T sz = sizeof(void*))
    {
        if (!ptr) return false;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT)  return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        return ((uintptr_t)ptr + sz) <=
            ((uintptr_t)mbi.BaseAddress + mbi.RegionSize);
    }

    static bool safe_read_ptr(void** out, const void* addr)
    {
        if (!mem_readable(addr, sizeof(void*))) return false;
        *out = *reinterpret_cast<void* const*>(addr);
        return true;
    }

    static void** vtbl_of(const void* obj)
    {
        if (!obj) return nullptr;
        void* vp = nullptr;
        if (!safe_read_ptr(&vp, obj)) return nullptr;
        if (!mem_readable(vp, 128))   return nullptr;
        return reinterpret_cast<void**>(vp);
    }

    static bool patch_vtable_slot(void** vt, size_t slot,
        void* newFn, void** oldFn)
    {
        if (!vt || !newFn) return false;
        void* cell = &vt[slot];
        DWORD oldProt = 0;
        if (!VirtualProtect(cell, sizeof(void*), PAGE_READWRITE, &oldProt))
            return false;
        if (oldFn && !*oldFn) *oldFn = vt[slot];
        vt[slot] = newFn;
        VirtualProtect(cell, sizeof(void*), oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), cell, sizeof(void*));
        return true;
    }

    static IDXGISwapChain* g_swapChain = nullptr;
    static ID3D11Device* g_device = nullptr;
    static ID3D11DeviceContext* g_context = nullptr;
    static ID3D11Texture2D* g_depthSrc = nullptr;
    static ID3D11Texture2D* g_boundDepth = nullptr;
    static ID3D11Texture2D* g_depthSnapshot = nullptr;
    static uint32_t g_depthActivityFrame = 0;
    static uint32_t g_depthDraws = 0;
    static uint32_t g_snapshotDraws = 0;

    static ID3D11Texture2D* g_sharedDepth = nullptr;
    static HANDLE           g_depthHandle = nullptr;
    static uint32_t         g_depthW = 0;
    static uint32_t         g_depthH = 0;
    static DXGI_FORMAT      g_depthFmt = DXGI_FORMAT_UNKNOWN;

    static ID3D11Texture2D* g_sharedColor = nullptr;
    static HANDLE           g_colorHandle = nullptr;
    static uint32_t         g_colorW = 0;
    static uint32_t         g_colorH = 0;
    static DXGI_FORMAT      g_colorFmt = DXGI_FORMAT_UNKNOWN;

    static uint32_t g_vpX = 0;
    static uint32_t g_vpY = 0;
    static uint32_t g_vpW = 0;
    static uint32_t g_vpH = 0;
    static bool     g_vpOK = false;

    typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(
        IDXGISwapChain*, UINT, UINT);

    typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(
        IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

    static PFN_Present         g_origPresent = nullptr;
    static PFN_ResizeBuffers   g_origResizeBuffers = nullptr;

    static CRITICAL_SECTION g_cs;
    static volatile LONG    g_running = 0;
    static HANDLE           g_initThr = nullptr;
    static HANDLE           g_pipeThr = nullptr;
    static volatile LONG    g_pipeStop = 0;
    static HANDLE           g_pipeEvt = nullptr;
    static volatile LONG    g_pipeStarted = 0;
    static qwui::Server g_uiControl;

    static PipePayload   g_payload = {};
    static volatile LONG g_payloadNew = 0;
    static volatile LONG g_frameCount = 0;

    static DXGI_FORMAT depth_to_typeless(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT:            return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D16_UNORM:            return DXGI_FORMAT_R16_TYPELESS;
        default:                               return f;
        }
    }

    static DXGI_FORMAT strip_srgb(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
        default:                              return f;
        }
    }

    static bool is_depth_desc(const D3D11_TEXTURE2D_DESC* d, bool* exact)
    {
        if (!d) return false;
        if (exact) *exact = false;

        if (d->ArraySize != 1) return false;
        if (d->SampleDesc.Count > 1) return false;
        if (!(d->BindFlags & D3D11_BIND_DEPTH_STENCIL)) return false;

        if (exact &&
            d->Format == DXGI_FORMAT_R32_TYPELESS &&
            (d->BindFlags & DEPTH_BINDFLAG_DS_SRV) == DEPTH_BINDFLAG_DS_SRV)
        {
            *exact = true;
            return true;
        }

        switch (d->Format)
        {
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_D16_UNORM:
            return true;
        default:
            return false;
        }
    }


    static const int STABLE_FRAMES = 10;
    static int       g_colorSettle = 0;
    static int       g_depthSettle = 0;
    static uint32_t  g_lastCW = 0;
    static uint32_t  g_lastCH = 0;
    static uint32_t  g_lastDW = 0;
    static uint32_t  g_lastDH = 0;

    static void update_settle(int& settle, uint32_t& lastW, uint32_t& lastH,
        uint32_t w, uint32_t h)
    {
        if (!w || !h)
        {
            settle = 0;
            return;
        }
        if (w != lastW || h != lastH)
        {
            lastW = w;
            lastH = h;
            settle = 0;
        }
        else if (settle < (int)STABLE_FRAMES)
        {
            ++settle;
        }
    }

    static void release_shared_depth()
    {
        if (g_sharedDepth) { g_sharedDepth->Release(); g_sharedDepth = nullptr; }
        g_depthHandle = nullptr;
        g_depthW = 0;
        g_depthH = 0;
        g_depthFmt = DXGI_FORMAT_UNKNOWN;
    }

    static void release_shared_color()
    {
        if (g_sharedColor) { g_sharedColor->Release(); g_sharedColor = nullptr; }
        g_colorHandle = nullptr;
        g_colorW = 0;
        g_colorH = 0;
        g_colorFmt = DXGI_FORMAT_UNKNOWN;
    }

    static bool create_shared_texture(
        uint32_t          w,
        uint32_t          h,
        DXGI_FORMAT       fmt,
        UINT              bindFlags,
        ID3D11Texture2D** outTex,
        HANDLE* outHandle)
    {
        if (!g_device || !w || !h) return false;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = fmt;
        desc.SampleDesc = { 1, 0 };
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = bindFlags;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        ID3D11Texture2D* tex = nullptr;
        HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &tex);
        if (FAILED(hr)) return false;

        IDXGIResource* res = nullptr;
        hr = tex->QueryInterface(__uuidof(IDXGIResource), (void**)&res);
        if (FAILED(hr) || !res)
        {
            tex->Release();
            return false;
        }

        HANDLE sharedHandle = nullptr;
        hr = res->GetSharedHandle(&sharedHandle);
        res->Release();

        if (FAILED(hr) || !sharedHandle)
        {
            tex->Release();
            return false;
        }

        *outTex = tex;
        *outHandle = sharedHandle;
        return true;
    }

    typedef void (STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(
        ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
    typedef void (STDMETHODCALLTYPE* PFN_OMSetRenderTargetsAndUnorderedAccessViews)(
        ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
        UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
    typedef void (STDMETHODCALLTYPE* PFN_ClearDepthStencilView)(
        ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
    typedef void (STDMETHODCALLTYPE* PFN_DrawIndexed)(ID3D11DeviceContext*, UINT count, UINT start, INT base);
    typedef void (STDMETHODCALLTYPE* PFN_Draw)(ID3D11DeviceContext*, UINT count, UINT start);
    typedef void (STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D11DeviceContext*, UINT count, UINT instances, UINT start, INT base, UINT first);
    typedef void (STDMETHODCALLTYPE* PFN_DrawInstanced)(ID3D11DeviceContext*, UINT count, UINT instances, UINT start, UINT first);
    typedef void (STDMETHODCALLTYPE* PFN_DrawAuto)(ID3D11DeviceContext*);
    typedef void (STDMETHODCALLTYPE* PFN_DrawIndexedInstancedIndirect)(ID3D11DeviceContext*, ID3D11Buffer* args, UINT offset);
    typedef void (STDMETHODCALLTYPE* PFN_DrawInstancedIndirect)(ID3D11DeviceContext*, ID3D11Buffer* args, UINT offset);

    static constexpr size_t kOMSetRenderTargetsSlot = 33;
    static constexpr size_t kOMSetRenderTargetsAndUAVSlot = 34;
    static constexpr size_t kClearDepthStencilViewSlot = 53;

    struct ContextHooks
    {
        void** table = nullptr;
        PFN_OMSetRenderTargets setTargets = nullptr;
        PFN_OMSetRenderTargetsAndUnorderedAccessViews setTargetsAndUAV = nullptr;
        PFN_ClearDepthStencilView clearDepth = nullptr;
        PFN_DrawIndexed DrawIndexed = nullptr;
        PFN_Draw Draw = nullptr;
        PFN_DrawIndexedInstanced DrawIndexedInstanced = nullptr;
        PFN_DrawInstanced DrawInstanced = nullptr;
        PFN_DrawAuto DrawAuto = nullptr;
        PFN_DrawIndexedInstancedIndirect DrawIndexedInstancedIndirect = nullptr;
        PFN_DrawInstancedIndirect DrawInstancedIndirect = nullptr;
    };

    static ContextHooks g_contextHooks[8];
    static int g_contextHookCount = 0;
    static SRWLOCK g_contextHookLock = SRWLOCK_INIT;

    struct DepthCandidate
    {
        ID3D11Texture2D* texture = nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        uint32_t frame = 0;
        uint32_t binds = 0;
        uint32_t clears = 0;
    };

    static DepthCandidate g_depthCandidates[32];
    static uint32_t g_referenceW = 0, g_referenceH = 0;
    static int depth_candidate_score(const DepthCandidate&, uint32_t, uint32_t);

    static void reset_depth_history()
    {
        if (g_depthSnapshot) { g_depthSnapshot->Release(); g_depthSnapshot = nullptr; }
        g_depthActivityFrame = 0;
        g_depthDraws = g_snapshotDraws = 0;
    }

    static void advance_depth_activity(uint32_t frame)
    {
        if (g_depthActivityFrame != frame)
        {
            g_depthActivityFrame = frame;
            g_depthDraws = g_snapshotDraws = 0;
        }
    }

    static void reset_depth_candidates()
    {
        g_boundDepth = nullptr;
        reset_depth_history();
        if (g_depthSrc) { g_depthSrc->Release(); g_depthSrc = nullptr; }
        g_referenceW = g_referenceH = 0;
        for (auto& candidate : g_depthCandidates)
        {
            if (candidate.texture) candidate.texture->Release();
            candidate = {};
        }
    }

    static ContextHooks context_originals(ID3D11DeviceContext* context)
    {
        ContextHooks result{};
        void** table = vtbl_of(context);
        AcquireSRWLockShared(&g_contextHookLock);
        for (int i = 0; i < g_contextHookCount; ++i)
        {
            if (g_contextHooks[i].table == table) { result = g_contextHooks[i]; break; }
        }
        ReleaseSRWLockShared(&g_contextHookLock);
        return result;
    }

    static void observe_depth_view(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, bool binding)
    {
        if (!context || !InterlockedCompareExchange(&g_running, 0, 0)) return;
        if (binding)
        {
            EnterCriticalSection(&g_cs);
            if (context == g_context) g_boundDepth = nullptr;
            LeaveCriticalSection(&g_cs);
        }
        if (!view) return;
        ID3D11Device* device = nullptr;
        context->GetDevice(&device);
        if (!device) return;
        ID3D11Resource* resource = nullptr;
        view->GetResource(&resource);
        ID3D11Texture2D* texture = nullptr;
        if (resource)
        {
            resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture);
            resource->Release();
        }
        if (!texture) { device->Release(); return; }
        ID3D11Device* textureDevice = nullptr;
        texture->GetDevice(&textureDevice);
        const bool sameDevice = textureDevice == device;
        if (textureDevice) textureDevice->Release();
        if (!sameDevice) { texture->Release(); device->Release(); return; }
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc{};
        view->GetDesc(&viewDesc);
        if (!is_depth_desc(&desc, nullptr) || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
            desc.Width < 64 || desc.Height < 64 ||
            viewDesc.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || viewDesc.Texture2D.MipSlice != 0)
        {
            texture->Release(); device->Release(); return;
        }

        EnterCriticalSection(&g_cs);
        if (device == g_device && InterlockedCompareExchange(&g_running, 0, 0))
        {
            if (binding && context == g_context) g_boundDepth = texture;
            const uint32_t frame = static_cast<uint32_t>(InterlockedCompareExchange(&g_frameCount, 0, 0)) + 1;
            DepthCandidate* slot = nullptr;
            DepthCandidate* oldest = nullptr;
            for (auto& candidate : g_depthCandidates)
            {
                if (candidate.texture == texture) { slot = &candidate; break; }
                if (!candidate.texture && !slot) slot = &candidate;
                if (candidate.texture && (!oldest || uint32_t(frame - candidate.frame) > uint32_t(frame - oldest->frame)))
                    oldest = &candidate;
            }
            if (!slot && oldest)
            {
                if (!g_referenceW || !g_referenceH) slot = oldest;
                else
                {
                    DepthCandidate incoming{}; incoming.desc = desc;
                    incoming.binds = binding ? 1 : 0;
                    const int incomingScore = depth_candidate_score(incoming, g_referenceW, g_referenceH);
                    DepthCandidate* worst = nullptr; int worstScore = 0x7fffffff;
                    for (auto& candidate : g_depthCandidates)
                    {
                        int score = depth_candidate_score(candidate, g_referenceW, g_referenceH);
                        if (uint32_t(frame - candidate.frame) > 2) score = -10000;
                        if (score < worstScore) { worstScore = score; worst = &candidate; }
                    }
                    if (worst && incomingScore >= 0 && incomingScore > worstScore) slot = worst;
                }
            }
            if (slot)
            {
                if (slot->texture != texture)
                {
                    if (slot->texture) slot->texture->Release();
                    *slot = {};
                    slot->texture = texture;
                    texture = nullptr;
                    slot->desc = desc;
                }
                if (slot->frame != frame)
                {
                    slot->frame = frame; slot->binds = 0; slot->clears = 0;
                }
                if (binding) { if (slot->binds < 4096) ++slot->binds; }
                else { if (slot->clears < 4096) ++slot->clears; }
            }
        }
        LeaveCriticalSection(&g_cs);
        if (texture) texture->Release();
        device->Release();
    }

    static void note_depth_draw(ID3D11DeviceContext* context)
    {
        if (!InterlockedCompareExchange(&g_running, 0, 0)) return;
        EnterCriticalSection(&g_cs);
        if (context == g_context && g_depthSrc && g_boundDepth == g_depthSrc)
        {
            advance_depth_activity(static_cast<uint32_t>(InterlockedCompareExchange(&g_frameCount, 0, 0)) + 1);
            if (g_depthDraws != UINT32_MAX) ++g_depthDraws;
        }
        LeaveCriticalSection(&g_cs);
    }

    static void snapshot_before_depth_clear(ID3D11DeviceContext* context,
        ID3D11DepthStencilView* view, UINT flags)
    {
        if (!view || !(flags & D3D11_CLEAR_DEPTH) ||
            !InterlockedCompareExchange(&g_running, 0, 0)) return;
        EnterCriticalSection(&g_cs);
        if (context == g_context && g_depthSrc)
        {
            ID3D11Resource* resource = nullptr;
            view->GetResource(&resource);
            ID3D11Texture2D* texture = nullptr;
            if (resource)
            {
                resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture);
                resource->Release();
            }
            if (texture == g_depthSrc)
            {
                advance_depth_activity(static_cast<uint32_t>(InterlockedCompareExchange(&g_frameCount, 0, 0)) + 1);
                if (g_depthDraws > g_snapshotDraws)
                {
                    if (!g_depthSnapshot)
                    {
                        D3D11_TEXTURE2D_DESC desc{};
                        texture->GetDesc(&desc);
                        desc.MiscFlags = 0;
                        desc.Usage = D3D11_USAGE_DEFAULT;
                        desc.CPUAccessFlags = 0;
                        g_device->CreateTexture2D(&desc, nullptr, &g_depthSnapshot);
                    }
                    if (g_depthSnapshot)
                    {
                        context->CopyResource(g_depthSnapshot, texture);
                        g_snapshotDraws = g_depthDraws;
                    }
                }
                g_depthDraws = 0;
            }
            if (texture) texture->Release();
        }
        LeaveCriticalSection(&g_cs);
    }

    static void STDMETHODCALLTYPE hook_DrawIndexed(ID3D11DeviceContext* self, UINT count, UINT start, INT base)
    {
        const ContextHooks original = context_originals(self);
        if (original.DrawIndexed)
        {
            original.DrawIndexed(self, count, start, base);
            note_depth_draw(self);
        }
    }

    static void STDMETHODCALLTYPE hook_Draw(ID3D11DeviceContext* self, UINT count, UINT start)
    {
        const ContextHooks original = context_originals(self);
        if (original.Draw)
        {
            original.Draw(self, count, start);
            note_depth_draw(self);
        }
    }

    static void STDMETHODCALLTYPE hook_DrawIndexedInstanced(ID3D11DeviceContext* self, UINT count, UINT instances, UINT start, INT base, UINT first)
    {
        const ContextHooks original = context_originals(self);
        if (original.DrawIndexedInstanced)
        {
            original.DrawIndexedInstanced(self, count, instances, start, base, first);
            note_depth_draw(self);
        }
    }

    static void STDMETHODCALLTYPE hook_DrawInstanced(ID3D11DeviceContext* self, UINT count, UINT instances, UINT start, UINT first)
    {
        const ContextHooks original = context_originals(self);
        if (original.DrawInstanced)
        {
            original.DrawInstanced(self, count, instances, start, first);
            note_depth_draw(self);
        }
    }

    static void STDMETHODCALLTYPE hook_DrawAuto(ID3D11DeviceContext* self)
    {
        const ContextHooks original = context_originals(self);
        if (original.DrawAuto)
        {
            original.DrawAuto(self);
            note_depth_draw(self);
        }
    }

    static void STDMETHODCALLTYPE hook_DrawIndexedInstancedIndirect(ID3D11DeviceContext* self, ID3D11Buffer* args, UINT offset)
    {
        const ContextHooks original = context_originals(self);
        if (original.DrawIndexedInstancedIndirect)
        {
            original.DrawIndexedInstancedIndirect(self, args, offset);
            note_depth_draw(self);
        }
    }

    static void STDMETHODCALLTYPE hook_DrawInstancedIndirect(ID3D11DeviceContext* self, ID3D11Buffer* args, UINT offset)
    {
        const ContextHooks original = context_originals(self);
        if (original.DrawInstancedIndirect)
        {
            original.DrawInstancedIndirect(self, args, offset);
            note_depth_draw(self);
        }
    }

    static void STDMETHODCALLTYPE hook_OMSetRenderTargets(ID3D11DeviceContext* self, UINT count,
        ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* depth)
    {
        const ContextHooks original = context_originals(self);
        if (original.setTargets) original.setTargets(self, count, targets, depth);
        observe_depth_view(self, depth, true);
    }

    static void STDMETHODCALLTYPE hook_OMSetRenderTargetsAndUnorderedAccessViews(ID3D11DeviceContext* self,
        UINT count, ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* depth,
        UINT start, UINT uavCount, ID3D11UnorderedAccessView* const* uavs, const UINT* initialCounts)
    {
        const ContextHooks original = context_originals(self);
        if (original.setTargetsAndUAV)
            original.setTargetsAndUAV(self, count, targets, depth, start, uavCount, uavs, initialCounts);
        if (count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) observe_depth_view(self, depth, true);
    }

    static void STDMETHODCALLTYPE hook_ClearDepthStencilView(ID3D11DeviceContext* self,
        ID3D11DepthStencilView* depth, UINT flags, FLOAT value, UINT8 stencil)
    {
        observe_depth_view(self, depth, false);
        snapshot_before_depth_clear(self, depth, flags);
        const ContextHooks original = context_originals(self);
        if (original.clearDepth) original.clearDepth(self, depth, flags, value, stencil);
    }

    static void ensure_context_hooked(ID3D11DeviceContext* context)
    {
        void** table = vtbl_of(context);
        if (!table) return;
        AcquireSRWLockExclusive(&g_contextHookLock);
        for (int i = 0; i < g_contextHookCount; ++i)
        {
            if (g_contextHooks[i].table == table)
            {
                ReleaseSRWLockExclusive(&g_contextHookLock); return;
            }
        }
        if (g_contextHookCount == 8)
        {
            ReleaseSRWLockExclusive(&g_contextHookLock); return;
        }
        ContextHooks original{}; original.table = table;
        void* set = nullptr; void* uav = nullptr; void* clear = nullptr;
        if (!safe_read_ptr(&set, table + kOMSetRenderTargetsSlot) ||
            !safe_read_ptr(&uav, table + kOMSetRenderTargetsAndUAVSlot) ||
            !safe_read_ptr(&clear, table + kClearDepthStencilViewSlot) || !set || !uav || !clear)
        {
            ReleaseSRWLockExclusive(&g_contextHookLock); return;
        }
        original.setTargets = reinterpret_cast<PFN_OMSetRenderTargets>(set);
        original.setTargetsAndUAV = reinterpret_cast<PFN_OMSetRenderTargetsAndUnorderedAccessViews>(uav);
        original.clearDepth = reinterpret_cast<PFN_ClearDepthStencilView>(clear);
        void* pDrawIndexed = nullptr;
        if (!safe_read_ptr(&pDrawIndexed, table + 12) || !pDrawIndexed)
        {
            ReleaseSRWLockExclusive(&g_contextHookLock); return;
        }
        original.DrawIndexed = reinterpret_cast<PFN_DrawIndexed>(pDrawIndexed);
        void* pDraw = nullptr;
        if (!safe_read_ptr(&pDraw, table + 13) || !pDraw)
        {
            ReleaseSRWLockExclusive(&g_contextHookLock); return;
        }
        original.Draw = reinterpret_cast<PFN_Draw>(pDraw);
        void* pDrawIndexedInstanced = nullptr;
        if (!safe_read_ptr(&pDrawIndexedInstanced, table + 20) || !pDrawIndexedInstanced)
        {
            ReleaseSRWLockExclusive(&g_contextHookLock); return;
        }
        original.DrawIndexedInstanced = reinterpret_cast<PFN_DrawIndexedInstanced>(pDrawIndexedInstanced);
        void* pDrawInstanced = nullptr;
        if (!safe_read_ptr(&pDrawInstanced, table + 21) || !pDrawInstanced)
        {
            ReleaseSRWLockExclusive(&g_contextHookLock); return;
        }
        original.DrawInstanced = reinterpret_cast<PFN_DrawInstanced>(pDrawInstanced);
        void* pDrawAuto = nullptr;
        if (!safe_read_ptr(&pDrawAuto, table + 38) || !pDrawAuto)
        {
            ReleaseSRWLockExclusive(&g_contextHookLock); return;
        }
        original.DrawAuto = reinterpret_cast<PFN_DrawAuto>(pDrawAuto);
        void* pDrawIndexedInstancedIndirect = nullptr;
        if (!safe_read_ptr(&pDrawIndexedInstancedIndirect, table + 39) || !pDrawIndexedInstancedIndirect)
        {
            ReleaseSRWLockExclusive(&g_contextHookLock); return;
        }
        original.DrawIndexedInstancedIndirect = reinterpret_cast<PFN_DrawIndexedInstancedIndirect>(pDrawIndexedInstancedIndirect);
        void* pDrawInstancedIndirect = nullptr;
        if (!safe_read_ptr(&pDrawInstancedIndirect, table + 40) || !pDrawInstancedIndirect)
        {
            ReleaseSRWLockExclusive(&g_contextHookLock); return;
        }
        original.DrawInstancedIndirect = reinterpret_cast<PFN_DrawInstancedIndirect>(pDrawInstancedIndirect);
        g_contextHooks[g_contextHookCount++] = original;
        patch_vtable_slot(table, 12, (void*)hook_DrawIndexed, nullptr);
        patch_vtable_slot(table, 13, (void*)hook_Draw, nullptr);
        patch_vtable_slot(table, 20, (void*)hook_DrawIndexedInstanced, nullptr);
        patch_vtable_slot(table, 21, (void*)hook_DrawInstanced, nullptr);
        patch_vtable_slot(table, 38, (void*)hook_DrawAuto, nullptr);
        patch_vtable_slot(table, 39, (void*)hook_DrawIndexedInstancedIndirect, nullptr);
        patch_vtable_slot(table, 40, (void*)hook_DrawInstancedIndirect, nullptr);
        patch_vtable_slot(table, kOMSetRenderTargetsSlot, (void*)hook_OMSetRenderTargets, nullptr);
        patch_vtable_slot(table, kOMSetRenderTargetsAndUAVSlot, (void*)hook_OMSetRenderTargetsAndUnorderedAccessViews, nullptr);
        patch_vtable_slot(table, kClearDepthStencilViewSlot, (void*)hook_ClearDepthStencilView, nullptr);
        ReleaseSRWLockExclusive(&g_contextHookLock);
    }

    static void ensure_device_contexts_hooked(ID3D11Device* device)
    {
        if (!device) return;
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        if (context) { ensure_context_hooked(context); context->Release(); }
        context = nullptr;
        if (SUCCEEDED(device->CreateDeferredContext(0, &context)) && context)
        {
            ensure_context_hooked(context); context->Release();
        }
    }

    static int depth_candidate_score(const DepthCandidate& candidate, uint32_t width, uint32_t height)
    {
        const auto& desc = candidate.desc;
        if (!width || !height || !desc.Width || !desc.Height ||
            uint64_t(desc.Width) > uint64_t(width) * 2 || uint64_t(desc.Height) > uint64_t(height) * 2) return -1;
        const uint64_t a = uint64_t(desc.Width) * height;
        const uint64_t b = uint64_t(desc.Height) * width;
        const uint64_t aspectError = (a > b ? a - b : b - a) * 1000 / std::max(a, b);
        if (aspectError > 150) return -1;
        const uint64_t dx = desc.Width > width ? desc.Width - width : width - desc.Width;
        const uint64_t dy = desc.Height > height ? desc.Height - height : height - desc.Height;
        int score = 5000 - int(dx * 1500 / width + dy * 1500 / height + aspectError * 4);
        if (desc.Width == width && desc.Height == height) score += 5000;
        if (desc.Format == DXGI_FORMAT_R32_TYPELESS || desc.Format == DXGI_FORMAT_D32_FLOAT) score += 100;
        else if (desc.Format == DXGI_FORMAT_R24G8_TYPELESS || desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT) score += 50;
        score += int(std::min(candidate.binds, uint32_t(32))) * 8;
        score += int(std::min(candidate.clears, uint32_t(8))) * 2;
        if (!candidate.binds) score -= 500;
        return score;
    }

    static void select_depth_candidate(uint32_t frame, uint32_t width, uint32_t height)
    {
        DepthCandidate* best = nullptr;
        int bestScore = -1;
        for (auto& candidate : g_depthCandidates)
        {
            if (!candidate.texture) continue;
            const int32_t age = static_cast<int32_t>(frame - candidate.frame);
            if (age < 0) continue;
            if (age > 120)
            {
                candidate.texture->Release(); candidate = {};
                continue;
            }
            if (age > 2) continue;
            int score = depth_candidate_score(candidate, width, height);
            if (score < 0) continue;
            if (age == 0) score += 500;
            if (candidate.texture == g_depthSrc) score += 16;
            if (score > bestScore) { bestScore = score; best = &candidate; }
        }
        ID3D11Texture2D* selected = best ? best->texture : nullptr;
        if (selected != g_depthSrc)
        {
            reset_depth_history();
            if (selected) selected->AddRef();
            if (g_depthSrc) g_depthSrc->Release();
            g_depthSrc = selected;
        }
    }

    static void sample_bound_depth()
    {
        if (!g_context) return;
        ID3D11DepthStencilView* view = nullptr;
        g_context->OMGetRenderTargets(0, nullptr, &view);
        if (view) { observe_depth_view(g_context, view, true); view->Release(); }
    }

    static void restore_context_hooks()
    {
        AcquireSRWLockExclusive(&g_contextHookLock);
        for (int i = 0; i < g_contextHookCount; ++i)
        {
            const auto& hook = g_contextHooks[i];
            void* current = nullptr;
            if (safe_read_ptr(&current, hook.table + kOMSetRenderTargetsSlot) && current == (void*)hook_OMSetRenderTargets)
                patch_vtable_slot(hook.table, kOMSetRenderTargetsSlot, (void*)hook.setTargets, nullptr);
            if (safe_read_ptr(&current, hook.table + kOMSetRenderTargetsAndUAVSlot) && current == (void*)hook_OMSetRenderTargetsAndUnorderedAccessViews)
                patch_vtable_slot(hook.table, kOMSetRenderTargetsAndUAVSlot, (void*)hook.setTargetsAndUAV, nullptr);
            if (safe_read_ptr(&current, hook.table + kClearDepthStencilViewSlot) && current == (void*)hook_ClearDepthStencilView)
                patch_vtable_slot(hook.table, kClearDepthStencilViewSlot, (void*)hook.clearDepth, nullptr);
            if (safe_read_ptr(&current, hook.table + 12) && current == (void*)hook_DrawIndexed)
                patch_vtable_slot(hook.table, 12, (void*)hook.DrawIndexed, nullptr);
            if (safe_read_ptr(&current, hook.table + 13) && current == (void*)hook_Draw)
                patch_vtable_slot(hook.table, 13, (void*)hook.Draw, nullptr);
            if (safe_read_ptr(&current, hook.table + 20) && current == (void*)hook_DrawIndexedInstanced)
                patch_vtable_slot(hook.table, 20, (void*)hook.DrawIndexedInstanced, nullptr);
            if (safe_read_ptr(&current, hook.table + 21) && current == (void*)hook_DrawInstanced)
                patch_vtable_slot(hook.table, 21, (void*)hook.DrawInstanced, nullptr);
            if (safe_read_ptr(&current, hook.table + 38) && current == (void*)hook_DrawAuto)
                patch_vtable_slot(hook.table, 38, (void*)hook.DrawAuto, nullptr);
            if (safe_read_ptr(&current, hook.table + 39) && current == (void*)hook_DrawIndexedInstancedIndirect)
                patch_vtable_slot(hook.table, 39, (void*)hook.DrawIndexedInstancedIndirect, nullptr);
            if (safe_read_ptr(&current, hook.table + 40) && current == (void*)hook_DrawInstancedIndirect)
                patch_vtable_slot(hook.table, 40, (void*)hook.DrawInstancedIndirect, nullptr);
        }
        ReleaseSRWLockExclusive(&g_contextHookLock);
    }

    HRESULT STDMETHODCALLTYPE hook_Present(IDXGISwapChain*, UINT, UINT);
    HRESULT STDMETHODCALLTYPE hook_ResizeBuffers(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

    static void scan_and_patch_swapchain_vtables(void** refVt)
    {
        if (!refVt) return;

        void* pPresent = nullptr;
        void* pGetBuffer = nullptr;
        void* pGetDesc = nullptr;
        void* pResizeBuffers = nullptr;
        if (!safe_read_ptr(&pPresent, &refVt[VT_PRESENT]))         return;
        if (!safe_read_ptr(&pGetBuffer, &refVt[9]))                return;
        if (!safe_read_ptr(&pGetDesc, &refVt[12]))                 return;
        if (!safe_read_ptr(&pResizeBuffers, &refVt[VT_RESIZEBUFFERS])) return;
        if (!pPresent || !pGetBuffer || !pGetDesc || !pResizeBuffers) return;

        HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
        if (!dxgi) return;

        MODULEINFO mi = {};
        if (!GetModuleInformation(GetCurrentProcess(), dxgi, &mi, sizeof(mi))) return;
        uintptr_t lo = (uintptr_t)mi.lpBaseOfDll;
        uintptr_t hi = lo + mi.SizeOfImage;

        for (uintptr_t a = lo; a + 14 * sizeof(void*) <= hi; a += sizeof(void*))
        {
            const void* cell = (const void*)a;
            if (!mem_readable(cell, 14 * sizeof(void*))) continue;

            void* const* slots = (void* const*)a;
            if (slots[VT_PRESENT] != pPresent)   continue;
            if (slots[9] != pGetBuffer) continue;
            if (slots[12] != pGetDesc)   continue;
            if (slots[VT_RESIZEBUFFERS] != pResizeBuffers) continue;
            if ((uintptr_t)a == (uintptr_t)refVt) continue;

            patch_vtable_slot((void**)a, VT_PRESENT, (void*)hook_Present,
                (void**)&g_origPresent);
            patch_vtable_slot((void**)a, VT_RESIZEBUFFERS, (void*)hook_ResizeBuffers,
                (void**)&g_origResizeBuffers);
        }
    }

    static bool wait_pipe_io(HANDLE pipe, OVERLAPPED& ov, DWORD timeout, DWORD& transferred)
    {
        const ULONGLONG deadline = GetTickCount64() + timeout;
        while (!InterlockedCompareExchange(&g_pipeStop, 0, 0) && GetTickCount64() < deadline)
        {
            g_uiControl.Tick();
            const DWORD wait = WaitForSingleObject(ov.hEvent, 20);
            if (wait == WAIT_OBJECT_0) return GetOverlappedResult(pipe, &ov, &transferred, FALSE) != FALSE;
            if (wait == WAIT_FAILED) break;
        }
        CancelIoEx(pipe, &ov);
         
        GetOverlappedResult(pipe, &ov, &transferred, TRUE);
        return false;
    }

    static DWORD WINAPI pipe_thread(LPVOID)
    {
        HANDLE writeEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!writeEvt) return 0;
        g_uiControl.Initialize();

        while (!InterlockedCompareExchange(&g_pipeStop, 0, 0))
        {
            g_uiControl.Tick();
            HANDLE hPipe = CreateNamedPipeW(
                PIPE_NAME,
                PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                (DWORD)(sizeof(PipePayload) * 8), 0, 0, nullptr);

            if (hPipe == INVALID_HANDLE_VALUE)
            {
                Sleep(200);
                continue;
            }

            HANDLE     connEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            OVERLAPPED ovConn = {};
            ovConn.hEvent = connEvt;

            bool connected = false;
            if (connEvt)
            {
                const BOOL immediate = ConnectNamedPipe(hPipe, &ovConn);
                const DWORD connErr = immediate ? ERROR_SUCCESS : GetLastError();
                if (immediate || connErr == ERROR_PIPE_CONNECTED) connected = true;
                else if (connErr == ERROR_IO_PENDING)
                {
                    DWORD dummy = 0;
                    connected = wait_pipe_io(hPipe, ovConn, 5000, dummy);
                }
            }
            CloseHandle(connEvt);

            if (!connected)
            {
                CloseHandle(hPipe);
                Sleep(20);
                continue;
            }

            while (!InterlockedCompareExchange(&g_pipeStop, 0, 0))
            {
                g_uiControl.Tick();
                WaitForSingleObject(g_pipeEvt, 20);
                 
                if (!InterlockedExchange(&g_payloadNew, 0)) continue;

                EnterCriticalSection(&g_cs);
                PipePayload p = g_payload;
                LeaveCriticalSection(&g_cs);

                OVERLAPPED ovWrite = {};
                ovWrite.hEvent = writeEvt;
                ResetEvent(writeEvt);

                DWORD wrote = 0;
                BOOL  ok = WriteFile(hPipe, &p, (DWORD)sizeof(p), &wrote, &ovWrite);
                DWORD wErr = GetLastError();

                if (!ok && wErr == ERROR_IO_PENDING)
                {
                    ok = wait_pipe_io(hPipe, ovWrite, 200, wrote);
                }

                if (!ok || wrote != (DWORD)sizeof(p)) break;
            }

            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }

        g_uiControl.Close();
        CloseHandle(writeEvt);
        return 0;
    }

    HRESULT STDMETHODCALLTYPE hook_Present(IDXGISwapChain* self,
        UINT SyncInterval,
        UINT Flags)
    {
        if (!self || (Flags & DXGI_PRESENT_TEST) || !InterlockedCompareExchange(&g_running, 0, 0)) goto CALL_ORIG;

        if (!g_device || g_swapChain != self)
        {
            EnterCriticalSection(&g_cs);
            reset_depth_candidates();
            release_shared_depth();
            release_shared_color();
            g_colorSettle = g_depthSettle = 0;
            g_lastCW = g_lastCH = g_lastDW = g_lastDH = 0;
            if (g_context) { g_context->Release(); g_context = nullptr; }
            if (g_device) { g_device->Release();  g_device = nullptr; }

            g_swapChain = self;
            if (SUCCEEDED(self->GetDevice(__uuidof(ID3D11Device), (void**)&g_device)) && g_device)
            {
                g_device->GetImmediateContext(&g_context);
                ensure_device_contexts_hooked(g_device);
            }
            LeaveCriticalSection(&g_cs);
        }

        if (!g_device || !g_context) goto CALL_ORIG;

        if (InterlockedCompareExchange(&g_pipeStarted, 1, 0) == 0)
        {
            g_pipeEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (g_pipeEvt) g_pipeThr = CreateThread(nullptr, 0, pipe_thread, nullptr, 0, nullptr);
            if (!g_pipeThr)
            {
                if (g_pipeEvt) CloseHandle(g_pipeEvt);
                g_pipeEvt = nullptr;
                InterlockedExchange(&g_pipeStarted, 0);  
            }
        }

        {
            ID3D11Texture2D* frameBackBuffer = nullptr;
            const HRESULT frameBackBufferHR = self->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&frameBackBuffer);
            D3D11_TEXTURE2D_DESC frameDesc{};
            if (SUCCEEDED(frameBackBufferHR) && frameBackBuffer) frameBackBuffer->GetDesc(&frameDesc);
            sample_bound_depth();
            PipePayload p = {};
            p.frameCount = (uint32_t)InterlockedIncrement(&g_frameCount);
            uint32_t newFlags = 0;

            EnterCriticalSection(&g_cs);
            g_referenceW = frameDesc.Width; g_referenceH = frameDesc.Height;
            select_depth_candidate(p.frameCount, frameDesc.Width, frameDesc.Height);
            ID3D11Texture2D* depthSrc = g_depthSrc;
            if (g_depthSnapshot && g_depthActivityFrame == p.frameCount &&
                g_snapshotDraws > g_depthDraws)
                depthSrc = g_depthSnapshot;
            if (depthSrc) depthSrc->AddRef();
            LeaveCriticalSection(&g_cs);

            if (depthSrc)
            {
                D3D11_TEXTURE2D_DESC sd;
                depthSrc->GetDesc(&sd);

                uint32_t    dw = sd.Width;
                uint32_t    dh = sd.Height;
                DXGI_FORMAT fmt = depth_to_typeless(sd.Format);

                update_settle(g_depthSettle, g_lastDW, g_lastDH, dw, dh);

                if (g_depthSettle >= (int)STABLE_FRAMES &&
                    (!g_sharedDepth ||
                        g_depthW != dw ||
                        g_depthH != dh ||
                        g_depthFmt != fmt))
                {
                    release_shared_depth();
                    create_shared_texture(dw, dh, fmt,
                        D3D11_BIND_SHADER_RESOURCE,
                        &g_sharedDepth, &g_depthHandle);
                    g_depthW = dw;
                    g_depthH = dh;
                    g_depthFmt = fmt;
                }

                if (g_sharedDepth && g_depthW == dw && g_depthH == dh && g_depthFmt == fmt)
                {
                    g_context->CopyResource(g_sharedDepth, depthSrc);

                    p.depthHandle = (uint64_t)(uintptr_t)g_depthHandle;
                    p.depthWidth = g_depthW;
                    p.depthHeight = g_depthH;
                    p.depthFormat = (uint32_t)g_depthFmt;
                    newFlags |= FLAG_DEPTH_OK;
                }
                else
                {
                    p.depthWidth = dw;
                    p.depthHeight = dh;
                    p.depthFormat = (uint32_t)fmt;
                }

                depthSrc->Release();
            }

            {
                ID3D11Texture2D* bb = frameBackBuffer;
                const HRESULT hbb = frameBackBufferHR;

                if (SUCCEEDED(hbb) && bb)
                {
                    D3D11_TEXTURE2D_DESC bd;
                    bb->GetDesc(&bd);

                    DXGI_FORMAT colorFmt = strip_srgb(bd.Format);
                    uint32_t targetW = bd.Width;
                    uint32_t targetH = bd.Height;

                    update_settle(g_colorSettle, g_lastCW, g_lastCH, targetW, targetH);

                    if (g_colorSettle >= (int)STABLE_FRAMES &&
                        (!g_sharedColor ||
                            g_colorW != targetW ||
                            g_colorH != targetH ||
                            g_colorFmt != colorFmt))
                    {
                        release_shared_color();
                        create_shared_texture(targetW, targetH, colorFmt,
                            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                            &g_sharedColor, &g_colorHandle);
                        g_colorW = targetW;
                        g_colorH = targetH;
                        g_colorFmt = colorFmt;
                    }

                    if (g_sharedColor && g_colorW == targetW && g_colorH == targetH &&
                        g_colorFmt == colorFmt && bd.SampleDesc.Count == 1)
                    {
                        g_context->CopyResource(g_sharedColor, bb);

                        p.colorHandle = (uint64_t)(uintptr_t)g_colorHandle;
                        p.colorWidth = g_colorW;
                        p.colorHeight = g_colorH;
                        p.colorFormat = (uint32_t)g_colorFmt;
                        newFlags |= FLAG_COLOR_OK;

                        g_vpX = 0;
                        g_vpY = 0;
                        g_vpW = targetW;
                        g_vpH = targetH;
                        g_vpOK = true;
                    }
                    else
                    {
                        p.colorWidth = targetW;
                        p.colorHeight = targetH;
                        p.colorFormat = (uint32_t)colorFmt;
                    }

                    bb->Release();
                }
            }

            if (g_vpOK)
            {
                p.vpX = g_vpX;
                p.vpY = g_vpY;
                p.vpWidth = g_vpW;
                p.vpHeight = g_vpH;
                newFlags |= FLAG_VIEWPORT_OK;
            }
            else
            {
                p.vpX = 0;
                p.vpY = 0;
                p.vpWidth = p.colorWidth;
                p.vpHeight = p.colorHeight;
            }

            if (newFlags & (FLAG_COLOR_OK | FLAG_DEPTH_OK))
                g_context->Flush();
            p.flags = newFlags | FLAG_FULL_FRAME;

            EnterCriticalSection(&g_cs);
            g_payload = p;
            LeaveCriticalSection(&g_cs);
            InterlockedExchange(&g_payloadNew, 1);
            if (g_pipeEvt) SetEvent(g_pipeEvt);
        }

    CALL_ORIG:
        return g_origPresent
            ? g_origPresent(self, SyncInterval, Flags)
            : S_OK;
    }

    HRESULT STDMETHODCALLTYPE hook_ResizeBuffers(IDXGISwapChain* self,
        UINT   BufferCount,
        UINT   Width,
        UINT   Height,
        DXGI_FORMAT NewFormat,
        UINT   SwapChainFlags)
    {
        EnterCriticalSection(&g_cs);
        reset_depth_candidates();
        g_payload = {};
        release_shared_color();
        release_shared_depth();
        g_vpOK = false;
        g_colorSettle = 0;
        g_depthSettle = 0;
        g_lastCW = 0;
        g_lastCH = 0;
        g_lastDW = 0;
        g_lastDH = 0;
        LeaveCriticalSection(&g_cs);

        return g_origResizeBuffers
            ? g_origResizeBuffers(self, BufferCount, Width, Height,
                NewFormat, SwapChainFlags)
            : S_OK;
    }

    static bool install_hooks(IDXGISwapChain* sc, ID3D11Device* dev)
    {
        void** scVtbl = vtbl_of(sc);
        if (!scVtbl) return false;

        void* oldP = nullptr;
        void* oldRB = nullptr;

        if (!patch_vtable_slot(scVtbl, VT_PRESENT, (void*)hook_Present, &oldP)) return false;
        g_origPresent = (PFN_Present)oldP;

        if (!patch_vtable_slot(scVtbl, VT_RESIZEBUFFERS, (void*)hook_ResizeBuffers, &oldRB)) return false;
        g_origResizeBuffers = (PFN_ResizeBuffers)oldRB;

        scan_and_patch_swapchain_vtables(scVtbl);

        ensure_device_contexts_hooked(dev);

        return true;
    }

    static DWORD WINAPI init_thread(LPVOID)
    {
        InitializeCriticalSection(&g_cs);

        for (int i = 0; i < 200 && InterlockedCompareExchange(&g_running, 0, 0); i++)
        {
            if (GetModuleHandleA("d3d11.dll") && GetModuleHandleA("dxgi.dll")) break;
            Sleep(100);
        }
        if (!InterlockedCompareExchange(&g_running, 0, 0)) return 0;

        WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_CLASSDC, DefWindowProcA, 0L, 0L, GetModuleHandleA(NULL), NULL, NULL, NULL, NULL, "FM_DUMMY", NULL };
        RegisterClassExA(&wc);
        HWND hWnd = CreateWindowA("FM_DUMMY", "FM_DUMMY", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);

        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 1;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hWnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        D3D_FEATURE_LEVEL featureLevel;
        const D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

        IDXGISwapChain* dummySC = nullptr;
        ID3D11Device* dummyDev = nullptr;
        ID3D11DeviceContext* dummyCtx = nullptr;

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            featureLevels,
            2,
            D3D11_SDK_VERSION,
            &sd,
            &dummySC,
            &dummyDev,
            &featureLevel,
            &dummyCtx);

        if (FAILED(hr))
        {
            hr = D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                0,
                featureLevels,
                2,
                D3D11_SDK_VERSION,
                &sd,
                &dummySC,
                &dummyDev,
                &featureLevel,
                &dummyCtx);
        }

        if (SUCCEEDED(hr) && dummySC && dummyDev)
        {
            EnterCriticalSection(&g_cs);
            install_hooks(dummySC, dummyDev);
            LeaveCriticalSection(&g_cs);

            dummyCtx->Release();
            dummySC->Release();
            dummyDev->Release();
        }

        DestroyWindow(hWnd);
        UnregisterClassA("FM_DUMMY", wc.hInstance);

        return 0;
    }

    void shutdown()
    {
        InterlockedExchange(&g_running, 0);
        InterlockedExchange(&g_pipeStop, 1);
        if (g_pipeEvt) SetEvent(g_pipeEvt);

        if (g_pipeThr)
        {
            WaitForSingleObject(g_pipeThr, 2000);
            CloseHandle(g_pipeThr); g_pipeThr = nullptr;
        }
        if (g_initThr)
        {
            WaitForSingleObject(g_initThr, 3000);
            CloseHandle(g_initThr); g_initThr = nullptr;
        }
        if (g_pipeEvt) { CloseHandle(g_pipeEvt); g_pipeEvt = nullptr; }

        restore_context_hooks();
        EnterCriticalSection(&g_cs);
        release_shared_color();
        release_shared_depth();
        reset_depth_candidates();
        if (g_context) { g_context->Release();  g_context = nullptr; }
        if (g_device) { g_device->Release();   g_device = nullptr; }
        LeaveCriticalSection(&g_cs);

        DeleteCriticalSection(&g_cs);
    }

    void initialize()
    {
        InterlockedExchange(&g_running, 1);
        g_initThr = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
    }

}

extern "C" BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        fm::initialize();
        break;
    case DLL_PROCESS_DETACH:
         
         
        InterlockedExchange(&fm::g_running, 0);
        InterlockedExchange(&fm::g_pipeStop, 1);
        (void)reserved;
        break;
    }
    return TRUE;
}