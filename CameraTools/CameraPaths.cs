using System.Text.Json;
using System.Text.Json.Serialization;
using MelonLoader;
using MelonLoader.Utils;
using UnityEngine;
using NumericsQuaternion = System.Numerics.Quaternion;
using NumericsVector3 = System.Numerics.Vector3;

namespace CameraTools
{
    public sealed class CameraPath
    {
        public const float MinDuration = 1f;
        public const float MaxDuration = 120f;
        public const float DefaultDuration = 10f;

        public List<CameraNode> Nodes { get; } = new();
        public float Duration { get; set; } = DefaultDuration;
    }

    // Playback options shared by every path. Saved as they are, so the names are the JSON keys.
    public sealed class PathOptions
    {
        public const float MaxShake = 5f;

        public bool Loop { get; set; }
        public bool ConstantSpeed { get; set; } = true;
        public bool EaseIn { get; set; } = true;
        public bool EaseOut { get; set; } = true;
        public bool UnpauseGame { get; set; }
        public bool HideUi { get; set; } = true;
        public bool Countdown { get; set; } = true;
        public float MoveShakeFrequency { get; set; } = 1.5f;
        public float RotateShakeFrequency { get; set; } = 1.5f;
        public float MoveShakeStrength { get; set; }
        public float RotateShakeStrength { get; set; }
    }

    // The camera path library: the paths, which path and node the Paths tab points at, and the playback options, saved to
    // UserData/CameraTools/CameraPaths.json after every edit.
    internal static class CameraPaths
    {
        private static readonly string Folder = Path.Combine(MelonEnvironment.UserDataDirectory, "CameraTools");
        private static readonly string FilePath = Path.Combine(Folder, "CameraPaths.json");
        private static readonly JsonSerializerOptions Json = new()
        {
            WriteIndented = true,
            NumberHandling = JsonNumberHandling.AllowNamedFloatingPointLiterals,
        };

        public static List<CameraPath> Paths { get; } = new();
        public static PathOptions Options { get; private set; } = new();

        // -1 only while there are no paths.
        public static int Active { get; private set; } = -1;

        // 0 while the active path has no nodes.
        public static int Node { get; private set; }

        public static CameraPath Current => Active >= 0 ? Paths[Active] : null;

        public static bool HasPath => Current != null;

        public static bool HasNode => Current is { Nodes.Count: > 0 };

        public static bool CanPlay => Current is { Nodes.Count: >= 2 };

        public static string PathNote
        {
            get
            {
                var path = Current;
                if (path == null || path.Nodes.Count == 0)
                    return "Add a node to start a path";
                int count = path.Nodes.Count;
                return $"{count} node{(count == 1 ? "" : "s")} · {path.Duration:0.0} s";
            }
        }

        public static float Duration => Current?.Duration ?? CameraPath.DefaultDuration;

        public static void SelectPath(int index)
        {
            if (index < 0 || index >= Paths.Count || index == Active)
                return;
            Active = index;
            Node = 0;
            Save();
        }

        public static void SelectNode(int index)
        {
            if (!HasNode || index < 0 || index >= Current.Nodes.Count || index == Node)
                return;
            Node = index;
            Save();
        }

        public static void NewPath()
        {
            Paths.Add(new CameraPath());
            Active = Paths.Count - 1;
            Node = 0;
            Save();
            CameraUi.Toast($"Path {Active + 1} created");
        }

        public static void DeletePath()
        {
            if (!HasPath)
                return;
            int number = Active + 1;
            Paths.RemoveAt(Active);
            Active = Math.Min(Active, Paths.Count - 1);
            Node = 0;
            Save();
            CameraUi.Toast($"Path {number} deleted");
        }

        // With no path yet, adding a node starts one.
        public static void AddNode()
        {
            if (!Capture(out var node))
                return;
            if (!HasPath)
            {
                Paths.Add(new CameraPath());
                Active = Paths.Count - 1;
            }
            Current.Nodes.Add(node);
            Node = Current.Nodes.Count - 1;
            Save();
            CameraUi.Toast($"Node {Node + 1} added to path {Active + 1}");
        }

