using CameraToolsEnhancements;

namespace CameraTools
{
    // The game's weathers as the physical sky shows them under Experimental features > Weather.
    internal enum WeatherKind { Sunny, Cloudy, Rain, Thunderstorm, Snow, Mist }

    // A weather's look: our clouds (a Clouds Type's shape and its rows), the sky's haziness, the mist (the Haze effect's
    // density row and height falloff), the share of the sun's and the moon's direct light the cover lets through, the
    // rain, 0 none to 1 a downpour, the lightning, 0 none to 1 a strike about every 7 seconds, and the mist's banks: how
    // patchy, 0 an even haze to 1 banks with clear air between, and how many meters across, and a lift of the finished
    // picture in stops.
    internal sealed record WeatherLook(int Kind, float Coverage, float Height, float Thickness, float Size, float Density, float EdgeDetail,
        float CloudSun, float CloudSky, float Haziness, float Mist, float MistFalloff, float Sun, bool Clouds = true, float Rain = 0f, float Lightning = 0f, float Banks = 0f, float BankSize = 40f, float Exposure = 0f);

    internal static class WeatherLooks
    {
        private const int Stratocumulus = 1, Stratus = 2, HighPatches = 4;

        // Each weather but Sunny, which is the user's own rows. Mist is in the Haze effect's density row's units. The clouds
        // are the volume ones, lit at 1 as the Types are: the painted layer's sunlight of 2 to 4 washed them out.
        private static readonly Dictionary<WeatherKind, WeatherLook> Looks = new()
        {
            [WeatherKind.Cloudy] = new(Stratocumulus, 82f, 2200f, 800f, 500f, 0.1f, 0.8f, 1f, 1f, 0.15f, 1.5f, 0.02f, 0.45f, Banks: 0.4f, BankSize: 80f),
            [WeatherKind.Rain] = new(Stratus, 97f, 900f, 600f, 1000f, 0.1f, 0.5f, 0.6f, 0.8f, 0.3f, 6f, 0.012f, 0.12f, Rain: 0.5f, Banks: 0.6f, BankSize: 40f),
            [WeatherKind.Thunderstorm] = new(Stratus, 100f, 700f, 900f, 1000f, 0.14f, 0.4f, 0.4f, 0.6f, 0.35f, 8f, 0.01f, 0.06f, Rain: 1f, Lightning: 1f, Banks: 0.6f, BankSize: 50f),
            // Snow is bright: a thinner, broken deck lit through, a whiter sky, more of the sun let through, and more
            // exposure, as the snow on the ground lights the air and the game lights its snowy world dimly.
            [WeatherKind.Snow] = new(Stratocumulus, 84f, 1500f, 400f, 700f, 0.08f, 0.7f, 1f, 1.3f, 0.45f, 5f, 0.012f, 0.7f, Banks: 0.5f, BankSize: 60f, Exposure: 1.5f),
            [WeatherKind.Mist] = new(HighPatches, 40f, 4500f, 400f, 200f, 0.03f, 0.9f, 4f, 1f, 0.25f, 10f, 0.05f, 0.6f, Banks: 0.85f, BankSize: 30f),
        };

        public static string Name(WeatherKind kind) => kind.ToString();

        // The weather preset the game's sky renders, such as "BigWorld/Weather_Rain_Light", as one of ours by the words in its
        // name: the weather manager's climate is the gameplay weather, which the World tab's weather and much of what the
        // sky shows never change. Sand and dust read as Mist, a dusty haze; anything else, such as a clear sky, is Sunny,
        // which keeps the user's sky.
        public static WeatherKind OfPreset(string preset)
        {
            string name = preset ?? "";
            bool Has(string word) => name.Contains(word, StringComparison.OrdinalIgnoreCase);
            return Has("Sand") || Has("Dust") ? WeatherKind.Mist
                : Has("Storm") || Has("Thunder") ? WeatherKind.Thunderstorm
                : Has("Snow") || Has("Blizzard") ? WeatherKind.Snow
                : Has("Rain") || Has("Drizzle") ? WeatherKind.Rain
                : Has("Fog") || Has("Mist") ? WeatherKind.Mist
                : Has("Cloud") || Has("Overcast") ? WeatherKind.Cloudy
                : WeatherKind.Sunny;
        }

