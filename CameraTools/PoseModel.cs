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

    // Bone is the biped bone's name, looked up anywhere under the character. BendSign makes a positive Bend swing the joint
    // forward: +1 for a bone that points up from its joint, which a positive turn about the character's right axis tips
    // forward, and -1 for one that hangs down, which the same turn swings back. A sign the Deck shows backwards is flipped
    // here.
    public sealed record JointInfo(PoseJoint Joint, string Name, string Bone, BodySide Side, PoseJoint? Mirror, float BendSign)
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
                if ((int)All[i].Joint != i)
                    throw new InvalidOperationException($"Joints.All row {i} ({All[i].Joint}) is out of PoseJoint order.");
            if (All.Length != Enum.GetValues<PoseJoint>().Length)
                throw new InvalidOperationException("Joints.All is missing a PoseJoint.");
        }

        public static JointInfo Of(PoseJoint joint) => All[(int)joint];
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

    // Curl in percent, from straight to closed, and Spread in degrees, positive away from the thumb. Whole numbers, so a
    // hand whose fingers are set back to a preset's numbers is that preset again.
    public readonly record struct FingerPose(float Curl, float Spread)
    {
        public const float MaxSpread = 30f;

        public FingerPose Clamped() => new(MathF.Round(Math.Clamp(Curl, 0f, 100f)), MathF.Round(Math.Clamp(Spread, -MaxSpread, MaxSpread)));
    }

    // Each hand's fingers, thumb first; each finger has three joints from the knuckle out.
    public static class Fingers
    {
        public const int Count = 5;
        public const int Selectable = 2 * Count;

        public static readonly string[] Names = { "thumb", "index", "middle", "ring", "little" };

        // How far each joint bends at 100% curl, in degrees.
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
        Custom,
    }

    public static class HandShapes
    {
        public static readonly string[] Names = { "Game's", "Relaxed", "Fist", "Open", "Peace", "Point", "Thumbs up", "Custom" };

        // Curl and spread for each finger, thumb first; a preset is just these numbers.
        private static readonly (HandShape Shape, FingerPose[] Fingers)[] Presets =
        {
            (HandShape.Relaxed, Hand((25, 0), (30, 0), (35, 0), (40, 0), (45, 0))),
            (HandShape.Fist, Hand((90, 0), (100, 0), (100, 0), (100, 0), (100, 0))),
            (HandShape.Open, Hand((0, 0), (0, -6), (0, 0), (0, 6), (0, 12))),
            (HandShape.Peace, Hand((90, 0), (0, -10), (0, 10), (100, 0), (100, 0))),
            (HandShape.Point, Hand((80, 0), (0, 0), (100, 0), (100, 0), (100, 0))),
            (HandShape.ThumbsUp, Hand((0, 0), (100, 0), (100, 0), (100, 0), (100, 0))),
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

        private static FingerPose[] Hand(params (float Curl, float Spread)[] fingers) => fingers.Select(each => new FingerPose(each.Curl, each.Spread)).ToArray();
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
            fingers[finger] = pose.Clamped();
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

    // One pose as the Pose tab edits it and Poses.json stores it: a turn for each joint that has one, on top of the frozen
    // game pose, each hand, the face, and the gaze.
    public sealed class PoseSetup
    {
        public Dictionary<PoseJoint, JointTurn> Joints { get; private init; } = new();
        public HandPose Left { get; set; } = HandPose.Game;
        public HandPose Right { get; set; } = HandPose.Game;
        public FacePose Face { get; set; } = FacePose.Default;
        public GazePose Gaze { get; set; } = GazePose.Default;

        public int PosedJoints => Joints.Count;

        public JointTurn Turn(PoseJoint joint) => Joints.GetValueOrDefault(joint);

        // A joint turned back to zero is dropped, so Joints holds only the posed ones.
        public void SetTurn(PoseJoint joint, JointTurn turn)
        {
            turn = turn.Clamped();
            if (turn.IsZero)
                Joints.Remove(joint);
            else
                Joints[joint] = turn;
        }

        public HandPose Hand(Side side) => side == Side.Left ? Left : Right;

        public void SetHand(Side side, HandPose hand)
        {
            if (side == Side.Left)
                Left = hand;
            else
                Right = hand;
        }

        // Hands, face and gaze are records that are replaced, never changed, so only the joints need copying.
        public PoseSetup Clone() => new() { Joints = new Dictionary<PoseJoint, JointTurn>(Joints), Left = Left, Right = Right, Face = Face, Gaze = Gaze };
    }

    public sealed record SavedPose(string Name, PoseSetup Setup);
}
