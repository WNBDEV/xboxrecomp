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
 */
#include "nv2a_gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

static int s_state = -1;                /* -1 untried, 0 unavailable, 1 ready */
static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static ID3D11VertexShader *s_vs, *s_clear_vs;
static ID3D11PixelShader *s_clear_ps;
static ID3D11InputLayout *s_layout;
static ID3D11Buffer *s_vb, *s_vs_cb, *s_ps_cb, *s_clear_cb;
static ID3D11RasterizerState *s_rs;
static ID3D11ShaderResourceView *s_white2d, *s_whitecube;
static uint32_t s_vb_pos;
static uint32_t s_frame = 1;

#define GPU_VB_BYTES (16u << 20)

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

static ID3DBlob *compile(const char *src, const char *profile, const char *what)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), what, NULL, NULL, "main", profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
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
        if (k->tex0)
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

/* ---- render targets and depth buffers ---------------------------------- */

typedef struct {
    uint32_t addr, pitch, bpp, w, h;
    ID3D11Texture2D *tex, *staging;
    ID3D11RenderTargetView *rtv;
    int gpu_dirty;                      /* GPU holds what guest memory lacks */
    int synced;                         /* guest_hash describes guest memory */
    uint64_t guest_hash;
    uint32_t last_used;
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

static uint64_t rt_guest_hash(const GpuRt *r)
{
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t y;
    for (y = 0; y < r->h; y++)
        h = hash_bytes(h, guest(r->addr + y * r->pitch), (size_t)r->w * r->bpp);
    return h;
}

/* GPU -> guest. */
static void rt_readback(GpuRt *r)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t x, y;

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
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)r->tex, 0, NULL,
                                          buf, r->w * 4, 0);
    free(buf);
}

static void rt_release(GpuRt *r)
{
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
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &r->tex)))
        return NULL;
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
 * guest memory since it was last synced, start from that. */
static void rt_begin(GpuRt *r)
{
    if (!r->gpu_dirty) {
        uint64_t h = rt_guest_hash(r);
        if (!r->synced || h != r->guest_hash)
            rt_upload(r);
        r->guest_hash = h;
        r->synced = 1;
    }
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
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
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

typedef struct {
    uint32_t addr, bytes, palette, palette_bytes, width, height, pitch, format;
    int cube;
    uint64_t hash;
    uint32_t checked;                   /* frame of the last content check */
    uint32_t last_used;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} GpuTex;

#define GPU_TEXTURES 768
static GpuTex s_tex[GPU_TEXTURES];

static uint64_t tex_hash(const Nv2aGpuTexture *t)
{
    uint64_t h = hash_bytes(0xCBF29CE484222325ull, guest(t->addr), t->bytes);
    if (t->palette && t->palette_bytes)
        h = hash_bytes(h, guest(t->palette), t->palette_bytes);
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
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, init, &e->tex))
        || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex,
                                                        &v, &e->srv))) {
        if (e->tex) ID3D11Texture2D_Release(e->tex);
        e->tex = NULL;
        free(buf);
        return 0;
    }
    free(buf);
    return 1;
}

static ID3D11ShaderResourceView *get_tex(const Nv2aGpuTexture *t)
{
    GpuTex *e = NULL;
    int i, oldest = 0;

    if (!t->valid || !t->width || !t->height || t->width > 4096 || t->height > 4096)
        return NULL;
    /* Render to texture, through guest memory: bring back anything the GPU
     * drew over these bytes before they are read. */
    nv2a_gpu_sync(t->addr, t->bytes);

    for (i = 0; i < GPU_TEXTURES; i++) {
        GpuTex *c = &s_tex[i];
        if (c->srv && c->addr == t->addr && c->format == t->format
            && c->width == t->width && c->height == t->height && c->pitch == t->pitch
            && c->palette == t->palette && c->cube == t->cube && c->bytes == t->bytes) {
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

static ID3D11SamplerState *get_sampler(int linear, int wrap_u, int wrap_v)
{
    uint64_t key = (uint64_t)linear | ((uint64_t)wrap_u << 1) | ((uint64_t)wrap_v << 2);
    ID3D11SamplerState *s = (ID3D11SamplerState *)cache_get(s_smp, 32, key);
    D3D11_SAMPLER_DESC d;
    if (s)
        return s;
    memset(&d, 0, sizeof d);
    d.Filter = linear ? D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_POINT;
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
    s_vs_cb = make_cb(16);
    s_ps_cb = make_cb(sizeof(float) * 4 * (8 + 8 + 3 + 4 + 1));
    s_clear_cb = make_cb(32);
    if (!s_vs_cb || !s_ps_cb || !s_clear_cb)
        return 0;

    /* No culling (the software rasteriser draws both faces), scissor for the
     * clip rectangle, and depth clamped rather than clipped: it clipped only
     * at the near plane, at w just above 0. */
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    rd.ScissorEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs)))
        return 0;
    s_white2d = make_white(0);
    s_whitecube = make_white(1);
    gpu_log("ready: rasterisation and combiners on the GPU, vertex programs on the CPU");
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
    if (s_state < 0)
        s_state = gpu_init();
    return s_state == 1;
}

