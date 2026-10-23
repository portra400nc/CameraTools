using MelonLoader;
using UnityEngine;
using Object = UnityEngine.Object;

namespace CameraTools
{
    // One recorded hierarchy over time. A character or weapon has one track per replay, an effect or gadget one per play,
    // and an enemy one per stretch with an unchanged structure.
    internal sealed class Track
    {
        public TrackKind Kind;
        public Rig Source;
        public StandIn Copy;
        public float Start, End = float.MaxValue;
        // An effect's renderers as the game had set them up when its play began, before the recorder watched them.
        public ReplayHooks.Given[] Given;
        public readonly Timeline<RigPose> Poses = new();

        // The materials captured for the copy, and the playback's own clones of them, which the recorded calls change.
        public Material[][] Materials, Working;

        // The game's setter calls on this object's materials and renderers, and playback's place in them.
        public readonly List<LookCall> Calls = new();
        public int Cursor;

        // The replay time the copy's looks were last brought to. Playing an earlier time rewinds them first; a playback
        // starts at infinity, so its first play starts from fresh materials.
        public float LooksAt = float.PositiveInfinity;

        // The recorder's state: when the current run on the field began and when an effect last had live particles,
        // whether the captured materials are from a settled character, the structure of an enemy's stretch, and the
        // materials and meshes last seen in each slot and renderer.
        public float RunStart, LastAlive;
        public bool Settled;
        public (int Hash, string[] Parts) Structure;
        public IntPtr[] LastSlots, LastMeshes;
        public int MeshSwaps;

        public bool Covers(float time) => time >= Start && time <= End;

        // Back to the start of the calls, on fresh clones of the captured materials and the copy as it was made.
        public void Rewind()
        {
            Destroy(Working);
            Working = Materials?.Select(row => row?.Select(material => material ? new Material(material) : null).ToArray()).ToArray();
            Cursor = 0;
            if (Copy is { Usable: true })
                Copy.Reset();
            if (Copy?.Owner == this)
                Copy.Owner = null;
        }

        public void Release()
        {
            Destroy(Working);
            Destroy(Materials);
            foreach (var call in Calls)
                if (call.Snapshot)
                    Object.Destroy(call.Snapshot);
            if (Copy is { Own: true })
                Copy.Destroy();
        }

        private static void Destroy(Material[][] materials)
        {
            foreach (var material in materials?.SelectMany(row => row ?? Array.Empty<Material>()) ?? Array.Empty<Material>())
                if (material)
                    Object.Destroy(material);
        }
    }

    // One recording: the party, their weapons, effects and enemies, and how it plays. PathId links it to a camera path,
    // which plays it Offset seconds after the path starts.
    internal sealed class Replay
    {
        public string Name;
        public float Duration;
        public readonly List<Track> Tracks = new();
        public string PathId;
        public float Offset;
        public ReplayWorld World;
        public string[] Party = Array.Empty<string>();
        public int Enemies;

        public IEnumerable<Track> Of(TrackKind kind) => Tracks.Where(track => track.Kind == kind);

        // The character or weapon on the field at a time.
        public Track Latest(TrackKind kind, float time) => Timeline<RigPose>.Latest(Of(kind), track => track.Poses, time);

        public void Release()
        {
            foreach (var track in Tracks)
                track.Release();
        }
    }

    // The session's replays, the one the Replays tab points at, and the recording options. Replays last until the game
    // closes.
    internal static class Replays
    {
        public const float Limit = 300f;
        public const float MaxOffset = 10f;

        public static readonly List<Replay> All = new();

        // -1 only while there are no replays.
        public static int Active { get; private set; } = -1;

        public static Replay Current => Active >= 0 ? All[Active] : null;

        public static bool HasReplay => Current != null;

        public static bool PlayPath = false, RecordEnemies = true, RecordEffects = true, Countdown = true;

        public static string Note => Current is { } replay ? ReplayText.Note(replay.Duration, replay.Party, replay.Enemies) : "Record a replay to start";

        public static string RecordNote => PlayPath && CameraPaths.HasPath ? $"Plays {CameraPaths.CurrentLabel} while you act"
            : $"Records until you stop it, up to {Limit / 60f:0} minutes";

