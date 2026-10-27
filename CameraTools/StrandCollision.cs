using Il2CppInterop.Runtime;
using UnityEngine;
using VerletEngine;
using NumericsQuaternion = System.Numerics.Quaternion;
using NumericsVector3 = System.Numerics.Vector3;

namespace CameraTools
{
    // Keeps posed hair out of the arms and hands, and hair and cloth above the ground, by turning their bones after the pose
    // is written. The game's hair physics collides with nothing, and its meshes cannot be read back (Deck build 344), so
    // only bones are tested: a chain's bones and its last bone's reach. A chain is every bone below a bone outside the body
    // that hangs from a body bone and that a shown mesh is skinned to, so it covers hair and cloth no physics moves, whatever
    // the character's bones are named. Every chain bone the rig does not write goes back to its frozen rotation first each
    // frame, so the turns never pile up.
    internal sealed class StrandCollision
    {
        // Capsule radii round the arms and hands, in meters, before the gap.
        private const float UpperArmRadius = 0.04f;
        private const float ForearmRadius = 0.032f;
        private const float PalmRadius = 0.025f;
        private const float KnuckleRadius = 0.014f;
        private const float FingerRadius = 0.008f;
        private const float ThumbRadius = 0.01f;
        // How far a last bone reaches when nothing in its chain says, along the axis most chain bones point their child.
        private const float LeafLength = 0.08f;
        private const int Passes = 3;
        // A segment whose closest point is this near its start is left to the bone above it.
        private const float StartShare = 0.05f;
        // Bones that count as body: the biped's, and helpers that shape the body rather than hang from it. Odette's whole
        // skirt hangs from +PelvisTwist CF A01, so a chain can start below a helper.
        private static readonly string[] Helpers = { "EyeBone", "ToothBone", "Twist", "Breast", "AO_", "Knee", "Elbow", "Shoulder", "Shape", "Pos_", "JigBuns" };

        private readonly Chain[] chains;
        private readonly (Transform From, Transform To, float Radius)[] arms;
        private readonly Transform ground;
        private HashSet<IntPtr> written = new();
        private bool turned;

        public StrandCollision(Transform avatar, IReadOnlyDictionary<string, Transform> bones, IEnumerable<DynamicBoneArray> physics)
        {
            ground = avatar;
            var head = bones.GetValueOrDefault("Bip001 Head");
            var radii = PhysicsRadii(physics);
            var found = new List<Chain>();
            FindChains(avatar, head, SkinnedBones(avatar), found);
            chains = found.ToArray();
            var leafAxis = LeafAxis(chains);
            foreach (var chain in chains)
                for (int b = 0; b < chain.Bones.Length; b++)
                {
                    var bone = chain.Bones[b];
                    bone.Radius = radii.GetValueOrDefault(bone.Transform.Pointer);
                    if (bone.Offsets.Length > 0)
                        continue;
                    // A last bone reaches as far as it hangs from its parent, the way its parent points it, as a finger tip does.
                    var parent = bone.Transform.parent;
                    bone.Offsets = new[]
                    {
                        b > 0 ? NumericsVector3.Transform(bone.Transform.position.ToNumerics() - parent.position.ToNumerics(), NumericsQuaternion.Inverse(parent.rotation.ToNumerics()))
                            : leafAxis * LeafLength,
                    };
                }
            arms = ArmBones().Select(arm => (bones.GetValueOrDefault(arm.From), bones.GetValueOrDefault(arm.To), arm.Radius))
                .Where(arm => arm.Item1 && arm.Item2)
                .ToArray();
        }

        public int Chains => chains.Length;

        public int HairChains => chains.Count(chain => chain.Hair);

        // The first bone of each hair chain on the head: Furina's +HairF and +HairS locks by her face, which no physics
        // moves, become strands in the Joint list.
        public IEnumerable<Transform> HairRoots => chains.Where(chain => chain.Hair).Select(chain => chain.Bones[0].Transform);

        // The bones the rig writes every frame, which the reset leaves alone.
        public void Written(IEnumerable<Transform> bones) => written = new HashSet<IntPtr>(bones.Select(bone => bone.Pointer));

