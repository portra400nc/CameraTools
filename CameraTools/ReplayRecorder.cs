using System.Runtime.InteropServices;
using Il2CppInterop.Runtime;
using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    internal abstract record RecordState
    {
        public sealed record Idle : RecordState;

        // Shown is the number last shown in the countdown, 0 before the first.
        public sealed record Countdown(float Until, int Shown, Handoff Before) : RecordState;

        public sealed record Recording(RecordSession Session) : RecordState;
    }

    // The free camera and pause from before a recording took them, given back when it stops.
    internal readonly record struct Handoff(bool Freecam, bool Paused);

    // What one recording builds and what it keeps between frames. Path is the number of the path it plays, or null.
    internal sealed class RecordSession
    {
        public readonly Replay Replay = new();
        public float Start;
        public long StartStamp;
        public Handoff Before;
        public int? Path;
        public string PathId;
        public bool Enemies, Effects;
        public int LastFrame = -1;
        public Track Current;
        public readonly Dictionary<IntPtr, Track> Held = new(), Live = new();
        public readonly HashSet<IntPtr> Ignored = new();
        public readonly List<Track> Posed = new();
        public readonly List<string> Party = new();
        public readonly Dictionary<(Track, int, int), IntPtr> LastTextures = new();
        // Diagnostic lines left for this recording, so a character that changes every frame cannot flood the log.
        public int Notes = 60;
        // Diagnostic: effect and gadget names already noted as started or as too far.
        public readonly HashSet<string> Named = new();

        // Real time since the recording started, which moves on inside a frame that copies: a clock of frame start times
        // lost the first seconds of a recording to the copying done in its first frame.
        public float Elapsed => Time.realtimeSinceStartup - Start;
    }

    // Records the party, their weapons, effects and enemies once a frame, at Camera.onPreCull of the game camera:
    // weapons and effects are placed after the late update, and read there they trailed the character by a frame.
    internal static class ReplayRecorder
    {
        private const float CountdownSeconds = 3f;
        private const float EffectRange = 30f, MonsterRange = 60f, SettleTime = 1f, Grace = 0.3f, MinDuration = 0.5f;
        private const int MaxMeshSwaps = 40;

        private static readonly Dictionary<IntPtr, ParticleSystem[]> systemsOf = new();
        private static readonly HashSet<IntPtr> seen = new();

        public static RecordState State { get; private set; } = new RecordState.Idle();

        public static bool Idle => State is RecordState.Idle;

        public static bool Active => State is RecordState.Recording;

        public static RecordSession Session => (State as RecordState.Recording)?.Session;

        // Closes the panel and records, after the countdown when it is on. You act while recording, so input goes to the
        // game and the game's camera follows you, unless a path to play flies the free camera.
        public static void Arm()
        {
            if (!Idle)
                return;
            // Recording runs in the game camera's pre-cull callback, which injecting the free camera registers.
            if (!CameraTools.camera)
            {
                CameraUi.Toast("Turn on the free camera once first");
                return;
            }
            // Playback first, so a frozen world's speed is back before the pause is read.
            ReplayPlayer.Stop(null);
            PathPlayback.Stop(null);
            var before = new Handoff(CameraTools.freecamActive, Time.timeScale == 0f);
            CameraUi.ClosePanel();
            Melon<CameraTools>.Instance.SetFreecam(Replays.PlayPath && CameraPaths.CanPlay);
            // The free camera may have paused the game, and a path keeps it on.
            if (Time.timeScale == 0f)
                CameraTools.SetPaused(false);
            if (!Replays.Countdown)
            {
                Begin(before);
                return;
            }
            State = new RecordState.Countdown(Time.unscaledTime + CountdownSeconds, 0, before);
            Count((RecordState.Countdown)State);
        }

        public static void Update()
        {
            // Here rather than in the pre-cull callback, so the free camera does not come back in the middle of drawing.
            if (Session is { } session && session.Elapsed > Replays.Limit)
                Stop();
            if (State is RecordState.Countdown countdown)
                Count(countdown);
        }

        private static void Count(RecordState.Countdown countdown)
        {
            float remaining = countdown.Until - Time.unscaledTime;
            if (remaining <= 0f)
            {
                Begin(countdown.Before);
                return;
            }
            int shown = (int)MathF.Ceiling(remaining);
            if (shown == countdown.Shown)
                return;
            CameraUi.Toast(countdown.Shown == 0 ? $"Recording in {shown}" : $"{shown}", whileHidden: true);
            State = countdown with { Shown = shown };
        }

        // Every weapon and pooled effect is copied before the clock starts, so a pooled effect costs nothing while
        // recording. Characters are copied once they are on the field. A path to play starts with the clock.
        private static void Begin(Handoff before)
        {
            var prepare = System.Diagnostics.Stopwatch.StartNew();
            var session = new RecordSession { Before = before, Enemies = Replays.RecordEnemies, Effects = Replays.RecordEffects };
            ReplayHooks.Drain();
            foreach (var weapon in ReplayScene.Children(ReplayScene.EquipRoot))
                StandIns.Shared(weapon, TrackKind.Weapon);
            if (session.Effects)
            {
                foreach (var effect in ReplayScene.Children(ReplayScene.EffectPool))
                {
                    StandIns.Shared(effect, TrackKind.Effect);
                    // Effects already playing are left out until they stop.
                    if (Playing(effect))
                        session.Ignored.Add(effect.Pointer);
                }
                foreach (var gadget in ReplayScene.Gadgets())
                    session.Ignored.Add(gadget.Pointer);
            }
            session.Start = Time.realtimeSinceStartup;
            session.StartStamp = System.Diagnostics.Stopwatch.GetTimestamp();
            ReplayHooks.TakeGlobals();
            State = new RecordState.Recording(session);
            if (Replays.PlayPath && CameraPaths.CanPlay && PathPlayback.PlayNow((float)prepare.Elapsed.TotalSeconds))
            {
                session.Path = CameraPaths.Active + 1;
                session.PathId = CameraPaths.Current.Id;
            }
            ReplayHooks.Recording = true;
            Melon<CameraTools>.Logger.Msg($"Replays: recording started after {prepare.Elapsed.TotalMilliseconds:0} ms of copying"
                + $"{(session.Path is int path ? $", with path {path}" : "")}; enemies {(session.Enemies ? "on" : "off")}, effects {(session.Effects ? "on" : "off")}.");
        }

        // Keeps what was recorded as a new replay, unless it is too short to play. A path the recording started stops with it.
        // The free camera and pause come back as they were before the recording.
        public static void Stop()
        {
            var state = State;
            State = new RecordState.Idle();
            ReplayHooks.Recording = false;
            ReplayHooks.Watched = null;
            ReplayHooks.WatchedNatives = null;
            if (state is RecordState.Countdown countdown)
            {
                Restore(countdown.Before);
                CameraUi.Toast("Recording cancelled");
                return;
            }
            if (state is not RecordState.Recording { Session: var session })
                return;
            if (session.Path != null && PathPlayback.State is PlaybackState.Playing)
                PathPlayback.Stop(null);
            Restore(session.Before);
            var replay = session.Replay;
            replay.Duration = session.Elapsed;
            if (replay.Duration < MinDuration)
            {
                replay.Release();
                CameraUi.Toast("Recording cancelled");
                return;
            }
            foreach (var track in session.Live.Values)
                track.End = Math.Min(track.End, replay.Duration);
            foreach (var track in replay.Tracks)
                if (track.Materials == null && track.Kind is TrackKind.Character or TrackKind.Weapon && track.Source.Root)
                    Capture(track, false);
            foreach (var track in replay.Of(TrackKind.Character))
                if (track.Source.Root)
                    track.AtEnd = Enumerable.Range(0, track.Source.Renderers.Length).Select(track.Source.Describe).ToArray();
            replay.Name = ReplayText.NextName(Replays.All.Select(each => each.Name));
            replay.Party = session.Party.ToArray();
            replay.PathId = session.PathId;
            Replays.Add(replay);
            Melon<CameraTools>.Logger.Msg($"Replays: recorded {replay.Name}, {replay.Duration:0.00} s, {replay.Tracks.Count} tracks: "
                + $"{replay.Of(TrackKind.Character).Count()} characters, {replay.Enemies} enemies, {replay.Of(TrackKind.Effect).Count()} effect plays, "
                + $"{replay.Of(TrackKind.Gadget).Count()} gadgets, {replay.Tracks.Sum(track => track.Calls.Count)} look calls.");
            CameraUi.Toast(ReplayText.Recorded(replay.Name, replay.Duration, Replays.PathLabel(session.Path)));
            NoteGlobals(session, replay);
            NoteNativeVectors(replay);
            foreach (var beam in replay.Of(TrackKind.Effect).Where(track => track.Source.Name.Contains("WaterTornado")))
                for (int r = 0; r < beam.Source.Renderers.Length; r++)
                {
                    var meshes = beam.Calls.Where(call => call.Kind == CallKind.LineMesh && call.Slot == r && call.Line != null).Select(call => call.Line).ToList();
                    if (meshes.Count == 0)
                        continue;
                    int Distinct(Func<LineMesh, byte[]> part) => meshes.Select(part).Select(bytes => bytes == null ? "-" : Convert.ToBase64String(System.Security.Cryptography.SHA1.HashData(bytes))).Distinct().Count();
                    var first = meshes[0];
                    var settings = beam.Given?.ElementAtOrDefault(r)?.Values;
                    Melon<CameraTools>.Logger.Msg($"Replays: note: {(beam.Source.Renderers[r] ? beam.Source.Renderers[r].name : $"renderer {r}")} {meshes.Count} meshes: {Distinct(m => m.Positions)} position sets, {Distinct(m => m.Uv0)} uv0 sets, "
                        + $"{Distinct(m => m.Colors)} colour sets, {Distinct(m => m.Uv1)} uv1 sets, {Distinct(m => m.Normals)} normal sets; first has {first.PositionCount} positions, "
                        + $"{first.TriangleCount} indices, uv1 {first.Uv1Count}; settings at start [{string.Join(", ", settings?.Select(pair => $"{ReplayHooks.Setters[pair.Key].Name}={pair.Value.Int}/{pair.Value.Float:0.##}") ?? Array.Empty<string>())}]");
                }
            foreach (var kind in new[] { TrackKind.Weapon, TrackKind.Gadget })
                Melon<CameraTools>.Logger.Msg($"Replays: note: {kind} tracks: "
                    + string.Join(", ", replay.Of(kind).Select(track => $"{track.Source.Name} {track.Start:0.0}-{Math.Min(track.End, replay.Duration):0.0} s")));
        }

        private static void Restore(Handoff before)
        {
            Melon<CameraTools>.Instance.SetFreecam(before.Freecam);
            if (before.Paused && Time.timeScale != 0f)
                CameraTools.SetPaused(true);
        }

        public static void PreCull() => Replays.Guard("recording", Frame, Stop);

        private static void Frame()
        {
            if (Session is not { } session || Time.frameCount == session.LastFrame)
                return;
            session.LastFrame = Time.frameCount;
            float t = session.Elapsed;
            if (t > Replays.Limit)
                return;
            var avatar = Character.Active();
            if (avatar)
            {
                var track = Held(session, avatar, TrackKind.Character, t);
                if (track != session.Current)
                {
                    // A copy made as a character leaves the field does not draw, so it is made again once it settles.
                    if (session.Current is { Materials: null } leaving)
                        Capture(leaving, false);
                    session.Current = track;
                }
                AddPose(session, track, t);
                if (Time.frameCount % 10 == 0)
                    NoteParts(session, track, t);
            }
            foreach (var weapon in ReplayScene.Children(ReplayScene.EquipRoot))
                if (weapon.gameObject.activeSelf)
                    AddPose(session, Held(session, weapon, TrackKind.Weapon, t), t);
            if (session.Effects)
            {
                RecordEffects(session, t, avatar);
                RecordGadgets(session, t, avatar);
            }
            if (session.Enemies)
                RecordMonsters(session, t, avatar);
            Attribute(session, t);
        }

        private static Track Held(RecordSession session, Transform root, TrackKind kind, float t)
        {
            if (session.Held.TryGetValue(root.Pointer, out var known))
                return known;
            var track = new Track
            {
                Kind = kind, Source = new Rig(root) { Watched = true }, Start = t,
                Copy = kind == TrackKind.Character ? null : StandIns.Shared(root, kind),
                Structure = kind == TrackKind.Character ? Rig.Structure(root) : default,
            };
            session.Replay.Tracks.Add(track);
            session.Held[root.Pointer] = track;
            if (kind == TrackKind.Character)
                session.Party.Add(ReplayText.CharacterName(root.name));
            return track;
        }

        private static Track Begin(RecordSession session, Transform root, TrackKind kind, float t)
        {
            // Effects watched like characters: the game fades and stretches some by setting their materials from code each
            // frame, as Neuvillette's charged attack beam, and a copy on its own materials kept them hidden.
            var source = new Rig(root) { Watched = true };
            var track = new Track
            {
                Kind = kind, Source = source, Copy = StandIns.Shared(root, kind), Start = t, LastAlive = t, Materials = source.TakeMaterials(),
                Given = source.Renderers.Select(renderer => renderer ? ReplayHooks.Remembered(renderer) : null).ToArray(),
            };
            if (root.name.Contains("WaterTornado"))
                NoteName(session, $"effect {root.name} records the particles of [{source.TracedNames}]");
            if (root.name.Contains("WaterTornado"))
                for (int r = 0; r < source.Renderers.Length; r++)
                    if (source.Renderers[r] is { } line && line && line.TryCast<LineRenderer>() != null && track.Given[r] is { } given)
                        NoteName(session, $"effect {root.name} line {line.name} starts with {given.Values?.Count ?? 0} settings, custom floats [{string.Join(", ", given.Floats?.Select(pair => $"{pair.Key}={pair.Value:0.###}") ?? Array.Empty<string>())}], "
                            + $"others [{string.Join(", ", given.Props?.Select(pair => $"{pair.Key.Kind}/{pair.Key.Material}/{pair.Key.Id}=({pair.Value.Value.x:0.##},{pair.Value.Value.y:0.##},{pair.Value.Value.z:0.##},{pair.Value.Value.w:0.##})") ?? Array.Empty<string>())}]");
            session.Replay.Tracks.Add(track);
            session.Live[root.Pointer] = track;
            return track;
        }

        private static void Finish(RecordSession session, IntPtr root, Track track, float at)
        {
            track.End = at;
            session.Live.Remove(root);
        }

        private static void AddPose(RecordSession session, Track track, float t)
        {
            if (!track.Source.Root)
                return;
            if (track.Poses.Count == 0 || t - track.Poses.LastTime > Timeline<RigPose>.Gap)
                track.RunStart = t;
            track.Poses.Add(t, track.Source.Read());
            if (track.Source.Watched)
                session.Posed.Add(track);
            if (!track.Settled && track.Kind is TrackKind.Character or TrackKind.Weapon && t - track.RunStart >= SettleTime)
                Capture(track, true);
        }

        // Characters and weapons fade in when switched in and out when switched out, so their materials are captured once
        // they have been on the field a while. A character is copied then too: copies of off-field characters, or of one
        // leaving the field, mostly do not draw, while copies of on-field characters do. A copy made at a switch-out or at
        // the end is unsettled and is made again once the character has been on the field long enough.
        private static void Capture(Track track, bool settled)
        {
            track.Settled = settled;
            if (track.Kind == TrackKind.Character)
            {
                track.Copy?.Destroy();
                track.Copy = StandIns.Own(track.Source.Root, TrackKind.Character, track.Source);
                track.AtCopy = Enumerable.Range(0, track.Source.Renderers.Length).Select(track.Source.Describe).ToArray();
                if (!track.GivenNoted)
                {
                    track.GivenNoted = true;
                    foreach (var renderer in track.Source.Renderers)
                        if (renderer && renderer.enabled && renderer.TryCast<SkinnedMeshRenderer>() != null && ReplayHooks.Remembered(renderer) is var given)
                            Melon<CameraTools>.Logger.Msg($"Replays: note: {track.Source.Name} {renderer.name} remembered {given.Values?.Count ?? 0} settings, {given.Props?.Count ?? 0} other custom properties, custom floats "
                                + $"[{string.Join(", ", given.Floats?.Select(pair => $"{pair.Key}={pair.Value:0.###}") ?? Array.Empty<string>())}]");
                }
                if (track.Copy?.Rig is { } made && (made.Bones.Length != track.Source.Bones.Length || made.Renderers.Length != track.Source.Renderers.Length))
                    Melon<CameraTools>.Logger.Msg($"Replays: note: {track.Source.Name} was recorded with {track.Source.Bones.Length} parts and "
                        + $"{track.Source.Renderers.Length} renderers, its copy has {made.Bones.Length} and {made.Renderers.Length}.");
            }
            track.Materials = track.Source.TakeMaterials();
            if (track.Copy is { Usable: true } copy)
                copy.Rig.UseMaterials(track.Materials);
        }

        // Diagnostic: the first 0x800 bytes of a renderer's engine object, which playback compares with its copy's.
        // Diagnostic notes that take long enough to stall the game for a second or more.
        public const bool HeavyNotes = false;
        public static readonly Dictionary<string, byte[]> RealNatives = new();

        public static byte[] NativeBytes(Renderer renderer)
        {
            var bytes = new byte[0x800];
            var native = Marshal.ReadIntPtr(renderer.Pointer, 0x10);
            if (native != IntPtr.Zero)
                Marshal.Copy(native, bytes, 0, bytes.Length);
            return bytes;
        }

        // Diagnostic: the engine's custom vector calls on the beam's renderers, from any caller.
        private static void NoteNativeVectors(Replay replay)
        {
            List<(IntPtr Native, int Id, Vector4 Value, bool Color, bool Binding)> calls;
            lock (ReplayHooks.NativeVectorLog)
            {
                calls = new(ReplayHooks.NativeVectorLog);
                ReplayHooks.NativeVectorLog.Clear();
            }
            var parts = new Dictionary<IntPtr, string>();
            foreach (var track in replay.Of(TrackKind.Effect).Where(track => track.Source.Name.Contains("WaterTornado")))
                foreach (var renderer in track.Source.Renderers)
                    if (renderer && Marshal.ReadIntPtr(renderer.Pointer, 0x10) is var native && native != IntPtr.Zero)
                        parts[native] = $"{track.Source.Name}/{renderer.name}";
            var hits = calls.Where(call => parts.ContainsKey(call.Native)).ToList();
            Melon<CameraTools>.Logger.Msg($"Replays: note: {calls.Count} engine custom vector calls while recording, {calls.Select(call => call.Native).Distinct().Count()} renderers, {hits.Count} on the beam's {parts.Count} renderers");
            foreach (var group in hits.GroupBy(call => (call.Native, call.Id, call.Color)).Take(60))
                Melon<CameraTools>.Logger.Msg($"Replays: note: beam {parts[group.Key.Native]} {(group.Key.Color ? "colour" : "vector")} {group.Key.Id}: {group.Count()} calls, {group.Count(call => call.Binding)} through a binding, "
                    + $"first {group.First().Value}, last {group.Last().Value}");
        }

        // Diagnostic: the global shader values that are set only while Neuvillette's beam plays, or that change only then.
        private static void NoteGlobals(RecordSession session, Replay replay)
        {
            var beams = replay.Of(TrackKind.Effect).Where(track => track.Source.Name.Contains("WaterTornado")).ToList();
            var sets = ReplayHooks.TakeGlobals();
            Melon<CameraTools>.Logger.Msg($"Replays: note: {sets.Count} global shader sets recorded, {sets.Select(set => (set.Kind, set.Id)).Distinct().Count()} globals, {beams.Count} beam plays");
            if (beams.Count == 0 || sets.Count == 0)
                return;
            double frequency = System.Diagnostics.Stopwatch.Frequency;
            float At(long stamp) => (float)((stamp - session.StartStamp) / frequency);
            bool During(float t) => beams.Any(beam => t >= beam.Start - 0.05f && t <= Math.Min(beam.End, replay.Duration) + 0.05f);
            foreach (var group in sets.GroupBy(set => (set.Kind, set.Id)))
            {
                var inside = group.Where(set => During(At(set.Stamp))).ToList();
                if (inside.Count == 0)
                    continue;
                var outside = group.Where(set => !During(At(set.Stamp))).ToList();
                var outsideValues = new HashSet<(Vector4, IntPtr)>(outside.Select(set => (set.Value, set.Texture)));
                int fresh = inside.Count(set => !outsideValues.Contains((set.Value, set.Texture)));
                // Globals that animate all the time, such as the time of day, have new values in any window.
                if (outside.Count > 0 && (fresh == 0 || outsideValues.Count > 2))
                    continue;
                var first = inside[0];
                var last = inside[^1];
                string Show(ReplayHooks.GlobalSet set) => set.Kind == 't' ? $"tex 0x{set.Texture.ToInt64():x}" : $"({set.Value.x:0.###},{set.Value.y:0.###},{set.Value.z:0.###},{set.Value.w:0.###})";
                NoteName(session, $"global {group.Key.Kind} {group.Key.Id}: {inside.Count} sets in the beam ({fresh} new values), {outside.Count} outside; first {Show(first)} at {At(first.Stamp):0.00} s, last {Show(last)} at {At(last.Stamp):0.00} s");
            }
        }

        // Diagnostic: every value that could carry the beam's animation time, read twice to find the ones that change: the
        // lines' custom floats and textures, their property blocks, their materials' floats and vectors, and the globals.
        private static (Rig Rig, Dictionary<string, string> Values, float At) customScan;

        // The beam's values one second in, which playback compares its copy with.
        public static Dictionary<string, string> BeamAtOneSecond;

        public static Dictionary<string, string> ScanCustom(Rig rig)
        {
            var found = new Dictionary<string, string>();
            var block = new MaterialPropertyBlock();
            foreach (var renderer in rig.Renderers)
                if (renderer && renderer.TryCast<LineRenderer>() != null)
                {
                    string name = renderer.name;
                    for (int id = 0; id < 10000; id++)
                    {
                        found[$"{name} custom float {id}"] = renderer.GetCustomPropertyFloat(id).ToString("R");
                        found[$"{name} custom texture {id}"] = renderer.GetCustomPropertyTexture(id)?.name;
                    }
                    var materials = renderer.sharedMaterials;
                    for (int m = -1; m < materials.Length; m++)
                    {
                        if (m < 0)
                            renderer.GetPropertyBlock(block);
                        else
                            renderer.GetPerMaterialPropertyBlock(block, m);
                        if (block.isEmpty)
                            continue;
                        for (int id = 0; id < 10000; id++)
                            found[$"{name} block {m} {id}"] = $"{block.GetFloat(id):R} {block.GetVector(id)}";
                    }
                    var line = renderer.TryCast<LineRenderer>();
                    found[$"{name} line"] = $"enabled {renderer.enabled}, world {line.useWorldSpace}, layer {renderer.gameObject.layer}, sorting {renderer.sortingOrder}";
                    foreach (var material in materials)
                        if (material)
                        {
                            found[$"{name} material {material.name}"] = $"shader {material.shader?.name}, queue {material.renderQueue}, keywords [{string.Join(" ", material.shaderKeywords.OrderBy(word => word))}]";
                            for (int id = 0; id < 10000; id++)
                                if (material.HasProperty(id))
                                {
                                    found[$"{name} material {material.name} {id}"] = $"{material.GetFloat(id):R} {material.GetVector(id)}";
                                    if (material.GetTexture(id) is { } texture && texture)
                                        found[$"{name} material {material.name} texture {id}"] = $"{texture.name} {material.GetTextureOffset(id)} {material.GetTextureScale(id)}";
                                }
                        }
                }
            for (int id = 0; id < 10000; id++)
                found[$"global {id}"] = $"{Shader.GetGlobalFloat(id):R} {Shader.GetGlobalVector(id)}";
            return found;
        }

        // Diagnostic: the beam's lines read again later in the frame they were noted in, before and after the camera draws.
        private static (Rig Rig, int Left) pendingLines;

        public static void LaterInFrame(string when)
        {
            if (pendingLines.Rig is not { } rig || Session is not { } session)
                return;
            for (int r = 0; r < rig.Renderers.Length; r++)
                if (rig.Renderers[r] is { } line && line && line.TryCast<LineRenderer>() != null)
                    NoteName(session, $"effect {rig.Name} line {line.name} {when}: {rig.DescribeLine(r)}, bounds {line.bounds.center} size {line.bounds.size}");
            pendingLines = --pendingLines.Left > 0 ? pendingLines : default;
        }

        // Diagnostic: every enabled renderer within 30 m that no track records, to find what draws Neuvillette's beam.
        private static void NoteUnrecorded(RecordSession session, Transform near)
        {
            var recorded = new HashSet<IntPtr>(session.Replay.Tracks.Where(track => track.Source.Root)
                .SelectMany(track => track.Source.Renderers.Where(renderer => renderer).Select(renderer => renderer.Pointer)));
            var center = near.position;
            var found = new List<(float Distance, Renderer Renderer, Transform Root)>();
            var all = UnityEngine.Object.FindObjectsOfType(Il2CppType.Of<Renderer>());
            for (int i = 0; i < all.Length; i++)
            {
                var renderer = all[i]?.TryCast<Renderer>();
                if (!renderer || !renderer.enabled || !renderer.gameObject.activeInHierarchy || recorded.Contains(renderer.Pointer))
                    continue;
                var bounds = renderer.bounds;
                float distance = Math.Max(0f, (bounds.center - center).magnitude - bounds.extents.magnitude);
                if (distance <= 30f)
                    found.Add((distance, renderer, renderer.transform.root));
            }
            foreach (var root in found.GroupBy(each => each.Root.name).OrderByDescending(group => group.Count()))
                NoteName(session, $"unrecorded renderers under /{root.Key}: {root.Count()}");
            var counts = found.GroupBy(each => each.Root.Pointer).ToDictionary(group => group.Key, group => group.Count());
            foreach (var (distance, renderer, root) in found.Where(each => each.Root.name == "EntityRoot").OrderBy(each => each.Distance).Take(120))
            {
                var path = new List<string>();
                for (var at = renderer.transform; at; at = at.parent)
                    path.Add(at.name);
                path.Reverse();
                var mesh = renderer.TryCast<SkinnedMeshRenderer>() is { } skinned ? skinned.sharedMesh
                    : renderer.GetComponent(Il2CppType.Of<MeshFilter>())?.TryCast<MeshFilter>()?.sharedMesh;
                var shaders = renderer.sharedMaterials.Select(material => material && material.shader ? material.shader.name.Split('/').Last() : "null");
                NoteName(session, $"unrecorded {ReplayScene.TypeName(renderer)} {distance:0.0} m /{string.Join("/", path)} mesh {(mesh ? $"{mesh.name}:{mesh.vertexCount}" : "none")} "
                    + $"size {renderer.bounds.size} [{string.Join(", ", shaders)}]");
            }
        }

        // Diagnostic: a line for each effect or gadget name, once a recording.
        private static void NoteName(RecordSession session, string note)
        {
            if (session.Named.Count < 600 && session.Named.Add(note))
                Melon<CameraTools>.Logger.Msg($"Replays: note: {note} at {session.Elapsed:0.00} s");
        }

        // Diagnostic: a character whose parts change while recording, with what came and went.
        private static void NoteParts(RecordSession session, Track track, float t)
        {
            if (session.Notes <= 0 || !track.Source.Root)
                return;
            var now = Rig.Structure(track.Source.Root);
            if (now.Hash == track.Structure.Hash)
                return;
            var added = now.Parts.Except(track.Structure.Parts).Take(8);
            var removed = track.Structure.Parts.Except(now.Parts).Take(8);
            session.Notes--;
            Melon<CameraTools>.Logger.Msg($"Replays: note: {track.Source.Name} parts changed at {t:0.00} s, {track.Structure.Parts.Length} -> {now.Parts.Length}"
                + $" (recorded rig {track.Source.Bones.Length}); added [{string.Join(", ", added)}] removed [{string.Join(", ", removed)}]");
            track.Structure = now;
        }

        // A pooled effect plays while one of its top particle systems is alive, or while it is active if it has none.
        private static bool Playing(Transform effect)
        {
            if (!effect.gameObject.activeSelf)
                return false;
            if (!systemsOf.TryGetValue(effect.Pointer, out var systems))
                systemsOf[effect.Pointer] = systems = ReplayScene.TopSystems(effect);
            if (systems.Length == 0)
                return true;
            foreach (var system in systems)
                if (system && system.IsAlive(true))
                    return true;
            return false;
        }

        private static bool Near(Transform root, Transform avatar, float range)
            => !avatar || Vector3.Distance(root.position, avatar.position) <= range;

        // The pool keeps used effects active and plays them again. A play ends once the effect has not been playing for
        // Grace seconds, so a brief gap between bursts of particles does not split it.
        private static void RecordEffects(RecordSession session, float t, Transform avatar)
        {
            seen.Clear();
            foreach (var effect in ReplayScene.Children(ReplayScene.EffectPool))
            {
                seen.Add(effect.Pointer);
                session.Live.TryGetValue(effect.Pointer, out var track);
                if (!Playing(effect))
                {
                    session.Ignored.Remove(effect.Pointer);
                    if (track != null && t - track.LastAlive > Grace)
                        Finish(session, effect.Pointer, track, track.LastAlive);
                    continue;
                }
                if (track == null)
                {
                    if (session.Ignored.Contains(effect.Pointer))
                        continue;
                    if (!Near(effect, avatar, EffectRange))
                    {
                        session.Ignored.Add(effect.Pointer);
                        NoteName(session, $"effect {effect.name} too far at {Vector3.Distance(effect.position, avatar.position):0} m");
                        continue;
                    }
                    track = Begin(session, effect, TrackKind.Effect, t);
                    NoteName(session, $"effect {effect.name} started");
                }
                track.LastAlive = t;
                AddPose(session, track, t);
                // Diagnostic: the real beam's tubes as the engine holds them, one second in.
                if (HeavyNotes && effect.name.EndsWith("WaterTornado") && t - track.Start >= 1f && RealNatives.Count == 0)
                    foreach (var tube in track.Source.Renderers)
                        if (tube && tube.TryCast<LineRenderer>() != null)
                            RealNatives[tube.name] = NativeBytes(tube);
                if (HeavyNotes && effect.name.EndsWith("WaterTornado") && t - track.Start >= 1f && !track.LinesNoted)
                {
                    track.LinesNoted = true;
                    foreach (var (label, root) in new[] { ("real", track.Source.Root), ("copy", track.Copy?.Rig?.Root) })
                        if (root)
                            Melon<CameraTools>.Logger.Msg($"Replays: note: {label} beam components: {string.Join(", ", root.GetComponentsInChildren<Component>(true).GroupBy(component => ReplayScene.TypeName(component)).Select(group => $"{group.Key} x{group.Count()}"))}");
                    foreach (var tube in track.Source.Renderers)
                        if (tube && tube.TryCast<LineRenderer>() != null)
                            foreach (var material in tube.sharedMaterials)
                                if (material)
                                    for (int id = 0; id < 10000; id++)
                                        if (material.HasProperty(id) && material.GetTexture(id) is { } texture && texture)
                                            Melon<CameraTools>.Logger.Msg($"Replays: note: {tube.name} {material.name} texture {id}: {texture.name} {ReplayScene.TypeName(texture)} {texture.width}x{texture.height}, "
                                                + $"render texture {texture.TryCast<RenderTexture>() != null}, offset {material.GetTextureOffset(id)} scale {material.GetTextureScale(id)}");
                }
                if (HeavyNotes && !track.MaterialsNoted && t - track.Start >= 0.5f && effect.name.Contains("ExtraAttack"))
                {
                    track.MaterialsNoted = true;
                    var rig = track.Source;
                    for (int r = 0; r < rig.Renderers.Length; r++)
                        if (rig.Renderers[r] is { } renderer && renderer)
                            NoteName(session, $"effect {effect.name} part {renderer.name} {ReplayScene.TypeName(renderer)}"
                                + (renderer.TryCast<LineRenderer>() is { } line ? $" {line.positionCount} points" : "") + $": {rig.Describe(r)}");
                }
            }
            // The pool moves a finished effect out of /EffectPool; a play left open there kept its copy where the effect
            // was last seen, such as crystallize shields left behind at every switch.
            foreach (var (root, track) in session.Live.Where(pair => pair.Value.Kind == TrackKind.Effect && !seen.Contains(pair.Key)).ToList())
            {
                if (session.Notes > 0 && track.Source.Root is { } gone && gone)
                {
                    session.Notes--;
                    var path = new List<string>();
                    for (var at = gone.parent; at; at = at.parent)
                        path.Add(at.name);
                    path.Reverse();
                    Melon<CameraTools>.Logger.Msg($"Replays: note: effect {gone.name} left the pool at {t:0.00} s after {t - track.Start:0.00} s, "
                        + $"{(gone.gameObject.activeInHierarchy ? "active" : "inactive")}, playing {Playing(gone)}, now under /{string.Join("/", path)}");
                }
                Finish(session, root, track, track.LastAlive);
            }
        }

        // Gadgets and bullets that appear during the recording, such as Lisa's burst.
        private static void RecordGadgets(RecordSession session, float t, Transform avatar)
        {
            foreach (var gadget in ReplayScene.Gadgets())
            {
                if (session.Ignored.Contains(gadget.Pointer))
                    continue;
                session.Live.TryGetValue(gadget.Pointer, out var track);
                if (!gadget.gameObject.activeSelf)
                {
                    if (track != null)
                        Finish(session, gadget.Pointer, track, t);
                    continue;
                }
                if (track == null)
                {
                    if (!Near(gadget, avatar, EffectRange))
                    {
                        NoteName(session, $"gadget {gadget.name} too far at {Vector3.Distance(gadget.position, avatar.position):0} m");
                        session.Ignored.Add(gadget.Pointer);
                        continue;
                    }
                    track = Begin(session, gadget, TrackKind.Gadget, t);
                }
                AddPose(session, track, t);
            }
            foreach (var (root, track) in session.Live.Where(pair => pair.Value.Kind == TrackKind.Gadget && !pair.Value.Source.Root).ToList())
                Finish(session, root, track, t);
        }

        // Enemies and their weapons, from appearing to disappearing, so a spawn and a death play with their animations.
        // Domains reuse enemy objects for later waves, so an enemy that appears again starts a new track.
        private static void RecordMonsters(RecordSession session, float t, Transform avatar)
        {
            seen.Clear();
            foreach (var (root, kind) in ReplayScene.Children(ReplayScene.MonsterRoot).Select(each => (each, TrackKind.Monster))
                .Concat(ReplayScene.Children(ReplayScene.MonsterEquipRoot).Select(each => (each, TrackKind.MonsterWeapon))))
            {
                seen.Add(root.Pointer);
                session.Live.TryGetValue(root.Pointer, out var track);
                if (!root.gameObject.activeSelf)
                {
                    if (track != null)
                        Finish(session, root.Pointer, track, t);
                    continue;
                }
                if (track == null)
                {
                    if (!Near(root, avatar, MonsterRange))
                        continue;
                    track = Segment(session, root, kind, t, Rig.Structure(root));
                    if (kind == TrackKind.Monster)
                        session.Replay.Enemies++;
                }
                else if (Time.frameCount % 10 == 0 && Rig.Structure(root) is var structure && structure.Hash != track.Structure.Hash)
                {
                    track.End = t;
                    track = Segment(session, root, kind, t, structure);
                }
                AddPose(session, track, t);
            }
            // A dead enemy is moved to /CacheRoot rather than destroyed; a track left open there held its copy on its last
            // pose until the replay ended.
            foreach (var (root, track) in session.Live.Where(pair => pair.Value.Kind is TrackKind.Monster or TrackKind.MonsterWeapon && !seen.Contains(pair.Key)).ToList())
                Finish(session, root, track, track.Poses.Count > 0 ? track.Poses.LastTime : t);
        }

        // A track for one stretch of an enemy with an unchanged structure, with a fresh copy of it made in the same frame,
        // so the copy has the structure the track's poses are read from: a copy queued for a later frame broke playback
        // when the enemy changed again first.
        private static Track Segment(RecordSession session, Transform root, TrackKind kind, float t, (int, string[]) structure)
        {
            var track = new Track { Kind = kind, Source = new Rig(root) { Watched = true }, Start = t, Structure = structure };
            session.Replay.Tracks.Add(track);
            session.Live[root.Pointer] = track;
            track.Copy = StandIns.Own(root, kind);
            track.Materials = track.Source.TakeMaterials();
            if (track.Copy.Usable)
            {
                track.Copy.Rig.UseMaterials(track.Materials);
                track.Copy.Rig.StopLods();
            }
            return track;
        }

        // Files the game's setter calls of this frame under the recorded slots and renderers they target. A slot whose
        // material the game replaced this frame gets a snapshot of the new material, which already holds this frame's
        // calls on it. Then it watches the materials and renderers recorded this frame for the next.
        private static void Attribute(RecordSession session, float t)
        {
            var slots = new Dictionary<IntPtr, (Track Track, int Slot)>();
            var renderers = new Dictionary<IntPtr, (Track Track, int Renderer)>();
            var swapped = new HashSet<IntPtr>();
            foreach (var track in session.Posed)
            {
                var rig = track.Source;
                track.LastSlots ??= (IntPtr[])rig.SlotPointers.Clone();
                for (int slot = 0; slot < rig.Slots.Length; slot++)
                {
                    var pointer = rig.SlotPointers[slot];
                    if (pointer != IntPtr.Zero && pointer != track.LastSlots[slot] && rig.SlotMaterials[slot])
                    {
                        track.Calls.Add(new LookCall { Time = t, Slot = slot, Kind = CallKind.Swap, Snapshot = new Material(rig.SlotMaterials[slot]) });
                        swapped.Add(pointer);
                    }
                    track.LastSlots[slot] = pointer;
                    if (pointer != IntPtr.Zero)
                        slots[pointer] = (track, slot);
                }
                for (int r = 0; r < rig.Renderers.Length; r++)
                    if (rig.Renderers[r])
                        renderers[rig.Renderers[r].Pointer] = (track, r);
                if (track.Kind is TrackKind.Monster or TrackKind.MonsterWeapon)
                    RecordMeshes(track, t);
            }
            session.Posed.Clear();
            var watched = new HashSet<IntPtr>(slots.Keys);
            watched.UnionWith(renderers.Keys);
            ReplayHooks.Watched = watched;
            var natives = new Dictionary<IntPtr, IntPtr>(renderers.Count);
            foreach (var renderer in renderers.Keys)
                if (Marshal.ReadIntPtr(renderer, 0x10) is var native && native != IntPtr.Zero)
                    natives[native] = renderer;
            ReplayHooks.WatchedNatives = natives;
            foreach (var raw in ReplayHooks.Drain())
            {
                if (CallTarget.IsRenderer(raw.Kind))
                {
                    if (renderers.TryGetValue(raw.Self, out var owner))
                        owner.Track.Calls.Add(new LookCall
                        {
                            Time = t, Slot = owner.Renderer, Kind = raw.Kind, Id = raw.Id, Int = raw.Int, Value = raw.Value,
                            Texture = raw.Kind == CallKind.CustomTexture && raw.Ref != IntPtr.Zero ? new Texture(raw.Ref) : null,
                            Line = raw.Line,
                        });
                    continue;
                }
                if (swapped.Contains(raw.Self) || !slots.TryGetValue(raw.Self, out var at))
                    continue;
                // The game's every-frame render texture on slime bodies had long changed by playback, and repeating it
                // made the bodies vanish. Only texture assets, and only changes, are kept.
                Texture texture = null;
                if (raw.Kind == CallKind.Texture)
                {
                    texture = raw.Ref != IntPtr.Zero ? new Texture(raw.Ref) : null;
                    if (texture && texture.TryCast<RenderTexture>() != null)
                        continue;
                    var key = (at.Track, at.Slot, raw.Id);
                    if (session.LastTextures.TryGetValue(key, out var last) && last == raw.Ref)
                        continue;
                    session.LastTextures[key] = raw.Ref;
                }
                at.Track.Calls.Add(new LookCall
                {
                    Time = t, Slot = at.Slot, Kind = raw.Kind, Id = raw.Id, Value = raw.Value, Text = raw.Text,
                    Shader = raw.Kind == CallKind.Shader && raw.Ref != IntPtr.Zero ? new Shader(raw.Ref) : null,
                    Texture = texture,
                });
            }
        }

        // A mesh the original now has and its copy does not, such as the detailed mesh the engine's LOD streams in a moment
        // after an enemy appears. A readable mesh is copied, so the engine unloading it later cannot take it from the
        // replay. A streamed LOD mesh is not readable, its data lives only on the GPU, and a copy of it has no shape, so it
        // is used as it is. Stops after MaxMeshSwaps per track in case a mesh is rebuilt every frame.
        private static void RecordMeshes(Track track, float t)
        {
            var rig = track.Source;
            var copy = track.Copy?.Rig;
            bool first = track.LastMeshes == null;
            track.LastMeshes ??= new IntPtr[rig.Renderers.Length];
            for (int r = 0; r < rig.Renderers.Length; r++)
            {
                var mesh = rig.MeshOf(r);
                var pointer = mesh ? mesh.Pointer : IntPtr.Zero;
                bool changed = first ? copy != null && r < copy.Renderers.Length && (copy.MeshOf(r)?.Pointer ?? IntPtr.Zero) != pointer : pointer != track.LastMeshes[r];
                track.LastMeshes[r] = pointer;
                if (!changed || !mesh || track.MeshSwaps >= MaxMeshSwaps)
                    continue;
                track.MeshSwaps++;
                Mesh owned = null;
                if (mesh.isReadable)
                    try
                    {
                        owned = UnityEngine.Object.Instantiate(mesh).TryCast<Mesh>();
                    }
                    catch
                    {
                        owned = null;
                    }
                track.Calls.Add(new LookCall { Time = t, Slot = r, Kind = CallKind.MeshSwap, Mesh = owned ? owned : mesh });
            }
        }
    }
}
