# Camera Tools

## Features

 - Free camera
 - Field of view adjustments
 - Game speed control (pause, slow-motion, speed up)
 - Hide UI for clean shots
 - Smooth camera movement (damping)
 - Graphics presets for screenshots and for performance, every graphics setting in one list, and values past the game's own limits
 - Two resolution slots
 - Configurable hotkeys
 - Controller support (XInput, including the Steam Deck)

## Settings panel

CameraTools draws its own UI in the style of Genshin's, with the game's font. The UI is ready as soon as the game has loaded that font, right after login.

- F10 (Start on a controller) opens the settings panel while the free camera is on. Click or drag with the mouse, or use the controller.
- Button hints follow the device you used last.
- The free camera hides the game's HUD. Hide UI (PageDown, or B) hides CameraTools' hints for a clean shot.
- A tab longer than the panel scrolls. Scroll with the mouse wheel, or move the selection with the controller.

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

Back (View) is a modifier. Hold it for the actions in the right-hand column.

| Controller | Action | Back + controller |
|--|--|--|
| Left stick | Camera movement (analog) | |
| Right stick | Look around | |
| LT / RT | Down / up (analog) | |
| LB / RB | Roll left / right | Slow / fast camera movement (hold) |
| R3 | Reset roll | |
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
| D-pad left / right | Decrease / increase a slider (hold to repeat), flip a switch, change a setting, or step a resolution slot through common sizes |
| A | Flip a switch, run a preset, or apply a resolution slot |
| Y | Type a resolution slot's size |
| LB / RB | Previous / next tab |
| B, or the ToggleGUI button (Start by default) | Close the panel, or cancel typing a size |

Controller bindings are in the `[CameraToolsController]` section of `MelonPreferences.cfg`, under the same names as the hotkeys. A binding is either buttons joined with `+` (`A`, `B`, `X`, `Y`, `LB`, `RB`, `LT`, `RT`, `Back`, `Start`, `L3`, `R3`, `DpadUp`, `DpadDown`, `DpadLeft`, `DpadRight`), for example `Back+A`, or one analog direction: `LeftStickUp`, `LeftStickDown`, `LeftStickLeft`, `LeftStickRight`, `RightStickUp`, `RightStickDown`, `RightStickLeft`, `RightStickRight`, `LT`, or `RT`. An empty binding does nothing. A binding that does not parse falls back to its default, and the log names it.

## Credits
Free camera script by FreyaHolmer: https://gist.github.com/FreyaHolmer/650ecd551562352120445513efa1d952
