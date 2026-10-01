namespace CameraTools
{
    // A rectangle on the screen as fractions of its width and height, measured from the top left.
    internal readonly record struct ScreenBox(float Left, float Top, float Right, float Bottom);

    // The arithmetic of the depth of field shader's camera, which has a full-frame sensor. It uses nothing of Unity's, so
    // it can be checked outside the game.
    internal static class Lens
    {
        // Half of the sensor's 43.3 mm diagonal.
        private const float HalfDiagonal = 21.65f;

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
