using CameraToolsEnhancements;

namespace CameraTools
{
    // One volume layer of a layered sky: a Clouds Type (an index of EnhancementsModel.CloudKinds) at its own coverage, 0 to
    // 1, height, thickness, cloud size and density.
    internal readonly record struct SkyLayer(int Kind, float Coverage, float Height, float Thickness, float Size, float Density)
    {
        public SkyLayer Scaled(float share) => this with { Coverage = Math.Min(Coverage * share, 1f) };
    }

    // How much of its coverage each layer of a mix shows now, 1 the mix's own.
    internal readonly record struct SkyScales(float Low, float Middle, float Cirrus)
    {
        public static readonly SkyScales One = new(1f, 1f, 1f);
    }

    // A mix the Layered type rolls: its low and middle layers, either missing, the painted cirrus's coverage, how often it
    // comes up, and the game's weathers it can come up in while Weather runs.
    internal sealed record SkyMix(string Name, SkyLayer? Low, SkyLayer? Middle, float Cirrus, float Weight, params WeatherKind[] Weathers)
    {
        // An overcast's low layer covers the whole sky every day: the day's random coverage and its drift do not thin it.
        public bool Whole { get; init; }
    }

    // What the sky shows now: a mix's layers, scaled while one blends into another.
    internal readonly record struct SkyLook(SkyLayer? Low, SkyLayer? Middle, float Cirrus);

    // Clouds' Layered type: a mix of low and middle clouds and cirrus rolled each game day, blending in over three minutes,
    // or at once for New sky, its coverage drifting through the day, and with Weather on only mixes the game's weather
    // allows.
    internal static class LayeredSky
    {
        private const int Altocumulus = 0, Stratocumulus = 1, Stratus = 2, Cumulus = 3, Towering = 7;
        private const float BlendSeconds = 180f;

        private static SkyLayer Low(int kind, float coverage) => kind switch
        {
            Cumulus => new(kind, coverage, 2000f, 1500f, 400f, 0.1f),
            Towering => new(kind, coverage, 2000f, 9000f, 3000f, 0.1f),
            Stratocumulus => new(kind, coverage, 2200f, 700f, 500f, 0.1f),
            Stratus => new(kind, coverage, 1300f, 400f, 1000f, 0.1f),
            _ => new(kind, coverage, 3000f, 700f, 500f, 0.1f),
        };

        private static SkyLayer Middle(float coverage) => new(Altocumulus, coverage, 3500f, 700f, 500f, 0.1f);

        private static readonly WeatherKind Sunny = WeatherKind.Sunny, Cloudy = WeatherKind.Cloudy, Rain = WeatherKind.Rain,
            Storm = WeatherKind.Thunderstorm, Snow = WeatherKind.Snow, Mist = WeatherKind.Mist;

        // Clear and nearly clear skies (the first four) come up about a third of the time with Weather off.
        public static readonly SkyMix[] Mixes =
        {
            new("Clear", null, null, 0f, 2f, Sunny, Mist),
            new("A few cumulus", Low(Cumulus, 0.2f), null, 0f, 2f, Sunny),
            new("Cirrus", null, null, 0.6f, 1f, Sunny, Mist),
            new("Cumulus under cirrus", Low(Cumulus, 0.35f), null, 0.5f, 1.5f, Sunny),
            new("Cumulus", Low(Cumulus, 0.45f), null, 0f, 2f, Sunny, Cloudy),
            new("Altocumulus", Low(Altocumulus, 0.7f), null, 0f, 1f, Sunny, Cloudy),
            new("Altocumulus over cumulus", Low(Cumulus, 0.3f), Middle(0.6f), 0f, 1.2f, Sunny, Cloudy),
            new("Towering cumulus", Low(Towering, 0.35f), null, 0.3f, 1f, Sunny),
            new("Towering cumulus under altocumulus", Low(Towering, 0.3f), Middle(0.5f), 0f, 0.8f, Sunny, Cloudy),
            new("Stratocumulus", Low(Stratocumulus, 0.75f), null, 0f, 1f, Cloudy, Snow),
            new("Overcast stratus", Low(Stratus, 0.95f), null, 0f, 0.6f, Cloudy, Snow, Mist) { Whole = true },
            // Rain and thunderstorms fall from a whole gray deck, low and a kilometer and a half thick so its base is dark:
            // under white towers, rain read as a sunny day's.
            new("Nimbostratus", new SkyLayer(Stratus, 1f, 800f, 1500f, 1000f, 0.1f), null, 0f, 1f, Rain, Storm) { Whole = true },
            new("Towering cumulus building", Low(Towering, 0.45f), Middle(0.5f), 0f, 0.5f, Cloudy),
        };

