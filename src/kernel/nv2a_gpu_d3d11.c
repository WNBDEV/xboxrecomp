/**
 * NV2A rasterisation and pixel shading on Direct3D 11. See nv2a_gpu.h.
 *
 * Under Wine this goes through DXVK to Vulkan. One device, used only from the
 * executor's thread (D3D11_CREATE_DEVICE_SINGLETHREADED), no swap chain: it
 * draws into textures standing in for the title's surfaces and copies them
 * back into guest memory, where the framebuffer window already looks.
 *
 * Pixel shaders are generated per register-combiner program, from the same
 * decode as the software evaluator (nv2a_combiner.c), so the two can be held
 * against each other. Constants travel in a cbuffer, so a program differing
 * only in its factors reuses the shader.
 *
 * Natively on Linux the same code runs on DXVK Native -- DXVK's D3D11 built as
 * Linux libraries -- when built with XBOX_DXVK_NATIVE; see the block below
 * the includes.
 */
#include "nv2a_gpu.h"
#include "nv2a_vsh_interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <math.h>

#if defined(_WIN32) || defined(XBOX_DXVK_NATIVE)

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#if !defined(_WIN32)
/* DXVK Native: the D3D11 that runs under Wine, as Linux libraries
 * (libdxvk_d3d11/libdxvk_dxgi), with HWND standing for an SDL_Window -- the
 * one the framebuffer window opens (fb_present.c). Its headers give the D3D11
 * and DXGI interfaces and little else of Win32, so the few other calls this
 * file makes are stood in for here.
 *
 * Nothing is linked: DXVK, and vkd3d-utils for D3DCompile (the HLSL compiler
 * Wine's d3dcompiler_47 is built on, so shaders compile the same way and the
 * shader-cache/ blobs from a Wine run are reused as they are), are opened at
 * run time. Without them D3D11CreateDevice fails and the software rasteriser
 * draws, as it does without a GPU on Windows. */
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <SDL.h>
#include <SDL_vulkan.h>

typedef HRESULT (*PfnCreateDevice)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE,
                                   UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                   ID3D11Device **, D3D_FEATURE_LEVEL *,
                                   ID3D11DeviceContext **);
/* vkd3d-utils keeps the Windows x64 calling convention on Linux (its WINAPI
 * and STDMETHODCALLTYPE are ms_abi), where DXVK Native uses the host's. So
 * D3DCompile is called as ms_abi, and the blobs it returns -- whose methods
 * are ms_abi too -- are copied into blobs of our own (NativeBlob) before
 * anything here or in DXVK touches them. Calling across the two unmarked
 * scrambled the arguments: "Ignoring effect flags 0x24c80", then a crash. */
#define MS_ABI __attribute__((ms_abi))
typedef struct MsBlob MsBlob;
typedef struct {
    HRESULT (MS_ABI *QueryInterface)(MsBlob *, const GUID *, void **);
    ULONG   (MS_ABI *AddRef)(MsBlob *);
    ULONG   (MS_ABI *Release)(MsBlob *);
    void   *(MS_ABI *GetBufferPointer)(MsBlob *);
    SIZE_T  (MS_ABI *GetBufferSize)(MsBlob *);
} MsBlobVtbl;
struct MsBlob { const MsBlobVtbl *lpVtbl; };
typedef HRESULT (MS_ABI *PfnCompile)(const void *, SIZE_T, const char *,
                                     const D3D_SHADER_MACRO *, void *,
                                     const char *, const char *, UINT, UINT,
                                     MsBlob **, MsBlob **);
static PfnCreateDevice p_create_device;
static PfnCompile p_compile;

/* An ID3DBlob in this side's convention: a refcount and a copy of the bytes. */
typedef struct {
    ID3D10Blob iface;
    LONG refs;
    SIZE_T size;
    unsigned char data[];
} NativeBlob;

static HRESULT STDMETHODCALLTYPE nb_query(ID3D10Blob *b, REFIID riid, void **out)
{
    (void)b; (void)riid;
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE nb_addref(ID3D10Blob *b)
{
    return (ULONG)__atomic_add_fetch(&((NativeBlob *)b)->refs, 1, __ATOMIC_SEQ_CST);
}
static ULONG STDMETHODCALLTYPE nb_release(ID3D10Blob *b)
{
    LONG n = __atomic_sub_fetch(&((NativeBlob *)b)->refs, 1, __ATOMIC_SEQ_CST);
    if (!n)
        free(b);
    return (ULONG)n;
}
static LPVOID STDMETHODCALLTYPE nb_pointer(ID3D10Blob *b) { return ((NativeBlob *)b)->data; }
static SIZE_T STDMETHODCALLTYPE nb_size(ID3D10Blob *b) { return ((NativeBlob *)b)->size; }
static ID3D10BlobVtbl s_nb_vtbl = { nb_query, nb_addref, nb_release, nb_pointer, nb_size };

static ID3DBlob *native_blob(const void *data, SIZE_T n)
{
    NativeBlob *b = (NativeBlob *)malloc(sizeof(*b) + n);
    if (!b)
        return NULL;
    b->iface.lpVtbl = &s_nb_vtbl;
    b->refs = 1;
    b->size = n;
    if (data)
        memcpy(b->data, data, n);
    return (ID3DBlob *)&b->iface;
}

/* An ms_abi blob from vkd3d, copied over and released. */
static ID3DBlob *native_from_ms(MsBlob *m)
{
    ID3DBlob *b;
    if (!m)
        return NULL;
    b = native_blob(m->lpVtbl->GetBufferPointer(m), m->lpVtbl->GetBufferSize(m));
    m->lpVtbl->Release(m);
    return b;
}

static void *native_sym(const char *const *libs, const char *name)
{
    for (; *libs; libs++) {
        void *h = dlopen(*libs, RTLD_NOW | RTLD_LOCAL);
        if (h)
            return dlsym(h, name);
    }
    return NULL;
}

static int native_load(void)
{
    static int state = -1;
    if (state < 0) {
        static const char *const d3d11[] = { "libdxvk_d3d11.so.0", "libdxvk_d3d11.so", NULL };
        static const char *const utils[] = { "libvkd3d-utils.so.1", "libvkd3d-utils.so", NULL };
        /* DXVK Native picks its window system from this; ours is SDL2. */
        setenv("DXVK_WSI_DRIVER", "SDL2", 0);
        /* And it asks SDL for Vulkan's instance extensions when the device
         * is made, which can be before the framebuffer window has opened --
         * with video not yet initialised that read through a null device
         * inside SDL. Initialise it here; the window's own init then only
         * counts up. */
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
            fprintf(stderr, "[GPU] SDL video: %s\n", SDL_GetError());
        p_create_device = (PfnCreateDevice)native_sym(d3d11, "D3D11CreateDevice");
        p_compile = (PfnCompile)native_sym(utils, "D3DCompile");
        state = p_create_device && p_compile;
        if (!state)
            fprintf(stderr, "[GPU] %s not found; drawing in software\n",
                    !p_create_device ? "DXVK Native (libdxvk_d3d11.so.0)"
                                     : "vkd3d-utils (libvkd3d-utils.so.1)");
    }
    return state;
}

static HRESULT native_D3D11CreateDevice(IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE m,
                                        UINT f, const D3D_FEATURE_LEVEL *fl, UINT nfl,
                                        UINT sdk, ID3D11Device **dev,
                                        D3D_FEATURE_LEVEL *got, ID3D11DeviceContext **ctx)
{
    if (!native_load())
        return E_FAIL;
    return p_create_device(a, t, m, f, fl, nfl, sdk, dev, got, ctx);
}

static HRESULT native_D3DCompile(const void *src, SIZE_T n, const char *name,
                                 const D3D_SHADER_MACRO *defs, ID3DInclude *inc,
                                 const char *entry, const char *profile, UINT f1,
                                 UINT f2, ID3DBlob **code, ID3DBlob **err)
{
    MsBlob *mc = NULL, *me = NULL;
    HRESULT hr;

    if (code) *code = NULL;
    if (err) *err = NULL;
    if (!native_load())
        return E_FAIL;
    (void)inc;                          /* never used here: no #include */
    hr = p_compile(src, n, name, defs, NULL, entry, profile, f1, f2, &mc, &me);
    if (code) *code = native_from_ms(mc); else if (mc) mc->lpVtbl->Release(mc);
    if (err) *err = native_from_ms(me); else if (me) me->lpVtbl->Release(me);
    return hr;
}

static HRESULT native_D3DCreateBlob(SIZE_T n, ID3DBlob **blob)
{
    *blob = native_blob(NULL, n);
    return *blob ? S_OK : E_OUTOFMEMORY;
}

#define D3D11CreateDevice native_D3D11CreateDevice
#define D3DCompile        native_D3DCompile
#define D3DCreateBlob     native_D3DCreateBlob

static BOOL QueryPerformanceCounter(LARGE_INTEGER *c)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    c->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return TRUE;
}

static BOOL QueryPerformanceFrequency(LARGE_INTEGER *f)
{
    f->QuadPart = 1000000000LL;
    return TRUE;
}

static DWORD GetTickCount(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (DWORD)(c.QuadPart / 1000000);
}

#define MOVEFILE_REPLACE_EXISTING 1
static BOOL CreateDirectoryA(const char *d, void *sa) { (void)sa; return mkdir(d, 0755) == 0; }
static BOOL MoveFileExA(const char *a, const char *b, DWORD f) { (void)f; return rename(a, b) == 0; }
static BOOL DeleteFileA(const char *a) { return unlink(a) == 0; }

/* The drawable size of the SDL window the swap chain presents to. */
static BOOL GetClientRect(HWND w, RECT *r)
{
    int cw = 0, ch = 0;
    SDL_Vulkan_GetDrawableSize((SDL_Window *)w, &cw, &ch);
    r->left = r->top = 0;
    r->right = cw;
    r->bottom = ch;
    return cw > 0 && ch > 0;
}
#endif /* !_WIN32 */

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_FramebufferWindowGpuUnavailable(void);

static int s_state = -1;                /* -1 untried, 0 unavailable, 1 ready */
static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static ID3D11VertexShader *s_vs, *s_clear_vs;
static ID3D11PixelShader *s_clear_ps;
static ID3D11InputLayout *s_layout;
static ID3D11Buffer *s_vb, *s_vs_cb, *s_ps_cb, *s_clear_cb;
static ID3D11Buffer *s_ib, *s_const_cb, *s_prog_cb;
/* 16 zero bytes, bound with stride 0 to the inputs a program does not read. */
static ID3D11Buffer *s_zero_vb;
static ID3D11InputLayout *s_attr_layout;
static uint32_t s_ib_pos, s_const_gen_up;
static ID3D11RasterizerState *s_rs, *s_rs_cull[2];   /* none; CW, CCW culled */
static ID3D11ShaderResourceView *s_white2d, *s_whitecube;
static uint32_t s_vb_pos;
static uint32_t s_frame = 1;

/* RECOMP_SCALE=N (1-4): render N times the console's resolution. Every
 * render target and depth buffer is N x N larger and drawn through a scaled
 * viewport; guest memory keeps the console's size, so a surface read back for
 * the CPU or a texture is first filtered down (rt_readback), and one the CPU
 * wrote is filtered up (rt_upload). Vertex positions are untouched: they are
 * in guest pixels and the viewport does the rest. */
static uint32_t s_scale = 1;
static ID3D11VertexShader *s_blit_vs;
static ID3D11PixelShader *s_blit_ps;
static ID3D11SamplerState *s_blit_smp;
static ID3D11PixelShader *s_sharp_ps;   /* RECOMP_SHARPEN, at the present */
static ID3D11Buffer *s_sharp_cb;

/* RECOMP_SHARPEN=0..1 [0]: contrast-adaptive sharpening as the frame goes to
 * the window. Each pixel is pushed away from its four neighbours, less where
 * the neighbourhood already spans a wide range (an edge, which would ring)
 * and more where it is flat (a magnified texture, the softness 2x rendering
 * of 480i-era art shows). 0 leaves the frame exactly as rendered. */
static float sharpen_amount(void)
{
    static float a = -1.0f;
    if (a < 0.0f) {
        const char *e = getenv("RECOMP_SHARPEN");
        a = e && *e ? (float)atof(e) : 0.0f;
        if (!(a > 0.0f)) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
    }
    return a;
}

#define GPU_VB_BYTES (16u << 20)
#define GPU_IB_BYTES (4u << 20)

static void gpu_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[GPU-D3D11] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}

static uint8_t *guest(uint32_t va)
{
    return (uint8_t *)xbox_GetMemoryOffset() + va;
}

