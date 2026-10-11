// d3d12.dll proxy dropped next to witcher3.exe.
// The Witcher 3 5.01 under CrossOver/D3DMetal crashes during startup because wine's
// D3D12CreateDevice returns S_FALSE in shapes Windows never produces; the game then
// dereferences a NULL device / misreads probe results -> page fault at
// witcher3+0x1cd4544 -> its crash handler deadlocks in dbghelp -> black screen.
// This proxy normalizes those return values and, the moment a device is created,
// patches its vtable to reject stream-output pipelines (GS without PS / SO entries)
// that crash Apple's D3DMetal shader converter (libmetalirconverter).
// All other exports forward to the real system d3d12.dll (resolved by name/absolute
// path, never recursing into ourselves). Log: %USERPROFILE%\d3d12proxy.log
//
// NOTE: CrossOver forces d3d12=builtin for the game process tree, which ignores an
// app-dir native dll. The bottle needs a one-time registry override:
//   reg add "HKCU\Software\Wine\DllOverrides" /v d3d12 /t REG_SZ /d "native,builtin" /f
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

static HMODULE real;
static FILE *logf;
static CRITICAL_SECTION lock;

static void logmsg(const char *fmt, ...)
{
    if (!logf) return;
    va_list ap;
    va_start(ap, fmt);
    EnterCriticalSection(&lock);
    fprintf(logf, "[%lu] ", GetTickCount());
    vfprintf(logf, fmt, ap);
    fputc('\n', logf);
    fflush(logf);
    LeaveCriticalSection(&lock);
    va_end(ap);
}

static char self_path[MAX_PATH];

static void load_real(void)
{
    if (real) return;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&load_real, &self);
    GetModuleFileNameA(self, self_path, MAX_PATH);
    HMODULE m = LoadLibraryExA("d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m || m == self) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH - 16);
        if (n) { strcat(path, "\\d3d12.dll"); m = LoadLibraryExA(path, NULL, 0); }
    }
    char got[MAX_PATH] = "?";
    if (m) GetModuleFileNameA(m, got, MAX_PATH);
    if (!m || _stricmp(got, self_path) == 0) {
        logmsg("FATAL: real d3d12 not loadable (m=%p got=%s, self=%s)", (void *)m, got, self_path);
        return;
    }
    real = m;
    logmsg("real d3d12 = %s (self = %s)", got, self_path);
}

/* ---- device vtable hooks (slot numbers verified against d3d12.h) ---- */

typedef struct { const void *p; SIZE_T n; } Bytecode;
typedef long (__stdcall *PfnCreateGfxPSO)(void *dev, const void *desc, const void *riid, void **out);
typedef long (__stdcall *PfnCreateCsPSO)(void *dev, const void *desc, const void *riid, void **out);
typedef long (__stdcall *PfnCreatePSO)(void *dev, const void *desc, const void *riid, void **out);
typedef long (__stdcall *PfnCreateStateObject)(void *dev, const void *desc, const void *riid, void **out);
typedef long (__stdcall *PfnCreatePipelineLibrary)(void *dev, const void *blob, SIZE_T len, const void *riid, void **out);

static PfnCreateGfxPSO origGfx;
static PfnCreateCsPSO origCs;
static PfnCreatePSO origPso;
static PfnCreateStateObject origSo;
static PfnCreatePipelineLibrary origLib;
static volatile LONG psoCounter;

static long __stdcall hkGfx(void *dev, const void *desc, const void *riid, void **out)
{
    LONG id = InterlockedIncrement(&psoCounter);
    const char *d = (const char *)desc;
    const Bytecode *ps = (const Bytecode *)(d + 8 + 16);
    const Bytecode *gs = (const Bytecode *)(d + 8 + 16 * 4);
    uint32_t soEntries = *(const uint32_t *)(d + 96);
    uint32_t soStrides = *(const uint32_t *)(d + 112);
    uint32_t soRaster = *(const uint32_t *)(d + 116);
    logmsg("CreateGraphicsPipelineState #%ld tid=%lu (vs=%zu ps=%zu gs=%zu so=%u/%u/%u)",
           id, GetCurrentThreadId(),
           ((const Bytecode *)(d + 8))->n, ps->n, gs->n, soEntries, soStrides, soRaster);
    if (gs->n && (soEntries || !ps->n)) {
        logmsg("  -> #%ld SKIPPED (stream output / GS without PS)", id);
        if (out) *out = NULL;
        return (long)0x80070057; /* E_INVALIDARG */
    }
    long hr = origGfx(dev, desc, riid, out);
    logmsg("  -> #%ld hr=0x%08lx", id, hr);
    return hr;
}

