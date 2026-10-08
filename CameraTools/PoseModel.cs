using System.Text.RegularExpressions;
namespace CameraTools
{
    // The joints the Pose tab turns, in the order the Joint row and the D-pad step through them.
    public enum PoseJoint
    {
        Hips,
        Waist,
        Chest,
        Neck,
        Head,
        LeftShoulder,
        LeftUpperArm,
        LeftForearm,
        LeftHand,
        RightShoulder,
        RightUpperArm,
        RightForearm,
        RightHand,
        LeftThigh,
        LeftKnee,
        LeftFoot,
        RightThigh,
        RightKnee,
        RightFoot,
    }

    public enum BodySide
    {
        Center,
        Left,
        Right,
    }

    // A hand, or an eye.
    public enum Side
    {
        Left,
        Right,
    }

    public static class Sides
    {
        public static readonly Side[] Both = { Side.Left, Side.Right };

        // As the biped's bone names and the face's blend shape names spell it.
        public static string Letter(this Side side) => side == Side.Left ? "L" : "R";
    }

    // What the Joint row and Pose joints turn: a body joint, or a hair or cloth strand by its root bone's name.
    public readonly record struct PoseTarget
    {
        private PoseTarget(PoseJoint? joint, string strand)
        {
            Joint = joint;
            Strand = strand;
        }

        public PoseJoint? Joint { get; }

        public string Strand { get; }

        public bool IsStrand => Strand != null;

        public static PoseTarget OfStrand(string bone) => new(null, bone);

        public static implicit operator PoseTarget(PoseJoint joint) => new(joint, null);
    }

    // One entry of the Joint row. Bone is the biped bone's name, looked up anywhere under the character, or a strand's root
    // bone. BendSign makes a positive Bend swing the joint forward: +1 for a bone that points up from its joint, which a
    // positive turn about the character's right axis tips forward, and -1 for one that hangs down, which the same turn
    // swings back. A sign the Deck shows backwards is flipped here.
    public sealed record JointInfo(PoseTarget Target, string Name, string Bone, BodySide Side, PoseTarget? Mirror, float BendSign)
    {
        // Turn and Twist go the other way on the right side, so equal values on both sides look like mirror images.
        public float SideSign => Side == BodySide.Right ? -1f : 1f;
    }

    public static class Joints
    {
        private const float Up = 1f;
        private const float Down = -1f;

        public static readonly JointInfo[] All =
        {
            new(PoseJoint.Hips, "Hips", "Bip001 Pelvis", BodySide.Center, null, Up),
            new(PoseJoint.Waist, "Waist", "Bip001 Spine", BodySide.Center, null, Up),
            new(PoseJoint.Chest, "Chest", "Bip001 Spine2", BodySide.Center, null, Up),
            new(PoseJoint.Neck, "Neck", "Bip001 Neck", BodySide.Center, null, Up),
            new(PoseJoint.Head, "Head", "Bip001 Head", BodySide.Center, null, Up),
            new(PoseJoint.LeftShoulder, "Left shoulder", "Bip001 L Clavicle", BodySide.Left, PoseJoint.RightShoulder, Down),
            new(PoseJoint.LeftUpperArm, "Left upper arm", "Bip001 L UpperArm", BodySide.Left, PoseJoint.RightUpperArm, Down),
            new(PoseJoint.LeftForearm, "Left forearm", "Bip001 L Forearm", BodySide.Left, PoseJoint.RightForearm, Down),
            new(PoseJoint.LeftHand, "Left hand", "Bip001 L Hand", BodySide.Left, PoseJoint.RightHand, Down),
            new(PoseJoint.RightShoulder, "Right shoulder", "Bip001 R Clavicle", BodySide.Right, PoseJoint.LeftShoulder, Down),
            new(PoseJoint.RightUpperArm, "Right upper arm", "Bip001 R UpperArm", BodySide.Right, PoseJoint.LeftUpperArm, Down),
            new(PoseJoint.RightForearm, "Right forearm", "Bip001 R Forearm", BodySide.Right, PoseJoint.LeftForearm, Down),
            new(PoseJoint.RightHand, "Right hand", "Bip001 R Hand", BodySide.Right, PoseJoint.LeftHand, Down),
            new(PoseJoint.LeftThigh, "Left thigh", "Bip001 L Thigh", BodySide.Left, PoseJoint.RightThigh, Down),
            new(PoseJoint.LeftKnee, "Left knee", "Bip001 L Calf", BodySide.Left, PoseJoint.RightKnee, Down),
            new(PoseJoint.LeftFoot, "Left foot", "Bip001 L Foot", BodySide.Left, PoseJoint.RightFoot, Down),
            new(PoseJoint.RightThigh, "Right thigh", "Bip001 R Thigh", BodySide.Right, PoseJoint.LeftThigh, Down),
            new(PoseJoint.RightKnee, "Right knee", "Bip001 R Calf", BodySide.Right, PoseJoint.LeftKnee, Down),
            new(PoseJoint.RightFoot, "Right foot", "Bip001 R Foot", BodySide.Right, PoseJoint.LeftFoot, Down),
        };

