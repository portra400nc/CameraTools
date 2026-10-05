using Il2CppInterop.Runtime;
using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // A throwaway test of how the Lights tab's Shadows switch must set up a point or a spot for Genshin to draw its
    // shadows. Back+D-pad right (Numpad 0) and Back+D-pad left (Numpad .) step through the ways; each rebuilds the lights
    // that have shadows on, and two seconds later the log records what the engine reports about local light shadows.
    internal static class ShadowTest
    {
        private sealed record Way(string Name, LightShadows Shadows, int? UpdateFrequency = null, bool FullShadows = false, bool Copy = false);

        private static readonly Way[] Ways =
        {
            new("Soft shadows", LightShadows.Soft),
            new("Soft shadows, update frequency 0", LightShadows.Soft, 0),
            new("Soft shadows, update frequency 1", LightShadows.Soft, 1),
            new("Soft shadows, update frequency 1, full shadows", LightShadows.Soft, 1, true),
            new("Hard shadows, update frequency 1, full shadows", LightShadows.Hard, 1, true),
            new("A copy of the game's own shadow lamp", LightShadows.Soft, Copy: true),
        };

        private static readonly PadBinding NextPad = new(PadButtons.Back | PadButtons.DpadRight, PadAxis.None);
        private static readonly PadBinding PreviousPad = new(PadButtons.Back | PadButtons.DpadLeft, PadAxis.None);
        private const float ReportDelay = 2f;

        private static float? reportAt;
        private static bool lampsLogged;

        public static int Variant { get; private set; }

        public static void Update()
        {
            bool pad = Controls.Owner == PadOwner.CameraTools && CameraUi.View is View.Hud;
            bool keys = !Controls.TextCapture;
            if (keys && Input.GetKeyDown(KeyCode.Keypad0) || pad && NextPad.Pressed(Gamepad.Current, Gamepad.Previous))
                Step(1);
            else if (keys && Input.GetKeyDown(KeyCode.KeypadPeriod) || pad && PreviousPad.Pressed(Gamepad.Current, Gamepad.Previous))
                Step(-1);
            if (reportAt is float at && Time.unscaledTime >= at)
            {
                reportAt = null;
                Report();
            }
        }

        // For a light with Shadows on, after its other settings.
        public static void Apply(Light light)
        {
            var way = Ways[Variant];
            if (way.Copy)
                return;
            light.shadows = way.Shadows;
            light.shadowStrength = 1f;
            if (way.UpdateFrequency is int frequency)
                light.set_shadowUpdateFrequency(frequency);
            if (way.FullShadows)
                light.set_dummyFullShadowsEnabled(true);
        }

        // A copy of the nearest game light that casts shadows, or null when this way does not copy or none is near.
        public static GameObject CloneTemplate()
        {
            if (!Ways[Variant].Copy || Lamps().FirstOrDefault() is not { } lamp)
                return null;
            var copy = UnityEngine.Object.Instantiate(lamp.gameObject.Cast<UnityEngine.Object>()).TryCast<GameObject>();
            if (copy)
                Melon<CameraTools>.Logger.Msg($"Shadow test: copied {Path(lamp.transform)}; {Describe(copy.GetComponent<Light>())}; components {Components(copy)}.");
            return copy;
        }

        private static void Step(int direction)
        {
            Variant = (Variant + direction + Ways.Length) % Ways.Length;
            Lights.Rebuild();
            string label = $"Shadow test {Variant + 1}/{Ways.Length}: {Ways[Variant].Name}";
            CameraUi.Toast(label);
            Melon<CameraTools>.Logger.Msg($"{label}.");
            reportAt = Time.unscaledTime + ReportDelay;
            if (lampsLogged)
                return;
            lampsLogged = true;
            var log = Melon<CameraTools>.Logger;
            var lamps = Lamps().ToList();
            log.Msg($"Shadow test: {lamps.Count} game lights near the camera cast shadows.");
            foreach (var lamp in lamps.Take(8))
                log.Msg($"  {Path(lamp.transform)}: {Describe(lamp)}; components {Components(lamp.gameObject)}.");
        }

        private static void Report()
        {
            var ours = UnityEngine.Object.FindObjectsOfType(Il2CppType.Of<Light>())
                .Select(found => found.TryCast<Light>())
                .Where(light => light && light.name == Lights.ObjectName)
                .ToList();
            Melon<CameraTools>.Logger.Msg($"Shadow test {Variant + 1}: the engine reports {RuntimeProfiler.ShadowBakedLocalLights} shadow-baked local lights, "
                + $"{RuntimeProfiler.GetShadowPixels()} shadow pixels, shadow ratio {RuntimeProfiler.GetShadowRatio():0.###}; "
                + $"CameraTools has {ours.Count} game lights: {string.Join("; ", ours.Select(Describe))}.");
        }

        // The game's own lights that cast shadows, nearest to the camera first.
        private static IEnumerable<Light> Lamps()
        {
            var camera = Lights.Camera();
            if (!camera)
                return Enumerable.Empty<Light>();
            var from = camera.transform.position;
            return UnityEngine.Object.FindObjectsOfType(Il2CppType.Of<Light>())
                .Select(found => found.TryCast<Light>())
                .Where(light => light && light.name != Lights.ObjectName && light.shadows != LightShadows.None && light.type != LightType.Directional)
                .OrderBy(light => Vector3.Distance(light.transform.position, from));
        }

        private static string Path(Transform transform) => transform.parent ? $"{transform.parent.name}/{transform.name}" : transform.name;

        private static string Components(GameObject owner)
            => string.Join(", ", owner.GetComponents(Il2CppType.Of<Component>()).Select(component => component.ToString()));

        private static string Describe(Light light)
        {
            if (!light)
                return "no light";
            return $"type {light.type}, shadows {light.shadows}, strength {light.shadowStrength:0.##}, layer {light.gameObject.layer}, "
                + $"culling 0x{light.cullingMask:X}, proxy shadow mask {light.useProxyShadowMask}, distant shadow bound {light.distantShadowBoundThresh:0.##}, "
                + $"near plane offset {light.shadowNearPlaneOffsetOverride:0.###}, kodama mask 0x{light.kodamaShadowVisibilityMask:X}, "
                + $"intensity {light.intensity:0.##}, range {light.range:0.#}, spot {light.spotAngle:0.#}";
        }
    }
}