static long __stdcall hkCs(void *dev, const void *desc, const void *riid, void **out)
{
    LONG id = InterlockedIncrement(&psoCounter);
    logmsg("CreateComputePipelineState #%ld tid=%lu", id, GetCurrentThreadId());
    long hr = origCs(dev, desc, riid, out);
    logmsg("  -> #%ld hr=0x%08lx", id, hr);
    return hr;
}

/* Pipeline state stream: subobjects are pointer-aligned, each starting with a type enum. */
static SIZE_T subobject_size(uint32_t type)
{
    switch (type) {
    case 0: return 8;           /* root signature pointer */
    case 1: case 2: case 3: case 4: case 5: case 6: case 24: case 25: return 16; /* shader bytecode */
    case 7: return 40;          /* stream output */
    case 8: return 328;         /* blend desc */
    case 9: return 4;           /* sample mask */
    case 10: return 44;         /* rasterizer */
    case 11: return 52;         /* depth stencil */
    case 12: return 16;         /* input layout */
    case 13: return 4;          /* strip cut */
    case 14: return 4;          /* topology type */
    case 15: return 36;         /* rtv formats */
    case 16: return 4;          /* dsv format */
    case 17: return 8;          /* sample desc */
    case 18: return 4;          /* node mask */
    case 19: return 16;         /* cached pso */
    case 20: return 4;          /* flags */
    case 21: return 52;         /* depth stencil1 */
    case 22: return 16;         /* view instancing */
    case 26: return 52;         /* depth stencil2 (approx) */
    case 27: return 44;         /* rasterizer1 */
    case 28: return 44;         /* rasterizer2 */
    default: return 0;
    }
}

static long __stdcall hkPso(void *dev, const void *desc, const void *riid, void **out)
{
    LONG id = InterlockedIncrement(&psoCounter);
    SIZE_T size = *(const SIZE_T *)desc;
    const char *stream = *(const char *const *)((const char *)desc + 8);
    logmsg("CreatePipelineState #%ld tid=%lu stream=%zu bytes", id, GetCurrentThreadId(), (size_t)size);
    int sawGS = 0, sawPS = 0, sawSO = 0;
    SIZE_T off = 0;
    while (off + 4 <= size) {
        uint32_t type = *(const uint32_t *)(stream + off);
        SIZE_T body = subobject_size(type);
        if (!body) { logmsg("  unknown subobject %u at %zu, stop parsing", type, (size_t)off); break; }
        SIZE_T align = (type <= 7 || type == 12 || type == 19 || type == 22 || (type >= 24 && type <= 25)) ? 8 : 4;
        SIZE_T bodyOff = (off + 4 + align - 1) & ~(align - 1);
        if ((type >= 1 && type <= 6) || type == 24 || type == 25) {
            Bytecode bc = *(const Bytecode *)(stream + bodyOff);
            if (type == 5 && bc.p && bc.n) sawGS = 1;
            if (type == 2 && bc.p && bc.n) sawPS = 1;
        }
        if (type == 7) sawSO = 1;
        off = (bodyOff + body + 7) & ~(SIZE_T)7;
    }
    logmsg("  subobjects: gs=%d ps=%d streamout=%d", sawGS, sawPS, sawSO);
    if (sawGS && (sawSO || !sawPS)) {
        logmsg("  -> #%ld SKIPPED (stream output / GS without PS)", id);
        if (out) *out = NULL;
        return (long)0x80070057; /* E_INVALIDARG */
    }
    long hr = origPso(dev, desc, riid, out);
    logmsg("  -> #%ld hr=0x%08lx", id, hr);
    return hr;
}