        static Joints()
        {
            for (int i = 0; i < All.Length; i++)
                if (All[i].Target.Joint != (PoseJoint)i)
                    throw new InvalidOperationException($"Joints.All row {i} ({All[i].Name}) is out of PoseJoint order.");
            if (All.Length != Enum.GetValues<PoseJoint>().Length)
                throw new InvalidOperationException("Joints.All is missing a PoseJoint.");
        }

        public static JointInfo Of(PoseJoint joint) => All[(int)joint];
    }

    // The Joint row's entries for a character's hair and cloth strands, from their root bones' names. Characters name these
    // bones in different ways ("+HairB L L02" on Skirk, "Bone_HairB01_L" on Columbina), so a name is read as words: the
    // first real word is the part, a B, F or S right after it (or a Back, Front or Side word anywhere) the position, and the
    // first L, R or M after the part the side, in any letter case. Both of those read "Back hair left". Strands hang, so
    // they bend like the arms and legs.
    public static class StrandJoints
    {
        private const float Hanging = -1f;

        // In the Joint row's order; any other part keeps its own word and comes last.
        private static readonly (string Part, string Word)[] Parts =
        {
            ("Hair", "hair"), ("Skirt", "skirt"), ("Amice", "cloth"), ("Overcoat", "coat"), ("Hem", "hem"),
        };
        private static readonly (string Token, string Word)[] Positions =
        {
            ("B", "Back"), ("F", "Front"), ("S", "Side"), ("Back", "Back"), ("Front", "Front"), ("Side", "Side"),
        };
        private static readonly (string Token, BodySide Side, string Word, string Twin)[] SideWords =
        {
            ("L", BodySide.Left, "left", "R"), ("R", BodySide.Right, "right", "L"), ("M", BodySide.Center, "middle", null),
            ("Left", BodySide.Left, "left", "Right"), ("Right", BodySide.Right, "right", "Left"),
        };
        // Words that name the kind of bone, not the part.
        private static readonly HashSet<string> Filler = new(StringComparer.OrdinalIgnoreCase) { "Bone", "Bip", "Dyn", "Root" };

        // A run of capitals not followed by a small letter, a capitalised word, or digits: "+HairB L L02" is Hair, B, L, L,
        // 02, and "Bone_HairB01_L" is Bone, Hair, B, 01, L.
        private static readonly Regex Words = new(@"[A-Z]+(?![a-z])|[A-Z]?[a-z]+|\d+", RegexOptions.CultureInvariant);

        private static bool Is(string token, string word) => string.Equals(token, word, StringComparison.OrdinalIgnoreCase);

