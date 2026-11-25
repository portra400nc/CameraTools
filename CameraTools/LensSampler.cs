namespace CameraTools
{
    // The aperture a lens-sampled screenshot looks through: the depth of field's shape rows, as the Enhancements add-on's
    // gather draws a disc (shaders/dof.hlsli, dof_shape_distance), and how much the rim's points are thinned out.
    internal readonly record struct Aperture(int Blades, float Roundness, float RotationDegrees, float Anamorphic, float EdgeSoftness)
    {
        public static readonly Aperture Round = new(0, 1f, 0f, 1f, 0f);
    }

    // One lens-sampled screenshot: its aperture points, the aperture's radius in meters, the focus distance in meters along
    // the view, locked before the first sample, the cat's eye and swirl the add-on's accumulator cuts each pixel's points
    // by, red's and blue's shift against green in render pixels per unit of a point, the spherical aberration that
    // focuses each point by its radius, the highlight lift's strength and threshold, by which the add-on multiplies each
    // sample's out-of-focus light that stands above its surroundings (Enhancements/shaders/lift.hlsli), and the lens's CoC
    // radius in render pixels per diopter from focus, which fades the lift in.
    // A long exposure's lens samples are also its time steps: Order gives the point each frame looks through, so every
    // stretch of the shutter sees the whole aperture.
    internal sealed record LensShot((float X, float Y)[] Points, float Radius, float Focus, float CatEye, float Swirl = 0f,
        (float Red, float Blue) Chromatic = default, float SphericalAberration = 0f, float Lift = 0f, float LiftThreshold = 2f,
        float CocPerDiopter = 0f, int[] Order = null)
    {
        // The point frame looks through.
        public int PointOf(int frame) => Order is { } order && frame < order.Length ? order[frame] : frame;

        // The farthest point from the center: the add-on's lens sample and its cuts take the points over it, so the
        // aperture fits their unit disc whatever its blades or squeeze.
        public float Reach { get; } = Points.Length == 0 ? 1f : MathF.Max(Points.Max(p => MathF.Sqrt(p.X * p.X + p.Y * p.Y)), 1e-6f);

        public (float X, float Y) Sample(int index) => (Points[index].X / Reach, Points[index].Y / Reach);

        public float FocusAt(int index) => LensSampler.ZoneFocus(Focus, Points[index].X, Points[index].Y, SphericalAberration);
    }

    // The camera arithmetic of lens-sampled depth of field: where in the aperture each sample looks from, how wide the
    // aperture is, how the projection keeps the focus plane still while the camera moves inside it, and how much each
    // sample's own depth of field fills the gaps between them. It uses nothing of Unity's, so it can be checked outside
    // the game.
    internal static class LensSampler
    {
        // About 137.5 degrees, so no two rings' points line up.
        private static readonly float GoldenAngle = MathF.PI * (3f - MathF.Sqrt(5f));
        private const int Steps = 4096;

        // count points over the aperture, x right and y up, the shape's area that of the unit disc: one at the center, then
        // rings, each turned by the golden angle, with points in proportion to their radius, so every point stands for the
        // same area. Concentric rings give cleaner disc edges than a spiral (Unreal Engine's accumulation depth of field
        // calls its pattern Golden Hexaweb). Within a ring the points are spaced by the shape's area, not its angle, so a
        // bladed aperture's corners are not thinned out; across the rings, by the edge softness's density.
        public static (float X, float Y)[] Points(int count, Aperture aperture)
        {
            count = Math.Max(count, 1);
            var points = new (float X, float Y)[count];
            if (count == 1)
                return points;
            var shape = new Shape(aperture);
            var radii = new Rim(aperture.EdgeSoftness);
            int rings = Math.Max(1, (int)MathF.Round(MathF.Sqrt(count / MathF.PI)));
            int left = count - 1, total = rings * (rings + 1) / 2, at = 1;
            for (int ring = 1; ring <= rings; ring++)
            {
                int n = ring == rings ? left : Math.Min(left, Math.Max(1, (int)MathF.Round((count - 1) * (float)ring / total)));
                left -= n;
                // The ring stands for the share of the area between the points before it and after it, and sits where
                // half of that share lies inside it.
                float q = radii.Radius((at + 0.5f * n) / count);
                float turn = ring * GoldenAngle / (2f * MathF.PI);
                for (int i = 0; i < n; i++)
                {
                    float share = (i + 0.5f) / n + turn;
                    float angle = shape.Angle(share - MathF.Floor(share));
                    float r = q * shape.Reach(angle);
                    points[at + i] = (r * MathF.Cos(angle) / shape.Squeeze, r * MathF.Sin(angle));
                }
                at += n;
            }
            return points;
        }

        // The aperture's radius in meters: a full-frame lens's focal length for this vertical field of view over twice the
        // f-number, times the miniature scale, as the scene would show through that lens were it a 1:scale model.
        public static float Radius(float fieldOfView, float aspect, float fNumber, int miniature)
            => Math.Max(miniature, 1) * Lens.FocalLength(fieldOfView, aspect) / 1000f / (2f * fNumber);

        // What to add to a symmetric projection's m02 and m12 when the camera moves x meters right and y meters up, so the
        // plane focus meters ahead stays where it was on the screen. Unity's view space looks down -z: a point there at
        // depth d has z = -d, and the camera's move shifts it by -x, so m02 * -focus must give back m00 * x.
        public static (float M02, float M12) Shear(float m00, float m11, float x, float y, float focus)
            => (-m00 * x / focus, -m11 * y / focus);

        // Where the zone of the aperture through a point focuses, in meters, under spherical aberration sa: in inverse
        // distance, each zone's focus moves by sa times its radius squared over the mean, 0.5, so the mean focus stays. With
        // sa above 0 the rim focuses farther: behind focus a point's disc gathers its light at the rim, a soap bubble, and in
        // front it spreads it there, as an overcorrected lens's does. 0.5 times the soap bubble row gives the rim the
        // fourfold light of the gather's disc at 0.5.
        public static float ZoneFocus(float focus, float x, float y, float sa) => focus / (1f - sa * (x * x + y * y - 0.5f));

        // How many render pixels a channel's picture lies further along a unit lens point than green's: its focal length is
        // dispersion longer, so its CoC is green's less v d / (2 N (1 + d)) on the sensor, v the sensor's distance behind
        // the lens, f S / (S - f), as the add-on's depth of field reckons it (shaders/dof.hlsli, dof_lens_at).
        public static float ChromaticShift(float focalMm, float fNumber, float focusMeters, float sensorMm, int heightPx, float dispersion)
        {
            float s = focusMeters * 1000f, v = focalMm * s / MathF.Max(s - focalMm, 1e-3f);
            return v * dispersion / (2f * fNumber * (1f + dispersion)) * heightPx / sensorMm;
        }

        // The share of the full CoC each sample's own depth of field blurs by, so the copies of a point the samples make
        // merge into its disc: neighboring points lie sqrt(pi / count) of the aperture's radius apart, and a disc that wide
        // overlaps the next copies'. Against 4096 samples (Enhancements/tests/lens_hybrid.py) half of that already rippled
        // as the exact disc does on lamps, but on the Deck stars at night still showed each copy as a dot; the whole
        // spacing smoothed the simulation's lamps below the exact disc's ripple.
        public static float GapFill(int count) => MathF.Sqrt(MathF.PI / Math.Max(count, 1));

        // The order a long exposure's frames take the points in, as their time steps. The points run from the center out,
        // ring by ring; taken in that order, a motion streak's start would be seen through the center and its end through the
        // rim, a sharp head and a soft tail. The points ordered by the golden ratio's multiples of their index spread each
        // run of frames over every ring and every side, so each moment of the shutter looks through the whole aperture.
        public static int[] TimeOrder(int count)
        {
            float phi = (MathF.Sqrt(5f) - 1f) / 2f;
            return Enumerable.Range(0, Math.Max(count, 0)).OrderBy(i => ((i + 0.5) * phi) % 1.0).ToArray();
        }

        // The aperture's outline: how far it reaches in each direction, and the angles that split its area evenly.
        private sealed class Shape
        {
            private readonly float[] reach = new float[Steps];
            private readonly float[] area = new float[Steps + 1];
            public readonly float Squeeze;

            public Shape(Aperture a)
            {
                Squeeze = MathF.Max(a.Anamorphic, 0.01f);
                float sector = a.Blades >= 3 ? 2f * MathF.PI / a.Blades : 0f;
                float roundness = Math.Clamp(a.Roundness, 0f, 1f), rotation = a.RotationDegrees * MathF.PI / 180f;
                float sum = 0f;
                for (int i = 0; i < Steps; i++)
                {
                    float angle = (i + 0.5f) / Steps * 2f * MathF.PI;
                    float g = 1f;
                    if (sector > 0f)
                    {
                        // dof_shape_distance's polygon, its sides rounded toward the circle by roundness.
                        float t = angle - rotation - MathF.PI * 0.5f - sector * 0.5f;
                        float phi = t - sector * MathF.Floor(t / sector + 0.5f);
                        g = (1f - roundness) * MathF.Cos(phi) / MathF.Cos(sector * 0.5f) + roundness;
                    }
                    reach[i] = 1f / g;
                    area[i] = sum;
                    sum += reach[i] * reach[i];
                }
                area[Steps] = sum;
                // The shape's area is pi times the mean of reach squared, and the anamorphic squeeze narrows x by Squeeze:
                // scale so the squeezed shape's area is the unit disc's.
                float scale = MathF.Sqrt(Squeeze * Steps / sum);
                for (int i = 0; i < Steps; i++)
                    reach[i] *= scale;
            }

            public float Reach(float angle)
            {
                int i = (int)(angle / (2f * MathF.PI) * Steps);
                return reach[Math.Clamp(i, 0, Steps - 1)];
            }

            // The angle below which a share of the shape's area lies.
            public float Angle(float share)
            {
                float target = share * area[Steps];
                int lo = 0, hi = Steps;
                while (hi - lo > 1)
                {
                    int mid = (lo + hi) / 2;
                    if (area[mid] <= target)
                        lo = mid;
                    else
                        hi = mid;
                }
                float span = area[lo + 1] - area[lo];
                float f = span > 0f ? (target - area[lo]) / span : 0f;
                return (lo + f) / Steps * 2f * MathF.PI;
            }
        }

        // How the points spread from the center to the rim: evenly over the area, or with fewer toward the rim as the edge
        // softness rises, as a lens's apodizing filter dims its rim. The density falls from 1 inside half the radius to
        // 1 - softness at the rim.
        private sealed class Rim
        {
            private readonly float[] cumulative = new float[Steps + 1];

            public Rim(float softness)
            {
                softness = Math.Clamp(softness, 0f, 1f);
                float sum = 0f;
                for (int i = 0; i < Steps; i++)
                {
                    cumulative[i] = sum;
                    float q = (i + 0.5f) / Steps;
                    float s = Math.Clamp((q - 0.5f) / 0.5f, 0f, 1f);
                    sum += (1f - softness * s * s * (3f - 2f * s)) * q;
                }
                cumulative[Steps] = sum;
            }

            // The radius within which a share of the points lies.
            public float Radius(float share)
            {
                float target = Math.Clamp(share, 0f, 1f) * cumulative[Steps];
                int lo = 0, hi = Steps;
                while (hi - lo > 1)
                {
                    int mid = (lo + hi) / 2;
                    if (cumulative[mid] <= target)
                        lo = mid;
                    else
                        hi = mid;
                }
                float span = cumulative[lo + 1] - cumulative[lo];
                float f = span > 0f ? (target - cumulative[lo]) / span : 0f;
                return (lo + f) / Steps;
            }
        }
    }
}
