# xboxrecomp — gpu-backend fork

A fork of [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp),
the static recompilation toolkit for original Xbox games: it translates a
game's x86 code to C and reimplements the console around it (kernel, NV2A
graphics, MCPX audio, USB pads), so the game runs as an ordinary PC program
rather than under a CPU emulator.

This fork branches from upstream **v0.13.1** (7 Oct 2026) and carries the work
done bringing up *Alter Echo* in
[alter-echo-recomp](https://github.com/FILL-IN/alter-echo-recomp): a Direct3D
11 GPU backend, correctness fixes across the translator, graphics, audio and
kernel layers, and an experimental native Linux build. The branch is
`gpu-backend`.

**Upstream's documentation still applies** -- the pipeline, the runtime, the
porting guide. Its README is kept as [UPSTREAM-README.md](UPSTREAM-README.md),
and `docs/` is upstream's. Start with
[docs/GETTING_STARTED.md](docs/GETTING_STARTED.md).

## Status

| Title | State on this branch |
|---|---|
| Alter Echo (NTSC) | The first missions played start to finish: 60 fps at 2x resolution under Wine + DXVK, with sound, pad and rumble. Native Linux boots to the title. |
| Anything else | Not tested here. The changes are title-agnostic and the translator suite passes, but only Alter Echo has exercised them. |

The Windows build is cross-compiled with MinGW-w64 and has been run under Wine
only; real Windows is untested.

## What this fork adds

**Graphics (NV2A)**
- A Direct3D 11 backend (`RECOMP_GPU=1`): rasterisation, register combiners
  and vertex programs (translated to HLSL per program) on the host GPU. The
  software rasteriser stays as the reference (`RECOMP_GPU=0`).
- Frames presented straight to the window with vsync, compiled shaders cached
  on disk, rendering at N x the console's resolution, borderless fullscreen
  letterboxed at 4:3.
- Stencil test and ops, face culling, the 2D blit path (NV062 surfaces, NV09F
  image blits), GPU semaphores and software methods, pushbuffer execution in
  ring order, `ARRAY_ELEMENT32`, batches of up to 65536 indices, a moving CRTC
  scanline, vblanks kept pending until the title acknowledges them.

**Translator and lifter**
- Fixes found mostly by the conformance fuzzer, each with tests: one-operand
  multiply/divide widths and flags, signed conditions after add/sub, `movsx`
  of bp/sp, `neg` and rotate flags, `cwd`, register `xadd`; `cpuid`,
  `pushfd`/`popfd` lifted instead of skipped.
- A compiled switch whose table padding was taken for a function no longer
  loses the arm that branches back to its shared epilogue.
- The conformance harness runs on Linux (`gcc -m32`).
- Unresolved call stubs report their first call instead of returning silently.

**Audio (MCPX APU)**
- Voices resampled at their own rate; the DSP mixdown actually played, folded
  to stereo.
- Physical addresses resolved by what the title's `MmGetPhysicalAddress` said
  (RAM or contiguous window) instead of guessed from the allocator.

**Kernel, memory, USB**
- The utility drive's cluster size from the title; IRQL raises serialised
  (`RECOMP_IRQL_LOCK`); PGRAPH interrupt write-one-to-clear; the PFB flush
  completed on the write.
- Freed video buffers and contiguous blocks reusable (`RECOMP_HEAP_VFREE`,
  `RECOMP_CONTIG_FREE`); the stack region sized at run time.
- Pad rumble passed to the host pad; code loaded at run time can be
  registered (`xbox_AddCodeRange`) -- what runtime-loaded game modules need.

**Native Linux (experimental)**
- Vectored exception handling on signals, the guest's address window held as
  one reservation, SDL2 for window, sound and pads, DXVK Native for Direct3D
  11, vkd3d-utils for shader compilation, guest functions named like libc ones
  renamed.

## Building

The same way as upstream: a game project adds this tree as a subdirectory
(see the Quick Start in [UPSTREAM-README.md](UPSTREAM-README.md)).
[alter-echo-recomp](https://github.com/FILL-IN/alter-echo-recomp) is a
complete worked example.

- **Windows executable, built on Linux:** CMake with
  `-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake`.
- **Native Linux:** a normal CMake build with SDL2 installed;
  `XBOX_DXVK_NATIVE_INCLUDE` points at DXVK Native's headers for the GPU
  backend (otherwise it falls back to the software rasteriser). DXVK Native and
  vkd3d-utils are loaded at run time.

## Runtime switches added here

Environment variables, read at start-up. Defaults in brackets.

| Area | Switch | Effect |
|---|---|---|
| Graphics | `RECOMP_GPU` [0] | 1: draw with the host GPU (Direct3D 11) |
| | `RECOMP_GPU_VS` [1] | 0: transform vertices on the CPU instead |
| | `RECOMP_GPU_PRESENT` [1] | 0: the old window path, flips capped at `RECOMP_FLIP_HZ` [60] |
| | `RECOMP_SCALE` [1] | render at N x the console's resolution (1-4) |
| | `RECOMP_RT_DIRECT` [0] | 1: sample render targets on the GPU, no readback |
| | `RECOMP_BLIT_GPU` [1] | 0: 2D image blits of GPU-drawn surfaces only in guest memory (console size) |
| | `RECOMP_TEXFILTER` [1] | 0: top mip level only (the old behaviour); 1: mip chains where the title minifies between levels; 2-16: plus anisotropic filtering |
| | `RECOMP_SHARPEN` [0] | 0-1: contrast-adaptive sharpening of the frame as it reaches the window |
| | `RECOMP_SHADER_CACHE` [shader-cache] | the cache directory; 0 turns it off |
| | `RECOMP_FULLSCREEN` [0] | 1: start fullscreen (Alt+Enter / F11 toggle) |
| | `RECOMP_FPS30` [0] | 1: start held at 30 fps (F2 toggles 30/60) |
| | `RECOMP_CAMERA_FLIP_X` [0] | 1: right stick left/right swapped (pad, mouse and keys), for a camera that turns against the stick |
| | `RECOMP_CULL` [1] | 0 disables face culling, 2 inverts the winding |
| | `RECOMP_RASTER_BATCH` [1] | 0: software rasteriser, one triangle at a time |
| Memory | `RECOMP_HEAP_VFREE` [0] | 1: memory the title frees goes back to the heap |
| | `RECOMP_CONTIG_FREE` [0] | 1: freed contiguous blocks are reused |
| | `RECOMP_STACK_KB` [8192] | size of the main thread's stack region; it comes out of the heap |
| Input | `RECOMP_IRQL_LOCK` [0] | 1: DPCs and the title never run at DISPATCH_LEVEL together |
| | `RECOMP_RUMBLE` [1] | 0: ignore the title's rumble |
| Audio | `RECOMP_AUDIO_MUTE` [0] | 1: everything as usual, but silence to the device |
| | `RECOMP_AUDIO_DUMP` | file: append the output as raw 48 kHz stereo s16le |
| Diagnostics | `RECOMP_WAIT_PROFILE`, `RECOMP_GPU_PROFILE` | where threads block; where GPU time goes |
| | `RECOMP_AUDIO_STATS`, `RECOMP_APU_VOICES`, `RECOMP_APU_PHYS_STATS` | levels, voices on/off, how APU reads resolve |
| | `RECOMP_HEAP_STATS`, `RECOMP_CONTIG_TRACE` | heap use; contiguous allocations and frees |
| | `RECOMP_FRAME_TRACE`, `RECOMP_TRI_PROBE=x,y`, `RECOMP_DEBUG_HOLES`, `RECOMP_TEX_DIAG`, `RECOMP_VSH_NAN` | one frame's methods; the triangles covering a pixel; undrawn pixels in magenta; bad texels; NaN vertex outputs |
| | `RECOMP_GPU_SHOT=<prefix>`, `RECOMP_GPU_SHOT_EVERY` [60] | the presented frame at full render resolution as `<prefix>NNNNN.bmp`, from the GPU (works with the window hidden); `RECOMP_GPU_SHOT_WINDOW=1` takes the window's back buffer instead (sharpening and letterbox included) |
| | `RECOMP_RT_DIRECT_WHY` | why a texture over a GPU-drawn surface is or is not sampled from it directly |
| | `RECOMP_BUSY_ACK`, `RECOMP_PFB_TRAP` [1] | 0 to turn off the busy-bit acknowledger, the PFB flush trap |

## Tests

- `python -m pytest tools/recomp` -- the translator and lifter (about 500).
- `tools/conformance` -- the lifter checked against real x86 (`--help`).
- `tests/` -- the device models and kernel layer, in C.

## Upstream and credit

The toolkit is sp00nz's and upstream's contributors' work; this fork adds to
it. Fixes here that belong to shared code may be offered upstream piece by
piece (the lifter fixes, with their tests, first).

## License

As upstream: **MIT** ([LICENSE](LICENSE)), with the xemu-derived APU and NV2A
files under LGPL-2.1-or-later -- see the License section of
[UPSTREAM-README.md](UPSTREAM-README.md), [NOTICE](NOTICE) and
[LICENSES/](LICENSES/). Changes made here to those files stay under their
licence; everything else this fork adds is MIT.
