using System.Text.Json;
using System.Text.Json.Serialization;
using MelonLoader;
using MelonLoader.Utils;
using MoleMole;
using UnityEngine;
using NumericsQuaternion = System.Numerics.Quaternion;
using NumericsVector3 = System.Numerics.Vector3;

namespace CameraTools
{
    // The Lights tab's lights, shown while the free camera is on. Points and spots are Light components on objects of
    // CameraTools' own; spheres are drawn by ReLight, which runs alone while any sphere shows so the user's other ReShade
    // effects cost nothing until a screenshot turns them back on. Saved to UserData/CameraTools/Lights.json a second after
    // the last change.
    internal static class Lights
    {
        private const float PlaceAhead = 1.5f;
        private const float SaveDelay = 1f;
        // World lights hardly reach Genshin's toon-shaded characters, so a light that lights everything brings a characters-only
        // twin at this share of its intensity. On the Deck, half was a clear fill and the whole intensity blew the character out.
        private const float TwinShare = 0.5f;
        // The toon falloff the game gives its own character lights (Odette's burst lights use 0.6 to 0.7 and 0.1 to 0.18). A new
        // Light has 0 and 0, which lights the whole character evenly.
        private const float CharacterThreshold = 0.6f;
        private const float CharacterSoftness = 0.1f;
        // Move light widens the view until a spot's whole cone fits in this share of the screen's height, up to MaxMoveFov.
        private const float ConeFit = 0.8f;
        private const float MaxMoveFov = 150f;
        // Per second while Move light's FOV or roll buttons are held, on unscaled time so they work while the game is paused.
        private const float SpotAngleRate = 40f;
        private const float IntensityRate = 2f;

        private static readonly PadBinding PadA = new(PadButtons.A, PadAxis.None);
        private static readonly PadBinding PadB = new(PadButtons.B, PadAxis.None);
        // Move light's spot angle has buttons of its own, which nothing else reads while a light moves.
        public static readonly PadBinding PadNarrower = new(PadButtons.X, PadAxis.None);
        public static readonly PadBinding PadWider = new(PadButtons.Y, PadAxis.None);
        public const KeyCode KeyNarrower = KeyCode.Alpha4;
        public const KeyCode KeyWider = KeyCode.Alpha5;
        private static readonly string Folder = Path.Combine(MelonEnvironment.UserDataDirectory, "CameraTools");
        private static readonly string FilePath = Path.Combine(Folder, "Lights.json");
        private static readonly JsonSerializerOptions Json = new()
        {
            WriteIndented = true,
            NumberHandling = JsonNumberHandling.AllowNamedFloatingPointLiterals,
            Converters = { new JsonStringEnumConverter() },
        };

        public static readonly string[] KindNames = { "Point", "Spot", "Sphere" };
        public static readonly string[] ReachNames = { "Everything", "Characters only" };
        public static readonly string[] FollowNames = { "Nothing", "Camera", "Character" };
        public static readonly string[] SourceNames = { "Temperature", "Hue" };

        public static List<LightSetup> All { get; } = new();

        // -1 only while there are no lights.
        public static int Active { get; private set; } = -1;

        public static LightSetup Current => Active >= 0 ? All[Active] : null;

        public static bool HasLight => Current != null;

        // The Alternative shading row's note: on a light without it, the light that has it.
        public static string SunShadingNote
        {
            get
            {
                int owner = All.FindIndex(light => light.SunShading);
                return owner < 0 || owner == Active ? "Use the sun's shading" : $"Light {owner + 1} uses the sun's shading";
            }
        }

        // One light at a time shades characters as the sun does.
        public static void SetSunShading(bool on)
        {
            var light = Current;
            if (light == null)
                return;
            foreach (var other in All)
                other.SunShading = false;
            light.SunShading = on;
            Changed();
        }

        public static bool Markers { get; private set; } = true;

        // ReLight's ambient level: below 1 dims the game's own light, so the spheres take over.
        public static float Ambient { get; private set; } = 1f;

        public static bool HasSpheres => All.Exists(light => light.Kind == LightKind.Sphere);

