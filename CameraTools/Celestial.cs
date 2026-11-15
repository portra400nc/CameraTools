using Vec3 = System.Numerics.Vector3;

namespace CameraTools
{
    // The game's sky as a celestial sphere. The game turns its sun on an exact circle about a fixed pole and keeps its
    // declination all year, so the pole and the meridian are fitted to that circle, and the stars, the moon and the sun's
    // place among the stars hang from it. The math runs in a right-handed copy of Unity's world, (x, y, -z): Unity's world
    // is left-handed, and only in a right-handed frame does the sky's turn give the physical north pole. The game's sky
    // turns out to be a southern one, about 12.6 degrees south.
    internal readonly record struct CelestialFrame(Vec3 North, Vec3 Meridian, Vec3 West, float SunDeclination)
    {
        // Hours at which the fit reads the sun, and one more that checks it.
        private static readonly float[] FitHours = { 0f, 8f, 16f };
        private const float CheckHour = 4f;
        // The largest error, in radians, of the checked hour's sun off the fitted circle.
        private const float FitTolerance = 0.01f;

        public float Latitude => MathF.Asin(Math.Clamp(North.Y, -1f, 1f));

        // From the game's direction toward its sun at a game hour, in Unity's world. Null when the sun's path is no circle
        // about a fixed pole, or too small to fit.
        public static CelestialFrame? Fit(Func<float, Vec3> towardSunAt)
        {
            var a = Celestial.FromUnity(towardSunAt(FitHours[0]));
            var b = Celestial.FromUnity(towardSunAt(FitHours[1]));
            var c = Celestial.FromUnity(towardSunAt(FitHours[2]));
            var normal = Vec3.Cross(b - a, c - b);
            if (normal.LengthSquared() < 1e-6f)
                return null;
            var pole = Vec3.Normalize(normal);
            // The sky turns about the north pole by the left hand: a star moves along -north x star.
            if (Vec3.Dot(Vec3.Cross(pole, a), b - a) > 0)
                pole = -pole;
            float declination = MathF.Asin(Math.Clamp(Vec3.Dot(a, pole), -1f, 1f));
            var check = Celestial.FromUnity(towardSunAt(CheckHour));
            if (MathF.Abs(MathF.Asin(Math.Clamp(Vec3.Dot(check, pole), -1f, 1f)) - declination) > FitTolerance)
                return null;
            var up = Vec3.UnitY;
            var meridian = up - Vec3.Dot(up, pole) * pole;
            meridian = meridian.LengthSquared() < 1e-8f ? Vec3.Normalize(Vec3.Cross(pole, Vec3.UnitX)) : Vec3.Normalize(meridian);
            return new CelestialFrame(pole, meridian, Vec3.Cross(meridian, pole), declination);
        }

        // The hour angle of a direction (right-handed), west of the upper meridian, in radians.
        public float HourAngle(Vec3 direction) => MathF.Atan2(Vec3.Dot(direction, West), Vec3.Dot(direction, Meridian));
    }

    // Where the sky's bodies are at one moment, in Unity's world. StarsX, StarsY and StarsZ are the J2000 equatorial axes
    // (x toward the March equinox, z toward the north pole): a star at right ascension a and declination d is along
    // cos d cos a StarsX + cos d sin a StarsY + sin d StarsZ. MoonNorth is the moon's north, which turns its face.
    internal readonly record struct CelestialSky(Vec3 TowardSun, Vec3 TowardMoon, Vec3 MoonNorth, float MoonPhaseAngle, float MoonIlluminated,
        Vec3 StarsX, Vec3 StarsY, Vec3 StarsZ);

    internal static class Celestial
    {
        // A tropical year, the March equinox to the next, in days.
        public const double YearDays = 365.2422;
        // The obliquity of the ecliptic at J2000.
        private const double Obliquity = 23.4393 * Math.PI / 180;
        // Day 0 is noon UT of 2026 April 22, whose evening shows a first-quarter moon high in the sky.
        public const double EpochJulianDay = 2461153.0;
        // Earth radii.
        private const double MoonParallaxRadius = 1.0;

        public static Vec3 FromUnity(Vec3 v) => new(v.X, v.Y, -v.Z);

        public static Vec3 ToUnity(Vec3 v) => new(v.X, v.Y, -v.Z);

        // The sky at days since the epoch (whole days plus the hour's share), with the game's own direction toward its sun
        // (Unity's world), which places the meridian: the stars turn exactly with the drawn sun.
        public static CelestialSky Place(CelestialFrame frame, double days, Vec3 gameTowardSun)
        {
            var sun = Vec3.Normalize(FromUnity(gameTowardSun));
            // The sun keeps the frame's declination and gains a turn on the stars each year.
            double sunRightAscension = EpochSunRightAscension(frame.SunDeclination) + 2 * Math.PI * days / YearDays;
            double siderealTime = frame.HourAngle(sun) + sunRightAscension;
            float cosT = (float)Math.Cos(siderealTime), sinT = (float)Math.Sin(siderealTime);
            var starsX = cosT * frame.Meridian + sinT * frame.West;
            var starsY = sinT * frame.Meridian - cosT * frame.West;
            var starsZ = frame.North;
            Vec3 World(Vec3 equatorial) => equatorial.X * starsX + equatorial.Y * starsY + equatorial.Z * starsZ;

            var geocentric = MoonGeocentric(EpochJulianDay + days);
            double latitude = frame.Latitude;
            var zenith = new Vec3((float)(Math.Cos(latitude) * Math.Cos(siderealTime)), (float)(Math.Cos(latitude) * Math.Sin(siderealTime)), (float)Math.Sin(latitude));
            var moon = Vec3.Normalize(World(Vec3.Normalize(geocentric - (float)MoonParallaxRadius * zenith)));
            var eclipticNorth = new Vec3(0, (float)-Math.Sin(Obliquity), (float)Math.Cos(Obliquity));
            float elongation = MathF.Acos(Math.Clamp(Vec3.Dot(moon, sun), -1f, 1f));
            float phaseAngle = MathF.PI - elongation;
            return new CelestialSky(ToUnity(sun), ToUnity(moon), ToUnity(Vec3.Normalize(World(eclipticNorth))), phaseAngle,
                (1 + MathF.Cos(phaseAngle)) / 2, ToUnity(starsX), ToUnity(starsY), ToUnity(starsZ));
        }

