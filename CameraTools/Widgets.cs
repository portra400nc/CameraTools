using UnityEngine;
using UnityEngine.UI;

namespace CameraTools
{
    // Moves from 0 (hidden) to 1 (shown) by a step per frame; Eased follows CSS's default transition curve.
    internal struct Tween
    {
        public float Value;

        public float Eased => Style.Ease(Value);

        public bool Toward(bool shown, float step)
        {
            float target = shown ? 1f : 0f;
            if (Value == target)
                return false;
            Value = Value < target ? Math.Min(Value + step, target) : Math.Max(Value - step, target);
            return true;
        }
    }

    // Fades a part of the UI in from an offset, and switches it off while it is fully hidden so it neither draws nor
    // catches the pointer.
    internal sealed class Fader
    {
        private readonly RectTransform rect;
        private readonly CanvasGroup group;
        private readonly Vector2 rest;
        private readonly Vector2 away;
        private readonly float duration;
        private Tween tween;
        private bool active = true;

        public Fader(RectTransform rect, Vector2 away, float duration)
        {
            this.rect = rect;
            this.away = away;
            this.duration = duration;
            rest = rect.anchoredPosition;
            group = rect.gameObject.AddComponent<CanvasGroup>();
            Apply();
        }

        public bool Visible => tween.Value > 0f;

        public void Update(bool shown, float deltaTime)
        {
            if (tween.Toward(shown, deltaTime / duration))
                Apply();
        }

        private void Apply()
        {
            float t = tween.Eased;
            group.alpha = t;
            rect.anchoredPosition = new Vector2 { x = rest.x + away.x * (1f - t), y = rest.y + away.y * (1f - t) };
            if (active == Visible)
                return;
            active = Visible;
            rect.gameObject.SetActive(active);
        }
    }

    internal sealed record HintView(GameObject Root, Text Label);

    // A section header, or a setting's row with its selection band and arrow.
    internal class RowView
    {
        public Row Row;
        public RectTransform Rect;

        public virtual void Select(bool selected)
        {
        }

        // step: how far a switch slides this frame, 1 to jump.
        public virtual void Sync(float step)
        {
        }
    }

    internal abstract class ItemView : RowView
    {
        public GameObject Band;
        public GameObject Arrow;

        public override void Select(bool selected)
        {
            Band.SetActive(selected);
            Arrow.SetActive(selected);
        }
    }

    internal sealed class SliderView : ItemView
    {
        public RectTransform Track;
        public RectTransform Fill;
        public RectTransform Handle;
        public Text Value;
        private float shown = float.NaN;
        private string text;

        // Hotkeys can push game speed past the row's range; the bar stops at its end and the value shows the real one.
        public override void Sync(float step)
        {
            var slider = (SliderRow)Row;
            float value = slider.Get();
            if (value == shown)
                return;
            shown = value;
            float t = Math.Clamp((value - slider.Min) / (slider.Max - slider.Min), 0f, 1f);
            Fill.anchorMax = new Vector2 { x = t, y = 0.5f };
            Handle.anchorMin = new Vector2 { x = t, y = 0.5f };
            Handle.anchorMax = new Vector2 { x = t, y = 0.5f };
            string formatted = value.ToString(slider.Format);
            if (formatted == text)
                return;
            Value.text = formatted;
            text = formatted;
        }
    }

    internal sealed class ToggleView : ItemView
    {
        public RectTransform Knob;
        public Image On;
        public Image Mark;
        private Tween tween;
        private bool? marked;

        public override void Sync(float step)
        {
            bool on = ((ToggleRow)Row).Get();
            if (marked != on)
            {
                Mark.sprite = Shapes.Get(on ? Shapes.Check : Shapes.Cross);
                marked = on;
            }
            if (!tween.Toward(on, step))
                return;
            float t = tween.Eased;
            Knob.anchoredPosition = new Vector2 { x = Style.KnobInset + (Style.SwitchWidth - Style.KnobSize - 2f * Style.KnobInset) * t };
            On.color = Style.WithAlpha(Style.White, t);
            Mark.color = Style.Lerp(Style.KnobInkOff, Style.KnobInkOn, t);
        }
    }

