using MelonLoader;
using MelonLoader.Utils;
using UnityEngine;
using NumericsQuaternion = System.Numerics.Quaternion;
using NumericsVector3 = System.Numerics.Vector3;

namespace CameraTools
{
    // The camera path library: the paths, which path and node the Paths tab points at, and the playback options, saved to
    // UserData/CameraTools/CameraPaths.json after every edit.
    internal static class CameraPaths
    {
        private static readonly string Folder = Path.Combine(MelonEnvironment.UserDataDirectory, "CameraTools");
        private static readonly string FilePath = Path.Combine(Folder, "CameraPaths.json");

        public static List<CameraPath> Paths { get; } = new();
        public static PathOptions Options { get; private set; } = new();

        // -1 only while there are no paths.
        public static int Active { get; private set; } = -1;

        // 0 while the active path has no nodes.
        public static int Node { get; private set; }

        public static CameraPath Current => Active >= 0 ? Paths[Active] : null;

        // Rises with every save, which follows every change.
        public static int Revision { get; private set; }

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
                return $"{Active + 1} of {Paths.Count} · {count} node{(count == 1 ? "" : "s")} · {path.Duration:0.0} s";
            }
        }

        // The path's name, or "Path 2" for the second path without one.
        public static string Label(int index) => index >= 0 && index < Paths.Count && !string.IsNullOrWhiteSpace(Paths[index].Name)
            ? Paths[index].Name : $"Path {index + 1}";

        public static string CurrentLabel => HasPath ? Label(Active) : "None";

        // An empty name gives the path its number back.
        public static void SetName(string name)
        {
            if (Current is not { } path)
                return;
            string before = Label(Active);
            path.Name = string.IsNullOrWhiteSpace(name) ? null : name.Trim();
            Save();
            CameraUi.Toast(path.Name == null ? $"{before} is Path {Active + 1} again" : $"Path {Active + 1} is now \"{path.Name}\"");
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
            CameraUi.Toast($"{Label(Active)} created");
        }

        public static void DeletePath()
        {
            if (!HasPath)
                return;
            string label = Label(Active);
            Paths.RemoveAt(Active);
            Active = Math.Min(Active, Paths.Count - 1);
            Node = 0;
            Save();
            CameraUi.Toast($"{label} deleted");
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
            CameraUi.Toast($"Node {Node + 1} added to {Label(Active)}");
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

        // A pinned node keeps its time.
        public static void ReplaceNode()
        {
            if (!HasNode || !Capture(out var node))
                return;
            Current.Nodes[Node] = node with { Time = Current.Nodes[Node].Time };
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
            path.SetDuration(seconds);
            Save();
        }

        // A new path for a replay's camera nodes, as long as the replay and selected in the Paths tab.
        public static CameraPath NewPath(string name, float duration)
        {
            var path = new CameraPath { Name = name, Duration = Math.Clamp(duration, CameraPath.MinDuration, CameraPath.MaxDuration) };
            Paths.Add(path);
            Active = Paths.Count - 1;
            Node = 0;
            Save();
            return path;
        }

        // Pins the free camera's view at time seconds into path, in place of the node at replacing if there is one, and
        // selects it. Returns its index, or null with the free camera off.
        public static int? PlaceView(CameraPath path, float time, int? replacing)
        {
            if (!Capture(out var node))
                return null;
            if (replacing is int old)
                path.Nodes.RemoveAt(old);
            int index = path.Place(node, time, Options.ConstantSpeed);
            if (path == Current)
                Node = index;
            Save();
            return index;
        }

        public static void RemoveNode(CameraPath path, int index)
        {
            path.Nodes.RemoveAt(index);
            if (path == Current)
                Node = Math.Clamp(index, 0, Math.Max(path.Nodes.Count - 1, 0));
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
                var library = PathFile.Read(File.ReadAllText(FilePath), out bool gaveIds);
                Paths.Clear();
                Paths.AddRange(library.Paths);
                Options = library.Options;
                Active = library.Active;
                Node = library.Node;
                if (gaveIds)
                    Save();
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
            Revision++;
            try
            {
                Directory.CreateDirectory(Folder);
                string temporary = FilePath + ".tmp";
                File.WriteAllText(temporary, PathFile.Write(new PathLibrary(Paths, Active, Node, Options)));
                File.Move(temporary, FilePath, true);
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Camera paths: saving {FilePath} failed: {e.Message}");
            }
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
