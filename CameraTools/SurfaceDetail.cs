using CameraToolsEnhancements;
using Il2CppInterop.Runtime;
using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // Surface detail, a probe: the Enhancements add-on blends a rock detail map into the G-buffer where rock is drawn. It
    // knows rock by pixel shader, which Learn finds: a census of the G-buffer draws by shader, then another with every
    // renderer of a rock material (miHoYo/Terrain/BlendStone) hidden for a few frames; the shaders that drop out draw rock.
    internal static class SurfaceDetail
    {
        // Set by the probe; 0 off.
        public static float Strength { get; set; }

        private static float sent = -1;
        private static int step = -1, wait;
        private static readonly List<Renderer> hidden = new();

        public static void Learn()
        {
            if (step < 0)
                step = 0;
        }

        public static void Update()
        {
            if (!Enhancements.Connected)
                return;
            if (step >= 0)
            {
                Learning();
                return;
            }
            if (Strength == sent)
                return;
            if (!Enhancements.SetSurfaceDetail(Strength, null))
            {
                CameraTools.LogOnce("Surface detail: the Enhancements add-on has no EnhancementsSetSurfaceDetail.");
                return;
            }
            Melon<CameraTools>.Logger.Msg($"Surface detail: strength {Strength:0.##}.");
            sent = Strength;
        }

        private static void Learning()
        {
            if (wait-- > 0)
                return;
            switch (step++)
            {
                case 0:
                    Enhancements.SetSurfaceDetail(-1f, null);
                    wait = 4;
                    break;
                case 1:
                    Hide();
                    wait = 3;
                    break;
                case 2:
                    Enhancements.SetSurfaceDetail(-2f, null);
                    wait = 4;
                    break;
                default:
                    foreach (var renderer in hidden)
                        if (renderer)
                            renderer.enabled = true;
                    Melon<CameraTools>.Logger.Msg($"Surface detail: learned with {hidden.Count} rock renderers hidden; the add-on's log names the shaders.");
                    hidden.Clear();
                    step = -1;
                    sent = -1;
                    break;
            }
        }

        private static void Hide()
        {
            hidden.Clear();
            foreach (var o in UnityEngine.Object.FindObjectsOfType(Il2CppType.Of<Renderer>()))
            {
                var renderer = o.Cast<Renderer>();
                if (renderer == null || !renderer.enabled)
                    continue;
                foreach (var material in renderer.sharedMaterials)
                {
                    var shader = material ? material.shader : null;
                    if (shader != null && shader.name.Contains("BlendStone"))
                    {
                        renderer.enabled = false;
                        hidden.Add(renderer);
                        break;
                    }
                }
            }
        }
    }
}