        public static void InsertBefore() => Insert(Node);

        public static void InsertAfter() => Insert(Node + 1);

        private static void Insert(int index)
        {
            if (!HasNode || !Capture(out var node))
                return;
            Current.Nodes.Insert(index, node);
            Node = index;
            Save();
            CameraUi.Toast($"Inserted as node {Node + 1}");
        }

        public static void ReplaceNode()
        {
            if (!HasNode || !Capture(out var node))
                return;
            Current.Nodes[Node] = node;
            Save();
            CameraUi.Toast($"Node {Node + 1} now uses the current view");
        }

        public static void GoToNode()
        {
            if (!HasNode || !FreecamOn())
                return;
            var node = Current.Nodes[Node];
            CameraTools.freecam.Snap(WorldShift.Relative(node.Position.ToUnity()), node.Rotation.ToUnity(), node.Fov);
            CameraUi.Toast($"Camera moved to node {Node + 1}");
        }

        public static void DeleteNode()
        {
            if (!HasNode)
                return;
            int number = Node + 1;
            Current.Nodes.RemoveAt(Node);
            Node = Math.Clamp(Node, 0, Math.Max(Current.Nodes.Count - 1, 0));
            Save();
            CameraUi.Toast($"Node {number} deleted");
        }

        public static void SetDuration(float seconds)
        {
            var path = Current;
            seconds = Math.Clamp(seconds, CameraPath.MinDuration, CameraPath.MaxDuration);
            if (path == null || path.Duration == seconds)
                return;
            path.Duration = seconds;
            Save();
        }

        public static void ChangeOptions(Action<PathOptions> change)
        {
            change(Options);
            Save();
        }

        public static bool FreecamOn()
        {
            if (CameraTools.freecamActive)
                return true;
            CameraUi.Toast("Turn on the free camera first");
            return false;
        }

        // A node records what is on screen, at its absolute position.
        private static bool Capture(out CameraNode node)
        {
            node = default;
            if (!FreecamOn())
                return false;
            var (position, rotation, fov) = CameraTools.freecam.Pose;
            node = new CameraNode(WorldShift.Absolute(position).ToNumerics(), rotation.ToNumerics(), fov);
            return true;
        }

        public static void Load()
        {
            if (!File.Exists(FilePath))
                return;
            try
            {
                Read(JsonSerializer.Deserialize<LibraryFile>(File.ReadAllText(FilePath), Json));
                Melon<CameraTools>.Logger.Msg($"Camera paths: loaded {Paths.Count} path{(Paths.Count == 1 ? "" : "s")} from {FilePath}.");
            }
            catch (Exception e)
            {
                string backup = FilePath + ".bak";
                Melon<CameraTools>.Logger.Warning($"Camera paths: {FilePath} could not be read ({e.Message}); moved it to {backup} and started empty.");
                try
                {
                    File.Move(FilePath, backup, true);
                }
                catch (Exception moveFailed)
                {
                    Melon<CameraTools>.Logger.Warning($"Camera paths: moving it aside failed: {moveFailed.Message}");
                }
            }
        }

        // Written beside the file and moved over it, so a crash mid-write leaves the last good save.
        private static void Save()
        {
            try
            {
                Directory.CreateDirectory(Folder);
                string temporary = FilePath + ".tmp";
                File.WriteAllText(temporary, JsonSerializer.Serialize(Write(), Json));
                File.Move(temporary, FilePath, true);
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Camera paths: saving {FilePath} failed: {e.Message}");
            }
        }

        private static LibraryFile Write() => new()
        {
            Paths = Paths.Select(path => new PathFile
            {
                Duration = path.Duration,
                Nodes = path.Nodes.Select(node => new NodeFile
                {
                    Position = new[] { node.Position.X, node.Position.Y, node.Position.Z },
                    Rotation = new[] { node.Rotation.X, node.Rotation.Y, node.Rotation.Z, node.Rotation.W },
                    Fov = node.Fov,
                }).ToList(),
            }).ToList(),
            ActivePath = Active,
            ActiveNode = Node,
            Options = Options,
        };

