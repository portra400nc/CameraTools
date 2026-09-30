using System.Numerics;

namespace CameraTools
{
    // Position is an absolute world position, so a node stays put when Genshin shifts its world origin. Fov is in degrees.
    public readonly record struct CameraNode(Vector3 Position, Quaternion Rotation, float Fov);

    // Plain math over System.Numerics, because Unity's structs call into the game and cannot run outside it. Built once
    // when playback starts; Sample maps progress from 0 to 1 onto the path.
    public sealed class PathSampler
    {
        public const float MinFov = 1f;
        public const float MaxFov = 160f;
        // At 32 the camera's speed still varied by 11% on uneven node spacing; at 128 it varies by under 3%.
        private const int SamplesPerSegment = 128;
        private const float Alpha = 0.5f;
        // Below this total length a path only turns in place, and each segment gets equal time.
        private const float StillLength = 1e-3f;

        private readonly CameraNode[] nodes;
        // Distance along the path at each arc-length sample, SamplesPerSegment per segment; null for equal time per segment.
        private readonly float[] lengths;

        public PathSampler(IReadOnlyList<CameraNode> path, bool constantSpeed)
        {
            if (path.Count == 0)
                throw new ArgumentException("A path needs at least one node.", nameof(path));
            nodes = new CameraNode[path.Count];
            for (int index = 0; index < path.Count; index++)
            {
                var rotation = Quaternion.Normalize(path[index].Rotation);
                // q and -q are the same rotation; keeping each within 90 degrees of the last turns the short way.
                if (index > 0 && Quaternion.Dot(rotation, nodes[index - 1].Rotation) < 0f)
                    rotation = Quaternion.Negate(rotation);
                nodes[index] = path[index] with { Rotation = rotation };
            }
            if (constantSpeed && nodes.Length > 1)
                lengths = MeasureLengths();
        }

        public int Count => nodes.Length;

        public CameraNode Sample(float progress)
        {
            if (nodes.Length == 1)
                return nodes[0];
            var (segment, u) = Locate(Math.Clamp(progress, 0f, 1f));
            var rotation = Quaternion.Normalize(CatmullRom(Rotation(segment - 1), Rotation(segment), Rotation(segment + 1), Rotation(segment + 2), u));
            float fov = CatmullRom(Fov(segment - 1), Fov(segment), Fov(segment + 1), Fov(segment + 2), u);
            return new CameraNode(Position(segment, u), rotation, Math.Clamp(fov, MinFov, MaxFov));
        }

        // The progress at which the camera reaches node index.
        public float NodeProgress(int index)
        {
            int last = nodes.Length - 1;
            if (last == 0)
                return 0f;
            if (lengths == null)
                return (float)index / last;
            return lengths[index * SamplesPerSegment] / lengths[^1];
        }

        // The share of the duration at which the camera reaches each node, for the play bar's ticks.
        public float[] NodeTimes(bool easeIn, bool easeOut)
            => Enumerable.Range(0, nodes.Length).Select(index => Unease(NodeProgress(index), easeIn, easeOut)).ToArray();

        // Seconds into the current pass after elapsed seconds of playback; a looping path starts again from its first node.
        public static float PassTime(float elapsed, float duration, bool loop)
            => loop ? elapsed % duration : Math.Min(elapsed, duration);

        public static float Ease(float t, bool easeIn, bool easeOut)
        {
            t = Math.Clamp(t, 0f, 1f);
            return easeIn && easeOut ? t * t * (3f - 2f * t)
                : easeIn ? t * t
                : easeOut ? 1f - (1f - t) * (1f - t)
                : t;
        }

        // The time share at which Ease reaches progress; every curve rises from 0 to 1, so bisection finds it.
        public static float Unease(float progress, bool easeIn, bool easeOut)
        {
            // Smoothstep is flat near its ends, where float bisection would stop short of them.
            if (progress <= 0f || progress >= 1f)
                return Math.Clamp(progress, 0f, 1f);
            float low = 0f, high = 1f;
            for (int i = 0; i < 24; i++)
            {
                float mid = (low + high) / 2f;
                if (Ease(mid, easeIn, easeOut) < progress)
                    low = mid;
                else
                    high = mid;
            }
            return (low + high) / 2f;
        }

