# CameraTools Photoreal

`CameraToolsPhotoreal.addon64` is a ReShade add-on that works inside Genshin Impact's own D3D11 frame. Each frame it finds the game's G-buffer and lighting buffers by format, size and order, never by shader hash. It can show any of them full screen as a debug view. Before the game's combine pass reads them, it can add contact shadows to the sun's shadow mask and replace the world's ambient light. It can also render one frame with several settings variants and save each as a PNG, so effects can be judged from images. A MelonLoader mod drives it through C exports.

## Layout

| Path | Contents |
|---|---|
| `photoreal.h` | The C interface: exports, settings, camera, status, bit constants. |
| `core/recipe.*` | The game's frame as a table of steps (`kRecipe`) and the `FrameTracker` that matches events against it. Pure C++17. |
| `core/plan.*` | Settings parsing, our passes (`kPasses`) and views (`kViews`), the per-step plan, the frame report and its status line. Pure C++17. |
| `core/compare.*` | The comparison capture's variants, its schedule of presents (`compare_tick`), back buffer pixels, flicker and TSV rows. Pure C++17. |
| `core/png.*` | A minimal PNG encoder: 8-bit RGB with stored deflate blocks, so the add-on needs no compression library. Pure C++17. |
| `addon.cpp` | The ReShade shell: events, exports, mailboxes between the C# thread and the render thread, and the capture's file writer thread. |
| `gpu.*` | The D3D11 side: `StateGuard`, `Mirror`, `Scratch`, `Staging`, the contact-shadow and ambient passes, snapshots and the debug composite. |
| `shaders/` | HLSL, compiled to DXBC by `build.py` and embedded in the add-on. |
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
| `uint32_t PhotorealVersion(void)` | Returns `PHOTOREAL_VERSION`, 4. Version 2 added the sun direction to `PhotorealCamera`, so a binding of version 1 must not call `PhotorealSetCamera`. Version 3 added `PhotorealCompare` and `PhotorealStatus.compare_remaining`. Version 4 added the sun and sky colors to `PhotorealCamera`, which has no size field, so a binding of an earlier version must not call `PhotorealSetCamera`. |
| `uint64_t PhotorealApply(const PhotorealSettings *)` | Copies the whole desired state and returns its generation. `size` says which fields the caller knows, and later fields keep their defaults. |
| `void PhotorealSetCamera(const PhotorealCamera *)` | Unity's `worldToCameraMatrix` and `GL.GetGPUProjectionMatrix(projectionMatrix, false)`, in Unity's column-major memory order, then the world direction toward the sun, the sun light's color in linear RGB times its intensity, and the sky's ambient light in linear RGB, each as four floats with the last unused. The direction may have any length; zero means no sun. A color component that is negative or not finite reads as 0. Call it every frame from the main camera's `onPreCull` while Photoreal is on. |
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

## Status line

`PhotorealDescribe` reports the frame in one line with no frame counter, so a mod can log it when it changes. For example:

```
1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap; ran contact-shadows@combine ambient@combine; view normals
1152x720 found gbuffer quarter-shadow ambient-pair combine bloom tonemap; ran ambient@combine; skipped contact-shadows: missing shadow-mask
found nothing; skipped contact-shadows: missing normals depth shadow-mask; skipped ambient: missing normals depth ambient-diffuse
off; error shader
```

The parts are the render size and the steps found, the passes that ran and the step they ran at, each skipped pass with its reason, the debug view, the G-buffer restarts, a comparison capture's progress as `comparing <saved>/<variants>`, and an error. A skip reason is `missing <entries>`, `no <step>`, `no camera`, or `no texture`. A view that could not be shown says `missing <entry>` or `no <step>`.

The errors are `not-d3d11`, `shader` (our shaders or states failed to create), `texture` (a texture of ours failed to create this frame), and `state`. A `state` error means the game's render target 0, depth view or pixel shader differed after the add-on restored state. The add-on then stays off until the game restarts and writes which binding moved to `ReShade.log`.

`PhotorealStatus` carries the same facts as bit sets (`PHOTOREAL_STEP_*`, `PHOTOREAL_PASS_*`, `PHOTOREAL_ENTRY_*`), plus the frame count, the settings generation the last frame used, the frames since the last camera, and `compare_remaining`, the variants of a comparison capture not yet saved.

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

The G-buffer and lighting views show what the game's combine pass reads, after the contact-shadow and ambient passes ran, so `shadow-mask` includes the contact shadows. `hdr-scene` shows the image just before the game's tone map, and `bloom-final` the quarter-size bloom the tone map adds to it. When the view's entry was not found, the screen shows dark magenta diagonal stripes, and the label ends in `MISSING`.

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

`tests/run.sh` builds and runs every `*_test.cpp`. `png_test.cpp` checks the PNG encoder's bytes for a 2x2 image and writes a 300x200 image that `check_png.py` decodes with Python's `zlib`. `compare_test.cpp` checks the capture's schedule, variant parsing, pixels, flicker and TSV rows. `recipe_test.cpp` and `plan_test.cpp` replay four FrameCensus captures in `tests/fixtures/`, which hold only event kinds, resource ids, format names and sizes, with each draw's inputs by slot. Three are 1152x720 frames, and `capture-20261010-111824` is a 1920x1200 frame at the user's normal graphics settings. They check the moment and resource of every step, the same frame at 3456x2160 and at 1153x721 with the quarter-size target rounded either way, a frame without the ambient pair, the shadow mask's (1,1,1,0) clear in the same bind or in an earlier one, G-buffer restarts, the order of the passes at combine, the status line, the debug label's glyphs, and settings and camera parsing.

To add a capture as a fixture:

```sh
python3 tests/make_fixture.py <FrameCensus capture folder> tests/fixtures/<name>.tsv
```
