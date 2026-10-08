using Il2CppInterop.Runtime;
using MelonLoader;
using miHoYoEmotion;
using UnityEngine;
using VerletEngine;
using NumericsQuaternion = System.Numerics.Quaternion;
using NumericsVector3 = System.Numerics.Vector3;

namespace CameraTools
{
    // The parts of the active character that posing reads and writes, found once when posing starts, with the rotations
    // they had when the character froze. Every write is absolute, so writing twice a frame, or every frame, gives the same
    // picture. Restore puts back everything the rig changed.
    internal sealed class CharacterRig
    {
        // Pose joints has a marker for each body joint and up to this many strands.
        public const int MaxStrands = 48;

        private const float BlendTime = 0.2f;
        private const float LookTime = 0.3f;
        private const float HeadYawLimit = 60f;
        private const float HeadPitchLimit = 30f;
        // The game's own eye range, from EyeCtrl's eyeRotationRangeY and eyeRotationRangeX on Skirk.
        private const float EyeYawLimit = 17f;
        private const float EyePitchLimit = 6.5f;
        // A bit of EyeCtrl's auto-blink reason mask, and of EyeKey's enable reason mask, that is CameraTools' own.
        private const int BlinkReason = 31;
        private const int EyeKeyReason = 31;
        private const float Radians = MathF.PI / 180f;

        private static readonly string[] EyeBones = { "+EyeBone L A01", "+EyeBone R A01" };
        private static readonly NumericsVector3 Right = NumericsVector3.UnitX;
        private static readonly NumericsVector3 Up = NumericsVector3.UnitY;

        public readonly Transform Avatar;
        public readonly string[] Emotions;
        // "Game's", then each expression as the Expression row shows it: Angry_01 is "Angry 1".
        public readonly string[] ExpressionNames;
        // The Joint row's entries: every body joint, found on this character or not, then the strands it has.
        public readonly JointInfo[] Targets;

        // The inverse of the character's rotation at the freeze, which turns the scene's directions into the character's.
        private readonly NumericsQuaternion toCharacter;
        private readonly Dictionary<string, Transform> bones = new();
        private readonly List<(Animator Animator, bool Paused)> animators = new();
        private readonly Bone[] joints = new Bone[Joints.All.Length];
        // Each hand's fingers, thumb first; a hand without the bones to find its palm is null, and so is a finger missing a joint.
        private readonly FingerRig[][] hands = new FingerRig[2][];
        private readonly Bone[] eyes = new Bone[2];
        private readonly Dictionary<string, ShapeSlot> shapes = new();
        private readonly List<ShapeSlot> mouthShapes = new();
        private readonly List<ShapeSlot> eyeShapes = new();
        private readonly List<ShapeSlot> browShapes = new();
        private readonly List<(DynamicBoneArray Physics, bool Enabled, DynamicBoneArray.UpdateMode Mode)> hair = new();
        private readonly Strand[] strands;
        // Targets' bones, null for a joint this character lacks, and their own axes.
        private readonly Bone[] targetBones;
        private readonly JointFrame[] frames;
        private readonly EmoSync emoSync;
        private readonly EyeCtrl eyeCtrl;
        private readonly EyeKey eyeKey;

        // What the rig last told the game's face and eye systems, so each changes once when the pose does.
        private string expression;
        private bool blinking = true;
        private FacePose planned;
        private List<ShapeWrite> plan = new();