        // The look of a weather: Sunny's is the user's clouds, haziness and Haze effect as their rows set them.
        public static WeatherLook Look(WeatherKind kind, EnhancementValues values) => kind == WeatherKind.Sunny
            ? new((int)values[Enhancement.CloudKind], values[Enhancement.CloudCoverage], values[Enhancement.CloudHeight], values[Enhancement.CloudThickness],
                values[Enhancement.CloudSize], values[Enhancement.CloudDensity], values[Enhancement.CloudEdgeDetail], values[Enhancement.CloudSunlight],
                values[Enhancement.CloudSkyLight], values[Enhancement.SkyHaziness], values.On(Enhancement.Haze) ? values[Enhancement.HazeDensity] : 0f,
                values[Enhancement.HazeHeightFalloff], 1f, values.On(Enhancement.Clouds))
            : Looks[kind];

        // The look a share t of the way from a to b. The clouds' shape changes half way, where both are mostly cover.
        public static WeatherLook Between(WeatherLook a, WeatherLook b, float t)
        {
            t = Math.Clamp(t, 0f, 1f);
            float Mix(float x, float y) => x + (y - x) * t;
            var shape = t < 0.5f ? a : b;
            // A sky without clouds fades them in or out by coverage rather than switching them.
            float coverage = Mix(a.Clouds ? a.Coverage : 0f, b.Clouds ? b.Coverage : 0f);
            return new WeatherLook(shape.Kind, coverage, Mix(a.Height, b.Height), Mix(a.Thickness, b.Thickness), Mix(a.Size, b.Size), Mix(a.Density, b.Density),
                Mix(a.EdgeDetail, b.EdgeDetail), Mix(a.CloudSun, b.CloudSun), Mix(a.CloudSky, b.CloudSky), Mix(a.Haziness, b.Haziness), Mix(a.Mist, b.Mist),
                Mix(a.MistFalloff, b.MistFalloff), Mix(a.Sun, b.Sun), coverage > 0f, Mix(a.Rain, b.Rain), Mix(a.Lightning, b.Lightning), Mix(a.Banks, b.Banks), Mix(a.BankSize, b.BankSize), Mix(a.Exposure, b.Exposure));
        }

        // The add-on's settings with the look's clouds, haziness and mist, the mist scaled by mist (1 the look's own). The
        // haziness is rounded to hundredths, as each change builds the sky's tables again.
        public static void Apply(ref EnhancementsSettings settings, WeatherLook look, float mist, float hazeDensityUnit)
        {
            var kind = EnhancementsModel.CloudKinds[Math.Clamp(look.Kind, 0, EnhancementsModel.CloudKinds.Length - 1)];
            settings.Clouds.Enabled = look.Clouds ? 1u : 0u;
            settings.Clouds.Coverage = look.Coverage / 100f;
            settings.Clouds.Base = look.Height;
            settings.Clouds.Thickness = look.Thickness;
            settings.Clouds.Size = look.Size;
            settings.Clouds.Stretch = kind.Stretch;
            settings.Clouds.Density = look.Density;
            settings.Clouds.Detail = look.EdgeDetail;
            settings.Clouds.Sun = look.CloudSun;
            settings.Clouds.Ambient = look.CloudSky;
            settings.Clouds.Type = kind.Type;
            settings.CloudPattern.Pattern = EnhancementsModel.PatternOf(kind);
            settings.Sky.Haze = MathF.Round(look.Haziness * 100f) / 100f;
            float density = look.Mist * mist;
            settings.Atmosphere.Enabled = density > 0f ? 1u : 0u;
            settings.Atmosphere.Density = density * hazeDensityUnit;
            settings.Atmosphere.HeightFalloff = look.MistFalloff;
            settings.Rain.Amount = look.Rain;
            settings.Rain.Lightning = look.Lightning;
            settings.Mist.Patchiness = look.Banks;
            settings.Mist.Size = look.BankSize;
            settings.SceneLight.ExposureEv = look.Exposure;
        }
    }
}
