using CameraToolsEnhancements;
using Il2CppInterop.Runtime;
using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // Surface detail: the Enhancements add-on blends a rock detail map into the G-buffer where rock is drawn. It knows rock
    // by pixel shader, which a learn finds: a census of the G-buffer draws by shader, then another with every renderer of a
    // type's material (miHoYo/Terrain/BlendStone for rock) hidden for a few frames; the shaders that drop out draw that type. The add-on
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

        private static ulong shownPick;
        private static string pickedAs;

        // From the probe: the pixel shader under the screen's middle takes a type, 2 ground or 0 none. The add-on answers
        // a frame or two later; Update shows it.
        public static void Pick(int type)
        {
            if (Enhancements.SetSurfaceDetail(-200f - type, null))
                pickedAs = type == 2 ? "ground" : type == 1 ? "rock" : "no detail";
        }

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
            if (pickedAs != null && Enhancements.SurfaceLastPick() is var picked && picked != shownPick)
            {
                shownPick = picked;
                CameraUi.Toast($"Surface detail: {picked:x16} marked as {pickedAs}", whileHidden: true);
                Melon<CameraTools>.Logger.Msg($"Surface detail: pixel shader {picked:x16} marked as {pickedAs}.");
                pickedAs = null;
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

        // The types the add-on learns, in its numbering, by the Unity shaders that draw them.
        // The types the add-on learns, in its numbering, by the Unity shaders that draw them. Ground's near terrain is a
        // Terrain component, not a renderer, so its learn also stops the terrains drawing their ground.
        private static readonly (int Type, string Shader)[] Types = { (1, "BlendStone"), (2, "Terrain Mesh") };
        private static readonly List<Terrain> hiddenTerrains = new();

        private static IEnumerable<Renderer> RenderersOf(string pattern)
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
                        seen[material.Pointer] = rock = shader != null && shader.name.Contains(pattern);
                    }
                    if (rock)
                    {
                        yield return renderer;
                        break;
                    }
                }
            }
        }

        // A census as the scene is, then for each type one with its renderers hidden for a few frames, then the end.
        private static void Learning()
        {
            if (wait-- > 0)
                return;
            step++;
            if (step == 1)
            {
                Enhancements.SetSurfaceDetail(-1f, null);
                wait = 4;
                return;
            }
            int index = (step - 2) / 3, part = (step - 2) % 3;
            if (index >= Types.Length)
            {
                Enhancements.SetSurfaceDetail(-100f, null);
                Melon<CameraTools>.Logger.Msg("Surface detail: learned; the add-on's log names the shaders.");
                learnedAt = Time.unscaledTime;
                step = -1;
                sent = -1;
                return;
            }
            switch (part)
            {
                case 0:
                    hidden.Clear();
                    hidden.AddRange(RenderersOf(Types[index].Shader));
                    foreach (var renderer in hidden)
                        renderer.enabled = false;
                    hiddenTerrains.Clear();
                    if (Types[index].Type == 2)
                        // This build keeps only drawHeightmap's setter: an active terrain is taken to draw its ground.
                        foreach (var terrain in Terrain.activeTerrains)
                            if (terrain)
                            {
                                terrain.set_drawHeightmap(false);
                                hiddenTerrains.Add(terrain);
                            }
                    wait = 3;
                    break;
                case 1:
                    Enhancements.SetSurfaceDetail(-1f - Types[index].Type, null);
                    wait = 4;
                    break;
                default:
                    foreach (var renderer in hidden)
                        if (renderer)
                            renderer.enabled = true;
                    foreach (var terrain in hiddenTerrains)
                        if (terrain)
                            terrain.set_drawHeightmap(true);
                    Melon<CameraTools>.Logger.Msg($"Surface detail: learn of {Types[index].Shader} with {hidden.Count} renderers and {hiddenTerrains.Count} terrains hidden.");
                    hiddenTerrains.Clear();
                    hidden.Clear();
                    wait = 3;
                    break;
            }
        }
    }
}
