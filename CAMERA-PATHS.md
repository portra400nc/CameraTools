# Make your first camera path

A camera path moves the free camera for you, so you can record smooth shots that are hard to fly by hand. You save a few camera positions along the way, and CameraTools flies the camera through them on a smooth curve. It turns and zooms smoothly from one position to the next. Use it for fly-throughs, slow reveals, and orbits around a scene, including a frozen one while the game is paused. You can keep as many paths as you like, and each one has its own length in seconds.

In this tutorial we record three camera positions and fly the camera through them in one smooth shot. It takes about five minutes. The steps give the controller button first and the keyboard key in brackets.

You need the free camera working. The [README](README.md) covers setup and the full list of controls.

## Record the nodes

A node is one saved camera position. It keeps where the camera is, where it looks, and its field of view.

1. Turn on the free camera. On the controller, press L3+R3. On the keyboard, press F9, then Insert.
2. Fly to where the shot should start, and frame the view.
3. Press Back+LT (Numpad +).

   The notice "Node 1 added to path 1" appears at the top.

4. Fly to a second spot, and press Back+LT (Numpad +) again.

   The notice says "Node 2 added to path 1".

5. Fly to a third spot, and add one more node.

## Play the path

1. Press Back+RT (Numpad Enter).

   The screen counts down 3, 2, 1. Then CameraTools' UI hides, and the camera flies through all three nodes. It stops at the last node, and the notice "Playback finished" appears.

2. To stop a path before it ends, press Back+RT (Numpad Enter) again.

## Adjust the shot

1. Open the settings panel with Start (F10), and go to the **Paths** tab with LB or RB.

   Under the **Path** row, the note says "3 nodes · 10.0 s".

2. Set **Duration** to how many seconds the whole flight takes.
3. To fix one node, pick it in the **Node** row, then select **Go to node**.

   The camera jumps to that node.

4. Frame a better view, then select **Replace with current view**.
5. To add a handheld look, set **Movement strength** and **Rotation strength** to 0.5.
6. Close the panel with B, and play the path again.

## Next steps

- **Loop** plays the path over and over.
- **Ease in** and **Ease out** start and end the flight gently.
- **Constant speed** moves the camera at an even speed, however far apart the nodes are.
- **Insert before this node** and **Insert after this node** add a node in the middle of a path.
- **New path** starts another path. Numpad 1 to 9 pick paths 1 to 9.
- **Delete path** asks you to press again within 3 seconds. **Delete node** does not ask.
- A path plays even while the game is paused. Turn on **Unpause game while playing** to let the world move during the shot.
- Turn off **Hide UI while playing** to see the time and a tick for each node while the path plays.

CameraTools saves your paths in `UserData/CameraTools/CameraPaths.json` after every change, so they are still there after a restart. If CameraTools cannot read the file, it renames the file to `CameraPaths.json.bak` and starts with no paths.
