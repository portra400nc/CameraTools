using MelonLoader;
using UnityEngine;
using Vec3 = System.Numerics.Vector3;

namespace CameraTools
{
    // The Eyes focus mode's eyes: the active character's eye bones, found by name once per character with the axis of each
    // that points out of the face, and the iris of the nearer one, which the depth of field focuses on. An eye bone is the
    // eyeball's center inside the head, which from above shows over the brow; EyeCtrl turns the bones with the gaze, so
    // the axis follows the look. The posing rig finds the same bones, but Eyes runs without it.
    internal static class FocusEyes
    {
        private static readonly string[] Bones = { "+EyeBone L A01", "+EyeBone R A01" };
        // A character whose eyes were not found is searched again this often, as its model may still be loading.
        private const float RetrySeconds = 1f;

        private static readonly Transform[] eyes = new Transform[2];
        private static readonly Vec3[] gazes = new Vec3[2];
        private static Transform avatar;
        private static float retryAt;
        private static int current = -1;

        // The iris in focus this frame, or null while the focus point stands in.
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
            float separation = eyes[0] && eyes[1] ? Vector3.Distance(eyes[0].position, eyes[1].position) : 0f;
            var left = Sight(0, separation, camera);
            var right = Sight(1, separation, camera);
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
            {
                eyes[i] = active ? Search(active, Bones[i]) : null;
                if (eyes[i])
                    gazes[i] = Lens.FacingAxis(eyes[i].InverseTransformDirection(active.forward).ToNumerics());
            }
            if (active && (changed || eyes[0] || eyes[1]))
                Melon<CameraTools>.Logger.Msg($"Eyes focus: {active.name} has {Describe(0, "left")} and {Describe(1, "right")}.");
        }

        private static string Describe(int eye, string side) => eyes[eye] ? $"a {side} eye bone looking along {Lens.AxisName(gazes[eye])}" : $"no {side} eye bone";

        private static Transform Search(Transform node, string name)
        {
            if (node.name == name)
                return node;
            for (int i = 0; i < node.childCount; i++)
                if (Search(node.GetChild(i), name) is { } found)
                    return found;
            return null;
        }

        // Rounded to the millimeter and to a thousandth of the screen, so an eye that holds still does not send the add-on
        // new settings every frame.
        private static EyeSight? Sight(int index, float separation, Camera camera)
        {
            var eye = eyes[index];
            if (!eye)
                return null;
            var iris = Lens.Iris(eye.position.ToNumerics(), eye.TransformDirection(gazes[index].ToUnity()).ToNumerics(), separation);
            if (ScreenPoint.Of(camera, iris.ToUnity()) is not { } point)
                return null;
            var view = camera.transform;
            float meters = Lens.AlongView(iris, view.position.ToNumerics(), view.forward.ToNumerics());
            return new EyeSight(MathF.Round(meters, 3), MathF.Round(point.U * 2f - 1f, 3), MathF.Round(point.V * 2f - 1f, 3));
        }
    }
}
