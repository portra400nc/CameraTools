using UnityEngine;
using UnityEngine.UI;
using static CameraTools.Builder;
using Object = UnityEngine.Object;

namespace CameraTools
{
    // CameraTools draws its UI itself, from its own shapes and the game's font, on a canvas of its own that survives scene
    // changes. Genshin cannot run custom MonoBehaviours, so the canvas holds only plain graphics and layout components:
    // CameraTools reads the mouse and the pad itself, and writes each setting's value, the selection, and every animation
    // frame to the UI.
    internal static class CameraUi
    {
        private const string WaitHint = "The CameraTools UI appears once the game has loaded its font";
        private const string ReloadHint = "The CameraTools UI failed; it reloads in a few seconds";
        private const float RepeatDelay = 0.4f;
        private const float RepeatInterval = 0.08f;
        private const float RetryInterval = 10f;
        private const int SortingOrder = 30000;
        private const float ReferenceWidth = 1280f;
        private const float ReferenceHeight = 800f;
        // Wider than any legend, which packs its hints against one end.
        private const float LegendWidth = 1200f;

        private static readonly PadBinding PadA = new(PadButtons.A, PadAxis.None);
        private static readonly PadBinding PadB = new(PadButtons.B, PadAxis.None);
        private static readonly PadBinding PadLB = new(PadButtons.LB, PadAxis.None);
        private static readonly PadBinding PadRB = new(PadButtons.RB, PadAxis.None);
        private static readonly PadBinding DpadUp = new(PadButtons.DpadUp, PadAxis.None);
        private static readonly PadBinding DpadDown = new(PadButtons.DpadDown, PadAxis.None);
        private static readonly PadBinding DpadLeft = new(PadButtons.DpadLeft, PadAxis.None);
        private static readonly PadBinding DpadRight = new(PadButtons.DpadRight, PadAxis.None);
        private static readonly PadBinding StickUp = new(PadButtons.None, PadAxis.LeftStickUp);
        private static readonly PadBinding StickDown = new(PadButtons.None, PadAxis.LeftStickDown);

        private static readonly Repeater up = new(), down = new(), left = new(), right = new();
        private static Live live;
        private static int failures;
        private static int builds;
        private static float nextBuild;
        private static bool hintPending;
        private static bool panelOpen;
        private static int tab;
        private static int selected;
        private static string toast;
        private static float toastUntil;
        private static Vector2 lastMouse;
        private static SliderView dragging;

        public static View View => !CameraTools.freecamActive || CameraTools.uiHidden ? View.Hidden : panelOpen ? View.Panel : View.Hud;

        // Drawn with IMGUI until the UI has been built.
        public static string FallbackToast => live == null && Time.unscaledTime < toastUntil ? toast : null;

        public static void Toast(string message)
        {
            toast = message;
            toastUntil = Time.unscaledTime + Style.ToastTime;
        }

        private static string MissingHint => Time.unscaledTime < nextBuild ? ReloadHint : WaitHint;

        public static void FreecamChanged(bool active)
        {
            panelOpen = false;
            hintPending = active;
        }

        public static void Update()
        {
            string step = "build";
            try
            {
                if (!Ensure())
                {
                    if (hintPending || CameraTools.freecamActive && Controls.Pressed(CamAction.ToggleGUI))
                        Toast(MissingHint);
                    hintPending = false;
                    return;
                }
                hintPending = false;
                step = "input";
                HandleInput();
                step = "render";
                Render();
            }
            catch (Exception e)
            {
                failures++;
                CameraTools.LogOnce($"UI: {step} failed (failure {failures}); CameraTools rebuilds its UI in {RetryInterval:0} s. {e}");
                if (live != null && live.Root)
                    Object.Destroy(live.Root);
                live = null;
                ClosePanel();
                nextBuild = Time.unscaledTime + RetryInterval;
                Toast(ReloadHint);
            }
        }

        private static bool Ensure()
        {
            if (live != null)
                return live.Root && live.Font ? true : throw new InvalidOperationException("The CameraTools canvas or its font was destroyed.");
            if (Time.unscaledTime < nextBuild)
                return false;
            var font = Assets.Font(CameraTools.freecamActive);
            if (!font)
                return false;
            live = Build(font);
            builds++;
            CameraTools.LogOnce($"UI: built the CameraTools UI (build {builds}).");
            return true;
        }