static uint64_t hash_bytes(uint64_t h, const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* ---- shaders ------------------------------------------------------------ */

/* Compiled shaders, kept on disk by a hash of their source.
 *
 * Every combiner setup and vertex program a title uses becomes HLSL that is
 * compiled the first time it is drawn, and each compile holds up the frame
 * it lands in. Kept, each is compiled once ever rather than once a session.
 * RECOMP_SHADER_CACHE names the directory (default shader-cache, under the
 * working directory); 0 turns it off. A stale entry cannot be wrong: the key
 * is the source itself, profile included. */
static const char *shader_cache_dir(void)
{
    static const char *dir = (const char *)-1;
    if (dir == (const char *)-1) {
        const char *e = getenv("RECOMP_SHADER_CACHE");
        dir = (e && *e == '0' && !e[1]) ? NULL : (e && *e ? e : "shader-cache");
        if (dir)
            CreateDirectoryA(dir, NULL);
    }
    return dir;
}

static ID3DBlob *compile(const char *src, const char *profile, const char *what)
{
    static unsigned compiled, loaded;
    static double compile_ms;
    ID3DBlob *code = NULL, *err = NULL;
    const char *dir = shader_cache_dir();
    char path[512];
    LARGE_INTEGER t0, t1, fq;
    HRESULT hr;

    if (dir) {
        uint64_t key = hash_bytes(hash_bytes(0xCBF29CE484222325ull, (const uint8_t *)src,
                                             strlen(src)),
                                  (const uint8_t *)profile, strlen(profile));
        FILE *f;
        snprintf(path, sizeof path, "%s/%016llx.%s.bin", dir, (unsigned long long)key, profile);
        f = fopen(path, "rb");
        if (f) {
            long n;
            fseek(f, 0, SEEK_END);
            n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0 && SUCCEEDED(D3DCreateBlob((SIZE_T)n, &code))
                && fread(ID3D10Blob_GetBufferPointer(code), 1, (size_t)n, f) == (size_t)n) {
                fclose(f);
                loaded++;
                return code;
            }
            if (code) {
                ID3D10Blob_Release(code);
                code = NULL;
            }
            fclose(f);
        }
    }
    QueryPerformanceCounter(&t0);
    hr = D3DCompile(src, strlen(src), what, NULL, NULL, "main", profile,
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    compiled++;
    compile_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
    if ((compiled & (compiled - 1)) == 0)       /* 1, 2, 4, 8, ... */
        gpu_log("shaders: %u compiled (%.0f ms in all), %u from the cache",
                compiled, compile_ms, loaded);
    if (SUCCEEDED(hr) && dir) {
        char tmp[520];
        FILE *f;
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        f = fopen(tmp, "wb");
        if (f) {
            size_t n = ID3D10Blob_GetBufferSize(code);
            int ok = fwrite(ID3D10Blob_GetBufferPointer(code), 1, n, f) == n;
            ok &= fclose(f) == 0;
            if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
                DeleteFileA(tmp);
        }
    }
    if (FAILED(hr)) {
        gpu_log("compiling %s failed (0x%08lX): %s", what, (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err)
            ID3D10Blob_Release(err);
        return NULL;
    }
    if (err)
        ID3D10Blob_Release(err);
    return code;
}

/* Screen space back to clip space. The title's program left x, y in pixels
 * and z in depth-buffer units, divided by w; multiplying by w again gives the
 * rasteriser the w it needs for perspective-correct interpolation and for
 * clipping triangles that cross behind the eye. */
static const char s_vs_src[] =
"cbuffer V : register(b0) { float4 vp; };\n"
"struct VI { float4 p : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
"  float4 f : FOG; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
"  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
"struct VO { float4 p : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
"  float4 f : FOG; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
"  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
"VO main(VI i) {\n"
"  VO o; float w = i.p.w;\n"
"  o.p = float4((i.p.x * vp.x - 1.0) * w, (1.0 - i.p.y * vp.y) * w,\n"
"               i.p.z * vp.z * w, w);\n"
"  o.d0 = i.d0; o.d1 = i.d1; o.f = i.f;\n"
"  o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n"
"  return o;\n"
"}\n";

/* Clears are draws: they honour the clip rectangle and the colour channels
 * CLEAR_SURFACE names, which ClearRenderTargetView cannot. */
static const char s_clear_vs_src[] =
"cbuffer C : register(b0) { float4 col; float4 dep; };\n"
"float4 main(uint id : SV_VertexID) : SV_Position {\n"
"  float2 p = float2((id << 1) & 2, id & 2);\n"
"  return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), dep.x, 1.0);\n"
"}\n";
static const char s_clear_ps_src[] =
"cbuffer C : register(b0) { float4 col; float4 dep; };\n"
"float4 main(float4 p : SV_Position) : SV_Target { return col; }\n";

/* ---- generated pixel shaders --------------------------------------------- */

typedef struct {
    uint32_t control, stage_program, final0, final1, clip_plane_mode;
    uint32_t cicw[8], aicw[8], cocw[8], aocw[8];
    uint8_t cube[4], use_rc, tex0;
    uint8_t alpha_one;                  /* bit per stage: a render target bound for
                                           a format with no alpha reads 1 */
} PsKey;

typedef struct {
    PsKey key;
    ID3D11PixelShader *ps;
} PsEntry;

#define PS_CACHE 1024
static PsEntry s_ps[PS_CACHE];
static int s_ps_count;

typedef struct { char *p; size_t n, cap; } Str;

static void sb(Str *s, const char *fmt, ...)
{
    va_list ap;
    int k;
    for (;;) {
        va_start(ap, fmt);
        k = vsnprintf(s->p + s->n, s->cap - s->n, fmt, ap);
        va_end(ap);
        if (k >= 0 && (size_t)k < s->cap - s->n) {
            s->n += (size_t)k;
            return;
        }
        s->cap = s->cap * 2 + 4096;
        s->p = (char *)realloc(s->p, s->cap);
    }
}

/* An input byte: reg | alpha<<4 | mapping<<5 (nv2a_combiner.c). RGB portion:
 * the alpha bit replicates .a. Alpha portion: it picks .a over .b. */
static void in_rgb(Str *s, const char *name, uint32_t byte)
{
    sb(s, "  float3 %s = mi%u(R[%u].%s);\n", name, (byte >> 5) & 7, byte & 0xF,
       (byte & 0x10) ? "aaa" : "rgb");
}

static void in_alpha(Str *s, const char *name, uint32_t byte)
{
    sb(s, "  float %s = mi%u(R[%u].%s).x;\n", name, (byte >> 5) & 7, byte & 0xF,
       (byte & 0x10) ? "aaa" : "bbb");
}

static const char *map_out(uint32_t m)
{
    switch (m) {
    case 0x08: return "(%s - 0.5)";
    case 0x10: return "(%s * 2.0)";
    case 0x18: return "((%s - 0.5) * 2.0)";
    case 0x20: return "(%s * 4.0)";
    case 0x30: return "(%s * 0.5)";
    default:   return "(%s)";
    }
}

static void emit_out(Str *s, const char *dst, const char *val, uint32_t m)
{
    char inner[96];
    snprintf(inner, sizeof inner, map_out(m), val);
    sb(s, "  %s = clamp(%s, -1.0, 1.0);\n", dst, inner);
}

static char *gen_ps(const PsKey *k)
{
    Str s = { NULL, 0, 0 };
    uint32_t n = k->control & 0xFF, flags = k->control >> 8, st, i;

    sb(&s,
       "Texture2D T0 : register(t0); Texture2D T1 : register(t1);\n"
       "Texture2D T2 : register(t2); Texture2D T3 : register(t3);\n"
       "TextureCube Q0 : register(t4); TextureCube Q1 : register(t5);\n"
       "TextureCube Q2 : register(t6); TextureCube Q3 : register(t7);\n"
       "SamplerState S0 : register(s0); SamplerState S1 : register(s1);\n"
       "SamplerState S2 : register(s2); SamplerState S3 : register(s3);\n"
       "cbuffer P : register(b0) { float4 C0[8]; float4 C1[8]; float4 FC0;\n"
       "  float4 FC1; float4 FOGC; float4 TS[4]; float4 AT; };\n"
       "struct PI { float4 p : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
       "  float4 f : FOG; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
       "  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
       "float3 mi0(float3 x) { return max(x, 0.0); }\n"
       "float3 mi1(float3 x) { return 1.0 - saturate(x); }\n"
       "float3 mi2(float3 x) { return 2.0 * max(x, 0.0) - 1.0; }\n"
       "float3 mi3(float3 x) { return -2.0 * max(x, 0.0) + 1.0; }\n"
       "float3 mi4(float3 x) { return max(x, 0.0) - 0.5; }\n"
       "float3 mi5(float3 x) { return -max(x, 0.0) + 0.5; }\n"
       "float3 mi6(float3 x) { return x; }\n"
       "float3 mi7(float3 x) { return -x; }\n"
       "void atest(float a) {\n"
       "  if (AT.z == 0.0) return;\n"
       "  int v = (int)(a * 255.0 + 0.5), r = (int)AT.x, f = (int)AT.y; bool p;\n"
       "  if (f == 0x200) p = false; else if (f == 0x201) p = v < r;\n"
       "  else if (f == 0x202) p = v == r; else if (f == 0x203) p = v <= r;\n"
       "  else if (f == 0x204) p = v > r; else if (f == 0x205) p = v != r;\n"
       "  else if (f == 0x206) p = v >= r; else p = true;\n"
       "  if (!p) discard;\n"
       "}\n"
       "float4 main(PI i) : SV_Target {\n");

    if (!k->use_rc) {
        /* Before a title programs the combiners: vertex colour, modulated by
         * stage 0 when there is a texture (the software path's modulate). */
        if (k->tex0 && (k->alpha_one & 1))
            sb(&s, "  float4 o = float4(T0.Sample(S0, i.t0.xy * TS[0].xy).rgb, 1.0) * saturate(i.d0);\n");
        else if (k->tex0)
            sb(&s, "  float4 o = T0.Sample(S0, i.t0.xy * TS[0].xy) * saturate(i.d0);\n");
        else
            sb(&s, "  float4 o = saturate(i.d0);\n");
        sb(&s, "  atest(o.a);\n  return o;\n}\n");
        return s.p;
    }

    sb(&s, "  float4 R[16]; float4 t;\n"
           "  R[0] = 0; R[1] = 0; R[2] = 0; R[6] = 0; R[7] = 0;\n"
           "  R[12] = 0; R[13] = 0; R[14] = 0; R[15] = 0;\n"
           "  R[3] = float4(FOGC.rgb, i.f.x); R[4] = i.d0; R[5] = i.d1;\n");

    /* Texture stages (rc_stage_fetch). */
    for (st = 0; st < 4; st++) {
        uint32_t mode = (k->stage_program >> (st * 5)) & 0x1F;
        sb(&s, "  { float4 c = i.t%u;\n", st);
        switch (mode) {
        case 0:
            sb(&s, "    t = float4(0, 0, 0, 1);\n");
            break;
        case 1: case 2:
            sb(&s, "    float q = c.w != 0.0 ? c.w : 1.0;\n"
                   "    t = T%u.Sample(S%u, c.xy / q * TS[%u].xy);\n", st, st, st);
            break;
        case 3:
            if (k->cube[st])
                sb(&s, "    t = Q%u.Sample(S%u, c.xyz);\n", st, st);
            else
                sb(&s, "    t = T%u.Sample(S%u, c.xy * TS[%u].xy);\n", st, st, st);
            break;
        case 4:
            sb(&s, "    t = saturate(c);\n");
            break;
        case 5:
            for (i = 0; i < 4; i++) {
                int ge = (k->clip_plane_mode >> (st * 4 + i)) & 1;
                sb(&s, "    if (c[%u] %s 0.0) discard;\n", i, ge ? ">=" : "<");
            }
            sb(&s, "    t = 0;\n");
            break;
        default:
            sb(&s, "    t = 0;\n");
            break;
        }
        if ((k->alpha_one >> st) & 1)
            sb(&s, "    t.a = 1.0;\n");
        sb(&s, "    R[%u] = t; }\n", 8 + st);
    }
    /* r0.a starts as texture 0's alpha, or 1 with stage 0 off (psh.c). */
    sb(&s, "  R[12].a = %s;\n", (k->stage_program & 0x1F) ? "R[8].a" : "1.0");

    if (n > 8)
        n = 8;
    for (st = 0; st < n; st++) {
        uint32_t icw = k->cicw[st], ocw = k->cocw[st];
        uint32_t aicw = k->aicw[st], aocw = k->aocw[st];
        uint32_t fl = ocw >> 12, afl = aocw >> 12;
        uint32_t c0 = (flags & 0x010) ? st : 0, c1 = (flags & 0x100) ? st : 0;

        sb(&s, "  { R[1] = C0[%u]; R[2] = C1[%u];\n", c0, c1);
        if (flags & 1)
            sb(&s, "  bool mux = R[12].a >= 0.5;\n");
        else
            sb(&s, "  bool mux = (((int)(R[12].a * 255.0)) & 1) != 0;\n");
        in_rgb(&s, "A", icw >> 24); in_rgb(&s, "B", (icw >> 16) & 0xFF);
        in_rgb(&s, "C", (icw >> 8) & 0xFF); in_rgb(&s, "D", icw & 0xFF);
        in_alpha(&s, "aA", aicw >> 24); in_alpha(&s, "aB", (aicw >> 16) & 0xFF);
        in_alpha(&s, "aC", (aicw >> 8) & 0xFF); in_alpha(&s, "aD", aicw & 0xFF);
        sb(&s, "  float3 pab = %s; float3 pcd = %s;\n",
           (fl & 2) ? "dot(A, B).xxx" : "A * B",
           (fl & 1) ? "dot(C, D).xxx" : "C * D");
        if (fl & 4)
            sb(&s, "  float3 sum = mux ? pcd : pab;\n");
        else
            sb(&s, "  float3 sum = pab + pcd;\n");
        sb(&s, "  float apab = aA * aB, apcd = aC * aD;\n");
        if (afl & 4)
            sb(&s, "  float asum = mux ? apcd : apab;\n");
        else
            sb(&s, "  float asum = apab + apcd;\n");
        sb(&s, "  float3 ab, cd, ms; float aab, acd, ams;\n");
        emit_out(&s, "ab", "pab", fl & 0x38);
        emit_out(&s, "cd", "pcd", fl & 0x38);
        emit_out(&s, "ms", "sum", fl & 0x38);
        emit_out(&s, "aab", "apab", afl & 0x38);
        emit_out(&s, "acd", "apcd", afl & 0x38);
        emit_out(&s, "ams", "asum", afl & 0x38);
        if ((ocw >> 4) & 0xF) {
            sb(&s, "  R[%u].rgb = ab;\n", (ocw >> 4) & 0xF);
            if (fl & 0x80)
                sb(&s, "  R[%u].a = ab.b;\n", (ocw >> 4) & 0xF);
        }
        if (ocw & 0xF) {
            sb(&s, "  R[%u].rgb = cd;\n", ocw & 0xF);
            if (fl & 0x40)
                sb(&s, "  R[%u].a = cd.b;\n", ocw & 0xF);
        }
        if ((ocw >> 8) & 0xF)
            sb(&s, "  R[%u].rgb = ms;\n", (ocw >> 8) & 0xF);
        if ((aocw >> 4) & 0xF)
            sb(&s, "  R[%u].a = aab;\n", (aocw >> 4) & 0xF);
        if (aocw & 0xF)
            sb(&s, "  R[%u].a = acd;\n", aocw & 0xF);
        if ((aocw >> 8) & 0xF)
            sb(&s, "  R[%u].a = ams;\n", (aocw >> 8) & 0xF);
        sb(&s, "  R[0] = 0; }\n");
    }

    if (k->final0 || k->final1) {
        uint32_t f0 = k->final0, f1 = k->final1, ff = f1 & 0xFF;
        sb(&s, "  R[1] = FC0; R[2] = FC1;\n");
        sb(&s, "  { float3 a = %s; float3 b = %s; R[14] = float4(%s, 0.0); }\n",
           (ff & 0x40) ? "1.0 - R[5].rgb" : "R[5].rgb",
           (ff & 0x20) ? "1.0 - R[12].rgb" : "R[12].rgb",
           (ff & 0x80) ? "saturate(a + b)" : "a + b");
        in_rgb(&s, "E", f1 >> 24);
        in_rgb(&s, "F", (f1 >> 16) & 0xFF);
        sb(&s, "  R[15] = float4(E * F, 0.0);\n");
        in_rgb(&s, "A", f0 >> 24); in_rgb(&s, "B", (f0 >> 16) & 0xFF);
        in_rgb(&s, "C", (f0 >> 8) & 0xFF); in_rgb(&s, "D", f0 & 0xFF);
        in_alpha(&s, "G", (f1 >> 8) & 0xFF);
        sb(&s, "  float4 o = float4(D + C * (1.0 - A) + B * A, G);\n");
    } else {
        sb(&s, "  float4 o = R[12];\n");
    }
    sb(&s, "  o = saturate(o);\n  atest(o.a);\n  return o;\n}\n");
    return s.p;
}

static ID3D11PixelShader *get_ps(const PsKey *k)
{
    int i;
    char *src;
    ID3DBlob *code;
    ID3D11PixelShader *ps = NULL;

    for (i = 0; i < s_ps_count; i++)
        if (!memcmp(&s_ps[i].key, k, sizeof *k))
            return s_ps[i].ps;
    src = gen_ps(k);
    code = compile(src, "ps_4_0", "combiners");
    if (!code && getenv("RECOMP_GPU_TRACE"))
        fprintf(stderr, "%s\n", src);
    free(src);
    if (!code)
        return NULL;
    if (FAILED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                              ID3D10Blob_GetBufferSize(code), NULL, &ps)))
        ps = NULL;
    ID3D10Blob_Release(code);
    if (ps && s_ps_count < PS_CACHE) {
        s_ps[s_ps_count].key = *k;
        s_ps[s_ps_count].ps = ps;
        s_ps_count++;
    }
    return ps;
}