        public static string PlayNote => PathNumber(Current) is int number ? $"Plays with {CameraPaths.Label(number - 1)}" : "Plays on its own; the free camera stays yours";

        // A path number's label, its name or "Path 2", or null for no path.
        public static string PathLabel(int? number) => number is int n ? CameraPaths.Label(n - 1) : null;

        // The number of the path a replay is linked to, or null when it has none or its path was deleted.
        public static int? PathNumber(Replay replay)
        {
            if (replay?.PathId == null)
                return null;
            int index = CameraPaths.Paths.FindIndex(path => path.Id == replay.PathId);
            return index >= 0 ? index + 1 : null;
        }

        public static string[] PathChoices() => new[] { "None" }.Concat(CameraPaths.Paths.Select((_, index) => CameraPaths.Label(index))).ToArray();

        public static int PathChoice => PathNumber(Current) ?? 0;

        public static void SetPathChoice(int choice)
        {
            if (Current is { } replay)
                replay.PathId = choice >= 1 && choice <= CameraPaths.Paths.Count ? CameraPaths.Paths[choice - 1].Id : null;
        }

        public static void SetOffset(float seconds)
        {
            if (Current is { } replay)
                replay.Offset = Math.Clamp(seconds, 0f, MaxOffset);
        }

        public static void SetWorld(ReplayWorld world)
        {
            if (Current is { } replay)
                replay.World = world;
        }

        public static void Select(int index)
        {
            if (index >= 0 && index < All.Count)
                Active = index;
        }

        public static void Add(Replay replay)
        {
            All.Add(replay);
            Active = All.Count - 1;
        }

        public static void Delete()
        {
            if (Current is not { } replay)
                return;
            ReplayPlayer.Forget(replay);
            replay.Release();
            All.RemoveAt(Active);
            Active = Math.Min(Active, All.Count - 1);
            CameraUi.Toast($"{replay.Name} deleted");
        }

        public static void Record() => ReplayRecorder.Arm();

        // With a linked path, the path plays and every replay linked to it plays along; without, the replay plays alone.
        public static void Play()
        {
            if (ReplayRecorder.Active)
                ReplayRecorder.Stop();
            if (Current is not { } replay || !CameraPaths.FreecamOn())
                return;
            if (PathNumber(replay) is int number)
            {
                CameraPaths.SelectPath(number - 1);
                PathPlayback.Play();
                return;
            }
            PathPlayback.Stop(null);
            ReplayPlayer.PlaySolo(replay);
        }

        // After the path playback's update, so a linked replay reads the path's clock as this frame has it.
        public static void Update()
        {
            ReplayHooks.Update();
            // A recording gives the game input, so the free camera's toggle stops it, not this.
            if (Controls.Pressed(CamAction.RecordReplay) && ReplayRecorder.Idle)
                Record();
            if (Controls.Pressed(CamAction.PlayReplay))
            {
                if (ReplayPlayer.Idle || ReplayPlayer.Editing != null)
                    Play();
                else if (ReplayPlayer.State is PlayState.Linked)
                    PathPlayback.Stop("Playback stopped");
                else
                    ReplayPlayer.Stop("Playback stopped");
            }
            // A replay linked to a path stops with the path.
            else if (ReplayPlayer.Solo != null && Controls.EscapePressed)
                ReplayPlayer.Stop("Playback stopped");
            Guard("starting the recording", ReplayRecorder.Update, ReplayRecorder.Stop);
            Guard("editing", ReplayEditor.Update, () => ReplayPlayer.Stop("Edit closed; see the log"));
            ReplayPlayer.Update();
        }

        // Camera.onPreCull of the game camera, after every script has placed what is drawn.
        public static void PreCull()
        {
            ReplayRecorder.PreCull();
            ReplayPlayer.PreCull();
        }

        // A failure stops the recording or playback it happened in rather than every frame after it.
        public static void Guard(string what, Action action, Action stop)
        {
            try
            {
                action();
            }
            catch (Exception e)
            {
                Melon<CameraTools>.Logger.Error($"Replays: {what} failed: {e}");
                try
                {
                    stop();
                }
                catch (Exception again)
                {
                    Melon<CameraTools>.Logger.Error($"Replays: stopping after that failed too: {again}");
                }
            }
        }
    }
}