        private static Live Build(Font font)
        {
            Shapes.Load();
            var ui = new Builder(font);
            var root = new GameObject("CameraTools UI");
            try
            {
                Object.DontDestroyOnLoad(root);
                var canvas = root.AddComponent<Canvas>();
                canvas.renderMode = RenderMode.ScreenSpaceOverlay;
                // Text placed between pixels is sampled soft; snapping keeps it sharp at the Deck's 1:1 scale.
                canvas.pixelPerfect = true;
                canvas.sortingOrder = SortingOrder;
                // CameraTools hit-tests the mouse itself. The raycaster only tells the game's EventSystem that the pointer is over
                // UI, so a click on the panel is not also a click in the world.
                root.AddComponent<GraphicRaycaster>();
                // Canvas units are the mockup's pixels at 1280x800, and any other screen scales them to fit.
                var scaler = root.AddComponent<CanvasScaler>();
                scaler.uiScaleMode = CanvasScaler.ScaleMode.ScaleWithScreenSize;
                scaler.referenceResolution = V(ReferenceWidth, ReferenceHeight);
                scaler.screenMatchMode = CanvasScaler.ScreenMatchMode.Expand;
                scaler.referencePixelsPerUnit = 100f;
                var built = new Live { Root = root, Font = font };
                BuildHud(ui, built, root.transform);
                BuildPanel(ui, built, root.transform);
                BuildToast(ui, built, root.transform);
                ShowTab(Math.Min(tab, UiModel.Tabs.Length - 1), built);
                built.ShownTab = tab;
                for (int index = 0; index < built.Tabs.Length; index++)
                {
                    built.Tabs[index].Rows.SetActive(index == tab);
                    built.Tabs[index].Show(index == tab, 1f);
                }
                return built;
            }
            catch
            {
                Object.Destroy(root);
                throw;
            }
        }

        private static void BuildHud(Builder ui, Live target, Transform root)
        {
            var hud = Node("Hud", root);
            Fill(hud);
            target.Hud = new Fader(hud, V(0f, 0f), Style.HudFade);
            var bottom = HintRow(hud, "Bottom", TextAnchor.MiddleRight);
            Pin(bottom, 1f, 0f, -Style.Margin, Style.Margin, LegendWidth, Style.HintHeight);
            var top = HintRow(hud, "Top", TextAnchor.MiddleRight);
            Pin(top, 1f, 1f, -Style.Margin, -Style.TopLegendY, LegendWidth, Style.HintHeight);
            target.Legends = UiModel.BottomHints.Select(hint => BuildLegend(ui, bottom, hint))
                .Concat(UiModel.TopHints.Select(hint => BuildLegend(ui, top, hint)))
                .ToArray();

            var fov = Node("Fov", hud);
            Pin(fov, 0f, 0.5f, Style.FovX, 0f, Style.FovWidth, Style.FovHeight);
            // The track's image carries a one unit ring outside the track.
            var track = Picture(fov, "Track", Shapes.FovTrack, Style.White).rectTransform;
            Place(track, V(0.5f, 0f), V(0.5f, 1f), V(0.5f, 0.5f), V(0f, 0f), V(5f, 2f - 2f * Style.FovInset));
            var plus = Picture(fov, "Plus", Shapes.Plus, Style.Text);
            Place(plus.rectTransform, V(0.5f, 1f), V(0.5f, 1f), V(0.5f, 0.5f), V(0f, -Style.FovEndY), V(Shapes.Plus.Width, Shapes.Plus.Height));
            DropShadow(plus, Style.FovEndShadow);
            var minus = Picture(fov, "Minus", Shapes.Minus, Style.Text);
            Place(minus.rectTransform, V(0.5f, 0f), V(0.5f, 0f), V(0.5f, 0.5f), V(0f, Style.FovEndY), V(Shapes.Minus.Width, Shapes.Minus.Height));
            DropShadow(minus, Style.FovEndShadow);
            target.FovHandle = Picture(fov, "Handle", Shapes.Handle, Style.White).rectTransform;
            Place(target.FovHandle, V(0.5f, 1f), V(0.5f, 1f), V(0.5f, 0.5f), V(0f, -Style.FovInset), V(Shapes.Handle.Width, Shapes.Handle.Height));
        }

