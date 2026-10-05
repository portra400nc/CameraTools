using System.Globalization;
using System.Numerics;

namespace CameraTools
{
    public enum LightKind
    {
        Point,
        Spot,
        // Drawn by iMMERSE ReLight over the finished frame, not by the game.
        Sphere,
    }

    public enum LightReach
    {
        Everything,
        Characters,
    }

    public enum LightFollow
    {
        Nothing,
        Camera,
        Character,
    }

    public enum ColorSource
    {
        Temperature,
        Hue,
    }

    public readonly record struct Pose(Vector3 Position, Quaternion Rotation);

    // What a light follows, as a pose in the scene: the camera, the character, or the world itself.
    public readonly record struct Frame(Vector3 Position, Quaternion Rotation)
    {
        public Pose ToWorld(Pose local) => new(Position + Vector3.Transform(local.Position, Rotation), Rotation * local.Rotation);

        public Pose ToLocal(Pose world)
        {
            var inverse = Quaternion.Inverse(Rotation);
            return new(Vector3.Transform(world.Position - Position, inverse), inverse * world.Rotation);
        }
    }

    // One light as the Lights tab edits it and Lights.json stores it. Local is its pose in the frame Follow names; for
    // Nothing that frame is the absolute world, so the light keeps its place when Genshin shifts its world origin.
    public sealed class LightSetup
    {
        public const float MaxIntensity = 10f;
        // On the Deck, a characters-only light at 1 was a soft fill and at 3 already blew the character out.
        public const float MaxCharacterIntensity = 3f;

        public LightKind Kind { get; set; }
        public LightReach Reach { get; set; }
        public LightFollow Follow { get; set; }
        public Pose Local { get; set; } = new(Vector3.Zero, Quaternion.Identity);
        public float Intensity { get; set; } = 2f;
        public float Range { get; set; } = 8f;
        public float SpotAngle { get; set; } = 60f;
        public float InnerAngle { get; set; } = 5f;
        public float Radius { get; set; } = 0.5f;
        public ColorSource Source { get; set; }
        public float Kelvin { get; set; } = 6500f;
        public float Hue { get; set; }
        public float Saturation { get; set; }

        public bool CharactersOnly => Kind != LightKind.Sphere && Reach == LightReach.Characters;

        public float IntensityLimit => CharactersOnly ? MaxCharacterIntensity : MaxIntensity;

        public Vector3 Color => Source == ColorSource.Temperature ? Colors.Kelvin(Kelvin) : Colors.Hsv(Hue, Saturation / 100f, 1f);
    }

    // Colours as 0 to 1 RGB in a Vector3.
    public static class Colors
    {
        // Tanner Helland's fit to the blackbody colours, good from 1000 K to 40000 K.
        public static Vector3 Kelvin(float kelvin)
        {
            float t = kelvin / 100f;
            float red = t <= 66f ? 255f : 329.698727446f * MathF.Pow(t - 60f, -0.1332047592f);
            float green = t <= 66f ? 99.4708025861f * MathF.Log(t) - 161.1195681661f : 288.1221695283f * MathF.Pow(t - 60f, -0.0755148492f);
            float blue = t >= 66f ? 255f : t <= 19f ? 0f : 138.5177312231f * MathF.Log(t - 10f) - 305.0447927307f;
            return new Vector3(Unit(red), Unit(green), Unit(blue));
        }

        // hue in degrees, saturation and value from 0 to 1.
        public static Vector3 Hsv(float hue, float saturation, float value)
        {
            float Channel(float n)
            {
                float k = (n + hue / 60f) % 6f;
                return value - value * saturation * Math.Clamp(Math.Min(k, 4f - k), 0f, 1f);
            }
            return new Vector3(Channel(5f), Channel(3f), Channel(1f));
        }

        // The hue in degrees and the saturation from 0 to 1 of a colour; its value is dropped, because intensity sets brightness.
        public static (float Hue, float Saturation) ToHueSaturation(Vector3 rgb)
        {
            float max = Math.Max(rgb.X, Math.Max(rgb.Y, rgb.Z));
            float delta = max - Math.Min(rgb.X, Math.Min(rgb.Y, rgb.Z));
            float hue = delta == 0f ? 0f
                : max == rgb.X ? 60f * ((rgb.Y - rgb.Z) / delta % 6f)
                : max == rgb.Y ? 60f * ((rgb.Z - rgb.X) / delta + 2f)
                : 60f * ((rgb.X - rgb.Y) / delta + 4f);
            return ((hue + 360f) % 360f, max == 0f ? 0f : delta / max);
        }

        public static string Hex(Vector3 rgb) => $"#{Byte(rgb.X):X2}{Byte(rgb.Y):X2}{Byte(rgb.Z):X2}";

        // Six hex digits, with or without a leading #.
        public static bool TryParseHex(string text, out Vector3 rgb)
        {
            rgb = default;
            text = text.Trim().TrimStart('#');
            if (text.Length != 6 || !int.TryParse(text, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out int value))
                return false;
            rgb = new Vector3((value >> 16 & 0xFF) / 255f, (value >> 8 & 0xFF) / 255f, (value & 0xFF) / 255f);
            return true;
        }

        // A name for a colour temperature, as a photographer would call it.
        public static string KelvinName(float kelvin) => kelvin switch
        {
            < 2200f => "Candle",
            < 3500f => "Warm lamp",
            < 5000f => "Neutral",
            < 7000f => "Daylight",
            _ => "Overcast blue",
        };

        public static byte Byte(float channel) => (byte)MathF.Round(Math.Clamp(channel, 0f, 1f) * 255f, MidpointRounding.AwayFromZero);

        private static float Unit(float channel) => Math.Clamp(channel, 0f, 255f) / 255f;
    }
}
