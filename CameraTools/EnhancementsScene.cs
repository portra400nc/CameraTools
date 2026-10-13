using CameraToolsEnhancements;
using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // Turns the free camera to the cube face a Enhancements scene capture asks for. The add-on publishes the face it wants and
    // accepts a frame only when that frame's own camera shows it, so the free camera only follows. Its own pose is never
    // changed, so it comes back as soon as the add-on asks for no face.
    internal static class EnhancementsScene
    {
        private static EnhancementsFace face = EnhancementsFace.None;

        // The rotation and field of view to show instead of the free camera's, or null while no cube face is asked for,
        // the add-on is not loaded or the free camera is off. The view face keeps the camera as it is.
        public static (Quaternion Rotation, float Fov)? Pose { get; private set; }

        // Once a frame from OnLateUpdate, after Enhancements.Update, so every Freecam.Apply of the frame turns the same way.
        public static void Update(bool freecam)
        {
            var wanted = Enhancements.Connected && freecam ? Enhancements.Status.SceneFace : EnhancementsFace.None;
            if (wanted != face)
            {
                face = wanted;
                Melon<CameraTools>.Logger.Msg(face switch
                {
                    EnhancementsFace.None => "Enhancements scene capture: the free camera's own pose is back.",
                    EnhancementsFace.View => "Enhancements scene capture: capturing the view as it is.",
                    _ => $"Enhancements scene capture: turning the free camera to {face}.",
                });
            }
            Pose = null;
            if (EnhancementsFaces.Direction(face) is { } direction)
                Pose = (Quaternion.LookRotation(direction.Forward, direction.Up), EnhancementsFaces.FieldOfView);
        }
    }
}
