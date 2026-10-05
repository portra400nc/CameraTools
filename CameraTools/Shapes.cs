using Il2CppInterop.Runtime.InteropTypes.Arrays;
using UnityEngine;

namespace CameraTools
{
    // A picture CameraTools draws itself: Width x Height canvas units, colored by Pixel at a point in units from the top
    // left. A shape with a Border is 9-sliced, so it stretches to any size larger than twice its border.
    internal sealed record Shape(string Name, float Width, float Height, float Border, Func<float, float, Color> Pixel);

    // CameraTools' UI is drawn from these shapes and the game's font, not from the game's sprites. The textures are drawn
    // once, from signed distance fields, at Density pixels per unit with mipmaps: the 1:1 canvas of the Deck samples the
    // box-filtered first mip, and 1080p and 1440p screens (canvas scales 1.35 and 1.8) sample between the full texture and
    // that mip. They are DontSave, so the game's unloading of unused assets leaves them alone.
    internal static class Shapes
    {
        public const int Density = 2;

        // The sliced shapes are two texture pixels wider and taller than their borders, which is where they stretch.
        private const float Stretch = 2f / Density;
        private const float FocusCorner = 18f;

        public static readonly Shape Face = Glyph("Face", 24f, 24f, Circle(12f, 12f, 12f));
        public static readonly Shape Bumper = Solid("Bumper", 30f, 18f, 0f, Box(0f, 0f, 30f, 18f, 6f, 6f, 4f, 4f), Style.Light);
        public static readonly Shape Trigger = Solid("Trigger", 26f, 22f, 0f, Box(0f, 0f, 26f, 22f, 9f, 9f, 4f, 4f), Style.Light);
        public static readonly Shape Menu = Glyph("Menu", 26f, 20f, Box(0f, 0f, 26f, 20f, 6f),
            (Union(Box(7.5f, 6f, 11f, 2f, 0f), Box(7.5f, 9f, 11f, 2f, 0f), Box(7.5f, 12f, 11f, 2f, 0f)), Style.White));
        // Two overlapping squares, the back one showing only outside the front one.
        public static readonly Shape View = Glyph("View", 26f, 20f, Box(0f, 0f, 26f, 20f, 6f),
            (Subtract(Ring(Box(11f, 4.5f, 7f, 7f, 1f), 1.5f), Box(7.5f, 8f, 7f, 7f, 1f)), Style.White),
            (Ring(Box(7.5f, 8f, 7f, 7f, 1f), 1.5f), Style.White));
        public static readonly Shape DpadUp = Dpad("DpadUp", Box(10f, 5f, 4f, 5f, 1f));
        public static readonly Shape DpadDown = Dpad("DpadDown", Box(10f, 14f, 4f, 5f, 1f));
        public static readonly Shape DpadLeft = Dpad("DpadLeft", Box(5f, 10f, 5f, 4f, 1f));
        public static readonly Shape DpadRight = Dpad("DpadRight", Box(14f, 10f, 5f, 4f, 1f));
        public static readonly Shape DpadSides = Dpad("DpadSides", Union(Box(5f, 10f, 5f, 4f, 1f), Box(14f, 10f, 5f, 4f, 1f)));
        public static readonly Shape DpadUpDown = Dpad("DpadUpDown", Union(Box(10f, 5f, 4f, 5f, 1f), Box(10f, 14f, 4f, 5f, 1f)));
        public static readonly Shape Key = Framed("Key", 10f + Stretch, 10f + Stretch, 5f, 5f, 1.5f, Style.KeyFill, Style.Cream);
        public static readonly Shape Toast = Framed("Toast", Style.ToastHeight + Stretch, Style.ToastHeight + Stretch,
            Style.ToastHeight / 2f, Style.ToastHeight / 2f, 1f, Style.ToastFill, Style.ToastRing);
        // A name beside a marker, like the toast but smaller.
        public static readonly Shape Pill = Framed("Pill", Style.PillHeight + Stretch, Style.PillHeight + Stretch, Style.PillHeight / 2f,
            Style.PillHeight / 2f, 1f, Style.ToastFill, Style.PillRing);
        public static readonly Shape Bar = Solid("Bar", 4f + Stretch, 4f + Stretch, 2f, Box(0f, 0f, 4f + Stretch, 4f + Stretch, 2f), Style.White);
        // The field of view track is 3 units wide with a 1 unit ring outside it.
        public static readonly Shape FovTrack = new("FovTrack", 5f + Stretch, 5f + Stretch, 2.5f, Layers(
            (Subtract(Box(0f, 0f, 5f + Stretch, 5f + Stretch, 2.5f), Box(1f, 1f, 3f + Stretch, 3f + Stretch, 1.5f)), Style.FovRing),
            (Box(1f, 1f, 3f + Stretch, 3f + Stretch, 1.5f), Style.FovTrack)));
        public static readonly Shape SwitchOff = Framed("SwitchOff", 52f, 26f, 13f, 0f, 1.5f, Style.Track, Style.SwitchRingOff);
        public static readonly Shape SwitchOn = Framed("SwitchOn", 52f, 26f, 13f, 0f, 1.5f, Style.Cream, Style.SwitchRingOn);
        public static readonly Shape Knob = Solid("Knob", 20f, 20f, 0f, Circle(10f, 10f, 10f), Style.Light);
        public static readonly Shape Check = Solid("Check", 10f, 10f, 0f, Union(Line(1.6f, 5.3f, 4.1f, 7.7f, 0.9f), Line(4.1f, 7.7f, 8.5f, 2.5f, 0.9f)), Style.White);
        public static readonly Shape Cross = Solid("Cross", 10f, 10f, 0f, Union(Line(2.3f, 2.3f, 7.7f, 7.7f, 0.85f), Line(7.7f, 2.3f, 2.3f, 7.7f, 0.85f)), Style.White);
        public static readonly Shape Plus = Solid("Plus", 10f, 10f, 0f, Union(Box(1f, 4f, 8f, 2f, 0.3f), Box(4f, 1f, 2f, 8f, 0.3f)), Style.White);
        public static readonly Shape Minus = Solid("Minus", 10f, 10f, 0f, Box(1f, 4f, 8f, 2f, 0.3f), Style.White);
        // A 14 unit square with 2 unit corners, turned 45 degrees, with a dark ring and a light halo around it.
        public static readonly Shape Handle = new("Handle", 26f, 26f, 0f, Layers(
            (Subtract(Grow(Diamond(13f, 13f, 7f, 2f), 3.5f), Diamond(13f, 13f, 7f, 2f)), Style.HandleHalo),
            (Subtract(Grow(Diamond(13f, 13f, 7f, 2f), 2f), Diamond(13f, 13f, 7f, 2f)), Style.HandleRing),
            (Diamond(13f, 13f, 7f, 2f), Style.Handle)));
        public static readonly Shape Arrow = Solid("Arrow", 7f, 10f, 0f, Triangle(0f, 0f, 7f, 5f, 0f, 10f), Style.White);
        public static readonly Shape Back = new("Back", 34f, 34f, 0f, Layers((Circle(17f, 17f, 17f), Style.Light),
            (Union(Line(19.2f, 12.2f, 14.6f, 17f, 1.15f), Line(14.6f, 17f, 19.2f, 21.8f, 1.15f)), Style.BackInk)));
        public static readonly Shape Chevron = Solid("Chevron", 8f, 12f, 0f, Union(Line(2.2f, 1.8f, 6f, 6f, 1.1f), Line(6f, 6f, 2.2f, 10.2f, 1.1f)), Style.White);
        // The stepper's buttons: the Back button's chevron on a faint 22 unit disc.
        public static readonly Shape StepLeft = new("StepLeft", Style.StepSize, Style.StepSize, 0f, Layers((Circle(11f, 11f, 11f), Style.StepFill),
            (Union(Line(12.4f, 7.9f, 9.4f, 11f, 0.9f), Line(9.4f, 11f, 12.4f, 14.1f, 0.9f)), Style.Text)));
        public static readonly Shape StepRight = new("StepRight", Style.StepSize, Style.StepSize, 0f, Layers((Circle(11f, 11f, 11f), Style.StepFill),
            (Union(Line(9.6f, 7.9f, 12.6f, 11f, 0.9f), Line(12.6f, 11f, 9.6f, 14.1f, 0.9f)), Style.Text)));
        public static readonly Shape Button = Solid("Button", Style.ButtonWidth, Style.ButtonHeight, 0f,
            Box(0f, 0f, Style.ButtonWidth, Style.ButtonHeight, Style.ButtonHeight / 2f), Style.Light);
        public static readonly Shape PanelFill = new("PanelFill", 1f, 64f, 0f, (_, y) => Style.Lerp(Style.PanelTop, Style.PanelBottom, y / 64f));
        // box-shadow 8px 0 30px: the panel's edge offset by 8 and blurred with a standard deviation of 15.
        public static readonly Shape PanelShadow = new("PanelShadow", Style.PanelShadowWidth, 1f, 0f,
            (x, _) => Style.WithAlpha(Style.PanelShadow, Style.PanelShadow.a * (1f - Normal((x - 8f) / 15f))));

