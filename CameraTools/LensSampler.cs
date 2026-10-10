namespace CameraTools
{
    // The camera arithmetic of lens-sampled depth of field: where in the aperture each frame looks from, how wide the
    // aperture is, and how the projection keeps the focus plane still while the camera moves inside it. It uses nothing of
    // Unity's, so it can be checked outside the game.
    internal static class LensSampler
    {
        // About 137.5 degrees, so no two points of the spiral line up.
        private static readonly float GoldenAngle = MathF.PI * (3f - MathF.Sqrt(5f));

        // The index-th of count points spread evenly over the unit disk on a Vogel spiral, x right and y up. With 3 or more
        // blades the disk is squeezed into the regular polygon with its corners on the unit circle and one corner turned
        // rotation degrees from the right, as an aperture of that many straight blades.
        public static (float X, float Y) Sample(int index, int count, int blades, float rotation)
        {
            float radius = MathF.Sqrt((index + 0.5f) / count);
            float angle = index * GoldenAngle;
            if (blades >= 3)
            {
                float sector = 2f * MathF.PI / blades;
                float fromCorner = (angle - rotation * MathF.PI / 180f) % sector;
                if (fromCorner < 0f)
                    fromCorner += sector;
                radius *= MathF.Cos(sector / 2f) / MathF.Cos(fromCorner - sector / 2f);
            }
            return (radius * MathF.Cos(angle), radius * MathF.Sin(angle));
        }

        // The aperture's radius in metres for an f-number at the focal length a full-frame camera needs for this vertical
        // field of view, so the blur matches that camera's.
        public static float ApertureRadius(float fieldOfView, float aspect, float fNumber)
            => Lens.FocalLength(fieldOfView, aspect) / 1000f / (2f * fNumber);

        // The depth in metres along the view of a value in the camera's reversed depth buffer, which holds 1 at the near
        // plane and 0 at the far one.
        public static float ViewDepth(float reversedDepth, float near, float far)
            => far * near / ((far - near) * reversedDepth + near);

        // What to add to a symmetric projection's m02 and m12 when the camera moves x metres right and y metres up, so the
        // plane focus metres ahead stays where it was on the screen. Unity's view space looks down -z: a point there at
        // depth d has z = -d, and the camera's move shifts it by -x, so m02 * -focus must give back m00 * x.
        public static (float M02, float M12) Shear(float m00, float m11, float x, float y, float focus)
            => (-m00 * x / focus, -m11 * y / focus);
    }
}
