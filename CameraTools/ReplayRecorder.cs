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

    // The free camera, pad owner and pause from before a recording took them, given back when it stops.
    internal readonly record struct Handoff(bool Freecam, PadOwner Owner, bool Paused);

    // What one recording builds and what it keeps between frames. Path is the number of the path it plays, or null.
    internal sealed class RecordSession
    {
        public readonly Replay Replay = new();
        public float Start;
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

        // Closes the panel and records, after the countdown when it is on. You act while recording, so the pad goes to the
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
            var before = new Handoff(CameraTools.freecamActive, Controls.Owner, Time.timeScale == 0f);
            ReplayPlayer.Stop(null);
            PathPlayback.Stop(null);
            CameraUi.ClosePanel();
            var mod = Melon<CameraTools>.Instance;
            mod.SetFreecam(Replays.PlayPath && CameraPaths.CanPlay);
            if (Controls.Owner == PadOwner.CameraTools)
                mod.SetPadOwner(PadOwner.Game, announce: false);
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
            // Taking the pad back with L3+R3 stops the recording and keeps the free camera that taking it turns on.
            if (!Idle && Controls.Owner == PadOwner.CameraTools)
                Stop(restore: false);
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

        public static void Stop() => Stop(restore: true);

        // Keeps what was recorded as a new replay, unless it is too short to play. A path the recording started stops with it.
        // restore gives back the free camera and pad as they were before the recording.
        private static void Stop(bool restore)
        {
            var state = State;
            State = new RecordState.Idle();
            ReplayHooks.Recording = false;
            ReplayHooks.Watched = null;
            if (state is RecordState.Countdown countdown)
            {
                if (restore)
                    Restore(countdown.Before);
                CameraUi.Toast("Recording cancelled");
                return;
            }
            if (state is not RecordState.Recording { Session: var session })
                return;
            if (session.Path != null && PathPlayback.State is PlaybackState.Playing)
                PathPlayback.Stop(null);
            if (restore)
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
            replay.Name = ReplayText.NextName(Replays.All.Select(each => each.Name));
            replay.Party = session.Party.ToArray();
            replay.PathId = session.PathId;
            Replays.Add(replay);
            Melon<CameraTools>.Logger.Msg($"Replays: recorded {replay.Name}, {replay.Duration:0.00} s, {replay.Tracks.Count} tracks: "
                + $"{replay.Of(TrackKind.Character).Count()} characters, {replay.Enemies} enemies, {replay.Of(TrackKind.Effect).Count()} effect plays, "
                + $"{replay.Of(TrackKind.Gadget).Count()} gadgets, {replay.Tracks.Sum(track => track.Calls.Count)} look calls.");
            CameraUi.Toast(ReplayText.Recorded(replay.Name, replay.Duration, Replays.PathLabel(session.Path)));
        }

        private static void Restore(Handoff before)
        {
            var mod = Melon<CameraTools>.Instance;
            if (before.Owner == PadOwner.CameraTools && Controls.Owner != PadOwner.CameraTools)
                mod.SetPadOwner(PadOwner.CameraTools, announce: false);
            mod.SetFreecam(before.Freecam);
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
            };
            session.Replay.Tracks.Add(track);
            session.Held[root.Pointer] = track;
            if (kind == TrackKind.Character)
                session.Party.Add(ReplayText.CharacterName(root.name));
            return track;
        }

        private static Track Begin(RecordSession session, Transform root, TrackKind kind, float t)
        {
            var track = new Track { Kind = kind, Source = new Rig(root), Copy = StandIns.Shared(root, kind), Start = t, LastAlive = t };
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
                track.Copy = StandIns.Own(track.Source.Root, TrackKind.Character);
            }
            track.Materials = track.Source.TakeMaterials();
            if (track.Copy is { Usable: true } copy)
                copy.Rig.UseMaterials(track.Materials);
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
                        continue;
                    }
                    track = Begin(session, effect, TrackKind.Effect, t);
                }
                track.LastAlive = t;
                AddPose(session, track, t);
            }
            // The pool moves a finished effect out of /EffectPool; a play left open there kept its copy where the effect
            // was last seen, such as crystallize shields left behind at every switch.
            foreach (var (root, track) in session.Live.Where(pair => pair.Value.Kind == TrackKind.Effect && !seen.Contains(pair.Key)).ToList())
                Finish(session, root, track, track.LastAlive);
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
            foreach (var raw in ReplayHooks.Drain())
            {
                if (raw.Kind is CallKind.CustomFloat or CallKind.RendererSet or CallKind.EnableProperty)
                {
                    if (renderers.TryGetValue(raw.Self, out var owner))
                        owner.Track.Calls.Add(new LookCall { Time = t, Slot = owner.Renderer, Kind = raw.Kind, Id = raw.Id, Int = raw.Int, Value = raw.Value });
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