/* ---- generated vertex shaders ------------------------------------------- */

/* The title's vertex program, statement for statement as nv2a_vsh_interp.c
 * runs it: sources read before either unit writes, the ILU's temp write
 * going to R1 when it shares the instruction with a MAC op, a0-relative
 * constants clamped into range, unwritten outputs at their defaults. Then the
 * same step back from screen space to clip space as the fixed shader above,
 * and the fixed-function fog factor from oFog.x. */

static uint32_t vfield(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

static void vs_src(Str *s, const char *name, const uint32_t *ins, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;
    char base[48];

    if (which == 0) {
        neg = vfield(ins, 1, 8, 1); swz = vfield(ins, 1, 0, 8);
        mux = vfield(ins, 2, 26, 2); reg = vfield(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = vfield(ins, 2, 25, 1); swz = vfield(ins, 2, 17, 8);
        mux = vfield(ins, 2, 11, 2); reg = vfield(ins, 2, 13, 4);
    } else {
        neg = vfield(ins, 2, 10, 1); swz = vfield(ins, 2, 2, 8);
        mux = vfield(ins, 3, 28, 2);
        reg = (vfield(ins, 2, 0, 2) << 2) | vfield(ins, 3, 30, 2);
    }
    if (mux == 1)
        snprintf(base, sizeof base, "%s", reg == 12 ? "P" : "");
    if (mux == 1 && reg != 12)
        snprintf(base, sizeof base, "R[%u]", reg < 12 ? reg : 0);
    else if (mux == 2)
        snprintf(base, sizeof base, "v[%u]", vfield(ins, 1, 9, 4));
    else if (mux != 1) {
        uint32_t ci = vfield(ins, 1, 13, 8);
        if (vfield(ins, 3, 1, 1))
            snprintf(base, sizeof base, "C[cidx(%u + a0)]", ci);
        else
            snprintf(base, sizeof base, "C[%u]", ci < 192 ? ci : 0);
    }
    sb(s, "    float4 %s = %s%s.%c%c%c%c;\n", name, neg ? "-" : "", base,
       sw[(swz >> 6) & 3], sw[(swz >> 4) & 3], sw[(swz >> 2) & 3], sw[swz & 3]);
}

static void vs_write(Str *s, const char *dst, const char *src, uint32_t mask)
{
    if (mask & 8) sb(s, "    %s.x = %s.x;\n", dst, src);
    if (mask & 4) sb(s, "    %s.y = %s.y;\n", dst, src);
    if (mask & 2) sb(s, "    %s.z = %s.z;\n", dst, src);
    if (mask & 1) sb(s, "    %s.w = %s.w;\n", dst, src);
}

static char *gen_vs(const uint32_t (*prog)[4], uint32_t n)
{
    Str s = { NULL, 0, 0 };
    uint32_t i;

    sb(&s,
       "cbuffer K : register(b0) { float4 C[192]; };\n"
       "cbuffer V : register(b1) { float4 vp; float4 fogp; };\n"
       "struct VI {\n");
    for (i = 0; i < 16; i++)
        sb(&s, "  float4 v%u : ATTR%u;\n", i, i);
    sb(&s,
       "};\n"
       "struct VO { float4 p : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
       "  float4 f : FOG; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
       "  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
       "int cidx(int i) { return (i < 0 || i >= 192) ? 0 : i; }\n"
       "float4 splat(float f) { return float4(f, f, f, f); }\n"
       "float fogf(float d) {\n"
       "  if (fogp.x == 0.0) return 1.0;\n"
       "  int mode = (int)fogp.y; float f;\n"
       "  if (mode == 0x800 || mode == 0x802) f = fogp.z + exp2(d * fogp.w * 16.0) - 1.5;\n"
       "  else if (mode == 0x801 || mode == 0x803) f = fogp.z + exp2(-d * d * fogp.w * fogp.w * 32.0) - 1.5;\n"
       "  else f = fogp.z + d * fogp.w - 1.0;\n"
       "  if (mode == 0x802 || mode == 0x803 || mode == 0x804) f = abs(f);\n"
       "  if (f != f) f = 1.0;\n"
       "  return saturate(f);\n"
       "}\n"
       "VO main(VI i) {\n"
       "  float4 v[16];\n");
    for (i = 0; i < 16; i++)
        sb(&s, "  v[%u] = i.v%u;\n", i, i);
    sb(&s,
       "  float4 R[12]; float4 P = 0; float4 O[13]; int a0 = 0;\n");
    for (i = 0; i < 12; i++)
        sb(&s, "  R[%u] = 0;\n", i);
    for (i = 0; i < 13; i++)
        sb(&s, "  O[%u] = float4(0, 0, 0, %s);\n", i,
           (i == 3 || i == 4 || (i >= 9 && i <= 12)) ? "1" : "0");

    for (i = 0; i < n; i++) {
        const uint32_t *ins = prog[i];
        uint32_t mac = vfield(ins, 1, 21, 4), ilu = vfield(ins, 1, 25, 3);
        uint32_t mmask = vfield(ins, 3, 24, 4), imask = vfield(ins, 3, 16, 4);
        uint32_t tdst = vfield(ins, 3, 20, 4);
        uint32_t omask = vfield(ins, 3, 12, 4), oaddr = vfield(ins, 3, 3, 8);

        sb(&s, "  {\n");
        vs_src(&s, "a", ins, 0);
        vs_src(&s, "b", ins, 1);
        vs_src(&s, "c", ins, 2);
        switch (mac) {
        case 0:  sb(&s, "    float4 m = 0;\n"); break;
        case 2:  sb(&s, "    float4 m = a * b;\n"); break;
        case 3:  sb(&s, "    float4 m = a + c;\n"); break;
        case 4:  sb(&s, "    float4 m = a * b + c;\n"); break;
        case 5:  sb(&s, "    float4 m = splat(dot(a.xyz, b.xyz));\n"); break;
        case 6:  sb(&s, "    float4 m = splat(dot(a.xyz, b.xyz) + b.w);\n"); break;
        case 7:  sb(&s, "    float4 m = splat(dot(a, b));\n"); break;
        case 8:  sb(&s, "    float4 m = float4(1.0, a.y * b.y, a.z, b.w);\n"); break;
        case 9:  sb(&s, "    float4 m = (a < b) ? a : b;\n"); break;
        case 10: sb(&s, "    float4 m = (a >= b) ? a : b;\n"); break;
        case 11: sb(&s, "    float4 m = (a < b) ? 1.0 : 0.0;\n"); break;
        case 12: sb(&s, "    float4 m = (a >= b) ? 1.0 : 0.0;\n"); break;
        default: sb(&s, "    float4 m = a;\n"); break;   /* mov, arl, 14, 15 */
        }
        switch (ilu) {
        case 0: sb(&s, "    float4 l = 0;\n"); break;
        case 2: sb(&s, "    float4 l = splat(c.x == 0.0 ? asfloat(0x7f800000) : 1.0 / c.x);\n"); break;
        case 3: sb(&s, "    float rf = 1.0 / c.x;\n"
                       "    if (abs(rf) < 5.42101e-20) rf = rf < 0.0 ? -5.42101e-20 : 5.42101e-20;\n"
                       "    if (abs(rf) > 1.884467e19) rf = rf < 0.0 ? -1.884467e19 : 1.884467e19;\n"
                       "    float4 l = splat(rf);\n"); break;
        case 4: sb(&s, "    float4 l = splat(1.0 / sqrt(abs(c.x)));\n"); break;
        case 5: sb(&s, "    float fl = floor(c.x);\n"
                       "    float4 l = float4(exp2(fl), c.x - fl, exp2(c.x), 1.0);\n"); break;
        case 6: sb(&s, "    float ax = abs(c.x); float4 l;\n"
                       "    if (ax == 0.0) l = float4(-asfloat(0x7f800000), 1.0, -asfloat(0x7f800000), 1.0);\n"
                       "    else { float e = floor(log2(ax)); l = float4(e, ax / exp2(e), log2(ax), 1.0); }\n"); break;
        case 7: sb(&s, "    float nl = max(c.x, 0.0), nh = max(c.y, 0.0);\n"
                       "    float pe = clamp(c.w, -127.9961, 127.9961);\n"
                       "    float pw = nh > 0.0 ? pow(nh, pe) : (pe == 0.0 ? 1.0 : (pe > 0.0 ? 0.0 : asfloat(0x7f800000)));\n"
                       "    float4 l = float4(1.0, nl, c.x > 0.0 ? pw : 0.0, 1.0);\n"); break;
        default: sb(&s, "    float4 l = c;\n"); break;   /* mov */
        }
        if (mac == 13)
            sb(&s, "    a0 = (int)floor(a.x + 0.001);\n");
        if (mac && mac != 13 && mmask && tdst <= 12) {
            char d[8];
            snprintf(d, sizeof d, tdst == 12 ? "P" : "R[%u]", tdst);
            vs_write(&s, d, "m", mmask);
        }
        if (ilu && imask) {
            uint32_t it = mac ? 1u : tdst;
            if (it <= 12) {
                char d[8];
                snprintf(d, sizeof d, it == 12 ? "P" : "R[%u]", it);
                vs_write(&s, d, "l", imask);
            }
        }
        if (omask && vfield(ins, 3, 11, 1)) {
            const char *src = vfield(ins, 3, 2, 1) ? "l" : "m";
            if (oaddr == 0)
                vs_write(&s, "P", src, omask);
            else if (oaddr < 13) {
                char d[8];
                snprintf(d, sizeof d, "O[%u]", oaddr);
                vs_write(&s, d, src, omask);
            }
        }
        sb(&s, "  }\n");
        if (vfield(ins, 3, 0, 1))
            break;
    }
    sb(&s,
       "  VO o; float w = P.w;\n"
       "  if (w == 0.0 || w != w || abs(w) > 3.0e38) w = 1.0;\n"
       "  o.p = float4((P.x * vp.x - 1.0) * w, (1.0 - P.y * vp.y) * w, P.z * vp.z * w, w);\n"
       "  o.d0 = saturate(O[3]); o.d1 = saturate(O[4]);\n"
       "  o.f = float4(fogf(O[5].x), 0.0, 0.0, 1.0);\n"
       "  o.t0 = O[9]; o.t1 = O[10]; o.t2 = O[11]; o.t3 = O[12];\n"
       "  return o;\n"
       "}\n");
    return s.p;
}

typedef struct { uint64_t hash; uint32_t n; ID3D11VertexShader *vs; } VsEntry;
#define VS_CACHE 1024
static VsEntry s_vsc[VS_CACHE];
static int s_vsc_count;

static ID3D11VertexShader *get_program_vs(void)
{
    static uint32_t gen_seen;
    static ID3D11VertexShader *cur;
    static uint32_t prog[NV2A_VSH_SLOTS][4];
    uint32_t gen = nv2a_vsh_program_gen(), n, i;
    uint64_t h;
    char *src;
    ID3DBlob *code;
    ID3D11VertexShader *vs = NULL;

    if (gen == gen_seen && cur)
        return cur;
    n = nv2a_vsh_program_copy(prog, NV2A_VSH_SLOTS);
    if (!n)
        return NULL;
    h = hash_bytes(0xCBF29CE484222325ull, (const uint8_t *)prog, (size_t)n * 16);
    gen_seen = gen;
    for (i = 0; i < (uint32_t)s_vsc_count; i++)
        if (s_vsc[i].hash == h && s_vsc[i].n == n)
            return cur = s_vsc[i].vs;
    src = gen_vs((const uint32_t (*)[4])prog, n);
    code = compile(src, "vs_4_0", "vertex program");
    if (!code && getenv("RECOMP_GPU_TRACE"))
        fprintf(stderr, "%s\n", src);
    free(src);
    cur = NULL;
    if (!code)
        return NULL;
    if (FAILED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                               ID3D10Blob_GetBufferSize(code), NULL, &vs)))
        vs = NULL;
    ID3D10Blob_Release(code);
    if (vs && s_vsc_count < VS_CACHE) {
        s_vsc[s_vsc_count].hash = h;
        s_vsc[s_vsc_count].n = n;
        s_vsc[s_vsc_count].vs = vs;
        s_vsc_count++;
    }
    return cur = vs;
}

/* ---- render targets and depth buffers ---------------------------------- */

typedef struct {
    uint32_t addr, pitch, bpp, w, h;    /* w, h in guest pixels */
    ID3D11Texture2D *tex, *staging;     /* tex is w*s_scale x h*s_scale */
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;      /* tex, for filtering it down */
    ID3D11Texture2D *small;             /* w x h, between tex and guest */
    ID3D11RenderTargetView *small_rtv;
    ID3D11ShaderResourceView *small_srv;
    int gpu_dirty;                      /* GPU holds what guest memory lacks */
    int synced;                         /* guest_hash describes guest memory */
    uint64_t guest_hash;
    uint32_t last_used;
    uint32_t checked;                   /* s_frame guest memory was last compared */
} GpuRt;

typedef struct {
    uint32_t addr, w, h;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
    uint32_t last_used;
} GpuDs;

#define GPU_RTS 8
static GpuRt s_rt[GPU_RTS];
static GpuDs s_ds[GPU_RTS];

/* A sample of the surface's guest memory: one word in sixteen of every other
 * row. It only has to notice that something other than the GPU wrote the
 * surface -- a CPU-decoded video frame, a 2D blit -- which changes far more
 * than it samples, and it runs once a frame for every surface, where a hash
 * of every byte cost 7% of the GPU thread. */
static uint64_t rt_guest_hash(const GpuRt *r)
{
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t y, x, words = r->w * r->bpp / 4;
    for (y = 0; y < r->h; y += 2) {
        const uint32_t *row = (const uint32_t *)guest(r->addr + y * r->pitch);
        for (x = 0; x < words; x += 16) {
            h = (h ^ row[x]) * 0x100000001B3ull;
            h ^= h >> 29;
        }
    }
    return h;
}

/* Draw src over the whole of dst (dw x dh) through a linear sampler: the
 * filter between a scaled surface and its console-sized copy. Leaves no
 * target or view bound, so either can be bound the other way next. */
/* src drawn into the viewport (x, y, w, h) of dst, kept inside the scissor
 * rectangle `clip` (left, top, right, bottom). */
static void blit_rect_ps(ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst,
                         float x, float y, float w, float h, const LONG clip[4],
                         ID3D11PixelShader *ps, ID3D11Buffer *cb);

