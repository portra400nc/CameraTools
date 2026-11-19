using CameraToolsEnhancements;
using Il2CppInterop.Runtime;
using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // Surface detail: the Enhancements add-on blends a rock detail map into the G-buffer where rock is drawn. It knows rock
    // by pixel shader, which a learn finds: a census of the G-buffer draws by shader, then another with every renderer of a
    // rock material (miHoYo/Terrain/BlendStone) hidden for a few frames; the shaders that drop out draw rock. The add-on
    // remembers what each shader draws across launches and asks for a learn only when it sees a G-buffer shader it has not
    // classified, as in a region not visited before. Finding the renderers to hide takes about a second.
    internal static class SurfaceDetail
    {
        // At most one learn this often, in real seconds.
        private const float LearnEvery = 60f;

        // Set by the probe; 0 off.
        public static float Strength { get; set; }

        private static float sent = -1, learnedAt = float.NegativeInfinity;
        private static int step = -1, wait;
        private static readonly List<Renderer> hidden = new();

        // From the probe: learn now.
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
            if (Strength != sent && Enhancements.SetSurfaceDetail(Strength, null))
            {
                Melon<CameraTools>.Logger.Msg($"Surface detail: strength {Strength:0.##}.");
                sent = Strength;
            }
            if (Strength > 0 && Time.unscaledTime >= learnedAt + LearnEvery && Enhancements.SurfaceWantsLearn())
            {
                Melon<CameraTools>.Logger.Msg("Surface detail: the add-on saw G-buffer shaders it has not classified, learning.");
                step = 0;
            }
        }

        private static IEnumerable<Renderer> RockRenderers()
        {
            var seen = new Dictionary<IntPtr, bool>();
            foreach (var o in UnityEngine.Object.FindObjectsOfType(Il2CppType.Of<Renderer>()))
            {
                var renderer = o.Cast<Renderer>();
                if (renderer == null || !renderer.enabled)
                    continue;
                foreach (var material in renderer.sharedMaterials)
                {
                    if (material == null)
                        continue;
                    if (!seen.TryGetValue(material.Pointer, out bool rock))
                    {
                        var shader = material.shader;
                        seen[material.Pointer] = rock = shader != null && shader.name.Contains("BlendStone");
                    }
                    if (rock)
                    {
                        yield return renderer;
                        break;
                    }
                }
            }
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
                    hidden.Clear();
                    hidden.AddRange(RockRenderers());
                    foreach (var renderer in hidden)
                        renderer.enabled = false;
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
                    learnedAt = Time.unscaledTime;
                    step = -1;
                    sent = -1;
                    break;
            }
        }
    }
}
