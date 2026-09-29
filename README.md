# Camera Tools

## Features

 - Free camera
 - Field of view adjustments
 - Game speed control (pause, slow-motion, speed up)
 - HUD Toggle
 - Smooth camera movement (damping)
 - Configurable hotkeys
 - Controller support (XInput, including the Steam Deck)

## Hotkeys
| Key | Description |
|--|--|
| F9 | Inject free camera
| F10 | Toggle GUI (while in free camera only)
| Quote (') | Focus cursor
| PageDown | Toggle HUD
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
| B | Toggle HUD | Toggle damage overlay |
| X | Toggle pause | Remove enemy hp overlay |
| Y | Reset game speed | Toggle game speed to 5.0 |
| Start | Toggle GUI | Toggle max detail |

Controller bindings are in the `[CameraToolsController]` section of `MelonPreferences.cfg`, under the same names as the hotkeys. A binding is either buttons joined with `+` (`A`, `B`, `X`, `Y`, `LB`, `RB`, `LT`, `RT`, `Back`, `Start`, `L3`, `R3`, `DpadUp`, `DpadDown`, `DpadLeft`, `DpadRight`), for example `Back+A`, or one analog direction: `LeftStickUp`, `LeftStickDown`, `LeftStickLeft`, `LeftStickRight`, `RightStickUp`, `RightStickDown`, `RightStickLeft`, `RightStickRight`, `LT`, or `RT`. An empty binding does nothing. A binding that does not parse falls back to its default, and the log names it.

## Credits
Free camera script by FreyaHolmer: https://gist.github.com/FreyaHolmer/650ecd551562352120445513efa1d952