        // A spot that casts shadows is showing.
        public static bool NeedsShadows => Shown && All.Exists(light => light.CastsShadows);

        // While spheres show, ReShade's effects stay on, after a screenshot too.
        public static bool KeepsEffectsOn => Shown && HasSpheres;

        private static bool Shown => CameraTools.freecamActive;

        public static bool Moving => move.HasValue;

        // Follow Camera keeps a light on the camera, so flying the camera as the light would change nothing.
        public static bool CanMove => Current is { Follow: not LightFollow.Camera };

        // Each light's pose in the scene this frame, by index, or null while what it follows is missing.
        private static readonly List<Pose?> placed = new();
        private static readonly List<GameLight> gameLights = new();
        private static readonly List<ReLight.Sphere> spheres = new();
        private static float? changedAt;
        private static MoveStart? move;
        // What the bridge was last told: whether spheres run, solo or beside the user's effects, and on which connection.
        private static (bool Spheres, bool Solo, int Connection)? overrideSent;
        // ReShade's effects switch from before the first sphere, put back when the last one goes.
        private static bool? effectsBefore;

        private sealed class GameLight
        {
            public GameObject Object;
            public Light Light;
            // The characters-only twin of a light that lights everything, on a child object so it follows.
            public Light Twin;
            public Applied Shown;
        }

        // Move light flies the free camera as the light. B puts the light back, and either way the camera returns to the shot.
        // Frame is the frame Move light was chosen on, whose A press must not also finish the move.
        private readonly record struct MoveStart(Pose Light, float Intensity, float SpotAngle, Vector3 ShotPosition, Quaternion ShotRotation,
            float ShotFov, int Frame);

        // A light's settings as last written to its component.
        private readonly record struct Applied(LightKind Kind, LightReach Reach, float Intensity, float Range, float SpotAngle,
            float InnerAngle, NumericsVector3 Color, bool CastsShadows);

        public static string LightNote
        {
            get
            {
                var light = Current;
                if (light == null)
                    return "Add a light to start";
                string reach = light.Kind == LightKind.Sphere ? $"{light.Radius:0.00} m" : light.CharactersOnly ? "characters only" : "everything";
                string follow = light.Follow switch
                {
                    LightFollow.Camera => "follows the camera",
                    LightFollow.Character => "follows the character",
                    _ => "fixed",
                };
                return $"{KindNames[(int)light.Kind]} · {reach} · {follow}";
            }
        }

        public static void Load()
        {
            if (!File.Exists(FilePath))
                return;
            try
            {
                var file = JsonSerializer.Deserialize<LightsFile>(File.ReadAllText(FilePath), Json);
                All.AddRange(file.Lights.Select(FromFile));
                foreach (var extra in All.Where(light => light.SunShading).Skip(1))
                    extra.SunShading = false;
                Active = All.Count == 0 ? -1 : Math.Clamp(file.Active, 0, All.Count - 1);
                Markers = file.Markers;
                Ambient = Math.Clamp(file.Ambient, 0f, 1f);
                Melon<CameraTools>.Logger.Msg($"Lights: loaded {All.Count} light{(All.Count == 1 ? "" : "s")} from {FilePath}.");
            }
            catch (Exception e)
            {
                All.Clear();
                Active = -1;
                string backup = FilePath + ".bak";
                Melon<CameraTools>.Logger.Warning($"Lights: {FilePath} could not be read ({e.Message}); moved it to {backup} and started empty.");
                try
                {
                    File.Move(FilePath, backup, true);
                }
                catch (Exception moveFailed)
                {
                    Melon<CameraTools>.Logger.Warning($"Lights: moving it aside failed: {moveFailed.Message}");
                }
            }
        }

        public static void Select(int index)
        {
            if (index < 0 || index >= All.Count || index == Active)
                return;
            Active = index;
            Changed();
        }