        // Finds everything before freezing, so a failure leaves the character as it was.
        public CharacterRig(Transform avatar)
        {
            Avatar = avatar;
            toCharacter = NumericsQuaternion.Inverse(avatar.rotation.ToNumerics());
            Collect(avatar);
            for (int i = 0; i < joints.Length; i++)
                joints[i] = BoneOf(Joints.All[i].Bone);
            foreach (var side in Sides.Both)
                hands[(int)side] = BuildHand(side);
            for (int i = 0; i < eyes.Length; i++)
                eyes[i] = BoneOf(EyeBones[i]);
            FindShapes();
            emoSync = Find<EmoSync>();
            eyeCtrl = Find<EyeCtrl>();
            eyeKey = Find<EyeKey>();
            var data = emoSync ? emoSync.setData : null;
            Emotions = (data ? data.emotionSet?.ToArray() : null) ?? Array.Empty<string>();
            ExpressionNames = Emotions.Select(Label).Prepend("Game's").ToArray();
            var physics = avatar.GetComponentsInChildren(Il2CppType.Of<DynamicBoneArray>(), true);
            for (int i = 0; i < physics.Length; i++)
                if (physics[i].TryCast<DynamicBoneArray>() is { } each && each)
                    hair.Add((each, each.enabled, each.m_UpdateMode));
            strands = FindStrands();
            Targets = Joints.All.Concat(strands.Select(strand => strand.Info)).ToArray();
            targetBones = joints.Concat(strands.Select(strand => strand.Bone)).ToArray();
            var guessed = new List<string>();
            frames = Targets.Select((info, i) => JointFrame.Along(Length(info, targetBones[i]?.Transform, guessed))).ToArray();
            var found = avatar.GetComponentsInChildren(Il2CppType.Of<Animator>(), true);
            for (int i = 0; i < found.Length; i++)
                if (found[i].TryCast<Animator>() is { } animator && animator)
                    animators.Add((animator, animator.isAnimationPaused));
            foreach (var (animator, _) in animators)
                animator.isAnimationPaused = true;
            // On a frozen character the hair physics only holds the hair's frozen shape (build 155), so the strands are
            // CameraTools' to place.
            foreach (var (each, _, _) in hair)
                each.enabled = false;
            TakeFace();
            // EyeKey's LateUpdate returns at once while any bit of its enable reasons is set. Left running on the frozen
            // character it moved the eyes after CameraTools' late update write, and on frames without a canvases callback
            // the eyes followed EyeKey and popped out.
            if (eyeKey)
                eyeKey.SetReasonEnable(false, EyeKeyReason);
            var missing = Joints.All.Where((_, i) => joints[i] == null).Select(info => info.Bone).ToArray();
            Melon<CameraTools>.Logger.Msg($"Posing {avatar.name}: {joints.Length - missing.Length} of {joints.Length} joints"
                + (missing.Length > 0 ? $" (no {string.Join(", ", missing)})" : "")
                + $", hands {(hands[0] != null ? "left" : "no left")} and {(hands[1] != null ? "right" : "no right")}, {(HasEyes ? "eye bones" : "no eye bones")},"
                + $" {shapes.Count} face shapes, {Emotions.Length} expressions, {hair.Count} hair physics, {animators.Count} animators.");
            Melon<CameraTools>.Logger.Msg($"Posing {avatar.name}: {strands.Length} strands, {strands.Count(strand => strand.Hair)} of them hair"
                + (strands.Length > 0 ? $": {string.Join(", ", strands.Select(strand => $"{strand.Info.Name} ({strand.Info.Bone})"))}." : "."));
            Melon<CameraTools>.Logger.Msg($"Posing {avatar.name}: "
                + (guessed.Count == 0 ? "every joint's own axes run to its child bone." : $"no child bone for the own axes of {string.Join(", ", guessed)}."));
        }

        public bool Has(PoseTarget target) => target.Joint is { } joint ? joints[(int)joint] != null : strands.Any(strand => strand.Info.Target == target);

        public bool HasHairStrands => strands.Any(strand => strand.Hair);

        public bool HasHand(Side side) => hands[(int)side] != null;

        // The own axes of a position in Targets, as they were at the freeze.
        public JointFrame Frame(int target) => frames[target];

        public bool HasShape(string name) => shapes.ContainsKey(name);

        public bool HasBrows(string pair) => HasShape(FaceChoices.Brow(pair, Side.Left)) || HasShape(FaceChoices.Brow(pair, Side.Right));

        public bool HasEyes => eyes.Any(eye => eye != null);

        // index is a position in Targets.
        public ScreenPoint? Point(int index, Camera camera)
            => index < targetBones.Length && targetBones[index] is { } bone && ScreenPoint.Of(camera, bone.Transform.position) is { OnScreen: true } point
                ? point : null;

