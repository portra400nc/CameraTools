using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // The Eyes focus mode's eyes: the active character's eye bones, found by name once per character, and the nearer one,
    // which the depth of field focuses on. The posing rig finds the same bones, but Eyes runs without it.
    internal static class FocusEyes
    {
        private static readonly string[] Bones = { "+EyeBone L A01", "+EyeBone R A01" };
        // A character whose eyes were not found is searched again this often, as its model may still be loading.
        private const float RetrySeconds = 1f;

        private static readonly Transform[] eyes = new Transform[2];
        private static Transform avatar;
        private static float retryAt;
        private static int current = -1;

        // The eye in focus this frame, or null while the focus point stands in.
        public static EyeSight? Target { get; private set; }

        // Once a frame, with the camera that renders, or null while Eyes is not in use.
        public static void Update(Camera camera)
        {
            if (!camera)
            {
                Target = null;
                current = -1;
                return;
            }
            Find(Character.Active());
            var left = Sight(eyes[0], camera);
            var right = Sight(eyes[1], camera);
            current = Lens.NearerEye(left, right, current);
            Target = current switch { 0 => left, 1 => right, _ => null };
        }

        private static void Find(Transform active)
        {
            if (active == avatar && (eyes[0] || eyes[1] || Time.unscaledTime < retryAt))
                return;
            bool changed = active != avatar;
            avatar = active;
            retryAt = Time.unscaledTime + RetrySeconds;
            current = -1;
            for (int i = 0; i < eyes.Length; i++)
                eyes[i] = active ? Search(active, Bones[i]) : null;
            if (active && (changed || eyes[0] || eyes[1]))
                Melon<CameraTools>.Logger.Msg($"Eyes focus: {active.name} has {(eyes[0] ? "a" : "no")} left and {(eyes[1] ? "a" : "no")} right eye bone.");
        }

        private static Transform Search(Transform node, string name)
        {
            if (node.name == name)
                return node;
            for (int i = 0; i < node.childCount; i++)
                if (Search(node.GetChild(i), name) is { } found)
                    return found;
            return null;
        }

        // Rounded to the millimeter, so an eye that holds still does not send the add-on new settings every frame.
        private static EyeSight? Sight(Transform eye, Camera camera)
        {
            if (!eye || ScreenPoint.Of(camera, eye.position) is not { } point)
                return null;
            var view = camera.transform;
            float meters = Lens.AlongView(eye.position.ToNumerics(), view.position.ToNumerics(), view.forward.ToNumerics());
            return new EyeSight(MathF.Round(meters, 3), point.U * 2f - 1f, point.V * 2f - 1f);
        }
    }
}