static void blit_rect(ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst,
                      float x, float y, float w, float h, const LONG clip[4])
{
    blit_rect_ps(src, dst, x, y, w, h, clip, s_blit_ps, NULL);
}

static void blit_rect_ps(ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst,
                         float x, float y, float w, float h, const LONG clip[4],
                         ID3D11PixelShader *ps, ID3D11Buffer *cb)
{
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    ID3D11ShaderResourceView *none = NULL;
    vp.TopLeftX = x; vp.TopLeftY = y; vp.Width = w; vp.Height = h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    sc.left = clip[0]; sc.top = clip[1]; sc.right = clip[2]; sc.bottom = clip[3];
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &dst, NULL);
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, ps, NULL, 0);
    if (cb)
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &src);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_blit_smp);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
}

static void blit(ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst,
                 uint32_t dw, uint32_t dh)
{
    const LONG all[4] = { 0, 0, (LONG)dw, (LONG)dh };
    blit_rect(src, dst, 0.0f, 0.0f, (float)dw, (float)dh, all);
}

/* The console-sized companion of a scaled surface, made on first need. */
static int rt_small(GpuRt *r)
{
    D3D11_TEXTURE2D_DESC d;
    if (r->small)
        return 1;
    memset(&d, 0, sizeof d);
    d.Width = r->w; d.Height = r->h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &r->small))
        || FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)r->small,
                                                      NULL, &r->small_rtv))
        || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)r->small,
                                                        NULL, &r->small_srv))
        || (!r->srv && FAILED(ID3D11Device_CreateShaderResourceView(
                s_dev, (ID3D11Resource *)r->tex, NULL, &r->srv))))
        return 0;
    return 1;
}

/* GPU -> guest. */
static void rt_readback(GpuRt *r)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t x, y;

    if (s_scale > 1) {
        if (!rt_small(r))
            return;
        blit(r->srv, r->small_rtv, r->w, r->h);
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)r->staging,
                                         (ID3D11Resource *)r->small);
    } else
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)r->staging,
                                     (ID3D11Resource *)r->tex);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)r->staging, 0,
                                       D3D11_MAP_READ, 0, &m)))
        return;
    for (y = 0; y < r->h; y++) {
        const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
        uint8_t *dst = guest(r->addr + y * r->pitch);
        if (r->bpp == 4) {
            memcpy(dst, src, (size_t)r->w * 4);
        } else {
            uint16_t *d16 = (uint16_t *)dst;
            for (x = 0; x < r->w; x++) {
                uint32_t c = src[x];
                d16[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0)
                                    | ((c >> 3) & 0x001F));
            }
        }
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)r->staging, 0);
    r->gpu_dirty = 0;
    r->synced = 1;
    r->guest_hash = rt_guest_hash(r);
    nv2a_gpu_guest_written(r->addr, r->pitch * r->h);
}

/* Guest -> GPU. */
static void rt_upload(GpuRt *r)
{
    uint32_t *buf = (uint32_t *)malloc((size_t)r->w * r->h * 4);
    uint32_t x, y;
    if (!buf)
        return;
    for (y = 0; y < r->h; y++) {
        const uint8_t *src = guest(r->addr + y * r->pitch);
        uint32_t *dst = buf + (size_t)y * r->w;
        if (r->bpp == 4) {
            memcpy(dst, src, (size_t)r->w * 4);
        } else {
            const uint16_t *s16 = (const uint16_t *)src;
            for (x = 0; x < r->w; x++) {
                uint32_t t = s16[x];
                dst[x] = 0xFF000000u | ((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5)
                       | ((t & 0x001Fu) << 3);
            }
        }
    }
    if (s_scale > 1 && rt_small(r)) {
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)r->small, 0, NULL,
                                              buf, r->w * 4, 0);
        blit(r->small_srv, r->rtv, r->w * s_scale, r->h * s_scale);
    } else {
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)r->tex, 0, NULL,
                                              buf, r->w * 4, 0);
    }
    free(buf);
}

static void rt_release(GpuRt *r)
{
    if (r->small_srv) ID3D11ShaderResourceView_Release(r->small_srv);
    if (r->small_rtv) ID3D11RenderTargetView_Release(r->small_rtv);
    if (r->small) ID3D11Texture2D_Release(r->small);
    if (r->srv) ID3D11ShaderResourceView_Release(r->srv);
    if (r->rtv) ID3D11RenderTargetView_Release(r->rtv);
    if (r->tex) ID3D11Texture2D_Release(r->tex);
    if (r->staging) ID3D11Texture2D_Release(r->staging);
    memset(r, 0, sizeof *r);
}

static GpuRt *get_rt(uint32_t addr, uint32_t pitch, uint32_t bpp, uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC d;
    GpuRt *r = NULL;
    int i, oldest = 0;

    for (i = 0; i < GPU_RTS; i++) {
        if (s_rt[i].tex && s_rt[i].addr == addr && s_rt[i].pitch == pitch
            && s_rt[i].bpp == bpp) {
            if (s_rt[i].w >= w && s_rt[i].h >= h) {
                s_rt[i].last_used = s_frame;
                return &s_rt[i];
            }
            if (s_rt[i].gpu_dirty)          /* growing: keep what it drew */
                rt_readback(&s_rt[i]);
            if (w < s_rt[i].w) w = s_rt[i].w;
            if (h < s_rt[i].h) h = s_rt[i].h;
            rt_release(&s_rt[i]);
            r = &s_rt[i];
            break;
        }
    }
    if (!r) {
        for (i = 0; i < GPU_RTS; i++) {
            if (!s_rt[i].tex) { r = &s_rt[i]; break; }
            if (s_rt[i].last_used < s_rt[oldest].last_used) oldest = i;
        }
        if (!r) {
            r = &s_rt[oldest];
            if (r->gpu_dirty)
                rt_readback(r);
            rt_release(r);
        }
    }
    memset(&d, 0, sizeof d);
    d.Width = w * s_scale; d.Height = h * s_scale; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &r->tex)))
        return NULL;
    d.Width = w; d.Height = h;              /* the readback copy is guest-sized */
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &r->staging))
        || FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)r->tex,
                                                      NULL, &r->rtv))) {
        rt_release(r);
        return NULL;
    }
    r->addr = addr; r->pitch = pitch; r->bpp = bpp; r->w = w; r->h = h;
    r->last_used = s_frame;
    gpu_log("render target %08X %ux%u, %u bytes/pixel", addr, w, h, bpp);
    return r;
}

/* Before the GPU draws into a surface: if anything but the GPU changed its
 * guest memory since it was last synced, start from that.
 *
 * Checked whenever guest memory holds the surface (after a readback a blit
 * may have written it), and once a frame while the GPU holds it: frames are
 * no longer read back at the flip (nv2a_gpu_present), so a surface can stay
 * the GPU's for good, and the CPU writing a video frame into it is only
 * seen here. */
static void rt_check(GpuRt *r)
{
    uint64_t h;
    if (r->gpu_dirty && r->checked == s_frame)
        return;
    r->checked = s_frame;
    h = rt_guest_hash(r);
    if (!r->synced || h != r->guest_hash)
        rt_upload(r);
    r->guest_hash = h;
    r->synced = 1;
}

static void rt_begin(GpuRt *r)
{
    rt_check(r);
    r->gpu_dirty = 1;
}

static GpuDs *get_ds(uint32_t addr, uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC d;
    GpuDs *z = NULL;
    int i, oldest = 0;

    for (i = 0; i < GPU_RTS; i++)
        if (s_ds[i].tex && s_ds[i].addr == addr && s_ds[i].w == w && s_ds[i].h == h) {
            s_ds[i].last_used = s_frame;
            return &s_ds[i];
        }
    for (i = 0; i < GPU_RTS; i++) {
        if (!s_ds[i].tex) { z = &s_ds[i]; break; }
        if (s_ds[i].last_used < s_ds[oldest].last_used) oldest = i;
    }
    if (!z) {
        z = &s_ds[oldest];
        ID3D11DepthStencilView_Release(z->dsv);
        ID3D11Texture2D_Release(z->tex);
        memset(z, 0, sizeof *z);
    }
    memset(&d, 0, sizeof d);
    d.Width = w * s_scale; d.Height = h * s_scale; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &z->tex)))
        return NULL;
    if (FAILED(ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)z->tex,
                                                   NULL, &z->dsv))) {
        ID3D11Texture2D_Release(z->tex);
        z->tex = NULL;
        return NULL;
    }
    z->addr = addr; z->w = w; z->h = h; z->last_used = s_frame;
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, z->dsv,
        D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    return z;
}

/* ---- textures ------------------------------------------------------------ */

/* RECOMP_TEXFILTER: 0 samples the top level only, as this backend used to
 * (minified textures shimmer); 1 [default] gives a texture the title
 * minifies between mip levels its mip chain -- the title's own levels
 * (RECOMP_TEXMIPS) -- and samples it as the title asks; 2-16 adds anisotropic
 * filtering of that many samples where the title filters linearly. */
static int tex_filter_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("RECOMP_TEXFILTER");
        mode = e && *e ? atoi(e) : 1;
        if (mode < 0) mode = 0;
        if (mode > 16) mode = 16;
    }
    return mode;
}

static int tex_wants_mips(const Nv2aGpuTexture *t)
{
    return tex_filter_mode() > 0 && t->mipmap && !t->cube && t->width > 1 && t->height > 1;
}

/* RECOMP_TEXMIPS: the title's own lower levels, decoded from its texture
 * [default], or "generate": box-filtered on the GPU from the top level, also
 * the fallback when a level does not decode. */
static int tex_mips_from_title(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_TEXMIPS");
        on = !(e && strcmp(e, "generate") == 0);
    }
    return on;
}

typedef struct {
    uint32_t addr, bytes, palette, palette_bytes, width, height, pitch, format;
    int cube;
    int mips;                           /* uploaded with a generated mip chain */
    uint64_t hash;
    uint32_t checked;                   /* frame of the last content check */
    uint32_t last_used;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} GpuTex;

#define GPU_TEXTURES 768
static GpuTex s_tex[GPU_TEXTURES];

/* RECOMP_TEX_STATS=1: every ten seconds, how much texture work the frames
 * cost -- bytes hashed to notice changes, uploads (each a full decode) and
 * the textures uploaded most, with their format and whether they overlap a
 * render target (render-to-texture through guest memory). */
static struct {
    int on;
    DWORD last;
    uint64_t hashed, decoded_px;
    uint32_t uploads, lookups;
    struct { uint32_t addr, fmt, w, h, n, rt; } top[16];
} s_ts = { .on = -1 };

static void tex_stats_upload(const Nv2aGpuTexture *t)
{
    int i, rt = 0, slot = -1, low = 0;
    s_ts.uploads++;
    s_ts.decoded_px += (uint64_t)t->width * t->height * (t->cube ? 6 : 1);
    for (i = 0; i < GPU_RTS; i++)
        if (s_rt[i].tex && t->addr < s_rt[i].addr + s_rt[i].pitch * s_rt[i].h
            && s_rt[i].addr < t->addr + t->bytes)
            rt = 1;
    for (i = 0; i < 16; i++) {
        if (s_ts.top[i].n && s_ts.top[i].addr == t->addr) { slot = i; break; }
        if (s_ts.top[i].n < s_ts.top[low].n) low = i;
    }
    if (slot < 0) {
        slot = low;
        s_ts.top[slot].addr = t->addr; s_ts.top[slot].n = 0;
    }
    s_ts.top[slot].fmt = t->format; s_ts.top[slot].w = t->width;
    s_ts.top[slot].h = t->height; s_ts.top[slot].rt = rt;
    s_ts.top[slot].n++;
}

static void tex_stats_tick(void)
{
    DWORD now = GetTickCount();
    int i;
    if (!s_ts.last) s_ts.last = now;
    if (now - s_ts.last < 10000)
        return;
    gpu_log("textures, %.1f s: %u lookups, %llu MB hashed, %u uploads (%llu Mpx decoded)",
            (now - s_ts.last) / 1000.0, s_ts.lookups,
            (unsigned long long)(s_ts.hashed >> 20), s_ts.uploads,
            (unsigned long long)(s_ts.decoded_px / 1000000));
    for (i = 0; i < 16; i++)
        if (s_ts.top[i].n > 20)
            gpu_log("   %08X fmt %02X %ux%u uploaded %u times%s", s_ts.top[i].addr,
                    s_ts.top[i].fmt, s_ts.top[i].w, s_ts.top[i].h, s_ts.top[i].n,
                    s_ts.top[i].rt ? " (a render target)" : "");
    memset(&s_ts.hashed, 0, sizeof s_ts - offsetof(__typeof__(s_ts), hashed));
    s_ts.last = now;
}

/* Four independent lanes over 32-byte steps: the single chain in
 * hash_bytes waits on its own multiply every 8 bytes, and every texture a
 * frame uses is hashed in full once a frame to notice changes. Only compared
 * within a run, so it need not match hash_bytes (which names shader-cache
 * files and must not change). */
static uint64_t hash_lanes(uint64_t seed, const uint8_t *p, size_t n)
{
    uint64_t a = seed, b = seed ^ 0x9E3779B97F4A7C15ull,
             c = seed ^ 0xC2B2AE3D27D4EB4Full, d = seed ^ 0x165667B19E3779F9ull;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        memcpy(v, p + i, 32);
        a = (a ^ v[0]) * 0x100000001B3ull;
        b = (b ^ v[1]) * 0x100000001B3ull;
        c = (c ^ v[2]) * 0x100000001B3ull;
        d = (d ^ v[3]) * 0x100000001B3ull;
    }
    a ^= (b << 1) ^ (c << 2) ^ (d << 3) ^ (b >> 63) ^ (c >> 62) ^ (d >> 61);
    return hash_bytes(a, p + i, n - i);
}

static uint64_t tex_hash(const Nv2aGpuTexture *t)
{
    uint64_t h = hash_lanes(0xCBF29CE484222325ull, guest(t->addr), t->bytes);
    s_ts.hashed += t->bytes;
    if (t->palette && t->palette_bytes)
        h = hash_lanes(h, guest(t->palette), t->palette_bytes);
    return h;
}