        public static void Add()
        {
            if (Camera() is not { } camera)
            {
                CameraUi.Toast("No camera to place the light by");
                return;
            }
            var light = new LightSetup();
            light.Local = WorldFrame().ToLocal(Ahead(camera));
            All.Add(light);
            Active = All.Count - 1;
            Changed();
            CameraUi.Toast($"Light {Active + 1} placed {PlaceAhead:0.0} m in front of the camera");
        }

        public static void Delete()
        {
            if (Current == null)
                return;
            int number = Active + 1;
            if (Active < gameLights.Count)
            {
                Destroy(gameLights[Active]);
                gameLights.RemoveAt(Active);
            }
            All.RemoveAt(Active);
            Active = All.Count == 0 ? -1 : Math.Min(Active, All.Count - 1);
            Changed();
            CameraUi.Toast($"Light {number} deleted");
        }

        public static void SetMarkers(bool on)
        {
            Markers = on;
            Changed();
        }

        public static void SetAmbient(float value)
        {
            Ambient = Math.Clamp(value, 0f, 1f);
            Changed();
        }

        // Changing what a light follows keeps it where it is now.
        public static void SetFollow(int choice)
        {
            var light = Current;
            var follow = (LightFollow)choice;
            if (light == null || light.Follow == follow)
                return;
            if (Place(light) is not { } world || FrameOf(follow) is not { } frame)
            {
                CameraUi.Toast(follow == LightFollow.Character ? "No character found to follow" : "No camera found to follow");
                return;
            }
            light.Follow = follow;
            light.Local = frame.ToLocal(world);
            Changed();
        }

        public static void MoveToCamera()
        {
            var light = Current;
            if (light == null || Camera() is not { } camera || FrameOf(light.Follow) is not { } frame)
                return;
            light.Local = frame.ToLocal(Ahead(camera));
            Changed();
            CameraUi.Toast($"Light {Active + 1} moved in front of the camera");
        }

        // The camera jumps to the light, or for a sphere to PlaceAhead behind it, because ReLight cannot draw a sphere at the lens.
        public static void StartMove()
        {
            var light = Current;
            var freecam = CameraTools.freecam;
            if (!CanMove || !CameraTools.freecamActive || freecam == null || Place(light) is not { } world)
                return;
            var (position, rotation, fov) = freecam.Pose;
            move = new MoveStart(light.Local, light.Intensity, light.SpotAngle, position, rotation, fov, Time.frameCount);
            var at = world.Position;
            if (light.Kind == LightKind.Sphere)
                at -= NumericsVector3.Transform(NumericsVector3.UnitZ, world.Rotation) * PlaceAhead;
            freecam.Snap(at.ToUnity(), world.Rotation.ToUnity(), MoveFov(light, fov));
            CameraUi.ClosePanel();
            CameraUi.Toast($"Moving light {Active + 1}");
        }

        // Puts the shot back, and the light too unless it is kept. Turning the free camera off cancels the move first.
        public static void EndMove(bool keep)
        {
            if (move is not { } start)
                return;
            move = null;
            if (Current is { } light && !keep)
            {
                light.Local = start.Light;
                light.Intensity = start.Intensity;
                light.SpotAngle = start.SpotAngle;
            }
            CameraTools.freecam?.Snap(start.ShotPosition, start.ShotRotation, start.ShotFov);
            Changed();
            CameraUi.Toast(keep ? $"Light {Active + 1} placed" : "Move cancelled");
            if (CameraTools.freecamActive)
                CameraUi.OpenPanel();
        }

        // The vertical field of view for moving a spot: the one given, or wider so the whole cone fits.
        private static float MoveFov(LightSetup light, float fov)
        {
            if (light.Kind != LightKind.Spot)
                return fov;
            float fit = 2f * MathF.Atan(MathF.Tan(light.SpotAngle * MathF.PI / 360f) / ConeFit) * 180f / MathF.PI;
            return Math.Clamp(fit, fov, Math.Max(fov, MaxMoveFov));
        }