        // The focus point's window: a bracket in each corner, drawn 1 unit inside the edge so the shadow around it fits.
        // The arms end short of the middle, which is the part that stretches.
        public static readonly Shape FocusWindow = Shadowed("FocusWindow", 2f * FocusCorner + Stretch, FocusCorner, Subtract(
            Ring(Box(1f, 1f, 2f * FocusCorner + Stretch - 2f, 2f * FocusCorner + Stretch - 2f, 3f), 2f),
            Union(Box(FocusCorner - 3f, 0f, Stretch + 6f, 2f * FocusCorner + Stretch, 0f),
                Box(0f, FocusCorner - 3f, 2f * FocusCorner + Stretch, Stretch + 6f, 0f))));
        public static readonly Shape FocusCross = Shadowed("FocusCross", 14f, 0f, Union(Box(2f, 6f, 10f, 2f, 1f), Box(6f, 2f, 2f, 10f, 1f)));

        public static readonly Shape HueRail = Gradient("HueRail", t => Colors.Hsv(t * 360f, 1f, 1f));
        public static readonly Shape TemperatureRail = Gradient("TemperatureRail", t => Colors.Kelvin(1000f + t * 11000f));

        public static readonly Shape[] All =
        {
            Face, Bumper, Trigger, Menu, View, DpadUp, DpadDown, DpadLeft, DpadRight, DpadSides, DpadUpDown, Key, Toast, Pill, Bar,
            FovTrack, SwitchOff, SwitchOn, Knob, Check, Cross, Plus, Minus, Handle, Arrow, Back, Chevron, StepLeft, StepRight, Button,
            PanelFill, PanelShadow, FocusWindow, FocusCross, HueRail, TemperatureRail,
        };