        // The body in the late update, parents before children, so the head's aim reads its posed neck and the strands read
        // the posed bones they hang from.
        public void WriteBody(PoseSetup pose, Transform camera)
        {
            for (int i = 0; i < joints.Length; i++)
            {
                if (joints[i] is not { } bone)
                    continue;
                var joint = (PoseJoint)i;
                bone.Write(joint == PoseJoint.Head && pose.Gaze.HeadAtCamera && camera
                    ? Aim(bone, camera, HeadYawLimit, HeadPitchLimit)
                    : Rotation(i, pose.Turn(joint), pose.Axes));
            }
            foreach (var side in Sides.Both)
                WriteHand(hands[(int)side], pose.Hand(side));
            WriteStrands(pose);
        }

        // Ahead holds the eyes as they froze, By hand turns them from there, and At the camera aims each one at the camera
        // within the game's eye range. The game's eye controller pointed at the camera did nothing on the Deck once posing
        // ran, while these writes showed in build 148. EyeKey moves the eyes after the late update, so this also runs in
        // Canvas.willRenderCanvases.
        public void WriteEyes(GazePose gaze, Transform camera)
        {
            foreach (var eye in eyes)
            {
                if (eye == null)
                    continue;
                eye.Write(gaze.Eyes switch
                {
                    EyesLook.ByHand => AngleAxis(Up, -gaze.X) * AngleAxis(Right, -gaze.Y),
                    EyesLook.Camera when camera => Aim(eye, camera, EyeYawLimit, EyePitchLimit),
                    _ => NumericsQuaternion.Identity,
                });
            }
        }

        // The game's face system rewrites the shapes after the late update, so this runs again in Canvas.willRenderCanvases.
        public void WriteFace(FacePose face)
        {
            if (!ReferenceEquals(face, planned))
            {
                var next = Plan(face);
                foreach (var write in plan)
                    if (!next.Exists(each => each.Slot == write.Slot))
                        write.Slot.Release();
                plan = next;
                planned = face;
            }
            foreach (var write in plan)
                write.Slot.Write(write);
        }

        // Expressions and blinking change once when the pose asks for something else. Blinking stops through the auto-blink
        // reasons: EyeCtrl.ToggleBlink pauses a track of the face animation, and on the Deck that also kept expressions
        // from playing.
        public void Apply(FacePose face)
        {
            if (face.Expression != expression && emoSync)
                emoSync.SetEmotion(face.Expression ?? emoSync.defaultEmotion, BlendTime);
            expression = face.Expression;
            if (face.Blink != blinking && eyeCtrl)
                eyeCtrl.EnableAutoBlokingByReason(face.Blink, BlinkReason);
            blinking = face.Blink;
        }

        // A copy of the pose without what this character lacks, and how many parts that left out.
        public PoseSetup Fit(PoseSetup pose, out int skipped)
        {
            var fit = pose.Clone();
            int dropped = 0;
            foreach (var joint in fit.Joints.Keys.ToList())
            {
                if (Has(joint))
                    continue;
                fit.Joints.Remove(joint);
                dropped++;
            }
            foreach (var bone in fit.Strands.Keys.ToList())
            {
                if (Has(PoseTarget.OfStrand(bone)))
                    continue;
                fit.Strands.Remove(bone);
                dropped++;
            }
            foreach (var side in Sides.Both)
            {
                if (fit.Hand(side).Fingers == null || HasHand(side))
                    continue;
                fit.SetHand(side, HandPose.Game);
                dropped++;
            }
            string Keep(string name, Func<string, bool> has)
            {
                if (name == null || has(name))
                    return name;
                dropped++;
                return null;
            }
            var face = fit.Face with
            {
                Expression = Keep(fit.Face.Expression, Emotions.Contains),
                Mouth = Keep(fit.Face.Mouth, HasShape),
                Eyes = Keep(fit.Face.Eyes, HasShape),
                Brows = Keep(fit.Face.Brows, HasBrows),
            };
            if (face.LeftClosed > 0f && !HasShape(FaceChoices.Wink('A', Side.Left)))
            {
                face = face with { LeftClosed = 0f };
                dropped++;
            }
            if (face.RightClosed > 0f && !HasShape(FaceChoices.Wink('A', Side.Right)))
            {
                face = face with { RightClosed = 0f };
                dropped++;
            }
            fit.Face = face;
            if (fit.Gaze.Eyes == EyesLook.ByHand && !HasEyes)
            {
                fit.Gaze = fit.Gaze with { Eyes = EyesLook.Camera };
                dropped++;
            }
            if (fit.Gaze.HeadAtCamera && !Has(PoseJoint.Head))
            {
                fit.Gaze = fit.Gaze with { HeadAtCamera = false };
                dropped++;
            }
            skipped = dropped;
            return fit;
        }

