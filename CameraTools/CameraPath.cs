using System.Text.Json;
using System.Text.Json.Serialization;
using NumericsQuaternion = System.Numerics.Quaternion;
using NumericsVector3 = System.Numerics.Vector3;

namespace CameraTools
{
    // The nodes stay in the order the camera reaches them: a pinned node goes in where its time belongs.
    public sealed class CameraPath
    {
        public const float MinDuration = 1f;
        // Long enough for the longest replay, 5 minutes, starting 10 s into its path.
        public const float MaxDuration = 310f;
        public const float DefaultDuration = 10f;

        // Replays link to a path by this, so the link survives paths being added, deleted or reordered.
        public string Id { get; init; } = NewId();
        // A name the user typed, shown in place of "Path 2"; null for the number.
        public string Name { get; set; }
        public List<CameraNode> Nodes { get; } = new();
        public float Duration { get; set; } = DefaultDuration;

        public static string NewId() => Guid.NewGuid().ToString("N")[..8];

        // The pinned nodes' times scale with the duration, so the path keeps its shape in time.
        public void SetDuration(float seconds)
        {
            seconds = Math.Clamp(seconds, MinDuration, MaxDuration);
            float scale = seconds / Duration;
            for (int index = 0; index < Nodes.Count; index++)
                if (Nodes[index].Time is float time)
                    Nodes[index] = Nodes[index] with { Time = time * scale };
            Duration = seconds;
        }

        // When the camera reaches each node, in seconds from the start, without easing: a path with a pinned node has none.
        public float[] Moments(bool constantSpeed)
        {
            if (Nodes.Count == 0)
                return Array.Empty<float>();
            var sampler = new PathSampler(Nodes, constantSpeed, Duration);
            return Enumerable.Range(0, Nodes.Count).Select(index => sampler.NodeProgress(index) * Duration).ToArray();
        }

        // Puts a node pinned at time in after every node the camera reaches by then, making the path long enough to reach
        // it, and returns its index.
        public int Place(CameraNode node, float time, bool constantSpeed)
        {
            time = Math.Clamp(time, 0f, MaxDuration);
            if (time > Duration)
                Duration = time;
            int index = Moments(constantSpeed).Count(moment => moment <= time);
            Nodes.Insert(index, node with { Time = time });
            return index;
        }
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

    // Everything CameraPaths.json holds. Active is -1 only while there are no paths; Node is 0 while the active path has
    // no nodes.
    public sealed record PathLibrary(List<CameraPath> Paths, int Active, int Node, PathOptions Options);

    // CameraPaths.json's text. A node's Time is left out when it has none, and a file without times loads.
    public static class PathFile
    {
        private static readonly JsonSerializerOptions Json = new()
        {
            WriteIndented = true,
            NumberHandling = JsonNumberHandling.AllowNamedFloatingPointLiterals,
        };

        public static string Write(PathLibrary library) => JsonSerializer.Serialize(new LibraryFile
        {
            Paths = library.Paths.Select(path => new PathEntry
            {
                Id = path.Id,
                Name = path.Name,
                Duration = path.Duration,
                Nodes = path.Nodes.Select(node => new NodeEntry
                {
                    Position = new[] { node.Position.X, node.Position.Y, node.Position.Z },
                    Rotation = new[] { node.Rotation.X, node.Rotation.Y, node.Rotation.Z, node.Rotation.W },
                    Fov = node.Fov,
                    Time = node.Time,
                }).ToList(),
            }).ToList(),
            ActivePath = library.Active,
            ActiveNode = library.Node,
            Options = library.Options,
        }, Json);

        // The file is outside input: any shape it should not have fails the whole read with InvalidDataException.
        // gaveIds is whether a path had no Id and was given one, which the file should then keep.
        public static PathLibrary Read(string text, out bool gaveIds)
        {
            var file = JsonSerializer.Deserialize<LibraryFile>(text, Json);
            if (file?.Paths == null)
                throw new InvalidDataException("no Paths list");
            var paths = file.Paths.Select(ReadPath).ToList();
            var options = file.Options ?? new PathOptions();
            options.MoveShakeFrequency = Math.Clamp(options.MoveShakeFrequency, 0f, PathOptions.MaxShake);
            options.RotateShakeFrequency = Math.Clamp(options.RotateShakeFrequency, 0f, PathOptions.MaxShake);
            options.MoveShakeStrength = Math.Clamp(options.MoveShakeStrength, 0f, PathOptions.MaxShake);
            options.RotateShakeStrength = Math.Clamp(options.RotateShakeStrength, 0f, PathOptions.MaxShake);
            int active = paths.Count == 0 ? -1 : Math.Clamp(file.ActivePath, 0, paths.Count - 1);
            int node = active < 0 ? 0 : Math.Clamp(file.ActiveNode, 0, Math.Max(paths[active].Nodes.Count - 1, 0));
            gaveIds = file.Paths.Any(path => string.IsNullOrEmpty(path.Id));
            return new PathLibrary(paths, active, node, options);
        }

        private static CameraPath ReadPath(PathEntry file)
        {
            if (file?.Nodes == null)
                throw new InvalidDataException("a path has no Nodes list");
            var path = new CameraPath { Id = string.IsNullOrEmpty(file.Id) ? CameraPath.NewId() : file.Id,
                Name = string.IsNullOrWhiteSpace(file.Name) ? null : file.Name, Duration = Math.Clamp(Finite(file.Duration, "Duration"), CameraPath.MinDuration, CameraPath.MaxDuration) };
            foreach (var node in file.Nodes)
            {
                if (node?.Position is not { Length: 3 } p || node.Rotation is not { Length: 4 } r)
                    throw new InvalidDataException("a node needs a 3-number Position and a 4-number Rotation");
                var rotation = new NumericsQuaternion(Finite(r[0], "Rotation"), Finite(r[1], "Rotation"), Finite(r[2], "Rotation"), Finite(r[3], "Rotation"));
                if (rotation.Length() < 1e-3f)
                    throw new InvalidDataException("a node's Rotation is zero");
                var position = new NumericsVector3(Finite(p[0], "Position"), Finite(p[1], "Position"), Finite(p[2], "Position"));
                float? time = node.Time is float t ? Math.Clamp(Finite(t, "Time"), 0f, path.Duration) : null;
                path.Nodes.Add(new CameraNode(position, NumericsQuaternion.Normalize(rotation),
                    Math.Clamp(Finite(node.Fov, "Fov"), PathSampler.MinFov, PathSampler.MaxFov), time));
            }
            return path;
        }

        private static float Finite(float value, string name)
            => float.IsFinite(value) ? value : throw new InvalidDataException($"{name} is not a finite number");

        private sealed class LibraryFile
        {
            public List<PathEntry> Paths { get; set; }
            public int ActivePath { get; set; }
            public int ActiveNode { get; set; }
            public PathOptions Options { get; set; }
        }

        private sealed class PathEntry
        {
            public string Id { get; set; }
            public string Name { get; set; }
            public float Duration { get; set; }
            public List<NodeEntry> Nodes { get; set; }
        }

        private sealed class NodeEntry
        {
            public float[] Position { get; set; }
            public float[] Rotation { get; set; }
            public float Fov { get; set; }
            [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
            public float? Time { get; set; }
        }
    }
}