        private static readonly Dictionary<Shape, Sprite> sprites = new();

        // False once Sprite.Create with a border has failed; the shapes are then drawn unsliced.
        public static bool Sliced { get; private set; } = true;

        public static Sprite Get(Shape shape)
        {
            if (sprites.TryGetValue(shape, out var sprite) && sprite)
                return sprite;
            sprite = Create(shape);
            sprites[shape] = sprite;
            return sprite;
        }

        // Draws every shape that has no live sprite yet.
        public static void Load()
        {
            float started = Time.realtimeSinceStartup;
            int drawn = 0;
            foreach (var shape in All)
            {
                if (sprites.TryGetValue(shape, out var sprite) && sprite)
                    continue;
                Get(shape);
                drawn++;
            }
            if (drawn > 0)
                CameraTools.LogOnce($"UI: drew {drawn} shapes in {(Time.realtimeSinceStartup - started) * 1000f:0} ms"
                    + (Sliced ? "." : ", unsliced."));
        }

        // RGBA bytes, bottom row first as Texture2D stores them.
        public static byte[] Raster(Shape shape, out int width, out int height)
        {
            width = (int)MathF.Round(shape.Width * Density);
            height = (int)MathF.Round(shape.Height * Density);
            var bytes = new byte[width * height * 4];
            for (int row = 0; row < height; row++)
            {
                float y = shape.Height - (row + 0.5f) / Density;
                for (int column = 0; column < width; column++)
                {
                    var color = shape.Pixel((column + 0.5f) / Density, y);
                    int at = (row * width + column) * 4;
                    bytes[at] = Byte(color.r);
                    bytes[at + 1] = Byte(color.g);
                    bytes[at + 2] = Byte(color.b);
                    bytes[at + 3] = Byte(color.a);
                }
            }
            return bytes;
        }

