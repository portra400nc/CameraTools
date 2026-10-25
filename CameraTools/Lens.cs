using Vec3 = System.Numerics.Vector3;

namespace CameraTools
{
    // A rectangle on the screen as fractions of its width and height, measured from the top left.
    internal readonly record struct ScreenBox(float Left, float Top, float Right, float Bottom);

    // An eye as the camera sees it: meters along the view axis, and where it is on the screen from -1 at the left and the
    // top to 1, as the focus point is.
    internal readonly record struct EyeSight(float Meters, float X, float Y)
    {
        public bool Seen => Meters > 0f && MathF.Abs(X) <= 1f && MathF.Abs(Y) <= 1f;
    }

    // The arithmetic of the depth of field shader's camera, which has a full-frame sensor. It uses nothing of Unity's, so
    // it can be checked outside the game.
    internal static class Lens
    {
        // Half of the sensor's 43.3 mm diagonal.
        private const float HalfDiagonal = 21.65f;
        // The eye in focus keeps it until the other is this many meters nearer, so a face turned to the camera does not
        // flicker between them.
        public const float EyeMargin = 0.03f;

        // Third-stops, as lenses mark them.
        public static readonly float[] Stops =
        {
            0.95f, 1f, 1.1f, 1.2f, 1.4f, 1.6f, 1.8f, 2f, 2.2f, 2.5f, 2.8f, 3.2f, 3.5f, 4f, 4.5f, 5f, 5.6f, 6.3f, 7.1f, 8f, 9f, 10f, 11f,
            13f, 14f, 16f, 18f, 20f, 22f,
        };

        // The focal length in mm that gives a vertical field of view in degrees on a sensor as wide as the picture: the
        // sensor's height is its diagonal divided by sqrt(1 + aspect^2), where aspect is width / height.
        public static float FocalLength(float fieldOfView, float aspect)
            => HalfDiagonal / (MathF.Tan(fieldOfView * MathF.PI / 360f) * MathF.Sqrt(1f + aspect * aspect));

        // The height in mm of a full-frame diagonal's sensor in the picture's shape, as FocalLength reckons it.
        public static float SensorHeight(float aspect) => 2f * HalfDiagonal / MathF.Sqrt(1f + aspect * aspect);

        // A rail from 0 to 1 over meters from near to far on a logarithmic scale, so a near focus, where centimeters
        // matter, gets as much of the rail as a far one.
        public static float Meters(float rail, float near, float far) => near * MathF.Pow(far / near, rail);

        public static float Rail(float meters, float near, float far) => MathF.Log(meters / near) / MathF.Log(far / near);

        // The distance from the camera to a point along the view axis, which the add-on's focus distance is measured on.
        public static float AlongView(Vec3 point, Vec3 camera, Vec3 forward) => Vec3.Dot(point - camera, Vec3.Normalize(forward));

        // The Eyes mode's eye in focus after current, 0 the left and 1 the right, or -1 while neither is seen and the focus
        // point stands in: the nearer seen eye, and the current one until the other is EyeMargin nearer.
        public static int NearerEye(EyeSight? left, EyeSight? right, int current)
        {
            bool seesLeft = left is { Seen: true }, seesRight = right is { Seen: true };
            if (!seesLeft || !seesRight)
                return seesLeft ? 0 : seesRight ? 1 : -1;
            float toLeft = left.Value.Meters, toRight = right.Value.Meters;
            return current switch
            {
                0 => toRight < toLeft - EyeMargin ? 1 : 0,
                1 => toLeft < toRight - EyeMargin ? 0 : 1,
                _ => toLeft <= toRight ? 0 : 1,
            };
        }

        // The Focus speed row's words for the seconds the autofocus takes to cover 99% of the way.
        public static string Settle(float seconds) => MathF.Round(seconds, 1) <= 0f ? "Instant" : seconds.ToString("0.#' s'");

        // iMMERSE's autofocus covers 99% of the way in 2 - 2 * AUTOFOCUS_SPEED seconds.
        public static float ImmerseSeconds(float speed) => 2f - 2f * Math.Clamp(speed, 0f, 1f);

        public static float ImmerseSpeed(float seconds) => 1f - Math.Clamp(seconds, 0f, 2f) / 2f;

        public static float FieldOfView(float focalLength, float aspect)
            => MathF.Atan(HalfDiagonal / (focalLength * MathF.Sqrt(1f + aspect * aspect))) * 360f / MathF.PI;

        // The position in Stops of the stop nearest to an aperture, by ratio as stops are spaced.
        public static int NearestStop(float aperture)
        {
            int nearest = 0;
            float least = float.PositiveInfinity;
            for (int index = 0; index < Stops.Length; index++)
            {
                float distance = MathF.Abs(MathF.Log(Math.Max(aperture, 0.01f) / Stops[index]));
                if (distance >= least)
                    continue;
                least = distance;
                nearest = index;
            }
            return nearest;
        }

        // The shader's get_af_aabb: the autofocus samples a square as wide as range times the picture's width around the
        // focus point, cut off at the screen's edges. The point runs from -1 at the left and the top to 1.
        public static ScreenBox FocusWindow(float pointX, float pointY, float range, float aspect)
        {
            float u = pointX * 0.5f + 0.5f, v = pointY * 0.5f + 0.5f;
            float halfWidth = range * 0.5f, halfHeight = range * aspect * 0.5f;
            return new ScreenBox(Math.Clamp(u - halfWidth, 0f, 1f), Math.Clamp(v - halfHeight, 0f, 1f),
                Math.Clamp(u + halfWidth, 0f, 1f), Math.Clamp(v + halfHeight, 0f, 1f));
        }
    }
}
