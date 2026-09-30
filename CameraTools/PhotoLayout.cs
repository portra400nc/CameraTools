using System.Text.Json;
using UnityEngine;
using UnityEngine.UI;

namespace CameraTools
{
    internal sealed record Box(Vector2 AnchorMin, Vector2 AnchorMax, Vector2 Pivot, Vector2 Position, Vector2 SizeDelta,
        Vector3 Scale, Vector3 Rotation);

    internal sealed record ImageStyle(bool Enabled, string Sprite, Image.Type Type, bool FillCenter, Image.FillMethod FillMethod,
        float FillAmount, bool PreserveAspect, Color Color);

    internal sealed record TextStyle(bool Enabled, string Text, string Font, int FontSize, FontStyle Style, TextAnchor Alignment,
        float LineSpacing, bool RichText, HorizontalWrapMode HorizontalOverflow, VerticalWrapMode VerticalOverflow, bool BestFit,
        Color Color);

    // An Outline, or the Shadow it derives from.
    internal sealed record Effect(bool Outline, bool Enabled, Color Color, Vector2 Distance, bool UseGraphicAlpha);

    internal sealed record LayoutGroupStyle(bool Vertical, bool Enabled, int Left, int Right, int Top, int Bottom, float Spacing,
        TextAnchor ChildAlignment, bool ControlWidth, bool ControlHeight, bool ExpandWidth, bool ExpandHeight);

    internal sealed record LayoutElementStyle(bool Enabled, bool IgnoreLayout, Vector2 Min, Vector2 Preferred, Vector2 Flexible);

    internal sealed record FitterStyle(bool Enabled, ContentSizeFitter.FitMode Horizontal, ContentSizeFitter.FitMode Vertical);

    internal sealed record Sorting(bool Override, int Order);

    internal sealed record Node(string Name, bool Active, Box Box, ImageStyle Image, TextStyle Text, Effect[] Effects,
        LayoutGroupStyle LayoutGroup, LayoutElementStyle LayoutElement, FitterStyle Fitter, float? GroupAlpha, Sorting Canvas,
        Node[] Children)
    {
        public Node Find(string path)
        {
            var node = this;
            foreach (var name in path.Split('/'))
                node = Array.Find(node.Children, child => child.Name == name)
                    ?? throw new InvalidOperationException($"The photo mode layout has no {path} under {Name}.");
            return node;
        }

        // A copy with the node at path replaced by what change makes of it.
        public Node With(string path, Func<Node, Node> change)
        {
            int slash = path.IndexOf('/');
            string name = slash < 0 ? path : path[..slash];
            var child = Find(name);
            var changed = slash < 0 ? change(child) : child.With(path[(slash + 1)..], change);
            return this with { Children = Children.Select(other => ReferenceEquals(other, child) ? changed : other).ToArray() };
        }
    }

