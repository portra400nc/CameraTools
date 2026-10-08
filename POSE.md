# Pose

The Pose tab freezes the active character and lets you pose them for a shot: turn their joints, shape their hands, set their face, and choose where they look. CameraTools saves poses you want to keep, so they are back the next time you play.

## Posing

Posing needs the free camera. Turn on Posing, and the active character holds still in the pose the game had them in. Everything else in the scene keeps moving.

Posing ends when you turn it off, leave the free camera, or switch character. The character then moves again, with their face, eyes and hair as the game had them. Bring back last pose puts back the pose from before posing ended, in case it ended by accident. That last pose is kept until you quit the game, and is not saved.

Back to the game's pose clears every change and keeps the character frozen.

## Saved poses

Save as new pose keeps the current pose as Pose 1, Pose 2, and on. Saved pose picks one, and its note says how many joints it poses and whether the current pose has changes that no saved pose holds. Load pose starts posing if it is off. Save over this pose replaces the picked pose with the current one. Delete pose asks you to press again to confirm.

Poses are saved in `UserData/CameraTools/Poses.json` a second after the last change. A pose keeps expressions and face shapes by their names, so a pose made on one character loads on another. Anything the other character lacks, such as an expression, a face shape or a hair strand, is left out, and the notice says how many parts were left out.

## Body

Each joint turns three ways, in degrees, on top of the frozen pose. Turn around, under the Joint row, decides what the three rows turn about:

| Row | The joint itself | The character |
|--|--|--|
| Bend | Folds the joint, as an elbow or a knee folds. | Turns the joint about the character's side-to-side axis. |
| Turn | Swings the joint to the side. | Turns the joint about the character's up axis. |
| Twist | Spins the joint along its own length. | Turns the joint about the axis from the character's back to their front. |

With The joint itself, the rows follow the joint whichever way it points, so a raised arm still folds, and spins along its own length. New poses start with it. With The character, every joint is measured in the character's own frame, which is how poses saved before Turn around were made, so they load with it. Either way a joint turns the same however the character faces. A positive Bend swings a hanging or upright joint forward, and a knee bends back, so it takes a negative Bend; a collarbone, which points to the side, rises. Switching Turn around changes the numbers, not the pose: CameraTools works out each posed joint's numbers in the other axes, to the nearest degree, so the character looks the same. It needs posing on, so the row is dimmed while posing is off.

A joint carries everything below it: bending the upper arm moves the forearm and the hand with it. On the right side, Turn and Twist go the other way, so equal values on both sides look like mirror images. Copy to the other side gives the joint's mirror the same values. Reset joint puts one joint back as the game posed it.

The Joint row picks a joint, with Bend, Turn and Twist under it. Pose joints is the quicker way: it hides the panel and puts a marker on every joint, with the selected one larger and cream, its name beside it, and its angles at the bottom left. The sticks turn the joint about the axes Turn around picks.

Hair and cloth strands are joints too, at the end of the Joint list, with smaller markers in Pose joints. CameraTools finds them on the character from the game's hair and cloth physics, so each character has their own, named from their bones: `+HairB L L01` is Back hair left, `+SkirtS R A21` is Side skirt right, and `+AmiceB L D01` is Back cloth left. A strand turns from its root, the bone the Joint row's note names, and carries the bones below it. It bends like a hanging limb, and a strand with a twin on the other side has Copy to the other side and the D-pad's jump to it.

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

Each hand starts from a preset: Game's leaves the hand as the game posed it, and Relaxed, Fist, Open, Peace, Point, Thumbs up, OK and Pinch shape every finger. The Finger row picks one finger of either hand, and the rows below it set that finger:

| Row | What it does |
|--|--|
| Curl | Bends all three joints together, from 0, straight, to 100, a closed finger. It shows the three joints' average. |
| Knuckle | Bends the first joint alone. Below 0 bends it back, down to -20. |
| Middle joint | Bends the second joint alone. |
| Tip | Bends the last joint alone, so a finger can hook at the tip with a straight knuckle. |
| Spread | Moves the finger toward the thumb or away from it. |
| Across the palm | Swings the thumb in front of the palm, toward the fingertips, from 0 to 100. |
| Thumb twist | Turns the thumb's pad toward the fingers or away, up to 60° either way. |

Across the palm and Thumb twist are for the thumb, so they are dimmed while another finger is picked. With them, the thumb's tip can meet a fingertip, as OK and Pinch do. The notes under Knuckle, Middle joint and Tip name the bone each one bends. Changing a finger makes its hand Custom, unless the numbers match a preset again. A pose saved before these rows loads with each finger's curl on all three joints, and a hand whose numbers no longer match its preset loads as Custom.

## Face

Expression plays one of the character's own expressions. The list comes from the character, so each character has their own. Mouth, Eyes and Brows set one of the game's face shapes on top of the expression; their notes name the shape. Left eye closed and Right eye closed close each eye, for a wink. Blinking is off while posing, so the eyes stay as set; turn it on to let the character blink now and then.

## Gaze

Eyes look decides where the eyes point. At the camera follows the camera as it moves. Ahead holds them straight ahead from the head. By hand sets them with the two rows below, up to the game's own eye range of 17° left and right and 6.5° up and down. Head set to At the camera turns the head toward the camera, up to 60° to the side and 30° up or down; the Head joint's rows wait while it does.

## Hair

The game's hair physics only holds the hair's shape on a frozen character, so it is off while posing, and a bowed or turned head would swing the hair into the body. Hair follows head sets how much the hair strands turn with the head and the body above them. At 100% the hair moves with the head, as the game holds it. At 0% each hair strand keeps the direction it had before posing. In between, lower values keep the hair off the body when the head bows or turns. A strand turned by hand keeps that turn on top. Cloth strands always move with the body. The row is dimmed for a character with no hair strands.