        // A spot's cone while moving it, as radii from the screen's middle in halves of the screen's height, and its angle.
        public static (float Outer, float Inner, float Angle)? Cone(Camera camera)
        {
            if (!Moving || Current is not { Kind: LightKind.Spot } light || !camera)
                return null;
            float half = MathF.Tan(camera.fieldOfView * MathF.PI / 360f);
            float Radius(float angle) => MathF.Tan(Math.Min(angle, 179f) * MathF.PI / 360f) / half;
            return (Radius(light.SpotAngle), Radius(Math.Min(light.InnerAngle, light.SpotAngle)), light.SpotAngle);
        }

        // Move light's line at the bottom left: the light's settings, then how far it is from the character and how high.
        public static (string Name, string Values, string Depth) MoveReadout
        {
            get
            {
                var light = Current;
                if (light == null)
                    return ("", "", "");
                string values = light.Kind switch
                {
                    LightKind.Spot => $"Spot {light.SpotAngle:0}° · Inner {Math.Min(light.InnerAngle, light.SpotAngle):0}° · Intensity {light.Intensity:0.00} · View {CameraTools.settings.Fov.Value:0}°",
                    LightKind.Sphere => $"Sphere {light.Radius:0.00} m · Intensity {light.Intensity:0.00}",
                    _ => $"Point · Intensity {light.Intensity:0.00}",
                };
                string depth = "";
                if (Active < placed.Count && placed[Active] is { } pose && Character.Active() is { } avatar && avatar)
                {
                    var feet = avatar.position.ToNumerics();
                    var offset = pose.Position - feet;
                    float across = MathF.Sqrt(offset.X * offset.X + offset.Z * offset.Z);
                    depth = $"{across:0.0} m from the character · {offset.Y:0.0} m above their feet";
                }
                return ($"Light {Active + 1}", values, depth);
            }
        }

        // Every setting change goes through here, so the light is saved and its intensity stays in range.
        public static void Edit(Action<LightSetup> change)
        {
            var light = Current;
            if (light == null)
                return;
            change(light);
            light.Intensity = Math.Clamp(light.Intensity, 0f, light.IntensityLimit);
            Changed();
        }

        public static void SetHex(string text)
        {
            if (!Colors.TryParseHex(text, out var rgb))
            {
                CameraUi.Toast("Type six hex digits, like FFB46B");
                return;
            }
            var (hue, saturation) = Colors.ToHueSaturation(rgb);
            Edit(light =>
            {
                light.Source = ColorSource.Hue;
                light.Hue = hue;
                light.Saturation = saturation * 100f;
            });
        }

        // Where a light is on screen this frame, for its marker, and for a spot the point a metre along its aim. The light being
        // moved has none, since the camera looks out of it.
        public static (ScreenPoint Point, ScreenPoint? Aim)? Marker(int index, Camera camera)
        {
            if (Moving && index == Active)
                return null;
            if (index >= placed.Count || placed[index] is not { } pose || ScreenPoint.Of(camera, pose.Position.ToUnity()) is not { OnScreen: true } point)
                return null;
            if (All[index].Kind != LightKind.Spot)
                return (point, null);
            var aim = pose.Position + NumericsVector3.Transform(NumericsVector3.UnitZ, pose.Rotation);
            return (point, ScreenPoint.Of(camera, aim.ToUnity()));
        }

        public static Camera Camera() => CameraTools.maincam ? CameraTools.maincam : UnityEngine.Camera.main;

        // After the free camera has moved, so lights that follow the camera are where it is this frame.
        public static void Update()
        {
            try
            {
                var camera = Camera();
                if (Moving)
                    Move();
                placed.Clear();
                foreach (var light in All)
                    placed.Add(Shown ? Place(light) : null);
                ApplyGameLights();
                ApplySpheres(camera);
                int sun = All.FindIndex(light => light.SunShading);
                var color = sun >= 0 ? All[sun].Color : default;
                CharacterSun.Update(sun >= 0 ? placed[sun]?.Position.ToUnity() : null, new Color(color.X, color.Y, color.Z, 1f));
                if (changedAt is float at && Time.unscaledTime - at >= SaveDelay)
                {
                    changedAt = null;
                    Save();
                }
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Lights: updating the lights failed: {e}");
            }
        }

