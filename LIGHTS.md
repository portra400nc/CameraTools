# Lights

The Lights tab adds lights of your own to a shot. Lights show while the free camera is on, and CameraTools saves them, so they are back the next time you play.

## Kinds of light

| Type | What it is |
|--|--|
| Point | A light of the game's own that shines in every direction. It can light everything or only characters. |
| Spot | A game light that shines in a cone where it faces. |
| Sphere | A soft light with a size, drawn by ReShade's iMMERSE ReLight instead of the game. It casts soft shadows and lights everything around it. |

A light that lights only characters leaves the ground and the scenery as they are, so it works as a key or fill light for portraits. Its intensity stops at 3, because 1 is already a soft fill and 3 is very bright. Lights of everything reach characters only weakly.

The game's lights cast no shadows and have no size. A sphere has both: a bigger radius makes it softer, not brighter.

## Placing a light

New light places a light 1.5 m in front of the camera, facing where the camera looks. Follow decides what the light moves with:

- Nothing: it stays where it was placed.
- Camera: it moves and turns with the camera.
- Character: it keeps its place beside the active character as she moves and turns.

Move light hides the panel. The left stick moves the light across the ground as the camera faces, LT and RT lower and raise it, and the right stick turns it, which aims a spot. A keeps the new place and B puts the light back. On the keyboard, the free camera's movement keys move it, the mouse turns it, Enter keeps it and Escape cancels.

Move in front of camera puts the light 1.5 m in front of the camera again.

Each light has a marker on the screen: a dot of its colour with its number, ringed in cream for the selected light, with a line for a spot's aim. ReLight also draws a see-through ball where a sphere is. Show light markers turns them off. Hide UI hides them, and they are never in a screenshot.

## Colour

Colour from Temperature sets the colour in kelvin, from candle light at 1000 K to an overcast blue at 12000 K. Colour from Hue sets it by hue and saturation. Intensity sets the brightness. Type a colour into Hex as six hex digits, like FFB46B, with the keyboard or with Steam+X on the Deck.

## Spheres and ReShade

Spheres need ReShade with the `GenshinReShadeBridge.addon64` add-on, and iMMERSE's `MartysMods_RELIGHT.fx` and `MartysMods_LAUNCHPAD.fx` shaders. ReLight takes its normals from Launchpad. Do not load ReLight's own add-on, `MartysMods_ReLightAddon.addon64`: it writes the same lights as CameraTools, and the two would fight.

While a sphere exists, CameraTools turns ReShade's effects on and runs only ReLight and Launchpad, so your other effects cost no frame rate while you compose. A screenshot turns your own effects back on beside ReLight, and switches them off again after the shot. Your ReShade preset keeps your own choice of effects throughout. When the last sphere goes, or the free camera turns off, everything is put back as it was.

ReLight works on the picture on the screen, so a sphere lights only while its place is in view. Ambient light, in the Spheres section, dims the game's own light below 1, so the spheres take over the scene.