        private static Sprite Create(Shape shape)
        {
            var bytes = Raster(shape, out int width, out int height);
            var pixels = new Il2CppStructArray<Color32>(width * height);
            for (int i = 0; i < pixels.Length; i++)
                pixels[i] = new Color32 { r = bytes[i * 4], g = bytes[i * 4 + 1], b = bytes[i * 4 + 2], a = bytes[i * 4 + 3] };
            var texture = new Texture2D(width, height, TextureFormat.RGBA32, true)
            {
                name = "CameraTools " + shape.Name,
                hideFlags = HideFlags.DontSave,
                filterMode = FilterMode.Trilinear,
                wrapMode = TextureWrapMode.Clamp,
            };
            texture.SetPixels32(pixels);
            texture.Apply();

            var rect = new Rect { m_XMin = 0f, m_YMin = 0f, m_Width = width, m_Height = height };
            var pivot = new Vector2 { x = 0.5f, y = 0.5f };
            // Pixels per unit relative to the canvas scaler's 100 reference pixels per unit, so a border in texture pixels
            // is Density times its size in canvas units.
            const float pixelsPerUnit = 100f * Density;
            Sprite sprite = null;
            if (shape.Border > 0f && Sliced)
            {
                float border = shape.Border * Density;
                try
                {
                    sprite = UnityEngine.Sprite.Create(texture, rect, pivot, pixelsPerUnit, 0, SpriteMeshType.FullRect,
                        new Vector4 { x = border, y = border, z = border, w = border });
                }
                catch (Exception e)
                {
                    Sliced = false;
                    CameraTools.LogOnce($"UI: Sprite.Create with a border failed, so CameraTools stretches its shapes unsliced: {e}");
                }
            }
            sprite ??= UnityEngine.Sprite.Create(texture, rect, pivot, pixelsPerUnit, 0, SpriteMeshType.FullRect);
            sprite.name = texture.name;
            sprite.hideFlags = HideFlags.DontSave;
            return sprite;
        }

        private static byte Byte(float value) => (byte)MathF.Round(Math.Clamp(value, 0f, 1f) * 255f);

        // A signed distance in units at a point measured from the top left: negative inside, positive outside.
        private delegate float Field(float x, float y);

        private static Shape Solid(string name, float width, float height, float border, Field field, Color color)
            => new(name, width, height, border, Layers((field, color)));

        // A light square mark with a dark shadow 1 unit wide all around it, so it reads on bright and on dark scenes.
        private static Shape Shadowed(string name, float size, float border, Field mark)
            => new(name, size, size, border, Layers((Grow(mark, 1f), Style.FocusShadow), (mark, Style.Text)));

        // A slider rail as wide as the slider, coloured from left to right. It is not sliced, because the colours run along
        // its whole length; the slider stretches it by nothing.
        private static Shape Gradient(string name, Func<float, System.Numerics.Vector3> along)
        {
            const float width = Style.RowWidth, height = Style.RailHeight;
            var outline = Box(0f, 0f, width, height, height / 2f);
            return new(name, width, height, 0f, (x, y) =>
            {
                var rgb = along(x / width);
                return new Color { r = rgb.X, g = rgb.Y, b = rgb.Z, a = Math.Clamp(0.5f - outline(x, y) * Density, 0f, 1f) };
            });
        }

        // A rounded rectangle filling the whole shape, with a ring inside its edge.
        private static Shape Framed(string name, float width, float height, float radius, float border, float ring, Color fill,
            Color edge)
        {
            var outline = Box(0f, 0f, width, height, radius);
            return new(name, width, height, border, Layers((outline, fill), (Ring(outline, ring), edge)));
        }

        // A controller glyph: a dark plate with a faint ring inside its edge, and white marks on it.
        private static Shape Glyph(string name, float width, float height, Field plate, params (Field, Color)[] marks)
            => new(name, width, height, 0f, Layers(new[] { (plate, Style.GlyphFill), (Ring(plate, 2f), Style.GlyphRing) }.Concat(marks).ToArray()));

        private static Shape Dpad(string name, Field arm)
            => Glyph(name, 24f, 24f, Circle(12f, 12f, 12f),
                (Union(Box(10f, 5f, 4f, 14f, 1f), Box(5f, 10f, 14f, 4f, 1f)), Style.DpadRest), (arm, Style.White));

        // Paints each field's color over the ones before it, anti-aliased across one texture pixel. Transparent pixels keep
        // the first color, so filtering does not pull dark fringes in from outside the shape.
        private static Func<float, float, Color> Layers(params (Field Field, Color Color)[] layers) => (x, y) =>
        {
            float r = 0f, g = 0f, b = 0f, a = 0f;
            foreach (var (field, color) in layers)
            {
                float alpha = color.a * Math.Clamp(0.5f - field(x, y) * Density, 0f, 1f);
                r = color.r * alpha + r * (1f - alpha);
                g = color.g * alpha + g * (1f - alpha);
                b = color.b * alpha + b * (1f - alpha);
                a = alpha + a * (1f - alpha);
            }
            var first = layers[0].Color;
            return a > 0f ? new Color { r = r / a, g = g / a, b = b / a, a = a } : Style.WithAlpha(first, 0f);
        };