        private static void Changed() => changedAt = Time.unscaledTime;

        private static Pose? Place(LightSetup light) => FrameOf(light.Follow)?.ToWorld(light.Local);

        // Nothing's frame turns an absolute position into the scene's, which Genshin shifts as the player travels.
        private static Frame WorldFrame() => new(WorldShift.Relative(default).ToNumerics(), NumericsQuaternion.Identity);

        private static Frame? FrameOf(LightFollow follow)
        {
            switch (follow)
            {
                case LightFollow.Camera:
                    var camera = Camera();
                    return camera ? new Frame(camera.transform.position.ToNumerics(), camera.transform.rotation.ToNumerics()) : null;
                case LightFollow.Character:
                    var avatar = Character.Active();
                    return avatar ? new Frame(avatar.position.ToNumerics(), avatar.rotation.ToNumerics()) : null;
                default:
                    return WorldFrame();
            }
        }

        // In the scene, PlaceAhead in front of the camera and facing where it looks, so a spot aims at what is on screen.
        private static Pose Ahead(Camera camera)
        {
            var transform = camera.transform;
            var position = transform.position + transform.forward * PlaceAhead;
            return new Pose(position.ToNumerics(), transform.rotation.ToNumerics());
        }

        // After the free camera has moved: the light takes its pose. X and Y, or 4 and 5, narrow and widen a spot, and the roll
        // buttons dim and brighten the light.
        private static void Move()
        {
            var light = Current;
            var freecam = CameraTools.freecam;
            if (light == null || freecam == null || !CameraTools.freecamActive || move is not { } start)
            {
                EndMove(false);
                return;
            }
            var now = Gamepad.Current;
            var before = Gamepad.Previous;
            bool pad = Controls.Owner == InputOwner.CameraTools;
            if (Time.frameCount != start.Frame)
            {
                if (pad && PadA.Pressed(now, before) || Input.GetKeyDown(KeyCode.Return) || Input.GetKeyDown(KeyCode.KeypadEnter))
                {
                    EndMove(true);
                    return;
                }
                if (pad && PadB.Pressed(now, before) || Input.GetKeyDown(KeyCode.Escape))
                {
                    EndMove(false);
                    return;
                }
            }
            float step = Time.unscaledDeltaTime;
            if (light.Kind == LightKind.Spot)
            {
                float widen = (pad && PadWider.Held(now) || Input.GetKey(KeyWider) ? 1f : 0f)
                    - (pad && PadNarrower.Held(now) || Input.GetKey(KeyNarrower) ? 1f : 0f);
                light.SpotAngle = Math.Clamp(light.SpotAngle + widen * SpotAngleRate * step, LightSetup.MinSpotAngle, LightSetup.MaxSpotAngle);
                // A wider cone widens the view until it fits; the FOV buttons change the view as they do in the free camera.
                if (widen > 0f)
                    CameraTools.settings.Fov.Value = MoveFov(light, CameraTools.settings.Fov.Value);
            }
            float brighten = Held(CamAction.RollRight) - Held(CamAction.RollLeft);
            light.Intensity = Math.Clamp(light.Intensity + brighten * IntensityRate * step, 0f, light.IntensityLimit);
            var (position, rotation, _) = freecam.Pose;
            var at = position.ToNumerics();
            var turn = rotation.ToNumerics();
            if (light.Kind == LightKind.Sphere)
                at += NumericsVector3.Transform(NumericsVector3.UnitZ, turn) * PlaceAhead;
            if (FrameOf(light.Follow) is { } frame)
                light.Local = frame.ToLocal(new Pose(at, turn));
        }

        private static float Held(CamAction action) => Controls.Held(action) ? 1f : 0f;

