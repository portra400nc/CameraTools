# Camera Tools

## Features

 - Free camera
 - Field of view adjustments
 - Game speed control (pause, slow-motion, speed up)
 - Hide UI for clean shots
 - Smooth camera movement (damping)
 - Change graphics settings and resolution while in free cam
 - Change the time of day + time-lapse
 - Override the weather with sunny, cloudy, rain, thunderstorm, snow, or mist
 - Camera paths: record views as nodes and fly the camera through them
 - One-button screenshots through ReShade, at max quality with effects on
 - Depth of field through ReShade, with a focus point you move on the screen
 - Configurable hotkeys
 - Controller support (XInput, including the Steam Deck)

## Settings panel

CameraTools draws its own UI in the style of Genshin's, with the game's font. The UI is ready as soon as the game has loaded that font, right after login.

- F10 (Start on a controller) opens the settings panel while the free camera is on. Click or drag with the mouse, or use the controller.
- Button hints follow the device you used last.
- The free camera hides the game's HUD. Hide UI (PageDown, or B) hides CameraTools' hints for a clean shot.
- A tab longer than the panel scrolls. Scroll with the mouse wheel, or move the selection with the controller.
- The tab bar scrolls sideways when the tabs do not fit. LB and RB bring the selected tab into view. With the mouse, scroll the wheel over the tabs.

## Camera paths

Record camera positions as nodes, and fly the free camera through them in one smooth shot.

[Make your first camera path](CAMERA-PATHS.md)

## ReShade

The ReShade tab drives ReShade from the free camera. It needs ReShade and version 2 of the `GenshinReShadeBridge.addon64` add-on. Without them, its rows are dimmed and the tab reads "ReShade bridge not found".

ReShade effects turns all of ReShade's effects on or off. Turn it on to see the blur while you compose a shot. Every other row and the focus point also work while it is off, which keeps the frame rate up, and a screenshot turns the effects on by itself. A screenshot turns it off afterwards.

### Depth of field

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

In Focus point mode, CameraTools draws the focus point on the screen while the depth of field is on, also while ReShade effects is off: four corner brackets around the square that the shader measures the distance in, and a cross at its middle. Hide UI hides it, and it is never in a screenshot. Move it in one of these ways:

