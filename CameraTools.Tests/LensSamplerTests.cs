// The lens-sampled screenshot's aperture and camera arithmetic (CameraTools/LensSampler.cs): the points fill the shape the
// Enhancements add-on's gather draws a disc in (shaders/dof.hlsli, dof_shape_distance) evenly, the edge softness thins the
// rim, and the camera's move and the projection's shear keep the focus plane still.
using CameraTools;

internal static class LensSamplerTests
{
    // dof_shape_distance without swirl or its area scale: the polygon of blades with rounded sides, after the anamorphic
    // squeeze of x.
    private static float Distance(float x, float y, Aperture a)
    {
        x *= a.Anamorphic;
        float rho = MathF.Sqrt(x * x + y * y);
        if (a.Blades < 3 || rho < 1e-6f)
            return rho;
        float sector = 2f * MathF.PI / a.Blades;
        float t = MathF.Atan2(y, x) - a.RotationDegrees * MathF.PI / 180f - MathF.PI * 0.5f - sector * 0.5f;
        float phi = t - sector * MathF.Floor(t / sector + 0.5f);
        return (1f - a.Roundness) * rho * MathF.Cos(phi) / MathF.Cos(sector * 0.5f) + a.Roundness * rho;
    }

    // The shape's size over that of the shape with the unit disc's area, from a fine grid: the distance times it is 1 on
    // the outline of the shape the points fill.
    private static float AreaScale(Aperture a)
    {
        const int n = 1200;
        int inside = 0;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                if (Distance(-2f + 4f * (i + 0.5f) / n, -2f + 4f * (j + 0.5f) / n, a) <= 1f)
                    inside++;
        return MathF.Sqrt(16f * inside / ((float)n * n) / MathF.PI);
    }

    public static void Run(Action<string, bool> check)
    {
        var five = new Aperture(5, 0f, 0f, 1f, 0f);
        var round = LensSampler.Points(256, Aperture.Round);
        var bladed = LensSampler.Points(256, five);
        float s = AreaScale(five);
        check("256 points, the first at the center", round.Length == 256 && round[0] == (0f, 0f) && bladed.Length == 256);
        // Spread evenly over a shape whose size goes as its outline's distance, the points' mean squared distance is half the
        // outline's: 0.5 of the unit disc's radius squared. Shares within a radius would step by a ring at a time.
        check("a round aperture's points lie within the unit disc, spread evenly over it: their mean squared radius is 0.5",
            round.All(p => MathF.Sqrt(p.X * p.X + p.Y * p.Y) <= 1.0001f) && MathF.Abs(round.Average(p => p.X * p.X + p.Y * p.Y) - 0.5f) < 0.005f);
        check("five straight blades: every point inside the gather's pentagon of the disc's area, spread evenly over it",
            bladed.All(p => Distance(p.X, p.Y, five) * s <= 1.002f)
            && MathF.Abs(bladed.Average(p => Distance(p.X, p.Y, five) * Distance(p.X, p.Y, five)) * s * s - 0.5f) < 0.005f);
        float[] sectors = new float[5];
        foreach (var p in bladed)
        {
            float a = MathF.Atan2(p.Y, p.X) + MathF.PI;
            sectors[Math.Min((int)(a / (2f * MathF.PI) * 5f), 4)] += 1f / bladed.Length;
        }
        check("the pentagon's points are even around it: each fifth of the turn holds a fifth of them, its corners not thinned out",
            sectors.All(f => MathF.Abs(f - 0.2f) < 0.02f));
        check("the points center on the aperture's middle", MathF.Abs(round.Average(p => p.X)) < 0.01f && MathF.Abs(round.Average(p => p.Y)) < 0.01f
            && MathF.Abs(bladed.Average(p => p.X)) < 0.01f && MathF.Abs(bladed.Average(p => p.Y)) < 0.01f);
        var squeezed = LensSampler.Points(256, new Aperture(0, 1f, 0f, 2f, 0f));
        float spreadX = MathF.Sqrt(squeezed.Average(p => p.X * p.X)), spreadY = MathF.Sqrt(squeezed.Average(p => p.Y * p.Y));
        check("an anamorphic ratio of 2 makes the disc half as wide as it is tall, with the disc's area", MathF.Abs(spreadX / spreadY - 0.5f) < 0.02f
            && MathF.Abs(spreadX * spreadY - 0.25f) < 0.01f);
        var soft = LensSampler.Points(256, new Aperture(0, 1f, 0f, 1f, 1f));
        check("edge softness 1 draws the points in from the rim: their mean squared radius is 0.312, against 0.5 evenly spread",
            MathF.Abs(soft.Average(p => p.X * p.X + p.Y * p.Y) - 0.312f) < 0.006f);
        check("the gaps fill with 0.0554 of the CoC at 256 points and 0.0277 at 1024", MathF.Abs(LensSampler.GapFill(256) - 0.0554f) < 1e-4f
            && MathF.Abs(LensSampler.GapFill(1024) - 0.0277f) < 1e-4f);
        // The vertical field of view a full-frame 24 mm lens has on a 16:9 picture.
        float aspect = 16f / 9f, fov = 2f * MathF.Atan(21.65f / (24f * MathF.Sqrt(1f + aspect * aspect))) * 180f / MathF.PI;
        check("a 24 mm lens at f/1.2 has an aperture 1 cm in radius, and 25 cm as a 1:25 miniature",
            MathF.Abs(LensSampler.Radius(fov, aspect, 1.2f, 1) - 0.01f) < 1e-5f && MathF.Abs(LensSampler.Radius(fov, aspect, 1.2f, 25) - 0.25f) < 1e-4f);
        // A point 3 m ahead and one 9 m ahead, seen before and after the camera moves 0.25 m right and 0.1 m up with the shear
        // for focus at 3 m: Unity's projection x = m00 * x_view + m02 * z_view over w = -z_view.
        float m00 = 1.2f, m11 = 2.1f;
        var (m02, m12) = LensSampler.Shear(m00, m11, 0.25f, 0.1f, 3f);
        float Ndc(float m, float shear, float along, float move, float depth) => (m * (along - move) + shear * -depth) / depth;
        check("with the shear, a point on the focus plane stays on the screen as the camera moves, and one behind it moves",
            MathF.Abs(Ndc(m00, m02, 0.4f, 0.25f, 3f) - Ndc(m00, 0f, 0.4f, 0f, 3f)) < 1e-6f && MathF.Abs(Ndc(m11, m12, -0.2f, 0.1f, 3f) - Ndc(m11, 0f, -0.2f, 0f, 3f)) < 1e-6f
            && MathF.Abs(Ndc(m00, m02, 0.4f, 0.25f, 9f) - Ndc(m00, 0f, 0.4f, 0f, 9f)) > 0.01f);
    }
}