        // Sorted by part, then by name, then by bone so names that repeat get " 2", " 3" the same way every time.
        public static JointInfo[] Of(IEnumerable<string> bones)
        {
            var known = bones.Distinct().ToHashSet();
            var read = known.Select(bone => (Bone: bone, Read: Read(bone)))
                .OrderBy(each => each.Read.Group).ThenBy(each => each.Read.Name, StringComparer.Ordinal).ThenBy(each => each.Bone, StringComparer.Ordinal)
                .ToList();
            var seen = new Dictionary<string, int>();
            return read.Select(each =>
            {
                int count = seen[each.Read.Name] = seen.GetValueOrDefault(each.Read.Name) + 1;
                string name = count == 1 ? each.Read.Name : $"{each.Read.Name} {count}";
                string mirror = each.Read.Mirror;
                return new JointInfo(PoseTarget.OfStrand(each.Bone), name, each.Bone, each.Read.Side,
                    mirror != null && known.Contains(mirror) ? PoseTarget.OfStrand(mirror) : null, Hanging);
            }).ToArray();
        }

        // Mirror is the same name with the side word swapped where it stands. A bone with no part word keeps its own name and
        // comes last.
        private static (int Group, string Name, BodySide Side, string Mirror) Read(string bone)
        {
            var words = Words.Matches(bone).ToList();
            int at = words.FindIndex(word => word.Value.Length > 1 && !char.IsDigit(word.Value[0]) && !Filler.Contains(word.Value)
                && !SideWords.Any(side => Is(side.Token, word.Value)) && !Positions.Any(position => Is(position.Token, word.Value)));
            if (at < 0)
                return (Parts.Length + 1, bone, BodySide.Center, null);
            string part = words[at].Value;
            int group = Array.FindIndex(Parts, each => Is(each.Part, part));
            string position = (at + 1 < words.Count ? Positions.FirstOrDefault(each => Is(each.Token, words[at + 1].Value)).Word : null)
                ?? Positions.Where(each => each.Token.Length > 1).FirstOrDefault(each => words.Any(word => Is(each.Token, word.Value))).Word;
            var sideWord = words.Skip(at + 1).FirstOrDefault(word => SideWords.Any(side => Is(side.Token, word.Value)));
            var side = sideWord == null ? default : SideWords.First(each => Is(each.Token, sideWord.Value));
            string twin = side.Twin == null ? null : sideWord.Value.ToLowerInvariant() == sideWord.Value ? side.Twin.ToLowerInvariant()
                : sideWord.Value.ToUpperInvariant() == sideWord.Value ? side.Twin.ToUpperInvariant() : side.Twin;
            string mirror = twin == null ? null : bone[..sideWord.Index] + twin + bone[(sideWord.Index + sideWord.Length)..];
            string name = string.Join(" ", new[] { position, group >= 0 ? Parts[group].Word : part.ToLowerInvariant(), side.Word }.Where(word => !string.IsNullOrEmpty(word)));
            return (group >= 0 ? group : Parts.Length, char.ToUpperInvariant(name[0]) + name[1..], side.Side, mirror);
        }
    }

    // Degrees on top of the frozen game pose, in the character's frame: Bend about its right axis, Turn about its up axis and
    // Twist about its forward axis.
    public readonly record struct JointTurn(float Bend, float Turn, float Twist)
    {
        public const float MaxBend = 180f;
        public const float MaxTurn = 90f;
        public const float MaxTwist = 180f;

        public bool IsZero => Bend == 0f && Turn == 0f && Twist == 0f;

        public JointTurn Clamped()
            => new(Math.Clamp(Bend, -MaxBend, MaxBend), Math.Clamp(Turn, -MaxTurn, MaxTurn), Math.Clamp(Twist, -MaxTwist, MaxTwist));
    }