static long __stdcall hkSo(void *dev, const void *desc, const void *riid, void **out)
{
    LONG id = InterlockedIncrement(&psoCounter);
    logmsg("CreateStateObject #%ld tid=%lu type=%u", id, GetCurrentThreadId(), *(const uint32_t *)desc);
    long hr = origSo(dev, desc, riid, out);
    logmsg("  -> #%ld hr=0x%08lx", id, hr);
    return hr;
}

static long __stdcall hkLib(void *dev, const void *blob, SIZE_T len, const void *riid, void **out)
{
    logmsg("CreatePipelineLibrary len=%zu tid=%lu", (size_t)len, GetCurrentThreadId());
    long hr = origLib(dev, blob, len, riid, out);
    logmsg("  -> lib hr=0x%08lx", hr);
    return hr;
}

static void patch_slot(void **vtbl, int idx, void *hook, void **orig)
{
    DWORD old;
    if (vtbl[idx] == hook) return;
    VirtualProtect(&vtbl[idx], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *orig = vtbl[idx];
    vtbl[idx] = hook;
    VirtualProtect(&vtbl[idx], sizeof(void *), old, &old);
}

static void write_slot(void **slot, void *hook)
{
    DWORD old;
    if (*slot == hook) return;
    VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *slot = hook;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

static void hook_device(void *dev)
{
    if (!dev) return;
    void **vtbl = *(void ***)dev;
    /* The game may call pipeline creators through ID3D12Device2+ interface pointers --
       different vtables whose entries point to the SAME functions (C++ inheritance).
       Capture the original pointers once, then patch every matching slot in the
       surrounding data segment, which covers all interface versions at once. */
    static void *origVals[5];
    static int captured;
    void *hkFns[5] = { (void *)hkGfx, (void *)hkCs, (void *)hkLib, (void *)hkPso, (void *)hkSo };
    if (!captured) {
        origVals[0] = vtbl[10]; origVals[1] = vtbl[11]; origVals[2] = vtbl[44];
        origVals[3] = vtbl[47]; origVals[4] = vtbl[62];
        captured = 1;
    }
    static const int slots[5] = { 10, 11, 44, 47, 62 };
    void **origTargets[5] = { (void **)&origGfx, (void **)&origCs, (void **)&origLib,
                              (void **)&origPso, (void **)&origSo };
    for (int i = 0; i < 5; i++) {
        void *cur = vtbl[slots[i]];
        if (cur == hkFns[i]) continue;
        if (cur != origVals[i]) {
            /* Someone else re-hooked this slot after us (another proxy, a mod, ...).
               Patching now would chain to their hook and could form a cycle; leave it. */
            logmsg("slot %d held by foreign hook %p, leaving alone", slots[i], cur);
            continue;
        }
        patch_slot(vtbl, slots[i], hkFns[i], origTargets[i]);
    }
    logmsg("hooked D3D12 device %p vtbl %p", dev, vtbl);

    /* sweep nearby pages for other vtables holding the same function pointers */
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t lo = ((uintptr_t)vtbl - 0x400000) & ~(uintptr_t)0xfff;
    uintptr_t hi = (uintptr_t)vtbl + 0x400000;
    int extra = 0;
    uintptr_t p = lo;
    while (p < hi) {
        if (!VirtualQuery((void *)p, &mbi, sizeof mbi)) break;
        int ok = mbi.State == MEM_COMMIT &&
                 (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY | PAGE_READONLY | PAGE_EXECUTE_READ)) != 0 &&
                 (mbi.Protect & PAGE_GUARD) == 0;
        if (ok) {
            char *base = (char *)mbi.BaseAddress;
            for (SIZE_T o = 0; o + 8 <= mbi.RegionSize; o += 8) {
                void *v = *(void **)(base + o);
                for (int i = 0; i < 5; i++) {
                    if (v == origVals[i] && v != hkFns[i]) {
                        write_slot((void **)(base + o), hkFns[i]);
                        extra++;
                        break;
                    }
                }
            }
        }
        p = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    logmsg("vtable sweep around %p: %d extra slots patched", (void *)vtbl, extra);
}

/* ---- exports ---- */

static void *cached_dev;   /* first successfully created device, for QI fixups */

__declspec(dllexport) long __stdcall D3D12CreateDevice(void *adapter, unsigned int featureLevel, const void *riid, void **device)
{
    load_real();
    long (__stdcall *fn)(void *, unsigned int, const void *, void **) =
        real ? (long (__stdcall *)(void *, unsigned int, const void *, void **))(void *)GetProcAddress(real, "D3D12CreateDevice") : NULL;
    if (!fn) { logmsg("D3D12CreateDevice: no real export"); return 0x80004001; }
    long hr = fn(adapter, featureLevel, riid, device);
    /* wine/GPTK's D3D12CreateDevice returns S_FALSE in two shapes Windows never produces:
       (a) pure capability probes (ppDevice == NULL) -- Windows answers S_OK, wine says 1;
           the game treats the odd value as a broken path and crashes at witcher3+0x1cd4544.
       (b) out-param form with *ppDevice left NULL -- game reads "not FAILED" as success
           and dereferences the NULL device.
       Normalize both: probes -> S_OK (the device demonstrably creates), out-param -> QI
       on the cached device or a proper error code. */
    if (hr >= 0 && hr != 0) {
        if (!device) {
            logmsg("probe D3D12CreateDevice(FL=0x%x) hr=0x%08lx -> S_OK", featureLevel, hr);
            hr = 0;
        } else if (!*device) {
            const unsigned int *r = (const unsigned int *)riid;
            logmsg("D3D12CreateDevice hr=0x%08lx NULL dev (riid %08x-%04x-%04x), QI fixup",
                   hr, r ? r[0] : 0, r ? r[1] & 0xffff : 0, r ? r[2] & 0xffff : 0);
            if (cached_dev) {
                long (__stdcall *qi)(void *, const void *, void **) = ((long (__stdcall ***)(void *, const void *, void **))cached_dev)[0][0];
                hr = qi(cached_dev, riid, device);
                logmsg("  QI fixup hr=0x%08lx dev=%p", hr, *device);
            } else {
                hr = 0x80004002; /* E_NOINTERFACE */
            }
        }
    }
    logmsg("D3D12CreateDevice(adapter=%p FL=0x%x out=%p) hr=0x%08lx dev=%p", adapter, featureLevel, (void *)device, hr,
           (hr == 0 && device) ? *device : NULL);
    if (hr == 0 && device && *device) {
        if (!cached_dev) cached_dev = *device;
        hook_device(*device);
    }
    return hr;
}

/* Generic passthrough forwarders; x64 calling convention makes extra register args harmless. */
#define FWD(name) \
    __declspec(dllexport) long long __stdcall name(void *a, void *b, void *c, void *d) \
    { \
        load_real(); \
        long long (__stdcall *fn)(void *, void *, void *, void *) = \
            real ? (long long (__stdcall *)(void *, void *, void *, void *))(void *)GetProcAddress(real, #name) : NULL; \
        if (!fn) { logmsg(#name ": no real export"); return 0x80004001; } \
        return fn(a, b, c, d); \
    }

FWD(GetBehaviorValue)
FWD(D3D12GetDebugInterface)
FWD(D3D12CoreCreateLayeredDevice)
FWD(D3D12CoreGetLayeredDeviceSize)
FWD(D3D12CoreRegisterLayers)
FWD(D3D12CreateRootSignatureDeserializer)
FWD(D3D12CreateVersionedRootSignatureDeserializer)
FWD(D3D12EnableExperimentalFeatures)
FWD(D3D12SerializeRootSignature)
FWD(D3D12SerializeVersionedRootSignature)

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&lock);
        char path[MAX_PATH];
        if (GetEnvironmentVariableA("USERPROFILE", path, MAX_PATH)) {
            strcat(path, "\\d3d12proxy.log");
            logf = fopen(path, "w");
        }
        logmsg("d3d12 proxy attached (pid %lu)", GetCurrentProcessId());
    }
    return TRUE;
}
