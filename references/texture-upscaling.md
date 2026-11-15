# Texture Upscaling - Real-ESRGAN default (R21)

Default upscaler for texture improvement/upscaling tasks: **Real-ESRGAN**
(https://github.com/xinntao/real-esrgan). This default was set by the user
(2026-11-14): when the user asks for texture upscaling or texture improvement and
names no tool, use Real-ESRGAN WITHOUT asking which tool - but ALWAYS run the fit
check below first. If the goal does not fit, say why and propose the alternative
before proceeding (R15).

## Fit check (run before any upscaling)

| Fit | Situation | Action |
| --- | --- | --- |
| YES | photographic/organic surfaces (roads, terrain, walls, car paint, skies) gain detail | proceed with Real-ESRGAN |
| YES | user wants "sharper/nicer/HD" on world or vehicle textures | proceed |
| NO | GUI sprites, fonts, tiny palettized icons - upscaling invents detail and breaks the look | decline; ask the user (hand-touchup or nearest-neighbour instead) |
| NO | the target slot has a hard byte budget that cannot hold the upscaled size (3D chunk budgets: capacity = bytes) | decline at that size; propose the largest size the budget allows |
| NO | texture is a mask/palette/lookup data, not a picture | decline |

When in doubt about style loss (a 1999 low-res look is the point), upscale ONE
sample texture, show the user the result, and let them decide - do not batch-process
an entire container on a guess.

## Setup (once per machine; download = consent-gated R15)

Preferred: the **ncnn-vulkan standalone binary** - no Python dependency, runs on any
GPU or CPU fallback:

1. Download the Windows zip from https://github.com/xinntao/Real-ESRGAN/releases
   (asset `realesrgan-ncnn-vulkan-<ver>-windows.zip`).
2. Extract into `<skill>/Toolbox/realesrgan/` so the exe is
   `<skill>/Toolbox/realesrgan/realesrgan-ncnn-vulkan.exe`.
3. Verify: run it with no args - it must print its usage (real functional check, R6).
   Models ship in the same folder (`models/realesrgan-x4plus.param` etc.).

The Python/pip route (`pip install realesrgan`) is the fallback when Vulkan is
unavailable - it needs Python + torch and is slower to set up; use only if the
standalone binary fails.

## Pipeline (order matters - engine rules first)

1. **Decode per the real engine rules first** (fundamentals A2 + `Examples/Texture,3D,Spline Edit`): Sega PVR/VQ codebook data with PowerVR2 Morton twiddle - NOT plain linear bitmaps. Extract the texture to PNG with the existing tooling (LyrasTextureMod source / AssetExtractor pattern).
2. **Upscale with Real-ESRGAN** (see command below). Keep the alpha channel: the ncnn binary preserves RGBA for PNG in/out - verify on ONE sample before batching (RGB565 game textures have no alpha; GUI sprites may).
3. **Rescale to the allowed target size**: hard power-of-two + aspect rules and the container/chunk byte budget (`REPLACE_3D_AND_SPLINES_GUIDE.md`, texture guide). Typical flow: x4 upscale -> Lanczos downscale to the largest size the budget allows. Never ship a non-power-of-two result.
4. **Re-encode** into the engine format (VQ codebook + twiddle, or the loose-file DDS path) and inject per the texture guide.
5. **Verify in the real game** (Deploy & verify): one texture end-to-end before batching; check for VQ decode artifacts, mip seams, and VRAM pressure.

```text
# general photographic texture, 4x
realesrgan-ncnn-vulkan.exe -i in.png -o out.png -n realesrgan-x4plus -s 4

# anime-style artwork (character portraits)
realesrgan-ncnn-vulkan.exe -i in.png -o out.png -n realesr-animevideov3 -s 4
```

## Model selection (default first)

| Model | Use for | Notes |
| --- | --- | --- |
| `realesrgan-x4plus` | **default** - real-world photos (SF/NY/London environments, vehicles) | best general quality, slowest |
| `realesr-animevideov3` | stylized/anime-like art, character portraits | much faster, lighter |
| `realesrgan-x4plus-anime` | anime-style stills | middle ground |

Scale: prefer `-s 4` then downscale to budget; use `-s 2` when 4x would overshoot
the target size anyway.

## Logging and debugging

Batch jobs are Python glue (Step 4 rule): log every file with its source slot,
target size, model and result; on failure log the reason and CONTINUE with the rest.
A failed batch must not leave the game directory in a half-injected state - inject
only after all textures processed and verified (R17: one folder per mod, no temp
leftovers; keep backups of every replaced original for rollback, R15).