        private static void ApplyGameLights()
        {
            while (gameLights.Count < All.Count)
                gameLights.Add(new GameLight());
            for (int i = 0; i < gameLights.Count; i++)
            {
                var game = gameLights[i];
                var light = i < All.Count ? All[i] : null;
                if (light == null || light.Kind == LightKind.Sphere || placed[i] is not { } pose)
                {
                    Destroy(game);
                    continue;
                }
                var color = light.Color;
                var wanted = new Applied(light.Kind, light.Reach, light.Intensity, light.Range, light.SpotAngle, light.InnerAngle, color,
                    light.CastsShadows);
                // A fresh component when the type or reach changes, so no character switch is left over from before.
                if (!game.Object || !game.Light || game.Shown.Kind != wanted.Kind || game.Shown.Reach != wanted.Reach)
                    Create(game, !light.CharactersOnly);
                game.Object.transform.position = pose.Position.ToUnity();
                game.Object.transform.rotation = pose.Rotation.ToUnity();
                if (game.Shown == wanted)
                    continue;
                var component = game.Light;
                component.type = light.Kind == LightKind.Spot ? LightType.Spot : LightType.Point;
                component.color = new Color(color.X, color.Y, color.Z, 1f);
                component.intensity = light.Intensity;
                component.range = light.Range;
                component.spotAngle = light.SpotAngle;
                component.spotInnerAngle = Math.Min(light.InnerAngle, light.SpotAngle);
                if (light.CharactersOnly)
                {
                    component.set_isCharacterLight(true);
                    component.characterLightCullingMask = uint.MaxValue;
                    component.characterIntensityMultiplier = 1f;
                    component.halfLambertThreshold = CharacterThreshold;
                    component.halfLambertSoftness = CharacterSoftness;
                }
                // On the Deck, soft shadows with nothing more showed once Unity's shadows were on.
                component.shadows = light.CastsShadows ? LightShadows.Soft : LightShadows.None;
                component.shadowStrength = 1f;
                if (game.Twin)
                {
                    var twin = game.Twin;
                    twin.type = component.type;
                    twin.color = component.color;
                    twin.intensity = Math.Min(light.Intensity * TwinShare, LightSetup.MaxCharacterIntensity);
                    twin.range = light.Range;
                    twin.spotAngle = component.spotAngle;
                    twin.spotInnerAngle = component.spotInnerAngle;
                }
                game.Shown = wanted;
            }
            gameLights.RemoveRange(All.Count, gameLights.Count - All.Count);
        }

        private static void Create(GameLight game, bool twin)
        {
            Destroy(game);
            game.Object = new GameObject("CameraTools Light");
            UnityEngine.Object.DontDestroyOnLoad(game.Object);
            game.Light = game.Object.AddComponent<Light>();
            if (twin)
            {
                var child = new GameObject("CameraTools Light Twin");
                child.transform.SetParent(game.Object.transform, false);
                game.Twin = child.AddComponent<Light>();
                game.Twin.set_isCharacterLight(true);
                game.Twin.characterLightCullingMask = uint.MaxValue;
                game.Twin.characterIntensityMultiplier = 1f;
                game.Twin.halfLambertThreshold = CharacterThreshold;
                game.Twin.halfLambertSoftness = CharacterSoftness;
                game.Twin.shadows = LightShadows.None;
            }
            game.Shown = default;
        }

        private static void Destroy(GameLight game)
        {
            if (game.Object)
                UnityEngine.Object.Destroy(game.Object);
            game.Object = null;
            game.Light = null;
            game.Twin = null;
            game.Shown = default;
        }

