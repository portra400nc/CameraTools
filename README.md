# Camera Tools

## Features

 - Free camera
 - Field of view adjustments
 - Game speed control (pause, slow-motion, speed up)
 - Hide UI for clean shots
 - Smooth camera movement (damping)
 - Configurable hotkeys
 - Controller support (XInput, including the Steam Deck)

## Settings panel

CameraTools draws its settings panel, button hints, and field of view bar with Genshin's own photo mode UI. It copies that UI from the game, and the game only creates it once you open photo mode. So open photo mode once after starting the game, then leave it. Until you do, the free camera works, but turning it on shows "Open photo mode once to load the CameraTools UI" and there is no settings panel.

While the free camera is on, press F10 (or Start on a controller) to open the settings panel. It has two tabs. Camera holds movement speed, look sensitivity, roll speed, zoom speed, field of view, damping, and "Remember last position". World holds game speed, pause, max detail, and damage numbers. With a keyboard, the panel frees the cursor, and you click the switches and drag the sliders. Turning on the free camera hides the game's HUD, except damage numbers, which have their own switch. Hide UI (PageDown, or B on a controller) then hides CameraTools' hints and the UID for a clean shot, and pressing it again brings them back. With the free camera off, Hide UI hides the game's HUD and the UID instead.

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
| Equals (=) | Set the screen resolution to 3840x2160
| Minus (-) | Set the screen resolution to 1920x1080
| Home | Toggle max detail (keeps every LOD at its most detailed level)

You can customize the hotkeys by editing `MelonPreferences.cfg` located in `\UserData`. Refer to this list of key codes: https://docs.unity3d.com/ScriptReference/KeyCode.html 

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
| D-pad left / right | Decrease / increase a slider (hold to repeat), or flip a switch |
| A | Flip a switch |
| LB / RB | Previous / next tab |
| B, or the ToggleGUI button (Start by default) | Close the panel |

Controller bindings are in the `[CameraToolsController]` section of `MelonPreferences.cfg`, under the same names as the hotkeys. A binding is either buttons joined with `+` (`A`, `B`, `X`, `Y`, `LB`, `RB`, `LT`, `RT`, `Back`, `Start`, `L3`, `R3`, `DpadUp`, `DpadDown`, `DpadLeft`, `DpadRight`), for example `Back+A`, or one analog direction: `LeftStickUp`, `LeftStickDown`, `LeftStickLeft`, `LeftStickRight`, `RightStickUp`, `RightStickDown`, `RightStickLeft`, `RightStickRight`, `LT`, or `RT`. An empty binding does nothing. A binding that does not parse falls back to its default, and the log names it.

## Credits
Free camera script by FreyaHolmer: https://gist.github.com/FreyaHolmer/650ecd551562352120445513efa1d952