    internal sealed class TabView
    {
        public RectTransform Button;
        public Text Label;
        public Image Line;
        public GameObject Rows;
        public readonly List<RowView> RowViews = new();
        private Tween tween;
        private bool drawn;

        public void Show(bool selected, float step)
        {
            if (!tween.Toward(selected, step) && drawn)
                return;
            drawn = true;
            float t = tween.Eased;
            Label.color = Style.Lerp(Style.Dim, Style.Text, t);
            Line.color = Style.WithAlpha(Style.Cream, t);
            Line.rectTransform.localScale = new Vector3 { x = Style.LineRestScale + (1f - Style.LineRestScale) * t, y = 1f, z = 1f };
        }
    }

    // Builds each kind of widget from the style table, CameraTools' shapes, and the game's font. Positions follow the
    // mockup: from the top left of the parent unless a widget is anchored to another edge.
    internal sealed class Builder
    {
        private sealed record GlyphLook(Shape Shape, string Text = null, Color Ink = default, int Size = 0);

        private static readonly Dictionary<PadButtons, GlyphLook> Glyphs = new()
        {
            [PadButtons.A] = new(Shapes.Face, "A", Style.XboxA, Style.FaceSize),
            [PadButtons.B] = new(Shapes.Face, "B", Style.XboxB, Style.FaceSize),
            [PadButtons.X] = new(Shapes.Face, "X", Style.XboxX, Style.FaceSize),
            [PadButtons.Y] = new(Shapes.Face, "Y", Style.XboxY, Style.FaceSize),
            [PadButtons.L3] = new(Shapes.Face, "L3", Style.White, Style.PillSize),
            [PadButtons.R3] = new(Shapes.Face, "R3", Style.White, Style.PillSize),
            [PadButtons.LB] = new(Shapes.Bumper, "LB", Style.LightInk, Style.PillSize),
            [PadButtons.RB] = new(Shapes.Bumper, "RB", Style.LightInk, Style.PillSize),
            [PadButtons.LT] = new(Shapes.Trigger, "LT", Style.LightInk, Style.PillSize),
            [PadButtons.RT] = new(Shapes.Trigger, "RT", Style.LightInk, Style.PillSize),
            [PadButtons.Start] = new(Shapes.Menu),
            [PadButtons.Back] = new(Shapes.View),
            [PadButtons.DpadUp] = new(Shapes.DpadUp),
            [PadButtons.DpadDown] = new(Shapes.DpadDown),
            [PadButtons.DpadLeft] = new(Shapes.DpadLeft),
            [PadButtons.DpadRight] = new(Shapes.DpadRight),
        };

        private static readonly Dictionary<KeyCode, string> KeyNames = new()
        {
            [KeyCode.Comma] = ",",
            [KeyCode.Period] = ".",
            [KeyCode.Semicolon] = ";",
            [KeyCode.Quote] = "'",
            [KeyCode.LeftBracket] = "[",
            [KeyCode.RightBracket] = "]",
            [KeyCode.Equals] = "=",
            [KeyCode.Minus] = "-",
            [KeyCode.PageDown] = "Page Down",
            [KeyCode.PageUp] = "Page Up",
            [KeyCode.RightShift] = "Right Shift",
            [KeyCode.RightAlt] = "Right Alt",
            [KeyCode.UpArrow] = "Up",
            [KeyCode.DownArrow] = "Down",
            [KeyCode.LeftArrow] = "Left",
            [KeyCode.RightArrow] = "Right",
        };

        private readonly Font font;

        public Builder(Font font)
        {
            this.font = font;
        }

        public static RectTransform Node(string name, Transform parent)
        {
            var rect = new GameObject(name).AddComponent<RectTransform>();
            rect.SetParent(parent, false);
            return rect;
        }