static int tex_upload(GpuTex *e, const Nv2aGpuTexture *t)
{
    D3D11_TEXTURE2D_DESC d;
    D3D11_SUBRESOURCE_DATA init[6];
    D3D11_SHADER_RESOURCE_VIEW_DESC v;
    uint32_t faces = t->cube ? 6 : 1, f;
    size_t face_px = (size_t)t->width * t->height;
    uint32_t *buf = (uint32_t *)malloc(face_px * 4 * faces);

    if (!buf)
        return 0;
    if (s_ts.on > 0)
        tex_stats_upload(t);
    for (f = 0; f < faces; f++) {
        nv2a_gpu_decode_texture(t->tex, f, buf + face_px * f);
        init[f].pSysMem = buf + face_px * f;
        init[f].SysMemPitch = t->width * 4;
        init[f].SysMemSlicePitch = 0;
    }
    if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
    if (e->tex) ID3D11Texture2D_Release(e->tex);
    e->srv = NULL; e->tex = NULL;

    memset(&d, 0, sizeof d);
    d.Width = t->width; d.Height = t->height; d.MipLevels = 1; d.ArraySize = faces;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = t->cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    memset(&v, 0, sizeof v);
    v.Format = d.Format;
    if (t->cube) {
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        v.TextureCube.MipLevels = 1;
    } else {
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
    }
    e->mips = tex_wants_mips(t);
    if (e->mips && tex_mips_from_title()) {
        /* The title's own levels, decoded like the top one -- as many as it
         * provides, which is often fewer than a full chain: the NV2A samples
         * only those, so neither does this. */
        uint32_t n = t->levels, lv, total = 0;
        D3D11_SUBRESOURCE_DATA lvl[16];
        uint32_t *chain;
        if (n > 16) n = 16;
        for (lv = 0; lv < n; lv++) {
            uint32_t lw = (t->width >> lv) ? (t->width >> lv) : 1;
            uint32_t lh = (t->height >> lv) ? (t->height >> lv) : 1;
            total += lw * lh;
        }
        chain = (uint32_t *)malloc((size_t)total * 4);
        if (chain) {
            uint32_t at = 0;
            for (lv = 0; lv < n; lv++) {
                uint32_t lw = (t->width >> lv) ? (t->width >> lv) : 1;
                uint32_t lh = (t->height >> lv) ? (t->height >> lv) : 1;
                if (lv == 0)
                    memcpy(chain, buf, (size_t)lw * lh * 4);
                else if (!nv2a_gpu_decode_texture_level(t->tex, 0, lv, chain + at))
                    break;
                lvl[lv].pSysMem = chain + at;
                lvl[lv].SysMemPitch = lw * 4;
                lvl[lv].SysMemSlicePitch = 0;
                at += lw * lh;
            }
            n = lv;                         /* the levels that decoded */
            d.MipLevels = n;
            v.Texture2D.MipLevels = n;
            if (n > 1
                && SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &d, lvl, &e->tex))
                && SUCCEEDED(ID3D11Device_CreateShaderResourceView(
                       s_dev, (ID3D11Resource *)e->tex, &v, &e->srv))) {
                free(chain);
                free(buf);
                return 1;
            }
            if (e->tex) ID3D11Texture2D_Release(e->tex);
            e->tex = NULL;
            d.MipLevels = 1;
            v.Texture2D.MipLevels = 1;
            free(chain);
        }
        /* else: fall through and generate them */
    }
    if (e->mips) {
        /* The whole chain, filled from the top level by GenerateMips: which
         * needs a render-target-capable texture, so no initial data. */
        d.MipLevels = 0;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        v.Texture2D.MipLevels = (UINT)-1;
    }
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, e->mips ? NULL : init, &e->tex))
        || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex,
                                                        &v, &e->srv))) {
        if (e->tex) ID3D11Texture2D_Release(e->tex);
        e->tex = NULL;
        free(buf);
        return 0;
    }
    if (e->mips) {
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)e->tex, 0, NULL,
                                              buf, t->width * 4, 0);
        ID3D11DeviceContext_GenerateMips(s_ctx, e->srv);
    }
    free(buf);
    return 1;
}

/* RECOMP_RT_DIRECT=1: a linear texture at the address of a surface the GPU
 * drew and still holds is that surface -- the title rendered something and
 * now samples it (glow, blur, a screen copy). Bind the render target itself
 * rather than reading it back, hashing it, decoding it and uploading it
 * again, which stalls the GPU and costs a decode a frame. Not for the target
 * being drawn into (D3D11 cannot read and write one resource at once), and
 * only when the texture lies within the surface. `tw`/`th` are the bound
 * resource's size in texels, for linear (texel) coordinates; `alpha_one`
 * says the format has no alpha, which the surface's alpha must not supply. */
static int rt_direct_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_RT_DIRECT");
        on = e && *e == '1';
    }
    return on;
}

static ID3D11ShaderResourceView *rt_as_texture(const Nv2aGpuTexture *t, const GpuRt *target,
                                               float *tw, float *th, int *alpha_one)
{
    static int why = -1;                /* read once: this runs per texture per draw */
    int i;
    if (why < 0)
        why = getenv("RECOMP_RT_DIRECT_WHY") != NULL;
    if (why) {
        /* Once per address: a texture over a surface the GPU drew, and
         * whether it can be sampled straight from it -- and if not, why. */
        static uint32_t seen[64];
        static int nseen;
        for (i = 0; i < GPU_RTS; i++) {
            const GpuRt *r = &s_rt[i];
            int k;
            if (!r->tex || t->addr < r->addr || t->addr >= r->addr + r->pitch * r->h)
                continue;
            for (k = 0; k < nseen && seen[k] != t->addr; k++) {}
            if (k < nseen || nseen >= 64)
                break;
            seen[nseen++] = t->addr;
            gpu_log("texture %08X %ux%u pitch %u fmt %02X linear %d cube %d over surface "
                    "%08X %ux%u pitch %u: %s%s%s%s", t->addr, t->width, t->height, t->pitch,
                    t->format, t->linear, t->cube, r->addr, r->w, r->h, r->pitch,
                    r == target ? "is the target; " : "", !r->gpu_dirty ? "not GPU-held; " : "",
                    r->addr != t->addr || r->pitch != t->pitch ? "offset/pitch differ; " : "",
                    !t->linear ? "swizzled" : "");
            break;
        }
    }
    if (!rt_direct_on() || !t->linear || t->cube)
        return NULL;
    for (i = 0; i < GPU_RTS; i++) {
        GpuRt *r = &s_rt[i];
        if (!r->tex || r == target || !r->gpu_dirty || r->addr != t->addr
            || r->pitch != t->pitch || t->width > r->w || t->height > r->h)
            continue;
        if (!r->srv && FAILED(ID3D11Device_CreateShaderResourceView(
                s_dev, (ID3D11Resource *)r->tex, NULL, &r->srv)))
            return NULL;
        *tw = (float)r->w;
        *th = (float)r->h;
        /* X8R8G8B8, R5G6B5, X1R5G5B5 */
        *alpha_one = t->format == 0x1E || t->format == 0x11 || t->format == 0x1C;
        r->last_used = s_frame;
        return r->srv;
    }
    return NULL;
}

static ID3D11ShaderResourceView *get_tex(const Nv2aGpuTexture *t)
{
    GpuTex *e = NULL;
    int i, oldest = 0;

    if (s_ts.on < 0)
        s_ts.on = getenv("RECOMP_TEX_STATS") != NULL;
    if (s_ts.on) {
        s_ts.lookups++;
        tex_stats_tick();
    }

    if (!t->valid || !t->width || !t->height || t->width > 4096 || t->height > 4096)
        return NULL;
    /* Render to texture, through guest memory: bring back anything the GPU
     * drew over these bytes before they are read. */
    nv2a_gpu_sync(t->addr, t->bytes);

    for (i = 0; i < GPU_TEXTURES; i++) {
        GpuTex *c = &s_tex[i];
        if (c->srv && c->addr == t->addr && c->format == t->format
            && c->width == t->width && c->height == t->height && c->pitch == t->pitch
            && c->palette == t->palette && c->cube == t->cube && c->bytes == t->bytes
            && c->mips == tex_wants_mips(t)) {
            e = c;
            break;
        }
    }
    if (e) {
        if (e->checked != s_frame) {
            uint64_t h = tex_hash(t);
            e->checked = s_frame;
            if (h != e->hash) {
                e->hash = h;
                if (!tex_upload(e, t))
                    return NULL;
            }
        }
        e->last_used = s_frame;
        return e->srv;
    }
    for (i = 0; i < GPU_TEXTURES; i++) {
        if (!s_tex[i].srv) { e = &s_tex[i]; break; }
        if (s_tex[i].last_used < s_tex[oldest].last_used) oldest = i;
    }
    if (!e) {
        e = &s_tex[oldest];
        if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
        if (e->tex) ID3D11Texture2D_Release(e->tex);
        memset(e, 0, sizeof *e);
    }
    e->addr = t->addr; e->bytes = t->bytes; e->palette = t->palette;
    e->palette_bytes = t->palette_bytes; e->width = t->width; e->height = t->height;
    e->pitch = t->pitch; e->format = t->format; e->cube = t->cube;
    e->hash = tex_hash(t);
    e->checked = s_frame;
    e->last_used = s_frame;
    if (!tex_upload(e, t)) {
        memset(e, 0, sizeof *e);
        return NULL;
    }
    return e->srv;
}

/* ---- fixed-function state ------------------------------------------------ */

typedef struct { uint64_t key; void *obj; } StateEntry;
static StateEntry s_blend[64], s_dss[64], s_smp[32];

static D3D11_BLEND blend_factor(uint32_t f, int *constant_alpha)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return D3D11_BLEND_SRC_COLOR;
    case 0x0301: return D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return D3D11_BLEND_DEST_COLOR;
    case 0x0307: return D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 0x8003: *constant_alpha = 1; return D3D11_BLEND_BLEND_FACTOR;
    case 0x8004: *constant_alpha = 1; return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return f ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO;
    }
}

static D3D11_BLEND alpha_slot(D3D11_BLEND b)
{
    switch (b) {                      /* colour factors are not allowed there */
    case D3D11_BLEND_SRC_COLOR:      return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR:  return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR:     return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    default:                         return b;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static UINT8 write_mask(uint32_t cm)
{
    UINT8 m = 0;
    if (cm & 0x00010000u) m |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (cm & 0x00000100u) m |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (cm & 0x00000001u) m |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (cm & 0x01000000u) m |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    return m;
}

static void *cache_get(StateEntry *c, int n, uint64_t key)
{
    int i;
    for (i = 0; i < n && c[i].obj; i++)
        if (c[i].key == key)
            return c[i].obj;
    return NULL;
}

static void cache_put(StateEntry *c, int n, uint64_t key, void *obj)
{
    int i;
    for (i = 0; i < n; i++)
        if (!c[i].obj) { c[i].key = key; c[i].obj = obj; return; }
    /* Full: keep it working, just uncached (it is released with the device). */
}

static ID3D11BlendState *get_blend(uint32_t enable, uint32_t src, uint32_t dst,
                                   uint32_t eq, UINT8 mask, int *constant_alpha)
{
    uint64_t key = ((uint64_t)(enable & 1) << 63) | ((uint64_t)(src & 0xFFFF) << 40)
                 | ((uint64_t)(dst & 0xFFFF) << 24) | ((uint64_t)(eq & 0xFFFF) << 8) | mask;
    ID3D11BlendState *b = (ID3D11BlendState *)cache_get(s_blend, 64, key);
    D3D11_BLEND_DESC d;

    *constant_alpha = 0;
    blend_factor(src, constant_alpha);
    blend_factor(dst, constant_alpha);
    if (b)
        return b;
    memset(&d, 0, sizeof d);
    d.RenderTarget[0].BlendEnable = enable && !(src == 1 && dst == 0);
    d.RenderTarget[0].SrcBlend = blend_factor(src, constant_alpha);
    d.RenderTarget[0].DestBlend = blend_factor(dst, constant_alpha);
    d.RenderTarget[0].BlendOp = blend_op(eq);
    d.RenderTarget[0].SrcBlendAlpha = alpha_slot(d.RenderTarget[0].SrcBlend);
    d.RenderTarget[0].DestBlendAlpha = alpha_slot(d.RenderTarget[0].DestBlend);
    d.RenderTarget[0].BlendOpAlpha = d.RenderTarget[0].BlendOp;
    d.RenderTarget[0].RenderTargetWriteMask = mask;
    if (FAILED(ID3D11Device_CreateBlendState(s_dev, &d, &b)))
        return NULL;
    cache_put(s_blend, 64, key, b);
    return b;
}

static D3D11_COMPARISON_FUNC cmp(uint32_t f)
{
    switch (f) {
    case 0x200: return D3D11_COMPARISON_NEVER;
    case 0x201: return D3D11_COMPARISON_LESS;
    case 0x202: return D3D11_COMPARISON_EQUAL;
    case 0x203: return D3D11_COMPARISON_LESS_EQUAL;
    case 0x204: return D3D11_COMPARISON_GREATER;
    case 0x205: return D3D11_COMPARISON_NOT_EQUAL;
    case 0x206: return D3D11_COMPARISON_GREATER_EQUAL;
    default:    return D3D11_COMPARISON_ALWAYS;
    }
}

static D3D11_STENCIL_OP sop(uint32_t op)
{
    switch (op) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

static ID3D11DepthStencilState *get_dss(const Nv2aGpuState *st, int has_ds)
{
    uint64_t key;
    ID3D11DepthStencilState *s;
    D3D11_DEPTH_STENCIL_DESC d;
    int dt = has_ds && st->depth_test, dw = has_ds && st->depth_mask;
    int sten = has_ds && st->stencil_test;

    key = (uint64_t)dt | ((uint64_t)dw << 1) | ((uint64_t)sten << 2)
        | ((uint64_t)(st->depth_func & 7) << 3) | ((uint64_t)(st->stencil_func & 7) << 6)
        | ((uint64_t)(st->stencil_fmask & 0xFF) << 9) | ((uint64_t)(st->stencil_wmask & 0xFF) << 17)
        | ((uint64_t)sop(st->stencil_op_fail) << 25) | ((uint64_t)sop(st->stencil_op_zfail) << 29)
        | ((uint64_t)sop(st->stencil_op_zpass) << 33);
    s = (ID3D11DepthStencilState *)cache_get(s_dss, 64, key);
    if (s)
        return s;
    memset(&d, 0, sizeof d);
    d.DepthEnable = dt || dw;
    d.DepthWriteMask = dw ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = dt ? cmp(st->depth_func) : D3D11_COMPARISON_ALWAYS;
    d.StencilEnable = sten;
    d.StencilReadMask = (UINT8)st->stencil_fmask;
    d.StencilWriteMask = (UINT8)st->stencil_wmask;
    d.FrontFace.StencilFunc = cmp(st->stencil_func);
    d.FrontFace.StencilFailOp = sop(st->stencil_op_fail);
    d.FrontFace.StencilDepthFailOp = sop(st->stencil_op_zfail);
    d.FrontFace.StencilPassOp = sop(st->stencil_op_zpass);
    d.BackFace = d.FrontFace;
    if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &d, &s)))
        return NULL;
    cache_put(s_dss, 64, key, s);
    return s;
}

/* `mip`: the title's filtering between mip levels (Nv2aGpuTexture.mipmap),
 * when the bound view has levels; anisotropic per RECOMP_TEXFILTER. */