    // Base, Middle and Tip bend a finger's three joints from straight, in percent of each joint's full bend; below 0 bends
    // it back. Spread is in degrees, positive away from the thumb. Across swings the thumb in front of the palm, in percent
    // of Fingers.FullAcross, and Twist turns its pad, in degrees; both stay 0 on the other fingers. Whole numbers, so a
    // hand whose fingers are set back to a preset's numbers is that preset again.
    public readonly record struct FingerPose(float Base, float Middle, float Tip, float Spread, float Across, float Twist)
    {
        public const float MinBend = -20f;
        public const float MaxBend = 100f;
        public const float MaxSpread = 30f;
        public const float MaxTwist = 60f;

        // The Curl row: the three joints' average, and all three set to one value.
        public float Curl => MathF.Round((Base + Middle + Tip) / 3f);

        public FingerPose WithCurl(float curl) => this with { Base = curl, Middle = curl, Tip = curl };

        // From the knuckle out.
        public float Bend(int joint) => joint switch { 0 => Base, 1 => Middle, _ => Tip };

        public FingerPose Clamped(int finger)
        {
            bool thumb = finger == Fingers.Thumb;
            return new(Whole(Base, MinBend, MaxBend), Whole(Middle, MinBend, MaxBend), Whole(Tip, MinBend, MaxBend), Whole(Spread, -MaxSpread, MaxSpread),
                thumb ? Whole(Across, 0f, 100f) : 0f, thumb ? Whole(Twist, -MaxTwist, MaxTwist) : 0f);
        }

        private static float Whole(float value, float min, float max) => MathF.Round(Math.Clamp(value, min, max));
    }

    // Each hand's fingers, thumb first; each finger has three joints from the knuckle out.
    public static class Fingers
    {
        public const int Count = 5;
        public const int Selectable = 2 * Count;
        public const int Thumb = 0;

        // How far the thumb swings at 100% Across, in degrees. A guess the Deck has to confirm.
        public const float FullAcross = 60f;

        public static readonly string[] Names = { "thumb", "index", "middle", "ring", "little" };

        // How far each joint bends at 100%, in degrees.
        public static readonly float[][] FullCurl =
        {
            new[] { 40f, 50f, 60f },
            new[] { 80f, 95f, 70f },
            new[] { 80f, 95f, 70f },
            new[] { 80f, 95f, 70f },
            new[] { 80f, 95f, 70f },
        };

        // Bip001 L Finger2, Finger21 and Finger22 for the left middle finger; the thumb is Finger0.
        public static string Bone(Side side, int finger, int joint) => $"Bip001 {side.Letter()} Finger{finger}{(joint == 0 ? "" : joint.ToString())}";

        // The Finger row's position: the left hand's five, then the right hand's.
        public static (Side Side, int Finger) At(int index) => (index < Count ? Side.Left : Side.Right, index % Count);
    }

    public enum HandShape
    {
        Game,
        Relaxed,
        Fist,
        Open,
        Peace,
        Point,
        ThumbsUp,
        OK,
        Pinch,
        Custom,
    }

    public static class HandShapes
    {
        public static readonly string[] Names = { "Game's", "Relaxed", "Fist", "Open", "Peace", "Point", "Thumbs up", "OK", "Pinch", "Custom" };

        private static readonly FingerPose Closed = F(90, 100, 90);

        // Each finger, thumb first; a preset is just these numbers.
        private static readonly (HandShape Shape, FingerPose[] Fingers)[] Presets =
        {
            (HandShape.Relaxed, new[] { F(20, 20, 20, 0, 20), F(25, 30, 30), F(30, 35, 35), F(35, 40, 40), F(40, 45, 45) }),
            (HandShape.Fist, new[] { F(40, 60, 60, 0, 60), Closed, Closed, Closed, Closed }),
            (HandShape.Open, new[] { F(0, 0, 0), F(0, 0, 0, -6), F(0, 0, 0), F(0, 0, 0, 6), F(0, 0, 0, 12) }),
            (HandShape.Peace, new[] { F(50, 60, 60, 0, 70), F(0, 0, 0, -10), F(0, 0, 0, 10), Closed, Closed }),
            (HandShape.Point, new[] { F(40, 60, 60, 0, 60), F(0, 0, 0), Closed, Closed, Closed }),
            (HandShape.ThumbsUp, new[] { F(0, 0, 0), Closed, Closed, Closed, Closed }),
            (HandShape.OK, new[] { F(30, 40, 40, 0, 80), F(55, 70, 45), F(10, 15, 10), F(10, 15, 10, 6), F(15, 20, 15, 12) }),
            (HandShape.Pinch, new[] { F(10, 30, 30, 0, 65), F(10, 60, 40, -8), F(10, 65, 45), F(20, 70, 50, 6), F(30, 75, 55, 12) }),
        };

