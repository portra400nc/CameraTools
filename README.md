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
 - Custom lights: points, spots and soft sphere lights through ReShade, for the world or only characters
 - Character posing: freeze the active character, turn their joints, shape their hands and face, and save poses
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

## Lights

Add point, spot and sphere lights to a shot, aim them with the sticks, and let them follow the camera or the character.

[Light a shot](LIGHTS.md)

## Pose

Freeze the active character, turn their joints with the sticks, shape their hands, set their expression and gaze, and save poses for later.

[Pose a character](POSE.md)

## ReShade

The ReShade tab drives ReShade from the free camera. It controls iMMERSE's depth of field, with a focus point you move on the screen, and it takes a screenshot at max quality with one button.

[Use ReShade from CameraTools](RESHADE.md)

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
| `HideOutlines` | `false` | Hides the outlines the game draws around characters, NPCs and weapons. The Graphics tab's Outlines switch sets it. Presets and the screenshot button leave it as it is. |

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

## Building

CameraTools has two parts: the mod, `CameraTools.dll`, and its ReShade add-on, `CameraToolsReShadeBridge.addon64`, which the ReShade tab and the sphere lights need. The mod and the add-on check each other's version, so build and install them together.

### The mod

You need the .NET SDK, version 6 or newer, and a custom MelonLoader install: the game folder whose `MelonLoader/net6` and `MelonLoader/Il2CppAssemblies` hold the references.

1. Build the mod, with `GameDir` set to that folder:

   ```sh
   dotnet build CameraTools -c Release -p:GameDir=<game folder>
   ```

   You can also set `GameDir` in the environment instead. If the references are not where `GameDir` points, the build stops and says which file is missing.

2. Copy `CameraTools/bin/Release/CameraTools.dll` to the game's `Mods` folder.

### The ReShade add-on

The add-on's source is in `ReShadeBridge/`. You need Python 3 and LLVM's `clang-cl` and `lld-link`. The build script downloads ReShade's API headers at the version the add-on is written for.

On macOS or Linux:

1. Install LLVM. On macOS, run `brew install llvm lld`.
2. Install [xwin](https://github.com/Jake-Shadle/xwin), and use it to download the Windows SDK and C runtime. This accepts Microsoft's license for them.

   ```sh
   xwin --accept-license splat --output <sdk folder>
   ```

3. Build the add-on:

   ```sh
   python3 ReShadeBridge/build.py --sdk <sdk folder>
   ```

On Windows, install LLVM, open a Visual Studio developer prompt, and run `python ReShadeBridge/build.py`. The prompt supplies the Windows SDK.

The add-on lands in `ReShadeBridge/bin/`. Copy it to the folder ReShade loads add-ons from: the ReShade folder, or the folder that `AddonPath` in `ReShade.ini` names. Run `python3 ReShadeBridge/build.py --help` for the other options.

The add-on's tests play its request tables against a fake ReShade, and need only a C++17 compiler:

```sh
ReShadeBridge/tests/run.sh
```

## Credits
Free camera script by FreyaHolmer: https://gist.github.com/FreyaHolmer/650ecd551562352120445513efa1d952
