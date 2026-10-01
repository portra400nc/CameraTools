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
        private const float ConfirmTime = 3f;
        private const int SortingOrder = 30000;
        private const float ReferenceWidth = 1280f;
        private const float ReferenceHeight = 800f;
        // Wider than any legend, which packs its hints against one end.
        private const float LegendWidth = 1200f;
        // "16384x16384", the largest size a slot takes.
        private const int MaxTyped = 11;

        private static readonly PadBinding PadA = new(PadButtons.A, PadAxis.None);
        private static readonly PadBinding PadB = new(PadButtons.B, PadAxis.None);
        private static readonly PadBinding PadY = new(PadButtons.Y, PadAxis.None);
        private static readonly PadBinding PadLB = new(PadButtons.LB, PadAxis.None);
        private static readonly PadBinding PadRB = new(PadButtons.RB, PadAxis.None);
        private static readonly PadBinding DpadUp = new(PadButtons.DpadUp, PadAxis.None);
        private static readonly PadBinding DpadDown = new(PadButtons.DpadDown, PadAxis.None);
        private static readonly PadBinding DpadLeft = new(PadButtons.DpadLeft, PadAxis.None);
        private static readonly PadBinding DpadRight = new(PadButtons.DpadRight, PadAxis.None);
        private static readonly PadBinding StickUp = new(PadButtons.None, PadAxis.LeftStickUp);
        private static readonly PadBinding StickDown = new(PadButtons.None, PadAxis.LeftStickDown);

        // The game's build has no Input.inputString, so typing is read key by key: digits from both rows, and x or the keypad's
        // * between the width and the height.
        private static readonly (KeyCode Key, char Typed)[] TypedKeys = Enumerable.Range(0, 10)
            .SelectMany(digit => new[] { (KeyCode.Alpha0 + digit, (char)('0' + digit)), (KeyCode.Keypad0 + digit, (char)('0' + digit)) })
            .Append((KeyCode.X, 'x'))
            .Append((KeyCode.KeypadMultiply, 'x'))
            .ToArray();

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
        private static bool toastWhileHidden;
        private static Vector2 lastMouse;
        private static SliderView dragging;
        private static ResolutionView editing;
        private static ActionView armed;
        private static float armedUntil;

        public static View View => !CameraTools.freecamActive || CameraTools.uiHidden ? View.Hidden
            : panelOpen ? View.Panel
            : PathPlayback.State is PlaybackState.Playing ? View.Playing
            : View.Hud;

        // Drawn with IMGUI until the UI has been built.
        public static string FallbackToast => live == null && Time.unscaledTime < toastUntil ? toast : null;

        // whileHidden: shown even while the UI is hidden, as the screenshot countdown is.
        public static void Toast(string message, bool whileHidden = false)
        {
            toast = message;
            toastUntil = Time.unscaledTime + Style.ToastTime;
            toastWhileHidden = whileHidden;
        }

        public static void ClearToast() => toastUntil = 0f;

        // Whether anything of CameraTools' is on screen, a part that is still fading out included.
        public static bool OnScreen => live != null
            ? live.Hud.Visible || live.Focus.Visible || live.PlayBar.Visible || live.Panel.Visible || live.Toast.Visible
            : FallbackToast != null;

        public static bool PanelOpen => panelOpen;

        private static string MissingHint => Time.unscaledTime < nextBuild ? ReloadHint : WaitHint;

        public static void FreecamChanged(bool active)
        {
            StopEditing();
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
                var built = new Live
                {
                    Root = root, Canvas = root.GetComponent<RectTransform>(), Font = font, MissingSettings = !Graphics.HasSettings,
                };
                var model = UiModel.Tabs();
                BuildFocus(built, root.transform);
                BuildHud(ui, built, root.transform);
                BuildPlayBar(ui, built, root.transform);
                BuildPanel(ui, built, root.transform, model);
                BuildToast(ui, built, root.transform);
                ShowTab(Math.Min(tab, model.Length - 1), built);
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

        // The depth of field's focus point: brackets at the corners of the window its autofocus samples, and a cross in
        // the middle. It is under everything else, so the panel covers it.
        private static void BuildFocus(Live target, Transform root)
        {
            var focus = Node("Focus", root);
            Fill(focus);
            target.Focus = new Fader(focus, V(0f, 0f), Style.HudFade);
            target.FocusWindow = Picture(focus, "Window", Shapes.FocusWindow, Style.White).rectTransform;
            var cross = Picture(target.FocusWindow, "Cross", Shapes.FocusCross, Style.White).rectTransform;
            Pin(cross, 0.5f, 0.5f, 0f, 0f, Shapes.FocusCross.Width, Shapes.FocusCross.Height);
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

        // Across the top, like the mockup's: the path and its time, a rail with a tick per node, and the Stop hint.
        private static void BuildPlayBar(Builder ui, Live target, Transform root)
        {
            var bar = Node("PlayBar", root);
            Place(bar, V(0f, 1f), V(1f, 1f), V(0.5f, 1f), V(0f, -Style.PlayBarY), V(-2f * Style.Margin, Style.HintHeight));
            Flow(bar.gameObject, Style.PlayBarGap, TextAnchor.MiddleLeft);
            target.PlayBar = new Fader(bar, V(0f, 0f), Style.HudFade);
            target.PlayMeta = ui.Label(bar, "Meta", "", Style.HintSize, Style.Text, TextAnchor.MiddleLeft);
            target.PlayMeta.rectTransform.sizeDelta = V(0f, Style.HintHeight);
            DropShadow(target.PlayMeta, Style.TextShadow);
            target.PlayMetaWidth = target.PlayMeta.gameObject.AddComponent<LayoutElement>();
            // The rail's image carries a one unit ring outside the track, like the field of view track's.
            target.PlayRail = Picture(bar, "Rail", Shapes.FovTrack, Style.White).rectTransform;
            target.PlayRail.sizeDelta = V(0f, Style.RailHeight + 2f);
            target.PlayRail.gameObject.AddComponent<LayoutElement>().flexibleWidth = 1f;
            target.PlayFill = Picture(target.PlayRail, "Fill", Shapes.Bar, Style.Cream).rectTransform;
            Place(target.PlayFill, V(0f, 0.5f), V(0f, 0.5f), V(0f, 0.5f), V(0f, 0f), V(0f, Style.RailHeight));
            target.Legends = target.Legends.Append(BuildLegend(ui, bar, new Hint(CamAction.PlayPath, "Stop"))).ToArray();
        }

        private static Legend BuildLegend(Builder ui, Transform row, Hint hint)
        {
            var (key, pad) = Controls.Binding(hint.Action);
            return new Legend(ui.PadHint(row, pad, hint.Label), pad != default, ui.KeyHint(row, key, hint.Label), key != KeyCode.None);
        }

        private static void BuildPanel(Builder ui, Live target, Transform root, Tab[] model)
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
            // The tabs are a viewport like the list: more tabs than fit between the glyphs scroll sideways, clipped to it.
            target.TabClip = Node("Clip", bar);
            Fill(target.TabClip);
            target.TabClip.gameObject.AddComponent<RectMask2D>();
            target.Strip = Node("Strip", target.TabClip);
            Place(target.Strip, V(0f, 0f), V(0f, 1f), V(0f, 0.5f), V(0f, 0f), V(Style.StripWidth, 0f));
            Flow(target.Strip.gameObject, Style.TabGap, TextAnchor.MiddleLeft);

            // The list is a viewport: a tab taller than the panel scrolls inside it, clipped to it.
            target.List = Node("List", panel);
            Place(target.List, V(0f, 0f), V(1f, 1f), V(0.5f, 0.5f), V(0f, (Style.FooterHeight - Style.TabBarHeight) / 2f),
                V(0f, -Style.FooterHeight - Style.TabBarHeight));
            target.List.gameObject.AddComponent<RectMask2D>();
            var rows = Node("Rows", target.List);
            TopLeft(rows, 0f, Style.ListPadding, Style.PanelWidth, 0f);
            target.Rows = rows.gameObject.AddComponent<CanvasGroup>();
            target.Tabs = model.Select(tabModel =>
            {
                var view = ui.Tab(target.Strip, tabModel.Name);
                var tabRows = Node(tabModel.Name, rows);
                TopLeft(tabRows, 0f, 0f, Style.PanelWidth, 0f);
                view.Rows = tabRows.gameObject;
                view.RowsRect = tabRows;
                float y = 0f;
                foreach (var row in tabModel.Rows)
                {
                    RowView built = row switch
                    {
                        Section section => ui.Section(tabRows, section, y),
                        SliderRow slider => ui.Slider(tabRows, slider, y),
                        ToggleRow toggle => ui.Toggle(tabRows, toggle, y),
                        ChoiceRow choice => ui.Choice(tabRows, choice, y),
                        StepperRow stepper => ui.Stepper(tabRows, stepper, y),
                        ActionRow action => ui.Action(tabRows, action, y),
                        ResolutionRow resolution => ui.Resolution(tabRows, resolution, y),
                        _ => throw new InvalidOperationException($"No widget for {row.GetType().Name}."),
                    };
                    built.Top = y;
                    built.Height = built.Rect.sizeDelta.y;
                    y += built.Height;
                    view.RowViews.Add(built);
                }
                view.Height = y;
                return view;
            }).ToArray();
            target.Thumb = Picture(target.List, "Thumb", Shapes.Bar, Style.Thumb).rectTransform;
            target.Thumb.gameObject.SetActive(false);

            var footer = HintRow(panel, "Footer", TextAnchor.MiddleCenter);
            Place(footer, V(0f, 0f), V(1f, 0f), V(0.5f, 0f), V(0f, Style.FooterBottom), V(0f, Style.HintHeight));
            target.Footer = footer.gameObject;
            target.Change = ui.PadHint(footer, PadA, "On");
            target.Steps = ui.GlyphHint(footer, PadButtons.DpadLeft | PadButtons.DpadRight, "Change");
            target.Edit = ui.PadHint(footer, PadY, "Edit");
            target.Return = ui.PadHint(footer, PadB, "Return");
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
            StopEditing();
            tab = index;
            target.StripReveal = true;
            dragging = null;
            selected = target.Tabs[index].RowViews.FindIndex(view => view.Row is not Section);
            ScrollTo(target, target.Tabs[index], 0f);
        }

        private static float ViewHeight(Live target) => target.List.rect.height - 2f * Style.ListPadding;

        private static void ScrollTo(Live target, TabView view, float scroll)
        {
            scroll = Math.Clamp(scroll, 0f, Math.Max(view.Height - ViewHeight(target), 0f));
            if (scroll == view.Scroll)
                return;
            view.Scroll = scroll;
            TopLeft(view.RowsRect, 0f, -scroll, Style.PanelWidth, 0f);
        }

        // The section header right above the selected row comes into view with it.
        private static void Reveal()
        {
            if (selected < 0)
                return;
            var view = live.Tabs[tab];
            var rows = view.RowViews;
            var row = rows[selected];
            float top = selected > 0 && rows[selected - 1].Row is Section ? rows[selected - 1].Top : row.Top;
            float bottom = row.Top + row.Height;
            float height = ViewHeight(live);
            if (top < view.Scroll)
                ScrollTo(live, view, top);
            else if (bottom > view.Scroll + height)
                ScrollTo(live, view, bottom - height);
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
                if (!panelOpen)
                    BuildSettingRows();
                SetPanel(!panelOpen);
                return;
            }
            if (panelOpen && CameraTools.uiHidden)
                SetPanel(false);
            if (editing != null)
                Type();
            if (panelOpen && padOwned)
                Navigate();
            if (panelOpen && !Freecam.Focused)
                HandleMouse();
        }

        // The game's option lists may not be readable yet when its font first loads, so the UI is built again once they are.
        private static void BuildSettingRows()
        {
            if (!live.MissingSettings || !Graphics.HasSettings)
                return;
            var font = live.Font;
            live.Root.SetActive(false);
            Object.Destroy(live.Root);
            live = Build(font);
            CameraTools.LogOnce("UI: rebuilt the CameraTools UI now that the game's settings can be read.");
        }

        private static void SetPanel(bool open)
        {
            StopEditing();
            panelOpen = open;
            dragging = null;
            Freecam.Focused = !open;
        }

        public static void ClosePanel()
        {
            if (panelOpen)
                SetPanel(false);
        }

        // On the tab and row it was closed on.
        public static void OpenPanel()
        {
            if (!panelOpen)
                SetPanel(true);
        }

        // The panel reads the pad with fixed buttons, like the L3+R3 switch, so rebinding camera actions cannot strand it.
        private static void Navigate()
        {
            var now = Gamepad.Current;
            var before = Gamepad.Previous;
            if (editing != null)
            {
                if (PadA.Pressed(now, before))
                    Commit();
                else if (PadB.Pressed(now, before))
                    StopEditing();
                return;
            }
            if (PadB.Pressed(now, before))
            {
                SetPanel(false);
                return;
            }
            int tabs = live.Tabs.Length;
            if (PadLB.Pressed(now, before))
                ShowTab((tab + tabs - 1) % tabs, live);
            if (PadRB.Pressed(now, before))
                ShowTab((tab + 1) % tabs, live);

            int move = up.Fire(DpadUp.Held(now) || StickUp.Held(now)) ? -1
                : down.Fire(DpadDown.Held(now) || StickDown.Held(now)) ? 1 : 0;
            if (move != 0)
            {
                selected = NextRow(move);
                Reveal();
            }

            bool stepRight = right.Fire(DpadRight.Held(now));
            bool stepLeft = left.Fire(DpadLeft.Held(now));
            // Choices and slots change once per press: each game setting change is saved, and each slot change is written
            // to MelonPreferences.cfg.
            bool pressRight = DpadRight.Pressed(now, before);
            bool pressLeft = DpadLeft.Pressed(now, before);
            bool pressA = PadA.Pressed(now, before);
            if (selected < 0)
                return;
            var view = CurrentRows[selected];
            if (!view.Row.Usable)
                return;
            switch (view.Row)
            {
                case ToggleRow toggle when pressA || pressLeft || pressRight:
                    toggle.Set(!toggle.Get());
                    break;
                case SliderRow slider when stepLeft != stepRight:
                    Step(slider, stepRight ? 1 : -1);
                    break;
                case ChoiceRow choice when pressLeft != pressRight:
                    Choose(choice, pressRight ? 1 : -1);
                    break;
                case StepperRow stepper when pressLeft != pressRight:
                    Browse(stepper, pressRight ? 1 : -1);
                    break;
                case ActionRow when pressA:
                    Run((ActionView)view);
                    break;
                case ResolutionRow slot when pressA:
                    Graphics.ApplySlot(slot.Slot);
                    break;
                case ResolutionRow slot when pressLeft != pressRight:
                    Graphics.CycleSlot(slot.Slot, pressRight ? 1 : -1);
                    break;
                case ResolutionRow when PadY.Pressed(now, before):
                    StartEditing((ResolutionView)view);
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

        private static void Choose(ChoiceRow choice, int direction)
        {
            int now = choice.Get();
            int next = Math.Clamp(now + direction, 0, choice.Options.Length - 1);
            if (next != now)
                choice.Set(next);
        }

        private static void Browse(StepperRow stepper, int direction)
        {
            int next = stepper.Get() + direction;
            if (next >= 0 && next < stepper.Count())
                stepper.Set(next);
        }

        private static void Change(Row row, int direction)
        {
            if (row is ChoiceRow choice)
                Choose(choice, direction);
            else if (row is StepperRow stepper)
                Browse(stepper, direction);
        }

        // A row that asks for confirmation runs on a second press within ConfirmTime; the first only arms it.
        private static void Run(ActionView view)
        {
            var action = (ActionRow)view.Row;
            if (action.Confirm != null && armed != view)
            {
                Disarm();
                armed = view;
                armed.Armed = true;
                armedUntil = Time.unscaledTime + ConfirmTime;
                return;
            }
            Disarm();
            action.Run();
        }

        private static void Disarm()
        {
            if (armed == null)
                return;
            armed.Armed = false;
            armed = null;
        }

        // Moving the selection, switching tabs, closing the panel, or waiting too long cancels a confirmation.
        private static void ExpireConfirm()
        {
            if (armed != null && (!panelOpen || Time.unscaledTime >= armedUntil || selected < 0 || CurrentRows[selected] != armed))
                Disarm();
        }

        private static void StartEditing(ResolutionView view)
        {
            if (editing == view)
                return;
            StopEditing();
            editing = view;
            view.Buffer = "";
            Controls.TextCapture = true;
            // The game reads the keyboard under the free camera too, and 1 to 4 would switch party members.
            CameraTools.SetPlayerInput(false, "Typing");
        }

        private static void StopEditing()
        {
            if (editing == null)
                return;
            editing.Buffer = null;
            editing = null;
            Controls.TextCapture = false;
            CameraTools.SetPlayerInput(Controls.Owner == PadOwner.Game, "Typing");
        }

        private static void Type()
        {
            foreach (var (key, typed) in TypedKeys)
                if (Input.GetKeyDown(key) && editing.Buffer.Length < MaxTyped)
                    editing.Buffer += typed;
            if (Input.GetKeyDown(KeyCode.Backspace) && editing.Buffer.Length > 0)
                editing.Buffer = editing.Buffer[..^1];
            if (Input.GetKeyDown(KeyCode.Return) || Input.GetKeyDown(KeyCode.KeypadEnter))
                Commit();
            else if (Input.GetKeyDown(KeyCode.Escape))
                StopEditing();
        }

        // An empty field keeps the slot as it was.
        private static void Commit()
        {
            var view = editing;
            string text = view.Buffer;
            StopEditing();
            if (text.Length == 0)
                return;
            int slot = ((ResolutionRow)view.Row).Slot;
            if (!ScreenSize.TryParse(text, out var size))
            {
                Toast($"Type a size like 2560x1600, from {ScreenSize.MinWidth}x{ScreenSize.MinHeight} to {ScreenSize.MaxSide}x{ScreenSize.MaxSide}");
                return;
            }
            Graphics.SetSlot(slot, size);
            Toast($"Slot {slot}: {size.Display}");
        }

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
            // The wheel steps the slider under the pointer, and scrolls the list anywhere else in it.
            float wheel = Input.mouseScrollDelta.y;
            if (wheel != 0f && row >= 0 && CurrentRows[row].Row is SliderRow { Usable: true } wheeled)
                Step(wheeled, wheel > 0f ? 1 : -1);
            else if (wheel != 0f && live.ShownTab == tab && Contains(live.List, point))
                ScrollTo(live, live.Tabs[tab], live.Tabs[tab].Scroll - wheel * Style.WheelStep);
            else if (wheel != 0f && Contains(live.TabClip, point))
                live.StripScroll -= wheel * Style.WheelStep;
            if (Input.GetMouseButtonDown(0))
                Click(point, row);
            if (dragging == null)
                return;
            if (Input.GetMouseButton(0) && dragging.Row.Usable)
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
            // Clicking anywhere but the field being typed in saves it, as leaving a text field does.
            if (editing != null && !Contains(editing.Value.rectTransform, point))
                Commit();
            // A tab scrolled out of the strip's viewport is not there to click.
            int tabIndex = Contains(live.TabClip, point) ? Array.FindIndex(live.Tabs, view => Contains(view.Button, point)) : -1;
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
            if (!CurrentRows[row].Row.Usable)
                return;
            // A slot's label only selects it: its size starts typing, and its button applies it.
            switch (CurrentRows[row])
            {
                case ToggleView { Row: ToggleRow toggle }:
                    toggle.Set(!toggle.Get());
                    break;
                case SliderView slider when Contains(slider.Track, point):
                    dragging = slider;
                    break;
                case ActionView action:
                    Run(action);
                    break;
                case ArrowsView arrows when Contains(arrows.Previous.rectTransform, point):
                    Change(arrows.Row, -1);
                    break;
                case ArrowsView arrows when Contains(arrows.Next.rectTransform, point):
                    Change(arrows.Row, 1);
                    break;
                case ResolutionView { Row: ResolutionRow slot } resolution when Contains(resolution.Apply, point):
                    Graphics.ApplySlot(slot.Slot);
                    break;
                case ResolutionView resolution when Contains(resolution.Value.rectTransform, point):
                    StartEditing(resolution);
                    break;
            }
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
            ExpireConfirm();
            var view = View;
            float deltaTime = Time.unscaledDeltaTime;
            // Values that changed while the panel or their tab was hidden show without animating.
            bool opened = view == View.Panel && live.Shown != View.Panel;
            live.Shown = view;
            RenderFocus(view, deltaTime);
            live.Hud.Update(view == View.Hud, deltaTime);
            live.PlayBar.Update(view == View.Playing, deltaTime);
            live.Panel.Update(view == View.Panel, deltaTime);
            var layout = Controls.Layout;
            if (live.Layout != layout)
            {
                live.Layout = layout;
                RenderLayout(layout == InputDevice.Pad);
            }
            if (live.Hud.Visible)
                RenderFov();
            if (live.PlayBar.Visible)
                RenderPlayBar();
            if (live.Panel.Visible)
                RenderPanel(opened, deltaTime);
            RenderToast(deltaTime);
        }

        // The focus point also shows beside the open panel, so the Focus point rows can be seen to move it. The window is
        // placed by its fractions of the canvas, which are its fractions of the screen at any canvas scale.
        private static void RenderFocus(View view, float deltaTime)
        {
            var area = live.Canvas.rect;
            var window = view is View.Hud or View.Panel && area.height > 0f ? DepthOfField.FocusWindow(area.width / area.height) : null;
            live.Focus.Update(window != null, deltaTime);
            if (window is not { } box || live.FocusShown == box)
                return;
            live.FocusShown = box;
            Place(live.FocusWindow, V(box.Left, 1f - box.Bottom), V(box.Right, 1f - box.Top), V(0.5f, 0.5f), V(0f, 0f), V(0f, 0f));
        }

        // Like Genshin on PC, the keyboard layout has a back button and no key hints in the panel.
        private static void RenderLayout(bool pad)
        {
            foreach (var key in live.TabKeys)
                key.SetActive(pad);
            live.Back.gameObject.SetActive(!pad);
            live.Footer.SetActive(pad);
            float glyph = Style.TabBarPadding + Shapes.Bumper.Width + Style.TabClipGap;
            live.TabClip.offsetMin = V(pad ? glyph : Style.TabsAfterBack, 0f);
            live.TabClip.offsetMax = V(pad ? -glyph : -Style.TabBarPadding, 0f);
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

        // The fill follows time, and each tick marks when the camera reaches a node. The bar keeps its last frame while it
        // fades out after playback.
        private static void RenderPlayBar()
        {
            if (PathPlayback.State is not PlaybackState.Playing playing)
                return;
            if (live.PlayShown != playing.Sampler)
            {
                live.PlayShown = playing.Sampler;
                var times = playing.Sampler.NodeTimes(playing.EaseIn, playing.EaseOut);
                while (live.Ticks.Count < times.Length)
                    live.Ticks.Add(Picture(live.PlayRail, "Tick", null, Style.Tick).rectTransform);
                for (int index = 0; index < live.Ticks.Count; index++)
                {
                    var tick = live.Ticks[index];
                    tick.gameObject.SetActive(index < times.Length);
                    if (index < times.Length)
                        Place(tick, V(times[index], 0.5f), V(times[index], 0.5f), V(0.5f, 0.5f), V(0f, 0f), V(Style.TickWidth, Style.TickHeight));
                }
                // As wide as this playback's longest text, so the rail does not move as the seconds count up.
                live.PlayMeta.text = PlayText(playing.Number, playing.Duration, playing.Duration);
                live.PlayMetaWidth.preferredWidth = live.PlayMeta.preferredWidth + Style.PlayMetaSlack;
                live.PlayText = null;
            }
            float seconds = playing.PassTime;
            string text = PlayText(playing.Number, seconds, playing.Duration);
            if (text != live.PlayText)
            {
                live.PlayMeta.text = text;
                live.PlayText = text;
            }
            live.PlayFill.anchorMax = V(seconds / playing.Duration, 0.5f);
        }

        private static string PlayText(int number, float seconds, float duration) => $"Path {number} · {seconds:0.0} / {duration:0.0} s";

        // A tab switch fades the shown rows out, swaps in the selected tab's, and fades them in.
        private static void RenderPanel(bool opened, float deltaTime)
        {
            for (int index = 0; index < live.Tabs.Length; index++)
                live.Tabs[index].Show(index == tab, opened ? 1f : deltaTime / Style.TabFade);
            RenderStrip(opened, deltaTime);
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

            var shownTab = live.Tabs[live.ShownTab];
            ScrollTo(live, shownTab, shownTab.Scroll);
            RenderThumb(shownTab);
            var rows = shownTab.RowViews;
            float step = opened || swapped ? 1f : deltaTime / Style.SwitchSlide;
            foreach (var row in rows)
                row.Sync(step);
            if (live.ShownTab == tab && live.ShownSelected != selected)
            {
                for (int index = 0; index < rows.Count; index++)
                    rows[index].Select(index == selected);
                live.ShownSelected = selected;
            }
            // The footer names what the buttons do to the selected row.
            var chosen = selected >= 0 ? CurrentRows[selected].Row : null;
            var footer = editing != null ? new Footer("Save", null, false, "Cancel") : (chosen is { Usable: true } ? chosen : null) switch
            {
                ToggleRow toggle => new Footer(toggle.Get() ? "Off" : "On", null, false, "Return"),
                ActionRow action => new Footer(action.Hint, null, false, "Return"),
                ChoiceRow => new Footer(null, "Change", false, "Return"),
                StepperRow => new Footer(null, "Browse", false, "Return"),
                ResolutionRow => new Footer("Apply", null, true, "Return"),
                _ => new Footer(null, null, false, "Return"),
            };
            if (footer == live.FooterShown)
                return;
            live.Change.Root.SetActive(footer.A != null);
            if (footer.A != null)
                live.Change.Label.text = footer.A;
            live.Steps.Root.SetActive(footer.Steps != null);
            if (footer.Steps != null)
                live.Steps.Label.text = footer.Steps;
            live.Edit.Root.SetActive(footer.Edit);
            live.Return.Label.text = footer.B;
            live.FooterShown = footer;
        }

        // Tabs that fit sit in the middle for the pad and after the back button for the keyboard. Tabs that do not fit
        // scroll, and a tab change brings the selected tab into view. The tabs' sizes come from the layout, which runs
        // after the panel is first shown, so the strip is placed from the frame after that.
        private static void RenderStrip(bool opened, float deltaTime)
        {
            float content = Right(live.Tabs[^1].Button);
            float room = live.TabClip.rect.width;
            if (content <= 0f || room <= 0f)
                return;
            bool resized = live.StripLaidOut != (content, room);
            if (resized || live.StripReveal)
            {
                live.StripLaidOut = (content, room);
                live.StripReveal = false;
                var button = live.Tabs[tab].Button;
                float left = Left(button) - Style.TabPeek;
                float right = Right(button) + Style.TabPeek;
                if (left < live.StripScroll)
                    live.StripScroll = left;
                else if (right > live.StripScroll + room)
                    live.StripScroll = right - room;
            }
            live.StripScroll = Math.Clamp(live.StripScroll, 0f, Math.Max(content - room, 0f));
            float target = MathF.Round(content > room ? -live.StripScroll : live.Layout == InputDevice.Pad ? (room - content) / 2f : 0f);
            float shown = opened || resized || Math.Abs(target - live.StripShown) < 0.5f ? target
                : live.StripShown + (target - live.StripShown) * Math.Min(deltaTime / Style.TabSlide, 1f);
            if (shown == live.StripShown)
                return;
            live.StripShown = shown;
            live.Strip.anchoredPosition = V(shown, 0f);
        }

        // A tab's edges from the strip's left edge.
        private static float Left(RectTransform button) => button.localPosition.x + button.rect.xMin;

        private static float Right(RectTransform button) => button.localPosition.x + button.rect.xMax;

        private static void RenderThumb(TabView view)
        {
            float height = ViewHeight(live);
            var shown = (view.Scroll, view.Height, height);
            if (live.ThumbShown == shown)
                return;
            live.ThumbShown = shown;
            bool scrolls = view.Height > height;
            live.Thumb.gameObject.SetActive(scrolls);
            if (!scrolls)
                return;
            float size = Math.Max(height * height / view.Height, Style.ThumbMin);
            float t = view.Scroll / (view.Height - height);
            Pin(live.Thumb, 1f, 1f, -Style.ThumbRight, -(Style.ListPadding + t * (height - size)), Style.ThumbWidth, size);
        }

        private static void RenderToast(float deltaTime)
        {
            bool show = (toastWhileHidden || !CameraTools.uiHidden) && Time.unscaledTime < toastUntil;
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

        // A: what A does, or null. Steps: what D-pad left and right do to the row, or null. Edit: Y starts typing. B: what B
        // does.
        private readonly record struct Footer(string A, string Steps, bool Edit, string B);

        // A hint's controller and keyboard versions; only one shows, and only if the action has a binding for it.
        private sealed record Legend(HintView Pad, bool HasPad, HintView Key, bool HasKey);

        private sealed class Live
        {
            public GameObject Root;
            public RectTransform Canvas;
            public Font Font;
            public Fader Hud;
            public Fader Focus;
            public RectTransform FocusWindow;
            public ScreenBox? FocusShown;
            public Fader Panel;
            public Fader Toast;
            public Legend[] Legends;
            public RectTransform FovHandle;
            public GameObject[] TabKeys;
            public RectTransform Back;
            public RectTransform TabClip;
            public RectTransform Strip;
            // How far the strip is scrolled, and where it is drawn while it slides there.
            public float StripScroll;
            public float StripShown;
            public bool StripReveal;
            // The tabs' width and the viewport's when the strip was last placed.
            public (float Content, float Room)? StripLaidOut;
            public TabView[] Tabs;
            public RectTransform List;
            public CanvasGroup Rows;
            public GameObject Footer;
            public HintView Change;
            public HintView Steps;
            public HintView Edit;
            public HintView Return;
            public RectTransform Thumb;
            public Fader PlayBar;
            public Text PlayMeta;
            public LayoutElement PlayMetaWidth;
            public RectTransform PlayRail;
            public RectTransform PlayFill;
            public readonly List<RectTransform> Ticks = new();
            // The playback the play bar was laid out for.
            public PathSampler PlayShown;
            public string PlayText;
            // The game's option lists could not be read when this UI was built, so its Graphics tab has no settings rows.
            public bool MissingSettings;
            public Text ToastLabel;
            public View? Shown;
            public InputDevice? Layout;
            public float FovShown = float.NaN;
            public int ShownTab;
            public Tween RowsFade = new() { Value = 1f };
            public float RowsAlpha = 1f;
            public int ShownSelected = -1;
            public Footer? FooterShown;
            public (float Scroll, float Height, float View)? ThumbShown;
            public string ToastText;
        }
    }
}