        // A copy of a preset's fingers, or null for the game's hand and for Custom.
        public static FingerPose[] Preset(HandShape shape)
        {
            foreach (var (preset, fingers) in Presets)
                if (preset == shape)
                    return (FingerPose[])fingers.Clone();
            return null;
        }

        public static HandShape Match(FingerPose[] fingers)
        {
            foreach (var (preset, numbers) in Presets)
                if (numbers.SequenceEqual(fingers))
                    return preset;
            return HandShape.Custom;
        }

        private static FingerPose F(float knuckle, float middle, float tip, float spread = 0f, float across = 0f, float twist = 0f)
            => new(knuckle, middle, tip, spread, across, twist);
    }

    // Fingers is null while the hand is as the game posed it. It is never changed in place, so poses can share it.
    public sealed record HandPose(HandShape Shape, FingerPose[] Fingers)
    {
        public static readonly HandPose Game = new(HandShape.Game, null);

        // A hand left to the game shows as Relaxed until a finger is changed.
        public FingerPose Finger(int finger) => (Fingers ?? HandShapes.Preset(HandShape.Relaxed))[finger];

        // Custom keeps the fingers as they are, and starts a hand left to the game from Relaxed.
        public HandPose WithShape(HandShape shape)
        {
            if (shape != HandShape.Custom)
                return new HandPose(shape, HandShapes.Preset(shape));
            return Fingers == null ? new HandPose(HandShape.Relaxed, HandShapes.Preset(HandShape.Relaxed)) : this with { Shape = HandShape.Custom };
        }

        // The hand becomes Custom unless its numbers now match a preset.
        public HandPose WithFinger(int finger, FingerPose pose)
        {
            var fingers = (FingerPose[])(Fingers ?? HandShapes.Preset(HandShape.Relaxed)).Clone();
            fingers[finger] = pose.Clamped(finger);
            return new HandPose(HandShapes.Match(fingers), fingers);
        }
    }

    // Expression is one of the character's own, by name, or null for whatever the game shows. Mouth, Eyes and Brows are
    // blend shape names, or null to keep the expression's; Brows names a pair without its _L and _R. LeftClosed and
    // RightClosed close each eye from 0 to 100 on top of the rest.
    public sealed record FacePose(string Expression, string Mouth, string Eyes, string Brows, float LeftClosed, float RightClosed, bool Blink)
    {
        public static readonly FacePose Default = new(null, null, null, null, 0f, 0f, false);

        public float Closed(Side side) => side == Side.Left ? LeftClosed : RightClosed;
    }

    public enum EyesLook
    {
        Ahead,
        Camera,
        ByHand,
    }

    // X turns the eyes to the character's left and Y up, in degrees, while Eyes is ByHand.
    public sealed record GazePose(EyesLook Eyes, float X, float Y, bool HeadAtCamera)
    {
        // The game's own eye range, eyeRotationRangeY and eyeRotationRangeX on Skirk.
        public const float MaxX = 17f;
        public const float MaxY = 6.5f;

        public static readonly GazePose Default = new(EyesLook.Camera, 0f, 0f, false);
    }

    // One choice of a face row: the label it shows and the blend shape it sets, or null to keep the expression's.
    public sealed record FaceChoice(string Label, string Shape);

