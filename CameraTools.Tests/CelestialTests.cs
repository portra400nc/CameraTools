// The celestial model (CameraTools/Celestial.cs) against the game's sun path as the Deck recorded it on 2026-11-15
// (.work/nightsky/sunpath-1.tsv) and against known eclipses and star positions.
using CameraTools;
using Vec3 = System.Numerics.Vector3;

internal static class CelestialTests
{
    // GetSunLightDirection at these hours on the Deck: the way the light shines, so minus the way toward the sun.
    private static readonly Dictionary<float, Vec3> Shine = new()
    {
        [0f] = new(0.07296f, 0.99731f, 0.00638f),
        [4f] = new(-0.11185f, 0.50272f, 0.85718f),
        [8f] = new(-0.32598f, -0.46251f, 0.82452f),
        [12f] = new(-0.357f, -0.93358f, -0.03123f),
        [16f] = new(-0.17785f, -0.46251f, -0.8686f),
        [21f] = new(0.07f, 0.70662f, -0.70412f),
    };

    private static Vec3 TowardSun(float hour) => -Shine[hour];

    private static float Degrees(float radians) => radians * 180f / MathF.PI;

    private static Vec3 Equatorial(double rightAscensionHours, double declinationDegrees)
    {
        double a = rightAscensionHours * Math.PI / 12, d = declinationDegrees * Math.PI / 180;
        return new Vec3((float)(Math.Cos(d) * Math.Cos(a)), (float)(Math.Cos(d) * Math.Sin(a)), (float)Math.Sin(d));
    }

    private static Vec3 World(CelestialSky sky, Vec3 e) => e.X * sky.StarsX + e.Y * sky.StarsY + e.Z * sky.StarsZ;

    // The real sun's J2000 equatorial direction (Schlyter's low-precision sun), for the eclipses.
    private static Vec3 RealSun(double julianDay)
    {
        const double rad = Math.PI / 180;
        double d = julianDay - 2451543.5;
        double w = (282.9404 + 4.70935e-5 * d) * rad, e = 0.016709, m = (356.0470 + 0.9856002585 * d) * rad;
        double ecc = m + e * Math.Sin(m) * (1 + e * Math.Cos(m));
        double x = Math.Cos(ecc) - e, y = Math.Sqrt(1 - e * e) * Math.Sin(ecc);
        double longitude = Math.Atan2(y, x) + w - 3.8239e-5 * rad * d, obliquity = 23.4393 * rad;
        return new Vec3((float)Math.Cos(longitude), (float)(Math.Sin(longitude) * Math.Cos(obliquity)), (float)(Math.Sin(longitude) * Math.Sin(obliquity)));
    }

    private static float Angle(Vec3 a, Vec3 b) => Degrees(MathF.Acos(Math.Clamp(Vec3.Dot(Vec3.Normalize(a), Vec3.Normalize(b)), -1f, 1f)));

