using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;
using MelonLoader;
using MelonLoader.Utils;
using UnityEngine;

namespace CameraTools
{
    // The Pose tab's saved poses, kept in UserData/CameraTools/Poses.json and saved a second after the last change, and the
    // last pose, which ending posing leaves behind for Bring back last pose and which is never saved.
    internal static class Poses
    {
        private const float SaveDelay = 1f;

        private static readonly string Folder = Path.Combine(MelonEnvironment.UserDataDirectory, "CameraTools");
        private static readonly string FilePath = Path.Combine(Folder, "Poses.json");
        private static readonly Regex Numbered = new(@"^Pose (\d+)$");
        private static float? changedAt;

        public static List<SavedPose> All { get; } = new();

        // -1 only while there are no saved poses.
        public static int Active { get; private set; } = -1;

        public static SavedPose Current => Active >= 0 ? All[Active] : null;

        public static PoseSetup Last { get; set; }

        public static void Load()
        {
            if (!File.Exists(FilePath))
                return;
            try
            {
                var (poses, active) = PoseJson.Read(File.ReadAllText(FilePath));
                All.AddRange(poses);
                Active = All.Count == 0 ? -1 : Math.Clamp(active, 0, All.Count - 1);
                Melon<CameraTools>.Logger.Msg($"Poses: loaded {All.Count} pose{(All.Count == 1 ? "" : "s")} from {FilePath}.");
            }
            catch (Exception e)
            {
                All.Clear();
                Active = -1;
                string backup = FilePath + ".bak";
                Melon<CameraTools>.Logger.Warning($"Poses: {FilePath} could not be read ({e.Message}); moved it to {backup} and started empty.");
                try
                {
                    File.Move(FilePath, backup, true);
                }
                catch (Exception moveFailed)
                {
                    Melon<CameraTools>.Logger.Warning($"Poses: moving it aside failed: {moveFailed.Message}");
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

        // Named Pose 1, Pose 2 and on, after the highest number in use.
        public static string Add(PoseSetup setup)
        {
            int last = All.Select(pose => Numbered.Match(pose.Name)).Where(match => match.Success)
                .Select(match => int.TryParse(match.Groups[1].Value, out int number) ? number : 0).DefaultIfEmpty(0).Max();
            string name = $"Pose {last + 1}";
            All.Add(new SavedPose(name, setup));
            Active = All.Count - 1;
            Changed();
            return name;
        }

        public static string Overwrite(PoseSetup setup)
        {
            if (Current is not { } saved)
                return null;
            All[Active] = saved with { Setup = setup };
            Changed();
            return saved.Name;
        }

        public static string Delete()
        {
            if (Current is not { } saved)
                return null;
            All.RemoveAt(Active);
            Active = All.Count == 0 ? -1 : Math.Min(Active, All.Count - 1);
            Changed();
            return saved.Name;
        }

        public static void Update()
        {
            if (changedAt is not float at || Time.unscaledTime - at < SaveDelay)
                return;
            changedAt = null;
            Save();
        }

        private static void Changed() => changedAt = Time.unscaledTime;

        // Written beside the file and moved over it, so a crash mid-write leaves the last good save.
        private static void Save()
        {
            try
            {
                Directory.CreateDirectory(Folder);
                string temporary = FilePath + ".tmp";
                File.WriteAllText(temporary, PoseJson.Write(All, Active));
                File.Move(temporary, FilePath, true);
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Poses: saving {FilePath} failed: {e.Message}");
            }
        }
    }

    // Poses.json, apart from the game, so a check can round-trip it outside Genshin. Joints are kept by name and expressions
    // and blend shapes by their own names, so a pose made on one character loads on another, and a name this build does not
    // know is dropped instead of failing the file.
    public static class PoseJson
    {
        private static readonly JsonSerializerOptions Options = new()
        {
            WriteIndented = true,
            NumberHandling = JsonNumberHandling.AllowNamedFloatingPointLiterals,
            Converters = { new JsonStringEnumConverter() },
        };

        public static string Write(IEnumerable<SavedPose> poses, int active)
            => JsonSerializer.Serialize(new PosesFile { Poses = poses.Select(ToFile).ToList(), Active = active }, Options);

        public static (List<SavedPose> Poses, int Active) Read(string text)
        {
            var file = JsonSerializer.Deserialize<PosesFile>(text, Options);
            return (file.Poses.Where(pose => pose != null).Select(FromFile).ToList(), file.Active);
        }

        private static PoseFile ToFile(SavedPose saved)
        {
            var setup = saved.Setup;
            return new PoseFile
            {
                Name = saved.Name,
                Axes = setup.Axes,
                Joints = setup.Joints.ToDictionary(each => each.Key.ToString(), each => Numbers(each.Value)),
                Strands = setup.Strands.ToDictionary(each => each.Key, each => Numbers(each.Value)),
                Left = ToFile(setup.Left),
                Right = ToFile(setup.Right),
                Face = setup.Face,
                Gaze = setup.Gaze,
                HairFollow = setup.HairFollow,
            };
        }

        private static float[] Numbers(JointTurn turn) => new[] { turn.Bend, turn.Turn, turn.Twist };

        private static HandFile ToFile(HandPose hand)
            => new()
            {
                Shape = hand.Shape,
                Fingers = hand.Fingers?.Select(finger => new[] { finger.Base, finger.Middle, finger.Tip, finger.Spread, finger.Across, finger.Twist }).ToArray(),
            };

        private static SavedPose FromFile(PoseFile file)
        {
            var setup = new PoseSetup(file.Axes ?? JointAxes.Character);
            foreach (var (name, values) in file.Joints ?? new())
                if (Enum.TryParse<PoseJoint>(name, out var joint) && values is { Length: 3 })
                    setup.SetTurn(joint, new JointTurn(values[0], values[1], values[2]));
            foreach (var (bone, values) in file.Strands ?? new())
                if (!string.IsNullOrEmpty(bone) && values is { Length: 3 })
                    setup.SetTurn(PoseTarget.OfStrand(bone), new JointTurn(values[0], values[1], values[2]));
            setup.Left = FromFile(file.Left);
            setup.Right = FromFile(file.Right);
            if (file.Face is { } face)
                setup.Face = face with { LeftClosed = Math.Clamp(face.LeftClosed, 0f, 100f), RightClosed = Math.Clamp(face.RightClosed, 0f, 100f) };
            if (file.Gaze is { } gaze)
                setup.Gaze = gaze with { X = Math.Clamp(gaze.X, -GazePose.MaxX, GazePose.MaxX), Y = Math.Clamp(gaze.Y, -GazePose.MaxY, GazePose.MaxY) };
            if (file.HairFollow is { } follow && float.IsFinite(follow))
                setup.HairFollow = Math.Clamp(follow, 0f, 100f);
            return new SavedPose(file.Name ?? "Pose", setup);
        }

        // A hand without five readable fingers is left to the game. A preset's name only stands while its numbers still match,
        // since the presets' numbers changed when the joints got their own rows.
        private static HandPose FromFile(HandFile file)
        {
            var poses = file?.Fingers is { Length: Fingers.Count } fingers ? fingers.Select(FromFile).ToArray() : null;
            if (poses == null || poses.Any(pose => pose == null))
                return HandPose.Game;
            var clamped = poses.Select((pose, f) => pose.Value.Clamped(f)).ToArray();
            return new HandPose(file.Shape == HandShape.Custom ? HandShape.Custom : HandShapes.Match(clamped), clamped);
        }

        // A file from before the knuckle, middle joint and tip had their own rows has [curl, spread].
        private static FingerPose? FromFile(float[] finger) => finger switch
        {
            { Length: 6 } => new FingerPose(finger[0], finger[1], finger[2], finger[3], finger[4], finger[5]),
            { Length: 2 } => new FingerPose(finger[0], finger[0], finger[0], finger[1], 0f, 0f),
            _ => null,
        };

        private sealed class PosesFile
        {
            public List<PoseFile> Poses { get; set; } = new();
            public int Active { get; set; }
        }

        // Joints as [bend, turn, twist] by PoseJoint name, and strands by their root bone's name. A file from before strands
        // has neither Strands nor HairFollow, and loads with no strands posed and the hair following the head. One from before
        // Axes made its numbers in the character's axes.
        private sealed class PoseFile
        {
            public string Name { get; set; }
            public JointAxes? Axes { get; set; }
            public Dictionary<string, float[]> Joints { get; set; }
            public Dictionary<string, float[]> Strands { get; set; }
            public float? HairFollow { get; set; }
            public HandFile Left { get; set; }
            public HandFile Right { get; set; }
            public FacePose Face { get; set; }
            public GazePose Gaze { get; set; }
        }

        // Fingers thumb first as [base, middle, tip, spread, across, twist], or none for the game's hand.
        private sealed class HandFile
        {
            public HandShape Shape { get; set; }
            public float[][] Fingers { get; set; }
        }
    }
}