        // Each part separately, so one that fails does not keep the others from going back.
        public void Restore()
        {
            Step("bones", () =>
            {
                foreach (var bone in AllBones())
                    if (bone.Transform)
                        bone.Write(NumericsQuaternion.Identity);
            });
            Step("face", () =>
            {
                foreach (var write in plan)
                    write.Slot.Release();
                plan.Clear();
                planned = null;
            });
            Step("expression", () =>
            {
                if (expression != null && emoSync)
                    emoSync.SetEmotion(emoSync.defaultEmotion, BlendTime);
                expression = null;
            });
            Step("blinking", () =>
            {
                if (!blinking && eyeCtrl)
                    eyeCtrl.EnableAutoBlokingByReason(true, BlinkReason);
                blinking = true;
            });
            Step("eye controller", () =>
            {
                if (eyeKey)
                    eyeKey.SetReasonEnable(true, EyeKeyReason);
            });
            Step("hair", () =>
            {
                foreach (var (physics, enabled, mode) in hair)
                {
                    if (!physics)
                        continue;
                    physics.m_UpdateMode = mode;
                    physics.enabled = enabled;
                }
            });
            Step("animators", () =>
            {
                foreach (var (animator, paused) in animators)
                    if (animator)
                        animator.isAnimationPaused = paused;
            });
        }

        private static void Step(string what, Action action)
        {
            try
            {
                action();
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Posing: putting back the {what} failed: {e}");
            }
        }

        private IEnumerable<Bone> AllBones()
            => joints.Concat(hands.Where(hand => hand != null).SelectMany(hand => hand).Where(finger => finger != null).SelectMany(finger => finger.Joints))
                .Concat(eyes)
                .Concat(strands.Select(strand => strand.Bone))
                .Where(bone => bone != null);

        // Bones are looked up by name, and the first of a name in the tree wins.
        private void Collect(Transform node)
        {
            bones.TryAdd(node.name, node);
            for (int i = 0; i < node.childCount; i++)
                Collect(node.GetChild(i));
        }

        // Each hair physics component simulates the strands that start at the bones in its root list. A strand is hair when
        // it hangs from the head. Root bones are kept by name, the first of a name winning, since poses keep strands by name.
        private Strand[] FindStrands()
        {
            var roots = new Dictionary<string, Transform>();
            foreach (var (physics, _, _) in hair)
            {
                var list = physics.m_RootList;
                for (int i = 0; list != null && i < list.Length; i++)
                    if (list[i] is { } root && root)
                        roots.TryAdd(root.name, root);
            }
            if (roots.Count > MaxStrands)
                CameraTools.LogOnce($"Posing {Avatar.name}: {roots.Count} strands; the Joint row has the first {MaxStrands}.");
            var head = joints[(int)PoseJoint.Head]?.Transform;
            bool UnderHead(Transform bone)
            {
                for (var node = bone.parent; head && node; node = node.parent)
                    if (node.Pointer == head.Pointer)
                        return true;
                return false;
            }
            return StrandJoints.Of(roots.Keys).Take(MaxStrands)
                .Select(info => new Strand(info, new Bone(roots[info.Bone], toCharacter), UnderHead(roots[info.Bone])))
                .ToArray();
        }

        private Bone BoneOf(string name) => bones.TryGetValue(name, out var transform) ? new Bone(transform, toCharacter) : null;

        private T Find<T>() where T : Component => Avatar.GetComponentInChildren(Il2CppType.Of<T>(), true)?.TryCast<T>();