- Hold Back and move the right stick. Back+R3 puts it back in the middle. The camera does not turn while Back is held.
- Free the cursor with Quote ('), close the panel, and click where to focus.
- Change the Focus point X and Focus point Y rows.

The focus point is a position relative to the screen, so it stays on the same part of the picture when the resolution changes.

CameraTools saves your changes to ReShade's current preset: when the panel closes, a second after you last moved the focus point or zoomed with the link on, and before a screenshot. ReShade loads its effects again from the preset whenever the window changes size.

### Screenshots

Take screenshot takes a screenshot through ReShade with one button.

Take screenshot does this:

1. Closes the panel and hides CameraTools' UI.
2. Applies the Max quality preset and resolution slot 1, and turns ReShade's effects on.
3. Counts down 3 seconds, if 3-second countdown is on.
4. Waits until the picture has settled and ReShade has compiled its effects at the new size. The first screenshot at a size can take many seconds.
5. Saves the screenshot to ReShade's screenshot folder.
6. Puts the graphics settings and the resolution back as they were, turns ReShade's effects off, opens the panel again, and shows the file's name.

Hide UI (PageDown, or B), opening the settings panel, or leaving the free camera cancels the screenshot and puts everything back. If a wait takes too long, CameraTools gives up, names what it waited for, and puts everything back.

The `[CameraToolsReShade]` section of `MelonPreferences.cfg` holds:

| Entry | Default | Description |
|--|--|--|
| `ScreenshotCountdown` | `true` | Count down 3 seconds before the screenshot. The ReShade tab's switch changes it. |
| `ScreenshotSettleSeconds` | `2` | The least time, in seconds, between changing the graphics settings and taking the screenshot, from 0 to 30 |
| `LinkFocalLengthToFov` | `false` | Keep the depth of field's focal length and the free camera's field of view in step. The ReShade tab's switch changes it. |

## Hotkeys
| Key | Description |
|--|--|
| F9 | Inject free camera
| F10 | Open or close the settings panel (while in free camera only)
| Quote (') | Focus cursor
| PageDown | Hide UI (CameraTools' UI with the free camera on, the game's HUD with it off)
| ] | Toggle damage overlay
| [ | Remove enemy hp overlay
| Insert | Toggle free camera
| I/J/K/L | Camera movement
| O/U | Vertical camera movement
| Comma (,) | Roll camera left
| Period (.) | Roll camera right
| RShift | Reset z rotation (roll)
| Semicolon (;) | Slow camera movement (hold)
| RAlt | Fast camera movement (hold)
| 8 | Decrease field of view
| 9 | Increase field of view
| 0 | Reset field of view
| Delete | Toggle pause
| End | Reset game speed
| Up | Increase game speed by 0.1
| Down | Decrease game speed by 0.1
| Left | Decrease game speed by 0.5
| Right | Increase game speed by 0.5
| CapsLock | Toggle game speed to 5.0
| Equals (=) | Apply resolution slot 2 (2560x1600 unless you change it)
| Minus (-) | Apply resolution slot 1 (1280x800 unless you change it)
| Home | Toggle max detail (keeps every LOD at its most detailed level)
| Numpad + | Add a node at the end of the selected camera path (starts a path if there is none)
| Numpad Enter | Play or stop the selected camera path
| Numpad 1-9 | Select camera path 1 to 9 (during playback, play that path from its start)
| Left click | Put the depth of field's focus point there (cursor free, panel closed)
| None | Put the focus point back in the middle (`CenterFocusPoint`)

You can customize the hotkeys by editing `MelonPreferences.cfg` located in `\UserData`. Refer to this list of key codes: https://docs.unity3d.com/ScriptReference/KeyCode.html 

The `=` and `-` hotkeys keep their old names, `SetResolutionTo4K` and `SetResolutionTo1080p`, so bindings you already changed keep working.

While you type a resolution, the hotkeys are off, so typing 8 or 9 does not change the field of view.

## Graphics preferences

The `[CameraToolsGraphics]` section of `MelonPreferences.cfg` holds:

| Entry | Default | Description |
|--|--|--|
| `ResolutionSlot1` | `1280x800` | The size the `-` hotkey and Slot 1 apply, as WxH |
| `ResolutionSlot2` | `2560x1600` | The size the `=` hotkey and Slot 2 apply, as WxH |
| `SavedSettings` | empty | Your game settings from before CameraTools first changed one, as setting number=option index, for Restore. CameraTools writes and empties it. |

A slot from 320x200 to 16384x16384 is accepted. A slot that does not parse falls back to its default, and the log names it.

## Controller

CameraTools reads an XInput controller, such as the Steam Deck's built-in controls. The game owns the controller until you press L3+R3 (both sticks) together. That hands the controller to CameraTools: it injects and turns on the free camera if needed, stops the game from reading the controller, rumbles, and shows "Controller: CameraTools" at the top of the screen. Press L3+R3 again to give the controller back to the game. The free camera stays where it is, so you can walk the character through a fixed shot. The L3+R3 switch cannot be rebound.

Back (View) is a modifier. Hold it for the actions in the right-hand column. While Back is held, LT and RT do not move the camera down or up, so Back+LT and Back+RT do not also move it. Back with the right stick is fixed and cannot be rebound.

| Controller | Action | Back + controller |
|--|--|--|
| Left stick | Camera movement (analog) | |
| Right stick | Look around | Move the depth of field's focus point, while it shows |
| LT / RT | Down / up (analog) | Add a camera path node / play or stop the camera path |
| LB / RB | Roll left / right | Slow / fast camera movement (hold) |
| R3 | Reset roll | Put the focus point back in the middle |
| D-pad up / down | Increase / decrease field of view | Increase / decrease game speed by 0.5 |
| D-pad left / right | Decrease / increase game speed by 0.1 | |
| A | Toggle free camera | Reset field of view |
| B | Hide UI | Toggle damage overlay |
| X | Toggle pause | Remove enemy hp overlay |
| Y | Reset game speed | Toggle game speed to 5.0 |
| Start | Open or close the settings panel | Toggle max detail |

While the settings panel is open, the controller drives the panel instead of the camera. These buttons are fixed and cannot be rebound.

| Controller | Settings panel |
|--|--|
| D-pad up / down, or left stick up / down | Select the previous / next setting (hold to repeat) |
| D-pad left / right | Decrease / increase a slider (hold to repeat), flip a switch, change a setting, browse camera paths and nodes, or step a resolution slot through common sizes |
| A | Flip a switch, run a preset or a camera path action, or apply a resolution slot |
| Y | Type a resolution slot's size |
| LB / RB | Previous / next tab |
| B, or the ToggleGUI button (Start by default) | Close the panel, or cancel typing a size |

Controller bindings are in the `[CameraToolsController]` section of `MelonPreferences.cfg`, under the same names as the hotkeys. A binding is either buttons joined with `+` (`A`, `B`, `X`, `Y`, `LB`, `RB`, `LT`, `RT`, `Back`, `Start`, `L3`, `R3`, `DpadUp`, `DpadDown`, `DpadLeft`, `DpadRight`), for example `Back+A`, or one analog direction: `LeftStickUp`, `LeftStickDown`, `LeftStickLeft`, `LeftStickRight`, `RightStickUp`, `RightStickDown`, `RightStickLeft`, `RightStickRight`, `LT`, or `RT`. An empty binding does nothing. A binding that does not parse falls back to its default, and the log names it.

## Credits
Free camera script by FreyaHolmer: https://gist.github.com/FreyaHolmer/650ecd551562352120445513efa1d952