        // The file is outside input: any shape it should not have fails the whole load, before anything is replaced.
        private static void Read(LibraryFile file)
        {
            if (file?.Paths == null)
                throw new InvalidDataException("no Paths list");
            var paths = file.Paths.Select(ReadPath).ToList();
            var options = file.Options ?? new PathOptions();
            options.MoveShakeFrequency = Math.Clamp(options.MoveShakeFrequency, 0f, PathOptions.MaxShake);
            options.RotateShakeFrequency = Math.Clamp(options.RotateShakeFrequency, 0f, PathOptions.MaxShake);
            options.MoveShakeStrength = Math.Clamp(options.MoveShakeStrength, 0f, PathOptions.MaxShake);
            options.RotateShakeStrength = Math.Clamp(options.RotateShakeStrength, 0f, PathOptions.MaxShake);
            Paths.Clear();
            Paths.AddRange(paths);
            Options = options;
            Active = Paths.Count == 0 ? -1 : Math.Clamp(file.ActivePath, 0, Paths.Count - 1);
            Node = Active < 0 ? 0 : Math.Clamp(file.ActiveNode, 0, Math.Max(Paths[Active].Nodes.Count - 1, 0));
        }

        private static CameraPath ReadPath(PathFile file)
        {
            if (file?.Nodes == null)
                throw new InvalidDataException("a path has no Nodes list");
            var path = new CameraPath { Duration = Math.Clamp(Finite(file.Duration, "Duration"), CameraPath.MinDuration, CameraPath.MaxDuration) };
            foreach (var node in file.Nodes)
            {
                if (node?.Position is not { Length: 3 } p || node.Rotation is not { Length: 4 } r)
                    throw new InvalidDataException("a node needs a 3-number Position and a 4-number Rotation");
                var rotation = new NumericsQuaternion(Finite(r[0], "Rotation"), Finite(r[1], "Rotation"), Finite(r[2], "Rotation"), Finite(r[3], "Rotation"));
                if (rotation.Length() < 1e-3f)
                    throw new InvalidDataException("a node's Rotation is zero");
                var position = new NumericsVector3(Finite(p[0], "Position"), Finite(p[1], "Position"), Finite(p[2], "Position"));
                path.Nodes.Add(new CameraNode(position, NumericsQuaternion.Normalize(rotation),
                    Math.Clamp(Finite(node.Fov, "Fov"), PathSampler.MinFov, PathSampler.MaxFov)));
            }
            return path;
        }

        private static float Finite(float value, string name)
            => float.IsFinite(value) ? value : throw new InvalidDataException($"{name} is not a finite number");

        private sealed class LibraryFile
        {
            public List<PathFile> Paths { get; set; }
            public int ActivePath { get; set; }
            public int ActiveNode { get; set; }
            public PathOptions Options { get; set; }
        }

        private sealed class PathFile
        {
            public float Duration { get; set; }
            public List<NodeFile> Nodes { get; set; }
        }

        private sealed class NodeFile
        {
            public float[] Position { get; set; }
            public float[] Rotation { get; set; }
            public float Fov { get; set; }
        }
    }

    // Unity's structs and System.Numerics' share a layout; the fields are copied, since Unity's constructors are IL2CPP
    // calls.
    internal static class NumericsConversions
    {
        public static Vector3 ToUnity(this NumericsVector3 v) => new() { x = v.X, y = v.Y, z = v.Z };

        public static Quaternion ToUnity(this NumericsQuaternion q) => new() { x = q.X, y = q.Y, z = q.Z, w = q.W };

        public static NumericsVector3 ToNumerics(this Vector3 v) => new(v.x, v.y, v.z);

        public static NumericsQuaternion ToNumerics(this Quaternion q) => new(q.x, q.y, q.z, q.w);
    }
}