        private static Legend BuildLegend(Builder ui, Transform row, Hint hint)
        {
            var (key, pad) = Controls.Binding(hint.Action);
            return new Legend(ui.PadHint(row, pad, hint.Label), pad != default, ui.KeyHint(row, key, hint.Label), key != KeyCode.None);
        }

        private static void BuildPanel(Builder ui, Live target, Transform root)
        {
            var panel = Node("Panel", root);
            Place(panel, V(0f, 0f), V(0f, 1f), V(0f, 0.5f), V(0f, 0f), V(Style.PanelWidth, 0f));
            target.Panel = new Fader(panel, V(-Style.PanelSlide, 0f), Style.PanelFade);
            var shadow = Picture(panel, "Shadow", Shapes.PanelShadow, Style.White);
            Place(shadow.rectTransform, V(1f, 0f), V(1f, 1f), V(0f, 0.5f), V(0f, 0f), V(Style.PanelShadowWidth, 0f));
            var edge = Picture(panel, "Edge", null, Style.PanelEdge);
            Place(edge.rectTransform, V(1f, 0f), V(1f, 1f), V(0f, 0.5f), V(0f, 0f), V(1f, 0f));
            Fill(Picture(panel, "Fill", Shapes.PanelFill, Style.White).rectTransform);

            var bar = Picture(panel, "Tabs", null, Style.TabBar).rectTransform;
            Place(bar, V(0f, 1f), V(1f, 1f), V(0.5f, 1f), V(0f, 0f), V(0f, Style.TabBarHeight));
            var line = Picture(bar, "Line", null, Style.PanelEdge).rectTransform;
            Place(line, V(0f, 0f), V(1f, 0f), V(0.5f, 0f), V(0f, 0f), V(0f, 1f));
            var lb = ui.Glyph(bar, PadButtons.LB);
            Pin(lb, 0f, 0.5f, Style.TabBarPadding, 0f, Shapes.Bumper.Width, Shapes.Bumper.Height);
            var rb = ui.Glyph(bar, PadButtons.RB);
            Pin(rb, 1f, 0.5f, -Style.TabBarPadding, 0f, Shapes.Bumper.Width, Shapes.Bumper.Height);
            target.TabKeys = new[] { lb.gameObject, rb.gameObject };
            target.Back = Picture(bar, "Back", Shapes.Back, Style.White).rectTransform;
            Pin(target.Back, 0f, 0.5f, Style.TabBarPadding, 0f, Style.BackSize, Style.BackSize);
            target.Strip = Node("Strip", bar);
            Fill(target.Strip);
            target.StripGroup = Flow(target.Strip.gameObject, Style.TabGap, TextAnchor.MiddleCenter);

            target.List = Node("List", panel);
            Place(target.List, V(0f, 0f), V(1f, 1f), V(0.5f, 0.5f), V(0f, (Style.FooterHeight - Style.TabBarHeight) / 2f),
                V(0f, -Style.FooterHeight - Style.TabBarHeight));
            var rows = Node("Rows", panel);
            TopLeft(rows, 0f, Style.ListTop, Style.PanelWidth, 0f);
            target.Rows = rows.gameObject.AddComponent<CanvasGroup>();
            target.Tabs = UiModel.Tabs.Select(model =>
            {
                var view = ui.Tab(target.Strip, model.Name);
                var tabRows = Node(model.Name, rows);
                TopLeft(tabRows, 0f, 0f, Style.PanelWidth, 0f);
                view.Rows = tabRows.gameObject;
                float y = 0f;
                foreach (var row in model.Rows)
                {
                    RowView built = row switch
                    {
                        Section section => ui.Section(tabRows, section, y),
                        SliderRow slider => ui.Slider(tabRows, slider, y),
                        ToggleRow toggle => ui.Toggle(tabRows, toggle, y),
                        _ => throw new InvalidOperationException($"No widget for {row.GetType().Name}."),
                    };
                    y += built.Rect.sizeDelta.y;
                    view.RowViews.Add(built);
                }
                return view;
            }).ToArray();

            var footer = HintRow(panel, "Footer", TextAnchor.MiddleCenter);
            Place(footer, V(0f, 0f), V(1f, 0f), V(0.5f, 0f), V(0f, Style.FooterBottom), V(0f, Style.HintHeight));
            target.Footer = footer.gameObject;
            target.Change = ui.PadHint(footer, PadA, "On");
            ui.PadHint(footer, PadB, "Return");
        }