        // The mix for a game day, the times New sky was pressed, and the game's weather (null with Weather off): drawn by
        // weight from those the weather allows, by a hash of the day and the presses so a day keeps its sky.
        public static int Pick(int day, int rolls, WeatherKind? weather)
        {
            // Weather keeps: half the days keep the day before's mix, so a run of days can share one, while the weather still
            // allows it.
            int from = day;
            while (from > day - 30 && Hash(from * 977 + rolls * 31 + 3) < 0.5f)
                from--;
            int pick = Fresh(from, rolls, weather);
            return weather is { } w && !Mixes[pick].Weathers.Contains(w) ? Fresh(day, rolls, weather) : pick;
        }

        private static int Fresh(int day, int rolls, WeatherKind? weather)
        {
            var allowed = Enumerable.Range(0, Mixes.Length).Where(i => weather is not { } w || Mixes[i].Weathers.Contains(w)).ToArray();
            if (allowed.Length == 0)
                allowed = Enumerable.Range(0, Mixes.Length).ToArray();
            float total = allowed.Sum(i => Mixes[i].Weight);
            float at = Hash(day * 7919 + rolls * 104729 + 17) * total;
            foreach (int i in allowed)
            {
                at -= Mixes[i].Weight;
                if (at < 0f)
                    return i;
            }
            return allowed[^1];
        }

        // How a layer's coverage drifts through a day: 0.75 to 1.25 times, slowly over a few game hours, each layer (salt) on
        // its own, so low clouds can thin while high ones build.
        public static float Drift(double days, int day, int salt)
        {
            double phase = Hash(day * 31 + salt * 977 + 5) * 6.2831853;
            double wave = 0.6 * Math.Sin(days * 6.2831853 * 3 + phase) + 0.4 * Math.Sin(days * 6.2831853 * 5.3 + phase * 1.7);
            return (float)(1 + 0.25 * wave);
        }

        // Each layer's share of its mix's coverage now: a random 0.6 to 1.4 for the day and the times New sky was pressed,
        // times the layer's drift, in hundredths so the add-on's clouds keep their history between changes.
        public static SkyScales Scales(double days, int day, int rolls)
        {
            float Scale(int salt) => MathF.Round((0.6f + 0.8f * Hash(day * 7907 + rolls * 131 + salt * 104723)) * Drift(days, day, salt) * 100f) / 100f;
            return new SkyScales(Scale(1), Scale(2), Scale(3));
        }

        // The look a share t of the way from one look to the next, in hundredths so the clouds change every couple of seconds.
        // A layer of the same type in both changes its coverage and shape gradually, so its clouds stay and drift on. One of
        // another type thins away while the next one grows in, both drawn at once while the two volume layers hold them:
        // thinned one after the other, half way the sky emptied. With more layers than that, the old one thins over the
        // first half and the next one grows in over the second.
        public static SkyLook Between(SkyLook from, SkyLook to, float t)
        {
            t = MathF.Round(Math.Clamp(t, 0f, 1f) * 100f) / 100f;
            float cirrus = from.Cirrus + (to.Cirrus - from.Cirrus) * t;
            var layers = new List<SkyLayer>();
            var leaving = new List<SkyLayer>();
            foreach (var (a, b) in new[] { (from.Low, to.Low), (from.Middle, to.Middle) })
            {
                if (a is { } x && b is { } y && x.Kind == y.Kind)
                    layers.Add(Layer(x, y, t)!.Value);
                else
                {
                    if (b is { } coming && t > 0f)
                        layers.Add(coming.Scaled(t));
                    if (a is { } going && t < 1f)
                        leaving.Add(going.Scaled(1f - t));
                }
            }
            layers.AddRange(leaving);
            if (layers.Count <= 2)
                return new SkyLook(layers.Count > 0 ? layers[0] : null, layers.Count > 1 ? layers[1] : null, cirrus);
            return new SkyLook(Layer(from.Low, to.Low, t), Layer(from.Middle, to.Middle, t), cirrus);
        }

        private static SkyLayer? Layer(SkyLayer? from, SkyLayer? to, float t)
        {
            float Mix(float a, float b) => a + (b - a) * t;
            if (from is { } a && to is { } b && a.Kind == b.Kind)
                return new SkyLayer(a.Kind, Mix(a.Coverage, b.Coverage), Mix(a.Height, b.Height), Mix(a.Thickness, b.Thickness), Mix(a.Size, b.Size),
                    Mix(a.Density, b.Density));
            return t < 0.5f ? from?.Scaled(1f - 2f * t) : to?.Scaled(2f * t - 1f);
        }