        private (int segment, float u) Locate(float progress)
        {
            int segments = nodes.Length - 1;
            if (lengths == null)
            {
                float scaled = progress * segments;
                int at = Math.Min((int)scaled, segments - 1);
                return (at, scaled - at);
            }
            float target = progress * lengths[^1];
            int index = Array.BinarySearch(lengths, target);
            if (index < 0)
                index = ~index - 1;
            index = Math.Clamp(index, 0, lengths.Length - 2);
            float span = lengths[index + 1] - lengths[index];
            float within = span > 0f ? (target - lengths[index]) / span : 0f;
            int segment = index / SamplesPerSegment;
            return (segment, (index % SamplesPerSegment + within) / SamplesPerSegment);
        }

        private float[] MeasureLengths()
        {
            int segments = nodes.Length - 1;
            var table = new float[segments * SamplesPerSegment + 1];
            var previous = nodes[0].Position;
            for (int segment = 0; segment < segments; segment++)
            {
                for (int step = 1; step <= SamplesPerSegment; step++)
                {
                    var point = Position(segment, (float)step / SamplesPerSegment);
                    int index = segment * SamplesPerSegment + step;
                    table[index] = table[index - 1] + Vector3.Distance(previous, point);
                    previous = point;
                }
            }
            return table[^1] < StillLength ? null : table;
        }

        // Centripetal Catmull-Rom (Barry and Goldman's form), relative to the segment's start so float error stays at the
        // segment's scale instead of that of Genshin's absolute coordinates.
        private Vector3 Position(int segment, float u)
        {
            var origin = nodes[segment].Position;
            var p0 = PositionAt(segment - 1) - origin;
            var p1 = Vector3.Zero;
            var p2 = PositionAt(segment + 1) - origin;
            var p3 = PositionAt(segment + 2) - origin;
            float t0 = 0f;
            float t1 = t0 + Knot(p0, p1);
            float t2 = t1 + Knot(p1, p2);
            float t3 = t2 + Knot(p2, p3);
            float t = t1 + (t2 - t1) * u;
            var a1 = Blend(p0, p1, t0, t1, t);
            var a2 = Blend(p1, p2, t1, t2, t);
            var a3 = Blend(p2, p3, t2, t3, t);
            var b1 = Blend(a1, a2, t0, t2, t);
            var b2 = Blend(a2, a3, t1, t3, t);
            return origin + Blend(b1, b2, t1, t2, t);
        }

        // A repeated node would make a zero knot interval, so every interval is kept just above zero.
        private static float Knot(Vector3 from, Vector3 to) => MathF.Max(MathF.Pow(Vector3.Distance(from, to), Alpha), 1e-4f);

        private static Vector3 Blend(Vector3 from, Vector3 to, float start, float end, float t)
            => (end - t) / (end - start) * from + (t - start) / (end - start) * to;

        // Past either end, a phantom node mirrors the neighbouring node through the end node.
        private Vector3 PositionAt(int index)
        {
            int last = nodes.Length - 1;
            return index < 0 ? 2f * nodes[0].Position - nodes[1].Position
                : index > last ? 2f * nodes[last].Position - nodes[last - 1].Position
                : nodes[index].Position;
        }

        private Quaternion Rotation(int index)
        {
            int last = nodes.Length - 1;
            return index < 0 ? Mirror(nodes[0].Rotation, nodes[1].Rotation)
                : index > last ? Mirror(nodes[last].Rotation, nodes[last - 1].Rotation)
                : nodes[index].Rotation;
        }

        private static Quaternion Mirror(Quaternion end, Quaternion neighbour)
            => new(2f * end.X - neighbour.X, 2f * end.Y - neighbour.Y, 2f * end.Z - neighbour.Z, 2f * end.W - neighbour.W);

        private float Fov(int index)
        {
            int last = nodes.Length - 1;
            return index < 0 ? 2f * nodes[0].Fov - nodes[1].Fov
                : index > last ? 2f * nodes[last].Fov - nodes[last - 1].Fov
                : nodes[index].Fov;
        }

        // Uniform Catmull-Rom, which passes through p1 at u = 0 and p2 at u = 1.
        private static float CatmullRom(float p0, float p1, float p2, float p3, float u)
            => 0.5f * (2f * p1 + (p2 - p0) * u + (2f * p0 - 5f * p1 + 4f * p2 - p3) * u * u + (3f * p1 - p0 - 3f * p2 + p3) * u * u * u);

        private static Quaternion CatmullRom(Quaternion p0, Quaternion p1, Quaternion p2, Quaternion p3, float u)
            => new(CatmullRom(p0.X, p1.X, p2.X, p3.X, u), CatmullRom(p0.Y, p1.Y, p2.Y, p3.Y, u),
                CatmullRom(p0.Z, p1.Z, p2.Z, p3.Z, u), CatmullRom(p0.W, p1.W, p2.W, p3.W, u));
    }
}