        private static void BuildToast(Builder ui, Live target, Transform root)
        {
            var toastRow = Node("Toast", root);
            Place(toastRow, V(0f, 1f), V(1f, 1f), V(0.5f, 1f), V(0f, -Style.ToastY), V(0f, Style.ToastHeight));
            Flow(toastRow.gameObject, 0f, TextAnchor.MiddleCenter);
            target.Toast = new Fader(toastRow, V(0f, 8f), Style.ToastFade);
            var pill = Picture(toastRow, "Pill", Shapes.Toast, Style.White);
            pill.rectTransform.sizeDelta = V(0f, Style.ToastHeight);
            var group = Flow(pill.gameObject, 0f, TextAnchor.MiddleCenter);
            group.padding.left = Style.ToastPadding;
            group.padding.right = Style.ToastPadding;
            target.ToastLabel = ui.Label(pill.rectTransform, "Label", "", Style.ToastSize, Style.Text, TextAnchor.MiddleCenter);
            target.ToastLabel.rectTransform.sizeDelta = V(0f, Style.ToastHeight);
        }

        private static void ShowTab(int index, Live target)
        {
            tab = index;
            dragging = null;
            selected = target.Tabs[index].RowViews.FindIndex(view => view.Row is not Section);
        }

        private static List<RowView> CurrentRows => live.Tabs[tab].RowViews;

        private static void HandleInput()
        {
            if (!CameraTools.freecamActive)
                return;
            bool padOwned = Controls.Owner == PadOwner.CameraTools;
            if (Controls.Pressed(CamAction.ToggleGUI)
                || panelOpen && padOwned && Controls.Binding(CamAction.ToggleGUI).Pad.Pressed(Gamepad.Current, Gamepad.Previous))
            {
                // Opening the panel brings back a hidden UI.
                if (CameraTools.uiHidden)
                    CameraTools.SetUiHidden(false);
                SetPanel(!panelOpen);
                return;
            }
            if (panelOpen && CameraTools.uiHidden)
                SetPanel(false);
            if (panelOpen && padOwned)
                Navigate();
            if (panelOpen && !Freecam.Focused)
                HandleMouse();
        }

        private static void SetPanel(bool open)
        {
            panelOpen = open;
            dragging = null;
            Freecam.Focused = !open;
        }

        private static void ClosePanel()
        {
            if (panelOpen)
                SetPanel(false);
        }

        // The panel reads the pad with fixed buttons, like the L3+R3 switch, so rebinding camera actions cannot strand it.
        private static void Navigate()
        {
            var now = Gamepad.Current;
            var before = Gamepad.Previous;
            if (PadB.Pressed(now, before))
            {
                SetPanel(false);
                return;
            }
            int tabs = UiModel.Tabs.Length;
            if (PadLB.Pressed(now, before))
                ShowTab((tab + tabs - 1) % tabs, live);
            if (PadRB.Pressed(now, before))
                ShowTab((tab + 1) % tabs, live);

            int move = up.Fire(DpadUp.Held(now) || StickUp.Held(now)) ? -1
                : down.Fire(DpadDown.Held(now) || StickDown.Held(now)) ? 1 : 0;
            if (move != 0)
                selected = NextRow(move);

            bool stepRight = right.Fire(DpadRight.Held(now));
            bool stepLeft = left.Fire(DpadLeft.Held(now));
            if (selected < 0)
                return;
            switch (CurrentRows[selected].Row)
            {
                case ToggleRow toggle when PadA.Pressed(now, before) || DpadLeft.Pressed(now, before) || DpadRight.Pressed(now, before):
                    toggle.Set(!toggle.Get());
                    break;
                case SliderRow slider when stepLeft != stepRight:
                    Step(slider, stepRight ? 1 : -1);
                    break;
            }
        }

        private static int NextRow(int direction)
        {
            var rows = CurrentRows;
            for (int index = selected + direction; index >= 0 && index < rows.Count; index += direction)
                if (rows[index].Row is not Section)
                    return index;
            return selected;
        }

        private static void Step(SliderRow slider, int direction)
            => slider.Set(Math.Clamp(slider.Get() + direction * slider.Step, slider.Min, slider.Max));