        public static void Place(RectTransform rect, Vector2 anchorMin, Vector2 anchorMax, Vector2 pivot, Vector2 position, Vector2 size)
        {
            rect.anchorMin = anchorMin;
            rect.anchorMax = anchorMax;
            rect.pivot = pivot;
            rect.anchoredPosition = position;
            rect.sizeDelta = size;
        }

        // Anchored and pivoted at the same point of the parent, such as (1, 0) for its bottom right corner.
        public static void Pin(RectTransform rect, float anchorX, float anchorY, float x, float y, float width, float height)
            => Place(rect, V(anchorX, anchorY), V(anchorX, anchorY), V(anchorX, anchorY), V(x, y), V(width, height));

        public static void TopLeft(RectTransform rect, float x, float y, float width, float height) => Pin(rect, 0f, 1f, x, -y, width, height);

        public static void Fill(RectTransform rect) => Place(rect, V(0f, 0f), V(1f, 1f), V(0.5f, 0.5f), V(0f, 0f), V(0f, 0f));

        // Unity's struct constructors are IL2CPP calls; filling the fields is not.
        public static Vector2 V(float x, float y) => new() { x = x, y = y };

        // A shape, or a plain rectangle without one.
        public static Image Picture(Transform parent, string name, Shape shape, Color color)
        {
            var image = Node(name, parent).gameObject.AddComponent<Image>();
            if (shape != null)
            {
                image.sprite = Shapes.Get(shape);
                image.type = shape.Border > 0f && Shapes.Sliced ? Image.Type.Sliced : Image.Type.Simple;
                image.rectTransform.sizeDelta = V(shape.Width, shape.Height);
            }
            image.color = color;
            return image;
        }

        public Text Label(Transform parent, string name, string text, int size, Color color, TextAnchor alignment)
        {
            var label = Node(name, parent).gameObject.AddComponent<Text>();
            label.font = font;
            label.fontSize = size;
            label.fontStyle = FontStyle.Bold;
            label.color = color;
            label.alignment = alignment;
            label.horizontalOverflow = HorizontalWrapMode.Overflow;
            label.verticalOverflow = VerticalWrapMode.Overflow;
            label.supportRichText = false;
            label.text = text;
            return label;
        }

        // CSS's text-shadow blurs; uGUI's Shadow is a sharp copy one unit down.
        public static void DropShadow(Graphic graphic, Color color)
        {
            var shadow = graphic.gameObject.AddComponent<Shadow>();
            shadow.effectColor = color;
            shadow.effectDistance = V(0f, -1f);
        }

        // Lays children out left to right at their preferred widths, leaving their heights alone.
        public static HorizontalLayoutGroup Flow(GameObject target, float spacing, TextAnchor alignment)
        {
            var group = target.AddComponent<HorizontalLayoutGroup>();
            group.spacing = spacing;
            group.childAlignment = alignment;
            group.childControlWidth = true;
            group.childControlHeight = false;
            group.childForceExpandWidth = false;
            group.childForceExpandHeight = false;
            return group;
        }

        public static bool HasGlyph(PadButtons button) => Glyphs.ContainsKey(button);

        public RectTransform Glyph(Transform parent, PadButtons button)
        {
            var look = Glyphs[button];
            var rect = Picture(parent, button.ToString(), look.Shape, Style.White).rectTransform;
            var element = rect.gameObject.AddComponent<LayoutElement>();
            element.preferredWidth = look.Shape.Width;
            element.preferredHeight = look.Shape.Height;
            if (look.Text != null)
                Fill(Label(rect, "Text", look.Text, look.Size, look.Ink, TextAnchor.MiddleCenter).rectTransform);
            return rect;
        }

        public RectTransform Keycap(Transform parent, string name)
        {
            var image = Picture(parent, "Key", Shapes.Key, Style.White);
            var rect = image.rectTransform;
            rect.sizeDelta = V(Style.KeyMinWidth, Style.KeyHeight);
            var group = Flow(image.gameObject, 0f, TextAnchor.MiddleCenter);
            group.padding.left = Style.KeyPadding;
            group.padding.right = Style.KeyPadding;
            image.gameObject.AddComponent<LayoutElement>().minWidth = Style.KeyMinWidth;
            Label(rect, "Text", name, Style.KeySize, Style.Text, TextAnchor.MiddleCenter).rectTransform.sizeDelta = V(0f, Style.KeyHeight);
            return rect;
        }

