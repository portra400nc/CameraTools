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
        private const float BlendTime = 0.2f;
        private const float LookTime = 0.3f;
        private const float HeadYawLimit = 60f;
        private const float HeadPitchLimit = 30f;
        private const float Radians = MathF.PI / 180f;

        private static readonly string[] EyeBones = { "+EyeBone L A01", "+EyeBone R A01" };
        private static readonly NumericsVector3 Right = NumericsVector3.UnitX;
        private static readonly NumericsVector3 Up = NumericsVector3.UnitY;
        private static readonly NumericsVector3 Forward = NumericsVector3.UnitZ;

        public readonly Transform Avatar;
        public readonly string[] Emotions;
        // "Game's", then each expression as the Expression row shows it: Angry_01 is "Angry 1".
        public readonly string[] ExpressionNames;

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
        private readonly List<(DynamicBoneArray Physics, bool Enabled)> hair = new();
        private readonly EmoSync emoSync;
        private readonly EyeCtrl eyeCtrl;
        private readonly EyeKey eyeKey;

        // What the rig last told the game's face and eye systems, so each changes once when the pose does.
        private string expression;
        // The face system's own reasons for being off, and for holding the default face, as they were before posing.
        private (uint Off, uint KeepDefault)? faceReasons;
        private bool blinking = true;
        private (Transform Target, bool Enabled, EyeKey.EyeKeyController Controller)? savedLook;
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
                    hair.Add((each, each.enabled));
            var found = avatar.GetComponentsInChildren(Il2CppType.Of<Animator>(), true);
            for (int i = 0; i < found.Length; i++)
                if (found[i].TryCast<Animator>() is { } animator && animator)
                    animators.Add((animator, animator.isAnimationPaused));
            foreach (var (animator, _) in animators)
                animator.isAnimationPaused = true;
            TakeFace();
            var missing = Joints.All.Where(info => joints[(int)info.Joint] == null).Select(info => info.Bone).ToArray();
            Melon<CameraTools>.Logger.Msg($"Posing {avatar.name}: {joints.Length - missing.Length} of {joints.Length} joints"
                + (missing.Length > 0 ? $" (no {string.Join(", ", missing)})" : "")
                + $", hands {(hands[0] != null ? "left" : "no left")} and {(hands[1] != null ? "right" : "no right")}, {(HasEyes ? "eye bones" : "no eye bones")},"
                + $" {shapes.Count} face shapes, {Emotions.Length} expressions, {hair.Count} hair physics, {animators.Count} animators.");
        }

        public bool Has(PoseJoint joint) => joints[(int)joint] != null;

        public bool HasHand(Side side) => hands[(int)side] != null;

        public bool HasShape(string name) => shapes.ContainsKey(name);

        public bool HasBrows(string pair) => HasShape(FaceChoices.Brow(pair, Side.Left)) || HasShape(FaceChoices.Brow(pair, Side.Right));

        public bool HasEyes => eyes.Any(eye => eye != null);

        public ScreenPoint? Point(PoseJoint joint, Camera camera)
            => joints[(int)joint] is { } bone && ScreenPoint.Of(camera, bone.Transform.position) is { OnScreen: true } point ? point : null;

        // The body in the late update, parents before children, so the head's aim reads its posed neck.
        public void WriteBody(PoseSetup pose, Transform camera)
        {
            for (int i = 0; i < joints.Length; i++)
            {
                if (joints[i] is not { } bone)
                    continue;
                var joint = (PoseJoint)i;
                bone.Write(joint == PoseJoint.Head && pose.Gaze.HeadAtCamera && camera ? HeadAim(bone, camera) : Rotation(Joints.All[i], pose.Turn(joint)));
            }
            foreach (var side in Sides.Both)
                WriteHand(hands[(int)side], pose.Hand(side));
        }

        // Ahead holds the eyes as they froze and By hand turns them from there. At the camera leaves them to the game's eye
        // controller, which Apply points at the camera.
        public void WriteEyes(GazePose gaze)
        {
            if (gaze.Eyes == EyesLook.Camera)
                return;
            var turn = gaze.Eyes == EyesLook.ByHand ? AngleAxis(Up, -gaze.X) * AngleAxis(Right, -gaze.Y) : NumericsQuaternion.Identity;
            foreach (var eye in eyes)
                eye?.Write(turn);
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

        // Expressions, blinking and the game's eye aim change once when the pose asks for something else.
        public void Apply(FacePose face, GazePose gaze, Transform camera)
        {
            KeepFace();
            if (face.Expression != expression && emoSync)
            {
                emoSync.SetEmotion(face.Expression ?? emoSync.defaultEmotion, BlendTime);
            }
            expression = face.Expression;
            if (face.Blink != blinking && eyeCtrl)
                eyeCtrl.ToggleBlink(face.Blink);
            blinking = face.Blink;
            bool look = gaze.Eyes == EyesLook.Camera && camera && eyeCtrl;
            if (look && savedLook == null)
                LookAt(camera);
            else if (!look)
                StopLooking();
        }

        public void SetHair(bool on)
        {
            foreach (var (physics, _) in hair)
                if (physics)
                    physics.enabled = on;
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
            Step("face system", GiveBackFace);
            Step("blinking", () =>
            {
                if (!blinking && eyeCtrl)
                    eyeCtrl.ToggleBlink(true);
                blinking = true;
            });
            Step("eye aim", StopLooking);
            Step("hair", () =>
            {
                foreach (var (physics, enabled) in hair)
                    if (physics)
                        physics.enabled = enabled;
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
                .Where(bone => bone != null);

        // Bones are looked up by name, and the first of a name in the tree wins.
        private void Collect(Transform node)
        {
            bones.TryAdd(node.name, node);
            for (int i = 0; i < node.childCount; i++)
                Collect(node.GetChild(i));
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

        // The game keeps the face system off, or on its default face, while any bit of these masks is set; abilities set
        // them (EmoSyncBanMixin, EmoSyncKeepDefaultMixin), and then expressions and the eye aim do nothing. Posing clears
        // them, and Toggle(true, true) switches the face system on, as the Deck tests that showed expressions did.
        private void TakeFace()
        {
            if (!emoSync)
                return;
            faceReasons = (emoSync._reasonToggle, emoSync._reasonKeepDefault);
            Melon<CameraTools>.Logger.Msg($"Posing: the face system's reasons were off 0x{emoSync._reasonToggle:X}, default face 0x{emoSync._reasonKeepDefault:X}.");
            emoSync._reasonToggle = 0;
            emoSync._reasonKeepDefault = 0;
            emoSync.Toggle(true, true);
        }

        // A reason the game sets while posing is cleared too, and given back with the others when posing ends.
        private void KeepFace()
        {
            if (faceReasons is not { } saved || !emoSync)
                return;
            uint off = emoSync._reasonToggle, keepDefault = emoSync._reasonKeepDefault;
            if (off == 0 && keepDefault == 0)
                return;
            CameraTools.LogOnce($"Posing: the game set face system reasons while posing (off 0x{off:X}, default face 0x{keepDefault:X}); clearing them.");
            faceReasons = (saved.Off | off, saved.KeepDefault | keepDefault);
            emoSync._reasonToggle = 0;
            emoSync._reasonKeepDefault = 0;
            emoSync.Toggle(true, true);
            // Switching on resets the face, so the chosen expression is set again.
            expression = null;
        }

        // Bit 0 is Toggle's own reason, so toggling with it as it was recomputes the face system's state from the masks.
        private void GiveBackFace()
        {
            if (faceReasons is not { } saved || !emoSync)
                return;
            faceReasons = null;
            emoSync._reasonKeepDefault = saved.KeepDefault;
            emoSync._reasonToggle = saved.Off & ~1u;
            emoSync.Toggle((saved.Off & 1u) == 0, false);
        }

        private void LookAt(Transform camera)
        {
            savedLook = (eyeCtrl.viewTarget, eyeCtrl.targetEnabled, eyeKey ? eyeKey.currentController : default);
            if (eyeKey)
                eyeKey.currentController = EyeKey.EyeKeyController.LookAtEyeCtrl;
            eyeCtrl.viewTarget = camera;
            eyeCtrl.targetEnabled = true;
            eyeCtrl.ForceUpdateLookTarget(LookTime);
        }

        private void StopLooking()
        {
            if (savedLook is not { } saved)
                return;
            savedLook = null;
            if (eyeCtrl)
            {
                eyeCtrl.viewTarget = saved.Target;
                eyeCtrl.targetEnabled = saved.Enabled;
                eyeCtrl.ClearLookat();
            }
            if (eyeKey)
                eyeKey.currentController = saved.Controller;
        }

        // Offsets are in the character's frame: Turn about its up axis, Bend about its right axis, Twist about its forward
        // axis.
        private static NumericsQuaternion Rotation(JointInfo info, JointTurn turn)
        {
            float side = info.SideSign;
            return AngleAxis(Up, turn.Turn * side) * AngleAxis(Right, turn.Bend * info.BendSign) * AngleAxis(Forward, turn.Twist * side);
        }

        // The head turns toward the camera from its frozen rotation, measured in the frame its posed parents carry it in,
        // up to a turn a neck can make.
        private NumericsQuaternion HeadAim(Bone head, Transform camera)
        {
            var aim = camera.position.ToNumerics() - head.Transform.position.ToNumerics();
            if (aim.LengthSquared() < 1e-6f)
                return NumericsQuaternion.Identity;
            var carried = head.Transform.parent.rotation.ToNumerics() * NumericsQuaternion.Inverse(head.ParentWorld);
            var look = NumericsVector3.Transform(NumericsVector3.Normalize(aim), toCharacter * NumericsQuaternion.Inverse(carried));
            float yaw = Math.Clamp(Degrees(MathF.Atan2(look.X, look.Z)), -HeadYawLimit, HeadYawLimit);
            float pitch = Math.Clamp(Degrees(MathF.Asin(Math.Clamp(look.Y, -1f, 1f))), -HeadPitchLimit, HeadPitchLimit);
            return AngleAxis(Up, yaw) * AngleAxis(Right, -pitch);
        }

        // Curl is measured from straight, so a preset looks the same whatever the game's idle hand was: each joint turns by
        // the difference between the bend it should have and the bend it froze with. A hand left to the game keeps its
        // frozen fingers.
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
                    var turn = AngleAxis(finger.CurlAxis, Fingers.FullCurl[f][k] * set.Curl / 100f - finger.Bent[k]);
                    if (k == 0)
                        turn = AngleAxis(finger.Palm, set.Spread * finger.SpreadSign) * turn;
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
            return chains.Select((chain, f) => chain == null ? null : BuildFinger(f, chain, across, palm)).ToArray();
        }

        // The thumb curls about the axis that takes its tip toward the palm, the other fingers about the line across the
        // knuckles; either way a positive turn closes the finger. The first joint's bend is its angle out of the palm's
        // plane, each other joint's its angle from the joint before.
        private FingerRig BuildFinger(int f, Transform[] chain, NumericsVector3 across, NumericsVector3 palm)
        {
            var points = chain.Select(bone => bone.position.ToNumerics()).Append(Tip(chain)).ToArray();
            var segments = Enumerable.Range(0, 3).Select(k => NumericsVector3.Normalize(points[k + 1] - points[k])).ToArray();
            var axis = f == 0 ? NumericsVector3.Cross(segments[0], palm) : across;
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
            var away = f == 0 ? -across : across;
            float spreadSign = NumericsVector3.Dot(NumericsVector3.Cross(palm, segments[0]), away) >= 0f ? 1f : -1f;
            return new FingerRig(chain.Select(bone => new Bone(bone, toCharacter)).ToArray(), NumericsVector3.Transform(axis, toCharacter),
                NumericsVector3.Transform(palm, toCharacter), spreadSign, bent);
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

        // CurlAxis and Palm are in the character's frame; Bent is each joint's bend at the freeze, in degrees.
        private sealed record FingerRig(Bone[] Joints, NumericsVector3 CurlAxis, NumericsVector3 Palm, float SpreadSign, float[] Bent);

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
