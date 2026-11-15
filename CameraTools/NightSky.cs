using CameraToolsEnhancements;
using MelonLoader;
using MoleMole;
using UnityEngine;
using Vec3 = System.Numerics.Vector3;

namespace CameraTools
{
    // While Ambient light's Sky is Physical: places the sky's bodies (Celestial.cs) for the hour and the day count, turns the
    // game's main light, the shadows' light, and the characters' light to them, and tells the Enhancements add-on, which
    // draws them. Puts the game's light back once the physical sky stops.
    internal static class NightSky
    {
        // The game's measured lights (2026-11-15) until it shows its own: its day sun's luminance on a white surface, and
        // its night light, color (0.47, 0.73, 0.92) at intensity 1.2, in linear color times intensity.
        private const float MeasuredDayLight = 1.5f;
        private static readonly Vec3 MeasuredNightLight = new(0.226f, 0.608f, 1.0f);

        // The provisional exposure (core/sky.h, SkyExposure) until the night has its own. Probe items tune them.
        public static float TwilightPower { get; set; } = 0.85f;
        public static float NightStops { get; set; } = 17f;

        private static MelonPreferences_Entry<int> savedDays;
        private static int days;
        private static float? lastHour;

        private static IntPtr fittedSky;
        private static (float, float, float) fittedAngles;
        private static CelestialFrame? frame;

        // The light as the game last set it, and as we last set it.
        private static Light held;
        private static Quaternion gameRotation, ourRotation;
        private static Color gameColor, ourColor;
        private static float gameIntensity, ourIntensity;
        private static float dayLight = MeasuredDayLight;
        private static Vec3 nightLight = MeasuredNightLight;

        // The sky whose fog we switched off: the game's distance and height fog take its painted time-of-day colors (gold at
        // dawn, cyan at night) over the whole view, which the physical sky's light contradicts.
        private static IntPtr fogOffIn;
        private static readonly Il2CppSystem.Object FogOwner = new();

        // This frame's sky, null while the physical sky is off or the game has no sky.
        public static CelestialSky? Sky { get; private set; }
        public static SkyLightState? Light { get; private set; }
        public static double Days => days + (lastHour ?? 0f) / 24.0;

        public static void Load()
        {
            savedDays = MelonPreferences.CreateCategory("CameraToolsEnhancements").CreateEntry("SkyDays", 0,
                description: "The physical sky's day count, which the moon's phase follows. It gains a day each time the sky's hour passes midnight.");
            days = savedDays.Value;
        }

        // Sets the day count, for the probe until the World tab has a row.
        public static void SetDays(int value)
        {
            days = value;
            Save();
        }

        // Once a frame after the time of day: on is whether the physical sky draws.
        public static void Update(bool on)
        {
            var sky = EnviroSky.Instance;
            if (!on || sky == null)
            {
                if (sky != null && sky.Pointer == fogOffIn)
                {
                    sky.SetFogEnable(true, FogOwner);
                    Log("the game's fog is back.");
                }
                fogOffIn = IntPtr.Zero;
                Release();
                Sky = null;
                Light = null;
                return;
            }
            if (sky.Pointer != fogOffIn)
            {
                sky.SetFogEnable(false, FogOwner);
                fogOffIn = sky.Pointer;
                Log("the game's fog is off.");
            }
            // The hour the sky shows, which trails a new lock by a moment, so the moon and the stars stay with its sun.
            float hour = sky.curTimeOfDay24;
            Count(sky, hour);
            var config = sky.skySettings.config;
            var angles = (config.sunAngle, config.sunInclination, config.sunInclinationOffset);
            if (sky.Pointer != fittedSky || angles != fittedAngles)
            {
                fittedSky = sky.Pointer;
                fittedAngles = angles;
                frame = CelestialFrame.Fit(h => ToNumerics(-sky.GetSunLightDirection(h, config.sunAngle, config.sunInclination, config.sunInclinationOffset)));
                Log(frame is { } f ? $"the sky's frame: {f.Latitude * 180 / MathF.PI:0.00} degrees of latitude, the sun {f.SunDeclination * 180 / MathF.PI:0.00} degrees north."
                    : "the sky's sun path fits no frame, so the moon and the stars are not drawn.");
            }
            var towardSun = ToNumerics(-sky.GetSunLightDirection(sky.curTimeOfDay24, config.sunAngle, config.sunInclination, config.sunInclinationOffset));
            float haze = EnhancementsControl.RunningValues[Enhancement.AmbientHaze];
            if (frame is not { } fitted || Enhancements.Sunlight(towardSun.Y, haze) is not { } through || Enhancements.Sunlight(1f, haze) is not { } zenith)
            {
                Release();
                Sky = null;
                Light = null;
                Push(towardSun, null);
                return;
            }
            var placed = Celestial.Place(fitted, Days, towardSun);
            Sky = placed;
            Watch(sky);
            Light = CelestialLight.Of(placed, new Vec3(through.R, through.G, through.B), new Vec3(zenith.R, zenith.G, zenith.B), dayLight, nightLight);
            Drive();
            Push(towardSun, placed);
        }

