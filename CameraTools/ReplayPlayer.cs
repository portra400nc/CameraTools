using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    internal abstract record PlayState
    {
        public sealed record Idle : PlayState;

        // Solo: one replay on its own clock, from Started, while the free camera stays the user's. Linked: the replays
        // linked to the playing camera path, on the path's clock. RestoreScale is the game speed a frozen world returns to.
        public abstract record Playing(Replay[] Replays, float? RestoreScale) : PlayState;

        public sealed record Solo(Replay Replay, float Started, float? RestoreScale) : Playing(new[] { Replay }, RestoreScale)
        {
            public float Elapsed => Time.unscaledTime - Started;
        }

        public sealed record Linked(Replay[] All, float? RestoreScale) : Playing(All, RestoreScale);
    }

    // Plays replays on script-free copies. The real party, weapons and enemies are hidden while they play: the game keeps
    // an off-field party member from drawing, while its copy draws.
    internal static class ReplayPlayer
    {
        private static readonly List<(Transform Root, bool On)> hidden = new();
        private static readonly HashSet<StandIn> shown = new();
        private static readonly List<(Rig Rig, RigPose A, RigPose B, float Blend, bool Opaque)> written = new();
        private static readonly Dictionary<Replay, float> lastTime = new();

        public static PlayState State { get; private set; } = new PlayState.Idle();

        public static bool Idle => State is PlayState.Idle;

        public static PlayState.Solo Solo => State as PlayState.Solo;

        public static void PlaySolo(Replay replay)
        {
            Stop(null);
            Begin(scale => new PlayState.Solo(replay, Time.unscaledTime, scale), new[] { replay });
        }

        // A camera path started: every replay linked to it plays along. A recording that plays its path records instead.
        public static void PathStarted(CameraPath path)
        {
            Stop(null);
            if (ReplayRecorder.Active)
                return;
            var linked = Replays.All.Where(replay => replay.PathId == path.Id).ToArray();
            if (linked.Length > 0)
                Begin(scale => new PlayState.Linked(linked, scale), linked);
        }

        public static void PathStopped()
        {
            if (State is PlayState.Linked)
                Stop(null);
        }

        // A deleted replay stops the playback it is part of.
        public static void Forget(Replay replay)
        {
            if (State is PlayState.Playing playing && playing.Replays.Contains(replay))
                Stop(null);
        }

        private static void Begin(Func<float?, PlayState.Playing> state, Replay[] replays)
        {
            hidden.Clear();
            foreach (var root in ReplayScene.Children(ReplayScene.AvatarRoot).Concat(ReplayScene.Children(ReplayScene.EquipRoot))
                .Concat(ReplayScene.Children(ReplayScene.MonsterRoot)).Concat(ReplayScene.Children(ReplayScene.MonsterEquipRoot)))
            {
                hidden.Add((root, root.gameObject.activeSelf));
                root.gameObject.SetActive(false);
            }
            float? restore = null;
            if (replays.Any(replay => replay.World == ReplayWorld.Frozen))
            {
                restore = Time.timeScale;
                Time.timeScale = 0f;
            }
            CameraTools.SetPlayerInput(false, "Replays");
            foreach (var replay in replays)
            {
                var dissolving = replay.Tracks.Where(track => track.Kind is TrackKind.Character or TrackKind.Monster && track.Materials != null).ToList();
                var dissolve = Rig.FindShader(dissolving.Select(track => track.Materials), Rig.DissolveShader);
                foreach (var track in dissolving)
                    track.Source.UseDissolve(track.Materials, dissolve);
                foreach (var track in replay.Tracks)
                    track.Rewind();
                lastTime[replay] = 0f;
            }
            State = state(restore);
            Melon<CameraTools>.Logger.Msg($"Replays: playing {string.Join(", ", replays.Select(replay => replay.Name))}"
                + $"{(State is PlayState.Linked ? " with its path" : " on its own")}, world {(restore != null ? "frozen" : "running")}.");
        }

        // message is the toast to show, or null for none.
        public static void Stop(string message)
        {
            if (State is not PlayState.Playing playing)
                return;
            State = new PlayState.Idle();
            foreach (var (root, on) in hidden)
                if (root)
                    root.gameObject.SetActive(on);
            hidden.Clear();
            foreach (var copy in playing.Replays.SelectMany(replay => replay.Tracks).Select(track => track.Copy).Where(copy => copy is { Usable: true }).Distinct())
            {
                copy.Rig.Root.gameObject.SetActive(false);
                copy.Owner = null;
            }
            if (playing.RestoreScale is float scale)
                Time.timeScale = scale;
            CameraTools.SetPlayerInput(Controls.Owner == PadOwner.Game, "Replays");
            written.Clear();
            lastTime.Clear();
            if (message != null)
                CameraUi.Toast(message);
        }

        public static void Update() => Replays.Guard("playback", Frame, () => Stop("Playback stopped; see the log"));

        private static void Frame()
        {
            if (State is not PlayState.Playing playing)
                return;
            if (!CameraTools.freecamActive)
            {
                Stop("Playback stopped");
                return;
            }
            float? pathSeconds = PathPlayback.State is PlaybackState.Playing path ? path.PassTime : null;
            if (playing is PlayState.Solo solo && solo.Elapsed > solo.Replay.Duration)
            {
                Stop("Replay finished");
                return;
            }
            if (playing is PlayState.Linked && pathSeconds == null)
            {
                Stop(null);
                return;
            }
            written.Clear();
            shown.Clear();
            foreach (var replay in playing.Replays)
            {
                float t = playing is PlayState.Solo each ? each.Elapsed : ReplayText.LinkedTime(pathSeconds.Value, replay.Offset, replay.Duration);
                // A looping path starts its replays again with each pass.
                if (t < lastTime[replay])
                    foreach (var track in replay.Tracks)
                        track.Rewind();
                lastTime[replay] = t;
                if (replay.Latest(TrackKind.Character, t) is { } character)
                    Play(character, t);
                if (replay.Latest(TrackKind.Weapon, t) is { } weapon)
                    Play(weapon, t);
                foreach (var track in replay.Tracks)
                    if (track.Kind is TrackKind.Effect or TrackKind.Gadget or TrackKind.Monster or TrackKind.MonsterWeapon && track.Covers(t))
                        Play(track, t);
            }
            foreach (var track in playing.Replays.SelectMany(replay => replay.Tracks))
                if (track.Copy is { Usable: true } copy && !shown.Contains(copy) && copy.Rig.Root.gameObject.activeSelf)
                {
                    copy.Rig.Root.gameObject.SetActive(false);
                    copy.Owner = null;
                }
        }

        private static void Play(Track track, float t)
        {
            var copy = track.Copy;
            if (copy is not { Usable: true } || shown.Contains(copy) || !track.Poses.Sample(t, out var a, out var b, out float blend))
                return;
            var rig = copy.Rig;
            bool opaque = track.Kind is TrackKind.Character or TrackKind.Weapon;
            if (!copy.Shown)
            {
                copy.Shown = true;
                ReplayCopies.Show(rig.Root.gameObject);
                if (opaque)
                    rig.ResumeLods();
            }
            if (!rig.Root.gameObject.activeSelf)
                rig.Root.gameObject.SetActive(true);
            rig.WriteTransforms(a, b, blend);
            rig.WriteLooks(a, b, blend, opaque);
            // One copy plays every play of a pooled effect, so a new play moves it to where that play starts. A trail
            // would draw that jump: energy orbs left lines across the arena that came back with each orb.
            if (copy.Owner != track)
            {
                copy.Owner = track;
                if ((track.Working ?? track.Materials) is { } materials)
                    rig.UseMaterials(materials);
                foreach (var trail in copy.Trails)
                    if (trail)
                        trail.Clear();
            }
            if ((track.Working ?? track.Materials) is { } current)
                rig.WriteDissolve(a, b, blend, current);
            ApplyCalls(track, rig, t);
            if (track.Kind is TrackKind.Effect or TrackKind.Gadget)
            {
                float local = t - track.Start;
                foreach (var system in copy.Tops)
                    if (system)
                        system.Simulate(local, true, true, true);
            }
            shown.Add(copy);
            written.Add((rig, a, b, blend, opaque));
        }

        // Repeats the game's recorded setter calls on the copy's own materials and renderers up to a time. Characters and
        // their weapons take property changes only: their materials stay on the dissolve shader, which draws.
        private static void ApplyCalls(Track track, Rig rig, float t)
        {
            var working = track.Working;
            if (working == null)
                return;
            bool character = track.Kind is TrackKind.Character or TrackKind.Weapon;
            while (track.Cursor < track.Calls.Count && track.Calls[track.Cursor].Time <= t)
            {
                var call = track.Calls[track.Cursor++];
                try
                {
                    Apply(call, track, rig, working, character);
                }
                catch (Exception e)
                {
                    CameraTools.LogOnce($"Replays: repeating a {call.Kind} call on {rig.Name} failed: {e.Message}");
                }
            }
        }

        private static void Apply(LookCall call, Track track, Rig rig, Material[][] working, bool character)
        {
            switch (call.Kind)
            {
                case CallKind.MeshSwap:
                    if (call.Slot < rig.Renderers.Length && call.Mesh)
                        rig.SetMesh(call.Slot, call.Mesh);
                    return;
                case CallKind.CustomFloat or CallKind.RendererSet or CallKind.EnableProperty:
                    var renderer = call.Slot < rig.Renderers.Length ? rig.Renderers[call.Slot] : null;
                    if (!renderer)
                        return;
                    if (call.Kind == CallKind.CustomFloat)
                        renderer.SetCustomPropertyFloat(call.Id, call.Value.x, true);
                    else if (call.Kind == CallKind.RendererSet)
                        ReplayHooks.Apply(call.Id, renderer.Pointer, call.Int, call.Value.x);
                    else
                        ReplayHooks.SetEnableProperty(renderer, call.Int != 0);
                    return;
            }
            var (r, m) = track.Source.Slots[call.Slot];
            if (r >= working.Length || working[r] == null || m >= working[r].Length || !working[r][m])
                return;
            var material = working[r][m];
            switch (call.Kind)
            {
                case CallKind.Swap when !character && call.Snapshot:
                    working[r][m] = new Material(call.Snapshot);
                    if (rig.Renderers[r])
                        rig.Renderers[r].sharedMaterials = working[r];
                    break;
                case CallKind.Shader when !character && call.Shader:
                    material.shader = call.Shader;
                    break;
                case CallKind.Float:
                    material.SetFloat(call.Id, call.Value.x);
                    break;
                case CallKind.Int:
                    material.SetInt(call.Id, (int)call.Value.x);
                    break;
                case CallKind.Color:
                    material.SetColor(call.Id, new Color(call.Value.x, call.Value.y, call.Value.z, call.Value.w));
                    break;
                case CallKind.Vector:
                    material.SetVector(call.Id, call.Value);
                    break;
                case CallKind.Texture:
                    material.SetTexture(call.Id, call.Texture);
                    break;
                case CallKind.KeywordOn:
                    material.EnableKeyword(call.Text);
                    break;
                case CallKind.KeywordOff:
                    material.DisableKeyword(call.Text);
                    break;
                case CallKind.KeywordIdOn:
                    material.EnableKeywordID(call.Id);
                    break;
                case CallKind.KeywordIdOff:
                    material.DisableKeywordID(call.Id);
                    break;
            }
        }

        // The game's scripts may set renderers, property blocks or face shapes after the late update, so those are
        // written again before the frame is drawn.
        public static void PreCull() => Replays.Guard("playback", () =>
        {
            foreach (var (rig, a, b, blend, opaque) in written)
                if (rig.Root)
                    rig.WriteLooks(a, b, blend, opaque);
        }, () => Stop("Playback stopped; see the log"));
    }
}