        // Nothing on the canvas is visible to the game's EventSystem, so CameraTools hit-tests the mouse itself. An overlay
        // canvas maps screen points without a camera.
        private static void HandleMouse()
        {
            var mouse = Input.mousePosition;
            var point = V(mouse.x, mouse.y);
            bool moved = point.x != lastMouse.x || point.y != lastMouse.y;
            lastMouse = point;

            int row = RowAt(point);
            if (moved && row >= 0 && dragging == null)
                selected = row;
            float wheel = Input.mouseScrollDelta.y;
            if (wheel != 0f && row >= 0 && CurrentRows[row].Row is SliderRow wheeled)
                Step(wheeled, wheel > 0f ? 1 : -1);
            if (Input.GetMouseButtonDown(0))
                Click(point, row);
            if (dragging == null)
                return;
            if (Input.GetMouseButton(0))
                Drag(dragging, point);
            else
                dragging = null;
        }

        private static bool Contains(RectTransform rect, Vector2 point) => RectTransformUtility.RectangleContainsScreenPoint(rect, point, null);

        // Only the shown tab's rows count, and not while they fade to another tab.
        private static int RowAt(Vector2 point)
            => live.ShownTab == tab && Contains(live.List, point)
                ? CurrentRows.FindIndex(view => view.Row is not Section && Contains(view.Rect, point))
                : -1;

        private static void Click(Vector2 point, int row)
        {
            int tabIndex = Array.FindIndex(live.Tabs, view => Contains(view.Button, point));
            bool back = live.Layout == InputDevice.Keyboard && Contains(live.Back, point);
            if (tabIndex < 0 && !back && row < 0)
                return;
            CameraTools.LogOnce("UI: CameraTools handled a mouse click on its panel.");
            if (tabIndex >= 0)
            {
                if (tabIndex != tab)
                    ShowTab(tabIndex, live);
                return;
            }
            if (back)
            {
                SetPanel(false);
                return;
            }
            selected = row;
            var view = CurrentRows[row];
            if (view.Row is ToggleRow toggle)
                toggle.Set(!toggle.Get());
            else if (view is SliderView slider && Contains(slider.Track, point))
                dragging = slider;
        }

        // The interop passes ScreenPointToLocalPointInRectangle's out value by value, so the slider's ends are projected to
        // the screen instead.
        private static void Drag(SliderView view, Vector2 point)
        {
            var rect = view.Track;
            var area = rect.rect;
            float from = RectTransformUtility.WorldToScreenPoint(null, rect.TransformPoint(new Vector3 { x = area.xMin })).x;
            float to = RectTransformUtility.WorldToScreenPoint(null, rect.TransformPoint(new Vector3 { x = area.xMax })).x;
            if (to <= from)
                return;
            float t = Math.Clamp((point.x - from) / (to - from), 0f, 1f);
            var slider = (SliderRow)view.Row;
            slider.Set(slider.Min + (slider.Max - slider.Min) * t);
        }

        private static void Render()
        {
            var view = View;
            float deltaTime = Time.unscaledDeltaTime;
            // Values that changed while the panel or their tab was hidden show without animating.
            bool opened = view == View.Panel && live.Shown != View.Panel;
            live.Shown = view;
            live.Hud.Update(view == View.Hud, deltaTime);
            live.Panel.Update(view == View.Panel, deltaTime);
            var layout = Controls.Layout;
            if (live.Layout != layout)
            {
                live.Layout = layout;
                RenderLayout(layout == InputDevice.Pad);
            }
            if (live.Hud.Visible)
                RenderFov();
            if (live.Panel.Visible)
                RenderPanel(opened, deltaTime);
            RenderToast(deltaTime);
        }

        // Like Genshin on PC, the keyboard layout has a back button and no key hints in the panel.
        private static void RenderLayout(bool pad)
        {
            foreach (var key in live.TabKeys)
                key.SetActive(pad);
            live.Back.gameObject.SetActive(!pad);
            live.Footer.SetActive(pad);
            live.Strip.offsetMin = V(pad ? 0f : Style.TabsAfterBack, 0f);
            live.StripGroup.childAlignment = pad ? TextAnchor.MiddleCenter : TextAnchor.MiddleLeft;
            foreach (var legend in live.Legends)
            {
                legend.Pad.Root.SetActive(pad && legend.HasPad);
                legend.Key.Root.SetActive(!pad && legend.HasKey);
            }
        }

