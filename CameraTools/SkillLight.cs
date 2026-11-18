using Il2CppInterop.Runtime.InteropTypes.Arrays;
using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // Skill light: point lights on the party's live skill, burst and attack effects, in the color of their particles, and on
    // the objects their skills leave behind (Mitya's partner, Furina's salon members), in the color of that character's
    // effects. The game draws them like its own lamps, so traced shots bake them into the bounce too.
    internal static class SkillLight
    {
        private const int MaxLights = 4;
        // Strength 100% is this intensity, a little over the game's lamps at night.
        private const float FullIntensity = 2f;
        // Seconds to ease brightness and color, and position.
        private const float Fade = 0.12f, Follow = 0.05f;
        // Particles at which a light reaches 63% of its strength.
        private const float ParticleScale = 60f;
        // A skill's object glows from its meshes, not particles: it counts as this many particles.
        private const int ObjectParticles = 60;
        private const int SampledParticles = 64;
        private const float CharacterThreshold = 0.6f, CharacterSoftness = 0.1f, TwinShare = 0.5f;
        // Body types in effect names: Eff_Avatar_<body>_<weapon>_<character>_...
        private static readonly HashSet<string> Bodies = new() { "Boy", "Girl", "Lady", "Male", "Loli" };

        private static Transform pool, gadgets;
        // Each effect root or skill object by pointer: the character it belongs to, else null.
        private static readonly Dictionary<IntPtr, string> owners = new();
        private static readonly Dictionary<IntPtr, string> labels = new();
        private static readonly Dictionary<IntPtr, ParticleSystem[]> systems = new();
        private static readonly Dictionary<IntPtr, Renderer[]> renderers = new();
        // Premultiplied particle color sums by effect root, and each character's last effect hue.
        private static readonly Dictionary<IntPtr, Color> colors = new();
        private static readonly Dictionary<string, Color> characterHues = new();
        private static readonly Lamp[] lamps = new Lamp[MaxLights];
        private static Il2CppStructArray<ParticleSystem.Particle> particles;
        // What has been lit, logged once each.
        private static readonly HashSet<string> logged = new();

        private sealed class Lamp
        {
            public GameObject Object;
            public Light Light, Twin;
            public IntPtr Root;
            public float Intensity;
            public Vector3 At;
            public Color Color = Color.white;
        }

        private readonly record struct Live(IntPtr Root, int Count, Vector3 At, float Size, Color Hue);

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
                gadgets = null;
                owners.Clear();
                labels.Clear();
                systems.Clear();
                renderers.Clear();
                colors.Clear();
            }
            if (!gadgets)
                gadgets = GameObject.Find("/EntityRoot/OtherGadgetRoot")?.transform;
            var live = new List<Live>();
            FindEffects(live, Time.frameCount % 4 == 0);
            if (gadgets)
                FindObjects(live);
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
                lamp.Color = fresh ? effect.Hue : Color.Lerp(lamp.Color, effect.Hue, fade);
                float target = FullIntensity * strength * (1 - MathF.Exp(-effect.Count / ParticleScale));
                lamp.Intensity += (target - lamp.Intensity) * fade;
                if (fresh && logged.Add(labels[effect.Root]))
                    Melon<CameraTools>.Logger.Msg($"Skill light: lights {labels[effect.Root]}, {effect.Count} particles, color {lamp.Color}.");
                Apply(lamp, Math.Clamp(effect.Size * 1.5f + 4f, 4f, 12f));
            }
            for (int i = 0; i < MaxLights; i++)
                if (!taken[i] && lamps[i] is { Object: var o } lamp && o)
                {
                    lamp.Intensity -= lamp.Intensity * fade;
                    Apply(lamp, lamp.Light.range);
                }
        }

        // The characters' effects with live particles: their particle count, the particles' center and size, and their
        // hue, from the particles' premultiplied mean color, sampled every few frames and the first time an effect shows.
        private static void FindEffects(List<Live> live, bool every)
        {
            for (int i = 0; i < pool.childCount; i++)
            {
                var root = pool.GetChild(i);
                if (!root.gameObject.activeSelf || EffectOwner(root) is not { } owner)
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
                if (!colors.TryGetValue(root.Pointer, out var sum) || sum.a <= 0)
                    continue;
                var hue = Hue(sum);
                // A character's hue leans to their larger effects, so a white sparkle does not set it.
                characterHues[owner] = characterHues.TryGetValue(owner, out var was) ? Color.Lerp(was, hue, count / (count + 200f)) : hue;
                live.Add(new Live(root.Pointer, count, center / count, bounds.extents.magnitude, hue));
            }
        }

        // The objects the party's skills leave in the world, by a name that holds a character's name from their effects
        // (SkillObj_Mitya_Partner_Enhance), lit steadily at the center of what they draw, in that character's hue.
        private static void FindObjects(List<Live> live)
        {
            for (int i = 0; i < gadgets.childCount; i++)
            {
                var root = gadgets.GetChild(i);
                if (!root.gameObject.activeInHierarchy || ObjectOwner(root) is not { } owner || !characterHues.TryGetValue(owner, out var hue))
                    continue;
                if (!renderers.TryGetValue(root.Pointer, out var list))
                    renderers[root.Pointer] = list = root.GetComponentsInChildren<Renderer>(true).ToArray();
                bool any = false;
                var bounds = new Bounds();
                foreach (var r in list)
                {
                    if (!r || !r.enabled || !r.gameObject.activeInHierarchy)
                        continue;
                    if (any)
                        bounds.Encapsulate(r.bounds);
                    else
                        bounds = r.bounds;
                    any = true;
                }
                if (any)
                    live.Add(new Live(root.Pointer, ObjectParticles, bounds.center, bounds.extents.magnitude, hue));
            }
        }

        // The character of an effect named like Eff_Avatar_Girl_Catalyst_Sucrose_DemiHurricane_Main, or null. Effects drawn
        // at the camera (InCamera, CameraFog, CameraEffect) would light the lens, so they are left out.
        private static string EffectOwner(Transform root)
        {
            if (!owners.TryGetValue(root.Pointer, out string owner))
            {
                string name = root.name;
                var parts = name.Split('_');
                owner = parts.Length >= 5 && parts[0] == "Eff" && parts[1] == "Avatar" && Bodies.Contains(parts[2]) && !name.Contains("Camera")
                    ? parts[4] : null;
                owners[root.Pointer] = owner;
                labels[root.Pointer] = name;
            }
            return owner;
        }

        // The character whose name a skill object's name holds, among those whose effects have shown, or null.
        private static string ObjectOwner(Transform root)
        {
            if (!owners.TryGetValue(root.Pointer, out string owner))
            {
                string name = root.name;
                owner = name.Split('_', '(', ')').FirstOrDefault(characterHues.ContainsKey);
                // Not cached while no character matches: the character's first effect may still come.
                if (owner == null)
                    return null;
                owners[root.Pointer] = owner;
                labels[root.Pointer] = name;
            }
            return owner;
        }

        // A premultiplied color sum's hue, scaled to a top channel of 1.
        private static Color Hue(Color sum)
        {
            float top = Math.Max(Math.Max(sum.r, sum.g), Math.Max(sum.b, 1e-4f));
            return new Color(sum.r / top, sum.g / top, sum.b / top);
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
