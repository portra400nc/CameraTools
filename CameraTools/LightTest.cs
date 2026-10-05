using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // A throwaway test of ReLight sphere lights, before the Lights tab is built. Each press of Back+D-pad right
    // (Numpad 0) shows the next state of the table; Back+D-pad left (Numpad .) removes the sphere. The first press, and
    // the first after a removal, places the sphere 1.5 m in front of the camera, where it is in view. Every state is
    // logged with the frame rate over its first seconds, so a screenshot per state settles what renders.
    internal static class LightTest
    {
        private sealed record Step(string Name, float Radius, float Intensity, float Ambient = 1f, bool Outline = false);

        private static readonly Color White = new(1f, 1f, 1f, 1f);
        private const float FrameRateWindow = 3f;
        private const float PlaceAhead = 1.5f;

        // Build 139 showed a sphere at intensity 5 lights the character brightly from where CameraTools put it, and that
        // the game light's toon softness settings change nothing. These check the position against ReLight's own outline,
        // then the intensity and radius scales.
        private static readonly Step[] Steps =
        {
            new("Sphere 0.5 m, intensity 2, outline shown", 0.5f, 2f, Outline: true),
            new("Sphere 0.5 m, intensity 2", 0.5f, 2f),
            new("Sphere 0.5 m, intensity 5", 0.5f, 5f),
            new("Sphere 0.1 m, intensity 2", 0.1f, 2f),
            new("Sphere 1.5 m, intensity 2", 1.5f, 2f),
            new("Sphere 0.5 m, intensity 2, ambient 0.4", 0.5f, 2f, 0.4f),
        };

        private static readonly PadBinding NextPad = new(PadButtons.Back | PadButtons.DpadRight, PadAxis.None);
        private static readonly PadBinding RemovePad = new(PadButtons.Back | PadButtons.DpadLeft, PadAxis.None);

        private static GameObject holder;
        private static int step = -1;
        private static Vector3 absolutePosition;
        private static Quaternion rotation;
        private static bool placementLogged;
        private static float windowTime;
        private static int windowFrames;
        private static bool windowLogged = true;

        public static void Update()
        {
            bool pad = Controls.Owner == PadOwner.CameraTools && CameraUi.View != View.Panel;
            bool keys = !Controls.TextCapture;
            if (keys && Input.GetKeyDown(KeyCode.Keypad0) || pad && NextPad.Pressed(Gamepad.Current, Gamepad.Previous))
                Next();
            else if (keys && Input.GetKeyDown(KeyCode.KeypadPeriod) || pad && RemovePad.Pressed(Gamepad.Current, Gamepad.Previous))
                Remove("removed", step);
            // A world shift moves the scene; the light keeps its absolute position.
            if (holder)
                holder.transform.position = WorldShift.Relative(absolutePosition);
            if (holder)
                ShowSphere(Steps[step]);
            else
                ReLight.Hide();
            CountFrames();
        }

        private static void ShowSphere(Step sphere)
        {
            var camera = CameraTools.maincam ? CameraTools.maincam : Camera.main;
            if (!camera)
                return;
            var placement = ReLight.Show(camera, holder.transform.position, sphere.Radius, White, sphere.Intensity, sphere.Ambient, sphere.Outline);
            if (placement is not ReLight.Placement p || placementLogged)
                return;
            placementLogged = true;
            Melon<CameraTools>.Logger.Msg($"Light test: {ReLight.Depth}; sphere at uv ({p.U:0.000}, {p.V:0.000}), {p.ViewZ:0.00} m away, "
                + $"linear depth {p.Linear:0.00000}, ReLight z {p.ProjectedZ:0.00}, radius {p.Radius:0.000}, "
                + $"packed {p.Packed.X:X8} {p.Packed.Y:X8} {p.Packed.Z:X8} {p.Packed.W:X8}, ReLight loaded {ReLight.TechniqueLoaded}.");
        }

        private static void Next()
        {
            try
            {
                var camera = CameraTools.camera ? CameraTools.camera.transform : Camera.main?.transform;
                if (!camera)
                {
                    CameraUi.Toast("Light test: no camera found");
                    return;
                }
                if (step == Steps.Length - 1)
                {
                    Remove("finished", -1);
                    return;
                }
                if (!holder)
                {
                    absolutePosition = WorldShift.Absolute(camera.position + camera.forward * PlaceAhead);
                    rotation = camera.rotation;
                }
                step++;
                Build(Steps[step]);
            }
            catch (Exception e)
            {
                Melon<CameraTools>.Logger.Error($"Light test: step {step + 1} failed: {e}");
                CameraUi.Toast($"Light test {step + 1}: failed, see the log");
            }
        }

        // A fresh holder per state, so a switch set by one state cannot leak into the next.
        private static void Build(Step current)
        {
            if (holder)
                UnityEngine.Object.Destroy(holder);
            holder = new GameObject("CameraTools Light Test");
            holder.transform.position = WorldShift.Relative(absolutePosition);
            holder.transform.rotation = rotation;
            string label = $"Light test {step + 1}/{Steps.Length}: {current.Name}";
            placementLogged = false;
            StartWindow();
            CameraUi.Toast(label);
            Melon<CameraTools>.Logger.Msg($"{label}.");
        }

        private static void Remove(string why, int keepStep)
        {
            if (holder)
                UnityEngine.Object.Destroy(holder);
            holder = null;
            step = keepStep;
            StartWindow();
            CameraUi.Toast($"Light test {why}");
            Melon<CameraTools>.Logger.Msg($"Light test {why}.");
        }

        private static void StartWindow()
        {
            windowTime = 0f;
            windowFrames = 0;
            windowLogged = false;
        }

        // ReLight compiles when first switched on, so the frame rate is logged after the window, not from the first frame.
        private static void CountFrames()
        {
            if (windowLogged)
                return;
            windowTime += Time.unscaledDeltaTime;
            windowFrames++;
            if (windowTime < FrameRateWindow)
                return;
            windowLogged = true;
            string state = holder ? Steps[step].Name : "no light";
            Melon<CameraTools>.Logger.Msg($"Light test: {windowFrames / windowTime:0.0} fps over {windowTime:0.0} s with {state}.");
        }
    }
}