/* ---- sync points --------------------------------------------------------- */

void nv2a_gpu_sync(uint32_t addr, uint32_t bytes)
{
    int i;
    if (s_state != 1)
        return;
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

void nv2a_gpu_frame(void)
{
    s_frame++;
    if (s_frame == 0)
        s_frame = 1;
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
    vp.Width = (float)r->w; vp.Height = (float)r->h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    sc.left = (LONG)clip[0]; sc.top = (LONG)clip[1];
    sc.right = (LONG)(clip[0] + clip[2]); sc.bottom = (LONG)(clip[1] + clip[3]);
    if (sc.right > (LONG)r->w) sc.right = (LONG)r->w;
    if (sc.bottom > (LONG)r->h) sc.bottom = (LONG)r->h;
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

void nv2a_gpu_draw(const Nv2aGpuState *st, const Nv2aGpuVertex *v, uint32_t n)
{
    GpuRt *r;
    GpuDs *z;
    PsKey key;
    ID3D11PixelShader *ps;
    ID3D11ShaderResourceView *srv[8];
    ID3D11SamplerState *smp[4];
    ID3D11BlendState *bs;
    ID3D11DepthStencilState *ds;
    D3D11_MAPPED_SUBRESOURCE m;
    float pcb[4 * (8 + 8 + 3 + 4 + 1)], vcb[4], bf[4];
    uint32_t bytes = n * (uint32_t)sizeof(Nv2aGpuVertex), i, stride, offset;
    int ca;

    if (s_state != 1 || n < 3 || bytes > GPU_VB_BYTES)
        return;
    if (!surface_targets(st, &r, &z))
        return;

    /* Textures first: fetching one may read back a surface, and that must
     * happen before this draw's own target is marked as the GPU's. */
    memset(&key, 0, sizeof key);
    for (i = 0; i < 4; i++) {
        const Nv2aGpuTexture *t = &st->tex[i];
        ID3D11ShaderResourceView *view = get_tex(t);
        key.cube[i] = (uint8_t)(t->valid && t->cube && view);
        srv[i] = (view && !key.cube[i]) ? view : s_white2d;
        srv[4 + i] = key.cube[i] ? view : s_whitecube;
        smp[i] = get_sampler(t->filter_linear, t->wrap_u, t->wrap_v);
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
        return;

    /* Constants. */
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
        ts[0] = (t->valid && t->linear && t->width) ? 1.0f / (float)t->width : 1.0f;
        ts[1] = (t->valid && t->linear && t->height) ? 1.0f / (float)t->height : 1.0f;
    }
    pcb[4 * 23 + 0] = (float)(st->alpha_ref & 0xFF);
    pcb[4 * 23 + 1] = (float)st->alpha_func;
    pcb[4 * 23 + 2] = st->alpha_test ? 1.0f : 0.0f;
    cb_write(s_ps_cb, pcb, sizeof pcb);
    vcb[0] = 2.0f / (float)r->w;
    vcb[1] = 2.0f / (float)r->h;
    vcb[2] = st->zeta_z16 ? 1.0f / 65535.0f : 1.0f / 16777215.0f;
    vcb[3] = 0.0f;
    cb_write(s_vs_cb, vcb, sizeof vcb);

    /* Vertices: appended to a ring, discarded and restarted when it fills. */
    if (s_vb_pos + bytes > GPU_VB_BYTES)
        s_vb_pos = 0;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0,
                                       s_vb_pos ? D3D11_MAP_WRITE_NO_OVERWRITE
                                                : D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    memcpy((uint8_t *)m.pData + s_vb_pos, v, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
    offset = s_vb_pos;
    s_vb_pos += bytes;

    /* State. */
    bs = get_blend(st->blend_enable, st->blend_src, st->blend_dst, st->blend_eq,
                   write_mask(st->color_mask), &ca);
    unpack_argb(st->blend_color, bf);
    if (ca)
        bf[0] = bf[1] = bf[2] = bf[3];
    ds = get_dss(st, z != NULL);
    if (!bs || !ds)
        return;

    set_target(r, z, st->clip);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, bs, bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, ds, st->stencil_ref & 0xFF);
    stride = sizeof(Nv2aGpuVertex);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_vs_cb);
    ID3D11DeviceContext_PSSetShader(s_ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_ps_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 8, srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smp);
    ID3D11DeviceContext_Draw(s_ctx, n, 0);
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
void nv2a_gpu_clear(const Nv2aGpuState *st, uint32_t flags, uint32_t color,
                    uint32_t zstencil)
{ (void)st; (void)flags; (void)color; (void)zstencil; }
void nv2a_gpu_sync(uint32_t addr, uint32_t bytes) { (void)addr; (void)bytes; }
void nv2a_gpu_guest_written(uint32_t addr, uint32_t bytes) { (void)addr; (void)bytes; }
void nv2a_gpu_frame(void) {}

#endif