        // A glyph for a binding that is one button, or the binding spelled out before the label.
        public HintView PadHint(Transform parent, PadBinding binding, string label)
        {
            var button = GlyphButton(binding);
            return HasGlyph(button) ? Hint(parent, rect => Glyph(rect, button), label) : Hint(parent, null, $"{binding}  {label}");
        }

        public HintView KeyHint(Transform parent, KeyCode key, string label) => Hint(parent, rect => Keycap(rect, KeyName(key)), label);

        private HintView Hint(Transform parent, Action<Transform> mark, string label)
        {
            var rect = Node("Hint", parent);
            rect.sizeDelta = V(0f, Style.HintHeight);
            Flow(rect.gameObject, Style.HintGap, TextAnchor.MiddleLeft);
            mark?.Invoke(rect);
            var text = Label(rect, "Label", label, Style.HintSize, Style.Text, TextAnchor.MiddleLeft);
            text.rectTransform.sizeDelta = V(0f, Style.HintHeight);
            DropShadow(text, Style.TextShadow);
            return new HintView(rect.gameObject, text);
        }

        public static RectTransform HintRow(Transform parent, string name, TextAnchor alignment)
        {
            var rect = Node(name, parent);
            Flow(rect.gameObject, Style.LegendGap, alignment);
            return rect;
        }

        public RowView Section(Transform parent, Section section, float y)
        {
            var rect = Node(section.Label, parent);
            TopLeft(rect, 0f, y, Style.PanelWidth, Style.SectionHeight);
            var text = Label(rect, "Label", section.Label, Style.SectionSize, Style.Cream, TextAnchor.MiddleLeft);
            TopLeft(text.rectTransform, Style.SectionX, Style.SectionTextY, Style.PanelWidth - 2f * Style.SectionX, Style.SectionTextHeight);
            return new RowView { Row = section, Rect = rect };
        }

        public SliderView Slider(Transform parent, SliderRow row, float y)
        {
            var view = new SliderView();
            var rect = Item(parent, view, row, y, Style.SliderRowHeight);
            var label = Label(rect, "Label", row.Label, Style.RowSize, Style.Text, TextAnchor.LowerLeft);
            TopLeft(label.rectTransform, Style.RowLeft, Style.RowTextY, Style.RowWidth, Style.RowTextHeight);
            view.Value = Label(rect, "Value", "", Style.ValueSize, Style.Dim, TextAnchor.LowerRight);
            TopLeft(view.Value.rectTransform, Style.RowLeft, Style.RowTextY, Style.RowWidth, Style.RowTextHeight);
            view.Track = Node("Slider", rect);
            TopLeft(view.Track, Style.RowLeft, Style.SliderY, Style.RowWidth, Style.SliderHeight);
            var rail = Picture(view.Track, "Rail", Shapes.Bar, Style.Track).rectTransform;
            Place(rail, V(0f, 0.5f), V(1f, 0.5f), V(0f, 0.5f), V(0f, 0f), V(0f, Style.RailHeight));
            view.Fill = Picture(view.Track, "Fill", Shapes.Bar, Style.Cream).rectTransform;
            Place(view.Fill, V(0f, 0.5f), V(0f, 0.5f), V(0f, 0.5f), V(0f, 0f), V(0f, Style.RailHeight));
            view.Handle = Picture(view.Track, "Handle", Shapes.Handle, Style.White).rectTransform;
            Place(view.Handle, V(0f, 0.5f), V(0f, 0.5f), V(0.5f, 0.5f), V(0f, 0f), V(Shapes.Handle.Width, Shapes.Handle.Height));
            view.Sync(1f);
            return view;
        }