        // Shapes are found by name across all the character's meshes; Face, Face_Eye and Brow hold the mouth, the eyes and
        // the brows. The Default shapes rest at 100 under every expression and are never touched.
        private void FindShapes()
        {
            var found = Avatar.GetComponentsInChildren(Il2CppType.Of<SkinnedMeshRenderer>(), true);
            for (int i = 0; i < found.Length; i++)
            {
                var renderer = found[i].TryCast<SkinnedMeshRenderer>();
                var mesh = renderer ? renderer.sharedMesh : null;
                int count = mesh ? mesh.blendShapeCount : 0;
                for (int index = 0; index < count; index++)
                {
                    string name = mesh.GetBlendShapeName(index);
                    if (shapes.ContainsKey(name))
                        continue;
                    var slot = new ShapeSlot(renderer, index);
                    shapes[name] = slot;
                    var group = name.EndsWith("_Default") ? null
                        : name.StartsWith("Mouth_") ? mouthShapes
                        : name.StartsWith("Eye_") ? eyeShapes
                        : name.StartsWith("Brow_") ? browShapes
                        : null;
                    group?.Add(slot);
                }
            }
        }

        // An override sets its shapes and zeroes the rest of its group, so the expression's own shape there does not add to
        // it. A closed eye sets its WinkA and lowers the lids the game rests on WinkB and WinkC in step, so they are gone at
        // 100.
        private List<ShapeWrite> Plan(FacePose face)
        {
            var targets = new Dictionary<ShapeSlot, ShapeWrite>();
            void Only(List<ShapeSlot> group, params string[] chosen)
            {
                foreach (var slot in group)
                    targets[slot] = new ShapeWrite(slot, chosen.Any(name => shapes.GetValueOrDefault(name) == slot) ? 100f : 0f, false);
            }
            if (face.Mouth != null && HasShape(face.Mouth))
                Only(mouthShapes, face.Mouth);
            if (face.Eyes != null && HasShape(face.Eyes))
                Only(eyeShapes, face.Eyes);
            if (face.Brows != null && HasBrows(face.Brows))
                Only(browShapes, FaceChoices.Brow(face.Brows, Side.Left), FaceChoices.Brow(face.Brows, Side.Right));
            foreach (var side in Sides.Both)
            {
                float closed = face.Closed(side);
                if (closed <= 0f || !shapes.TryGetValue(FaceChoices.Wink('A', side), out var wink))
                    continue;
                targets[wink] = new ShapeWrite(wink, closed, false);
                float open = 1f - closed / 100f;
                foreach (char kind in "BC")
                {
                    if (!shapes.TryGetValue(FaceChoices.Wink(kind, side), out var lid))
                        continue;
                    targets[lid] = targets.TryGetValue(lid, out var set) && !set.Scale ? set with { Value = set.Value * open } : new ShapeWrite(lid, open, true);
                }
            }
            return targets.Values.ToList();
        }

        // The game's expressions showed on the Deck only after Toggle(true, true), which clears CameraTools' own reason bit
        // and switches the face system on. The log shows the reason masks it found, which the game's abilities can set.
        private void TakeFace()
        {
            if (!emoSync)
                return;
            Melon<CameraTools>.Logger.Msg($"Posing: the face system's reasons were off 0x{emoSync._reasonToggle:X}, default face 0x{emoSync._reasonKeepDefault:X}.");
            emoSync.Toggle(true, true);
        }

        // target is a position in Targets. The rotation comes out in the character's frame whichever axes the numbers use.
        private NumericsQuaternion Rotation(int target, JointTurn turn, JointAxes axes)
            => JointFrame.For(axes, frames[target]).Compose(Targets[target], turn);

        // Where a target's bone pointed at the freeze, in the character's frame, toward its child bone. A bone this character
        // lacks points its Otherwise, and so does one without the child, whose name goes in guessed.
        private NumericsVector3 Length(JointInfo info, Transform bone, List<string> guessed)
        {
            var child = bone == null ? null
                : info.Toward == null ? (bone.childCount > 0 ? bone.GetChild(0) : null)
                : info.Toward.Select(name => bones.GetValueOrDefault(name)).FirstOrDefault(found => found != null);
            var length = child == null ? NumericsVector3.Zero
                : NumericsVector3.Transform(child.position.ToNumerics() - bone.position.ToNumerics(), toCharacter);
            if (length.LengthSquared() > 1e-8f)
                return length;
            if (bone != null)
                guessed.Add(info.Name);
            return info.Otherwise;
        }

