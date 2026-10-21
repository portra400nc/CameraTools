namespace CameraTools
{
    // Where each frame of a supersampled screenshot looks within its pixel, apart from the game so tests run it.
    internal static class SupersamplingPattern
    {
        // The index-th offset from the pixel's centre, in pixels, each axis in -0.5..0.5: the Halton sequence in bases 2
        // and 3, which spreads any number of samples evenly over the pixel.
        public static (float X, float Y) Offset(int index) => (Halton(index + 1, 2) - 0.5f, Halton(index + 1, 3) - 0.5f);

        private static float Halton(int index, int radix)
        {
            float result = 0f, fraction = 1f;
            for (; index > 0; index /= radix)
            {
                fraction /= radix;
                result += fraction * (index % radix);
            }
            return result;
        }
    }
}
