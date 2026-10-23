using System.Runtime.InteropServices;
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

        // Edit replay: one replay held at the playhead T, in replay seconds, while the free camera frames the shot. Plays
        // moves T on in real time, and Look poses the camera from the replay's path. Switches are when the party switched.
        public sealed record Editing(Replay Replay, float[] Switches, float? RestoreScale) : Playing(new[] { Replay }, RestoreScale)
        {
            public float T { get; set; }

            public bool Plays { get; set; }

            public bool Look { get; set; }
        }
    }

    // Plays replays on script-free copies. The real party, weapons and enemies are hidden while they play: the game keeps
    // an off-field party member from drawing, while its copy draws.
    internal static class ReplayPlayer
    {
        private static readonly List<(Transform Root, bool On)> hidden = new();
        private static readonly HashSet<StandIn> shown = new();
        private static readonly List<(Rig Rig, RigPose A, RigPose B, float Blend, bool Opaque)> written = new();

        public static PlayState State { get; private set; } = new PlayState.Idle();

        public static bool Idle => State is PlayState.Idle;

        public static PlayState.Solo Solo => State as PlayState.Solo;

        public static PlayState.Editing Editing => State as PlayState.Editing;

        public static void PlaySolo(Replay replay)
        {
            Stop(null);
            Begin(scale => new PlayState.Solo(replay, Time.unscaledTime, scale), new[] { replay });
        }

        // Holds a replay at its start for Edit replay, the same way playback shows it.
        public static void Edit(Replay replay)
        {
            Stop(null);
            var switches = EditTimeline.Switches(replay.Of(TrackKind.Character).Select(track => track.Poses).ToArray());
            Begin(scale => new PlayState.Editing(replay, switches, scale), new[] { replay });
        }

        // When the last replay linked to a path ends, in seconds after the path starts; 0 when none would play along.
        public static float LinkedEnd(CameraPath path) => ReplayRecorder.Active ? 0f
            : Replays.All.Where(replay => replay.PathId == path.Id).Select(replay => replay.Offset + replay.Duration).DefaultIfEmpty(0f).Max();

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
            foreach (var replay in replays)
            {
                var dissolving = replay.Tracks.Where(track => track.Kind is TrackKind.Character or TrackKind.Monster && track.Materials != null).ToList();
                var dissolve = Rig.FindShader(dissolving.Select(track => track.Materials), Rig.DissolveShader);
                foreach (var track in dissolving)
                    track.Source.UseDissolve(track.Materials, dissolve);
                foreach (var track in replay.Tracks)
                    track.LooksAt = float.PositiveInfinity;
            }
            State = state(restore);
            Melon<CameraTools>.Logger.Msg($"Replays: playing {string.Join(", ", replays.Select(replay => replay.Name))}"
                + $"{State switch { PlayState.Linked => " with its path", PlayState.Editing => " to edit", _ => " on its own" }}, world {(restore != null ? "frozen" : "running")}.");
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
            written.Clear();
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
                Stop(playing is PlayState.Editing editing ? ReplayEditor.Closed(ReplayEditor.PathOf(editing.Replay)) : "Playback stopped");
                return;
            }
            float? pathSeconds = PathPlayback.State is PlaybackState.Playing path ? path.Clock : null;
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
                float t = playing switch
                {
                    PlayState.Solo each => each.Elapsed,
                    PlayState.Editing edit => edit.T,
                    _ => ReplayText.LinkedTime(pathSeconds.Value, replay.Offset, replay.Duration),
                };
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

        // Diagnostic: each character renderer the recording draws, once, as the copy has it then and as the original had it.
        private static void Describe(Track track, RigPose pose, float t)
        {
            var rig = track.Copy.Rig;
            track.Described ??= new bool[rig.Renderers.Length];
            for (int r = 0; r < rig.Renderers.Length && r < pose.Drawn.Length; r++)
            {
                if (track.Described[r] || !pose.Drawn[r])
                    continue;
                track.Described[r] = true;
                string name = rig.Renderers[r] ? rig.Renderers[r].name : "?";
                Melon<CameraTools>.Logger.Msg($"Replays: note: {track.Source.Name} {name} at {t:0.00} s: copy {rig.Describe(r)}"
                    + $" | at copy {track.AtCopy?.ElementAtOrDefault(r) ?? "-"} | at end {track.AtEnd?.ElementAtOrDefault(r) ?? "-"}");
            }
        }

        private static void Play(Track track, float t)
        {
            var copy = track.Copy;
            if (copy is not { Usable: true } || shown.Contains(copy) || !track.Poses.Sample(t, out var a, out var b, out float blend))
                return;
            // A time earlier than the copy's looks rewinds them, as a looping path's next pass or a seek back does, so the
            // copy shows what playing there from the start shows. A jump either way clears the trails, which would draw it.
            bool back = t < track.LooksAt;
            if (back)
                track.Rewind();
            bool jumped = back || t - track.LooksAt > Timeline<RigPose>.Gap;
            track.LooksAt = t;
            var rig = copy.Rig;
            bool opaque = track.Kind is TrackKind.Character or TrackKind.Weapon;
            if (!copy.Shown)
            {
                copy.Shown = true;
                ReplayCopies.Show(rig.Root.gameObject);
                if (track.Kind == TrackKind.Character)
                    rig.HoldFullDetail();
                else if (track.Kind == TrackKind.Weapon)
                    rig.ResumeLods();
            }
            if (!rig.Root.gameObject.activeSelf)
                rig.Root.gameObject.SetActive(true);
            rig.WriteTransforms(a, b, blend);
            rig.WriteLooks(a, b, blend, opaque);
            if (ReplayRecorder.HeavyNotes && track.Kind == TrackKind.Effect && !track.LinesShown && t - track.Start >= 1f && rig.Name.Contains("WaterTornado"))
            {
                track.LinesShown = true;
                foreach (var tube in rig.Renderers)
                    if (tube && tube.TryCast<LineRenderer>() != null && ReplayRecorder.RealNatives.TryGetValue(tube.name, out var real))
                    {
                        var mine = ReplayRecorder.NativeBytes(tube);
                        var differ = new List<string>();
                        for (int at = 0; at + 8 <= real.Length && at + 8 <= mine.Length; at += 8)
                        {
                            long a8 = BitConverter.ToInt64(real, at), b8 = BitConverter.ToInt64(mine, at);
                            if (a8 == b8)
                                continue;
                            bool pointers = (a8 >> 40) is > 0 and < 0x8000 && (b8 >> 40) is > 0 and < 0x8000;
                            differ.Add($"+0x{at:x}: {a8:x} / {b8:x}{(pointers ? " (pointers)" : $" ({BitConverter.ToSingle(real, at):0.###},{BitConverter.ToSingle(real, at + 4):0.###} / {BitConverter.ToSingle(mine, at):0.###},{BitConverter.ToSingle(mine, at + 4):0.###})")}");
                        }
                        Melon<CameraTools>.Logger.Msg($"Replays: note: engine {tube.name} real / copy: {differ.Count} of {real.Length / 8} qwords differ");
                    }
                if (ReplayRecorder.BeamAtOneSecond is { } recorded && rig.Name.EndsWith("WaterTornado"))
                {
                    var now = ReplayRecorder.ScanCustom(rig);
                    var keys = recorded.Keys.Concat(now.Keys).Where(key => !key.StartsWith("global")).Distinct().ToList();
                    var differ = keys.Where(key => recorded.TryGetValue(key, out var a) && now.TryGetValue(key, out var b) && a != b).ToList();
                    var lost = keys.Where(key => !now.ContainsKey(key)).ToList();
                    var gained = keys.Where(key => !recorded.ContainsKey(key)).ToList();
                    Melon<CameraTools>.Logger.Msg($"Replays: note: copy beam against the recorded beam: {differ.Count} differ, {lost.Count} only recorded, {gained.Count} only on the copy, of {keys.Count}");
                    foreach (var key in differ.Take(120))
                        Melon<CameraTools>.Logger.Msg($"Replays: note: copy beam {key}: recorded {recorded[key]}, copy {now[key]}");
                    foreach (var key in lost.Take(10).Concat(gained.Take(10)))
                        Melon<CameraTools>.Logger.Msg($"Replays: note: copy beam key {key} on one side only");
                }
                foreach (var system in ReplayScene.All<ParticleSystem>(rig.Root))
                    Melon<CameraTools>.Logger.Msg($"Replays: note: copy of {rig.Name} system plays at {t:0.00} s: {ReplayCopies.DescribeSystem(system)}");
                for (int r = 0; r < rig.Renderers.Length; r++)
                    if (rig.Renderers[r] is { } line && line && line.TryCast<LineRenderer>() != null)
                        Melon<CameraTools>.Logger.Msg($"Replays: note: copy of {rig.Name} line {line.name} plays at {t:0.00} s: {rig.DescribeLine(r)}"
                            + $"; calls on it: [{string.Join(", ", track.Calls.Where(call => CallTarget.IsRenderer(call.Kind) ? call.Slot == r : call.Slot < track.Source.Slots.Length && track.Source.Slots[call.Slot].Renderer == r).GroupBy(call => call.Kind).Select(group => $"{group.Key} x{group.Count()}"))}]"
                            + $"; root at {rig.Root.position}");
                for (int r = 0; r < rig.Renderers.Length; r++)
                {
                    var meshes = track.Calls.Where(call => call.Kind == CallKind.LineMesh && call.Slot == r && call.Line != null).Select(call => call.Line).ToList();
                    if (meshes.Count == 0)
                        continue;
                    int Distinct(Func<LineMesh, byte[]> part) => meshes.Select(part).Select(bytes => bytes == null ? "-" : Convert.ToBase64String(System.Security.Cryptography.SHA1.HashData(bytes))).Distinct().Count();
                    var first = meshes[0];
                    var settings = track.Given?.ElementAtOrDefault(r)?.Values;
                    Melon<CameraTools>.Logger.Msg($"Replays: note: {rig.Renderers[r].name} {meshes.Count} meshes: {Distinct(m => m.Positions)} position sets, {Distinct(m => m.Uv0)} uv0 sets, "
                        + $"{Distinct(m => m.Colors)} colour sets, {Distinct(m => m.Uv1)} uv1 sets, {Distinct(m => m.Normals)} normal sets; first has {first.PositionCount} positions, "
                        + $"{first.TriangleCount} indices, uv1 {first.Uv1Count}; settings at start [{string.Join(", ", settings?.Select(pair => $"{ReplayHooks.Setters[pair.Key].Name}={pair.Value.Int}/{pair.Value.Float:0.##}") ?? Array.Empty<string>())}]");
                    string Uv(LineMesh mesh) => mesh.Uv0 == null ? "-" : string.Join(" ", Enumerable.Range(0, 6).Select(i => $"({BitConverter.ToSingle(mesh.Uv0, i * 8):0.###},{BitConverter.ToSingle(mesh.Uv0, i * 8 + 4):0.###})"));
                    string Colour(LineMesh mesh) => mesh.Colors == null ? "-" : string.Join(" ", Enumerable.Range(0, 4).Select(i => $"{mesh.Colors[i * 4]}/{mesh.Colors[i * 4 + 1]}/{mesh.Colors[i * 4 + 2]}/{mesh.Colors[i * 4 + 3]}"));
                    foreach (int i in new[] { 0, 30, 31, 32, 60 }.Where(i => i < meshes.Count))
                        Melon<CameraTools>.Logger.Msg($"Replays: note: {rig.Renderers[r].name} mesh {i}: {meshes[i].PositionCount} positions, uv0 {Uv(meshes[i])}, colours {Colour(meshes[i])}");
                }
                foreach (var group in track.Calls.Where(call => call.Kind is CallKind.Color or CallKind.Vector or CallKind.Float && call.Slot < track.Source.Slots.Length)
                    .GroupBy(call => (call.Slot, call.Kind, call.Id)))
                {
                    var (r, m) = track.Source.Slots[group.Key.Slot];
                    var renderer = r < rig.Renderers.Length ? rig.Renderers[r] : null;
                    if (!renderer || renderer.TryCast<LineRenderer>() == null)
                        continue;
                    var material = m < renderer.sharedMaterials.Length ? renderer.sharedMaterials[m] : null;
                    var calls = group.ToList();
                    string Show(Vector4 v) => $"({v.x:0.###},{v.y:0.###},{v.z:0.###},{v.w:0.###})";
                    string now = !material ? "no material" : group.Key.Kind == CallKind.Float ? $"{material.GetFloat(group.Key.Id):0.###}"
                        : group.Key.Kind == CallKind.Color ? Show((Vector4)material.GetColor(group.Key.Id)) : Show(material.GetVector(group.Key.Id));
                    Melon<CameraTools>.Logger.Msg($"Replays: note: {renderer.name} material {m} {group.Key.Kind} {group.Key.Id}: {calls.Count} calls, "
                        + $"first {Show(calls[0].Value)} at {calls[0].Time - track.Start:0.00} s, last {Show(calls[^1].Value)} at {calls[^1].Time - track.Start:0.00} s; copy's material has {now}"
                        + $" ({(material && track.Working?.ElementAtOrDefault(r)?.ElementAtOrDefault(m) is { } working && working.Pointer == material.Pointer ? "working" : "not the working material")})");
                }
            }
            if (track.Kind == TrackKind.Character)
            {
                Describe(track, a, t);
                track.FirstDrawn ??= t;
                if (ReplayRecorder.HeavyNotes && !track.MaterialsNoted && t - track.FirstDrawn >= 0.5f)
                {
                    track.MaterialsNoted = true;
                    for (int r = 0; r < rig.Renderers.Length; r++)
                        if (rig.Renderers[r] && rig.Renderers[r].enabled && rig.Renderers[r].gameObject.activeInHierarchy)
                            Melon<CameraTools>.Logger.Msg($"Replays: note: {track.Source.Name} {rig.Renderers[r].name} at {t:0.00} s plays with {rig.Describe(r)}"
                                + $"; recorded dissolve [{string.Join(", ", Enumerable.Range(0, rig.Slots.Length).Where(slot => rig.Slots[slot].Renderer == r).Select(slot => slot < a.Dissolve.Length ? $"{a.Dissolve[slot]:0.##}" : "-"))}]");
                }
            }
            // One copy plays every play of a pooled effect, so a new play moves it to where that play starts. A trail
            // would draw that jump: energy orbs left lines across the arena that came back with each orb.
            bool fresh = copy.Owner != track || jumped;
            if (fresh)
            {
                if (copy.Owner != track && (track.Working ?? track.Materials) is { } materials)
                    rig.UseMaterials(materials);
                if (copy.Owner != track && track.Given != null)
                    for (int r = 0; r < track.Given.Length && r < rig.Renderers.Length; r++)
                        if (track.Given[r] != null && rig.Renderers[r])
                            ReplayHooks.Give(track.Given[r], rig.Renderers[r]);
                copy.Owner = track;
                foreach (var trail in copy.Trails)
                    if (trail)
                        trail.Clear();
            }
            if ((track.Working ?? track.Materials) is { } current)
                rig.WriteDissolve(a, b, blend, current);
            ApplyCalls(track, rig, t);

            if (track.Kind is TrackKind.Effect or TrackKind.Gadget)
            {
                // Playing forward, the systems run on from the last frame, which grows their trails as the game's do: Neuvillette's
                // beam flows by its particles' trails. Restarting them at each frame drew the trails from nothing every time, and
                // the beam stood still. A jump, a rewind or a new play simulates them again from the start.
                float local = t - track.Start;
                float step = local - copy.SimulatedTo;
                bool runOn = !fresh && step >= 0f && step < 0.25f;
                foreach (var system in copy.Tops)
                    if (system)
                    {
                        if (!runOn)
                            system.Simulate(local, true, true, true);
                        else if (step > 0f)
                            system.Simulate(step, true, false, false);
                    }
                copy.SimulatedTo = local;
                rig.WriteParticles(a);
            }
            shown.Add(copy);
            written.Add((rig, a, b, blend, opaque));
        }

        // Repeats the game's recorded setter calls on the copy's own materials and renderers up to a time. Characters and
        // their weapons take property changes only: their materials stay on the dissolve shader, which draws. From a
        // rewind, only the calls whose effect lasts to t run, so a seek near the end of a long replay stays quick.
        private static void ApplyCalls(Track track, Rig rig, float t)
        {
            var working = track.Working;
            if (working == null)
                return;
            bool character = track.Kind is TrackKind.Character or TrackKind.Weapon;
            int count = 0;
            if (track.Cursor == 0)
                while (count < track.Calls.Count && track.Calls[count].Time <= t)
                    count++;
            if (count > 0)
            {
                var calls = track.Calls;
                foreach (int index in LookCalls.Live(count, index => CallTarget.Of(calls[index].Kind, calls[index].Slot, calls[index].Id, calls[index].Text, calls[index].Int), swaps: !character))
                    Repeat(calls[index], track, rig, working, character);
                track.Cursor = count;
            }
            while (track.Cursor < track.Calls.Count && track.Calls[track.Cursor].Time <= t)
                Repeat(track.Calls[track.Cursor++], track, rig, working, character);
        }

        private static void Repeat(LookCall call, Track track, Rig rig, Material[][] working, bool character)
        {
            try
            {
                Apply(call, track, rig, working, character);
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Replays: repeating a {call.Kind} call on {rig.Name} failed: {e.Message}");
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
                case CallKind.CustomFloat or CallKind.CustomVector or CallKind.CustomColor or CallKind.CustomTexture or CallKind.RendererSet
                    or CallKind.EnableProperty or CallKind.LineMesh:
                    var renderer = call.Slot < rig.Renderers.Length ? rig.Renderers[call.Slot] : null;
                    if (!renderer)
                        return;
                    if (call.Kind is CallKind.CustomFloat or CallKind.CustomVector or CallKind.CustomColor or CallKind.CustomTexture)
                        ReplayHooks.SetCustom(renderer, call.Kind, call.Int, call.Id, call.Value, call.Texture);
                    else if (call.Kind == CallKind.LineMesh)
                        call.Line?.Give(renderer);
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
                case CallKind.TextureOffset:
                    material.SetTextureOffset(call.Id, new Vector2(call.Value.x, call.Value.y));
                    break;
                case CallKind.TextureScale:
                    material.SetTextureScale(call.Id, new Vector2(call.Value.x, call.Value.y));
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