    public static class FaceChoices
    {
        public static readonly FaceChoice[] Mouths =
        {
            new("Expression's", null),
            new("Smile", "Mouth_Smile01"),
            new("Big smile", "Mouth_Smile02"),
            new("Open", "Mouth_Open01"),
            new("Ah", "Mouth_A01"),
            new("Angry", "Mouth_Angry01"),
            new("Smug", "Mouth_Doya01"),
            new("Cat", "Mouth_Neko01"),
            new("Tongue out", "Mouth_Pero01"),
            new("Flat", "Mouth_Line01"),
        };

        public static readonly FaceChoice[] Eyes =
        {
            new("Expression's", null),
            new("Wide open", "Eye_Ha"),
            new("Half-lidded", "Eye_Jito"),
            new("Teary", "Eye_Wail"),
            new("Glaring", "Eye_Hostility"),
            new("Tired", "Eye_Tired"),
        };

        // Each is a pair: Brow_Trouble is Brow_Trouble_L and Brow_Trouble_R.
        public static readonly FaceChoice[] Brows =
        {
            new("Expression's", null),
            new("Worried", "Brow_Trouble"),
            new("Happy", "Brow_Smily"),
            new("Angry", "Brow_Angry"),
            new("Shy", "Brow_Shy"),
            new("Raised", "Brow_Up"),
            new("Lowered", "Brow_Down"),
            new("Furrowed", "Brow_Squeeze"),
        };

        public static string[] Labels(FaceChoice[] choices) => choices.Select(choice => choice.Label).ToArray();

        public static int IndexOf(FaceChoice[] choices, string shape) => Array.FindIndex(choices, choice => choice.Shape == shape);

        // Closing an eye sets its WinkA shape; the game's face system rests the lids on WinkB and WinkC.
        public static string Wink(char kind, Side side) => $"Eye_Wink{kind}_{side.Letter()}";

        public static string Brow(string pair, Side side) => $"{pair}_{side.Letter()}";
    }

    // One pose as the Pose tab edits it and Poses.json stores it: a turn for each joint and strand that has one, on top of
    // the frozen game pose, each hand, the face, the gaze, and how much the hair follows the head, in percent.
    public sealed class PoseSetup
    {
        public Dictionary<PoseJoint, JointTurn> Joints { get; private init; } = new();
        // By the strand root bone's name.
        public Dictionary<string, JointTurn> Strands { get; private init; } = new();
        public HandPose Left { get; set; } = HandPose.Game;
        public HandPose Right { get; set; } = HandPose.Game;
        public FacePose Face { get; set; } = FacePose.Default;
        public GazePose Gaze { get; set; } = GazePose.Default;
        public float HairFollow { get; set; } = 100f;

        public int PosedJoints => Joints.Count + Strands.Count;

        public JointTurn Turn(PoseTarget target) => target.Joint is { } joint ? Joints.GetValueOrDefault(joint) : Strands.GetValueOrDefault(target.Strand);

        // A joint turned back to zero is dropped, so Joints and Strands hold only the posed ones.
        public void SetTurn(PoseTarget target, JointTurn turn)
        {
            if (target.Joint is { } joint)
                Set(Joints, joint, turn.Clamped());
            else
                Set(Strands, target.Strand, turn.Clamped());
        }

        public HandPose Hand(Side side) => side == Side.Left ? Left : Right;

        public void SetHand(Side side, HandPose hand)
        {
            if (side == Side.Left)
                Left = hand;
            else
                Right = hand;
        }

        // Hands, face and gaze are records that are replaced, never changed, so only the turns need copying.
        public PoseSetup Clone() => new()
        {
            Joints = new Dictionary<PoseJoint, JointTurn>(Joints),
            Strands = new Dictionary<string, JointTurn>(Strands),
            Left = Left,
            Right = Right,
            Face = Face,
            Gaze = Gaze,
            HairFollow = HairFollow,
        };

        private static void Set<TKey>(Dictionary<TKey, JointTurn> turns, TKey key, JointTurn turn)
        {
            if (turn.IsZero)
                turns.Remove(key);
            else
                turns[key] = turn;
        }
    }

    public sealed record SavedPose(string Name, PoseSetup Setup);
}