        // A bone turns toward the camera from its frozen rotation, measured in the frame its posed parents carry it in, up
        // to the limits. It assumes the bone faced the character's forward when the character froze.
        private NumericsQuaternion Aim(Bone bone, Transform camera, float yawLimit, float pitchLimit)
        {
            var aim = camera.position.ToNumerics() - bone.Transform.position.ToNumerics();
            if (aim.LengthSquared() < 1e-6f)
                return NumericsQuaternion.Identity;
            var carried = bone.Transform.parent.rotation.ToNumerics() * NumericsQuaternion.Inverse(bone.ParentWorld);
            var look = NumericsVector3.Transform(NumericsVector3.Normalize(aim), toCharacter * NumericsQuaternion.Inverse(carried));
            float yaw = Math.Clamp(Degrees(MathF.Atan2(look.X, look.Z)), -yawLimit, yawLimit);
            float pitch = Math.Clamp(Degrees(MathF.Asin(Math.Clamp(look.Y, -1f, 1f))), -pitchLimit, pitchLimit);
            return AngleAxis(Up, yaw) * AngleAxis(Right, -pitch);
        }

        // Below 100, Hair follows head takes back part of the turn a hair strand's posed parents carry it by; at 0 the strand
        // keeps the world rotation it froze with. The strand's own turn comes first, so a strand turned by hand keeps that
        // turn from where it hangs.
        private void WriteStrands(PoseSetup pose)
        {
            float keep = 1f - pose.HairFollow / 100f;
            for (int s = 0; s < strands.Length; s++)
            {
                var strand = strands[s];
                var turn = Rotation(joints.Length + s, pose.Turn(strand.Info.Target), pose.Axes);
                if (strand.Hair && keep > 0f)
                {
                    var carried = strand.Bone.Transform.parent.rotation.ToNumerics() * NumericsQuaternion.Inverse(strand.Bone.ParentWorld);
                    var undo = NumericsQuaternion.Slerp(NumericsQuaternion.Identity, NumericsQuaternion.Inverse(carried), keep);
                    turn = toCharacter * undo * NumericsQuaternion.Inverse(toCharacter) * turn;
                }
                strand.Bone.Write(turn);
            }
        }

        // Bends are measured from straight, so a preset looks the same whatever the game's idle hand was: each joint turns by
        // the difference between the bend it should have and the bend it froze with. On the first joint, spread, then
        // across and twist, then the bend, each about the axes the turns before it carried. A hand left to the game keeps
        // its frozen fingers.
        private static void WriteHand(FingerRig[] fingers, HandPose hand)
        {
            if (fingers == null)
                return;
            for (int f = 0; f < fingers.Length; f++)
            {
                if (fingers[f] is not { } finger)
                    continue;
                var pose = hand.Fingers == null ? (FingerPose?)null : hand.Finger(f);
                for (int k = 0; k < finger.Joints.Length; k++)
                {
                    if (pose is not { } set)
                    {
                        finger.Joints[k].Write(NumericsQuaternion.Identity);
                        continue;
                    }
                    var turn = AngleAxis(finger.CurlAxis, Fingers.FullCurl[f][k] * set.Bend(k) / 100f - finger.Bent[k]);
                    if (k == 0)
                        turn = AngleAxis(finger.Palm, set.Spread * finger.SpreadSign) * AngleAxis(finger.AcrossAxis, set.Across / 100f * Fingers.FullAcross)
                            * AngleAxis(finger.TwistAxis, set.Twist) * turn;
                    finger.Joints[k].Write(turn);
                }
            }
        }

