using Il2CppInterop.Runtime.InteropTypes.Arrays;
using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // Skill light: point lights on the party's live elemental skill and burst effects, in the color of their particles. The
    // game draws them like its own lamps, so traced shots bake them into the bounce too.
    internal static class SkillLight
    {
        private const int MaxLights = 4;
        // Strength 100% is this intensity, a little over the game's lamps at night.
        private const float FullIntensity = 2f;
        // Seconds to ease brightness and color, and position.
        private const float Fade = 0.12f, Follow = 0.05f;
        // Particles at which a light reaches 63% of its strength.
        private const float ParticleScale = 60f;
        private const int SampledParticles = 64;
        private const float CharacterThreshold = 0.6f, CharacterSoftness = 0.1f, TwinShare = 0.5f;

        private static Transform pool;
        // Each pooled effect root by pointer: its name if it is a character's skill or burst, else null, and its particle systems.
        private static readonly Dictionary<IntPtr, string> skillRoots = new();
        private static readonly Dictionary<IntPtr, ParticleSystem[]> systems = new();
        private static readonly Dictionary<IntPtr, Color> colors = new();
        private static readonly Lamp[] lamps = new Lamp[MaxLights];
        private static Il2CppStructArray<ParticleSystem.Particle> particles;

        private sealed class Lamp
        {
            public GameObject Object;
            public Light Light, Twin;
            public IntPtr Root;
            public float Intensity;
            public Vector3 At;
            public Color Color = Color.white;
        }

        private readonly record struct Live(IntPtr Root, int Count, Vector3 At, float Size);

        // The characters-only twins: the world light reaches the bounce, so traced shots leave these out.
        public static bool IsTwin(Light light) => Array.Exists(lamps, lamp => lamp != null && lamp.Twin == light);

        public static bool IsOurs(Light light) => Array.Exists(lamps, lamp => lamp != null && (lamp.Light == light || lamp.Twin == light));

        // strength is the row's share, 0 while Skill light is off.
        public static void Update(float strength)
        {
            if (strength <= 0f)
            {
                Clear();
                return;
            }
            if (!pool)
            {
                var found = GameObject.Find("/EffectPool");
                if (!found)
                {
                    Clear();
                    return;
                }
                pool = found.transform;
                skillRoots.Clear();
                systems.Clear();
                colors.Clear();
            }
            var live = Find(Time.frameCount % 4 == 0);
            live.Sort((a, b) => b.Count.CompareTo(a.Count));
            float dt = Math.Max(Time.unscaledDeltaTime, 1e-3f), fade = 1 - MathF.Exp(-dt / Fade), follow = 1 - MathF.Exp(-dt / Follow);
            var taken = new bool[MaxLights];
            foreach (var effect in live.Take(MaxLights))
            {
                // The lamp that already follows this effect, else one that has faded out.
                int slot = Array.FindIndex(lamps, lamp => lamp != null && lamp.Root == effect.Root);
                for (int i = 0; slot < 0 && i < MaxLights; i++)
                    if (!taken[i] && (lamps[i] == null || lamps[i].Intensity < 0.02f))
                        slot = i;
                if (slot < 0 || taken[slot])
                    continue;
                taken[slot] = true;
                var lamp = lamps[slot] ??= new Lamp();
                if (!lamp.Object)
                    Create(lamp);
                bool fresh = lamp.Root != effect.Root;
                lamp.Root = effect.Root;
                lamp.At = fresh ? effect.At : Vector3.Lerp(lamp.At, effect.At, follow);
                if (colors.TryGetValue(effect.Root, out var sum) && sum.a > 0)
                {
                    float top = Math.Max(Math.Max(sum.r, sum.g), Math.Max(sum.b, 1e-4f));
                    var hue = new Color(sum.r / top, sum.g / top, sum.b / top);
                    lamp.Color = fresh ? hue : Color.Lerp(lamp.Color, hue, fade);
                }
                float target = FullIntensity * strength * (1 - MathF.Exp(-effect.Count / ParticleScale));
                lamp.Intensity += (target - lamp.Intensity) * fade;
                if (fresh)
                    Melon<CameraTools>.Logger.Msg($"Skill light: lights {skillRoots[effect.Root]}, {effect.Count} particles, color {lamp.Color}.");
                Apply(lamp, Math.Clamp(effect.Size * 1.5f + 4f, 4f, 12f));
            }
            for (int i = 0; i < MaxLights; i++)
                if (!taken[i] && lamps[i] is { Object: var o } lamp && o)
                {
                    lamp.Intensity -= lamp.Intensity * fade;
                    Apply(lamp, lamp.Light.range);
                }
        }

        // The skill and burst effects with live particles: their particle count, the particles' center and size, and on
        // sampling frames, or the first time an effect shows, their premultiplied mean color into colors.
        private static List<Live> Find(bool every)
        {
            var live = new List<Live>();
            for (int i = 0; i < pool.childCount; i++)
            {
                var root = pool.GetChild(i);
                if (!root.gameObject.activeSelf || !IsSkill(root))
                    continue;
                if (!systems.TryGetValue(root.Pointer, out var list))
                    systems[root.Pointer] = list = root.GetComponentsInChildren<ParticleSystem>(true).ToArray();
                bool sample = every || !colors.ContainsKey(root.Pointer);
                int count = 0;
                var center = Vector3.zero;
                var bounds = new Bounds();
                var color = new Color(0, 0, 0, 0);
                foreach (var ps in list)
                {
                    int alive = ps ? ps.particleCount : 0;
                    if (alive <= 0)
                        continue;
                    var renderer = ps.GetComponent<ParticleSystemRenderer>();
                    var b = renderer ? renderer.bounds : new Bounds(ps.transform.position, Vector3.zero);
                    if (count == 0)
                        bounds = b;
                    else
                        bounds.Encapsulate(b);
                    center += b.center * alive;
                    count += alive;
                    if (sample)
                    {
                        particles ??= new Il2CppStructArray<ParticleSystem.Particle>(SampledParticles);
                        int n = ps.GetParticles(particles);
                        for (int p = 0; p < n; p++)
                        {
                            Color c = particles[p].GetCurrentColor(ps);
                            color += new Color(c.r * c.a, c.g * c.a, c.b * c.a, c.a);
                        }
                    }
                }
                if (count == 0)
                    continue;
                if (sample && color.a > 0)
                    colors[root.Pointer] = color;
                live.Add(new Live(root.Pointer, count, center / count, bounds.extents.magnitude));
            }
            return live;
        }

        // A character's skill or burst effect, named like Eff_Avatar_Girl_Sword_Furina_ElementalBurst_Blast. Effects drawn at
        // the camera (InCamera, CameraFog, CameraEffect) would light the lens, so they are left out.
        private static bool IsSkill(Transform root)
        {
            if (!skillRoots.TryGetValue(root.Pointer, out string skill))
            {
                string name = root.name;
                skill = name.StartsWith("Eff_Avatar_") && (name.Contains("_ElementalArt") || name.Contains("_ElementalBurst")) && !name.Contains("Camera") ? name : null;
                skillRoots[root.Pointer] = skill;
            }
            return skill != null;
        }

        private static void Create(Lamp lamp)
        {
            lamp.Object = new GameObject("CameraTools Skill Light");
            UnityEngine.Object.DontDestroyOnLoad(lamp.Object);
            lamp.Light = lamp.Object.AddComponent<Light>();
            lamp.Light.type = LightType.Point;
            lamp.Light.shadows = LightShadows.None;
            var child = new GameObject("CameraTools Skill Light Twin");
            child.transform.SetParent(lamp.Object.transform, false);
            lamp.Twin = child.AddComponent<Light>();
            lamp.Twin.type = LightType.Point;
            lamp.Twin.set_isCharacterLight(true);
            lamp.Twin.characterLightCullingMask = uint.MaxValue;
            lamp.Twin.characterIntensityMultiplier = 1f;
            lamp.Twin.halfLambertThreshold = CharacterThreshold;
            lamp.Twin.halfLambertSoftness = CharacterSoftness;
            lamp.Twin.shadows = LightShadows.None;
            lamp.Intensity = 0f;
            lamp.Root = IntPtr.Zero;
        }

        private static void Apply(Lamp lamp, float range)
        {
            bool lit = lamp.Intensity > 0.02f;
            lamp.Object.transform.position = lamp.At;
            if (lamp.Light.enabled != lit)
                lamp.Light.enabled = lamp.Twin.enabled = lit;
            if (!lit)
                return;
            lamp.Light.color = lamp.Twin.color = lamp.Color;
            lamp.Light.intensity = lamp.Intensity;
            lamp.Twin.intensity = Math.Min(lamp.Intensity * TwinShare, LightSetup.MaxCharacterIntensity);
            lamp.Light.range = lamp.Twin.range = range;
        }

        private static void Clear()
        {
            for (int i = 0; i < MaxLights; i++)
            {
                if (lamps[i]?.Object is { } o && o)
                    UnityEngine.Object.Destroy(o);
                lamps[i] = null;
            }
        }
    }
}
