# Pose

The Pose tab freezes the active character and lets you pose them for a shot: turn their joints, shape their hands, set their face, and choose where they look. CameraTools saves poses you want to keep, so they are back the next time you play.

## Posing

Posing needs the free camera. Turn on Posing, and the active character holds still in the pose the game had them in. Everything else in the scene keeps moving.

Posing ends when you turn it off, leave the free camera, or switch character. The character then moves again, with their face, eyes and hair as the game had them. Bring back last pose puts back the pose from before posing ended, in case it ended by accident. That last pose is kept until you quit the game, and is not saved.

Back to the game's pose clears every change and keeps the character frozen.

## Saved poses

Save as new pose keeps the current pose as Pose 1, Pose 2, and on. Saved pose picks one, and its note says how many joints it poses and whether the current pose has changes that no saved pose holds. Load pose starts posing if it is off. Save over this pose replaces the picked pose with the current one. Delete pose asks you to press again to confirm.

Poses are saved in `UserData/CameraTools/Poses.json` a second after the last change. A pose keeps expressions and face shapes by their names, so a pose made on one character loads on another. Anything the other character lacks, such as an expression or a face shape, is left out, and the notice says how many parts were left out.

## Body

Each joint turns three ways, in degrees, on top of the frozen pose:

| Row | What it does |
|--|--|
| Bend | Swings the joint forward, or back with a negative value. A knee bends back, so it takes a negative Bend. |
| Turn | Turns the joint about the character's up axis. |
| Twist | Turns the joint about the axis from the character's back to their front. |

The angles are measured in the character's own frame, so a joint turns the same way however the character faces. A joint carries everything below it: bending the upper arm moves the forearm and the hand with it. On the right side, Turn and Twist go the other way, so equal values on both sides look like mirror images. Copy to the other side gives the joint's mirror the same values. Reset joint puts one joint back as the game posed it.

The Joint row picks a joint, with Bend, Turn and Twist under it. Pose joints is the quicker way: it hides the panel and puts a marker on every joint, with the selected one larger and cream, its name beside it, and its angles at the bottom left.

| Controller | Keyboard | In Pose joints |
|--|--|--|
| D-pad up and down | Up and Down arrows | Previous and next joint |
| D-pad left and right | Left and Right arrows | The same joint on the other side |
| Left stick | I and K, J and L | Bend, and turn |
| LB and RB | U and O | Twist |
| Y | Backspace | Reset the joint |
| X | M | Copy it to the other side |
| A | Enter | Keep the pose |
| B | Escape | Cancel, putting back the pose from before |

With the mouse, click near a marker to pick its joint. The keyboard uses the free camera's movement keys, so I, K, J, L, U and O follow your bindings. The free camera holds still while you pose joints.

## Hands

Each hand starts from a preset: Game's leaves the hand as the game posed it, and Relaxed, Fist, Open, Peace, Point and Thumbs up shape every finger. The Finger row picks one finger of either hand. Curl closes it from 0, straight, to 100, a closed finger, and Spread moves it toward the thumb or away from it. Changing a finger makes its hand Custom, unless the numbers match a preset again.

## Face

Expression plays one of the character's own expressions. The list comes from the character, so each character has their own. Mouth, Eyes and Brows set one of the game's face shapes on top of the expression; their notes name the shape. Left eye closed and Right eye closed close each eye, for a wink. Blinking is off while posing, so the eyes stay as set; turn it on to let the character blink now and then.

## Gaze

Eyes look decides where the eyes point. At the camera follows the camera as it moves. Ahead holds them straight ahead from the head. By hand sets them with the two rows below, up to the game's own eye range of 17° left and right and 6.5° up and down. Head set to At the camera turns the head toward the camera, up to 60° to the side and 30° up or down; the Head joint's rows wait while it does.

## Hair

The game's hair physics shakes on a posed head, so hair holds its shape while posing. It falls into place for half a second when posing starts, and after Load pose, Back to the game's pose and Bring back last pose. Let hair settle lets it fall again, for example after you turn the head.