        private static Field Circle(float cx, float cy, float radius)
            => (x, y) => MathF.Sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) - radius;

        // A rectangle with its top left at (left, top) and corner radii from the top left clockwise.
        private static Field Box(float left, float top, float width, float height, float radius)
            => Box(left, top, width, height, radius, radius, radius, radius);

        private static Field Box(float left, float top, float width, float height, float topLeft, float topRight,
            float bottomRight, float bottomLeft)
        {
            float cx = left + width / 2f, cy = top + height / 2f, hw = width / 2f, hh = height / 2f;
            return (x, y) =>
            {
                float px = x - cx, py = y - cy;
                float radius = px < 0f ? py < 0f ? topLeft : bottomLeft : py < 0f ? topRight : bottomRight;
                return Rounded(MathF.Abs(px), MathF.Abs(py), hw, hh, radius);
            };
        }

        // A square of half size half, corners rounded by radius, turned 45 degrees about (cx, cy).
        private static Field Diamond(float cx, float cy, float half, float radius)
        {
            const float Cos = 0.70710678f;
            return (x, y) =>
            {
                float px = x - cx, py = y - cy;
                return Rounded(MathF.Abs((px + py) * Cos), MathF.Abs((py - px) * Cos), half, half, radius);
            };
        }

        private static float Rounded(float px, float py, float hw, float hh, float radius)
        {
            float qx = px - hw + radius, qy = py - hh + radius;
            float outside = MathF.Sqrt(MathF.Max(qx, 0f) * MathF.Max(qx, 0f) + MathF.Max(qy, 0f) * MathF.Max(qy, 0f));
            return outside + MathF.Min(MathF.Max(qx, qy), 0f) - radius;
        }

        // A segment from (x1, y1) to (x2, y2) with round ends, half as thick as its width.
        private static Field Line(float x1, float y1, float x2, float y2, float halfWidth)
            => (x, y) =>
            {
                float dx = x2 - x1, dy = y2 - y1;
                float t = Math.Clamp(((x - x1) * dx + (y - y1) * dy) / (dx * dx + dy * dy), 0f, 1f);
                float ex = x - x1 - dx * t, ey = y - y1 - dy * t;
                return MathF.Sqrt(ex * ex + ey * ey) - halfWidth;
            };

        // Inside all three edges; exact along the edges, which is where the anti-aliasing reads it.
        private static Field Triangle(float x1, float y1, float x2, float y2, float x3, float y3)
        {
            var edges = new[] { Edge(x1, y1, x2, y2, x3, y3), Edge(x2, y2, x3, y3, x1, y1), Edge(x3, y3, x1, y1, x2, y2) };
            return (x, y) => edges.Max(edge => edge(x, y));
        }

        // The distance from the line through a and b, negative on the side of c.
        private static Field Edge(float ax, float ay, float bx, float by, float cx, float cy)
        {
            float nx = by - ay, ny = ax - bx, length = MathF.Sqrt(nx * nx + ny * ny);
            float sign = (cx - ax) * nx + (cy - ay) * ny > 0f ? -1f : 1f;
            return (x, y) => sign * ((x - ax) * nx + (y - ay) * ny) / length;
        }

        private static Field Union(params Field[] fields) => (x, y) => fields.Min(field => field(x, y));

        private static Field Subtract(Field shape, Field cut) => (x, y) => MathF.Max(shape(x, y), -cut(x, y));

        private static Field Grow(Field shape, float by) => (x, y) => shape(x, y) - by;

        // The band of the given width just inside the shape's edge, as CSS's inset box-shadow draws it.
        private static Field Ring(Field shape, float width) => (x, y) =>
        {
            float distance = shape(x, y);
            return MathF.Max(distance, -distance - width);
        };

        // The standard normal cumulative distribution, from Abramowitz and Stegun's 7.1.26 approximation of erf.
        private static float Normal(float z)
        {
            float x = MathF.Abs(z) / MathF.Sqrt(2f);
            float t = 1f / (1f + 0.3275911f * x);
            float erf = 1f - ((((1.061405429f * t - 1.453152027f) * t + 1.421413741f) * t - 0.284496736f) * t + 0.254829592f) * t
                * MathF.Exp(-x * x);
            return z >= 0f ? (1f + erf) / 2f : (1f - erf) / 2f;
        }
    }
}
