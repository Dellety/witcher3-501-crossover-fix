// Proxy for amd_fidelityfx_loader_dx12.dll (The Witcher 3 5.00b/5.01 under CrossOver/D3DMetal).
// Forwards all FFX API calls to the real loader (amd_fidelityfx_loader_dx12_orig.dll).
// Hooks the game's ID3D12Device and refuses stream-output pipelines (GS without PS / SO
// entries): compiling those crashes Apple's D3DMetal shader converter (libmetalirconverter).
// 5.01 extra: ffxQuery(GET_VERSIONS) can fail on this GPU (provider vendor check) and the
// 5.01 game does not check the return code, dereferencing the untouched output arrays ->
// crash at witcher3+0x1cd4544. On failure we answer "0 versions available" + OK so the
// game skips FSR. Device-hook timing is handled by the d3d12.dll proxy in the game folder
// (d3d12proxy.c); the ffxQuery-time hook below stays as a harmless fallback. No background
// threads here -- they race the loader lock.
// Env knobs: FFXPROXY_DUMP=1 dumps shaders to %USERPROFILE%\ffxdump,
//            FFXPROXY_STUB_FSR=1 gives FSR upscale/framegen a dummy context.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

typedef struct ffxApiHeader { uint64_t type; struct ffxApiHeader *pNext; } ffxApiHeader;
typedef void *ffxContext;
typedef uint32_t ffxReturnCode_t;
typedef struct ffxAllocationCallbacks ffxAllocationCallbacks;

typedef ffxReturnCode_t (*PfnCreate)(ffxContext *, ffxApiHeader *, const ffxAllocationCallbacks *);
typedef ffxReturnCode_t (*PfnDestroy)(ffxContext *, const ffxAllocationCallbacks *);
typedef ffxReturnCode_t (*PfnConfigure)(ffxContext *, const ffxApiHeader *);
typedef ffxReturnCode_t (*PfnQuery)(ffxContext *, ffxApiHeader *);
typedef ffxReturnCode_t (*PfnDispatch)(ffxContext *, const ffxApiHeader *);

#define RET_OK 0u
#define RET_ERROR 1u

#define DESC_CREATE_UPSCALE 0x00010000u
#define DESC_CREATE_FRAMEGENERATION 0x00020001u
#define DESC_QUERY_GET_PROVIDER_VERSION 6u
#define DESC_QUERY_UPSCALE_GPU_MEMORY_USAGE 0x00010008u
#define DESC_QUERY_UPSCALE_GPU_MEMORY_USAGE_V2 0x00010009u
#define DESC_QUERY_FG_GPU_MEMORY_USAGE 0x00020007u
#define DESC_QUERY_FG_GPU_MEMORY_USAGE_V2 0x0002000bu

#define DUMMY_MAGIC 0x59444d4d55444646ull /* "FFDUMMY" */

typedef struct DummyCtx { uint64_t magic; uint64_t createType; } DummyCtx;

struct ffxQueryGetProviderVersion { ffxApiHeader header; uint64_t versionId; const char *versionName; };
struct FfxApiEffectMemoryUsage { uint64_t totalUsageInBytes; uint64_t aliasableUsageInBytes; };
struct ffxQueryGpuMemoryUsage { ffxApiHeader header; struct FfxApiEffectMemoryUsage *usage; };

static HMODULE real;
static PfnCreate pCreate;
static PfnDestroy pDestroy;
static PfnConfigure pConfigure;
static PfnQuery pQuery;
static PfnDispatch pDispatch;
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

static void load_real(void)
{
    if (real) return;
    char path[MAX_PATH];
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&load_real, &self);
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    path[n] = 0;
    strcat(path, "amd_fidelityfx_loader_dx12_orig.dll");
    real = LoadLibraryA(path);
    if (!real) { logmsg("failed to load %s (err %lu)", path, GetLastError()); return; }
    pCreate = (PfnCreate)GetProcAddress(real, "ffxCreateContext");
    pDestroy = (PfnDestroy)GetProcAddress(real, "ffxDestroyContext");
    pConfigure = (PfnConfigure)GetProcAddress(real, "ffxConfigure");
    pQuery = (PfnQuery)GetProcAddress(real, "ffxQuery");
    pDispatch = (PfnDispatch)GetProcAddress(real, "ffxDispatch");
    logmsg("loaded real loader %s", path);
}

struct ffxQueryDescGetVersions { ffxApiHeader header; uint64_t createDescType; void *device; uint64_t *outputCount; uint64_t *versionIds; const char **versionNames; };

static int is_dummy(ffxContext *ctx)
{
    return ctx && *ctx && ((DummyCtx *)*ctx)->magic == DUMMY_MAGIC;
}

__declspec(dllexport) ffxReturnCode_t ffxCreateContext(ffxContext *ctx, ffxApiHeader *desc, const ffxAllocationCallbacks *cb)
{
    uint64_t type = desc ? desc->type : 0;
    if (getenv("FFXPROXY_STUB_FSR") && (type == DESC_CREATE_UPSCALE || type == DESC_CREATE_FRAMEGENERATION)) {
        DummyCtx *d = (DummyCtx *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DummyCtx));
        d->magic = DUMMY_MAGIC;
        d->createType = type;
        *ctx = d;
        logmsg("CreateContext type=0x%llx -> dummy %p", (unsigned long long)type, d);
        return RET_OK;
    }
    load_real();
    ffxReturnCode_t r = pCreate ? pCreate(ctx, desc, cb) : RET_ERROR;
    if (r != RET_OK && (type == DESC_CREATE_UPSCALE || type == DESC_CREATE_FRAMEGENERATION)) {
        /* Real FFX context creation fails on this GPU the same way GET_VERSIONS does.
           The 5.01 game mishandles FFX failures (see ffxQuery), so hand it a dummy
           no-op context instead: the game runs with FSR effectively disabled. */
        DummyCtx *d = (DummyCtx *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DummyCtx));
        d->magic = DUMMY_MAGIC;
        d->createType = type;
        if (ctx) *ctx = d;
        logmsg("CreateContext type=0x%llx real ret=%u -> dummy %p", (unsigned long long)type, r, d);
        return RET_OK;
    }
    logmsg("CreateContext type=0x%llx -> forwarded, ret=%u ctx=%p", (unsigned long long)type, r, ctx ? *ctx : NULL);
    return r;
}

