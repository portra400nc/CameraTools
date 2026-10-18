using System.Numerics;

namespace CameraTools
{
    // Position is an absolute world position, so a node stays put when Genshin shifts its world origin. Fov is in degrees.
    // Time, in seconds from the path's start, pins the node: the camera reaches it exactly then.
    public readonly record struct CameraNode(Vector3 Position, Quaternion Rotation, float Fov, float? Time = null);

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
        // Distance along the path at each arc-length sample, SamplesPerSegment per segment; null for a path that only turns.
        private readonly float[] lengths;
        // The progress at which the camera reaches each node. Every channel is timed by these, so none changes its rate at a node.
        private readonly float[] times;
        // The camera's speed at each node, in metres per unit of progress; null with lengths.
        private readonly float[] speeds;

        public PathSampler(IReadOnlyList<CameraNode> path, bool constantSpeed, float duration)
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
            if (nodes.Length > 1)
                lengths = MeasureLengths();
            Pinned = nodes.Any(node => node.Time != null);
            times = Spread(constantSpeed, duration);
            if (lengths != null)
                speeds = constantSpeed && !Pinned ? Enumerable.Repeat(lengths[^1], nodes.Length).ToArray() : NodeSpeeds();
        }

        public int Count => nodes.Length;

        // Whether any node has a time of its own.
        public bool Pinned { get; }

        public CameraNode Sample(float progress)
        {
            if (nodes.Length == 1)
                return nodes[0];
            var (segment, u) = Locate(Math.Clamp(progress, 0f, 1f));
            float t0 = Time(segment - 1), t1 = times[segment], t2 = times[segment + 1], t3 = Time(segment + 2);
            var rotation = Quaternion.Normalize(Spline(Rotation(segment - 1), Rotation(segment), Rotation(segment + 1), Rotation(segment + 2), t0, t1, t2, t3, u));
            float fov = Spline(Fov(segment - 1), Fov(segment), Fov(segment + 1), Fov(segment + 2), t0, t1, t2, t3, u);
            return new CameraNode(Position(segment, Along(segment, u)), rotation, Math.Clamp(fov, MinFov, MaxFov));
        }

        // The progress at which the camera reaches node index.
        public float NodeProgress(int index) => times[index];

        // The share of the duration at which the camera reaches each node, for the play bar's ticks.
        public float[] NodeTimes(bool easeIn, bool easeOut)
            => Enumerable.Range(0, nodes.Length).Select(index => Pinned ? NodeProgress(index) : Unease(NodeProgress(index), easeIn, easeOut)).ToArray();

        // The progress at a share of the duration. Easing would move a pinned node's moment, so a pinned path has none.
        public float Progress(float share, bool easeIn, bool easeOut) => Pinned ? Math.Clamp(share, 0f, 1f) : Ease(share, easeIn, easeOut);

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

        // The progress at which the camera reaches each node. A pinned node's is its time's share of the duration. The
        // others share the time between the pinned nodes around them by the usual rule, distance with constant speed and
        // even spacing without, and the path's ends count as pinned at 0 and at the duration when they have no time.
        private float[] Spread(bool constantSpeed, float duration)
        {
            int last = nodes.Length - 1;
            var usual = new float[nodes.Length];
            for (int index = 0; index <= last; index++)
                usual[index] = last == 0 ? 0f : constantSpeed && lengths != null ? Distance(index) / lengths[^1] : (float)index / last;
            if (!Pinned)
                return usual;
            var result = new float[nodes.Length];
            result[0] = Pin(0, 0f);
            int from = 0;
            for (int to = 1; to <= last; to++)
            {
                if (nodes[to].Time == null && to < last)
                    continue;
                // A file's times out of order would make time run backward; the camera holds instead.
                result[to] = Math.Max(Pin(to, 1f), result[from]);
                float span = usual[to] - usual[from];
                for (int index = from + 1; index < to; index++)
                {
                    float share = span > 0f ? (usual[index] - usual[from]) / span : (float)(index - from) / (to - from);
                    result[index] = result[from] + share * (result[to] - result[from]);
                }
                from = to;
            }
            return result;

            float Pin(int index, float end) => nodes[index].Time is float time ? Math.Clamp(time / duration, 0f, 1f) : end;
        }

        // The segment that progress falls in, and how far through its time; a segment of no time is never the one.
        private (int segment, float u) Locate(float progress)
        {
            int last = nodes.Length - 1;
            int segment = Array.BinarySearch(times, progress);
            if (segment < 0)
                segment = ~segment - 1;
            segment = Math.Clamp(segment, 0, last - 1);
            while (segment < last - 1 && times[segment + 1] <= times[segment])
                segment++;
            while (segment > 0 && times[segment + 1] <= times[segment])
                segment--;
            float span = times[segment + 1] - times[segment];
            return (segment, Math.Clamp((progress - times[segment]) / span, 0f, 1f));
        }

        // The curve's parameter u of the way through a segment's time: the distance travelled by then follows a cubic
        // through both nodes' distances and speeds, which the arc-length table turns back into a parameter.
        private float Along(int segment, float u)
        {
            if (lengths == null)
                return u;
            float span = times[segment + 1] - times[segment];
            float target = Hermite(Distance(segment), Distance(segment + 1), speeds[segment] * span, speeds[segment + 1] * span, u);
            int first = segment * SamplesPerSegment;
            int index = Array.BinarySearch(lengths, first, SamplesPerSegment + 1, target);
            if (index < 0)
                index = ~index - 1;
            index = Math.Clamp(index, first, first + SamplesPerSegment - 1);
            float step = lengths[index + 1] - lengths[index];
            float within = step > 0f ? Math.Clamp((target - lengths[index]) / step, 0f, 1f) : 0f;
            return (index - first + within) / SamplesPerSegment;
        }

        // The speed at each node from its segments' average speeds. Inside, it is their weighted harmonic mean, as in Fritsch
        // and Butland's monotone cubic, so the camera never runs backward and slows down at a node beside a short segment.
        // At either end, it is the end segment's own average.
        private float[] NodeSpeeds()
        {
            int last = nodes.Length - 1;
            var spans = Enumerable.Range(0, last).Select(segment => times[segment + 1] - times[segment]).ToArray();
            // Two nodes pinned at one moment have no time between them, and the camera jumps.
            var averages = Enumerable.Range(0, last)
                .Select(segment => spans[segment] > 0f ? (Distance(segment + 1) - Distance(segment)) / spans[segment] : 0f).ToArray();
            var result = new float[nodes.Length];
            result[0] = averages[0];
            result[last] = averages[last - 1];
            for (int index = 1; index < last; index++)
            {
                float before = averages[index - 1], after = averages[index];
                if (before <= 0f || after <= 0f)
                    continue;
                float w1 = 2f * spans[index] + spans[index - 1], w2 = spans[index] + 2f * spans[index - 1];
                result[index] = (w1 + w2) / (w1 / before + w2 / after);
            }
            return result;
        }

        private float Distance(int index) => lengths[index * SamplesPerSegment];

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

        // Past either end, a phantom node's time is as far from the end node's as the neighbouring node's.
        private float Time(int index)
        {
            int last = nodes.Length - 1;
            return index < 0 ? 2f * times[0] - times[1]
                : index > last ? 2f * times[last] - times[last - 1]
                : times[index];
        }

        private float Fov(int index)
        {
            int last = nodes.Length - 1;
            return index < 0 ? 2f * nodes[0].Fov - nodes[1].Fov
                : index > last ? 2f * nodes[last].Fov - nodes[last - 1].Fov
                : nodes[index].Fov;
        }

        // Catmull-Rom over the nodes' times, which passes through p1 at u = 0 and p2 at u = 1: the slope at a node spans
        // both its neighbours' times, so the value changes at the same rate on either side of the node.
        private static float Spline(float p0, float p1, float p2, float p3, float t0, float t1, float t2, float t3, float u)
        {
            float span = t2 - t1;
            return Hermite(p1, p2, (p2 - p0) / (t2 - t0) * span, (p3 - p1) / (t3 - t1) * span, u);
        }

        private static Quaternion Spline(Quaternion p0, Quaternion p1, Quaternion p2, Quaternion p3, float t0, float t1, float t2, float t3, float u)
            => new(Spline(p0.X, p1.X, p2.X, p3.X, t0, t1, t2, t3, u), Spline(p0.Y, p1.Y, p2.Y, p3.Y, t0, t1, t2, t3, u),
                Spline(p0.Z, p1.Z, p2.Z, p3.Z, t0, t1, t2, t3, u), Spline(p0.W, p1.W, p2.W, p3.W, t0, t1, t2, t3, u));

        // The cubic from p1 to p2 with slopes m1 and m2, per unit of u.
        private static float Hermite(float p1, float p2, float m1, float m2, float u)
        {
            float u2 = u * u, u3 = u2 * u;
            return (2f * u3 - 3f * u2 + 1f) * p1 + (u3 - 2f * u2 + u) * m1 + (3f * u2 - 2f * u3) * p2 + (u3 - u2) * m2;
        }
    }
}