        // The palm faces the side the thumb tip is on. It needs the thumb, and the index and little fingers to find the line
        // across the knuckles.
        private FingerRig[] BuildHand(Side side)
        {
            var hand = joints[(int)(side == Side.Left ? PoseJoint.LeftHand : PoseJoint.RightHand)];
            var chains = Enumerable.Range(0, Fingers.Count)
                .Select(f => Enumerable.Range(0, 3).Select(k => bones.GetValueOrDefault(Fingers.Bone(side, f, k))).ToArray())
                .Select(chain => chain.All(bone => bone != null) ? chain : null)
                .ToArray();
            if (hand == null || chains[0] == null || chains[1] == null || chains[4] == null)
                return null;
            var wrist = hand.Transform.position.ToNumerics();
            var middle = (chains[2] ?? chains[1])[0].position.ToNumerics();
            var along = NumericsVector3.Normalize(middle - wrist);
            var across = NumericsVector3.Normalize(chains[4][0].position.ToNumerics() - chains[1][0].position.ToNumerics());
            var palm = NumericsVector3.Normalize(NumericsVector3.Cross(along, across));
            if (NumericsVector3.Dot(Tip(chains[0]) - wrist, palm) < 0f)
                palm = -palm;
            var index = chains[1][0].position.ToNumerics();
            return chains.Select((chain, f) => chain == null ? null : BuildFinger(f, chain, along, across, palm, index)).ToArray();
        }

        // The thumb curls about the axis that takes its tip toward the palm, the other fingers about the line across the
        // knuckles; either way a positive turn closes the finger. The first joint's bend is its angle out of the palm's
        // plane, each other joint's its angle from the joint before.
        private FingerRig BuildFinger(int f, Transform[] chain, NumericsVector3 along, NumericsVector3 across, NumericsVector3 palm, NumericsVector3 index)
        {
            var points = chain.Select(bone => bone.position.ToNumerics()).Append(Tip(chain)).ToArray();
            var segments = Enumerable.Range(0, 3).Select(k => NumericsVector3.Normalize(points[k + 1] - points[k])).ToArray();
            var axis = f == Fingers.Thumb ? NumericsVector3.Cross(segments[0], palm) : across;
            if (axis.LengthSquared() < 1e-6f)
                return null;
            axis = NumericsVector3.Normalize(axis);
            if (NumericsVector3.Dot(NumericsVector3.Cross(axis, segments[0]), palm) < 0f)
                axis = -axis;
            var bent = new[]
            {
                Degrees(MathF.Asin(Math.Clamp(NumericsVector3.Dot(segments[0], palm), -1f, 1f))),
                SignedAngle(segments[0], segments[1], axis),
                SignedAngle(segments[1], segments[2], axis),
            };
            // Spread moves a finger away from the thumb, and the thumb away from the fingers.
            var away = f == Fingers.Thumb ? -across : across;
            float spreadSign = NumericsVector3.Dot(NumericsVector3.Cross(palm, segments[0]), away) >= 0f ? 1f : -1f;
            // Across swings the thumb tip toward the palm's side about the line from the wrist to the middle knuckle. Twist
            // spins the thumb along its first segment, turning its pad, which faces where a bend moves the tip, toward the
            // index knuckle. The other fingers never use either.
            var swing = NumericsVector3.Zero;
            var spin = NumericsVector3.Zero;
            if (f == Fingers.Thumb)
            {
                swing = NumericsVector3.Dot(NumericsVector3.Cross(along, points[3] - points[0]), palm) >= 0f ? along : -along;
                var pad = NumericsVector3.Cross(axis, segments[0]);
                spin = NumericsVector3.Dot(NumericsVector3.Cross(segments[0], pad), index - points[0]) >= 0f ? segments[0] : -segments[0];
            }
            NumericsVector3 InCharacter(NumericsVector3 direction) => NumericsVector3.Transform(direction, toCharacter);
            return new FingerRig(chain.Select(bone => new Bone(bone, toCharacter)).ToArray(), InCharacter(axis), InCharacter(palm), spreadSign,
                InCharacter(swing), InCharacter(spin), bent);
        }