        // After the rig writes the pose. Hair turns out of the arms and hands; hair and cloth below the ground, which sits
        // where the feet were when the character froze, tilt up along their heading.
        public void Apply(AvoidPose avoid)
        {
            if (turned)
                Reset();
            turned = false;
            if (!avoid.Hands && !avoid.Ground)
                return;
            var shapes = avoid.Hands
                ? arms.Where(arm => arm.From && arm.To).Select(arm => new Capsule(arm.From.position.ToNumerics(), arm.To.position.ToNumerics(), arm.Radius)).ToArray()
                : Array.Empty<Capsule>();
            float? floor = avoid.Ground ? ground.position.y + avoid.GroundGap / 100f : null;
            foreach (var chain in chains)
                foreach (var bone in chain.Bones)
                    Solve(bone, chain.Hair ? shapes : Array.Empty<Capsule>(), avoid.HandGap / 100f, floor);
        }

        public void Restore()
        {
            Reset();
            turned = false;
        }

        private void Reset()
        {
            foreach (var chain in chains)
                foreach (var bone in chain.Bones)
                    if (bone.Transform && !written.Contains(bone.Transform.Pointer))
                        bone.Transform.localRotation = bone.Frozen;
        }

        // Turns the bone about its own position until the segment to each child, or a last bone's reach, clears every
        // capsule and the floor, a few passes since clearing one can move it into another.
        private void Solve(StrandBone bone, Capsule[] shapes, float gap, float? floor)
        {
            var transform = bone.Transform;
            float radius = bone.Radius + gap;
            foreach (var offset in bone.Offsets)
            {
                for (int pass = 0; pass < Passes; pass++)
                {
                    bool moved = false;
                    foreach (var shape in shapes)
                    {
                        var p = transform.position.ToNumerics();
                        var q = Tip(bone, offset);
                        var (t, onSegment, onAxis) = Closest(p, q, shape.A, shape.B);
                        float need = shape.Radius + radius;
                        var away = onSegment - onAxis;
                        float distance = away.Length();
                        if (distance >= need || t < StartShare)
                            continue;
                        away = distance > 1e-5f ? away / distance : Perpendicular(q - p, shape.B - shape.A);
                        Turn(bone, onSegment - p, onSegment + away * (need - distance) - p);
                        moved = true;
                    }
                    if (floor is float height)
                    {
                        var p = transform.position.ToNumerics();
                        var q = Tip(bone, offset);
                        float length = (q - p).Length();
                        // Above the floor the segment keeps its heading and tilts up just enough; one that starts below it is
                        // its parent's to lift.
                        if (q.Y < height && p.Y > height && length > 1e-5f)
                        {
                            var along = (q - p) / length;
                            float up = Math.Clamp((height - p.Y) / length, -1f, 1f);
                            var flat = new NumericsVector3(along.X, 0f, along.Z);
                            flat = flat.LengthSquared() > 1e-10f ? NumericsVector3.Normalize(flat) : NumericsVector3.UnitX;
                            Turn(bone, along, flat * MathF.Sqrt(1f - up * up) + NumericsVector3.UnitY * up);
                            moved = true;
                        }
                    }
                    if (!moved)
                        break;
                }
            }
        }

        private void Turn(StrandBone bone, NumericsVector3 from, NumericsVector3 to)
        {
            var turn = FromTo(from, to);
            bone.Transform.rotation = NumericsQuaternion.Normalize(turn * bone.Transform.rotation.ToNumerics()).ToUnity();
            turned = true;
        }

        private static NumericsVector3 Tip(StrandBone bone, NumericsVector3 offset)
            => bone.Transform.position.ToNumerics() + NumericsVector3.Transform(offset, bone.Transform.rotation.ToNumerics());