        public ToggleView Toggle(Transform parent, ToggleRow row, float y)
        {
            var view = new ToggleView();
            var rect = Item(parent, view, row, y, Style.ToggleRowHeight);
            var label = Label(rect, "Label", row.Label, Style.RowSize, Style.Text, TextAnchor.MiddleLeft);
            TopLeft(label.rectTransform, Style.RowLeft, 0f, Style.RowWidth - Style.SwitchWidth, Style.ToggleRowHeight);
            var track = Picture(rect, "Switch", Shapes.SwitchOff, Style.White).rectTransform;
            Pin(track, 1f, 1f, -Style.RowRight, -(Style.ToggleRowHeight - Style.SwitchHeight) / 2f, Style.SwitchWidth, Style.SwitchHeight);
            view.On = Picture(track, "On", Shapes.SwitchOn, Style.WithAlpha(Style.White, 0f));
            Fill(view.On.rectTransform);
            view.Knob = Picture(track, "Knob", Shapes.Knob, Style.White).rectTransform;
            Place(view.Knob, V(0f, 0.5f), V(0f, 0.5f), V(0f, 0.5f), V(Style.KnobInset, 0f), V(Style.KnobSize, Style.KnobSize));
            view.Mark = Picture(view.Knob, "Mark", Shapes.Cross, Style.KnobInkOff);
            Pin(view.Mark.rectTransform, 0.5f, 0.5f, 0f, 0f, Style.MarkSize, Style.MarkSize);
            view.Sync(1f);
            return view;
        }

        // The band and arrow that show which row is selected, under the row's own parts.
        private static RectTransform Item(Transform parent, ItemView view, Row row, float y, float height)
        {
            var rect = Node(row.Label, parent);
            TopLeft(rect, 0f, y, Style.PanelWidth, height);
            var band = Picture(rect, "Band", null, Style.Band);
            Fill(band.rectTransform);
            var arrow = Picture(rect, "Arrow", Shapes.Arrow, Style.Text);
            TopLeft(arrow.rectTransform, Style.ArrowX, Style.ArrowY, Shapes.Arrow.Width, Shapes.Arrow.Height);
            view.Row = row;
            view.Rect = rect;
            view.Band = band.gameObject;
            view.Arrow = arrow.gameObject;
            view.Select(false);
            return rect;
        }

        // A text tab with an underline that grows in from 40% of its width when the tab is selected.
        public TabView Tab(Transform strip, string name)
        {
            var rect = Node(name, strip);
            rect.sizeDelta = V(0f, Style.TabHeight);
            var group = Flow(rect.gameObject, 0f, TextAnchor.UpperCenter);
            group.padding.left = Style.TabPadX;
            group.padding.right = Style.TabPadX;
            group.padding.top = Style.TabPadTop;
            group.padding.bottom = Style.TabPadBottom;
            var label = Label(rect, "Label", name, Style.TabSize, Style.Dim, TextAnchor.MiddleCenter);
            label.rectTransform.sizeDelta = V(0f, Style.TabSize);
            var line = Picture(rect, "Line", Shapes.Bar, Style.Cream);
            line.gameObject.AddComponent<LayoutElement>().ignoreLayout = true;
            Place(line.rectTransform, V(0f, 0f), V(1f, 0f), V(0.5f, 0f), V(0f, Style.LineBottom), V(-2f * Style.LineInset, Style.LineHeight));
            return new TabView { Button = rect, Label = label, Line = line };
        }

        private static PadButtons GlyphButton(PadBinding pad) => pad.Axis switch
        {
            PadAxis.LT => PadButtons.LT,
            PadAxis.RT => PadButtons.RT,
            PadAxis.None when System.Numerics.BitOperations.IsPow2((uint)pad.Chord) => pad.Chord,
            _ => PadButtons.None,
        };

        private static string KeyName(KeyCode key)
            => KeyNames.TryGetValue(key, out var name) ? name
                : key is >= KeyCode.Alpha0 and <= KeyCode.Alpha9 ? ((int)(key - KeyCode.Alpha0)).ToString()
                : key.ToString();
    }
}