        // The + end is at the top, so a wide field of view moves the handle up.
        private static void RenderFov()
        {
            var fov = CameraTools.settings.Fov;
            if (live.FovShown == fov.Value)
                return;
            // "+" at the top zooms in, so a narrow field of view sits at the top, like Genshin's zoom bar.
            float t = (fov.Value - fov.Min) / (fov.Max - fov.Min);
            live.FovHandle.anchoredPosition = V(0f, -(Style.FovInset + t * (Style.FovHeight - 2f * Style.FovInset)));
            live.FovShown = fov.Value;
        }

        // A tab switch fades the shown rows out, swaps in the selected tab's, and fades them in.
        private static void RenderPanel(bool opened, float deltaTime)
        {
            for (int index = 0; index < live.Tabs.Length; index++)
                live.Tabs[index].Show(index == tab, opened ? 1f : deltaTime / Style.TabFade);
            float fade = opened ? 1f : deltaTime / Style.RowsFade;
            bool swapped = false;
            if (live.ShownTab != tab)
            {
                live.RowsFade.Toward(false, fade);
                swapped = live.RowsFade.Value == 0f;
            }
            if (swapped)
            {
                live.Tabs[live.ShownTab].Rows.SetActive(false);
                live.Tabs[tab].Rows.SetActive(true);
                live.ShownTab = tab;
                live.ShownSelected = -1;
            }
            if (live.ShownTab == tab)
                live.RowsFade.Toward(true, fade);
            float alpha = live.RowsFade.Eased;
            if (alpha != live.RowsAlpha)
            {
                live.Rows.alpha = alpha;
                live.RowsAlpha = alpha;
            }

            var rows = live.Tabs[live.ShownTab].RowViews;
            float step = opened || swapped ? 1f : deltaTime / Style.SwitchSlide;
            foreach (var row in rows)
                row.Sync(step);
            if (live.ShownTab == tab && live.ShownSelected != selected)
            {
                for (int index = 0; index < rows.Count; index++)
                    rows[index].Select(index == selected);
                live.ShownSelected = selected;
            }
            // The footer names what A does to the selected row.
            string change = selected >= 0 && CurrentRows[selected].Row is ToggleRow toggle ? toggle.Get() ? "Off" : "On" : null;
            if (change == live.ChangeText)
                return;
            live.Change.Root.SetActive(change != null);
            if (change != null)
                live.Change.Label.text = change;
            live.ChangeText = change;
        }

        private static void RenderToast(float deltaTime)
        {
            bool show = !CameraTools.uiHidden && Time.unscaledTime < toastUntil;
            if (show && live.ToastText != toast)
            {
                live.ToastLabel.text = toast;
                live.ToastText = toast;
            }
            live.Toast.Update(show, deltaTime);
        }

        // Fires on press, then repeats while held, on unscaled time so it works while the game is paused.
        private sealed class Repeater
        {
            private float? next;

            public bool Fire(bool held)
            {
                if (!held)
                {
                    next = null;
                    return false;
                }
                float now = Time.unscaledTime;
                if (next is null)
                {
                    next = now + RepeatDelay;
                    return true;
                }
                if (now < next)
                    return false;
                next = now + RepeatInterval;
                return true;
            }
        }

        // A hint's controller and keyboard versions; only one shows, and only if the action has a binding for it.
        private sealed record Legend(HintView Pad, bool HasPad, HintView Key, bool HasKey);

        private sealed class Live
        {
            public GameObject Root;
            public Font Font;
            public Fader Hud;
            public Fader Panel;
            public Fader Toast;
            public Legend[] Legends;
            public RectTransform FovHandle;
            public GameObject[] TabKeys;
            public RectTransform Back;
            public RectTransform Strip;
            public HorizontalLayoutGroup StripGroup;
            public TabView[] Tabs;
            public RectTransform List;
            public CanvasGroup Rows;
            public GameObject Footer;
            public HintView Change;
            public Text ToastLabel;
            public View? Shown;
            public InputDevice? Layout;
            public float FovShown = float.NaN;
            public int ShownTab;
            public Tween RowsFade = new() { Value = 1f };
            public float RowsAlpha = 1f;
            public int ShownSelected = -1;
            public string ChangeText = "";
            public string ToastText;
        }
    }
}