    public static void Run(Action<string, bool> check)
    {
        var fitted = CelestialFrame.Fit(TowardSun);
        check("the frame fits the Deck's sun path: a sky 12.59 degrees south, the sun 8.39 degrees north of the equator",
            fitted is { } f && MathF.Abs(Degrees(f.Latitude) + 12.59f) < 0.05f && MathF.Abs(Degrees(f.SunDeclination) - 8.39f) < 0.05f);
        check("a sun path that is no circle about a fixed pole fits no frame",
            CelestialFrame.Fit(hour => hour == 4f ? Vec3.Normalize(TowardSun(4f) + new Vec3(0, 0.1f, 0)) : TowardSun(hour)) == null);
        var frame = fitted.Value;
        check("the fitted south pole stands 12.6 degrees up toward Unity's -x, the noon sun's opposite side",
            Celestial.ToUnity(-frame.North) is var south && MathF.Abs(Degrees(MathF.Asin(south.Y)) - 12.59f) < 0.05f && south.X < -0.95f);

        var noon = Celestial.Place(frame, 0.5, TowardSun(12f));
        check("the stars' axes carry the sun's own equatorial place onto the game's sun",
            Angle(World(noon, Equatorial(Celestial.EpochSunRightAscension(frame.SunDeclination) * 12 / Math.PI + 24 * 0.5 / Celestial.YearDays, Degrees(frame.SunDeclination))),
                TowardSun(12f)) < 0.02f);
        // Acrux, RA 12h 26m 36s, declination -63 05' 57", at midnight of day 0: the meridian's right ascension is the sun's
        // (1.325 h) plus 12 h, so its hour angle is 0.89 h west, and from 12.59 degrees south it stands 38.6 degrees up.
        var midnight = Celestial.Place(frame, 0, TowardSun(0f));
        var acrux = World(midnight, Equatorial(12 + 26 / 60.0 + 36 / 3600.0, -(63 + 5 / 60.0 + 57 / 3600.0)));
        check("at midnight of day 0 Acrux stands 38.6 degrees up, just west of the south meridian",
            MathF.Abs(Degrees(MathF.Asin(acrux.Y)) - 38.6f) < 0.2f && acrux.X < 0 && Vec3.Dot(Celestial.ToUnity(frame.West), acrux) > 0);
        var nextMidnight = Celestial.Place(frame, 1, TowardSun(0f));
        check("a day later at the same hour the stars stand 0.986 degrees further west",
            MathF.Abs(Angle(World(midnight, Vec3.UnitX), World(nextMidnight, Vec3.UnitX)) - 0.9856f) < 0.01f);

        // Known eclipses of 2026: the total solar eclipse of August 12 (greatest at 17:46 UT) and the total lunar eclipse
        // of March 3 (greatest at 11:33 UT).
        double solar = 2461265.0 + (17 + 46 / 60.0) / 24 - 0.5, lunar = 2461103.0 + (11 + 33 / 60.0) / 24 - 0.5;
        // Seen from the earth's center the bodies stand apart by the eclipses' gamma (0.898 and -0.377) times the moon's
        // horizontal parallax, about 0.95 degrees.
        check("at the solar eclipse of 2026 August 12 the moon stands 0.85 degrees from the sun, as its gamma of 0.898 puts it",
            MathF.Abs(Angle(Celestial.MoonGeocentric(solar), RealSun(solar)) - 0.85f) < 0.1f);
        check("at the lunar eclipse of 2026 March 3 the moon stands 0.36 degrees from the sun's opposite, as its gamma of -0.377 puts it",
            MathF.Abs(Angle(Celestial.MoonGeocentric(lunar), -RealSun(lunar)) - 0.36f) < 0.1f);
        float distance = Celestial.MoonGeocentric(solar).Length();
        check("the moon's distance stays between 55.9 and 63.8 earth radii", distance > 55.9f && distance < 63.8f);

        // In the game's sky: the phase repeats every synodic month, and the moon reaches the meridian about 50 minutes
        // later each day.
        float Illuminated(double days) => Celestial.Place(frame, days, TowardSun(0f)).MoonIlluminated;
        float[] month = Enumerable.Range(0, 60).Select(day => Illuminated(day)).ToArray();
        int fullest = Array.IndexOf(month, month.Take(30).Max()), next = 30 + Array.IndexOf(month.Skip(30).ToArray(), month.Skip(30).Max());
        check("the moon is full again 29 or 30 days later, near fully lit, and a fortnight off it near dark",
            next - fullest is 29 or 30 && month[fullest] > 0.97f && month[(fullest + 15) % 60] < 0.05f);
        double drift = 0;
        for (int day = 0; day < 29; day++)
        {
            float h0 = frame.HourAngle(Celestial.FromUnity(Celestial.Place(frame, day, TowardSun(0f)).TowardMoon));
            float h1 = frame.HourAngle(Celestial.FromUnity(Celestial.Place(frame, day + 1, TowardSun(0f)).TowardMoon));
            drift += ((Degrees(h0 - h1) % 360) + 540) % 360 - 180;
        }
        check("the moon reaches the meridian about 50 minutes later each day", MathF.Abs((float)(drift / 29 / 15 * 60) - 50.5f) < 2f);
        float[] declinations = Enumerable.Range(0, 28).Select(day => Degrees(MathF.Asin(Vec3.Dot(Celestial.FromUnity(Celestial.Place(frame, day, TowardSun(0f)).TowardMoon), frame.North)))).ToArray();
        check("over a month the moon's declination swings past 25 degrees either way", declinations.Max() > 25 && declinations.Min() < -25);
        // The main light: the sun's light by day, the moon's at night, as the add-on's world light gives them.
        Vec3 Up(float degrees, float azimuth = 0) => new(MathF.Cos(degrees * MathF.PI / 180) * MathF.Cos(azimuth), MathF.Sin(degrees * MathF.PI / 180),
            MathF.Cos(degrees * MathF.PI / 180) * MathF.Sin(azimuth));
        CelestialSky At(float sun, float moon) => noon with { TowardSun = Up(sun), TowardMoon = Up(moon, 2f) };
        var sunLight = new Vec3(1.1f, 0.9f, 0.7f);
        var moonLight = new Vec3(0.02f, 0.018f, 0.015f);
        var daylit = CelestialLight.Of(At(30, 50), sunLight, moonLight);
        check("by day the main light shines from the sun with the sun's light", Angle(daylit.Toward, Up(30)) < 0.01f && daylit.Light == sunLight);
        var moonlit = CelestialLight.Of(At(-10, 40), Vec3.Zero, moonLight);
        check("at night it shines from the moon with the moon's light", Angle(moonlit.Toward, Up(40, 2f)) < 0.01f && moonlit.Light == moonLight);
        var moonless = CelestialLight.Of(At(-10, -30), Vec3.Zero, Vec3.Zero);
        check("with the moon down it shines from 75 degrees up on the moon's side, with no light", MathF.Abs(Degrees(MathF.Asin(moonless.Toward.Y)) - 75) < 0.1f
            && Vec3.Dot(moonless.Toward, Up(0, 2f)) > 0 && moonless.Light == Vec3.Zero);
        var dusk = CelestialLight.Of(At(-5, 40), Vec3.Zero, moonLight);
        check("with the sun 5 degrees down it is halfway to the moon's light, and its direction never lights from below",
            Vec3.Distance(dusk.Light, moonLight * 0.5f) < 1e-6f && dusk.Toward.Y > 0.01f && CelestialLight.Of(At(-2, 40), Vec3.Zero, moonLight).Toward.Y > 0.01f);
        check("the spherical blend of opposite directions goes by the zenith", Vec3.Distance(CelestialLight.Slerp(Vec3.UnitX, -Vec3.UnitX, 0.5f), Vec3.UnitY) < 1e-4f);

        var evening = Celestial.Place(frame, 21 / 24.0, TowardSun(21f));
        check("on the evening of day 0 a half-lit moon stands 35.7 degrees up at 21:00, and it is full on day 8",
            MathF.Abs(evening.MoonIlluminated - 0.5f) < 0.03f && MathF.Abs(Degrees(MathF.Asin(evening.TowardMoon.Y)) - 35.7f) < 0.3f && fullest == 8);
    }
}