        public static SkyLook Of(SkyMix mix, SkyScales scales)
            => new(mix.Low?.Scaled(mix.Whole ? 1f : scales.Low), mix.Middle?.Scaled(scales.Middle), Math.Min(mix.Cirrus * scales.Cirrus, 1f));

        // The mix shown or blending in, the look the blend started from, and how far it has come.
        private static int rolls, shown = -1;
        private static SkyLook from;
        private static float share = 1f;

        // The look the Layered type shows now, and whether it changed this frame.
        public static SkyLook Current { get; private set; }
        public static bool Changed { get; private set; }

        // The share of the sun's and the moon's direct light the look's cover lets through everywhere, as the add-on's overcast
        // grays the sky: all of it at 60% coverage and below, an eighth under a whole overcast. The clouds' shadows darken
        // the world where they fall; this dims the characters too, which those shadows do not reach.
        public static float SunShare(SkyLook look)
        {
            float cover = Math.Max(look.Low?.Coverage ?? 0f, look.Middle?.Coverage ?? 0f);
            float t = Math.Clamp((cover - 0.6f) / 0.4f, 0f, 1f);
            return 1f - 0.88f * t * t * (3f - 2f * t);
        }

        // Today's sky's name, for its row.
        public static string Name => shown < 0 ? "None yet" : Mixes[shown].Name;

        // New sky: another mix, shown at once.
        public static void Roll() => rolls++;
        private static int rollsShown;

        // Once a frame while the Layered type runs: days is the sky's day count (NightSky.Days), weather the game's while
        // Weather runs, seconds the frame's. A new day's mix, or the weather's, blends in from what the sky shows now; New
        // sky's replaces it at once, as the button promises, where a three-minute blend showed the old clouds and, half way,
        // an empty sky under the new name.
        public static void Update(double days, WeatherKind? weather, float seconds)
        {
            var before = Current;
            int today = (int)Math.Floor(days);
            int pick = Pick(today, rolls, weather);
            var scales = Scales(days, today, rolls);
            bool rolled = rolls != rollsShown;
            rollsShown = rolls;
            if (shown < 0 || rolled)
                share = 1f;
            else if (pick != shown)
            {
                from = Current;
                share = 0f;
            }
            shown = pick;
            if (share < 1f)
                share = Math.Min(share + seconds / BlendSeconds, 1f);
            var target = Of(Mixes[shown], scales);
            Current = share < 1f ? Between(from, target, share) : target;
            Changed = Current != before;
        }

        // The add-on's clouds for a look: the low layer as the main one, or the middle one alone, the cirrus behind them, or
        // the cirrus alone as the Cirrus type draws it; with none, no clouds.
        public static void Apply(ref EnhancementsSettings settings, SkyLook look)
        {
            var main = look.Low ?? look.Middle;
            var middle = look.Low != null ? look.Middle : null;
            settings.CloudLayers = default;
            if (main is not { } layer)
            {
                if (look.Cirrus <= 0f)
                {
                    settings.Clouds.Enabled = 0;
                    return;
                }
                var cirrus = EnhancementsModel.CloudKinds[5];
                layer = new SkyLayer(5, look.Cirrus, cirrus.Height, cirrus.Thickness, cirrus.Size, cirrus.Density);
            }
            var kind = EnhancementsModel.CloudKinds[layer.Kind];
            settings.Clouds.Coverage = layer.Coverage;
            settings.Clouds.Base = layer.Height;
            settings.Clouds.Thickness = layer.Thickness;
            settings.Clouds.Size = layer.Size;
            settings.Clouds.Density = layer.Density;
            settings.Clouds.Stretch = kind.Stretch;
            settings.Clouds.Type = kind.Type;
            settings.CloudPattern.Pattern = EnhancementsModel.PatternOf(kind);
            if (middle is { } m)
            {
                var mk = EnhancementsModel.CloudKinds[m.Kind];
                settings.CloudLayers.Middle = 1;
                settings.CloudLayers.MiddleLayer = new EnhancementsCloudLayer
                {
                    Coverage = m.Coverage, Base = m.Height, Thickness = m.Thickness, Size = m.Size, Stretch = mk.Stretch, Density = m.Density,
                    Detail = settings.Clouds.Detail, Type = mk.Type,
                };
            }
            settings.CloudLayers.Cirrus = main != null ? look.Cirrus : 0f;
        }

        private static float Hash(int x)
        {
            uint h = (uint)x * 2654435761u;
            h ^= h >> 15;
            h *= 2246822519u;
            h ^= h >> 13;
            h *= 3266489917u;
            h ^= h >> 16;
            return (h & 0xffffff) / 16777216f;
        }
    }
}
