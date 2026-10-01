using UnityEngine;

namespace CameraTools
{
    // The approved mockup's tokens. Sizes are canvas units, which equal mockup pixels on the 1280x800 reference canvas;
    // positions are measured from the top left of their parent, as in the mockup's CSS.
    internal static class Style
    {
        public static readonly Color Text = Hex(0xece5d8);
        public static readonly Color Dim = Hex(0xece5d8, 0.62f);
        public static readonly Color Cream = Hex(0xd3bc8e);
        public static readonly Color Track = Hex(0x343e50);
        public static readonly Color Band = Hex(0xece5d8, 0.10f);
        public static readonly Color PanelTop = Hex(0x222835, 0.90f);
        public static readonly Color PanelBottom = Hex(0x2c3342, 0.90f);
        public static readonly Color PanelEdge = Hex(0xd3bc8e, 0.18f);
        public static readonly Color PanelShadow = Hex(0x000000, 0.25f);
        public static readonly Color TabBar = Hex(0x12151c, 0.55f);
        public static readonly Color ToastFill = Hex(0x181c25, 0.78f);
        public static readonly Color ToastRing = Hex(0xd3bc8e, 0.35f);
        public static readonly Color TextShadow = Hex(0x000000, 0.35f);
        public static readonly Color GlyphFill = Hex(0x1c2029);
        public static readonly Color GlyphRing = Hex(0xffffff, 0.18f);
        public static readonly Color Light = Hex(0xeae6de);
        public static readonly Color LightInk = Hex(0x22262e);
        public static readonly Color BackInk = Hex(0x2a2f3a);
        public static readonly Color KeyFill = Hex(0x1c202a, 0.55f);
        public static readonly Color XboxA = Hex(0x6cbf3f);
        public static readonly Color XboxB = Hex(0xe0463c);
        public static readonly Color XboxX = Hex(0x3c8ee0);
        public static readonly Color XboxY = Hex(0xe7b52e);
        public static readonly Color White = Hex(0xffffff);
        public static readonly Color DpadRest = Hex(0xffffff, 0.35f);
        public static readonly Color Handle = Hex(0xf4efe4);
        public static readonly Color HandleRing = Hex(0x222835, 0.90f);
        public static readonly Color HandleHalo = Hex(0xf4efe4, 0.70f);
        public static readonly Color FovTrack = Hex(0x141820, 0.60f);
        public static readonly Color FovRing = Hex(0xffffff, 0.12f);
        public static readonly Color FovEndShadow = Hex(0x000000, 0.60f);
        public static readonly Color SwitchRingOff = Hex(0xece5d8, 0.35f);
        public static readonly Color SwitchRingOn = Hex(0xffffff, 0.40f);
        public static readonly Color KnobInkOff = Hex(0x7a7f8c);
        public static readonly Color KnobInkOn = Hex(0x9b7d45);
        public static readonly Color StepFill = Hex(0xece5d8, 0.12f);
        public static readonly Color Thumb = Hex(0xd3bc8e, 0.55f);
        public static readonly Color Tick = Hex(0xece5d8, 0.70f);

        public const float Margin = 44f;
        public const float TopLegendY = 28f;
        public const float LegendGap = 26f;
        public const float HintGap = 8f;
        public const float HintHeight = 24f;
        public const int HintSize = 14;
        public const float ToastY = 68f;
        public const float ToastHeight = 40f;
        public const int ToastPadding = 26;
        public const int ToastSize = 15;

        public const float FovX = 44f;
        public const float FovWidth = 22f;
        public const float FovHeight = 260f;
        public const float FovInset = 24f;
        public const float FovEndY = 7f;

        public const float PanelWidth = 440f;
        public const float PanelSlide = 16f;
        public const float PanelShadowWidth = 56f;
        public const float TabBarHeight = 64f;
        public const float TabBarPadding = 16f;
        // The mockup's spacing. Tabs that do not fit between the LB and RB glyphs scroll sideways.
        public const float TabGap = 4f;
        public const float TabClipGap = 8f;
        // How much past the selected tab comes into view with it, so the next tab shows that the strip scrolls.
        public const float TabPeek = 28f;
        // Wider than every tab together, so the strip's layout never squeezes one.
        public const float StripWidth = 1600f;
        public const float TabSlide = 0.08f;
        public const float TabsAfterBack = 72f;
        public const int TabPadX = 10;
        public const int TabPadTop = 10;
        public const int TabPadBottom = 12;
        public const float TabHeight = 39f;
        public const int TabSize = 17;
        public const float LineInset = 10f;
        public const float LineBottom = 2f;
        public const float LineHeight = 3f;
        public const float LineRestScale = 0.4f;
        public const float BackSize = 34f;