        private static void ApplySpheres(Camera camera)
        {
            bool any = Shown && HasSpheres;
            if (!ReShade.Connected)
            {
                overrideSent = null;
                return;
            }
            bool shooting = Screenshot.State is ShotState.Busy;
            // While a screenshot runs, the user's own effects render beside the spheres.
            var wanted = (any, any && !shooting, ReShade.Connections);
            if (overrideSent != wanted)
            {
                ReShade.SetOverride(any ? ReLight.Effects : Array.Empty<string>(), wanted.Item2);
                overrideSent = wanted;
            }
            var status = ReShade.Status;
            if (!any)
            {
                if (effectsBefore is bool before)
                {
                    ReLight.Clear();
                    if (!shooting)
                        ReShade.SetEffects(before);
                    effectsBefore = null;
                }
                return;
            }
            effectsBefore ??= status.EffectsEnabled == 1;
            if (!shooting && status.Runtime == 1 && status.EffectsEnabled == 0)
                ReShade.SetEffects(true);
            if (!camera)
                return;
            spheres.Clear();
            int selected = -1;
            for (int i = 0; i < All.Count; i++)
            {
                var light = All[i];
                if (light.Kind != LightKind.Sphere || placed[i] is not { } pose)
                    continue;
                if (i == Active)
                    selected = spheres.Count;
                var color = light.Color;
                spheres.Add(new ReLight.Sphere(pose.Position.ToUnity(), light.Radius, new Color(color.X, color.Y, color.Z, 1f), light.Intensity));
            }
            bool outlines = Markers && CameraUi.View is View.Hud or View.Panel or View.Moving;
            ReLight.Write(camera, spheres, selected, outlines, Ambient);
        }

        // Written beside the file and moved over it, so a crash mid-write leaves the last good save.
        private static void Save()
        {
            try
            {
                Directory.CreateDirectory(Folder);
                var file = new LightsFile { Lights = All.Select(ToFile).ToList(), Active = Active, Markers = Markers, Ambient = Ambient };
                string temporary = FilePath + ".tmp";
                File.WriteAllText(temporary, JsonSerializer.Serialize(file, Json));
                File.Move(temporary, FilePath, true);
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Lights: saving {FilePath} failed: {e.Message}");
            }
        }

        private static LightFile ToFile(LightSetup light) => new()
        {
            Kind = light.Kind,
            Reach = light.Reach,
            Follow = light.Follow,
            Position = new[] { light.Local.Position.X, light.Local.Position.Y, light.Local.Position.Z },
            Rotation = new[] { light.Local.Rotation.X, light.Local.Rotation.Y, light.Local.Rotation.Z, light.Local.Rotation.W },
            Intensity = light.Intensity,
            Range = light.Range,
            SpotAngle = light.SpotAngle,
            InnerAngle = light.InnerAngle,
            Radius = light.Radius,
            Shadows = light.Shadows,
            SunShading = light.SunShading,
            Source = light.Source,
            Kelvin = light.Kelvin,
            Hue = light.Hue,
            Saturation = light.Saturation,
        };

        private static LightSetup FromFile(LightFile file) => new()
        {
            Kind = file.Kind,
            Reach = file.Reach,
            Follow = file.Follow,
            Local = new Pose(new NumericsVector3(file.Position[0], file.Position[1], file.Position[2]),
                NumericsQuaternion.Normalize(new NumericsQuaternion(file.Rotation[0], file.Rotation[1], file.Rotation[2], file.Rotation[3]))),
            Intensity = file.Intensity,
            Range = file.Range,
            SpotAngle = file.SpotAngle,
            InnerAngle = file.InnerAngle,
            Radius = file.Radius,
            Shadows = file.Shadows,
            SunShading = file.SunShading,
            Source = file.Source,
            Kelvin = file.Kelvin,
            Hue = file.Hue,
            Saturation = file.Saturation,
        };

        private sealed class LightsFile
        {
            public List<LightFile> Lights { get; set; } = new();
            public int Active { get; set; }
            public bool Markers { get; set; } = true;
            public float Ambient { get; set; } = 1f;
        }

        // Position is absolute for a light that follows nothing, and relative to the camera or the character otherwise.
        private sealed class LightFile
        {
            public LightKind Kind { get; set; }
            public LightReach Reach { get; set; }
            public LightFollow Follow { get; set; }
            public float[] Position { get; set; }
            public float[] Rotation { get; set; }
            public float Intensity { get; set; }
            public float Range { get; set; }
            public float SpotAngle { get; set; }
            public float InnerAngle { get; set; }
            public float Radius { get; set; }
            public bool Shadows { get; set; }
            public bool SunShading { get; set; }
            public ColorSource Source { get; set; }
            public float Kelvin { get; set; }
            public float Hue { get; set; }
            public float Saturation { get; set; }
        }
    }
}
