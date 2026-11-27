using Il2CppInterop.Runtime;
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
        // Night brightness: how far the exposure follows the light as it fails past the sun at 6 degrees down, the end of
        // the blue hour. 0% follows it 0.7 of the way, a dark night, 100% all the way, a night as bright as the blue hour's
        // end, and 200% 1.3 of the way, a moonless night about as bright as a sunset.
        private static float TwilightPower => 0.7f + 0.3f * EnhancementsControl.RunningValues[Enhancement.NightBrightness] / 100f;
        // The add-on's most, so Night brightness alone sets how bright a night is, a moonless one too.
        public static float NightStops { get; set; } = 30f;

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
                // The game's own light, which the characters keep but for the sun's shade.
                if (sky != null && sky.MainLight is { } main)
                {
                    var direct = Linear(main.color) * main.intensity;
                    var ambient = GameAmbient();
                    CharacterWorld = WorldOnCharacter(direct, ambient, direct, ambient, Lamps.AtCharacter(), Openness.Update());
                }
                else
                {
                    CharacterWorld = null;
                    Openness.Reset();
                }
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
            float haze = EnhancementsControl.RunningValues[Enhancement.SkyHaziness];
            var placed = frame is { } fitted ? Celestial.Place(fitted, Days, towardSun) : (CelestialSky?)null;
            var bodies = Bodies(towardSun, placed);
            if (placed is not { } known || Enhancements.WorldLight(bodies, haze) is not { } world)
            {
                Release();
                Sky = null;
                Light = null;
                CharacterWorld = null;
                Enhancements.SetCelestial(bodies);
                return;
            }
            Sky = known;
            Watch(sky, towardSun);
            // Weather's cover, and a Layered sky's overcast, let through only their share of the sun's and the moon's direct light.
            var running = EnhancementsControl.RunningValues;
            bool layered = EnhancementsModel.CloudsRun(running) && (int)running[Enhancement.CloudType] == EnhancementsModel.LayeredClouds;
            float cover = WeatherFollow.Sun * (layered ? LayeredSky.SunShare(LayeredSky.Current) : 1f);
            Light = CelestialLight.Of(known, new Vec3(world.SunR, world.SunG, world.SunB) * cover, new Vec3(world.MoonR, world.MoonG, world.MoonB) * cover);
            Drive();
            // The game keeps its ambient light up through the night, so the ambient pass takes it at the physical sky's
            // share of a day's sky light over the game's own share.
            float gameAmbient = AmbientLuminance(RenderSettings.ambientSkyColor, RenderSettings.ambientGroundColor);
            float gameShare = gameAmbient / dayAmbient;
            bodies.AmbientScale = gameShare > 1e-4f ? Math.Clamp(world.SkyShare / gameShare, 0f, 4f) : 1f;
            AmbientScale = bodies.AmbientScale;
            // Characters keep the game's light, its night light and day-bright ambient: they take the share of it the world
            // gets, our direct light and the scaled ambient over the game's direct light and its ambient.
            // Only the sky's share dims, by the sky the character sees: the game's lamps near it keep lighting it as before.
            var ours = Light.Value.Light;
            float ourDirect = 0.2126f * ours.X + 0.7152f * ours.Y + 0.0722f * ours.Z;
            float gameDirect = Luminance(new Vec3(gameColor.r, gameColor.g, gameColor.b)) * gameIntensity;
            float gameSky = gameDirect + gameAmbient;
            float outdoors = gameSky > 1e-4f ? Math.Clamp((ourDirect + AmbientScale * gameAmbient) / gameSky, 0f, 1f) : 1f;
            bodies.VegetationScale = outdoors;
            // The game's own time, which stands still while it is paused, so a traced shot sees still water.
            waterSeconds = (waterSeconds + Time.deltaTime) % 100000f;
            bodies.WaterSeconds = waterSeconds;
            SendLamps();
            float open = Openness.Update();
            float lamps = Lamps.AtCharacter();
            float skyKept = gameSky * (open * outdoors + (1 - open));
            CharacterScale = gameSky + lamps > 1e-4f ? Math.Clamp((skyKept + lamps) / (gameSky + lamps), 0f, 1f) : 1f;
            var gameAmbientColor = GameAmbient();
            CharacterWorld = WorldOnCharacter(ours, gameAmbientColor * AmbientScale, Linear(gameColor) * gameIntensity, gameAmbientColor, lamps, open);
            LampLight = lamps;
            SkyOpenness = open;
            Enhancements.SetCelestial(bodies);
        }

        // Moon day: the day of the moon's cycle from the new moon, 0 to MoonDays - 1, from where the moon stands against the
        // sun: its age is its elongation east of the sun over a turn, in synodic months.
        public const int MoonDays = 30;
        private const float SynodicMonth = 29.530589f;

        public static int MoonDay => Sky is { } sky ? Math.Clamp((int)(MoonAge(sky) * SynodicMonth), 0, MoonDays - 1) : 0;

        // Moves the day count by as many days as the moon day moves.
        public static void SetMoonDay(int day) => SetDays(days + day - MoonDay);

        // The line under Moon day: the phase's name and how much of the moon is lit.
        public static string MoonPhase()
        {
            if (Sky is not { } sky)
                return "";
            float age = MoonAge(sky), lit = sky.MoonIlluminated;
            string name = age < 0.03f || age > 0.97f ? "New moon" : age < 0.22f ? "Waxing crescent" : age < 0.28f ? "First quarter"
                : age < 0.47f ? "Waxing gibbous" : age < 0.53f ? "Full moon" : age < 0.72f ? "Waning gibbous" : age < 0.78f ? "Last quarter" : "Waning crescent";
            return $"{name}, {lit * 100f:0}% lit";
        }

        // The moon's age as a share of its cycle, 0 new and 0.5 full: its right ascension's lead over the sun's, a turn round.
        private static float MoonAge(CelestialSky sky)
        {
            float RightAscension(Vec3 v) => MathF.Atan2(Vec3.Dot(v, sky.StarsY), Vec3.Dot(v, sky.StarsX));
            float lead = RightAscension(sky.TowardMoon) - RightAscension(sky.TowardSun);
            return ((lead / (2 * MathF.PI)) % 1f + 1f) % 1f;
        }

        private static float waterSeconds;

        // The game's lamps near the camera, sent again each second: the night gives back their light on vegetation.
        private const float LampsEvery = 1f;
        private static float lampsAt;

        private static void SendLamps()
        {
            var camera = CameraTools.maincam;
            if (Time.unscaledTime < lampsAt || !camera)
                return;
            lampsAt = Time.unscaledTime + LampsEvery;
            Enhancements.SetGameLights(EnhancementsShot.GameLightsNear(camera.transform.position));
        }

        // The ambient pass's multiple this frame, for the probe.
        public static float AmbientScale { get; private set; } = 1f;

        // The share of the game's light the characters keep this frame: 1 while the physical sky is off.
        public static float CharacterScale { get; private set; } = 1f;
        // For the probe: the game's lamps' light on the active character, and the share of the sky it sees.
        public static float LampLight { get; private set; }
        public static float SkyOpenness { get; private set; } = 1f;

        // The world's light on characters this frame, for the character pass: null without a sky to read.
        public static CharacterWorldLight? CharacterWorld { get; private set; }

        // Out in the open the character takes our sun, our sky and the lamps near it in place of the game's light; under a
        // roof it keeps the game's, as the game lights its interiors itself.
        // The sky's light takes the ambient pass's Level, as the world's does: without it a character kept all of the game's
        // sky light while the world around it lost 40% at the default 0.6, which at dusk, lit mostly by the sky, left the
        // character far brighter than the ground beside it.
        private static CharacterWorldLight WorldOnCharacter(Vec3 sun, Vec3 sky, Vec3 gameDirect, Vec3 gameAmbient, float lamps, float open)
        {
            var values = EnhancementsControl.RunningValues;
            sky *= values.On(Enhancement.AmbientLight) ? values[Enhancement.AmbientLevel] : 1f;
            var game = gameDirect + gameAmbient + new Vec3(lamps);
            return new CharacterWorldLight(ToUnity(sun * open), ToUnity((sky + new Vec3(lamps)) * open + game * (1 - open)), ToUnity(game));
        }

        private static Vec3 Linear(Color c)
        {
            var l = c.linear;
            return new Vec3(l.r, l.g, l.b);
        }

        private static Vec3 GameAmbient() => Linear(RenderSettings.ambientSkyColor) + Linear(RenderSettings.ambientGroundColor);

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
                // Held while a traced shot or live tracing runs: the server's day goes on under the held hour, and its turn
                // moved the moon a day's 12 degrees, which built the trace's sky again and started its samples over. A hold
                // that ended at convergence let the day catch up as the lens samples began (2026-11-27).
                if (days != (int)clock.GameDay && !EnhancementsShot.HoldsSky && !LiveTrace.On)
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
                DayLight = dayLight, TwilightPower = TwilightPower, NightStops = NightStops, Days = (float)Days, AmbientScale = 1f, VegetationScale = 1f,
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

    // How much of the sky the active character sees: rays over the upper half from above its head, a few each frame, the
    // share that reaches 80 meters unblocked, eased over about a second. 1 in the open, 0 under a roof.
    internal static class Openness
    {
        private const int Rings = 3, PerRing = 8, PerFrame = 8;
        private const float Reach = 80f, AboveFeet = 2.2f, EaseRate = 3f;
        private static readonly Vec3[] Directions = Enumerable.Range(0, Rings * PerRing).Select(i =>
        {
            float elevation = (25 + i / PerRing * 25) * MathF.PI / 180, azimuth = (i % PerRing * 45 + i / PerRing * 15) * MathF.PI / 180;
            return new Vec3(MathF.Cos(elevation) * MathF.Cos(azimuth), MathF.Sin(elevation), MathF.Cos(elevation) * MathF.Sin(azimuth));
        }).ToArray();
        private static readonly bool[] open = Enumerable.Repeat(true, Rings * PerRing).ToArray();
        private static int next;
        private static float eased = 1f;
        private static bool fresh = true;

        public static float Update()
        {
            var avatar = Character.Active();
            if (!avatar)
                return eased;
            var from = avatar.position + Vector3.up * AboveFeet;
            int rays = fresh ? Directions.Length : PerFrame;
            for (int k = 0; k < rays; k++)
            {
                var d = Directions[next];
                open[next] = !Physics.Raycast(from, new Vector3(d.X, d.Y, d.Z), Reach, -1L, QueryTriggerInteraction.Ignore);
                next = (next + 1) % Directions.Length;
            }
            float target = open.Count(o => o) / (float)open.Length;
            eased = fresh ? target : eased + (target - eased) * Math.Min(1f, Time.unscaledDeltaTime * EaseRate);
            fresh = false;
            return eased;
        }

        public static void Reset()
        {
            fresh = true;
            eased = 1f;
        }
    }

    // The game's own lamps near the active character (point and spot Lights), found again each second, and their light on
    // it as Genshin lights a surface facing them (shaders/local_light.hlsli): color times intensity to the power 2.2, over
    // 1 + 25 x^2 with x the distance over the range, faded over the last fifth of the range, a spot's cone ramped.
    internal static class Lamps
    {
        private const float Reach = 40f, FindEvery = 1f, AboveFeet = 1.1f;
        private static readonly List<Light> near = new();
        private static float findAt;

        public static float AtCharacter()
        {
            var avatar = Character.Active();
            if (!avatar)
                return 0f;
            var at = avatar.position + Vector3.up * AboveFeet;
            if (Time.unscaledTime >= findAt)
            {
                findAt = Time.unscaledTime + FindEvery;
                near.Clear();
                foreach (var o in UnityEngine.Object.FindObjectsOfType(Il2CppType.Of<Light>()))
                {
                    var light = o.Cast<Light>();
                    if (light.type is LightType.Point or LightType.Spot && !Lights.IsOurs(light) && !SkillLight.IsOurs(light)
                        && Vector3.Distance(light.transform.position, at) <= Math.Max(light.range, 0f) + 1f && light.range <= Reach * 4)
                        near.Add(light);
                }
            }
            float sum = 0f;
            foreach (var light in near)
            {
                if (light == null || !light.isActiveAndEnabled || light.intensity <= 0 || light.range <= 0)
                    continue;
                var v = light.transform.position - at;
                float d = Math.Max(v.magnitude, 1e-3f), x = d / light.range;
                if (x >= 1)
                    continue;
                float spot = 1f;
                if (light.type == LightType.Spot)
                {
                    float cosOuter = MathF.Cos(light.spotAngle * MathF.PI / 360f), cosInner = MathF.Cos(light.spotInnerAngle * MathF.PI / 360f);
                    float c = Vector3.Dot(-v / d, light.transform.forward);
                    spot = Math.Clamp((c - cosOuter) / Math.Max(cosInner - cosOuter, 1e-4f), 0f, 1f);
                }
                var color = light.color;
                float luminance = 0.2126f * Linear(color.r) + 0.7152f * Linear(color.g) + 0.0722f * Linear(color.b);
                sum += luminance * MathF.Pow(light.intensity, 2.2f) * Math.Clamp((1 - x) * 5f, 0f, 1f) * spot / (1 + 25 * x * x);
            }
            return sum;
        }

        private static float Linear(float v) => v <= 0.04045f ? v / 12.92f : MathF.Pow((v + 0.055f) / 1.055f, 2.4f);
    }
}
