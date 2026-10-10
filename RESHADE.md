# ReShade: depth of field and screenshots

The ReShade tab drives ReShade from the free camera. It needs ReShade and CameraTools' own add-on, `CameraToolsReShadeBridge.addon64`, whose source is in `ReShadeBridge/`. Without them, its rows are dimmed and the tab reads "ReShade bridge not found". If the add-on is older or newer than CameraTools, the tab says which version is loaded and which is needed: copy the add-on from the same release as CameraTools into the folder ReShade loads add-ons from. That is `C:\ReShade`, or the folder that `AddonPath` in `ReShade.ini` names, such as `C:\ReShade\reshade-shaders\Addons`.

ReShade effects turns all of ReShade's effects on or off. Turn it on to see the blur while you compose a shot. Every other row and the focus point also work while it is off, which keeps the frame rate up, and a screenshot turns the effects on by itself. A screenshot turns it off afterwards, unless a sphere light needs it; see [Lights](LIGHTS.md).

## Depth of field

These rows control iMMERSE's depth of field shader, `MartysMods_DEPTHOFFIELD.fx`. The shader must be installed in ReShade. While ReShade compiles its effects, or if the shader is missing, the rows are dimmed and the section header says why.

| Row | What it does |
|--|--|
| Depth of field | Turns the shader on or off |
| Focus mode | Focus point focuses on what is under the focus point. Manual distance focuses at the Focus distance. |
| Focus distance | The distance that is in focus, from 0.002 at the camera to 1 at infinity. For a second after you change it, the shader paints the plane that is in focus over the picture. Manual distance only. |
| Aperture | f/0.95 to f/22 in third-stops. A lower number blurs more. |
| Focal length | 10 to 350 mm on a full-frame camera. A longer lens blurs more. |
| Link focal length to field of view | Keeps Focal length and the free camera's field of view in step, so the blur matches the lens that the field of view stands for. Zooming changes the focal length, and the Focal length row zooms. |
| Show focus point | Draws the focus point on the picture. While it is off, the stick and the click do not move the point, and the focus stays where it was. The X and Y rows still move it. |
| Focus point X, Focus point Y | Where the focus point is, from -1 at the left and the top to 1 at the right and the bottom. Focus point only. |
| Tangential scale, Sagittal scale | Stretch each bokeh disc around the picture's centre or toward it. Tangential gives the swirly look of Petzval lenses. 0 leaves the discs round. |
| Anamorphic ratio | Squeezes the bokeh into the tall ovals of anamorphic lenses. 1 is off. |

In Focus point mode, CameraTools draws the focus point on the screen while the depth of field is on, also while ReShade effects is off: four corner brackets around the square that the shader measures the distance in, and a cross at its middle. Hide UI hides it, and it is never in a screenshot. Move it in one of these ways:

- Hold Back and move the right stick. Back+R3 puts it back in the middle. The camera does not turn while Back is held.
- Free the cursor with Quote ('), close the panel, and click where to focus.
- Change the Focus point X and Focus point Y rows.

The focus point is a position relative to the screen, so it stays on the same part of the picture when the resolution changes.

CameraTools saves your changes to ReShade's current preset: when the panel closes, a second after you last moved the focus point or zoomed with the link on, and before a screenshot. ReShade loads its effects again from the preset whenever the window changes size.

## Screenshots

Take screenshot takes a screenshot through ReShade with one button.

Take screenshot does this:

1. Closes the panel and hides CameraTools' UI.
2. Applies the Max quality preset and resolution slot 1, and turns ReShade's effects on.
3. Counts down 3 seconds, if 3-second countdown is on.
4. Waits until the picture has settled, ReShade has compiled its effects at the new size, and the depth of field has had time to focus. In Focus point mode the shader eases its focus toward the point, in up to 2 seconds depending on its Adjustment Speed setting, and starts over when the resolution changes. The first screenshot at a size can take many seconds.
5. Saves the screenshot to ReShade's screenshot folder.
6. Puts the graphics settings and the resolution back as they were, turns ReShade's effects off, opens the panel again, and shows the file's name.

Hide UI (PageDown, or B), opening the settings panel, or leaving the free camera cancels the screenshot and puts everything back. If a wait takes too long, CameraTools gives up, names what it waited for, and puts everything back.

The `[CameraToolsReShade]` section of `MelonPreferences.cfg` holds:

| Entry | Default | Description |
|--|--|--|
| `ScreenshotCountdown` | `true` | Count down 3 seconds before the screenshot. The ReShade tab's switch changes it. |
| `ScreenshotSettleSeconds` | `2` | The least time, in seconds, between changing the graphics settings and taking the screenshot, from 0 to 30 |
| `LinkFocalLengthToFov` | `false` | Keep the depth of field's focal length and the free camera's field of view in step. The ReShade tab's switch changes it. |
| `ShowFocusPoint` | `true` | Draw the focus point on the picture in Focus point mode. The ReShade tab's switch changes it. |
| `PhotorealDepthOfField` | `false` | Take screenshots with [lens depth of field](#lens-depth-of-field) in place of the shader's blur. |
| `LensSamples` | `96` | How many points of the aperture a lens depth of field screenshot averages, from 1 to 1024. |
| `LensBlades` | `0` | Aperture blades, from 3 to 16, for polygonal bokeh. 0 is a round aperture. |
| `LensBladeRotation` | `0` | How far the bladed aperture is turned, in degrees. |
| `CatEye` | `0` | Cat's-eye bokeh toward the picture's edges, from 0 (round everywhere) to 1. Try 0.4 for a swirl. |

## Lens depth of field

With `PhotorealDepthOfField` on, a screenshot blurs the way a real lens does instead of through the shader. It needs the Photoreal add-on, `CameraToolsPhotoreal.addon64`, whose source is in `Photoreal/`. Without it, the screenshot button says so and takes no screenshot. The settings have no rows in the panel yet: change them in `MelonPreferences.cfg`.

The screenshot renders the paused shot once for each point of the camera's aperture. For each point it moves the camera inside the aperture and tilts the view so that the plane in focus stays where it is on the screen. What lies nearer or farther moves a little between the points, and the add-on averages every point's picture before the game's bloom and tone map. Bright highlights out of focus become true bokeh, as wide as a full-frame camera's at the same field of view and f-number.

After step 4 of the screenshot, these steps come before the screenshot is saved:

1. Turns the shader's depth of field off. Before the settings settle, it also turns the game's anti-aliasing off, because FSR 2 would smear the moving camera.
2. Finds the focus distance. In Focus point mode it is the depth under the focus point, or under the middle of the screen while the shader is not loaded. In Manual distance mode it is the Focus distance row's distance.
3. Shows each aperture point for 2 frames. The camera and the controls hold still meanwhile. 96 points take 192 frames.
4. Centres the camera and has the add-on show the average.

The aperture is the Aperture row's f-number, or f/2.8 while the shader is not loaded. The focal length follows the field of view as if Link focal length to field of view were on, whatever the Focal length row says. Finishing or cancelling puts the camera, the shader and the anti-aliasing back.