__declspec(dllexport) ffxReturnCode_t ffxDestroyContext(ffxContext *ctx, const ffxAllocationCallbacks *cb)
{
    if (is_dummy(ctx)) {
        logmsg("DestroyContext dummy %p", *ctx);
        HeapFree(GetProcessHeap(), 0, *ctx);
        *ctx = NULL;
        return RET_OK;
    }
    load_real();
    ffxReturnCode_t r = pDestroy ? pDestroy(ctx, cb) : RET_ERROR;
    logmsg("DestroyContext forwarded ret=%u", r);
    return r;
}

__declspec(dllexport) ffxReturnCode_t ffxConfigure(ffxContext *ctx, const ffxApiHeader *desc)
{
    if (is_dummy(ctx)) return RET_OK;
    load_real();
    ffxReturnCode_t r = pConfigure ? pConfigure(ctx, desc) : RET_ERROR;
    logmsg("Configure type=0x%llx forwarded ret=%u", (unsigned long long)(desc ? desc->type : 0), r);
    return r;
}

__declspec(dllexport) ffxReturnCode_t ffxQuery(ffxContext *ctx, ffxApiHeader *desc)
{
    uint64_t type = desc ? desc->type : 0;
    if (is_dummy(ctx)) {
        if (type == DESC_QUERY_GET_PROVIDER_VERSION) {
            struct ffxQueryGetProviderVersion *q = (struct ffxQueryGetProviderVersion *)desc;
            q->versionId = 1;
            q->versionName = "3.1.5 (stub)";
        } else if (type == DESC_QUERY_UPSCALE_GPU_MEMORY_USAGE || type == DESC_QUERY_FG_GPU_MEMORY_USAGE) {
            struct ffxQueryGpuMemoryUsage *q = (struct ffxQueryGpuMemoryUsage *)desc;
            if (q->usage) { q->usage->totalUsageInBytes = 0; q->usage->aliasableUsageInBytes = 0; }
        } else {
            // Stateless queries (ratios, jitter, ...) don't need a live context.
            load_real();
            if (pQuery) {
                ffxReturnCode_t r = pQuery(NULL, desc);
                logmsg("Query dummy type=0x%llx -> stateless forward ret=%u", (unsigned long long)type, r);
                return r;
            }
        }
        logmsg("Query dummy type=0x%llx", (unsigned long long)type);
        return RET_OK;
    }
    if (type == 4) {
        struct ffxQueryDescGetVersions *q = (struct ffxQueryDescGetVersions *)desc;
        (void)q;
        load_real();
        ffxReturnCode_t r = pQuery ? pQuery(ctx, desc) : RET_ERROR;
        if (r != RET_OK) {
            /* 5.01's provider rejects this GPU (vendor check) and returns ERROR,
               leaving the caller's version arrays untouched. The game does not
               check the return code, and its version loop is do-while shaped: it
               reads versionIds[0] UNCONDITIONALLY (verified in the disassembly at
               witcher3+0x1cd4544: count==0 skips the allocation but still loads
               through the NULL array). Windows always reports >=1 version, so the
               game bug never fires there. Answer with exactly one stub version. */
            if (q->outputCount) *q->outputCount = 1;
            if (q->versionIds) q->versionIds[0] = 1;
            if (q->versionNames) q->versionNames[0] = "3.1.5 (stub)";
            logmsg("GET_VERSIONS real ret=%u -> answering 1 stub version, OK", r);
            return RET_OK;
        }
        logmsg("Query type=0x4 forwarded ret=%u", r);
        return r;
    }
    load_real();
    ffxReturnCode_t r = pQuery ? pQuery(ctx, desc) : RET_ERROR;
    logmsg("Query type=0x%llx forwarded ret=%u", (unsigned long long)type, r);
    return r;
}

__declspec(dllexport) ffxReturnCode_t ffxDispatch(ffxContext *ctx, const ffxApiHeader *desc)
{
    if (is_dummy(ctx)) return RET_OK;
    load_real();
    ffxReturnCode_t r = pDispatch ? pDispatch(ctx, desc) : RET_ERROR;
    logmsg("Dispatch type=0x%llx forwarded ret=%u", (unsigned long long)(desc ? desc->type : 0), r);
    return r;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&lock);
        char path[MAX_PATH];
        if (GetEnvironmentVariableA("USERPROFILE", path, MAX_PATH)) {
            strcat(path, "\\ffxproxy.log");
            logf = fopen(path, "w");
        }
        logmsg("ffxproxy attached");
        /* The game calls SetProcessDPIAware only after it has enumerated display modes, so with
           CrossOver's High Resolution Mode it sees a half-size virtual desktop and caps out at
           1512x982. Declaring DPI awareness this early exposes the real resolutions. */
        if (!getenv("FFXPROXY_NO_DPI")) {
            HMODULE u32 = GetModuleHandleA("user32.dll");
            BOOL (WINAPI *setAware)(void) = u32 ? (BOOL (WINAPI *)(void))GetProcAddress(u32, "SetProcessDPIAware") : NULL;
            logmsg("SetProcessDPIAware %s", setAware && setAware() ? "ok" : "failed");
        }
    }
    return TRUE;
}