        private static IEnumerable<(string From, string To, float Radius)> ArmBones()
        {
            foreach (var side in Sides.Both)
            {
                string bip = $"Bip001 {side.Letter()}";
                yield return ($"{bip} UpperArm", $"{bip} Forearm", UpperArmRadius);
                yield return ($"{bip} Forearm", $"{bip} Hand", ForearmRadius);
                yield return ($"{bip} Hand", Fingers.Bone(side, 2, 0), PalmRadius);
                yield return (Fingers.Bone(side, 1, 0), Fingers.Bone(side, 4, 0), KnuckleRadius);
                for (int f = 0; f < Fingers.Count; f++)
                {
                    float radius = f == Fingers.Thumb ? ThumbRadius : FingerRadius;
                    yield return (Fingers.Bone(side, f, 0), Fingers.Bone(side, f, 1), radius);
                    yield return (Fingers.Bone(side, f, 1), Fingers.Bone(side, f, 2), radius);
                    yield return (Fingers.Bone(side, f, 2), $"{bip} Finger{f}Nub", radius);
                }
            }
        }

        // The hair physics' own radius for each bone it simulates, by its share down its strand.
        private static Dictionary<IntPtr, float> PhysicsRadii(IEnumerable<DynamicBoneArray> physics)
        {
            var radii = new Dictionary<IntPtr, float>();
            foreach (var each in physics)
            {
                if (!each)
                    continue;
                float scale = Math.Abs(each.transform.lossyScale.x);
                var list = each.m_RootList;
                for (int i = 0; list != null && i < list.Length; i++)
                {
                    if (!list[i])
                        continue;
                    var bones = new List<StrandBone>();
                    Walk(list[i], 0, bones);
                    int deepest = Math.Max(1, bones.Max(bone => bone.Depth));
                    foreach (var bone in bones)
                        radii.TryAdd(bone.Transform.Pointer, each.m_Radius * scale * (each.m_RadiusDistrib?.Evaluate(bone.Depth / (float)deepest) ?? 1f));
                }
            }
            return radii;
        }

        // The bones of every shown skinned mesh; the glider's are left out while it is hidden.
        private static HashSet<IntPtr> SkinnedBones(Transform avatar)
        {
            var skinned = new HashSet<IntPtr>();
            var meshes = avatar.GetComponentsInChildren(Il2CppType.Of<SkinnedMeshRenderer>(), false);
            for (int i = 0; i < meshes.Length; i++)
            {
                var mesh = meshes[i].TryCast<SkinnedMeshRenderer>();
                var skin = mesh && mesh.enabled ? mesh.bones : null;
                for (int b = 0; skin != null && b < skin.Length; b++)
                    if (skin[b])
                        skinned.Add(skin[b].Pointer);
            }
            return skinned;
        }

        private static void FindChains(Transform node, Transform head, HashSet<IntPtr> skinned, List<Chain> into)
        {
            for (int i = 0; i < node.childCount; i++)
            {
                var child = node.GetChild(i);
                if (!Body(child) && Body(node))
                {
                    var bones = new List<StrandBone>();
                    Walk(child, 0, bones);
                    if (bones.Any(bone => skinned.Contains(bone.Transform.Pointer)))
                        into.Add(new Chain(bones.ToArray(), Under(child, head)));
                    continue;
                }
                FindChains(child, head, skinned, into);
            }
        }

        private static bool Body(Transform bone) => bone.name.StartsWith("Bip001") || Helpers.Any(bone.name.Contains);

        // Every bone of a chain, parents first.
        private static void Walk(Transform bone, int depth, List<StrandBone> into)
        {
            into.Add(new StrandBone(bone, depth));
            for (int i = 0; i < bone.childCount; i++)
                Walk(bone.GetChild(i), depth + 1, into);
        }

        private static bool Under(Transform bone, Transform head)
        {
            for (var node = bone.parent; head && node; node = node.parent)
                if (node.Pointer == head.Pointer)
                    return true;
            return false;
        }

        // The axis most chain bones point their child along, in their own frame: -X on Furina, +X on Odette.
        private static NumericsVector3 LeafAxis(Chain[] chains)
        {
            var votes = new Dictionary<NumericsVector3, int>();
            foreach (var bone in chains.SelectMany(chain => chain.Bones).Where(bone => bone.Offsets.Length > 0))
            {
                var d = bone.Offsets[0];
                var axis = Math.Abs(d.X) >= Math.Abs(d.Y) && Math.Abs(d.X) >= Math.Abs(d.Z) ? NumericsVector3.UnitX * Math.Sign(d.X)
                    : Math.Abs(d.Y) >= Math.Abs(d.Z) ? NumericsVector3.UnitY * Math.Sign(d.Y) : NumericsVector3.UnitZ * Math.Sign(d.Z);
                votes[axis] = votes.GetValueOrDefault(axis) + 1;
            }
            return votes.Count > 0 ? votes.MaxBy(vote => vote.Value).Key : NumericsVector3.UnitX;
        }