        // Camera.onPreCull of the main camera: puts the light back where we set it, should the game have turned it since.
        public static void PreCull()
        {
            if (held != null && held && Light != null && Quaternion.Angle(held.transform.rotation, ourRotation) > 0.01f)
                held.transform.rotation = ourRotation;
        }

        // Where the characters' light comes from: far along the main light from the active character, or null to leave it
        // to the game.
        public static Vector3? CharacterLightFrom()
        {
            var avatar = Character.Active();
            return Light is { } light && avatar ? avatar.position + Vector3.up * 1.1f + ToUnity(light.Toward) * 1000f : null;
        }

        // The own count gains a day each time the hour passes midnight forward and loses one going back; a jump of the
        // lock counts the shorter way round. On a server that syncs the game's clock, its day.
        private static void Count(EnviroSky sky, float hour)
        {
            var clock = sky.gameTime;
            if (clock != null && clock._playerGameTimeSynced)
            {
                if (days != (int)clock.GameDay)
                {
                    days = (int)clock.GameDay;
                    Save();
                }
                lastHour = hour;
                return;
            }
            if (lastHour is float last)
            {
                float step = ((hour - last) % 24f + 36f) % 24f - 12f;
                if (last + step >= 24f)
                    SetDays(days + 1);
                else if (last + step < 0f)
                    SetDays(days - 1);
            }
            lastHour = hour;
        }

        private static void Save()
        {
            if (savedDays == null || savedDays.Value == days)
                return;
            savedDays.Value = days;
            MelonPreferences.Save();
        }

        // Takes the game's main light, and reads what the game set on it whenever it differs from what we set.
        private static void Watch(EnviroSky sky)
        {
            var main = sky.MainLight;
            if (held != null && (!held || main == null || held.Pointer != main.Pointer))
                Release();
            if (main == null)
                return;
            bool fresh = held == null;
            if (fresh || Quaternion.Angle(main.transform.rotation, ourRotation) > 0.01f)
                gameRotation = main.transform.rotation;
            if (fresh || main.intensity != ourIntensity || main.color != ourColor)
            {
                gameColor = main.color;
                gameIntensity = main.intensity;
                var linear = main.color.linear * main.intensity;
                if (sky.IsNight)
                    nightLight = new Vec3(linear.r, linear.g, linear.b);
                else
                    dayLight = 0.2126f * linear.r + 0.7152f * linear.g + 0.0722f * linear.b;
            }
            if (fresh)
                Log($"the main light follows the sky's bodies; the game's day light {dayLight:0.###}, its night light ({nightLight.X:0.###}, {nightLight.Y:0.###}, {nightLight.Z:0.###}).");
            held = main;
        }

        private static void Drive()
        {
            if (held == null || Light is not { } light)
                return;
            float intensity = Math.Max(Math.Max(light.Light.X, light.Light.Y), Math.Max(light.Light.Z, 1e-4f));
            ourColor = new Color(light.Light.X / intensity, light.Light.Y / intensity, light.Light.Z / intensity, 1f).gamma;
            ourIntensity = intensity;
            ourRotation = Quaternion.LookRotation(-ToUnity(light.Toward));
            held.transform.rotation = ourRotation;
            held.color = ourColor;
            held.intensity = ourIntensity;
            // Read back, as Unity stores them, so the next frame tells the game's writes from ours.
            ourColor = held.color;
            ourIntensity = held.intensity;
            ourRotation = held.transform.rotation;
        }

        private static void Release()
        {
            if (held != null && held)
            {
                held.transform.rotation = gameRotation;
                held.color = gameColor;
                held.intensity = gameIntensity;
                Log("the main light is the game's again.");
            }
            held = null;
        }

        private static void Push(Vec3 towardSun, CelestialSky? placed)
        {
            var c = new EnhancementsCelestial
            {
                SunX = towardSun.X, SunY = towardSun.Y, SunZ = towardSun.Z,
                DayLight = dayLight, TwilightPower = TwilightPower, NightStops = NightStops, Days = (float)Days,
            };
            if (placed is { } p)
            {
                c.Stars = 1;
                (c.MoonX, c.MoonY, c.MoonZ, c.MoonPhase) = (p.TowardMoon.X, p.TowardMoon.Y, p.TowardMoon.Z, p.MoonPhaseAngle);
                (c.MoonNorthX, c.MoonNorthY, c.MoonNorthZ) = (p.MoonNorth.X, p.MoonNorth.Y, p.MoonNorth.Z);
                (c.StarsXX, c.StarsXY, c.StarsXZ) = (p.StarsX.X, p.StarsX.Y, p.StarsX.Z);
                (c.StarsYX, c.StarsYY, c.StarsYZ) = (p.StarsY.X, p.StarsY.Y, p.StarsY.Z);
                (c.StarsZX, c.StarsZY, c.StarsZZ) = (p.StarsZ.X, p.StarsZ.Y, p.StarsZ.Z);
            }
            Enhancements.SetCelestial(c);
        }

        private static Vec3 ToNumerics(Vector3 v) => new(v.x, v.y, v.z);

        private static Vector3 ToUnity(Vec3 v) => new(v.X, v.Y, v.Z);

        private static void Log(string message) => Melon<CameraTools>.Logger.Msg($"Night sky: {message}");
    }
}
