# CameraTools Photoreal

`CameraToolsPhotoreal.addon64` is a ReShade add-on that works inside Genshin Impact's own D3D11 frame. Each frame it finds the game's G-buffer and lighting buffers by format, size and order, never by shader hash. It can show any of them full screen as a debug view. Once the game has written its G-buffer and before anything reads it, it can make surfaces wet. Before the game's combine pass reads them, it can draw the sun's shadows again with soft edges from the game's own shadow atlas, add contact shadows to them, and replace the world's ambient light. Before the game's bloom reads the HDR scene, it can add aerial perspective and sun shafts through a ground mist, accumulate the HDR scene of paused frames into an average, and it can draw its own tone map in place of the game's. It can also render one frame with several settings variants and save each as a PNG, so effects can be judged from images, capture the scene's geometry around the camera from six directions, and trace ambient occlusion and sun shadows against that geometry in a compute shader. A MelonLoader mod drives it through C exports.

## Layout

| Path | Contents |
|---|---|
| `photoreal.h` | The C interface: exports, settings, camera, status, bit constants. |
| `core/recipe.*` | The game's frame as a table of steps (`kRecipe`) and the `FrameTracker` that matches events against it. Pure C++17. |
| `core/plan.*` | Settings parsing, our passes (`kPasses`) and views (`kViews`), the per-step plan, the frame report and its status line. Pure C++17. |
| `core/sun.*` | The game's sun shadow constants: the four constant buffers of its shadow-mask draw, parsed into cascades, atlas grid and camera, and checked. Pure C++17. |
| `core/accumulate.*` | The accumulator's schedule (`accumulate_tick`), the cat's-eye weight, and the depth read's pixel and meters. Pure C++17. |
| `core/compare.*` | The comparison capture's variants, its schedule of presents (`compare_tick`), back buffer pixels, flicker and TSV rows. Pure C++17. |
| `core/png.*` | A minimal PNG encoder: 8-bit RGB with stored deflate blocks, so the add-on needs no compression library. Pure C++17. |
| `core/dxbc.*` | A shader's output signature from its DXBC container, and the stream-output declaration that captures its position. Pure C++17. |
| `core/scene.*` | The [scene capture](#scene-capture)'s request, faces, the game's and the pushed camera as clip-to-world transforms, the draw records, the depth check, the verdict on a captured frame, and its schedule of presents (`scene_tick`). Pure C++17. |
| `core/scene_files.*` | The scene capture's files: each face's OBJ or binary PLY, picture, depth and raw stream, `scene.mtl`, and the rows of `scene.tsv`, `draws.tsv` and `camera.tsv`. Pure C++17. |
| `core/shaders.*` | The game's vertex and domain shader bytecode, kept from each shader's creation and looked up by the bound shader, with FrameCensus's hash. Pure C++17. |
| `core/trace_scene.*` | The [path tracer](#path-tracer)'s scene: a capture's triangles in one frame, deduplicated on a 1 mm grid, the self-test scene, and `trace-scene.bin`. Pure C++17. |
| `core/bvh.*` | The traced scene's binned-SAH BVH2 in the GPU's layout, and rays through it. Pure C++17. |
| `core/trace.*` | The trace pass's constants, tiles, schedule, time budget, reset rules, the game's sun and floating origin, staleness, and the kernel's pixel. Pure C++17. |
| `core/kernel.cpp`, `core/hlsl.h` | `shaders/bvh.hlsli` and `shaders/trace.hlsli` compiled as C++ through a small HLSL shim, so the tests and the Mac's reference tracer run the compute shader's own rays and traversal. |
| `core/trace_files.*` | The files `PhotorealSaveTrace` writes, and their reading back. Pure C++17. |
| `addon.cpp` | The ReShade shell: events, exports, mailboxes between the C# thread and the render thread, the shader registry's events, and the captures' file writer thread. |
| `gpu.*` | The D3D11 side: `StateGuard`, `Mirror`, `Scratch`, `Staging`, the sun constants' copies and readback, the wetness, sun-shadow, contact-shadow, ambient, atmosphere, light-shaft and tonemap passes, snapshots and the debug composite. |
| `scene_gpu.*` | The scene capture's D3D11 side: `ReplayGuard`, the stream-output geometry shaders made from the game's own bytecode, the replay of each G-buffer draw, its buffers and queries, and the copies and readback of a captured frame. |
| `trace_gpu.*` | The trace pass's D3D11 side: the scene's buffers and their upload, the history, the kernel's dispatch, timestamp queries and counters, and the copies of a saved frame. |
| `shaders/` | HLSL, compiled to DXBC by `build.py` and embedded in the add-on. `sun.hlsli` holds the sun atlas projection the sun-shadow and light-shaft passes share, and `scatter.hlsli` the scattering arithmetic of the atmosphere pass and the light shafts' mist. `cat_eye.hlsli` also compiles as C++ in `core/accumulate.cpp`, `wet.hlsli` and `scatter.hlsli` in `core/plan.cpp`, and `bvh.hlsli` and `trace.hlsli` in `core/kernel.cpp`, so the tests check the shaders' own arithmetic. `trace_cs.hlsl` is the one compute shader, built to `cs_5_0`. |
| `csharp/Photoreal.cs` | The C# binding a MelonLoader mod adds as is. |
| `tests/` | Native tests that replay recorded FrameCensus frames through `core/`, and the Mac's tools for scene captures and traced frames: `check_scene.py`, `check_bvh.sh`, `check_trace.sh` with `trace_reference.cpp` and `check_trace.py`, and `make_trace_fixture.py`. |

## Build

Run the tests with any C++17 compiler. They need no game, ReShade or D3D11.

```sh
tests/run.sh
```

Build the add-on with LLVM's `clang-cl` and `lld-link`, a Windows SDK made by `xwin splat`, and the pinned `vkd3d-compiler`:

```sh
python3 build.py --sdk <xwin folder> --out-dir <folder>
```

- `--vkd3d-compiler` names the shader compiler. Without it, the script reads `PHOTOREAL_VKD3D_COMPILER`, and then looks for `.work/build/reshade/vkd3d/vkd3d-compiler` in the folders above this one, which is where the melonloader workspace builds it.
- `--reshade-include` uses a ReShade include folder instead of downloading the headers of ReShade 6.8.0.
- `--clang-cl` and `--lld-link` name the tools when they are not on `PATH` or in Homebrew's folders.
- `--work` holds the DXBC, the generated `photoreal_shaders.h` and the object files. The default is `obj/`.

The build compiles with `/W4 /WX`. The add-on imports only `KERNEL32.dll`, because it uses the game's D3D11 device through ReShade.

## Exports

All exports use the C calling convention. Every one may be called from any thread and returns at once. The add-on takes settings and the camera at the next frame boundary on ReShade's render thread, so one frame always sees one settings value.

| Export | Does |
|---|---|
| `uint32_t PhotorealVersion(void)` | Returns `PHOTOREAL_VERSION`, 8. Version 2 added the sun direction to `PhotorealCamera`, so a binding of version 1 must not call `PhotorealSetCamera`. Version 3 added `PhotorealCompare` and `PhotorealStatus.compare_remaining`. Version 4 added the sun and sky colors to `PhotorealCamera`. Version 5 added its lens sample, `PhotorealSetAccumulate`, `PhotorealDepthAt` and the status's three accumulator fields. Version 6 removed light through leaves, its two settings blocks and its pass bit, which moved the wetness block from byte 128 to 116 and `PHOTOREAL_PASS_WETNESS` from bit 6 to bit 5, and appended `PhotorealLightShafts` and `PHOTOREAL_PASS_LIGHT_SHAFTS`, bit 6, so a binding of an earlier version must not call `PhotorealApply` or `PhotorealCompare`, or read `passes_ran` and `passes_skipped`. `PhotorealCamera` has no size field, so a binding of an earlier version must not call `PhotorealSetCamera`. Version 7 added `PhotorealCaptureScene`, `PHOTOREAL_FACE_*`, `PHOTOREAL_SCENE_*` and the status's three scene fields, which took the status's 4 bytes of padding and 8 more, so a binding of version 6 may call everything it called before. Version 8 added `PhotorealTrace` at the end of the settings, `PHOTOREAL_PASS_TRACE` (bit 7), `PHOTOREAL_VIEW_TRACE`, the scene flags `PHOTOREAL_SCENE_TRACE`, `_NO_MESHES`, `_GBUFFER` and `_RELEASE`, `PhotorealSaveTrace` and the status's five trace fields, so a binding of version 7 may call everything it called before except `PhotorealCompare`, whose variants hold the longer settings. |
| `uint64_t PhotorealApply(const PhotorealSettings *)` | Copies the whole desired state and returns its generation. `size` says which fields the caller knows, and later fields keep their defaults. |
| `void PhotorealSetCamera(const PhotorealCamera *)` | Unity's `worldToCameraMatrix` and `GL.GetGPUProjectionMatrix(projectionMatrix, false)`, in Unity's column-major memory order, then the world direction toward the sun, the sun light's color in linear RGB times its intensity, and the sky's ambient light in linear RGB, each as four floats. The direction may have any length; zero means no sun. A color component that is negative or not finite reads as 0. The fourth float of the sun direction and the sun color is unused. The fourth float of the sky color is the weather's wetness, from 0 dry to 1 soaked, which the [wetness pass](#wet-surfaces) follows while `wetness.wetness` is 0; it clamps to 0 to 1, and a value that is not finite reads as 0. CameraTools sends 1 while its World tab's weather is rain or a thunderstorm, or, with the game's weather, while the game's sky says it is wet, and else 0, eased over 5 seconds of real time. Then the [lens sample](#frame-accumulator): the sample's x and y, its index, and 1 on the frame to add, else 0; all zero when nothing is accumulated, which is what CameraTools pushes. Call it every frame from the main camera's `onPreCull`, after anything that moves the camera, so the add-on gets the camera that renders. Only one caller should push the camera. |
| `void PhotorealSetAccumulate(const PhotorealAccumulate *)` | The accumulator's mode, generation and cat's eye; see [Frame accumulator](#frame-accumulator). Separate from the settings, so a screenshot can accumulate whatever passes another caller set, and it works while `enabled` is 0. |
| `float PhotorealDepthAt(float u, float v)` | The view-space depth in meters at a screen point, from 0 at the top left to 1, read from a frame after the point was first asked for, or -1 until then. Call it once a frame until it is not negative. |
| `void PhotorealGetStatus(PhotorealStatus *)` | Copies the last frame's status. Set `size` first. |
| `uint32_t PhotorealDescribe(char *, uint32_t)` | Writes the last frame's status line and returns its length, or 0 when the buffer is too small. |
| `uint32_t PhotorealCaptureScene(const PhotorealSceneRequest *)` | Starts a [scene capture](#scene-capture). Returns 1 when accepted, and 0 while a scene capture is still running or saving, or the request is null, too short to hold `faces`, or names no known face. It works whether `enabled` is on or not. |
| `uint32_t PhotorealSaveTrace(void)` | Saves the next frame the [trace pass](#path-tracer) writes for the Mac's reference tracer. Returns 1 when accepted, and 0 while the traced scene is not ready, before its first full pass, or while a save waits. |
| `uint32_t PhotorealCompare(const PhotorealVariant *, uint32_t)` | Starts a [comparison capture](#comparison-capture) of 1 to 16 variants. Each `PhotorealVariant` is a 32-byte UTF-8 name and a `PhotorealSettings`. Returns 1 when accepted, and 0 when a capture is still running or saving, or the input is invalid: a null pointer, no variants, more than 16, an empty name, or settings whose `size` does not hold `enabled`. |

Settings:

| Field | Default | Meaning |
|---|---|---|
| `enabled` | 0 | The master switch. While it is 0, each hooked event costs one relaxed atomic load. |
| `view` | off | The debug view, a `PHOTOREAL_VIEW_*` value. Unknown values read as off. |
| `flip` | 1 | Game targets are stored upside down. The debug views and the ambient pass both read it, so an upright debug view means the ambient pass reconstructs positions upright too. |
| `ambient.enabled` | 0 | The ambient pass. |
| `ambient.level` | 0.6 | Multiplies the game's diffuse irradiance on world pixels. Clamped to 0 to 4. |
| `ambient.ao_strength` | 0.5 | Ambient occlusion from the G-buffer normals and depth. One draw writes it raw to a render-size texture of ours, and a second blurs it over 4x4 pixels, never across sky, characters or depth jumps, and applies it. Clamped to 0 to 1. |
| `ambient.ao_radius` | 1 | The occlusion radius in meters. Clamped to 0.05 to 10. |
| `ambient.foliage_ao_strength` | 0.5 | Multiplies `ao_strength` on grass, vegetation and foliage (stencil 129, 136 and 137). Clamped to 0 to 1. A caller built without this field sends a 32-byte struct and gets the default. |
| `contact_shadows.enabled` | 0 | The contact-shadow pass. It runs at the combine step before the ambient pass and lowers the sun's visibility (the shadow mask's red channel) on world pixels where a surface in the depth buffer lies between the pixel and the sun. It never brightens, and it leaves the green channel, characters and the sky as the game drew them. One draw marches 16 steps toward the sun from each pixel into a render-size texture of ours, passing through grass (stencil 129), which never casts, and a second blurs the result over 4x4 pixels like the ambient pass and applies it. A caller built without the block sends a 36-byte struct and gets the defaults. |
| `contact_shadows.length` | 0.6 | How far each world pixel looks toward the sun through the depth buffer, in meters. Clamped to 0.05 to 5. |
| `contact_shadows.strength` | 1 | How much a surface in the way lowers the sun's visibility. Clamped to 0 to 1. |
| `contact_shadows.thickness` | 0.25 | How deep a surface in the depth buffer is taken to be, in meters. A ray that passes further behind it is not shadowed. Clamped to 0.01 to 2. |
| `contact_shadows.foliage_strength` | 0 | Multiplies `strength` on grass, vegetation and foliage (stencil 129, 136 and 137), whose dense blades otherwise shadow each other into dark speckle. Clamped to 0 to 1. A caller built without this field sends a 52-byte struct and gets the default. |
| `atmosphere.enabled` | 0 | The atmosphere pass. It runs at the bloom step, just before the game's first bloom draw, when the HDR scene is complete, so the game's bloom and tone map see the haze. On every pixel with depth, the sky excepted, it reconstructs the distance d and the view ray from the camera, takes the haze's optical depth as `density` times the integral of exp(-`height_falloff` * (h - camera height)) along the ray, in closed form, and with T = exp(-optical depth) writes scene * T + (sky color + sun color * `sun_scatter` * phase) * (1 - T). The phase is Henyey-Greenstein of the angle between the ray and the sun. The colors come from `PhotorealSetCamera`, so without a camera the pass skips. A caller built without the block sends a 56-byte struct and gets the defaults. |
| `atmosphere.density` | 0.000325 | The haze's extinction per meter at the camera's height. The default hazes a pixel 500 m away and level with the camera by 15%. Clamped to 0 to 0.01. |
| `atmosphere.height_falloff` | 0.02 | Per meter. The haze thins by a factor of e every 1 / `height_falloff` meters above the camera and thickens as fast below it, so a valley far below the camera fills with haze. 0 makes the haze even. Clamped to 0 to 1. |
| `atmosphere.sun_scatter` | 1 | Multiplies the sun light the haze scatters toward the camera. Clamped to 0 to 10. |
| `atmosphere.anisotropy` | 0.7 | The Henyey-Greenstein g: 0 scatters sun light evenly, and toward 1 gathers it into a glow around the sun. Clamped to -0.95 to 0.95. |
| `tonemap.enabled` | 0 | The [tonemap pass](#tone-map). It runs at the tonemap step, skips the game's tone map draw and draws in its place. It needs no camera. A caller built without the block sends a 76-byte struct and gets the defaults. |
| `tonemap.exposure_ev` | 0 | Stops added to the game's exposure. Clamped to -10 to 10. |
| `tonemap.curve` | 0 | `PHOTOREAL_CURVE_GAME` (0), the game's filmic curve x(1.36x + 0.047) / (x(0.93x + 0.56) + 0.14); `PHOTOREAL_CURVE_AGX` (1), AgX after Benjamin Wrensch's minimal fit; or `PHOTOREAL_CURVE_NEUTRAL` (2), Khronos PBR Neutral. Unknown values read as 0. |
| `tonemap.bloom_strength` | 1 | Multiplies the game's bloom intensity. Clamped to 0 to 4. |
| `tonemap.saturation` | 1 | Mixes each color with its luma before the curve: 0 is gray. Clamped to 0 to 2. |
| `tonemap.contrast` | 1 | A power around middle gray (0.18) before the curve. Clamped to 0.5 to 2. |
| `sun_shadows.enabled` | 0 | The [sun-shadow pass](#sun-shadows). It runs at the combine step, before the contact-shadow pass, and replaces the sun's visibility in the shadow mask's red channel on world pixels with its own, whose penumbra widens with the distance to what casts the shadow. A caller built without the block sends a 100-byte struct and gets the defaults. |
| `sun_shadows.light_size` | 0.03 | The penumbra's width per meter between the shadow and its caster. The real sun's is 0.0093. The default gives a branch 10 m up a 0.3 m soft edge. Clamped to 0 to 0.2. |
| `sun_shadows.min_penumbra` | 0.02 | The penumbra's least width in meters, where the shadow meets its caster. Clamped to 0 to 0.5. |
| `sun_shadows.strength` | 1 | How much the sun's shadows darken: 0 leaves the sun everywhere, 1 is full. Clamped to 0 to 1. |
| `wetness.enabled` | 0 | The [wetness pass](#wet-surfaces). It runs at the `gbuffer-done` step and makes world pixels wet in the G-buffer, so the game's own lighting, reflections and ambient light see wet surfaces. It needs a camera for the puddles. A caller built without the block sends a 116-byte struct and gets the defaults. |
| `wetness.wetness` | 0 | How wet the world is, from 0 dry to 1 soaked. 0 follows the weather, which `PhotorealSetCamera` carries; any value above 0 replaces it. Clamped to 0 to 1. |
| `wetness.darkening` | 0.35 | How much a soaked surface's albedo darkens, in linear light. Clamped to 0 to 1. |
| `wetness.puddles` | 0.3 | How much of the flat ground puddles cover when soaked, from none at 0 to all of it at 1. Clamped to 0 to 1. |
| `light_shafts.enabled` | 0 | The [light-shaft pass](#light-shafts). It runs at the bloom step after the atmosphere pass and lays a ground mist over the HDR scene, lit by the sun where the game's sun shadow atlas says the sun reaches it and by the sky everywhere, so trunks, crowns and anything else that casts the game's sun shadows draw dark streaks through the mist and the gaps between them bright shafts, on screen or off. Like the sun-shadow pass it needs this frame's sun constants, and the sun and sky colors and the sun's direction from `PhotorealSetCamera`. A caller built without the block sends a 132-byte struct and gets the defaults. |
| `light_shafts.strength` | 3 | Multiplies the sun light the mist scatters. At 1 the mist is as bright as the game's own lighting says: the game lights a white surface facing the sun with the sun color, so the sun's irradiance is pi times it. Clamped to 0 to 10. |
| `light_shafts.density` | 0.006 | The mist's extinction per meter at the camera's height. The default hides 26% of a pixel 50 m away and level with the camera. Clamped to 0 to 0.2. |
| `light_shafts.height` | 5 | Meters: the mist thins by a factor of e every `height` meters above the camera and thickens as fast below it, to at most e^3 (20) times, so a valley below fills with mist. Clamped to 0.5 to 500. |
| `light_shafts.anisotropy` | 0.6 | The Henyey-Greenstein g of the sun light: 0 scatters it evenly, and toward 1 the shafts shine brighter the nearer the camera looks toward the sun. Clamped to -0.95 to 0.95. |
| `light_shafts.distance` | 150 | How many meters of mist each pixel's ray crosses at most, up to the pixel. The sky's rays cross all of it, so shafts show against the sky. Clamped to 5 to 900. |
| `light_shafts.patchiness` | 1 | 0 makes the mist even, and toward 1 it lies in banks with clear holes between them, fixed in the world. The mist's average amount stays the same. Clamped to 0 to 1. |
| `light_shafts.patch_size` | 6 | Meters across a patch of mist. Clamped to 0.5 to 100. |
| `light_shafts.ambient` | 0.3 | Multiplies the sky light the mist scatters, which keeps mist in shadow from going black. Lower it for darker gaps between the shafts. Clamped to 0 to 4. |
| `light_shafts.steps` | 48 | Samples along each ray. Clamped to 8 to 128. |
| `trace.enabled` | 0 | The [trace pass](#path-tracer). It runs first at the combine step while a traced scene is ready, and once every traced pixel has a sample it writes the traced sun visibility into the shadow mask and the traced occlusion into the diffuse irradiance, in place of the sun-shadow, contact-shadow and ambient passes. A caller built without the block sends a 172-byte struct and gets the defaults. |
| `trace.outputs` | both | `PHOTOREAL_TRACE_AO` (1), the occlusion into the irradiance, in place of the ambient pass; `PHOTOREAL_TRACE_SUN` (2), the sun visibility into the shadow mask, in place of the sun-shadow and contact-shadow passes. 0 reads as both. Both are always traced, so switching one on shows its samples at once. |
| `trace.casters` | characters and cards | `PHOTOREAL_TRACE_CAST_CHARACTERS` (1): characters cast, though their own pixels keep the game's values. `PHOTOREAL_TRACE_CAST_ALPHA_CARDS` (4): vegetation and foliage cast as their whole triangles, alpha-tested holes and all. The world and other draws always cast; grass is not in the scene. Unknown bits are dropped. |
| `trace.ao_distance` | 2 | Meters an occlusion ray looks. Its nearest hit at distance t occludes 1 - (t / ao_distance)^2, so occlusion fades out toward the distance instead of stopping at it. Clamped to 0.1 to 20. |
| `trace.ao_strength` | 1 | The occlusion's share: the irradiance is multiplied by `ambient.level` times lerp(1, occlusion, `ao_strength`). The traced and the raster ambient share `ambient.level`, whether the ambient pass is on or not. Clamped to 0 to 1. |
| `trace.sun_size` | 0.0093 | The sun's angular diameter in radians, the real sun's by default, which is also the penumbra's width per meter between caster and shadow, as for `sun_shadows.light_size`. Clamped to 0 to 0.2. |
| `trace.sun_strength` | 1 | The shadow mask's red channel becomes lerp(1, traced visibility, `sun_strength`). Clamped to 0 to 1. |
| `trace.ao_samples` | 64 | Occlusion rays per pixel, after which it stops. Clamped to 1 to 4096. |
| `trace.sun_samples` | 32 | Sun rays per pixel. Clamped to 1 to 4096. |
| `trace.budget_ms` | 20 | GPU time a frame for tracing, measured with timestamp queries. Clamped to 2 to 200. |

## Frame accumulator

The add-on averages the HDR scene of paused frames, for a caller that renders the same shot many times with a small change each time, such as a path tracer's samples. The caller changes the camera or the scene between frames, pushes the camera with the sample's `lens_sample` (x, y, index, and w = 1 on the one frame to add), and sets the accumulator's mode.

`PhotorealAccumulate`:

| Field | Default | Meaning |
|---|---|---|
| `mode` | off | `PHOTOREAL_ACCUMULATE_ADD` (1): at the bloom step, after the atmosphere pass and before the game's first bloom draw, the HDR scene of each frame whose camera came that frame with a marked sample is added into an RGBA32F sum of ours, rgb times the cat's-eye weight and the weight in alpha. A sample is added once, however many frames are marked. `PHOTOREAL_ACCUMULATE_PRESENT` (2): every frame at the bloom step, sum.rgb / sum.a replaces the HDR scene, so the game's bloom, our tone map, ReShade's effects and screenshots all see the average. A pixel with no weight keeps the scene. Off (0) does nothing and keeps the sum. Unknown values read as off. |
| `generation` | 0 | The sum belongs to one generation and one render size. In add mode, a new generation or render size empties it at the next bloom step, sample or not. Present mode presents only a sum of its own generation and size with at least one frame in it. |
| `cat_eye` | 0 | Optical vignetting. At each pixel, a lens point counts only within 1 of a center that moves from the middle of the aperture at the picture's center out to `cat_eye` at its corners, so out-of-focus highlights turn to cat's eyes toward the edges and swirl. Clamped to 0 to 1. |
| `cat_eye_falloff` | 0.1 | The width of that cut's soft edge, in aperture radii. Clamped to 0.01 to 1. |

The status reports `accum_mode`, `accum_samples` (frames in the sum) and `accum_generation` (the sum's generation, 0 before the first bloom step in add mode), and the status line ends in `; accumulating` or `; presenting <n> samples`.

Where the camera push and the render thread's frames line up is unknown, so the caller should hold each sample for 2 frames and mark the first. Whether the add-on takes the push before the frame renders or one frame later, the frame it then adds shows that sample. The accumulator runs at the bloom step, or at the tonemap step when the [bloom step falls back](#status-line), so it does nothing on a frame with neither.

`PhotorealDepthAt` reads the view-space depth at a screen point. At the bloom step, the add-on copies the main depth into its mirror and the asked point's texel into a 1x1 CPU-readable texture, reads it at a later present without waiting, and turns the reversed depth into meters through the inverse of that frame's pushed `view_to_clip`. A new point, or 60 presents without a call, forgets the answer.

## Wet surfaces

The wetness pass edits the G-buffer at the `gbuffer-done` step, after the game's last G-buffer write and before its half-size pass, the ambient pair's reflection draw and the combine pass read it. It copies the albedo (rt1), specular color (rt2), smoothness (rt4), material ID (rt3), normals and depth into mirrors, draws the wet albedo, specular color and smoothness into three more mirrors in one draw, and copies those over the game's. With a wetness of 0 it does nothing.

The wetness w is `wetness.wetness` when it is above 0, else the weather's from the camera. Per pixel, with up the world normal's y from rt0, which holds world-space normals:

1. Puddles: where up is above 0.95, a value noise fixed in the world's x and z, in blobs a few meters across, is a puddle above the threshold 1 - `puddles` * w, with a soft edge 0.05 wide in the noise and fading in from up 0.95 to 0.98. puddle is that cover, from 0 to 1, so puddles grow as the world wets.
2. The pixel's wetness is wet = lerp(w * (0.5 + 0.5 * saturate(up)), 1, puddle): all of w on the ground, half on walls and overhangs, and 1 in a puddle.
3. The albedo, decoded from sRGB, is multiplied by 1 - `darkening` * wet. Its alpha, the material's scalar, stays.
4. The smoothness becomes lerp(lerp(smoothness, 0.9, wet), 0.97, puddle).
5. The specular color, decoded from sRGB, moves toward water's F0 of 0.02 by wet. Vegetation (stencil 136 and 137) keeps its specular color, because the game's vegetation draw reads its x as a switch for the wrapped diffuse of leaves.

Only world pixels change: stencil & 0x84 == 0x80, so characters and the sky keep their G-buffer. Material IDs (rt3 times 255, rounded, without bits 6 and 7) whose rt2 or rt1.a hold something else stay dry: 3, 6 and 23 keep a vector in rt2, and 13, 16, 19 and 24 swap rt1.a and the smoothness. Every pixel the pass does not wet keeps its stored bytes.

The game's reflections come from baked probes, so a puddle reflects the probe's sky and surroundings, not the character or nearby trees. The game's G-buffer shaders have a rain wetness of their own. No census capture in the rain exists yet, so how much the game already wets surfaces in the rain, and how much this pass adds to that, is not known.

## Sun shadows

The game draws the sun's shadow mask in one full-screen draw, which the frame map's `shadow-mask` step finds. That draw samples the sun's shadow atlas at t2, a D16 texture of one square tile per cascade (4096x2048 with 8 cascades at the user's normal settings, 6144x4096 with 6 at PCSS High), and reads the cascades and the camera from its pixel shader constant buffers b0 to b3, of 768, 192, 352 and 912 bytes. At that draw, before it runs, the add-on:

1. Copies the four buffers into four of its own on the GPU, for this frame's pass, and into a CPU-readable buffer, one of three in turn.
2. Copies the atlas into a mirror, because the next frame's shadow pass draws over it.
3. Reads back, without waiting, the newest of the two previous frames' CPU-readable copies the GPU has finished, and `core/sun.cpp` parses it: the atlas's size and grid, the cascades in use (up to the first whose tile center is off the grid), their spheres, matrices and depth ranges, the shadow distance, and the camera. It checks that the radii rows repeat the spheres, the radii grow, the tiles are square and on the grid, the depth ranges in b0 match the matrices, the camera's basis is orthonormal and the camera is at cascade 0.

The add-on does this while the sun-shadow or light-shaft pass is on. Each of them runs only when the draw's buffers had those sizes, the last parse passed every check and described an atlas of this frame's size. Otherwise the status line says `sun constants <check>`, such as `sun constants unread` for the first frame or two after the pass is switched on. The readback only gates the passes and gives the cascade count and grid, which change only with the graphics settings. The passes draw with this frame's own copies, so their shadows stay on the atlas while the camera moves.

The first draw reconstructs each world pixel's position exactly as the game's shadow shaders do, from the game's own camera constants, picks the first cascade whose sphere holds it, and projects it into the atlas. In the outer tenth of a cascade's radius, a growing share of pixels take the next cascade, so the blur fades one into the other. A blocker search of 16 point taps finds how far the casters lie above the pixel toward the sun, and 16 bilinear comparison taps (lit where the pixel is at least as near the sun as the atlas) average the visibility over a disc as wide as `light_size` times that distance, at least `min_penumbra`, and at most 24 atlas texels in radius. The taps follow a Vogel disc turned by the 4x4 pattern, so each pixel's pattern is the same every frame. Each tap compares against the receiver's plane, which the depth buffer gives, so a wide penumbra does not shadow a slanted surface with itself. The second draw blurs the result over 4x4 pixels like the contact-shadow pass and writes it to the mask. Pixels past the game's shadow distance or outside every cascade keep the game's value.

## Light shafts

The light-shaft pass marches one view ray per 2x2 block of pixels through a mist and finds, at each step, whether the sun reaches that point from the game's sun shadow atlas, which the add-on copies at the shadow-mask draw as for the [sun-shadow pass](#sun-shadows). The atlas holds every caster of the game's sun shadows within its cascades around the camera, so a trunk or a crown behind the camera or above the top of the screen still darkens its streak of mist, and the shafts need no sun on screen.

The first draw writes two half-size textures of ours: the mist's in-scatter and transmittance, and each ray's length. Each half-size pixel takes its ray through one of the four full-size pixels of its cell, the four of a 2x2 block through all four, so a twig one pixel wide still gets rays. Rays start at the game's camera and run through the pixel, with positions from the game's own camera constants, as the sun-shadow pass reconstructs them, so each step lands on the atlas where the game's shadow shaders would put it. A ray is `distance` meters long, or shorter where it hits the pixel's surface; the sky's are whole. Its `steps` samples are spaced quadratically, short near the camera and long far away, each offset into its span by the 4x4 pattern. At each sample:

1. The mist's extinction is `density` times exp(-rise / `height`), with the exponent at most 3, times lerp(1, 2 smoothstep(0.25, 0.75, n), `patchiness`), where rise is the sample's height above the camera and n one octave of value noise at the world position over `patch_size`, fetched from a 64^3 lattice of random bytes uploaded once and wrapped, with the lattice's axes turned away from the world's.
2. The sun's visibility is one bilinear comparison sample of the atlas in the first cascade whose sphere holds the sample, as the game picks them: 1 past the game's shadow distance or outside every cascade. The ray's span in each cascade's sphere is solved once per ray, and the cascade's transform is affine along the ray, so a sample costs a few multiply-adds before its fetch.
3. The light is pi times the sun color times `strength` times the Henyey-Greenstein phase of the angle between the ray and the sun times the visibility, plus the sky color times `ambient`. The sky color is the sky's radiance, since the game lights an up-facing white surface with it, and the mist scatters it evenly.
4. The step adds transmittance times (1 - exp(-extinction times its span)) times the light to the in-scatter and multiplies the transmittance by exp(-extinction times its span). That integrates each span exactly, so an even mist gives the same result at any step count. A ray stops once its transmittance is below 0.001.

The second draw writes every full-size pixel from the 4x4 half-size samples around it, one whole period of the pattern, so the offsets average out. Each sample is first rescaled from its ray's length to the pixel's as if its mist were even, which is exact for an even mist, and a sample counts fully when its ray is as long as the pixel's and less the more they differ, down to 0.001 at a tenth of the pixel's length. The pixel becomes scene times transmittance plus in-scatter, written through a mirror of the HDR scene.

The mist scatters each ray of light once: the sun light that reaches a point is not dimmed by the mist between it and the sun. The mist follows the camera's height, so it thins when the camera rises. Its cost on the Deck has not been measured; the shaders' instruction counts suggest 3 to 4 ms at 1280x800 with the defaults, about three quarters of it the march, which scales with `steps`.

## Tone map

The game's tone map draw reads the HDR scene at t0 and the bloom chain's last result at t1, a quarter-size R11G11B10 texture the frame map calls `bloom-final`, and writes the render-size R8G8B8A8 target the next pass reads. The tonemap pass returns `GameCall::skip` for that draw and draws through a mirror of its target instead:

1. HDR scene + final bloom, bilinear, times the game's bloom intensity times `bloom_strength`. Without `bloom-final` the pass adds no bloom.
2. The game's 3x3 color matrix, then the game's exposure times 2^`exposure_ev`.
3. `saturation` and `contrast`, then the curve.
4. The game's encode, max(x^(1/2.4) * 1.055 - 0.055, 0), raised to the game's display gamma; the encoded luma in alpha, as the game's shader variant at the user's normal settings writes it; and the game's dither, interleaved gradient noise of -0.5/255 to 1.5/255.

The game's values come from the skipped draw's own constant buffer at b0, which the pass copies on the GPU each frame into a buffer of its own at b1: exposure is row 22.z, bloom intensity 22.y, the color matrix rows 99 to 101, and the display gamma 20.y. They sit at those rows in all three tone map shader variants the census found. At the user's normal settings they were exposure 0.985, bloom 0.75, a color matrix within 0.0006 of identity, and gamma 1. When the buffer is too small, or a value is out of range or NaN, the shader uses exposure 1, bloom 0.75, the identity matrix and gamma 1. With the defaults and curve 0, the pass computes what the game's draw computes at the user's normal settings, except for these, all off or absent in that capture:

- Eye adaptation (row 98.x and a 1x1 texture at t4).
- The screen overlay at t3 (row 25.x).
- The HDR display path (row 20.x), which PQ-encodes and applies a 3D LUT at t2. The pass always writes the SDR encode.
- The 2D 256x16 color LUT. Only the shader variant in capture `201640`, at low settings, samples one, and its rows 104 and 105 differ from the other variants, so the pass leaves it out.

## Status line

`PhotorealDescribe` reports the frame in one line with no frame counter, so a mod can log it when it changes. For example:

```
1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap forward gbuffer-done; ran wetness@gbuffer-done sun-shadows@combine contact-shadows@combine ambient@combine atmosphere@bloom light-shafts@bloom tonemap@tonemap; view normals
1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap forward gbuffer-done; ran ambient@combine; skipped sun-shadows: sun constants unread
1152x720 found gbuffer quarter-shadow ambient-pair combine bloom tonemap forward gbuffer-done; ran ambient@combine; skipped contact-shadows: missing shadow-mask
found nothing; skipped contact-shadows: missing normals depth shadow-mask; skipped ambient: missing normals depth ambient-diffuse
1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap forward gbuffer-done; ran trace@combine (64 ao, 32 sun); replaced sun-shadows contact-shadows ambient
1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap forward gbuffer-done; ran sun-shadows@combine contact-shadows@combine ambient@combine; skipped trace: scene building
off; error shader
```

The parts are the render size and the steps found, the passes that ran and the step they ran at (the `tonemap` step for a bloom-step pass on a frame whose `bloom` step fell back), each skipped pass with its reason, the debug view, the G-buffer restarts, the accumulator as `accumulating` or `presenting <n> samples`, a comparison capture's progress as `comparing <saved>/<variants>`, a [scene capture](#scene-capture)'s as `scene <written>/<faces>` or `scene stopped: <reason>`, and an error. A skip reason is `missing <entries>`, `no <step>`, `no camera`, `sun constants <check>`, `scene <state>` (the trace pass's traced scene is `none`, `building`, `uploading`, `stale` or `failed`), or `no texture`. Passes the trace pass writes for come after `replaced`; the trace pass names the samples each traced pixel has, as in `ran trace@combine (64 ao, 32 sun); replaced sun-shadows contact-shadows ambient`. The checks are `unread`, `signature` (the shadow-mask draw's constant buffers had other sizes), `atlas-changed`, and the parse's `not-finite`, `atlas`, `tiles`, `radii-rows`, `radii-order`, `depth-range`, `basis` and `camera`. A view that could not be shown says `missing <entry>`, `no <step>`, or, for the `light-shafts` view, `light-shafts did not run`.

The `forward` step is the first draw into the HDR scene after the combine clear that does not sample the G-buffer normals. Every deferred draw of the game reads them: world, characters and vegetation, and in capture `111824` two more surface classes that also write a second target. Its sky, transparent and fog draws do not. So at that draw the deferred lighting is whole and nothing has been drawn over it yet.

The `gbuffer-done` step is the first draw after the `gbuffer` step that samples the G-buffer's smoothness while the normals are not bound as a target. The game writes the G-buffer in three binds: its main draws, then decals into rt0 to rt4, and decals into rt0, rt1, rt2 and rt4. In every capture the first draw after them that reads the smoothness is a half-size pass that also reads the normals: seq 348 in `111824`, 680 in `214859` and `214907`, and 316 in `201640`. It comes before the ambient pair, the other early reader of the smoothness, so at that draw the G-buffer is whole and nothing has read it yet. A half-size draw before the decals reads only the normals, so the normals could not mark the step. In a frame without the half-size pass, the ambient pair's draw comes first and takes its own step, and `gbuffer-done` falls to the combine step's first draw, after the ambient pair.

The `bloom` step is the first draw into a single quarter-size R11G11B10 target that samples the HDR scene. On the Deck in build 196, the game's bloom did not match for up to a quarter second every 3 to 13 seconds of walking, while its tone map draw did, so the light shafts went off for a moment. Why the bloom differs on those frames is not known yet. The `bloom` step therefore falls back to the `tonemap` step: when the tone map draw matches and the bloom step has not, that draw is the bloom step's moment too. Its passes, the accumulator and the depth copy run there, just before the tonemap step's, while the HDR scene is still the next thing the game reads, and the light-shafts view is copied there. Only the game's bloom of that frame lacks their effect. A bloom draw after that does not match, so nothing runs twice. `bloom` stays out of the steps found, and the passes are named `@tonemap`:

```
1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine tonemap forward gbuffer-done; ran atmosphere@tonemap light-shafts@tonemap
```

So that the bloom rule can be fixed, the first frame of a session whose bloom step falls back writes a line to `ReShade.log` and turns on a trace of the draws for the frames after it. The next frame whose bloom step falls back writes its draws from the combine step to the tonemap draw to `ReShade.log`, one line per run of alike draws, such as `draws 54-56: rt #58 26:136x158; t0 #57 26:480x300 t1 material-id ... +15 more`. A line names the render targets (`rt`), the depth target (`ds`), and the pixel shader inputs by slot from `t0` to `t7` and any frame-map entry past them, then counts the other slots that hold something, which are mostly earlier draws' inputs left bound. A frame-map entry goes by its name, so a draw that samples the HDR scene shows `hdr-scene`. Any other texture goes by a number of its own in that trace, its `DXGI_FORMAT` number and its size. This happens at most three times a session. The trace asks every draw after the combine step for its inputs, so it stops 3600 presents after the last fall-back.

The errors are `not-d3d11`, `shader` (our shaders or states failed to create), `texture` (a texture of ours failed to create this frame), and `state`. A `state` error means the game's render target 0, depth view or pixel shader differed after the add-on restored state. The add-on then stays off until the game restarts and writes which binding moved to `ReShade.log`.

`PhotorealStatus` carries the same facts as bit sets (`PHOTOREAL_STEP_*`, `PHOTOREAL_PASS_*`, `PHOTOREAL_ENTRY_*`), plus the frame count, the settings generation the last frame used, the frames since the last camera, `compare_remaining`, the variants of a comparison capture not yet saved, the accumulator's mode, samples and generation, and the scene capture's face, remaining faces and stream-output support. `armed` is 1 while the add-on follows the game's frame, which it does while `enabled` is on, an accumulation or a scene capture runs, or `PhotorealDepthAt` is being asked. The trace's five fields are the traced scene's state, its triangles, the full passes since its samples last started, the last measured rays a second in millions times 100, and its generation.

## Debug views

A view is copied at its step and drawn over the back buffer at `reshade_present`, after ReShade's effects and overlay, so ReShade screenshots do not include it. The view's name, as the status line writes it, sits in white capitals on a dark box in the top-left corner, so it shows even though the view covers the mod's own messages. The shader draws it from a built-in 5x7 font at 3 screen pixels per font pixel at 800 lines, scaled with the back buffer's height.

| View | Shows |
|---|---|
| `normals`, `albedo`, `specular` | The G-buffer target's stored values as color. |
| `material-id` | The low six bits of the material ID, one color per ID. |
| `smoothness`, `character`, `shadow-mask`, `quarter-shadow` | The red channel as gray. |
| `depth` | Reversed depth: near is bright, the sky is black. |
| `stencil` | Sky black, world gray, grass green, characters magenta, vegetation dark green, foliage light green, any other value red. |
| `ambient-diffuse`, `ambient-specular`, `hdr-scene`, `bloom-final` | HDR values, tone mapped. |
| `sun-atlas` | The sun's shadow atlas, which the game's shadow-mask draw samples at t2, as gray, stretched over the screen: one square tile per cascade, nearest first, in rows from the top left. Stored depth grows toward the sun, and 0, where nothing was drawn, is black. It is copied at that draw. |
| `light-shafts` | The light the light-shaft pass's mist scatters toward the camera, alone, tone mapped like the HDR views. It is no frame-map entry: while this view is on, the pass's second draw also writes it to a render-size texture of ours, which is copied at the bloom step. While the pass does not run, the view shows the missing pattern. |
| `trace` | The trace pass's averages, unblurred: occlusion in red and sun visibility in green, and blue where a pixel has no sample yet, such as the sky and characters. The pass's draw writes it to a texture of ours at the combine step while this view is on, from its first frame, before it writes the frame map. While the pass does not run, the view shows the missing pattern. |

The G-buffer and lighting views show what the game's combine pass reads, after the wetness, sun-shadow, contact-shadow and ambient passes ran, so `albedo`, `specular` and `smoothness` include the wet surfaces and `shadow-mask` includes the soft sun shadows and the contact shadows. `hdr-scene` shows the image just before the game's tone map, after the atmosphere and light-shaft passes, and `bloom-final` the quarter-size bloom the tone map adds to it. When the view's entry was not found, the screen shows dark magenta diagonal stripes, and the label ends in `MISSING`.

## Comparison capture

`PhotorealCompare` renders the same frame with each variant's settings and saves it, so effects, flicker and side effects can be judged from images instead of by eye on the Deck. Pause the game first, so only the settings change between variants.

The render thread runs the variants in order from the next present, counting presents from 0:

1. At present 6v it applies variant v's settings, in place of the ones from `PhotorealApply`.
2. The 4 presents after that let the variant settle.
3. At presents 6v + 5 and 6v + 6 it copies the back buffer into a staging texture, at ReShade's `present` event, before ReShade's effects. The next variant applies at the second copy.
4. At 6v + 8 it reads both copies back, 2 presents after the second, so the GPU has finished them and the read does not wait. A worker thread converts them, saves the second as a PNG and writes the variant's row.

After the last variant, the settings from before the capture come back. Settings applied during the capture take over after it. Sixteen variants take 98 presents. The debug composite is not drawn during a capture, so a variant's debug view never shows in its PNG.

The files go to `<ReShade base path>\Photoreal\compare-<YYYYMMDD-HHMMSS>\`, and `ReShade.log` names the folder:

| File | Holds |
|---|---|
| `NN-<name>.png` | The variant's second frame, where `NN` is its index from `00`. A name keeps letters, digits, `.`, `_` and `-`, and any other byte becomes `-`. |
| `compare.tsv` | One row per saved variant: `index`, `name`, `status` (the status line of the saved frame), `flicker`, `flicker_max`, `flicker_mean`, `width`, `height`. It is rewritten after each variant. |
| `settings.tsv` | One row per variant with every parsed settings field, after defaults and clamping, as the add-on ran it. |
| `stopped.txt` | Only when the capture stopped early: how many variants it saved, and why. |

Flicker compares the two copied frames of one variant. A pixel's difference is its largest 8-bit RGB channel difference. `flicker` is the fraction of pixels whose difference is over 2, `flicker_max` the largest difference, and `flicker_mean` the mean difference over all pixels. A still frame with stable effects has a `flicker` of 0.

The capture stops early when the add-on is off after an error, the D3D11 device goes away, an armed frame's render size or steps found differ from the first armed frame of the capture, or the back buffer is not 8-bit RGBA or BGRA. It then saves a variant that was copied but not yet read, restores the settings and writes `stopped.txt`.

## Scene capture

`PhotorealCaptureScene` captures the geometry around the camera, the first step toward a path tracer that traces the game's own scene. For each requested face it asks the caller to turn the camera, and once a frame whose own camera shows that face has rendered, it replays every draw of that frame's G-buffer with stream output and writes the triangles in world space, with the frame's picture and depth. Pause the game and hide the UI first, so nothing moves between faces.

`PhotorealSceneRequest`:

| Field | Default | Meaning |
|---|---|---|
| `faces` | all seven | Bits `1 << PHOTOREAL_FACE_*`. 0 reads as all seven (0x7F), and unknown bits are dropped. |
| `settle` | 45 | Presents each turned face is held before its frame is captured, so streaming and LOD catch up: about 1.5 s at 30 fps. 0 reads as 45, and it clamps to 2 to 600. The view face waits 2, since the camera does not turn. |
| `flags` | 0 | `PHOTOREAL_SCENE_RAW` also writes each face's stream output as captured; `PHOTOREAL_SCENE_PLY` writes binary PLY in place of OBJ. `PHOTOREAL_SCENE_TRACE` keeps the faces' triangles and builds the [traced scene](#path-tracer) from them; with `faces` 0 it captures nothing and traces the self-test scene. `PHOTOREAL_SCENE_NO_MESHES` writes no OBJ or PLY. `PHOTOREAL_SCENE_GBUFFER` also writes the view face's normals and stencil and the game's shadow mask and diffuse irradiance. `PHOTOREAL_SCENE_RELEASE` captures nothing and frees the traced scene; `faces` and the other flags are then ignored. |
| `max_triangles` | 4000000 | Per face. Whole draws past it are left out of the file and counted. 0 reads as the default. |

The faces are world-axis cube faces in D3D cube-map order and up vectors, so a later step can resample the same frames into a cube:

| `PHOTOREAL_FACE_` | Value | Forward | Up |
|---|---|---|---|
| `POS_X` | 0 | (1, 0, 0) | (0, 1, 0) |
| `NEG_X` | 1 | (-1, 0, 0) | (0, 1, 0) |
| `POS_Y` | 2 | (0, 1, 0) | (0, 0, -1) |
| `NEG_Y` | 3 | (0, -1, 0) | (0, 0, 1) |
| `POS_Z` | 4 | (0, 0, 1) | (0, 1, 0) |
| `NEG_Z` | 5 | (0, 0, -1) | (0, 1, 0) |
| `VIEW` | 6 | the camera as it is | |

The view comes first, then the others in value order. While a face is wanted, `PhotorealStatus.scene_face` names it, and the caller turns the camera there at a 90-degree vertical field of view at the current aspect; at 16:10 that is 116 degrees across, so each face covers its cube square with overlap. Otherwise it is `PHOTOREAL_FACE_NONE`. CameraTools' free camera does this while it is on, without changing its own pose, which comes back when the face is none. The add-on accepts a frame for a face only when that frame's own camera, from the game's shadow-mask constants, looks within about 1.8 degrees of the face's forward with its up within about 2.6 degrees, so the delay between the caller's push and the render thread does not matter. Every face's camera must also sit within 1 mm of the first saved face's.

From the present the request arrives at, counting from each face's publish:

1. The face is published at present 0 and held for `settle` presents.
2. The frame after the next present is captured, when fewer than two saved faces still wait for the writer thread. At the shadow-mask draw the add-on copies that frame's sun constants, as for the [sun-shadow pass](#sun-shadows). While the G-buffer is bound (6 targets, the normals first, the main depth), each draw is replayed just before the game's own, with every binding the game made for it: a geometry shader with stream output made from the draw's own vertex shader's bytecode, or its domain shader's when the draw is tessellated, captures its `SV_Position` before the perspective divide, 16 bytes a corner. A triangle list replays as a point list: the vertex shader runs for the same indices, and every corner is streamed in index order, which is the list's triangles three corners at a time. DXVK builds that geometry shader as a pass-through that takes one point and emits one point, so fed triangles it streamed only each triangle's first corner on the Deck in build 198, while its query still counted every triangle. Strips and adjacency lists are skipped (`topology`); the census has none. Each replay writes at its own byte offset, which the add-on reserves front to back in draw order: a direct draw's `count x instances` corners, and 49,152 corners (16,384 triangles) for an indirect or tessellated draw, whose count only the GPU knows. A draw's corners are then read from where it wrote them, never from where the draw before it ended. Before the frame, the buffer is cleared to a NaN marker no vertex shader writes, so a corner no replay wrote can be told apart, which zero could not: shaders cull a vertex by writing a zero position. A blend state that writes nothing and a depth-stencil state with depth and stencil off keep every G-buffer byte as it was, and `ReplayGuard` puts back the game's geometry shader, blend and depth-stencil states, topology and stream-output targets. Each replay has its own `D3D11_QUERY_SO_STATISTICS` query. The depth prepass is captured too, and the decals and the sky, which draw outside that bind, are not. At a G-buffer restart the replays start over, so only the frame's last G-buffer, the one the passes use, is captured.
3. At the present that ends that frame, the add-on copies the stream output, the main depth and the back buffer, before ReShade's effects, into CPU-readable resources.
4. Two presents later it reads the queries and the copies without waiting, again at each present while they are not finished. The queries give each replay's corners. A draw whose reserved corners still hold the marker is left out of the files and counted as unwritten; so is one that needed more corners than its reservation, since the next draw wrote over the rest. The frame's camera is the game's own from its shadow-mask constants, as one homogeneous clip-to-world matrix, inverse(view) * inverse(GL projection) * inverse(to_d3d), where to_d3d flips y for the render texture and reverses z. Only `b1` row 5 and `b2` rows 10 to 13 and 18 to 20 are read and checked (finite, orthonormal basis); the sun-shadow pass's checks of the cascades in `b3` do not apply, and in build 198 they refused every face at `radii-order`. A corner behind the camera comes back exact, because the transform never divides by the clip w. When the constants are missing or fail their checks, the camera CameraTools pushed for that frame places the geometry instead, and `scene.tsv` says why. In build 198 the two cameras agreed to about 1e-6 of the distance on every face.
5. A frame that shows the face is handed to the writer thread, and the next face is published at once. A frame whose camera faced elsewhere, that had no G-buffer, that had a draw whose reservation did not fit (`buffer-full`) or that overran it, or whose copies stayed unread for 60 presents is captured again, up to 8 times. A draw that did not fit first makes the buffer 1.25 times what the frame's reservations wanted, up to 512 MB; at that size the face is saved partial. A draw that overran gets 1.25 times the corners it needed, for the rest of the capture, by its call, arguments and shaders, so an indirect draw that overran on one face starts large on the next.

The capture stops, and keeps what it saved, when the camera moved from the first saved face's position, a face was captured 8 times without success (when its camera faced elsewhere each time, the reason names the free camera), a face was not saved within 900 presents of its publish, no G-buffer draw could be replayed, the frame had no camera at all, `CreateGeometryShaderWithStreamOutput` returned `DXGI_ERROR_INVALID_CALL` (DXVK returns it when the Vulkan driver lacks transform feedback; `scene_support` then reads 2), a read failed, the add-on tripped, or the device went away. A draw that could not be replayed is skipped and named in `draws.tsv`: `no-bytecode` (its shader was created before the add-on loaded), `gs-bound`, `no-position`, `game-so` (the game had a stream-output target bound), `query-pool` (more than 2048 replays), `gs-create-failed` (refused for that shader only), `topology` (not a triangle list or a tessellated patch list), or `buffer-full` (its reservation did not fit).

`ReShade.log` gets one line per saved face with its replayed draws, triangles, the draws whose written triangles differ from what their call implies, the draws with unwritten corners that were left out, and the camera source with why when it is the pushed one. The last two counts say whether stream output wrote what D3D11 promises: a count can match while the bytes do not, as in build 198. The status reports `scene_face`, `scene_remaining` (faces not yet written) and `scene_support` (0 untried, 1 stream output wrote triangles, 2 refused). The status line ends in `; scene <written>/<faces>` while a capture runs or saves, and in `; scene stopped: <reason>` after one stopped, until the next starts. The stream-output buffer, its CPU-readable twin and the queries exist only while a capture runs. While none runs, each draw costs one more branch, and frames are exactly as before. The registry of the game's vertex and domain shader bytecode, which a replay needs because D3D11 cannot give bytecode back from a bound shader, fills as the game creates its shaders, from ReShade's `init_pipeline` event, which the add-on registers when it loads; it holds a few megabytes.

The files go to `<ReShade base path>\Photoreal\scene-<YYYYMMDD-HHMMSS>\`, and `ReShade.log` names the folder:

| File | Holds |
|---|---|
| `face-N-<name>.obj` | The face's triangles: `mtllib scene.mtl`, then per replayed draw a `g d<draw>_<kind>[_s<stencil>]_vs<hash>[_ds<hash>]` and a `usemtl <class>` line, its vertices with 3 decimals (millimeters) and its faces. The kind is `pre` (stencil off, depth write on), `equal` (depth test EQUAL) or `main`. Corners of one draw with the same clip bits came from the same vertex shader invocation and merge into one vertex. `N` is the face's value and `<name>` one of `pos-x`, `neg-x`, `pos-y`, `neg-y`, `pos-z`, `neg-z` and `view`. |
| `face-N-<name>.ply` | With `PHOTOREAL_SCENE_PLY`, in place of the OBJ: binary little-endian, float x, y, z per vertex and a uchar 3 with three int indices per triangle, without draws or classes. |
| `scene.mtl` | One color per class, the stencil debug view's: the prepass (`pre`) red, `s128` gray, `s129` grass green, `s133` magenta, `s136` dark green, `s137` light green, and `s144`, `s165` and `other` brown. |
| `face-N.png` | The back buffer before ReShade's effects at half size: what the face looked like. |
| `face-N-depth.r32` | The main depth as the game stored it, reversed, float32 little-endian, top row first, at the render size. |
| `face-N.so` | With `PHOTOREAL_SCENE_RAW`: the captured clip positions of the draws in the files, three float4 per triangle, in draw order. |
| `face-N-sun.bin` | The shadow-mask draw's `b0` to `b3` of that frame as read back, 2,224 bytes, when there were any: the camera's source, and the cascades the sun-shadow pass checks. |
| `face-6-stencil.r8`, `face-6-normals.r10g10b10a2`, `face-6-mask.r8g8`, `face-6-irradiance.r11g11b10` | With `PHOTOREAL_SCENE_GBUFFER`, the view face's stencil and G-buffer normals, and the game's shadow mask and diffuse irradiance, copied at the combine step before any pass of ours, as stored, at the render size, top row first. |
| `trace-scene.bin` | With `PHOTOREAL_SCENE_TRACE`, the triangles the add-on uploads, before the BVH reorders them: `PRTS`, a uint32 count, the reference as three float64, then per triangle nine float32 corners relative to it in Unity's axes and the uint32 info (bits 24 to 27 the class: world, vegetation, foliage, other, character). |
| `scene.tsv` | One row per saved face: its frame, captures, camera source and why, the captured frame's status line, the render size, draws replayed and skipped by reason, triangles written and needed, whether it was saved partial, triangles and vertices in the file, triangles dropped at infinity and past `max_triangles`, the draws with unwritten corners and how many corners, the draws that overran their reservation, the depth check per class, the mesh origin, and the camera's position, forward and up. |
| `draws.tsv` | One row per G-buffer draw of each saved face: kind, class, call and arguments, topology, vertex, domain and pixel shader hashes (FrameCensus's, so they match a census capture's `shaders/`), stencil, depth test and write, expected (direct triangle lists and strips), written and needed triangles, the first triangle in the files' stream, the skip reason, whether it replayed as points, its offset, reservation and written corners in the stream-output buffer, its unwritten corners, and the pixel shader's `t0` to `t7` as `id:DXGI_FORMAT:WxH`. |
| `camera.tsv` | Per face: the camera source, the shadow-mask draw's b1 rows 5 and 10 and b2 rows 6 to 13 and 18 to 20 as comma-separated rows, `clip_to_world`, and the pushed `world_to_view` and `view_to_clip`, each matrix row by row. |
| `stopped.txt` | Only when the capture stopped: how many faces it saved, and why. |

Mesh positions are relative to the first saved face's camera position (the view's, when it was asked for), so viewers keep their precision, and right-handed: Unity's (x, y, z) is written as (x, y, -z), with the second and third corner of each triangle swapped so the winding survives the mirror. Blender's default import (forward -Z, up Y) shows the scene upright and unmirrored; import all faces at once. Whether the game's winding faces outward after its render-texture flip has not been seen yet; `check_scene.py` reports the share of the view's triangles that face the camera.

The depth check in `scene.tsv` compares each captured corner in front of the camera with the main depth around its pixel, both in meters along the camera's forward axis: the corner's clip w, and the buffer through the same camera. The pixel comes from the clip the vertex shader wrote, so no flip is involved. A corner matches within max(0.05 m, 1%) of the depths of the 3x3 pixels around it, since a corner on a silhouette or a surface seen edge-on lies between pixel centers. It is `in_front` when nearer than all of them (wrong, or an alpha-cut corner) and `behind` when farther (occluded, which is fine). For stencil 128, few corners in front means the stream output holds what the G-buffer drew.

On the Mac, `python3 tests/check_scene.py <scene folder>` (numpy only) checks a folder: the depth check, every direct draw's triangle count, and that no replayed draw left a corner unwritten; each face's depth rendered from all faces' meshes against its own depth buffer at quarter size, which proves the geometry behind the camera is in the right place and writes `check-face-N.png`; the share of 20,000 directions all faces cover against the view alone; how closely two faces agree where both see stencil 128; the winding; and b1 row 5 against the pushed camera's position.

## Path tracer

The trace pass traces ambient occlusion and sun shadows against the geometry a [scene capture](#scene-capture) saw from all seven faces, so casters behind and above the camera, which the screen-space passes cannot see, shadow and occlude too. This is the path tracer's first step: every caster is opaque, vegetation and foliage cast as their whole triangles, and the game's shadow-only casters are not in the scene.

### The traced scene

A scene capture with `PHOTOREAL_SCENE_TRACE` hands each saved face's triangles to the writer thread as it writes the face, where `core/trace_scene.cpp` takes them into one scene:

1. Each corner goes to the world through the face's camera in double precision, relative to the first saved face's camera, the scene's reference. A triangle with a corner at infinity is dropped.
2. Its key is its corners on a 1 mm grid, rounded half up and sorted, so the same triangle from two faces is kept once; a triangle whose corners the grid merges is dropped as degenerate. Of two copies, a main draw's replaces an equal-depth draw's, which replaces the depth prepass's; of copies of one kind the first face's stays. On build 199's capture the 6,692,656 triangles of the seven faces came to 2,377,274.
3. Grass (stencil 129) is left out, since it never casts, as for contact shadows, and so is every draw that leaves no surface in the depth buffer: one that writes no depth and does not draw the prepass's surfaces again at EQUAL depth. In build 199's capture those were a 12-triangle stencil volume around the character (stencil 38, depth test ALWAYS), which cast a box-shaped shadow on the Mac's trace of that frame, and the character's overlays. Characters (133, 165) cast but are never traced themselves; vegetation (136) and foliage (137) cast as whole cards; the prepass's triangles without a main twin count as other.
4. Past the request's `max_triangles`, at most 4,000,000, the triangles whose centroid lies farthest from the reference are left out. When a scene's buffers could not be made, the next traced capture keeps half as many.

After the last face, the writer thread writes the triangles to `trace-scene.bin` in the scene folder and builds their BVH (`core/bvh.cpp`): binned SAH over the centroids with 16 bins an axis, traversal and intersection costing 1 each, leaves of at most 4 triangles, or up to 16 when splitting costs more, and a split in the middle where the centroids coincide. The first log2(3) + 2 levels build their left child on threads of their own, and the nodes are numbered depth first, so the bytes do not depend on the threads; with no fused multiply-adds, a build on the Mac gives the Deck's bytes too. A node is 64 bytes, both children's boxes and their links; a triangle 48, its first corner, its info word and its two edges. Build 199's 2,377,274 deduplicated triangles, grass included, give 756,325 nodes, 3.14 triangles a leaf, 48.4 MB of nodes and 114.1 MB of triangles, built in 2.3 s with the add-on's 3 tasks and 1.3 s with 8 on an M2.

The render thread then makes the node and triangle buffers, raw shader resources of at most 256 MB each, fills them 32 MB a present, and swaps the scene in once they are full. `trace_state` reads `BUILDING` from the capture's start, then `UPLOADING`, then `READY`, and `trace_generation` goes up with each scene. A capture that stops, a BVH or a buffer that cannot be made, or a failed D3D11 call leaves `FAILED`, and `ReShade.log` says why. `PHOTOREAL_SCENE_RELEASE` frees it all. `PHOTOREAL_SCENE_TRACE` with `faces` 0 traces the self-test scene in place of a capture: three triangles around the pushed camera, a roof 4 m above it, a wall 5 m ahead and a plate 1 m below it 2 m ahead, which cast shadows and occlusion onto whatever the frame shows, so the `trace` view of it proves the GPU's kernel against the Mac's. The consistency grid does not judge it, because no frame's depth buffer shows it, and it would otherwise go stale after its first pass.

### Tracing

The trace pass runs first at the combine step. It needs the normals, depth, shadow mask, diffuse irradiance, a camera and a ready scene. `shaders/trace_cs.hlsl` runs one thread a pixel in 8x8 groups, 64 groups to a 64x64 tile, over the tiles in Morton order. Each frame it traces as many tiles as the time budget allows, from where the last frame stopped, and each tile's visit adds one sample to each of its pixels' rays. A dispatch's two timestamp queries are read a few presents later without waiting, and the next frame takes tiles times `budget_ms` over the measured time, at most twice as many, at least 1, and never a tile twice in one frame. It starts at 4.

For each world pixel with depth, `shaders/trace.hlsli`:

1. Reconstructs its surface from the depth through the pushed camera, relative to the scene's reference, and its geometric normal from the nearer of the two neighbors with depth on each axis, facing the camera.
2. Starts its rays 0.02 m plus 1e-4 of its distance off the surface along that normal.
3. Shoots one occlusion ray, cosine-weighted around the G-buffer's normal and mirrored above the surface where it leans below it, while the pixel has fewer than `ao_samples`. Its nearest hit within `ao_distance` at t gives a visibility of (t / `ao_distance`)^2, a miss 1.
4. Shoots one sun ray, uniform in the cone of `sun_size` around the direction toward the sun, while it has fewer than `sun_samples`. Any hit within 100 km is shadow. The direction is the game's, cascade 0's depth gradient from the shadow-mask constants read back without the sun-shadow pass's checks, else the pushed camera's; the log names the source.
5. On every 16th pixel each way, unless it is grass, vegetation or foliage, shoots a ray from the camera to the surface against the world and other casters, the consistency grid.

The samples follow the R2 sequence, turned for each pixel by a hash of the pixel and the scene's generation, in 32-bit integers, so the rays are a fixed function of the pixel and its sample's index, and the Mac can shoot the same ones. The traversal keeps a stack of 24 nodes in group-shared memory, nearer child first, and stops at 4,096 nodes; a ray whose stack overflowed (its far child left out) or that met the guard is counted, and the counts go to the log.

The history is a buffer of four floats a pixel: the occlusion and sun sums and their counts. It is cleared, and the samples start again, when the camera's view or projection changes by more than 1e-6 of its largest element, the render size, the sun's direction (by more than 1e-5), the scene, `ao_distance`, `sun_size` or `casters` change. That frame traces nothing, so a moving camera costs no rays; `ao_samples`, `sun_samples`, the strengths and the outputs do not clear it.

Once every traced pixel has a sample, and the samples did not just start again, the pass's draw (`shaders/apply_trace_ps.hlsl`) writes the frame map on world pixels with samples, and the game's values elsewhere:

- The shadow mask's red channel becomes lerp(1, the sun visibility's average, `sun_strength`), with `outputs` sun and a sun; its green channel stays.
- The diffuse irradiance becomes the game's times `ambient.level` times lerp(1, the occlusion's average, `ao_strength`), with `outputs` occlusion.

A pixel with fewer than 16 samples of a ray takes the edge-aware 4x4 blur of its neighbors' averages that the other passes use, so the first passes do not look noisy; from 16 on its own average, so a finished shot has no blur. Then the passes whose output it wrote, the sun-shadow and contact-shadow passes for the sun and the ambient pass for the occlusion, do not run: the plan marks them `replaced`. On every other frame, with no scene, a scene not yet traced once, a camera that just moved, or `trace.enabled` 0, every raster pass runs as before; with no scene or the trace off, the pass costs one branch at the combine step and a few at present.

### When the scene goes stale

The scene is fixed in the world it was captured in, so the trace stops, `trace_state` reads `STALE`, and the raster passes run until another capture or a release, when:

- The game's floating origin, `b1` row 10 of the shadow-mask constants, moves by more than 1 cm from where it was at the capture: a world shift. Every change of it goes to the log.
- More than 5% of at least 32 rays of a pass's consistency grid met the scene more than max(0.05 m, 1%) from where the depth buffer says, or met nothing: something moved, or the camera sees what the capture did not.

### Saving a frame

`PhotorealSaveTrace` copies the next frame the pass writes and saves it to `<ReShade base path>\Photoreal\trace-<YYYYMMDD-HHMMSS>\`:

| File | Holds |
|---|---|
| `history.f32` | Four float32 a pixel, top row first: the occlusion sum, the sun sum and their counts. |
| `depth.r32`, `stencil.r8`, `normals.r10g10b10a2` | The frame the pass traced from, as stored. |
| `mask.r8g8`, `irradiance.r11g11b10` | The game's shadow mask and diffuse irradiance as the pass found them, before any pass of ours. |
| `constants.bin` | The 160 bytes of the trace's constants, `TraceConstants` in `core/trace.h`. |
| `sun.bin` | The newest shadow-mask constants `b0` to `b3` read back, when there were any. |
| `trace.tsv` | One key and value a line: the size, frame, the scene folder holding `trace-scene.bin`, its generation, BVH hash, triangles and nodes, the full passes, the rays, guard stops, stack overflows and consistency grid since the samples started, the rays a second, the reference, the floating origin, the sun's source and direction, the eye, the pushed camera's matrices, the trace settings and `ambient.level`. |

On the Mac, `tests/check_trace.sh <trace folder> [scene folder]` checks a saved frame: it builds `tests/trace_reference.cpp`, which reads the folder, builds the BVH of the scene folder's `trace-scene.bin` (by default the one `trace.tsv` names, beside the trace folder) with `core/bvh.cpp` and checks its hash against the Deck's, and gives every pixel the samples the Deck's history says it has, with the same constants, sequence and traversal (`shaders/trace.hlsli` compiled as C++), on all cores, into `reference.f32`. `tests/check_trace.py` then checks that every world pixel has the Deck's counts, that the occlusion and sun averages agree within 0.02 on at least 99% of the world pixels, and that no 16x16 tile's mean difference passes 0.005, and writes `check-trace.png` (the Deck's averages, the reference's, and their difference times 10, occlusion above sun). With the game's mask and the shadow-mask constants in the folder, it also reports the share of world pixels inside the game's cascades and shadow distance that the game shadows and the trace lights, which is casters the scene lacks, and the reverse; and how the traced occlusion follows the game's diffuse irradiance, as information.

`ReShade.log` gets a line when the scene is built (triangles in and kept, by reason, the BVH's nodes, build time and hash), when it is ready, at passes 1, 2, 4, 8 and so on and the last (rays so far, rays a second, tiles a frame and their time, guard stops, stack overflows, the consistency grid), when it goes stale or fails, and when a save is written. The speed on the Deck has not been measured yet; the design estimates 10 to 30 million rays a second, 7 to 22 s to 64 occlusion and 32 sun samples at 1920x1200.

## Tests

`tests/run.sh` builds and runs every `*_test.cpp`. `dxbc_test.cpp` reads the output signatures of four vertex and domain shaders and one pixel shader of capture `111824`, which `tests/fixtures/<hash>.<stage>.dxbc` hold as the census dumped them, checks their stages, semantics, registers and masks and the stream-output declaration each gets, and checks that a container cut at every 16-byte boundary, with a bad magic, or with an offset past its end gives nothing. `shaders_test.cpp` checks the census's hash of a fixture and that the shader registry shares bytecode by hash and frees it with its last handle. `scene_test.cpp` projects five world points, one behind the camera, through the G-buffer vertex shaders' own matrix of captures `111824` and `214859`, which `tests/fixtures/<capture>-vs-camera.bin` holds, and checks that the game's camera from the sun fixtures and a pushed camera rebuilt from them place each within 2 mm, that the camera read from `b1` and `b2` alone is the parsed one to the bit and survives cascades that fail `radii-order`, and that a skewed basis or a NaN gives none. It checks the stream-output layout's offsets, reservations, `buffer-full` and hints for a frame of direct, indirect and tessellated draws, and the readback of a cleared buffer: a draw that overran its reservation left out with its hint raised, a draw with unwritten corners left out, instances whose point count is not a multiple of 3, and a culled corner written as zeros kept. It also checks the face check on the camera turned onto each face and 2 degrees past it, the depth check's counts on a 4x4 depth buffer, the verdicts, request parsing, and the schedule's presents for all seven faces, the view alone, unready reads, failed captures, the writer's backlog, the 900-present limit and a moved camera. `scene_files_test.cpp` checks the OBJ and PLY bytes of two triangles that share an edge, a corner at infinity, the triangle limit, the TSV rows and the half-size picture, and writes a synthetic scene, with each game-camera face's `face-N-sun.bin` with the add-on's own writer: a room with a cube in it around a camera, seen from all seven faces by the game's and the pushed camera with ray-cast depth buffers. `check_scene.py` then runs every check on it, so the Mac's checks of a Deck capture are known to work. `trace_scene_test.cpp` checks the traced scene's keys on the 1 mm grid, which copy of a triangle is kept, grass left out, the cap, a face's corners into the reference's frame, the self-test scene, `trace-scene.bin`, and the counts of `tests/fixtures/scene-194351-near.bin`, the 48,894 triangles of build 199's capture within 10 m of its camera without its main character, which `make_trace_fixture.py` wrote and deduplicated itself. `bvh_test.cpp` checks the BVH of that fixture's 14,165 unique triangles: its literal node count, leaves and hash, the same for 1, 3 and 8 threads, its leaves and depth-first numbering, the compute shader's traversal against testing every triangle for 3,000 rays from the fixture's surfaces, and the edge cases: a ray through an edge two triangles share, a ray in a triangle's plane, a triangle reduced to a point, the end of a ray, classes left out, a scene of one triangle, an empty scene and coinciding centroids. When `run.sh` finds build 199's whole deduplicated scene (`PHOTOREAL_TRACE_DATA`, or `.work/photoreal/tracer/scene-194351-tris.bin` in a folder above), `check_bvh.sh` builds its BVH optimized on 1, 3 and 8 threads, prints the times, checks the hashes agree and that an optimized build gives the fixture's hash, checks 200 rays against every triangle, and measures the traversal's rays a second; otherwise it says it skipped. `trace_test.cpp` checks the ray generator's seeds, points and directions, the cosine and cone samplers' means over 100,000 samples, the tiles' Morton order, the schedule's wrapping and stop, the time budget, what starts the samples again, the classes rays see, the sun and floating origin of build 199's view face, the constants, and one dispatch's work on a camera looking down at a floor half under a roof: the occlusion of each ray's hit, the sun blocked, the next sample, the consistency grid with and without the floor, untraced pixels, a pixel with all its samples, and no sun. `trace_files_test.cpp` checks `trace.tsv` and writes and reads back a trace folder. `trace_test.cpp` also writes a synthetic traced frame, a courtyard of three walls with a cube in it seen from inside with the sun low, whose history is the kernel's own over 6 passes; `run.sh` has `trace_reference` trace it again and `check_trace.py` find it equal, and has `check_trace.py` refuse a copy with a block of sun samples flipped. `png_test.cpp` checks the PNG encoder's bytes for a 2x2 image and writes a 300x200 image that `check_png.py` decodes with Python's `zlib`. `sun_test.cpp` parses the shadow-mask draw's four constant buffers from captures `111824` and `214859`, which `tests/fixtures/<capture>-sun-b0.bin` to `b3.bin` hold as the census dumped them, checks their cascades, atlas, matrices, distances and camera against literal values, and breaks one value at a time to check each rejection. `compare_test.cpp` checks the capture's schedule, variant parsing, pixels, flicker and TSV rows. `accumulate_test.cpp` checks which frames clear, add and present, the cat's-eye weight at the center, the corners and its soft edge, the depth read's stored pixel and meters, and parsing of `PhotorealAccumulate` and the lens sample. `recipe_test.cpp` and `plan_test.cpp` replay four FrameCensus captures in `tests/fixtures/`, which hold only event kinds, resource ids, format names and sizes, with each draw's inputs by slot. Three are 1152x720 frames, and `capture-20261010-111824` is a 1920x1200 frame at the user's normal graphics settings. They check the moment and resource of every step, the same frame at 3456x2160 and at 1153x721 with the quarter-size target rounded either way, a frame without the ambient pair, the shadow mask's (1,1,1,0) clear in the same bind or in an earlier one, the forward moment past a draw that samples the normals or draws into another target, the bloom step falling back to the tonemap draw when the bloom draw is missing, draws into a half-size target or comes after the tonemap draw, with each bloom-step pass planned there once and before the tonemap pass, the trace of the draws from the combine step to the tonemap draw, the `gbuffer-done` moment past a decal draw that samples the smoothness and in a frame without the half-size pass, G-buffer restarts, the order of the passes at each step, the status line, the debug label's glyphs, settings and camera parsing, the wetness pass's amount and its shader's puddle, wetness and smoothness formulas, `scatter.hlsli`'s phase, mist density, march spacing, steps, rescaling and result over the scene, and the light shafts' noise lattice.

To add a capture as a fixture:

```sh
python3 tests/make_fixture.py <FrameCensus capture folder> tests/fixtures/<name>.tsv
```