        public const float ListPadding = 14f;
        public const float FooterHeight = 68f;
        public const float FooterBottom = 26f;
        public const float SectionHeight = 40f;
        public const float SectionX = 24f;
        public const float SectionTextY = 10f;
        public const float SectionTextHeight = 22f;
        public const int SectionSize = 15;
        public const float RowLeft = 28f;
        public const float RowRight = 24f;
        public const float RowWidth = PanelWidth - RowLeft - RowRight;
        public const int RowSize = 14;
        public const int ValueSize = 13;
        public const float SliderRowHeight = 67f;
        public const float RowTextY = 9f;
        public const float RowTextHeight = 20f;
        public const float SliderY = 37f;
        public const float SliderHeight = 18f;
        public const float RailHeight = 4f;
        public const float ToggleRowHeight = 50f;
        public const float ArrowX = 10f;
        public const float ArrowY = 16f;
        public const float SwitchWidth = 52f;
        public const float SwitchHeight = 26f;
        public const float KnobInset = 3f;
        public const float KnobSize = 20f;
        public const float MarkSize = 10f;
        // A preset note is a second, smaller line under a row's label.
        public const float NoteY = 30f;
        public const float NoteHeight = 16f;
        public const float NoteExtra = 17f;
        public const int NoteSize = 11;
        public const float ChoiceRowHeight = RowTextY + RowTextHeight + NoteExtra + 8f;
        public const float StepSize = 22f;
        public const float StepGap = 6f;
        public const float ChoiceValueWidth = 92f;
        public const float StepDimmed = 0.3f;
        public const float RowDimmed = 0.4f;
        public const float SlotValueWidth = 110f;
        public const float ButtonWidth = 60f;
        public const float ButtonHeight = 24f;
        public const float ButtonGap = 10f;
        public const int ButtonSize = 12;
        public const float ThumbWidth = 4f;
        public const float ThumbRight = 4f;
        public const float ThumbMin = 24f;
        public const float WheelStep = 60f;

        public const float PlayBarY = 24f;
        public const float PlayBarGap = 18f;
        public const float TickWidth = 2f;
        public const float TickHeight = 10f;
        public const float PlayMetaSlack = 6f;

        public const float ComboGap = 3f;
        public const int ComboSize = 12;
        public const float KeyHeight = 22f;
        public const float KeyMinWidth = 24f;
        public const int KeyPadding = 7;
        public const int KeySize = 11;
        public const int FaceSize = 12;
        public const int PillSize = 10;

        public const float HudFade = 0.2f;
        public const float PanelFade = 0.2f;
        public const float ToastFade = 0.25f;
        public const float ToastTime = 2f;
        public const float TabFade = 0.15f;
        public const float RowsFade = 0.15f;
        public const float SwitchSlide = 0.15f;

        // Unity's Color constructor is an IL2CPP call; filling the fields is not.
        public static Color Hex(uint rgb, float alpha = 1f)
            => new() { r = (rgb >> 16 & 0xff) / 255f, g = (rgb >> 8 & 0xff) / 255f, b = (rgb & 0xff) / 255f, a = alpha };

        public static Color Lerp(Color a, Color b, float t)
            => new() { r = a.r + (b.r - a.r) * t, g = a.g + (b.g - a.g) * t, b = a.b + (b.b - a.b) * t, a = a.a + (b.a - a.a) * t };

        public static Color WithAlpha(Color color, float alpha) => new() { r = color.r, g = color.g, b = color.b, a = alpha };

        // For a <color> tag in rich text.
        public static string Html(Color color) => $"#{Channel(color.r):x2}{Channel(color.g):x2}{Channel(color.b):x2}";

        private static int Channel(float value) => (int)MathF.Round(Math.Clamp(value, 0f, 1f) * 255f);

        // CSS's default transition timing, cubic-bezier(0.25, 0.1, 0.25, 1), solved for x by bisection.
        public static float Ease(float t)
        {
            float low = 0f, high = 1f;
            for (int i = 0; i < 20; i++)
            {
                float mid = (low + high) / 2f;
                if (Bezier(mid, 0.25f, 0.25f) < t)
                    low = mid;
                else
                    high = mid;
            }
            return Bezier((low + high) / 2f, 0.1f, 1f);
        }

        private static float Bezier(float s, float p1, float p2)
        {
            float u = 1f - s;
            return 3f * u * u * s * p1 + 3f * u * s * s * p2 + s * s * s;
        }
    }
}
