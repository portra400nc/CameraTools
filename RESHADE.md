# ReShade: depth of field, long exposure, screenshots and DNG

The ReShade tab drives ReShade from the free camera. It needs ReShade and CameraTools' own add-on, `CameraToolsReShadeBridge.addon64`, whose source is in `ReShadeBridge/`. Without them, its rows are dimmed and the tab reads "ReShade bridge not found". If the add-on is older or newer than CameraTools, the tab says which version is loaded and which is needed: copy the add-on from the same release as CameraTools into the folder ReShade loads add-ons from. That is `C:\ReShade`, or the folder that `AddonPath` in `ReShade.ini` names, such as `C:\ReShade\reshade-shaders\Addons`.

ReShade effects turns all of ReShade's effects on or off. Turn it on to see the blur while you compose a shot. Every other row and the focus point also work while it is off, which keeps the frame rate up, and a screenshot turns the effects on by itself. A screenshot turns it off afterwards, unless a sphere light needs it; see [Lights](LIGHTS.md).

## Depth of field

These rows control one of three depth of field shaders:

- CameraTools' own, the default. It needs only the Enhancements add-on, `CameraToolsEnhancements.addon64`, and no ReShade shader. See [CameraTools' depth of field](#cameratools-depth-of-field).
- [Otis_Inf's Cinematic DOF](https://github.com/FransBouma/OtisFX), `CinematicDOF.fx` (free)
  - Use [my fork](https://github.com/portra400nc/OtisFX) for tilt-shift, physical blur and miniature scale.
- [iMMERSE Depth of Field](https://www.martysmods.com/physical-depth-of-field/), `MartysMods_DEPTHOFFIELD.fx` (paid)

The Shader row picks the one the rows drive, and shows while more than one is available. Only the picked one renders: picking another while the depth of field is on switches it on and the first one off, and if two are switched on in ReShade's overlay, CameraTools switches off the one that is not picked. While ReShade compiles its effects, or if the picked shader is not loaded, the rows are dimmed and the section header says why. CameraTools finds Cinematic DOF and iMMERSE by their techniques, so with ReShade's "Load only enabled effects" option on, a shader has to be enabled in the preset to be found.

### CameraTools' depth of field

CameraTools' depth of field blurs the picture inside the game's own frame, from the game's depth buffer, before the game's bloom and its UI. Bright lights keep their full brightness when they blur, the game's HUD and CameraTools' panel never blur, and the blur shows with ReShade's effects off, so it costs no ReShade frame rate. It works only in the free camera. In a screenshot, it runs after the last ReShade effect that reads the depth buffer, such as RTGI, so those effects light the sharp picture and the saved shot is blurred over them.

The blur follows a real lens: the focal length is the free camera's own, on a full-frame camera, and the aperture and focus distance set how much each distance blurs. Something in front of the focus blurs over what lies behind it, which stays sharp right up to its outline.

| Row | What it does |
|--|--|
| Depth of field | Turns it on or off |
| Focus mode | Focus point focuses on what is under the focus point. Manual distance focuses at the Focus distance. Eyes focuses on the active character's nearer eye. |
| Focus speed | How long the focus takes to settle, from 3 s to Instant. 0.5 s by default. Not in Manual distance, which focuses at once. |
| Focus distance | The distance that is in focus, from 0.3 m to 50 m. Manual distance only. |
| Aperture | f/1 to f/22 in third-stops, f/2.8 by default. A lower number blurs more. |
| Focal length | The free camera's field of view as a full-frame focal length. Moving it zooms the camera, as 1 and 2 do. A longer lens blurs more. |
| Show focus point | Draws the focus point on the picture |
| Focus point X, Focus point Y | Where the focus point is, from -1 at the left and the top to 1 at the right and the bottom. Focus point only. |
| Miniature scale | Blurs the scene as if it were a 1:N model, from Off to 1:500. At 1:100 a house 30 m away blurs like a 30 cm model. |
| Max blur | Caps the blur, from 1 to 8 percent of the picture's height, 3 by default. Raise it for strong blur, and with Miniature scale, or the far blur stops growing. |

The **Bokeh** rows shape each blur disc, in front of the focus and behind it:

| Row | What it does |
|--|--|
| Aperture shape | Round, or 5 to 9 blades |
| Blade roundness | From straight blades at 0 to round at 1. With blades only. |
| Blade rotation | Turns the blades, from 0° to 90°. With blades only. |
| Soap bubble | The light across each disc, from -1 to 1, 0.6 by default. Above 0, discs behind the focus get bright rims and discs in front soft centers, as an over-corrected lens draws them; below 0 the other way round. 0 lights them evenly. |
| Chromatic aberration | Color fringes on the discs, from 0 to 5. 1 is a real lens's: green behind the focus and purple in front. Higher values exaggerate it. |
| Cat's eye | Cuts discs toward the picture's corners into cat's eyes, as a fast lens does, from 0 to 1 |
| Swirl | Stretches discs around the picture's center toward its corners, the swirly look of Petzval lenses, from 0 to 1 |
| Anamorphic ratio | Squeezes discs into the tall ovals of anamorphic lenses, from 1 (off) to 2 |

The **Highlights** rows bring out small bright things:

| Row | What it does |
|--|--|
| Highlight discs | Draws small bright lights, glints and sky specks as discs of their own, with smooth edges. Off by default. |
| Highlight boost | How much brighter a small light that stands out of its surroundings gets, from 0 to 4, 2 by default. With Highlight discs only. |
| Highlight threshold | How much brighter than the picture's average a light must be to count, from 1.5 to 4 times, 2 by default. With Highlight discs only. |
| Bokeh contrast | Lets spots that stand out of their surroundings, such as grass tips and leaf glints, blur into visible discs, from 0 (off) to 1 |

In Eyes mode the focus point sits on the iris of the active character's nearer eye, and the focus reads the eye's surface, so the iris is sharp even close up at f/1.2. It changes eyes only when the other one is 3 cm nearer, so it does not flicker while the character faces the camera. When no eye is on screen, it focuses on the focus point until an eye comes back.

These values are kept in the `[CameraToolsEnhancements]` section of `MelonPreferences.cfg`. The Graphics tab's Highest and Lowest presets leave them alone.

The blur comes from the depth buffer, so things that write no depth, such as glass, water surfaces and particles, blur as whatever lies behind them.

### Cinematic DOF and iMMERSE

With the fork, Cinematic DOF blurs like a real lens with the set focal length and aperture. CameraTools gives it the camera's near clip, so its distances are real meters.

| Row | What it does |
|--|--|
| Depth of field | Turns the shader on or off |
| Focus mode | Focus point focuses on what is under the focus point. Manual distance focuses at the Focus distance. |
| Focus speed | How long iMMERSE's autofocus takes to settle, from 2 s to Instant. iMMERSE only. |
| Focus distance | The distance that is in focus, from 0.002 at the camera to 1 at infinity. For a second after you change it, iMMERSE paints the plane that is in focus over the picture, and Cinematic DOF shows its blur instead of the picture: blue where it is in focus, red in front and grey behind. Tilt changes show it too. Manual distance only. |
| Aperture | f/0.95 to f/22 in third-stops. A lower number blurs more. |
| Focal length | 10 to 350 mm on a full-frame camera. A longer lens blurs more. |
| Link focal length to field of view | Keeps Focal length and the free camera's field of view in step, so the blur matches the lens that the field of view stands for. Zooming changes the focal length, and the Focal length row zooms. |
| Show focus point | Draws the focus point on the picture. While it is off, the stick and the click do not move the point, and the focus stays where it was. The X and Y rows still move it. |
| Focus point X, Focus point Y | Where the focus point is, from -1 at the left and the top to 1 at the right and the bottom. Focus point only, or Manual distance while Tilt is on. |
| Miniature scale | Blurs the scene as if it were a 1:N model, from Off to 1:500. At 1:100 a house 30 m away blurs like a 30 cm model. Cinematic DOF only. |
| Max blur | Caps the blur, from 1 to 8. Raise it to 3 or more for strong blur, or the blur stops growing close to the focus. Cinematic DOF only. |
| Tilt | Tilts the plane in focus like a tilt-shift lens, from -15° to 15°, off at 0. Positive keeps the ground sharp from near to far, negative gives the miniature look. The plane turns around the focus point. Far away or at a miniature scale, half a degree is already strong. Cinematic DOF only. |
| Tilt direction | Turns the sharp band, from -90° to 90°. 0 is level, 90 and -90 are upright. Cinematic DOF only. |
| Tangential scale, Sagittal scale | Stretch each bokeh disc around the picture's center or toward it. Tangential gives the swirly look of Petzval lenses. 0 leaves the discs round. iMMERSE only. |
| Anamorphic ratio | Squeezes the bokeh into the tall ovals of anamorphic lenses. 1 is off. |

CameraTools saves your changes to ReShade's current preset: when the panel closes, a second after you last moved the focus point or zoomed with the link on, and before a screenshot. ReShade loads its effects again from the preset whenever the window changes size.

### The focus point

In Focus point mode, and with Cinematic DOF in Manual distance while Tilt is on, CameraTools draws the focus point on the screen while the depth of field is on, also while ReShade effects is off: four corner brackets around the square that the shader measures the distance in, and a cross at its middle. Cinematic DOF measures the one pixel under the cross, so its square is small. In Eyes mode the point sits on the iris, and the stick and the click do not move it. Hide UI hides it, and it is never in a screenshot. Move it in one of these ways:

- Hold Back and move the right stick. Back+R3 puts it back in the middle. The camera does not turn while Back is held.
- Free the cursor with Quote ('), close the panel, and click where to focus.
- Change the Focus point X and Focus point Y rows.

The focus point is a position relative to the screen, so it stays on the same part of the picture when the resolution changes. CameraTools keeps Cinematic DOF's mouse-driven auto-focus off, as it would focus on the cursor instead of the focus point.

## Long exposure

Long exposure makes Take screenshot record for a while, as a camera on a tripod does. Water and clouds smooth out, moving characters blur, and lights draw trails. It uses SirCobra's [RealLongExposure](https://github.com/LordKobra/CobraFX) shader. Install `RealLongExposure.fx` and the `CobraUtility.fxh` beside it into ReShade's shader folder. Until ReShade has loaded the shader, the section header reads "Install RealLongExposure.fx first" and its rows are hidden. CameraTools finds the shader by its technique, so with ReShade's "Load only enabled effects" option on, the shader has to be enabled in the preset to be found.

| Row | What it does |
|--|--|
| Long exposure | Makes Take screenshot record for the shutter time. Off, Take screenshot takes a normal shot. |
| Shutter | How long the picture records, from 1/8 s to 120 s in full stops |
| ISO | 100 keeps the game's brightness. Each step brightens the picture. |
| Highlight boost | How strongly bright lights stay as they move, from 0.4 to 4.4. 1 is plain averaging. |

Shutter, ISO and Highlight boost show only while Long exposure is on. CameraTools keeps them in its own preferences, not in ReShade's preset, and switches the shader on only for the shot, so it costs no frame rate the rest of the time.

The shot runs as any screenshot does, and once the picture has settled it opens the shutter, waits the shutter time, and saves ReShade's average of every frame in that time. The game keeps running while it records: a paused game gives the same picture as a normal shot, and moving the camera blurs the whole picture. Nothing of CameraTools is on screen while it records. A long exposure skips the Enhancements' traced lighting, which pauses the game. If ReShade loads its effects again during the exposure, the screenshot fails and puts everything back.

## Screenshots

Take screenshot takes a screenshot through ReShade with one button.

Take screenshot does this:

1. Closes the panel and hides CameraTools' UI.
2. Applies the Highest preset, your own if you saved one, and resolution slot 1, and turns ReShade's effects on.
3. Counts down 3 seconds, if 3-second countdown is on.
4. Waits until the picture has settled, ReShade has compiled its effects at the new size, and the depth of field has had time to focus. CameraTools' own depth of field waits until its focus has held still for half a second, at most 5 seconds; it renders the shot with more samples than the live view, after ReShade's effects that read the depth buffer. In Focus point mode Cinematic DOF and iMMERSE ease their focus toward the point, and start over when the resolution changes. iMMERSE takes up to 2 seconds, depending on its Adjustment Speed setting. Cinematic DOF moves a share of the way each frame, set by its Auto-focus transition speed, so CameraTools waits as long as that takes at 8 frames per second: about 4 seconds at the default speed. The first screenshot at a size can take many seconds.
5. With Long exposure on, records for the shutter time.
6. Saves the screenshot to ReShade's screenshot folder.
7. Puts the graphics settings, the enhancements and the resolution back as they were, turns ReShade's effects off, opens the panel again, and shows the file's name.

Hide UI (H, or B), opening the settings panel, or leaving the free camera cancels the screenshot and puts everything back. If a wait takes too long, CameraTools gives up, names what it waited for, and puts everything back.

### DNG

Also save DNG makes Take screenshot save a DNG raw file beside the PNG, with the same name, for editing in Lightroom or another raw editor. It needs `CameraToolsEnhancements.addon64`, the add-on that draws the Enhancements.

The DNG holds the picture's light before the game's tone curve, so you have more room to recover highlights and lift shadows, and white balance works as it does on a camera's raw file. It holds:

- The game's picture with the Enhancements, such as ambient light and light shafts.
- CameraTools' depth of field, blurred before the tone curve, so out-of-focus lights keep their brightness.
- The game's bloom and exposure, with the Bloom and Exposure rows of the Enhancements' tone map.
- The lens flare.

It does not hold ReShade's effects, such as RTGI or the blur of Cinematic DOF and iMMERSE, or the tone map's Curve, Saturation, Contrast, Shadows and Highlights rows. You make those choices in the raw editor instead. The PNG is the same with Also save DNG on or off.

The DNG opens with the profile "CameraTools Game", which applies the game's tone curve, so the picture first looks as it does in the game. Lightroom's other profiles, such as Adobe Color, are in its Profile list. White balance As Shot is the game's own color.

The DNG stores 16 bits for each color of each pixel, compressed without loss: about 22 MB at 3840x2160. Its EXIF names the focal length and the date. While CameraTools' depth of field is on, it also names the aperture and the focus distance.

With Supersampling on, the DNG gets the full-precision average of all the samples. A long exposure saves no DNG.

The `[CameraToolsReShade]` section of `MelonPreferences.cfg` holds:

| Entry | Default | Description |
|--|--|--|
| `ScreenshotCountdown` | `true` | Count down 3 seconds before the screenshot. The ReShade tab's switch changes it. |
| `ScreenshotSettleSeconds` | `2` | The least time, in seconds, between changing the graphics settings and taking the screenshot, from 0 to 30 |
| `LinkFocalLengthToFov` | `false` | Keep the depth of field's focal length and the free camera's field of view in step. The ReShade tab's switch changes it. |
| `ShowFocusPoint` | `true` | Draw the focus point on the picture in Focus point mode. The ReShade tab's switch changes it. |
| `DepthOfFieldShader` | `CameraTools` | The depth of field the rows drive: `CameraTools`, `MartysMods_DEPTHOFFIELD.fx` or `CinematicDOF.fx`. The Shader row changes it. |
| `MiniatureScale` | `1` | The Miniature scale row's N: 1 (off), 10, 25, 50, 100, 200 or 500. CameraTools' depth of field and Cinematic DOF share it. |
| `LongExposure` | `false` | Take screenshot records for the shutter time. The ReShade tab's switch changes it. |
| `LongExposureShutter` | `8` | The shutter time in seconds, rounded to the nearest full stop from 1/8 s to 120 s |
| `LongExposureIso` | `100` | 100, 200, 400, 800 or 1600 |
| `LongExposureHighlightBoost` | `1` | From 0.4 to 4.4 |
| `ScreenshotDng` | `false` | Also save a DNG beside each screenshot. The ReShade tab's switch changes it. |
