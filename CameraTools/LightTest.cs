using Il2CppInterop.Runtime;
using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // A throwaway test of custom lights, before the Lights tab is designed. Each press of Back+D-pad right (Numpad 0)
    // rebuilds one light at the pose where the test started, in the next state of the table; Back+D-pad left (Numpad .)
    // removes it. Every state is logged with what the game reports, so a screenshot per state settles what renders.
    internal static class LightTest
    {
        private sealed record Step(string Name, Action<Light> Apply);

        private static readonly Color White = new(1f, 1f, 1f, 1f);
        private static readonly Color Red = new(1f, 0.15f, 0.1f, 1f);
        private static readonly Color Cyan = new(0.2f, 0.9f, 1f, 1f);
        private static readonly Color Magenta = new(1f, 0.2f, 0.9f, 1f);

        private static readonly Step[] Steps =
        {
            new("Point, white, intensity 2", light => Point(light, White, 2f)),
            new("Point, white, intensity 8", light => Point(light, White, 8f)),
            new("Point, red, intensity 8", light => Point(light, Red, 8f)),
            new("Spot, white, 40 degrees", light => Spot(light)),
            new("Spot, white, soft shadows", light =>
            {
                Spot(light);
                light.shadows = LightShadows.Soft;
                light.shadowStrength = 1f;
            }),
            new("Point, red, characters only", light =>
            {
                Point(light, Red, 8f);
                light.set_isCharacterLight(true);
                light.characterLightCullingMask = uint.MaxValue;
                light.characterIntensityMultiplier = 1f;
            }),
            new("Rim light, cyan front, magenta back", light =>
            {
                Point(light, White, 8f);
                light.set_isCharacterLight(true);
                light.characterLightCullingMask = uint.MaxValue;
                light.set_isRimLight(true);
                light.rimWidth = 1f;
                light.frontRimColor = Cyan;
                light.frontRimIntensity = 2f;
                light.backRimColor = Magenta;
                light.backRimIntensity = 2f;
            }),
            new("Point, white, main local light", light =>
            {
                Point(light, White, 8f);
                light.mainLocalLight = true;
            }),
        };

        private static readonly PadBinding NextPad = new(PadButtons.Back | PadButtons.DpadRight, PadAxis.None);
        private static readonly PadBinding RemovePad = new(PadButtons.Back | PadButtons.DpadLeft, PadAxis.None);

        private static GameObject holder;
        private static int step = -1;
        private static Vector3 absolutePosition;
        private static Quaternion rotation;
        private static bool referenceLogged;

        public static void Update()
        {
            bool pad = Controls.Owner == PadOwner.CameraTools && CameraUi.View != View.Panel;
            bool keys = !Controls.TextCapture;
            if (keys && Input.GetKeyDown(KeyCode.Keypad0) || pad && NextPad.Pressed(Gamepad.Current, Gamepad.Previous))
                Next();
            else if (keys && Input.GetKeyDown(KeyCode.KeypadPeriod) || pad && RemovePad.Pressed(Gamepad.Current, Gamepad.Previous))
                Remove("removed");
            // A world shift moves the scene; the light keeps its absolute position.
            if (holder)
                holder.transform.position = WorldShift.Relative(absolutePosition);
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
                    Remove("finished");
                    return;
                }
                if (step < 0)
                {
                    absolutePosition = WorldShift.Absolute(camera.position);
                    rotation = camera.rotation;
                    if (!referenceLogged)
                    {
                        referenceLogged = true;
                        LogGameLights(camera.position);
                    }
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

        // A fresh light per state, so a switch set by one state cannot leak into the next.
        private static void Build(Step current)
        {
            if (holder)
                UnityEngine.Object.Destroy(holder);
            int before = CountLights();
            holder = new GameObject("CameraTools Light Test");
            holder.transform.position = WorldShift.Relative(absolutePosition);
            holder.transform.rotation = rotation;
            var light = holder.AddComponent<Light>();
            current.Apply(light);
            string label = $"Light test {step + 1}/{Steps.Length}: {current.Name}";
            CameraUi.Toast(label);
            Melon<CameraTools>.Logger.Msg($"{label}. Lights in GetLights before {before}, after {CountLights()}. {Describe(light)}");
        }

        private static void Remove(string why)
        {
            if (holder)
                UnityEngine.Object.Destroy(holder);
            holder = null;
            step = -1;
            CameraUi.Toast($"Light test {why}");
            Melon<CameraTools>.Logger.Msg($"Light test {why}.");
        }

        private static void Point(Light light, Color color, float intensity)
        {
            light.type = LightType.Point;
            light.color = color;
            light.intensity = intensity;
            light.range = 8f;
        }

        private static void Spot(Light light)
        {
            light.type = LightType.Spot;
            light.color = White;
            light.intensity = 8f;
            light.range = 15f;
            light.spotAngle = 40f;
            light.spotInnerAngle = 25f;
        }

        private static int CountLights()
        {
            try
            {
                return Light.GetLights(LightType.Point, 0).Length + Light.GetLights(LightType.Spot, 0).Length;
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Light test: Light.GetLights failed ({e.Message}).");
                return -1;
            }
        }

        // The game's own lights near the camera, as reference values for intensity, range and the character switches.
        private static void LogGameLights(Vector3 from)
        {
            var lights = UnityEngine.Object.FindObjectsOfType(Il2CppType.Of<Light>())
                .Select(found => found.TryCast<Light>())
                .Where(light => light)
                .ToList();
            var logger = Melon<CameraTools>.Logger;
            logger.Msg($"Light test: the scene has {lights.Count} active lights: "
                + string.Join(", ", lights.GroupBy(light => light.type).Select(group => $"{group.Count()} {group.Key}")) + ".");
            foreach (var light in lights.OrderBy(light => Vector3.Distance(light.transform.position, from)).Take(30))
            {
                var parent = light.transform.parent;
                logger.Msg($"  {(parent ? parent.name + "/" : "")}{light.name} at {Vector3.Distance(light.transform.position, from):0.0} m: {Describe(light)}");
            }
        }

        private static string Describe(Light light)
        {
            var c = light.color;
            return $"type {light.type}, enabled {light.isActiveAndEnabled}, layer {light.gameObject.layer}, "
                + $"color ({c.r:0.##}, {c.g:0.##}, {c.b:0.##}), intensity {light.intensity:0.##}, range {light.range:0.##}, "
                + $"spot {light.spotAngle:0.#}/{light.spotInnerAngle:0.#}, shadows {light.shadows}, "
                + $"culling 0x{light.cullingMask:X}, character culling 0x{light.characterLightCullingMask:X}, "
                + $"character multiplier {light.characterIntensityMultiplier:0.##}, main local {light.mainLocalLight}, "
                + $"specular wrap {light.specularWrap:0.##}, rim group {light.rimLightGroup}";
        }
    }
}
