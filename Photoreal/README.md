# CameraTools Photoreal

`CameraToolsPhotoreal.addon64` is a ReShade add-on that works inside Genshin Impact's own D3D11 frame. Each frame it finds the game's G-buffer and lighting buffers by format, size and order, never by shader hash. It can show any of them full screen as a debug view, and it can replace the world's ambient light before the game's combine pass reads it. A MelonLoader mod drives it through C exports.

## Layout

| Path | Contents |
|---|---|
| `photoreal.h` | The C interface: exports, settings, camera, status, bit constants. |
| `core/recipe.*` | The game's frame as a table of steps (`kRecipe`) and the `FrameTracker` that matches events against it. Pure C++17. |
| `core/plan.*` | Settings parsing, our passes (`kPasses`) and views (`kViews`), the per-step plan, the frame report and its status line. Pure C++17. |
| `addon.cpp` | The ReShade shell: events, exports, mailboxes between the C# thread and the render thread. |
| `gpu.*` | The D3D11 side: `StateGuard`, `Mirror`, `Scratch`, the ambient pass, snapshots and the debug composite. |
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
| `uint32_t PhotorealVersion(void)` | Returns `PHOTOREAL_VERSION`, 1. |
| `uint64_t PhotorealApply(const PhotorealSettings *)` | Copies the whole desired state and returns its generation. `size` says which fields the caller knows, and later fields keep their defaults. |
| `void PhotorealSetCamera(const PhotorealCamera *)` | Unity's `worldToCameraMatrix` and `GL.GetGPUProjectionMatrix(projectionMatrix, false)`, in Unity's column-major memory order. Call it every frame from the main camera's `onPreCull` while Photoreal is on. |
| `void PhotorealGetStatus(PhotorealStatus *)` | Copies the last frame's status. Set `size` first. |
| `uint32_t PhotorealDescribe(char *, uint32_t)` | Writes the last frame's status line and returns its length, or 0 when the buffer is too small. |

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

## Status line

`PhotorealDescribe` reports the frame in one line with no frame counter, so a mod can log it when it changes. For example:

```
1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap; ran ambient@combine; view normals
1152x720 found gbuffer quarter-shadow shadow-mask combine bloom tonemap; skipped ambient: missing ambient-diffuse
found nothing; skipped ambient: missing normals depth ambient-diffuse
off; error shader
```

The parts are the render size and the steps found, the passes that ran and the step they ran at, each skipped pass with its reason, the debug view, the G-buffer restarts, and an error. A skip reason is `missing <entries>`, `no <step>`, `no camera`, or `no texture`. A view that could not be shown says `missing <entry>` or `no <step>`.

The errors are `not-d3d11`, `shader` (our shaders or states failed to create), `texture` (a texture of ours failed to create this frame), and `state`. A `state` error means the game's render target 0, depth view or pixel shader differed after the add-on restored state. The add-on then stays off until the game restarts and writes which binding moved to `ReShade.log`.

`PhotorealStatus` carries the same facts as bit sets (`PHOTOREAL_STEP_*`, `PHOTOREAL_PASS_*`, `PHOTOREAL_ENTRY_*`), plus the frame count, the settings generation the last frame used, and the frames since the last camera.

## Debug views

A view is copied at its step and drawn over the back buffer at `reshade_present`, after ReShade's effects and overlay, so ReShade screenshots do not include it.

| View | Shows |
|---|---|
| `normals`, `albedo`, `specular` | The G-buffer target's stored values as color. |
| `material-id` | The low six bits of the material ID, one color per ID. |
| `smoothness`, `character`, `shadow-mask`, `quarter-shadow` | The red channel as gray. |
| `depth` | Reversed depth: near is bright, the sky is black. |
| `stencil` | Sky black, world gray, grass green, characters magenta, vegetation dark green, foliage light green, any other value red. |
| `ambient-diffuse`, `ambient-specular`, `hdr-scene` | HDR values, tone mapped. |

The G-buffer and lighting views show what the game's combine pass reads, after the ambient pass ran. `hdr-scene` shows the image just before the game's tone map. When the view's entry was not found, the screen shows dark magenta diagonal stripes.

## Tests

`tests/run.sh` builds and runs `recipe_test.cpp` and `plan_test.cpp`. They replay four FrameCensus captures in `tests/fixtures/`, which hold only event kinds, resource ids, format names and sizes. Three are 1152x720 frames, and `capture-20261010-111824` is a 1920x1200 frame at the user's normal graphics settings. They check the moment and resource of every step, the same frame at 3456x2160 and at 1153x721 with the quarter-size target rounded either way, a frame without the ambient pair, the shadow mask's (1,1,1,0) clear in the same bind or in an earlier one, G-buffer restarts, the status line, and settings parsing.

To add a capture as a fixture:

```sh
python3 tests/make_fixture.py <FrameCensus capture folder> tests/fixtures/<name>.tsv
```
