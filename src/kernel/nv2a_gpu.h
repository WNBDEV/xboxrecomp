/**
 * NV2A on the host GPU (RECOMP_GPU=1): rasterisation and pixel shading on
 * Direct3D 11, everything else as before.
 *
 * The executor (nv2a_pb_exec.c) still parses the pushbuffer and keeps the
 * GPU state. What it hands over is a batch -- the untransformed vertex
 * attributes and an index list, run through the title's vertex program
 * translated to HLSL (nv2a_gpu_draw_program), or, when a program has to stay
 * on the CPU interpreter, a triangle list it transformed itself
 * (nv2a_gpu_draw) -- and the state to draw it with: render target and depth
 * buffer, the four texture stages, the register combiner program, blend,
 * depth, stencil and alpha test.
 *
 * Guest memory stays the truth everything else reads. A surface the GPU has
 * drawn into is copied back (nv2a_gpu_sync) before anything reads those
 * bytes -- a flip, a 2D blit, a texture fetched from them -- and guest memory
 * is uploaded again before the GPU next draws into a surface something else
 * changed. So the framebuffer window, the NV09F blits behind post-effects and
 * video all work unchanged; the cost is a copy per surface per sync point.
 */
#ifndef XBOXRECOMP_NV2A_GPU_H
#define XBOXRECOMP_NV2A_GPU_H

#include <stdint.h>
#include "nv2a_combiner.h"

/* One vertex as the title's program left it: x, y in pixels and z in depth
 * buffer units (the XDK epilogue applies the viewport and divides by w), w
 * as the clip-space w (1 when there is none). Colours saturated to [0, 1],
 * fog.x the fog factor, four texture coordinates. */
typedef struct {
    float pos[4];
    float d0[4], d1[4];
    float fog[4];
    float tex[4][4];
} Nv2aGpuVertex;

/* One texture stage. `tex` is the executor's own description, handed back to
 * nv2a_gpu_decode_texture; the rest identifies it for the cache. */
typedef struct {
    int valid;
    uint32_t addr, bytes;               /* guest VA of the texels, all faces */
    uint32_t palette, palette_bytes;
    uint32_t width, height, pitch, format;
    int cube;
    int linear;                         /* coordinates in texels, not 0..1 */
    int filter_linear;
    int wrap_u, wrap_v;
    const void *tex;
} Nv2aGpuTexture;

typedef struct {
    /* Where it draws. Addresses are guest VAs (dma_resolve). */
    uint32_t color_addr, pitch, bpp, width, height;
    uint32_t zeta_addr;
    int zeta_z16;                       /* Z16 rather than Z24S8 */
    uint32_t clip[4];                   /* x, y, w, h */
    /* How. */
    int use_rc;                         /* combiners programmed at all */
    Nv2aCombiner rc;
    uint32_t clip_plane_mode;
    Nv2aGpuTexture tex[4];
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t color_mask;                /* A<<24 R<<16 G<<8 B, a byte each */
    uint32_t depth_test, depth_func, depth_mask;
    int cull;                           /* 0 none, 1 CW, 2 CCW (on screen), 3 all */
    uint32_t stencil_test, stencil_func, stencil_ref, stencil_fmask,
             stencil_wmask, stencil_op_fail, stencil_op_zfail, stencil_op_zpass;
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t fog_color;                 /* R in bits 0-7, as NV097 has it */
    uint32_t fog_enable, fog_mode;      /* for the per-vertex fog factor */
    float fog_param[2];
} Nv2aGpuState;

/* RECOMP_GPU=1 and a device could be made. */
int  nv2a_gpu_enabled(void);
/* A triangle list of vertices the CPU transformed. */
void nv2a_gpu_draw(const Nv2aGpuState *st, const Nv2aGpuVertex *v, uint32_t n);
/* An indexed triangle list of untransformed vertices: v0..v15 per vertex, run
 * through the current vertex program (nv2a_vsh_interp's) on the GPU. Returns
 * 0 if that program cannot run there, and the caller transforms on the CPU. */
int  nv2a_gpu_draw_program(const Nv2aGpuState *st, const float (*attr)[16][4],
                           uint32_t nverts, const uint32_t *idx, uint32_t nidx);
/* NV097 CLEAR_SURFACE: flags bit 0 Z, bit 1 stencil, 0xF0 colour (R G B A). */
void nv2a_gpu_clear(const Nv2aGpuState *st, uint32_t flags, uint32_t color,
                    uint32_t zstencil);
/* Make guest memory in [addr, addr + bytes) hold what the GPU drew there. */
void nv2a_gpu_sync(uint32_t addr, uint32_t bytes);
/* Something other than the GPU wrote [addr, addr + bytes): textures cached
 * from those bytes are checked again before their next use. */
void nv2a_gpu_guest_written(uint32_t addr, uint32_t bytes);
/* An NV09F image blit of a w x h rectangle at src. Before the executor's
 * guest copy: the render target holding src at full scale, or -1 if the GPU
 * does not hold it (or it would gain nothing: RECOMP_SCALE 1, no
 * RECOMP_RT_DIRECT). After it: copy those pixels to dst on the GPU too. */
int  nv2a_gpu_blit_source(uint32_t src, uint32_t pitch, uint32_t bpp, uint32_t w, uint32_t h);
void nv2a_gpu_blit_copy(int src_rt, uint32_t src, uint32_t dst, uint32_t pitch,
                        uint32_t w, uint32_t h);
/* A frame was presented. */
void nv2a_gpu_frame(void);
/* Show the w x h surface at addr in the framebuffer window, straight from the
 * GPU, with vsync. Returns 0 if it cannot (no window yet, no swap chain,
 * RECOMP_GPU_PRESENT=0); the caller then shows it through guest memory. */
int  nv2a_gpu_present(uint32_t addr, uint32_t pitch, uint32_t bpp, uint32_t w, uint32_t h);

/* Provided by the executor: decode face `face` of `tex` into width*height
 * A8R8G8B8 texels, unreadable texels white, as the software sampler has it. */
void nv2a_gpu_decode_texture(const void *tex, uint32_t face, uint32_t *out);

#endif