        // The last joint's child, or, without one, a segment as long as the one before and pointing the same way in the
        // joint's own frame, since a finger's joints share their axes.
        private static NumericsVector3 Tip(Transform[] chain)
        {
            var last = chain[^1];
            if (last.childCount > 0)
                return last.GetChild(0).position.ToNumerics();
            var before = chain[^2];
            var segment = last.position.ToNumerics() - before.position.ToNumerics();
            var local = NumericsVector3.Transform(segment, NumericsQuaternion.Inverse(before.rotation.ToNumerics()));
            return last.position.ToNumerics() + NumericsVector3.Transform(local, last.rotation.ToNumerics());
        }

        // From a to b about the axis, positive the way a positive turn about the axis goes.
        private static float SignedAngle(NumericsVector3 a, NumericsVector3 b, NumericsVector3 axis)
        {
            var from = a - axis * NumericsVector3.Dot(a, axis);
            var to = b - axis * NumericsVector3.Dot(b, axis);
            return Degrees(MathF.Atan2(NumericsVector3.Dot(NumericsVector3.Cross(from, to), axis), NumericsVector3.Dot(from, to)));
        }

        private static NumericsQuaternion AngleAxis(NumericsVector3 axis, float degrees) => NumericsQuaternion.CreateFromAxisAngle(axis, degrees * Radians);

        private static float Degrees(float radians) => radians / Radians;

        private static string Label(string emotion)
        {
            int at = emotion.IndexOf("_0", StringComparison.Ordinal);
            return at < 0 ? emotion : $"{emotion[..at]} {emotion[(at + 2)..]}";
        }

        // A bone with the rotations it froze with: Relative is its rotation in the character's frame.
        private sealed class Bone
        {
            public readonly Transform Transform;
            public readonly NumericsQuaternion ParentWorld;
            private readonly NumericsQuaternion local;
            private readonly NumericsQuaternion relative;
            private readonly NumericsQuaternion inverseRelative;

            public Bone(Transform transform, NumericsQuaternion toCharacter)
            {
                Transform = transform;
                local = transform.localRotation.ToNumerics();
                var world = transform.rotation.ToNumerics();
                ParentWorld = world * NumericsQuaternion.Inverse(local);
                relative = toCharacter * world;
                inverseRelative = NumericsQuaternion.Inverse(relative);
            }

            // turn is in the character's frame and applies after the frozen rotation. Moved into the bone's own frame, it
            // rides on whatever its parents do, so children follow their parent.
            public void Write(NumericsQuaternion turn)
                => Transform.localRotation = NumericsQuaternion.Normalize(local * inverseRelative * turn * relative).ToUnity();
        }

        private sealed record Strand(JointInfo Info, Bone Bone, bool Hair);

        // The axes are in the character's frame, and the thumb's alone has AcrossAxis and TwistAxis; Bent is each joint's bend
        // at the freeze, in degrees.
        private sealed record FingerRig(Bone[] Joints, NumericsVector3 CurlAxis, NumericsVector3 Palm, float SpreadSign, NumericsVector3 AcrossAxis,
            NumericsVector3 TwistAxis, float[] Bent);

        // Scale multiplies the game's own weight; otherwise Value is the weight.
        private readonly record struct ShapeWrite(ShapeSlot Slot, float Value, bool Scale);

        // A blend shape on one of the character's meshes. Baseline is the weight the game last gave it, put back when the
        // pose lets the shape go; Written is what posing last wrote, so a weight that differs from it was the game's.
        private sealed class ShapeSlot
        {
            private readonly SkinnedMeshRenderer renderer;
            private readonly int index;
            private float baseline;
            private float? written;

            public ShapeSlot(SkinnedMeshRenderer renderer, int index)
            {
                this.renderer = renderer;
                this.index = index;
            }

            public void Write(ShapeWrite write)
            {
                if (!renderer)
                    return;
                float now = renderer.GetBlendShapeWeight(index);
                if (written is not float last || Math.Abs(now - last) > 0.01f)
                    baseline = now;
                float value = write.Scale ? baseline * write.Value : write.Value;
                renderer.SetBlendShapeWeight(index, value);
                written = value;
            }

            public void Release()
            {
                if (written == null || !renderer)
                    return;
                renderer.SetBlendShapeWeight(index, baseline);
                written = null;
            }
        }
    }
}