        // The right ascension of a sun on the real ecliptic with this declination, northward, in its spring.
        public static double EpochSunRightAscension(double declination)
        {
            double longitude = Math.Asin(Math.Clamp(Math.Sin(declination) / Math.Sin(Obliquity), -1, 1));
            return Math.Atan2(Math.Cos(Obliquity) * Math.Sin(longitude), Math.Cos(longitude));
        }

        // The moon's center from the earth's in J2000 equatorial axes, in earth radii: Paul Schlyter's low-precision orbit
        // ("How to compute planetary positions") with its largest perturbations, good to a few arcminutes, the ecliptic
        // moved back to J2000 by the precession.
        public static Vec3 MoonGeocentric(double julianDay)
        {
            const double rad = Math.PI / 180;
            double d = julianDay - 2451543.5;
            double node = (125.1228 - 0.0529538083 * d) * rad, inclination = 5.1454 * rad, perigee = (318.0634 + 0.1643573223 * d) * rad;
            const double a = 60.2666, e = 0.054900;
            double anomaly = (115.3654 + 13.0649929509 * d) * rad;
            double eccentric = anomaly;
            for (int i = 0; i < 8; i++)
                eccentric -= (eccentric - e * Math.Sin(eccentric) - anomaly) / (1 - e * Math.Cos(eccentric));
            double xv = a * (Math.Cos(eccentric) - e), yv = a * Math.Sqrt(1 - e * e) * Math.Sin(eccentric);
            double v = Math.Atan2(yv, xv), r = Math.Sqrt(xv * xv + yv * yv);
            double xh = r * (Math.Cos(node) * Math.Cos(v + perigee) - Math.Sin(node) * Math.Sin(v + perigee) * Math.Cos(inclination));
            double yh = r * (Math.Sin(node) * Math.Cos(v + perigee) + Math.Cos(node) * Math.Sin(v + perigee) * Math.Cos(inclination));
            double zh = r * Math.Sin(v + perigee) * Math.Sin(inclination);
            double longitude = Math.Atan2(yh, xh), latitude = Math.Atan2(zh, Math.Sqrt(xh * xh + yh * yh));

            double sunAnomaly = (356.0470 + 0.9856002585 * d) * rad, sunPerigee = (282.9404 + 4.70935e-5 * d) * rad;
            double sunLongitude = sunAnomaly + sunPerigee, moonLongitude = node + perigee + anomaly;
            double elongation = moonLongitude - sunLongitude, argument = moonLongitude - node;
            longitude += rad * (-1.274 * Math.Sin(anomaly - 2 * elongation) + 0.658 * Math.Sin(2 * elongation) - 0.186 * Math.Sin(sunAnomaly)
                - 0.059 * Math.Sin(2 * anomaly - 2 * elongation) - 0.057 * Math.Sin(anomaly - 2 * elongation + sunAnomaly)
                + 0.053 * Math.Sin(anomaly + 2 * elongation) + 0.046 * Math.Sin(2 * elongation - sunAnomaly) + 0.041 * Math.Sin(anomaly - sunAnomaly)
                - 0.035 * Math.Sin(elongation) - 0.031 * Math.Sin(anomaly + sunAnomaly) - 0.015 * Math.Sin(2 * argument - 2 * elongation)
                + 0.011 * Math.Sin(anomaly - 4 * elongation));
            latitude += rad * (-0.173 * Math.Sin(argument - 2 * elongation) - 0.055 * Math.Sin(anomaly - argument - 2 * elongation)
                - 0.046 * Math.Sin(anomaly + argument - 2 * elongation) + 0.033 * Math.Sin(argument + 2 * elongation) + 0.017 * Math.Sin(2 * anomaly + argument));
            r += -0.58 * Math.Cos(anomaly - 2 * elongation) - 0.46 * Math.Cos(2 * elongation);
            // The ecliptic of date back to J2000: 50.29 arcseconds a year of general precession.
            longitude -= 3.8239e-5 * rad * d;

            double cl = Math.Cos(latitude);
            double x = r * cl * Math.Cos(longitude), y = r * cl * Math.Sin(longitude), z = r * Math.Sin(latitude);
            return new Vec3((float)x, (float)(y * Math.Cos(Obliquity) - z * Math.Sin(Obliquity)), (float)(y * Math.Sin(Obliquity) + z * Math.Cos(Obliquity)));
        }
    }
}
