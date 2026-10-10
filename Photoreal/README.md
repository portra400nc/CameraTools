# CameraTools Photoreal

`CameraToolsPhotoreal.addon64` is a ReShade add-on that works inside Genshin Impact's own D3D11 frame. Each frame it finds the game's G-buffer and lighting buffers by format, size and order, never by shader hash. It can show any of them full screen as a debug view. Once the game has written its G-buffer and before anything reads it, it can make surfaces wet. Before the game's combine pass reads them, it can draw the sun's shadows again with soft edges from the game's own shadow atlas, add contact shadows to them, and replace the world's ambient light. After the game's deferred lighting, it can add the sun light that leaves and grass let through. Before the game's bloom reads the HDR scene, it can add aerial perspective, accumulate the HDR scene of paused frames into an average, and it can draw its own tone map in place of the game's. It can also render one frame with several settings variants and save each as a PNG, so effects can be judged from images. A MelonLoader mod drives it through C exports.

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
| `addon.cpp` | The ReShade shell: events, exports, mailboxes between the C# thread and the render thread, and the capture's file writer thread. |
| `gpu.*` | The D3D11 side: `StateGuard`, `Mirror`, `Scratch`, `Staging`, the sun constants' copies and readback, the wetness, sun-shadow, contact-shadow, ambient, leaves, atmosphere and tonemap passes, snapshots and the debug composite. |
| `shaders/` | HLSL, compiled to DXBC by `build.py` and embedded in the add-on. `cat_eye.hlsli` also compiles as C++ in `core/accumulate.cpp`, and `wet.hlsli` in `core/plan.cpp`, so the tests check the shaders' own arithmetic. |
| `csharp/Photoreal.cs` | The C# binding a MelonLoader mod adds as is. |
| `tests/` | Native tests that replay recorded FrameCensus frames through `core/`. |

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
| `uint32_t PhotorealVersion(void)` | Returns `PHOTOREAL_VERSION`, 5. Version 2 added the sun direction to `PhotorealCamera`, so a binding of version 1 must not call `PhotorealSetCamera`. Version 3 added `PhotorealCompare` and `PhotorealStatus.compare_remaining`. Version 4 added the sun and sky colors to `PhotorealCamera`. Version 5 added its lens sample, `PhotorealSetAccumulate`, `PhotorealDepthAt` and the status's three accumulator fields. `PhotorealCamera` has no size field, so a binding of an earlier version must not call `PhotorealSetCamera`. |
| `uint64_t PhotorealApply(const PhotorealSettings *)` | Copies the whole desired state and returns its generation. `size` says which fields the caller knows, and later fields keep their defaults. |
| `void PhotorealSetCamera(const PhotorealCamera *)` | Unity's `worldToCameraMatrix` and `GL.GetGPUProjectionMatrix(projectionMatrix, false)`, in Unity's column-major memory order, then the world direction toward the sun, the sun light's color in linear RGB times its intensity, and the sky's ambient light in linear RGB, each as four floats. The direction may have any length; zero means no sun. A color component that is negative or not finite reads as 0. The fourth float of the sun direction and the sun color is unused. The fourth float of the sky color is the weather's wetness, from 0 dry to 1 soaked, which the [wetness pass](#wet-surfaces) follows while `wetness.wetness` is 0; it clamps to 0 to 1, and a value that is not finite reads as 0. CameraTools sends 1 while its World tab's weather is rain or a thunderstorm, or, with the game's weather, while the game's sky says it is wet, and else 0, eased over 5 seconds of real time. Then the [lens sample](#frame-accumulator): the sample's x and y, its index, and 1 on the frame to add, else 0; all zero when nothing is accumulated, which is what CameraTools pushes. Call it every frame from the main camera's `onPreCull`, after anything that moves the camera, so the add-on gets the camera that renders. Only one caller should push the camera. |
| `void PhotorealSetAccumulate(const PhotorealAccumulate *)` | The accumulator's mode, generation and cat's eye; see [Frame accumulator](#frame-accumulator). Separate from the settings, so a screenshot can accumulate whatever passes another caller set, and it works while `enabled` is 0. |
| `float PhotorealDepthAt(float u, float v)` | The view-space depth in meters at a screen point, from 0 at the top left to 1, read from a frame after the point was first asked for, or -1 until then. Call it once a frame until it is not negative. |
| `void PhotorealGetStatus(PhotorealStatus *)` | Copies the last frame's status. Set `size` first. |
| `uint32_t PhotorealDescribe(char *, uint32_t)` | Writes the last frame's status line and returns its length, or 0 when the buffer is too small. |
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
| `leaves.enabled` | 0 | The leaves pass. It runs at the `forward` step, after the game's deferred lighting and before its sky, transparent and fog draws. On leaf and grass pixels it adds to the HDR scene the sun light the leaf lets through toward the camera. Those pixels have stencil 129, 136 or 137 and material ID 2 or 15 (leaves) or 3 (grass), where the ID is the G-buffer's rt3 times 255 without bits 6 and 7. The light is linear albedo times the sun color times the sun's visibility in the shadow mask times `strength` times saturate(dot(v, l))^`scatter_sharpness` times saturate(0.5 - 0.5 dot(n, l)), where v points from the camera to the pixel, l toward the sun, and n is the G-buffer normal. So a leaf glows where the camera looks toward the sun through its back, and a leaf lit from the front gets nothing more, because the game's wrapped diffuse already lights the backs of leaves dimly and uncolored. The pass copies the HDR scene into a mirror, draws into it additively and copies it back. The sun comes from `PhotorealSetCamera`, so without a camera the pass skips. A caller built without the block sends a 116-byte struct and gets the defaults. |
| `leaves.strength` | 0.6 | Multiplies the light let through. Clamped to 0 to 4. |
| `leaves.scatter_sharpness` | 4 | The power of the glow's falloff away from the sun: 1 is broad, and higher gathers it closer around the sun. Clamped to 1 to 32. |
| `wetness.enabled` | 0 | The [wetness pass](#wet-surfaces). It runs at the `gbuffer-done` step and makes world pixels wet in the G-buffer, so the game's own lighting, reflections and ambient light see wet surfaces. It needs a camera for the puddles. A caller built without the block sends a 128-byte struct and gets the defaults. |
| `wetness.wetness` | 0 | How wet the world is, from 0 dry to 1 soaked. 0 follows the weather, which `PhotorealSetCamera` carries; any value above 0 replaces it. Clamped to 0 to 1. |
| `wetness.darkening` | 0.35 | How much a soaked surface's albedo darkens, in linear light. Clamped to 0 to 1. |
| `wetness.puddles` | 0.3 | How much of the flat ground puddles cover when soaked, from none at 0 to all of it at 1. Clamped to 0 to 1. |

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

Where the camera push and the render thread's frames line up is unknown, so the caller should hold each sample for 2 frames and mark the first. Whether the add-on takes the push before the frame renders or one frame later, the frame it then adds shows that sample. The accumulator needs the game's bloom step, so it does nothing while the game's bloom is off.

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

The pass runs only when the draw's buffers had those sizes, the last parse passed every check and described an atlas of this frame's size. Otherwise the status line says `sun constants <check>`, such as `sun constants unread` for the first frame or two after the pass is switched on. The readback only gates the pass and gives the cascade count and grid, which change only with the graphics settings. The pass draws with this frame's own copies, so its shadows stay on the atlas while the camera moves.

The first draw reconstructs each world pixel's position exactly as the game's shadow shaders do, from the game's own camera constants, picks the first cascade whose sphere holds it, and projects it into the atlas. In the outer tenth of a cascade's radius, a growing share of pixels take the next cascade, so the blur fades one into the other. A blocker search of 16 point taps finds how far the casters lie above the pixel toward the sun, and 16 bilinear comparison taps (lit where the pixel is at least as near the sun as the atlas) average the visibility over a disc as wide as `light_size` times that distance, at least `min_penumbra`, and at most 24 atlas texels in radius. The taps follow a Vogel disc turned by the 4x4 pattern, so each pixel's pattern is the same every frame. Each tap compares against the receiver's plane, which the depth buffer gives, so a wide penumbra does not shadow a slanted surface with itself. The second draw blurs the result over 4x4 pixels like the contact-shadow pass and writes it to the mask. Pixels past the game's shadow distance or outside every cascade keep the game's value.

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
1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap forward gbuffer-done; ran wetness@gbuffer-done sun-shadows@combine contact-shadows@combine ambient@combine leaves@forward atmosphere@bloom tonemap@tonemap; view normals
1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap forward gbuffer-done; ran ambient@combine; skipped sun-shadows: sun constants unread
1152x720 found gbuffer quarter-shadow ambient-pair combine bloom tonemap forward gbuffer-done; ran ambient@combine; skipped contact-shadows: missing shadow-mask
found nothing; skipped contact-shadows: missing normals depth shadow-mask; skipped ambient: missing normals depth ambient-diffuse
off; error shader
```

The parts are the render size and the steps found, the passes that ran and the step they ran at, each skipped pass with its reason, the debug view, the G-buffer restarts, the accumulator as `accumulating` or `presenting <n> samples`, a comparison capture's progress as `comparing <saved>/<variants>`, and an error. A skip reason is `missing <entries>`, `no <step>`, `no camera`, `sun constants <check>`, or `no texture`. The checks are `unread`, `signature` (the shadow-mask draw's constant buffers had other sizes), `atlas-changed`, and the parse's `not-finite`, `atlas`, `tiles`, `radii-rows`, `radii-order`, `depth-range`, `basis` and `camera`. A view that could not be shown says `missing <entry>` or `no <step>`.

The `forward` step is the first draw into the HDR scene after the combine clear that does not sample the G-buffer normals. Every deferred draw of the game reads them: world, characters and vegetation, and in capture `111824` two more surface classes that also write a second target. Its sky, transparent and fog draws do not. So at that draw the deferred lighting is whole and nothing has been drawn over it yet.

The `gbuffer-done` step is the first draw after the `gbuffer` step that samples the G-buffer's smoothness while the normals are not bound as a target. The game writes the G-buffer in three binds: its main draws, then decals into rt0 to rt4, and decals into rt0, rt1, rt2 and rt4. In every capture the first draw after them that reads the smoothness is a half-size pass that also reads the normals: seq 348 in `111824`, 680 in `214859` and `214907`, and 316 in `201640`. It comes before the ambient pair, the other early reader of the smoothness, so at that draw the G-buffer is whole and nothing has read it yet. A half-size draw before the decals reads only the normals, so the normals could not mark the step. In a frame without the half-size pass, the ambient pair's draw comes first and takes its own step, and `gbuffer-done` falls to the combine step's first draw, after the ambient pair.

The errors are `not-d3d11`, `shader` (our shaders or states failed to create), `texture` (a texture of ours failed to create this frame), and `state`. A `state` error means the game's render target 0, depth view or pixel shader differed after the add-on restored state. The add-on then stays off until the game restarts and writes which binding moved to `ReShade.log`.

`PhotorealStatus` carries the same facts as bit sets (`PHOTOREAL_STEP_*`, `PHOTOREAL_PASS_*`, `PHOTOREAL_ENTRY_*`), plus the frame count, the settings generation the last frame used, the frames since the last camera, `compare_remaining`, the variants of a comparison capture not yet saved, and the accumulator's mode, samples and generation. `armed` is 1 while the add-on follows the game's frame, which it does while `enabled` is on, an accumulation runs, or `PhotorealDepthAt` is being asked.

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

The G-buffer and lighting views show what the game's combine pass reads, after the wetness, sun-shadow, contact-shadow and ambient passes ran, so `albedo`, `specular` and `smoothness` include the wet surfaces and `shadow-mask` includes the soft sun shadows and the contact shadows. `hdr-scene` shows the image just before the game's tone map, after the leaves and atmosphere passes, and `bloom-final` the quarter-size bloom the tone map adds to it. When the view's entry was not found, the screen shows dark magenta diagonal stripes, and the label ends in `MISSING`.

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

## Tests

`tests/run.sh` builds and runs every `*_test.cpp`. `png_test.cpp` checks the PNG encoder's bytes for a 2x2 image and writes a 300x200 image that `check_png.py` decodes with Python's `zlib`. `sun_test.cpp` parses the shadow-mask draw's four constant buffers from captures `111824` and `214859`, which `tests/fixtures/<capture>-sun-b0.bin` to `b3.bin` hold as the census dumped them, checks their cascades, atlas, matrices, distances and camera against literal values, and breaks one value at a time to check each rejection. `compare_test.cpp` checks the capture's schedule, variant parsing, pixels, flicker and TSV rows. `accumulate_test.cpp` checks which frames clear, add and present, the cat's-eye weight at the center, the corners and its soft edge, the depth read's stored pixel and meters, and parsing of `PhotorealAccumulate` and the lens sample. `recipe_test.cpp` and `plan_test.cpp` replay four FrameCensus captures in `tests/fixtures/`, which hold only event kinds, resource ids, format names and sizes, with each draw's inputs by slot. Three are 1152x720 frames, and `capture-20261010-111824` is a 1920x1200 frame at the user's normal graphics settings. They check the moment and resource of every step, the same frame at 3456x2160 and at 1153x721 with the quarter-size target rounded either way, a frame without the ambient pair, the shadow mask's (1,1,1,0) clear in the same bind or in an earlier one, the forward moment past a draw that samples the normals or draws into another target, the `gbuffer-done` moment past a decal draw that samples the smoothness and in a frame without the half-size pass, G-buffer restarts, the order of the passes at each step, the status line, the debug label's glyphs, settings and camera parsing, and the wetness pass's amount and its shader's puddle, wetness and smoothness formulas.

To add a capture as a fixture:

```sh
python3 tests/make_fixture.py <FrameCensus capture folder> tests/fixtures/<name>.tsv
```
