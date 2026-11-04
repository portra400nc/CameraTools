using Il2CppInterop.Runtime;
using MelonLoader;
using MelonLoader.Utils;
using UnityEngine;

namespace CameraTools
{
    // Test mode: the game's own lights near the camera beside a saved trace, for the Mac's fit of how Genshin's local
    // lights fall off.
    internal static class TracedLights
    {
        private const float Reach = 60f;

        public static void Save()
        {
            try
            {
                var camera = CameraTools.maincam;
                if (!camera)
                    return;
                var eye = camera.transform.position;
                var lights = UnityEngine.Object.FindObjectsOfType(Il2CppType.Of<Light>()).Select(o => o.Cast<Light>())
                    .Where(light => Vector3.Distance(light.transform.position, eye) <= Reach).ToList();
                string folder = Path.Combine(MelonEnvironment.UserDataDirectory, "CameraTools");
                Directory.CreateDirectory(folder);
                string path = Path.Combine(folder, $"traced-lights-{DateTime.Now:yyyyMMdd-HHmmss}.tsv");
                var m = camera.worldToCameraMatrix;
                var rows = new List<string>
                {
                    $"camera\t{eye.x}\t{eye.y}\t{eye.z}",
                    "world_to_camera\t" + string.Join("\t", new[] { m.m00, m.m01, m.m02, m.m03, m.m10, m.m11, m.m12, m.m13, m.m20, m.m21, m.m22, m.m23, m.m30, m.m31, m.m32, m.m33 }
                        .Select(v => v.ToString("R"))),
                    "path\ttype\tenabled\tx\ty\tz\tforward_x\tforward_y\tforward_z\tr\tg\tb\tintensity\trange\tspot\tinner\tshadows\tculling\tcharacter_culling",
                };
                foreach (var light in lights)
                {
                    var t = light.transform;
                    var p = t.position;
                    var f = t.forward;
                    var c = light.color;
                    string name = (t.parent ? t.parent.name + "/" : "") + light.name;
                    rows.Add(string.Join("\t", name.Replace('\t', ' '), light.type, light.isActiveAndEnabled ? 1 : 0, p.x, p.y, p.z, f.x, f.y, f.z, c.r, c.g, c.b,
                        light.intensity, light.range, light.spotAngle, light.spotInnerAngle, light.shadows, light.cullingMask, light.characterLightCullingMask));
                }
                File.WriteAllLines(path, rows);
                Melon<CameraTools>.Logger.Msg($"Traced shot: saved {lights.Count} lights within {Reach:0} m of the camera to {path}.");
            }
            catch (Exception e)
            {
                Melon<CameraTools>.Logger.Warning($"Traced shot: the lights could not be saved: {e.Message}");
            }
        }
    }
}