        // The closest points of segment p-q and segment a-b, with the share along p-q.
        private static (float T, NumericsVector3 OnSegment, NumericsVector3 OnAxis) Closest(NumericsVector3 p, NumericsVector3 q, NumericsVector3 a, NumericsVector3 b)
        {
            var d1 = q - p;
            var d2 = b - a;
            var r = p - a;
            float aa = NumericsVector3.Dot(d1, d1), ee = NumericsVector3.Dot(d2, d2), f = NumericsVector3.Dot(d2, r);
            float s, t;
            if (aa < 1e-10f && ee < 1e-10f)
                return (0f, p, a);
            if (aa < 1e-10f)
            {
                s = 0f;
                t = Math.Clamp(f / ee, 0f, 1f);
            }
            else
            {
                float c = NumericsVector3.Dot(d1, r);
                if (ee < 1e-10f)
                {
                    t = 0f;
                    s = Math.Clamp(-c / aa, 0f, 1f);
                }
                else
                {
                    float bb = NumericsVector3.Dot(d1, d2);
                    float denominator = aa * ee - bb * bb;
                    s = denominator > 1e-10f ? Math.Clamp((bb * f - c * ee) / denominator, 0f, 1f) : 0f;
                    t = (bb * s + f) / ee;
                    if (t < 0f)
                    {
                        t = 0f;
                        s = Math.Clamp(-c / aa, 0f, 1f);
                    }
                    else if (t > 1f)
                    {
                        t = 1f;
                        s = Math.Clamp((bb - c) / aa, 0f, 1f);
                    }
                }
            }
            return (s, p + d1 * s, a + d2 * t);
        }

        private static NumericsVector3 Perpendicular(NumericsVector3 segment, NumericsVector3 axis)
        {
            var side = NumericsVector3.Cross(axis.LengthSquared() > 1e-10f ? axis : NumericsVector3.UnitY, segment);
            if (side.LengthSquared() < 1e-10f)
                side = NumericsVector3.Cross(NumericsVector3.UnitX, segment);
            return NumericsVector3.Normalize(side);
        }

        private static NumericsQuaternion FromTo(NumericsVector3 from, NumericsVector3 to)
        {
            from = NumericsVector3.Normalize(from);
            to = NumericsVector3.Normalize(to);
            var axis = NumericsVector3.Cross(from, to);
            if (axis.LengthSquared() < 1e-12f)
                return NumericsQuaternion.Identity;
            return NumericsQuaternion.CreateFromAxisAngle(NumericsVector3.Normalize(axis), MathF.Acos(Math.Clamp(NumericsVector3.Dot(from, to), -1f, 1f)));
        }

        private readonly record struct Capsule(NumericsVector3 A, NumericsVector3 B, float Radius);

        private sealed record Chain(StrandBone[] Bones, bool Hair);

        // Offsets are where the bone's children sit in its own frame, which turning it never changes; a last bone gets one
        // reach once its whole chain is known.
        private sealed class StrandBone
        {
            public readonly Transform Transform;
            public readonly int Depth;
            public readonly Quaternion Frozen;
            public NumericsVector3[] Offsets;
            public float Radius;

            public StrandBone(Transform transform, int depth)
            {
                Transform = transform;
                Depth = depth;
                Frozen = transform.localRotation;
                var inverse = NumericsQuaternion.Inverse(transform.rotation.ToNumerics());
                Offsets = Enumerable.Range(0, transform.childCount)
                    .Select(i => NumericsVector3.Transform(transform.GetChild(i).position.ToNumerics() - transform.position.ToNumerics(), inverse))
                    .ToArray();
            }
        }
    }
}