static ID3D11SamplerState *get_sampler(int linear, int wrap_u, int wrap_v, int mip)
{
    uint64_t key = (uint64_t)linear | ((uint64_t)wrap_u << 1) | ((uint64_t)wrap_v << 2)
                 | ((uint64_t)mip << 3);
    ID3D11SamplerState *s = (ID3D11SamplerState *)cache_get(s_smp, 32, key);
    D3D11_SAMPLER_DESC d;
    int aniso = tex_filter_mode();
    if (s)
        return s;
    memset(&d, 0, sizeof d);
    if (linear && mip && aniso >= 2) {
        d.Filter = D3D11_FILTER_ANISOTROPIC;
        d.MaxAnisotropy = (UINT)aniso;
    } else if (linear)
        d.Filter = mip == 2 ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    else
        d.Filter = mip == 2 ? D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    d.AddressU = wrap_u ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    d.AddressV = wrap_v ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &d, &s)))
        return NULL;
    cache_put(s_smp, 32, key, s);
    return s;
}

/* ---- device -------------------------------------------------------------- */

static ID3D11Buffer *make_cb(UINT bytes)
{
    D3D11_BUFFER_DESC d;
    ID3D11Buffer *b = NULL;
    memset(&d, 0, sizeof d);
    d.ByteWidth = (bytes + 15) & ~15u;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(s_dev, &d, NULL, &b);
    return b;
}

static void cb_write(ID3D11Buffer *b, const void *data, size_t n)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)b, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, data, n);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)b, 0);
    }
}

static ID3D11ShaderResourceView *make_white(int cube)
{
    D3D11_TEXTURE2D_DESC d;
    D3D11_SUBRESOURCE_DATA init[6];
    D3D11_SHADER_RESOURCE_VIEW_DESC v;
    ID3D11Texture2D *t = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    static const uint32_t white = 0xFFFFFFFFu;
    int f;

    memset(&d, 0, sizeof d);
    d.Width = 1; d.Height = 1; d.MipLevels = 1; d.ArraySize = cube ? 6 : 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    for (f = 0; f < 6; f++) {
        init[f].pSysMem = &white;
        init[f].SysMemPitch = 4;
        init[f].SysMemSlicePitch = 0;
    }
    memset(&v, 0, sizeof v);
    v.Format = d.Format;
    v.ViewDimension = cube ? D3D11_SRV_DIMENSION_TEXTURECUBE : D3D11_SRV_DIMENSION_TEXTURE2D;
    if (cube) v.TextureCube.MipLevels = 1; else v.Texture2D.MipLevels = 1;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, init, &t)))
        return NULL;
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t, &v, &srv);
    ID3D11Texture2D_Release(t);
    return srv;
}

static int gpu_init(void)
{
    static const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                              D3D_FEATURE_LEVEL_10_0 };
    static const D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "FOG",      0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    D3D_FEATURE_LEVEL got;
    ID3DBlob *code;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                           D3D11_CREATE_DEVICE_SINGLETHREADED, want, 3,
                           D3D11_SDK_VERSION, &s_dev, &got, &s_ctx);
    if (FAILED(hr)) {
        gpu_log("no Direct3D 11 device (0x%08lX); staying on the software rasteriser",
                (unsigned long)hr);
        return 0;
    }
    {
        IDXGIDevice *dx = NULL;
        IDXGIAdapter *ad = NULL;
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&dx))
            && SUCCEEDED(IDXGIDevice_GetAdapter(dx, &ad))
            && SUCCEEDED(IDXGIAdapter_GetDesc(ad, &desc)))
            gpu_log("device on %ls, feature level %X", desc.Description, (unsigned)got);
        if (ad) IDXGIAdapter_Release(ad);
        if (dx) IDXGIDevice_Release(dx);
    }

    code = compile(s_vs_src, "vs_4_0", "transform");
    if (!code)
        return 0;
    hr = ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                         ID3D10Blob_GetBufferSize(code), NULL, &s_vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateInputLayout(s_dev, layout, 8, ID3D10Blob_GetBufferPointer(code),
                                            ID3D10Blob_GetBufferSize(code), &s_layout);
    ID3D10Blob_Release(code);
    if (FAILED(hr))
        return 0;
    code = compile(s_clear_vs_src, "vs_4_0", "clear vs");
    if (!code || FAILED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                                        ID3D10Blob_GetBufferSize(code), NULL,
                                                        &s_clear_vs)))
        return 0;
    ID3D10Blob_Release(code);
    code = compile(s_clear_ps_src, "ps_4_0", "clear ps");
    if (!code || FAILED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                                       ID3D10Blob_GetBufferSize(code), NULL,
                                                       &s_clear_ps)))
        return 0;
    ID3D10Blob_Release(code);

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = GPU_VB_BYTES;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vb)))
        return 0;
    {
        static const float zero[4];
        D3D11_BUFFER_DESC zd;
        D3D11_SUBRESOURCE_DATA zi;
        memset(&zd, 0, sizeof zd);
        zd.ByteWidth = sizeof zero;
        zd.Usage = D3D11_USAGE_IMMUTABLE;
        zd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        memset(&zi, 0, sizeof zi);
        zi.pSysMem = zero;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &zd, &zi, &s_zero_vb)))
            return 0;
    }
    {
        /* One input layout serves every generated program: v0..v15 as
         * float4, made against a shader that reads all sixteen (a program's
         * own signature is a subset, which a layout may exceed). Input k
         * comes from slot k, so each is uploaded as its own packed array
         * and only the ones a program reads are sent. */
        D3D11_INPUT_ELEMENT_DESC al[16];
        Str src = { NULL, 0, 0 };
        int k;
        sb(&src, "struct VI {");
        for (k = 0; k < 16; k++)
            sb(&src, " float4 v%d : ATTR%d;", k, k);
        sb(&src, " };\nfloat4 main(VI i) : SV_Position { return 0");
        for (k = 0; k < 16; k++)
            sb(&src, " + i.v%d", k);
        sb(&src, "; }\n");
        code = compile(src.p, "vs_4_0", "attribute layout");
        free(src.p);
        if (!code)
            return 0;
        for (k = 0; k < 16; k++) {
            al[k].SemanticName = "ATTR";
            al[k].SemanticIndex = (UINT)k;
            al[k].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            al[k].InputSlot = (UINT)k;
            al[k].AlignedByteOffset = 0;
            al[k].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            al[k].InstanceDataStepRate = 0;
        }
        hr = ID3D11Device_CreateInputLayout(s_dev, al, 16, ID3D10Blob_GetBufferPointer(code),
                                            ID3D10Blob_GetBufferSize(code), &s_attr_layout);
        ID3D10Blob_Release(code);
        if (FAILED(hr))
            return 0;
    }
    bd.ByteWidth = GPU_IB_BYTES;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_ib)))
        return 0;
    s_const_cb = make_cb(16 * 192);
    s_prog_cb = make_cb(32);
    if (!s_const_cb || !s_prog_cb)
        return 0;
    s_vs_cb = make_cb(16);
    s_ps_cb = make_cb(sizeof(float) * 4 * (8 + 8 + 3 + 4 + 1));
    s_clear_cb = make_cb(32);
    if (!s_vs_cb || !s_ps_cb || !s_clear_cb)
        return 0;

    /* Scissor for the clip rectangle, and depth clamped rather than clipped:
     * it clipped only at the near plane, at w just above 0. s_rs culls
     * nothing (clears use it); the other two cull clockwise and
     * counter-clockwise triangles, as they appear on the target. */
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    rd.ScissorEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs)))
        return 0;
    rd.CullMode = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_cull[0])))
        return 0;
    rd.FrontCounterClockwise = FALSE;
    if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_cull[1])))
        return 0;
    {
        static const char vs_src[] =
            "struct VO { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
            "VO main(uint id : SV_VertexID) { VO o;\n"
            "  float2 t = float2((id << 1) & 2, id & 2);\n"
            "  o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = t; return o; }\n";
        static const char ps_src[] =
            "Texture2D T : register(t0); SamplerState S : register(s0);\n"
            "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
            "{ return T.Sample(S, uv); }\n";
        D3D11_SAMPLER_DESC sd;
        const char *e = getenv("RECOMP_SCALE");
        ID3DBlob *bvs = compile(vs_src, "vs_4_0", "blit vs");
        ID3DBlob *bps = compile(ps_src, "ps_4_0", "blit ps");
        if (bvs) {
            ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(bvs),
                                            ID3D10Blob_GetBufferSize(bvs), NULL, &s_blit_vs);
            ID3D10Blob_Release(bvs);
        }
        if (bps) {
            ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(bps),
                                           ID3D10Blob_GetBufferSize(bps), NULL, &s_blit_ps);
            ID3D10Blob_Release(bps);
        }
        if (sharpen_amount() > 0.0f) {
            static const char sharp_src[] =
                "Texture2D T : register(t0); SamplerState S : register(s0);\n"
                "cbuffer C : register(b0) { float2 texel; float peak; float pad; };\n"
                "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
                "{\n"
                "  float4 c4 = T.Sample(S, uv);\n"
                "  float3 c = c4.rgb;\n"
                "  float3 n = T.Sample(S, uv - float2(0, texel.y)).rgb;\n"
                "  float3 s = T.Sample(S, uv + float2(0, texel.y)).rgb;\n"
                "  float3 w = T.Sample(S, uv - float2(texel.x, 0)).rgb;\n"
                "  float3 e = T.Sample(S, uv + float2(texel.x, 0)).rgb;\n"
                "  float3 mn = min(c, min(min(n, s), min(w, e)));\n"
                "  float3 mx = max(c, max(max(n, s), max(w, e)));\n"
                /* headroom to the nearer of 0 and 1, relative to the brightest:
                 * small on an edge, large on a flat patch */
                "  float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));\n"
                "  float3 k = amp * peak;\n"
                "  float3 r = (c + (n + s + w + e) * k) / (1.0 + 4.0 * k);\n"
                "  return float4(saturate(r), c4.a);\n"
                "}\n";
            ID3DBlob *bsp = compile(sharp_src, "ps_4_0", "sharpen ps");
            if (bsp) {
                ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(bsp),
                                               ID3D10Blob_GetBufferSize(bsp), NULL, &s_sharp_ps);
                ID3D10Blob_Release(bsp);
            }
            s_sharp_cb = make_cb(16);
            if (s_sharp_ps && s_sharp_cb)
                gpu_log("sharpening the presented frame (RECOMP_SHARPEN=%.2f)", sharpen_amount());
        }
        memset(&sd, 0, sizeof sd);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(s_dev, &sd, &s_blit_smp);
        s_scale = e ? (uint32_t)atoi(e) : 1;
        if (s_scale < 1 || s_scale > 4)
            s_scale = 1;
        if (s_scale > 1 && (!s_blit_vs || !s_blit_ps || !s_blit_smp)) {
            gpu_log("no blit shaders; rendering at the console's resolution");
            s_scale = 1;
        }
        if (s_scale > 1)
            gpu_log("rendering at %ux the console's resolution", s_scale);
    }
    s_white2d = make_white(0);
    s_whitecube = make_white(1);
    gpu_log("ready: vertex programs, rasterisation and combiners on the GPU");
    return 1;
}

int nv2a_gpu_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GPU");
        on = e && *e == '1';
    }
    if (!on)
        return 0;
    if (s_state < 0) {
        s_state = gpu_init();
        if (s_state != 1)
            xbox_FramebufferWindowGpuUnavailable();
    }
    return s_state == 1;
}

/* ---- freed memory -------------------------------------------------------- */

/* Surfaces and textures here are known by guest address, and freeing guest
 * memory told this backend nothing. Once freed blocks were reused
 * (RECOMP_CONTIG_FREE), a render target the GPU still held over memory the
 * title had freed and given to something else either stood in for the new
 * texture (rt_as_texture) or was read back over the title's fresh data
 * (nv2a_gpu_sync): Alter Echo's talking head for Nevin in the Surface: Arana
 * level intro came out as nothing above the neck. So a free is queued here,
 * from the title's thread, and the GPU thread drops what overlaps it before
 * its next command. Dropping without a readback is right: the title's
 * Direct3D waits for the GPU to be done with a resource before freeing it. */
#define GPU_FORGET_MAX 64
static struct { uint32_t addr, end; } s_forget[GPU_FORGET_MAX];
static int s_forget_n;
static volatile long s_forget_pending, s_forget_lock;

/* A spinlock, held for a few stores: the native build's windows.h is
 * DXVK's, with no SRW locks or Interlocked calls. */
#if defined(_MSC_VER)
#define FORGET_XCHG(p, v) InterlockedExchange((volatile LONG *)(p), (v))
#define FORGET_LOAD(p)    InterlockedCompareExchange((volatile LONG *)(p), 0, 0)
#else
#define FORGET_XCHG(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define FORGET_LOAD(p)    __atomic_load_n((p), __ATOMIC_SEQ_CST)
#endif
static void forget_lock(void)   { while (FORGET_XCHG(&s_forget_lock, 1)) {} }
static void forget_unlock(void) { FORGET_XCHG(&s_forget_lock, 0); }

void nv2a_gpu_forget(uint32_t addr, uint32_t bytes)
{
    if (!bytes)
        return;
    forget_lock();
    if (s_forget_n < GPU_FORGET_MAX) {
        s_forget[s_forget_n].addr = addr;
        s_forget[s_forget_n].end = addr + bytes;
        s_forget_n++;
    } else {                            /* full: widen the last range */
        if (addr < s_forget[GPU_FORGET_MAX - 1].addr)
            s_forget[GPU_FORGET_MAX - 1].addr = addr;
        if (addr + bytes > s_forget[GPU_FORGET_MAX - 1].end)
            s_forget[GPU_FORGET_MAX - 1].end = addr + bytes;
    }
    FORGET_XCHG(&s_forget_pending, 1);
    forget_unlock();
}

static void forget_drain(void)
{
    static int logged;
    int i, k, n, dropped = 0;
    struct { uint32_t addr, end; } q[GPU_FORGET_MAX];

    if (!FORGET_LOAD(&s_forget_pending))
        return;
    forget_lock();
    n = s_forget_n;
    memcpy(q, s_forget, sizeof q[0] * (size_t)n);
    s_forget_n = 0;
    FORGET_XCHG(&s_forget_pending, 0);
    forget_unlock();
    for (k = 0; k < n; k++) {
        for (i = 0; i < GPU_RTS; i++) {
            GpuRt *r = &s_rt[i];
            if (r->tex && r->addr < q[k].end && q[k].addr < r->addr + r->pitch * r->h) {
                rt_release(r);
                dropped++;
            }
            if (s_ds[i].tex && s_ds[i].addr >= q[k].addr && s_ds[i].addr < q[k].end) {
                ID3D11DepthStencilView_Release(s_ds[i].dsv);
                ID3D11Texture2D_Release(s_ds[i].tex);
                memset(&s_ds[i], 0, sizeof s_ds[i]);
            }
        }
        for (i = 0; i < GPU_TEXTURES; i++)
            if (s_tex[i].srv && s_tex[i].addr < q[k].end && q[k].addr < s_tex[i].addr + s_tex[i].bytes)
                s_tex[i].checked = 0;    /* hash it again before trusting it */
    }
    if (dropped && logged++ < 3)
        gpu_log("dropped %d surface(s) over memory the title freed", dropped);
}

/* ---- sync points --------------------------------------------------------- */

