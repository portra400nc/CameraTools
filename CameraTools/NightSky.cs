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
        // The game's measured light (2026-11-15) until it shows its own: its day sun's luminance on a white surface.
        private const float MeasuredDayLight = 1.5f;
        // The game's ambient light at noon (2026-11-15, RenderSettings' sky and ground colors), whose luminance the game's
        // keeps through the night: the physical sky's share of a day's sky light is taken over the game's share of it.
        private static readonly Vec3 MeasuredDayAmbientSky = new(0.482f, 0.758f, 0.939f), MeasuredDayAmbientGround = new(0.448f, 0.562f, 0.675f);
        // The sun's sine above which the game's ambient is a day's.
        private const float DaySun = 0.342f;

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
        private static float dayAmbient = AmbientLuminance(MeasuredDayAmbientSky, MeasuredDayAmbientGround);

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
                CharacterScale = 1f;
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
            var placed = frame is { } fitted ? Celestial.Place(fitted, Days, towardSun) : (CelestialSky?)null;
            var bodies = Bodies(towardSun, placed);
            if (placed is not { } known || Enhancements.WorldLight(bodies, haze) is not { } world)
            {
                Release();
                Sky = null;
                Light = null;
                Enhancements.SetCelestial(bodies);
                return;
            }
            Sky = known;
            Watch(sky, towardSun);
            Light = CelestialLight.Of(known, new Vec3(world.SunR, world.SunG, world.SunB), new Vec3(world.MoonR, world.MoonG, world.MoonB));
            Drive();
            // The game keeps its ambient light up through the night, so the ambient pass takes it at the physical sky's
            // share of a day's sky light over the game's own share.
            float gameAmbient = AmbientLuminance(RenderSettings.ambientSkyColor, RenderSettings.ambientGroundColor);
            float gameShare = gameAmbient / dayAmbient;
            bodies.AmbientScale = gameShare > 1e-4f ? Math.Clamp(world.SkyShare / gameShare, 0f, 4f) : 1f;
            AmbientScale = bodies.AmbientScale;
            // Characters keep the game's light, its night light and day-bright ambient: they take the share of it the world
            // gets, our direct light and the scaled ambient over the game's direct light and its ambient.
            var ours = Light.Value.Light;
            float ourDirect = 0.2126f * ours.X + 0.7152f * ours.Y + 0.0722f * ours.Z;
            float gameDirect = Luminance(new Vec3(gameColor.r, gameColor.g, gameColor.b)) * gameIntensity;
            float gameTotal = gameDirect + gameAmbient;
            CharacterScale = gameTotal > 1e-4f ? Math.Clamp((ourDirect + AmbientScale * gameAmbient) / gameTotal, 0f, 1f) : 1f;
            Enhancements.SetCelestial(bodies);
        }

        // The ambient pass's multiple this frame, for the probe.
        public static float AmbientScale { get; private set; } = 1f;

        // The share of the game's light the characters keep this frame: 1 while the physical sky is off.
        public static float CharacterScale { get; private set; } = 1f;

        // In C# alone: it also runs in the type's initializer, before any of Unity's calls can be made.
        private static float AmbientLuminance(Color sky, Color ground) => AmbientLuminance(new Vec3(sky.r, sky.g, sky.b), new Vec3(ground.r, ground.g, ground.b));

        private static float AmbientLuminance(Vec3 sky, Vec3 ground) => Luminance(sky) + Luminance(ground);

        // A gamma-encoded color's linear luminance.
        private static float Luminance(Vec3 c)
        {
            static float Linear(float v) => v <= 0.04045f ? v / 12.92f : MathF.Pow((v + 0.055f) / 1.055f, 2.4f);
            return 0.2126f * Linear(c.X) + 0.7152f * Linear(c.Y) + 0.0722f * Linear(c.Z);
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
        private static void Watch(EnviroSky sky, Vec3 towardSun)
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
                if (!sky.IsNight)
                    dayLight = 0.2126f * linear.r + 0.7152f * linear.g + 0.0722f * linear.b;
            }
            if (towardSun.Y > DaySun)
                dayAmbient = AmbientLuminance(RenderSettings.ambientSkyColor, RenderSettings.ambientGroundColor);
            if (fresh)
                Log($"the main light follows the sky's bodies; the game's day light {dayLight:0.###}, its day ambient {dayAmbient:0.###}.");
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

        private static EnhancementsCelestial Bodies(Vec3 towardSun, CelestialSky? placed)
        {
            var c = new EnhancementsCelestial
            {
                SunX = towardSun.X, SunY = towardSun.Y, SunZ = towardSun.Z,
                DayLight = dayLight, TwilightPower = TwilightPower, NightStops = NightStops, Days = (float)Days, AmbientScale = 1f,
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
            return c;
        }

        private static Vec3 ToNumerics(Vector3 v) => new(v.x, v.y, v.z);

        private static Vector3 ToUnity(Vec3 v) => new(v.X, v.Y, v.Z);

        private static void Log(string message) => Melon<CameraTools>.Logger.Msg($"Night sky: {message}");
    }
}
