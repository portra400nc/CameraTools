using System.Globalization;

namespace CameraTools
{
    // ReShade's depth settings as an effect sees them. ReLight rebuilds every position from the depth buffer through these,
    // so a light lands where a surface at the same screen point and depth would. A setting ReShade does not define takes
    // the default of iMMERSE's mmx_depth.fxh.
    internal readonly record struct DepthSettings(float Multiplier, bool Logarithmic, bool Reversed, float FarPlane)
    {
        public static readonly DepthSettings Defaults = new(1f, false, true, 1000f);

        public static DepthSettings Parse(string multiplier, string logarithmic, string reversed, string farPlane) => new(
            Number(multiplier, Defaults.Multiplier),
            logarithmic == "" ? Defaults.Logarithmic : Number(logarithmic, 0f) != 0f,
            reversed == "" ? Defaults.Reversed : Number(reversed, 1f) != 0f,
            Number(farPlane, Defaults.FarPlane));

        // mmx_depth.fxh's linearize, from a depth buffer value to 0 at the camera and 1 at the far plane.
        public float Linearize(float raw)
        {
            float x = raw * Multiplier;
            if (Logarithmic)
                x *= x + (1f - x) * 0.04975f;
            if (Reversed)
                x = 1f - x;
            x /= FarPlane - x * (FarPlane - 1f);
            return Math.Clamp(x, 0f, 1f);
        }

        // mmx_camera.fxh's depth_to_z: the distance along the view in ReLight's own units.
        public float ProjectedZ(float linear) => linear * FarPlane + 1f;

        private static float Number(string text, float fallback)
            => float.TryParse(text.TrimEnd('f', 'F'), NumberStyles.Float, CultureInfo.InvariantCulture, out float value) ? value : fallback;
    }

    // One light as ReLight's _LightDesc unpacks a RELIGHT_LIGHT_DATA uint4: x holds the screen position in 12 bits per axis
    // and red, y the cube root of the linear depth and the radius as 15-bit halves, z green, blue, the RGB flag and the
    // intensity as a 15-bit half. ReLight squares the intensity.
    internal static class ReLightPacking
    {
        private const uint RgbFlag = 0x8000;

        public static (uint X, uint Y, uint Z, uint W) Pack(float u, float v, float linearDepth, float radius,
            byte red, byte green, byte blue, float intensity)
        {
            uint x = Unit12(u) << 20 | Unit12(v) << 8 | red;
            uint y = Half15(MathF.Cbrt(linearDepth)) << 17 | Half15(radius) << 2;
            uint z = (uint)green << 24 | (uint)blue << 16 | RgbFlag | Half15(intensity);
            return (x, y, z, 0u);
        }

        private static uint Unit12(float value) => (uint)MathF.Round(Math.Clamp(value, 0f, 1f) * 4095f);

        // The shader reads 15 bits as a half, so a value keeps its exponent and mantissa and loses the sign.
        private static uint Half15(float value) => (uint)BitConverter.HalfToInt16Bits((Half)Math.Max(value, 0f)) & 0x7FFF;
    }
}