void nv2a_gpu_sync(uint32_t addr, uint32_t bytes)
{
    int i;
    if (s_state != 1)
        return;
    forget_drain();
    for (i = 0; i < GPU_RTS; i++) {
        GpuRt *r = &s_rt[i];
        if (r->tex && r->gpu_dirty && addr < r->addr + r->pitch * r->h
            && r->addr < addr + bytes)
            rt_readback(r);
    }
}

void nv2a_gpu_guest_written(uint32_t addr, uint32_t bytes)
{
    int i;
    if (s_state != 1)
        return;
    for (i = 0; i < GPU_TEXTURES; i++) {
        GpuTex *e = &s_tex[i];
        if (e->srv && addr < e->addr + e->bytes && e->addr < addr + bytes)
            e->checked = 0;
        else if (e->srv && e->palette && addr < e->palette + e->palette_bytes
                 && e->palette < addr + bytes)
            e->checked = 0;
    }
}

/* NV09F image blits, on the GPU too.
 *
 * The executor copies a blit's bytes in guest memory (image_blit), and at
 * RECOMP_SCALE > 1 guest memory only holds the console-sized readback of a
 * scaled surface. A title that copies its back buffer into a texture for a
 * full-screen effect -- Alter Echo's loading transition and its exposition
 * screens -- then drew that effect from a 640x480 frame, and the whole
 * picture went soft. When the GPU drew the source, also copy its scaled
 * pixels into a surface at the destination: with RECOMP_RT_DIRECT that is
 * what the effect samples (rt_as_texture). Guest memory still gets the
 * executor's copy and is recorded as matching, so the scaled pixels are not
 * replaced by an upload of the small ones.
 *
 * nv2a_gpu_blit_source runs before the executor's copy reads the source back
 * (afterwards it no longer looks like the GPU's), nv2a_gpu_blit_copy after.
 * RECOMP_BLIT_GPU=0 leaves blits to guest memory alone, as before. */
int nv2a_gpu_blit_source(uint32_t src, uint32_t pitch, uint32_t bpp, uint32_t w, uint32_t h)
{
    static int on = -1;
    int i;
    if (on < 0) {
        const char *e = getenv("RECOMP_BLIT_GPU");
        on = !(e && *e == '0');
    }
    if (!on || s_state != 1 || s_scale < 2 || !rt_direct_on())
        return -1;
    forget_drain();
    for (i = 0; i < GPU_RTS; i++) {
        const GpuRt *r = &s_rt[i];
        uint32_t off;
        if (!r->tex || !r->gpu_dirty || r->pitch != pitch || r->bpp != bpp
            || src < r->addr || src >= r->addr + r->pitch * r->h)
            continue;
        off = src - r->addr;
        if (off % pitch % bpp == 0 && off % pitch / bpp + w <= r->w && off / pitch + h <= r->h)
            return i;
    }
    return -1;
}

void nv2a_gpu_blit_copy(int src_rt, uint32_t src, uint32_t dst, uint32_t pitch,
                        uint32_t w, uint32_t h)
{
    GpuRt *s, *d;
    uint32_t off, s_addr;
    D3D11_BOX box;

    if (s_state != 1 || src_rt < 0 || src_rt >= GPU_RTS || !s_rt[src_rt].tex)
        return;
    s = &s_rt[src_rt];
    if (dst < s->addr + s->pitch * s->h && s->addr < dst + pitch * h)
        return;                         /* within one surface: the guest copy alone */
    s_addr = s->addr;
    s->last_used = s_frame;             /* not the one get_rt evicts */
    d = get_rt(dst, pitch, s->bpp, w, h);
    if (!d || d == s || !s->tex || s->addr != s_addr)
        return;
    if (w < d->w || h < d->h)
        rt_check(d);                    /* the rest of it is guest memory's */
    off = src - s->addr;
    box.left = off % s->pitch / s->bpp * s_scale;
    box.top = off / s->pitch * s_scale;
    box.right = box.left + w * s_scale;
    box.bottom = box.top + h * s_scale;
    box.front = 0;
    box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)d->tex, 0, 0, 0, 0,
                                              (ID3D11Resource *)s->tex, 0, &box);
    d->gpu_dirty = 1;
    d->synced = 1;
    d->guest_hash = rt_guest_hash(d);
    d->checked = s_frame;
    {
        static int logged;
        if (!logged++)
            gpu_log("image blits of GPU-drawn surfaces copied at %ux: %08X -> %08X %ux%u",
                    s_scale, src, dst, w, h);
    }
}

void nv2a_gpu_frame(void)
{
    if (s_state == 1)
        forget_drain();
    s_frame++;
    if (s_frame == 0)
        s_frame = 1;
}

/* ---- presenting ---------------------------------------------------------- */

/* Frames used to reach the screen through guest memory: read back from the
 * GPU at every flip (a stall while the GPU finished, then a copy), copied
 * again for the window, again by the window, and drawn with GDI on the
 * window's own 16 ms clock -- so even a steady 60 fps showed doubled and
 * dropped frames. Now the finished surface is copied on the GPU into a
 * swap chain on the same window and presented with vsync: one GPU copy,
 * shown at the display's refresh, and the swap chain's queue limits the
 * title to the display's rate the way the console's flip did.
 *
 * RECOMP_GPU_PRESENT=0 keeps the old path. */
extern void *xbox_FramebufferWindowHwnd(void);
extern void xbox_FramebufferWindowGpuOwned(int on);

static IDXGISwapChain *s_swap;
static uint32_t s_swap_w, s_swap_h;
static int s_swap_failed;

static int swap_create(HWND hwnd, uint32_t w, uint32_t h)
{
    IDXGIDevice *dx = NULL;
    IDXGIDevice1 *dx1 = NULL;
    IDXGIAdapter *ad = NULL;
    IDXGIFactory *f = NULL;
    DXGI_SWAP_CHAIN_DESC d;
    HRESULT hr = E_FAIL;

    memset(&d, 0, sizeof d);
    d.BufferDesc.Width = w;
    d.BufferDesc.Height = h;
    d.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.OutputWindow = hwnd;
    d.Windowed = TRUE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (SUCCEEDED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&dx))
        && SUCCEEDED(IDXGIDevice_GetAdapter(dx, &ad))
        && SUCCEEDED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&f)))
        hr = IDXGIFactory_CreateSwapChain(f, (IUnknown *)s_dev, &d, &s_swap);
    /* Two frames queued at most: enough to keep the GPU fed, few enough
     * that a button press shows within ~33 ms. */
    if (SUCCEEDED(hr) && dx
        && SUCCEEDED(IDXGIDevice_QueryInterface(dx, &IID_IDXGIDevice1, (void **)&dx1))) {
        IDXGIDevice1_SetMaximumFrameLatency(dx1, 2);
        IDXGIDevice1_Release(dx1);
    }
    if (f) {
        /* The window thread owns Alt+Enter and sizing; DXGI must not. */
        IDXGIFactory_MakeWindowAssociation(f, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        IDXGIFactory_Release(f);
    }
    if (ad) IDXGIAdapter_Release(ad);
    if (dx) IDXGIDevice_Release(dx);
    if (FAILED(hr)) {
        gpu_log("no swap chain on the window (0x%08lX); frames go through guest memory",
                (unsigned long)hr);
        s_swap = NULL;
        return 0;
    }
    s_swap_w = w;
    s_swap_h = h;
    gpu_log("presenting straight to the window (%ux%u now, 4:3 letterboxed), vsync", w, h);
    return 1;
}

/* RECOMP_GPU_SHOT=<prefix>: every RECOMP_GPU_SHOT_EVERY presented frames
 * [60], the frame at the resolution it was rendered, as <prefix>NNNNN.bmp
 * (32-bit, top-down), read straight from the GPU: what the window shows,
 * without the window having to be on screen, and not the console-sized guest
 * copy RECOMP_FB_DUMP takes. At most 1000. RECOMP_GPU_SHOT_WINDOW=1 takes
 * the window's back buffer instead, after the present -- letterbox,
 * window scaling and RECOMP_SHARPEN included. */
static int gpu_shot_window(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GPU_SHOT_WINDOW");
        on = e && *e == '1';
    }
    return on;
}

static void gpu_shot_tex(ID3D11Texture2D *src, uint32_t W, uint32_t H)
{
    static const char *prefix;
    static int every = -1, n, frames;
    uint8_t hdr[54];
    D3D11_TEXTURE2D_DESC d;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BOX box;
    ID3D11Texture2D *st = NULL;
    char path[512];
    uint32_t y, x, v;
    int swap_rb;
    FILE *f;

    static const char *trigger;
    if (every < 0) {
        const char *e = getenv("RECOMP_GPU_SHOT_EVERY");
        prefix = getenv("RECOMP_GPU_SHOT");
        every = e && atoi(e) > 0 ? atoi(e) : 60;
        /* RECOMP_GPU_SHOT_TRIGGER=<file>: shoot only while it exists, so a
         * script can take exactly the seconds it wants */
        trigger = getenv("RECOMP_GPU_SHOT_TRIGGER");
    }
    if (!prefix || !*prefix || n >= 1000 || ++frames % every)
        return;
    if (trigger && *trigger) {
        FILE *t = fopen(trigger, "rb");
        if (!t)
            return;
        fclose(t);
    }
    ID3D11Texture2D_GetDesc(src, &d);       /* the back buffer may be RGBA */
    swap_rb = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
    d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
    d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.MiscFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &st)))
        return;
    box.left = 0; box.top = 0; box.front = 0;
    box.right = W; box.bottom = H; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)st, 0, 0, 0, 0,
                                              (ID3D11Resource *)src, 0, &box);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        snprintf(path, sizeof path, "%s%05d.bmp", prefix, n++);
        f = fopen(path, "wb");
        if (f) {
            memset(hdr, 0, sizeof hdr);
            hdr[0] = 'B'; hdr[1] = 'M';
            v = 54 + W * H * 4;  memcpy(hdr + 2, &v, 4);
            v = 54;              memcpy(hdr + 10, &v, 4);
            v = 40;              memcpy(hdr + 14, &v, 4);
            v = W;               memcpy(hdr + 18, &v, 4);
            v = (uint32_t)-(int32_t)H; memcpy(hdr + 22, &v, 4);   /* top-down */
            hdr[26] = 1; hdr[28] = 32;
            fwrite(hdr, 1, sizeof hdr, f);
            for (y = 0; y < H; y++) {
                uint8_t *row = (uint8_t *)m.pData + (size_t)y * m.RowPitch;
                if (swap_rb)
                    for (x = 0; x < W; x++) {
                        uint8_t t = row[x * 4];
                        row[x * 4] = row[x * 4 + 2];
                        row[x * 4 + 2] = t;
                    }
                fwrite(row, 4, W, f);
            }
            fclose(f);
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
}

static void gpu_shot(const GpuRt *r, uint32_t w, uint32_t h)
{
    if (!gpu_shot_window())
        gpu_shot_tex(r->tex, w * s_scale, h * s_scale);
}

int nv2a_gpu_present(uint32_t addr, uint32_t pitch, uint32_t bpp, uint32_t w, uint32_t h)
{
    static int on = -1;
    ID3D11Texture2D *back = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    HWND hwnd;
    RECT cr;
    uint32_t cw, ch;
    float sx, sy, k, vw, vh;
    GpuRt *r;

    if (on < 0) {
        const char *e = getenv("RECOMP_GPU_PRESENT");
        on = !(e && *e == '0');
    }
    if (!on || s_state != 1 || s_swap_failed || !addr || !w || !h
        || (bpp != 4 && bpp != 2))
        return 0;
    forget_drain();
    hwnd = (HWND)xbox_FramebufferWindowHwnd();
    if (!hwnd)
        return 0;                       /* window not open yet */
    /* The swap chain is the window's size, whatever that is now -- resized,
     * maximised, fullscreen -- and the frame is drawn into it at the
     * console's 4:3, centred, with black bars on the long side. */
    if (!GetClientRect(hwnd, &cr))
        return 0;
    cw = cr.right > 0 ? (uint32_t)cr.right : 1;
    ch = cr.bottom > 0 ? (uint32_t)cr.bottom : 1;
    if (!s_swap) {
        if (!swap_create(hwnd, cw, ch)) {
            s_swap_failed = 1;
            xbox_FramebufferWindowGpuUnavailable();
            return 0;
        }
        xbox_FramebufferWindowGpuOwned(1);
    }
    if (cw != s_swap_w || ch != s_swap_h) {
        if (FAILED(IDXGISwapChain_ResizeBuffers(s_swap, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0)))
            return 0;
        s_swap_w = cw;
        s_swap_h = ch;
    }
    /* The surface as the GPU holds it, or as guest memory has it if the CPU
     * produced this frame. */
    r = get_rt(addr, pitch, bpp, w, h);
    if (!r)
        return 0;
    rt_check(r);
    gpu_shot(r, w, h);
    if (!r->srv && FAILED(ID3D11Device_CreateShaderResourceView(
            s_dev, (ID3D11Resource *)r->tex, NULL, &r->srv)))
        return 0;
    if (FAILED(IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&back)))
        return 0;
    if (FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)back, NULL, &rtv))) {
        ID3D11Texture2D_Release(back);
        return 0;
    }
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, rtv, black);
    /* The surface may be larger than the frame (get_rt grows, never
     * shrinks): draw only its w x h corner, by drawing the whole surface
     * into a rectangle that much larger and letting the viewport crop. */
    sx = (float)cw / (float)w;
    sy = (float)ch / (float)h;
    k = sx < sy ? sx : sy;
    vw = (float)w * k;
    vh = (float)h * k;
    {
        float x0 = ((float)cw - vw) * 0.5f, y0 = ((float)ch - vh) * 0.5f;
        const LONG frame[4] = { (LONG)x0, (LONG)y0, (LONG)(x0 + vw + 0.5f),
                                (LONG)(y0 + vh + 0.5f) };
        if (s_sharp_ps && s_sharp_cb) {
            /* one source texel to each side; the CAS-style peak, -1/8 at
             * the gentlest to -1/5 at full strength */
            const float c[4] = { 1.0f / (float)(r->w * s_scale), 1.0f / (float)(r->h * s_scale),
                                 -1.0f / (8.0f - 3.0f * sharpen_amount()), 0.0f };
            cb_write(s_sharp_cb, c, sizeof c);
            blit_rect_ps(r->srv, rtv, x0, y0, (float)r->w * k, (float)r->h * k, frame,
                         s_sharp_ps, s_sharp_cb);
        } else
            blit_rect(r->srv, rtv, x0, y0, (float)r->w * k, (float)r->h * k, frame);
    }
    if (gpu_shot_window())
        gpu_shot_tex(back, cw, ch);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(back);
    IDXGISwapChain_Present(s_swap, 1, 0);
    return 1;
}

/* ---- draws --------------------------------------------------------------- */

static void unpack_argb(uint32_t c, float *o)
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(c & 0xFF) / 255.0f;
    o[3] = (float)(c >> 24) / 255.0f;
}