    // Genshin's photo mode page as the snapshots recorded it at 1280x800, from PhotoModeLayout.json, which
    // tools/extract_photo_layout.py writes. Sprites and fonts are names, looked up in the running game.
    internal sealed record PhotoLayout(Vector2 ReferenceResolution, CanvasScaler.ScreenMatchMode MatchMode, float Match,
        float ReferencePixelsPerUnit, Box Pages, string[] RequiredSprites, string[] OptionalSprites, string[] Fonts, Node Root)
    {
        private const string Resource = "CameraTools.PhotoModeLayout.json";

        private static PhotoLayout current;

        public static PhotoLayout Current => current ??= Load();

        private static PhotoLayout Load()
        {
            using var stream = typeof(PhotoLayout).Assembly.GetManifestResourceStream(Resource)
                ?? throw new InvalidOperationException($"{Resource} is not embedded.");
            using var document = JsonDocument.Parse(stream);
            var json = document.RootElement;
            var scaler = json.GetProperty("canvasScaler");
            return new PhotoLayout(
                Vec2(scaler, "referenceResolution"),
                Enum<CanvasScaler.ScreenMatchMode>(scaler, "screenMatchMode"),
                Float(scaler, "matchWidthOrHeight"),
                Float(scaler, "referencePixelsPerUnit"),
                ParseBox(json.GetProperty("pages")),
                Strings(json, "requiredSprites"),
                Strings(json, "optionalSprites"),
                Strings(json, "fonts"),
                ParseNode(json.GetProperty("root")));
        }

        private static Node ParseNode(JsonElement json) => new(
            json.GetProperty("name").GetString(),
            Bool(json, "active"),
            ParseBox(json.GetProperty("rect")),
            Optional(json, "image", image => new ImageStyle(
                Bool(image, "enabled"),
                image.GetProperty("sprite").GetString(),
                Enum<Image.Type>(image, "imageType"),
                Bool(image, "fillCenter"),
                Enum<Image.FillMethod>(image, "fillMethod"),
                Float(image, "fillAmount"),
                Bool(image, "preserveAspect"),
                ParseColor(image, "color"))),
            Optional(json, "text", text => new TextStyle(
                Bool(text, "enabled"),
                text.GetProperty("text").GetString(),
                text.GetProperty("font").GetString(),
                text.GetProperty("fontSize").GetInt32(),
                Enum<FontStyle>(text, "fontStyle"),
                Enum<TextAnchor>(text, "alignment"),
                Float(text, "lineSpacing"),
                Bool(text, "richText"),
                Enum<HorizontalWrapMode>(text, "horizontalOverflow"),
                Enum<VerticalWrapMode>(text, "verticalOverflow"),
                Bool(text, "bestFit"),
                ParseColor(text, "color"))),
            json.TryGetProperty("effects", out var effects)
                ? effects.EnumerateArray().Select(effect => new Effect(
                    Bool(effect, "outline"),
                    Bool(effect, "enabled"),
                    ParseColor(effect, "effectColor"),
                    Vec2(effect, "effectDistance"),
                    Bool(effect, "useGraphicAlpha"))).ToArray()
                : Array.Empty<Effect>(),
            Optional(json, "layoutGroup", group =>
            {
                var padding = Floats(group, "padding");
                var control = Bools(group, "childControl");
                var expand = Bools(group, "childForceExpand");
                return new LayoutGroupStyle(Bool(group, "vertical"), Bool(group, "enabled"),
                    (int)padding[0], (int)padding[1], (int)padding[2], (int)padding[3], Float(group, "spacing"),
                    Enum<TextAnchor>(group, "childAlignment"), control[0], control[1], expand[0], expand[1]);
            }),
            Optional(json, "layoutElement", element => new LayoutElementStyle(
                Bool(element, "enabled"),
                Bool(element, "ignoreLayout"),
                Vec2(element, "min"),
                Vec2(element, "preferred"),
                Vec2(element, "flexible"))),
            Optional(json, "fitter", fitter =>
            {
                var fit = fitter.GetProperty("fit").EnumerateArray()
                    .Select(mode => System.Enum.Parse<ContentSizeFitter.FitMode>(mode.GetString())).ToArray();
                return new FitterStyle(Bool(fitter, "enabled"), fit[0], fit[1]);
            }),
            json.TryGetProperty("groupAlpha", out var alpha) ? alpha.GetSingle() : null,
            Optional(json, "canvas", canvas => new Sorting(Bool(canvas, "overrideSorting"), canvas.GetProperty("sortingOrder").GetInt32())),
            json.GetProperty("children").EnumerateArray().Select(ParseNode).ToArray());

        private static Box ParseBox(JsonElement json) => new(
            Vec2(json, "anchorMin"), Vec2(json, "anchorMax"), Vec2(json, "pivot"), Vec2(json, "anchoredPosition"),
            Vec2(json, "sizeDelta"), Vec3(json, "scale"), Vec3(json, "rotation"));

        private static T Optional<T>(JsonElement json, string name, Func<JsonElement, T> parse) where T : class
            => json.TryGetProperty(name, out var value) ? parse(value) : null;

        private static bool Bool(JsonElement json, string name) => json.GetProperty(name).GetBoolean();

        private static float Float(JsonElement json, string name) => json.GetProperty(name).GetSingle();

        private static float[] Floats(JsonElement json, string name)
            => json.GetProperty(name).EnumerateArray().Select(value => value.GetSingle()).ToArray();

        private static bool[] Bools(JsonElement json, string name)
            => json.GetProperty(name).EnumerateArray().Select(value => value.GetBoolean()).ToArray();

        private static string[] Strings(JsonElement json, string name)
            => json.GetProperty(name).EnumerateArray().Select(value => value.GetString()).ToArray();

        private static T Enum<T>(JsonElement json, string name) where T : struct, System.Enum
            => System.Enum.Parse<T>(json.GetProperty(name).GetString());

        // Unity's struct constructors are IL2CPP calls; filling the fields is not.
        private static Vector2 Vec2(JsonElement json, string name)
        {
            var v = Floats(json, name);
            return new Vector2 { x = v[0], y = v[1] };
        }

        private static Vector3 Vec3(JsonElement json, string name)
        {
            var v = Floats(json, name);
            return new Vector3 { x = v[0], y = v[1], z = v[2] };
        }

        private static Color ParseColor(JsonElement json, string name)
        {
            var v = Floats(json, name);
            return new Color { r = v[0], g = v[1], b = v[2], a = v[3] };
        }

        // Builds node under parent with plain Unity components. Its paths match the game's page, and sprites and fonts come
        // from the asset catalog; a missing required sprite throws.
        public static RectTransform Build(Node node, Transform parent)
        {
            var target = new GameObject(node.Name);
            var rect = target.AddComponent<RectTransform>();
            rect.SetParent(parent, false);
            Apply(rect, node.Box);
            if (node.Canvas is { } sorting)
            {
                var canvas = target.AddComponent<Canvas>();
                canvas.overrideSorting = sorting.Override;
                canvas.sortingOrder = sorting.Order;
            }
            if (node.GroupAlpha is { } alpha)
                target.AddComponent<CanvasGroup>().alpha = alpha;
            if (node.Image is { } image)
                Draw(target.AddComponent<Image>(), image);
            if (node.Text is { } text)
                Write(target.AddComponent<Text>(), text);
            foreach (var effect in node.Effects)
            {
                Shadow shadow = effect.Outline ? target.AddComponent<Outline>() : target.AddComponent<Shadow>();
                shadow.enabled = effect.Enabled;
                shadow.effectColor = effect.Color;
                shadow.effectDistance = effect.Distance;
                shadow.useGraphicAlpha = effect.UseGraphicAlpha;
            }
            if (node.LayoutGroup is { } style)
            {
                HorizontalOrVerticalLayoutGroup group = style.Vertical
                    ? target.AddComponent<VerticalLayoutGroup>()
                    : target.AddComponent<HorizontalLayoutGroup>();
                group.enabled = style.Enabled;
                var padding = group.padding;
                padding.left = style.Left;
                padding.right = style.Right;
                padding.top = style.Top;
                padding.bottom = style.Bottom;
                group.spacing = style.Spacing;
                group.childAlignment = style.ChildAlignment;
                group.childControlWidth = style.ControlWidth;
                group.childControlHeight = style.ControlHeight;
                group.childForceExpandWidth = style.ExpandWidth;
                group.childForceExpandHeight = style.ExpandHeight;
            }
            if (node.LayoutElement is { } element)
            {
                var layout = target.AddComponent<LayoutElement>();
                layout.enabled = element.Enabled;
                layout.ignoreLayout = element.IgnoreLayout;
                layout.minWidth = element.Min.x;
                layout.minHeight = element.Min.y;
                layout.preferredWidth = element.Preferred.x;
                layout.preferredHeight = element.Preferred.y;
                layout.flexibleWidth = element.Flexible.x;
                layout.flexibleHeight = element.Flexible.y;
            }
            if (node.Fitter is { } fit)
            {
                var fitter = target.AddComponent<ContentSizeFitter>();
                fitter.enabled = fit.Enabled;
                fitter.horizontalFit = fit.Horizontal;
                fitter.verticalFit = fit.Vertical;
            }
            foreach (var child in node.Children)
                Build(child, rect);
            target.SetActive(node.Active);
            return rect;
        }

        public static void Apply(RectTransform rect, Box box)
        {
            rect.anchorMin = box.AnchorMin;
            rect.anchorMax = box.AnchorMax;
            rect.pivot = box.Pivot;
            rect.anchoredPosition = box.Position;
            rect.sizeDelta = box.SizeDelta;
            rect.localScale = box.Scale;
            rect.localEulerAngles = box.Rotation;
        }

        // Every image built with a sprite, so a sprite the game destroys (a resolution change did) can be replaced.
        private static readonly List<(Image Image, string Sprite)> drawn = new();

        // An image whose sprite was destroyed draws as a plain square, so it stays hidden until the sprite is loaded again.
        // The view sets glyph sprites itself and redraws them after this, on the same Assets.Version change.
        public static void Repair()
        {
            drawn.RemoveAll(entry => !entry.Image);
            foreach (var (image, name) in drawn)
            {
                if (image.sprite)
                    continue;
                var sprite = Assets.Sprite(name);
                if (sprite)
                    image.sprite = sprite;
                image.enabled = sprite;
            }
        }

        private static void Draw(Image image, ImageStyle style)
        {
            image.enabled = style.Enabled;
            if (style.Sprite != null)
            {
                image.sprite = Assets.Sprite(style.Sprite) ?? (Current.OptionalSprites.Contains(style.Sprite) ? null
                    : throw new InvalidOperationException($"The sprite {style.Sprite} is not loaded."));
                if (style.Enabled && image.sprite)
                    drawn.Add((image, style.Sprite));
            }
            image.type = style.Type;
            image.fillCenter = style.FillCenter;
            image.fillMethod = style.FillMethod;
            image.fillAmount = style.FillAmount;
            image.preserveAspect = style.PreserveAspect;
            image.color = style.Color;
        }

        private static void Write(Text text, TextStyle style)
        {
            text.enabled = style.Enabled;
            text.font = Assets.Font(style.Font) ?? throw new InvalidOperationException($"The font {style.Font} is not loaded.");
            text.fontSize = style.FontSize;
            text.fontStyle = style.Style;
            text.alignment = style.Alignment;
            text.lineSpacing = style.LineSpacing;
            text.supportRichText = style.RichText;
            text.horizontalOverflow = style.HorizontalOverflow;
            text.verticalOverflow = style.VerticalOverflow;
            text.resizeTextForBestFit = style.BestFit;
            // The snapshots do not record best fit's range. A new Text grows up to size 40, so best fit is capped at the
            // recorded size, and only shrinks text that does not fit.
            text.resizeTextMaxSize = style.FontSize;
            text.resizeTextMinSize = Math.Min(10, style.FontSize);
            text.color = style.Color;
            text.text = style.Text;
        }
    }
}