static void set_target(GpuRt *r, GpuDs *z, const uint32_t clip[4])
{
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)(r->w * s_scale); vp.Height = (float)(r->h * s_scale);
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    sc.left = (LONG)(clip[0] * s_scale); sc.top = (LONG)(clip[1] * s_scale);
    sc.right = (LONG)((clip[0] + clip[2]) * s_scale);
    sc.bottom = (LONG)((clip[1] + clip[3]) * s_scale);
    if (sc.right > (LONG)(r->w * s_scale)) sc.right = (LONG)(r->w * s_scale);
    if (sc.bottom > (LONG)(r->h * s_scale)) sc.bottom = (LONG)(r->h * s_scale);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &r->rtv, z ? z->dsv : NULL);
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
}

static int surface_targets(const Nv2aGpuState *st, GpuRt **rp, GpuDs **zp)
{
    uint32_t w, h;
    if (!st->color_addr || !st->pitch || (st->bpp != 4 && st->bpp != 2))
        return 0;
    w = st->pitch / st->bpp;
    h = st->clip[1] + st->clip[3];
    if (w > 4096) w = 4096;
    if (!w || !h || h > 4096)
        return 0;
    *rp = get_rt(st->color_addr, st->pitch, st->bpp, w, h);
    if (!*rp)
        return 0;
    *zp = st->zeta_addr ? get_ds(st->zeta_addr, (*rp)->w, (*rp)->h) : NULL;
    return 1;
}

/* Everything about a draw but its vertices: targets, textures, the
 * combiner shader and its constants, blend, depth and stencil. */
static GpuRt *pixel_pipeline(const Nv2aGpuState *st)
{
    GpuRt *r;
    GpuDs *z;
    PsKey key;
    ID3D11PixelShader *ps;
    ID3D11ShaderResourceView *srv[8];
    ID3D11SamplerState *smp[4];
    ID3D11BlendState *bs;
    ID3D11DepthStencilState *ds;
    float pcb[4 * (8 + 8 + 3 + 4 + 1)], bf[4], tex_w[4], tex_h[4];
    uint32_t i;
    int ca;

    forget_drain();
    if (!surface_targets(st, &r, &z))
        return NULL;

    /* Textures first: fetching one may read back a surface, and that must
     * happen before this draw's own target is marked as the GPU's. */
    memset(&key, 0, sizeof key);
    for (i = 0; i < 4; i++) {
        const Nv2aGpuTexture *t = &st->tex[i];
        int a1 = 0;
        ID3D11ShaderResourceView *view = t->valid ? rt_as_texture(t, r, &tex_w[i], &tex_h[i], &a1)
                                                  : NULL;
        int mip = 0;
        if (view) {
            key.alpha_one |= (uint8_t)(a1 << i);
        } else {
            tex_w[i] = (float)t->width;
            tex_h[i] = (float)t->height;
            view = get_tex(t);
            if (view && tex_wants_mips(t))
                mip = t->mipmap;
        }
        key.cube[i] = (uint8_t)(t->valid && t->cube && view);
        srv[i] = (view && !key.cube[i]) ? view : s_white2d;
        srv[4 + i] = key.cube[i] ? view : s_whitecube;
        smp[i] = get_sampler(t->filter_linear, t->wrap_u, t->wrap_v, mip);
    }
    rt_begin(r);

    key.use_rc = (uint8_t)st->use_rc;
    key.tex0 = (uint8_t)(st->tex[0].valid);
    if (st->use_rc) {
        key.control = st->rc.control;
        key.stage_program = st->rc.stage_program;
        key.final0 = st->rc.final0;
        key.final1 = st->rc.final1;
        key.clip_plane_mode = st->clip_plane_mode;
        memcpy(key.cicw, st->rc.color_icw, sizeof key.cicw);
        memcpy(key.aicw, st->rc.alpha_icw, sizeof key.aicw);
        memcpy(key.cocw, st->rc.color_ocw, sizeof key.cocw);
        memcpy(key.aocw, st->rc.alpha_ocw, sizeof key.aocw);
    }
    ps = get_ps(&key);
    if (!ps)
        return NULL;

    memset(pcb, 0, sizeof pcb);
    for (i = 0; i < 8; i++) {
        unpack_argb(st->rc.factor0[i], &pcb[4 * i]);
        unpack_argb(st->rc.factor1[i], &pcb[4 * (8 + i)]);
    }
    unpack_argb(st->rc.final_c0, &pcb[4 * 16]);
    unpack_argb(st->rc.final_c1, &pcb[4 * 17]);
    {   /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
        float *f = &pcb[4 * 18], t;
        unpack_argb(st->fog_color, f);
        t = f[0]; f[0] = f[2]; f[2] = t;
    }
    for (i = 0; i < 4; i++) {
        const Nv2aGpuTexture *t = &st->tex[i];
        float *ts = &pcb[4 * (19 + i)];
        ts[0] = (t->valid && t->linear && tex_w[i] > 0.0f) ? 1.0f / tex_w[i] : 1.0f;
        ts[1] = (t->valid && t->linear && tex_h[i] > 0.0f) ? 1.0f / tex_h[i] : 1.0f;
    }
    pcb[4 * 23 + 0] = (float)(st->alpha_ref & 0xFF);
    pcb[4 * 23 + 1] = (float)st->alpha_func;
    pcb[4 * 23 + 2] = st->alpha_test ? 1.0f : 0.0f;
    cb_write(s_ps_cb, pcb, sizeof pcb);

    bs = get_blend(st->blend_enable, st->blend_src, st->blend_dst, st->blend_eq,
                   write_mask(st->color_mask), &ca);
    unpack_argb(st->blend_color, bf);
    if (ca)
        bf[0] = bf[1] = bf[2] = bf[3];
    ds = get_dss(st, z != NULL);
    if (!bs || !ds)
        return NULL;

    set_target(r, z, st->clip);
    if (st->cull == 1 || st->cull == 2)
        ID3D11DeviceContext_RSSetState(s_ctx, s_rs_cull[st->cull - 1]);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, bs, bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, ds, st->stencil_ref & 0xFF);
    ID3D11DeviceContext_PSSetShader(s_ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_ps_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 8, srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smp);
    return r;
}

/* Append to a dynamic ring buffer; discard and restart when it fills. */
static int ring_put(ID3D11Buffer *b, uint32_t *pos, uint32_t cap, const void *data,
                    uint32_t bytes, uint32_t *offset)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (bytes > cap)
        return 0;
    if (*pos + bytes > cap)
        *pos = 0;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)b, 0,
                                       *pos ? D3D11_MAP_WRITE_NO_OVERWRITE
                                            : D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return 0;
    memcpy((uint8_t *)m.pData + *pos, data, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)b, 0);
    *offset = *pos;
    *pos += bytes;
    return 1;
}

/* ring_put for several arrays under one Map: they land back to back, each at
 * a 16-byte-aligned offset[i]. */
static int ring_put_n(ID3D11Buffer *b, uint32_t *pos, uint32_t cap, const void *const *data,
                      const uint32_t *bytes, uint32_t n, uint32_t *offset)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t i, total = 0;
    for (i = 0; i < n; i++)
        total += (bytes[i] + 15u) & ~15u;
    if (total > cap)
        return 0;
    *pos = (*pos + 15u) & ~15u;
    if (*pos + total > cap)
        *pos = 0;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)b, 0,
                                       *pos ? D3D11_MAP_WRITE_NO_OVERWRITE
                                            : D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return 0;
    for (i = 0; i < n; i++) {
        memcpy((uint8_t *)m.pData + *pos, data[i], bytes[i]);
        offset[i] = *pos;
        *pos += (bytes[i] + 15u) & ~15u;
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)b, 0);
    return 1;
}

void nv2a_gpu_draw(const Nv2aGpuState *st, const Nv2aGpuVertex *v, uint32_t n)
{
    GpuRt *r;
    float vcb[4];
    uint32_t offset, stride = sizeof(Nv2aGpuVertex);

    if (s_state != 1 || n < 3 || st->cull == 3)
        return;
    r = pixel_pipeline(st);
    if (!r)
        return;
    vcb[0] = 2.0f / (float)r->w;
    vcb[1] = 2.0f / (float)r->h;
    vcb[2] = st->zeta_z16 ? 1.0f / 65535.0f : 1.0f / 16777215.0f;
    vcb[3] = 0.0f;
    cb_write(s_vs_cb, vcb, sizeof vcb);
    if (!ring_put(s_vb, &s_vb_pos, GPU_VB_BYTES, v, n * stride, &offset))
        return;
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_vs_cb);
    ID3D11DeviceContext_Draw(s_ctx, n, 0);
}

int nv2a_gpu_draw_program(const Nv2aGpuState *st, const float *const attr[16],
                          uint32_t nverts, const uint32_t *idx, uint32_t nidx)
{
    ID3D11VertexShader *vs;
    ID3D11Buffer *cbs[2], *vbs[16];
    GpuRt *r;
    float pcb[8];
    const void *src[16];
    uint32_t bytes[16], off[16], strides[16], offs[16], used[16];
    uint32_t ioff, a, n = 0;

    if (s_state != 1)
        return 0;
    if (nidx < 3 || st->cull == 3)
        return 1;
    vs = get_program_vs();
    if (!vs)
        return 0;
    r = pixel_pipeline(st);
    if (!r)
        return 1;
    /* Constant memory, when it has changed since it was last sent. */
    if (nv2a_vsh_constants_gen() != s_const_gen_up) {
        cb_write(s_const_cb, nv2a_vsh_constants(), 16 * 192);
        s_const_gen_up = nv2a_vsh_constants_gen();
    }
    pcb[0] = 2.0f / (float)r->w;
    pcb[1] = 2.0f / (float)r->h;
    pcb[2] = st->zeta_z16 ? 1.0f / 65535.0f : 1.0f / 16777215.0f;
    pcb[3] = 0.0f;
    pcb[4] = st->fog_enable ? 1.0f : 0.0f;
    pcb[5] = (float)st->fog_mode;
    pcb[6] = st->fog_param[0];
    pcb[7] = st->fog_param[1];
    cb_write(s_prog_cb, pcb, sizeof pcb);
    for (a = 0; a < 16; a++)
        if (attr[a]) {
            src[n] = attr[a];
            bytes[n] = nverts * 16;
            used[n++] = a;
        }
    if (!ring_put_n(s_vb, &s_vb_pos, GPU_VB_BYTES, src, bytes, n, off)
        || !ring_put(s_ib, &s_ib_pos, GPU_IB_BYTES, idx, nidx * 4, &ioff))
        return 1;
    for (a = 0; a < 16; a++) {
        vbs[a] = s_zero_vb;
        strides[a] = 0;
        offs[a] = 0;
    }
    for (a = 0; a < n; a++) {
        vbs[used[a]] = s_vb;
        strides[used[a]] = 16;
        offs[used[a]] = off[a];
    }
    cbs[0] = s_const_cb;
    cbs[1] = s_prog_cb;
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_attr_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 16, vbs, strides, offs);
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT, ioff);
    ID3D11DeviceContext_VSSetShader(s_ctx, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 2, cbs);
    ID3D11DeviceContext_DrawIndexed(s_ctx, nidx, 0, 0);
    return 1;
}

void nv2a_gpu_clear(const Nv2aGpuState *st, uint32_t flags, uint32_t color,
                    uint32_t zstencil)
{
    GpuRt *r;
    GpuDs *z;
    ID3D11BlendState *bs;
    ID3D11DepthStencilState *dss = NULL;
    D3D11_DEPTH_STENCIL_DESC d;
    float cb[8], bf[4] = { 0, 0, 0, 0 };
    UINT8 mask = 0;
    int ca;

    if (s_state == 1)
        forget_drain();
    if (s_state != 1 || !surface_targets(st, &r, &z))
        return;
    rt_begin(r);
    if (flags & 0x10) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (flags & 0x20) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (flags & 0x40) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (flags & 0x80) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    bs = get_blend(0, 1, 0, 0x8006, mask, &ca);

    memset(&d, 0, sizeof d);
    d.DepthEnable = z && (flags & 1);
    d.DepthWriteMask = (z && (flags & 1)) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    d.StencilEnable = z && (flags & 2);
    d.StencilReadMask = 0xFF;
    d.StencilWriteMask = 0xFF;
    d.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    d.FrontFace.StencilFailOp = D3D11_STENCIL_OP_REPLACE;
    d.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_REPLACE;
    d.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    d.BackFace = d.FrontFace;
    {
        uint64_t key = 0xC1EA000000000000ull | (uint64_t)(flags & 3);
        dss = (ID3D11DepthStencilState *)cache_get(s_dss, 64, key);
        if (!dss && SUCCEEDED(ID3D11Device_CreateDepthStencilState(s_dev, &d, &dss)))
            cache_put(s_dss, 64, key, dss);
    }
    if (!bs || !dss)
        return;

    unpack_argb(color, cb);
    cb[4] = st->zeta_z16 ? (float)(zstencil & 0xFFFF) / 65535.0f
                         : (float)(zstencil >> 8) / 16777215.0f;
    cb[5] = cb[6] = cb[7] = 0.0f;
    cb_write(s_clear_cb, cb, sizeof cb);

    set_target(r, z, st->clip);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, bs, bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, dss, zstencil & 0xFF);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_clear_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_clear_cb);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_clear_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_clear_cb);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
}

#else  /* !_WIN32: no Direct3D; the software rasteriser stays in charge. */

int  nv2a_gpu_enabled(void) { return 0; }
void nv2a_gpu_draw(const Nv2aGpuState *st, const Nv2aGpuVertex *v, uint32_t n)
{ (void)st; (void)v; (void)n; }
int nv2a_gpu_draw_program(const Nv2aGpuState *st, const float *const attr[16],
                          uint32_t nverts, const uint32_t *idx, uint32_t nidx)
{ (void)st; (void)attr; (void)nverts; (void)idx; (void)nidx; return 0; }
void nv2a_gpu_clear(const Nv2aGpuState *st, uint32_t flags, uint32_t color,
                    uint32_t zstencil)
{ (void)st; (void)flags; (void)color; (void)zstencil; }
void nv2a_gpu_sync(uint32_t addr, uint32_t bytes) { (void)addr; (void)bytes; }
void nv2a_gpu_guest_written(uint32_t addr, uint32_t bytes) { (void)addr; (void)bytes; }
void nv2a_gpu_forget(uint32_t addr, uint32_t bytes) { (void)addr; (void)bytes; }
int nv2a_gpu_blit_source(uint32_t src, uint32_t pitch, uint32_t bpp, uint32_t w, uint32_t h)
{ (void)src; (void)pitch; (void)bpp; (void)w; (void)h; return -1; }
void nv2a_gpu_blit_copy(int src_rt, uint32_t src, uint32_t dst, uint32_t pitch,
                        uint32_t w, uint32_t h)
{ (void)src_rt; (void)src; (void)dst; (void)pitch; (void)w; (void)h; }
void nv2a_gpu_frame(void) {}
int  nv2a_gpu_present(uint32_t addr, uint32_t pitch, uint32_t bpp, uint32_t w, uint32_t h)
{ (void)addr; (void)pitch; (void)bpp; (void)w; (void)h; return 0; }

#endif
