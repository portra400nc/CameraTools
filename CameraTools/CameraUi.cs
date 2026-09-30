using Il2CppInterop.Runtime;
using UnityEngine;
using UnityEngine.UI;
using Object = UnityEngine.Object;

namespace CameraTools
{
    // CameraTools' UI mimics Genshin's photo mode page. It is built once from the recorded layout on a canvas of its own,
    // which survives scene changes. Genshin cannot run custom MonoBehaviours, so it holds only plain graphics and layout
    // components: CameraTools reads the mouse and the pad itself, and writes each setting's value, the selection, and every
    // animation frame to the UI.
    internal static class CameraUi
    {
        private const string LoadHint = "Open photo mode or the map once to load the CameraTools UI";
        private const string ReloadHint = "The CameraTools UI failed; it reloads in a few seconds";
        private const float RepeatDelay = 0.4f;
        private const float RepeatInterval = 0.08f;
        private const float RetryInterval = 10f;
        private const float FollowInterval = 1f;
        private const float FadeTime = 0.15f;
        private const int SortingOrder = 30000;

        private const string SetUp = "GrpCom/GrpLeft/GrpSetUp";
        private const string Rows = SetUp + "/Content/ScrollView/Content";
        private const string TabRow = "GrpCom/GrpTab/Tab/Viewport/Tab";
        private const string PadBottom = "GrpCom/GrpAction_PS4";
        private const string PadTop = "GrpCom/GrpActionTop_PS4";
        private const string KeyBottom = "GrpCom/GrpAction_PC";
        private const string ZoomBar = "GrpCom/GrpMain/Zoom_Slider";
        private const string Switch = "Content/GrpToggle/Btn_Toggle/Content";
        private const string Bar = "Content/Slider_W/Content";

        private static readonly string[] TabButtons = { "BtnSetup_PC", "BtnAct_PC", "BtnEmot_PC", "BtnActionAndEmot_PC" };
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
        private static RowView dragging;

        public static View View => !CameraTools.freecamActive || CameraTools.uiHidden ? View.Hidden : panelOpen ? View.Panel : View.Hud;

        // Drawn with IMGUI until the UI has been built.
        public static string FallbackToast => live == null && Time.unscaledTime < toastUntil ? toast : null;

        public static void Toast(string message)
        {
            toast = message;
            toastUntil = Time.unscaledTime + 2f;
        }

        private static int repairedVersion = -1;

        private static string MissingHint => Assets.LayoutReady ? ReloadHint : LoadHint;

        public static void FreecamChanged(bool active)
        {
            panelOpen = false;
            if (!active)
                return;
            Assets.ScanSoon();
            hintPending = true;
        }

        public static void Update()
        {
            string step = "asset scan";
            try
            {
                Assets.Update(CameraTools.freecamActive,
                    CameraTools.freecamActive && Controls.Layout == InputDevice.Pad && View != View.Hidden);
                step = "build";
                if (!Ensure())
                {
                    if (hintPending || CameraTools.freecamActive && Controls.Pressed(CamAction.ToggleGUI))
                        Toast(MissingHint);
                    hintPending = false;
                    return;
                }
                hintPending = false;
                step = "layout";
                if (repairedVersion != Assets.Version)
                {
                    PhotoLayout.Repair();
                    repairedVersion = Assets.Version;
                }
                Follow();
                step = "input";
                HandleInput();
                step = "render";
                Render();
            }
            catch (Exception e)
            {
                failures++;
                CameraTools.LogOnce($"UI: {step} failed (failure {failures}); CameraTools rebuilds its UI from the layout "
                    + $"in {RetryInterval:0} s. {e}");
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
                return live.Root ? true : throw new InvalidOperationException("The CameraTools canvas was destroyed.");
            if (Time.unscaledTime < nextBuild || !Assets.LayoutReady)
                return false;
            live = Build(PhotoLayout.Current);
            builds++;
            CameraTools.LogOnce($"UI: built the CameraTools UI (build {builds}).");
            return true;
        }

        private static Live Build(PhotoLayout layout)
        {
            var root = new GameObject("CameraTools UI");
            try
            {
                Object.DontDestroyOnLoad(root);
                var canvas = root.AddComponent<Canvas>();
                canvas.renderMode = RenderMode.ScreenSpaceOverlay;
                canvas.sortingOrder = SortingOrder;
                // CameraTools hit-tests the mouse itself. The raycaster only tells the game's EventSystem that the pointer is over
                // UI, so a click on the panel is not also a click in the world, as it was for the copied page under /Canvas.
                root.AddComponent<GraphicRaycaster>();
                var scaler = root.AddComponent<CanvasScaler>();
                scaler.uiScaleMode = CanvasScaler.ScaleMode.ScaleWithScreenSize;
                scaler.referenceResolution = layout.ReferenceResolution;
                scaler.screenMatchMode = layout.MatchMode;
                scaler.matchWidthOrHeight = layout.Match;
                scaler.referencePixelsPerUnit = layout.ReferencePixelsPerUnit;
                var pages = new GameObject("Pages").AddComponent<RectTransform>();
                pages.SetParent(root.transform, false);
                PhotoLayout.Apply(pages, layout.Pages);

                var recorded = layout.Root;
                var page = PhotoLayout.Build(Page(recorded), pages);
                var com = Child(page, "GrpCom");
                var setUp = Child(page, SetUp);
                ShowOnly(setUp, "GrpBg", "Content", "GrpAction_PS4");
                ShowOnly(Child(setUp, "GrpAction_PS4"), "BtnChange_PS4", "BtnReturn_PS4");
                var tabKeys = Child(com, "GrpTab/Tab/Tab_Key");
                var toastPanel = Child(com, "Reminder_1/ShowPanel").gameObject;
                toastPanel.SetActive(false);

                var tabRow = Child(page, TabRow);
                if (UiModel.Tabs.Length > TabButtons.Length)
                    throw new InvalidOperationException($"The page has {TabButtons.Length} tab buttons for {UiModel.Tabs.Length} tabs.");
                ShowOnly(tabRow, TabButtons.Take(UiModel.Tabs.Length).ToArray());

                var rows = Child(page, Rows);
                var templates = RowTemplates.Of(recorded.Find(Rows));
                var built = new Live
                {
                    Root = root,
                    Pages = pages,
                    Scaler = scaler,
                    Hud = new[] { Child(com, "GrpMain").gameObject, Child(page, PadTop).gameObject, Child(page, PadBottom).gameObject,
                        Child(page, KeyBottom).gameObject },
                    Panel = new[] { Child(com, "GrpLeft").gameObject, Child(com, "GrpTab").gameObject },
                    RowsViewport = RectOf(Child(setUp, "Content/ScrollView")),
                    // The snapshots show photo mode with its first tab selected.
                    SelectedTab = TabLook.Of(recorded.Find(TabRow + "/" + TabButtons[0] + "/Content")),
                    OtherTab = TabLook.Of(recorded.Find(TabRow + "/" + TabButtons[1] + "/Content")),
                    SwitchOff = SwitchLook.Of(templates.ToggleOff.Find(Switch)),
                    SwitchOn = SwitchLook.Of(templates.ToggleOn.Find(Switch)),
                    TabKeys = tabKeys.gameObject,
                    TabKeyLeft = TabKey(tabKeys, "KeyL1"),
                    TabKeyRight = TabKey(tabKeys, "KeyR1"),
                    Back = Child(com, "GrpTab/BtnBack").gameObject,
                    BackButton = RectOf(Child(com, "GrpTab/BtnBack/Content")),
                    Footer = Child(setUp, "GrpAction_PS4").gameObject,
                    Change = PadEntry.Of(Child(setUp, "GrpAction_PS4/BtnChange_PS4").gameObject),
                    Return = PadEntry.Of(Child(setUp, "GrpAction_PS4/BtnReturn_PS4").gameObject),
                    Toast = toastPanel,
                    ToastLabel = Get<Text>(toastPanel.transform, "Desc/Text"),
                    ZoomHandle = RectOf(Child(page, ZoomBar + "/HandleSlideArea")),
                    ZoomHandleBox = recorded.Find(ZoomBar + "/HandleSlideArea").Box,
                };
                built.Legends = Legends(UiModel.BottomHints, recorded.Find(PadBottom + "/BtnHideUI_PS4"), Child(page, PadBottom),
                        recorded.Find(KeyBottom + "/BtnHideUI_PC"), Child(page, KeyBottom))
                    .Concat(Legends(UiModel.TopHints, recorded.Find(PadTop + "/BtnCameraPush_PS4"), Child(page, PadTop),
                        recorded.Find(KeyBottom + "/BtnHideUI_PC"), Child(page, PadTop)))
                    .ToArray();
                built.Tabs = UiModel.Tabs.Select((model, index) => BindTab(built, model, Child(tabRow, TabButtons[index] + "/Content"),
                    templates, rows)).ToArray();
                ShowTab(built, Math.Min(tab, UiModel.Tabs.Length - 1));
                foreach (var tabView in built.Tabs)
                {
                    tabView.Shown = tabView.Selected;
                    ApplyTab(built, tabView);
                }
                return built;
            }
            catch
            {
                Object.Destroy(root);
                throw;
            }
        }

        // The recorded page, changed where CameraTools draws its own parts or the snapshots caught the game mid-animation.
        private static Node Page(Node recorded) => recorded
            // Rows and legend entries are built per setting and per hint from the recorded ones.
            .With(Rows, rows => rows with { Children = Array.Empty<Node>() })
            .With(PadBottom, group => group with { Children = Array.Empty<Node>() })
            .With(PadTop, group => group with { Children = Array.Empty<Node>() })
            .With(KeyBottom, group => group with { Children = Array.Empty<Node>() })
            // The game fades the zoom bar out while its settings panel is open, as it was in the snapshots.
            .With(ZoomBar, bar => bar with { GroupAlpha = 1f });

        private static RectTransform RectOf(Transform transform) => transform.TryCast<RectTransform>();

        private static Transform Child(Transform root, string path)
        {
            var child = root.Find(path);
            return child ? child : throw new InvalidOperationException($"{root.name}/{path} is missing.");
        }

        private static T Get<T>(Transform root, string path) where T : Component
        {
            var component = Child(root, path).GetComponent(Il2CppType.Of<T>());
            return component ? component.TryCast<T>() : throw new InvalidOperationException($"{root.name}/{path} has no {typeof(T).Name}.");
        }

        private static void ShowOnly(Transform parent, params string[] names)
        {
            for (int index = 0; index < parent.childCount; index++)
            {
                var child = parent.GetChild(index).gameObject;
                child.SetActive(names.Contains(child.name));
            }
        }

        // Each hint gets a pad legend entry and a keyboard one, built from the recorded entries.
        private static IEnumerable<Legend> Legends(Hint[] hints, Node pad, Transform padGroup, Node keyboard, Transform keyGroup)
            => hints.Select(hint =>
            {
                var legend = new Legend(hint, PadEntry.Of(PhotoLayout.Build(pad, padGroup).gameObject),
                    KeyEntry.Of(PhotoLayout.Build(keyboard, keyGroup).gameObject));
                legend.Keyboard.Key.text = KeyName(Controls.Binding(hint.Action).Key);
                legend.Keyboard.Label.text = hint.Label;
                return legend;
            });

        private static string KeyName(KeyCode key)
            => KeyNames.TryGetValue(key, out var name) ? name
                : key is >= KeyCode.Alpha0 and <= KeyCode.Alpha9 ? ((int)(key - KeyCode.Alpha0)).ToString()
                : key.ToString();

        // The tab bar's LB and RB keys show a glyph, or a keycap with the button's name.
        private static Transform TabKey(Transform tabKeys, string name)
        {
            var key = Child(tabKeys, name);
            key.gameObject.SetActive(true);
            var content = Child(key, "Content");
            ShowOnly(content, "Key_Group", "Key_PC");
            ShowOnly(Child(content, "Key_Group"), "Icon1");
            ShowOnly(Child(content, "Key_PC"), "Text");
            return content;
        }

        private static void ShowTabKey(Transform content, PadButtons button)
        {
            var glyph = Assets.Glyph(button);
            Child(content, "Key_Group").gameObject.SetActive(glyph);
            Child(content, "Key_PC").gameObject.SetActive(!glyph);
            if (glyph)
            {
                var icon = Get<Image>(content, "Key_Group/Icon1");
                icon.sprite = glyph;
                icon.enabled = true;
            }
            else
                Get<Text>(content, "Key_PC/Text").text = button.ToString();
        }

        // Every tab's rows are built once; switching tabs shows one tab's rows.
        private static TabView BindTab(Live target, Tab model, Transform button, RowTemplates templates, Transform rows)
        {
            var view = new TabView
            {
                Button = RectOf(button),
                Bg = Get<Image>(button, "ImgBg"),
                Icon = Get<Image>(button, "Icon"),
                Line = Get<Image>(button, "ImgLine"),
            };
            foreach (var row in model.Rows)
            {
                if (row is Section && view.Rows.Count > 0)
                    view.Objects.Add(PhotoLayout.Build(templates.Space, rows).gameObject);
                var bound = BindRow(target, row, templates, rows);
                view.Objects.Add(bound.Root);
                view.Rows.Add(bound);
            }
            return view;
        }

        private static RowView BindRow(Live target, Row row, RowTemplates templates, Transform rows)
        {
            var template = row switch
            {
                Section => templates.Section,
                ToggleRow => templates.ToggleOff,
                SliderRow => templates.Slider,
                _ => throw new InvalidOperationException($"No template for {row.GetType().Name}."),
            };
            var transform = PhotoLayout.Build(template, rows);
            var view = new RowView { Row = row, Root = transform.gameObject };
            if (row is Section)
            {
                transform.GetComponent<Text>().text = row.Label;
                return view;
            }

            view.Rect = transform;
            view.Highlight = Child(transform, "Content/ImgHighlight").gameObject;
            view.Arrow = Child(transform, "Content/Selected_Arrow/ClickTips").gameObject;
            view.Label = Get<Text>(transform, "Content/Text");
            if (row is ToggleRow toggle)
            {
                var content = Child(transform, Switch);
                view.Knob = RectOf(Child(content, "Image"));
                view.Fill = Get<Image>(content, "ImgColor");
                view.IconOn = Child(content, "Image/IconOn");
                view.IconOff = Child(content, "Image/IconOff");
                view.Light = Get<Image>(content, "Image/ImgLight");
                view.Label.text = row.Label;
                view.Shown = toggle.Get() ? 1f : 0f;
                ApplySwitch(target, view);
            }
            else if (row is SliderRow slider)
            {
                view.Track = RectOf(Child(transform, Bar));
                view.HandleArea = RectOf(Child(transform, Bar + "/HandleSlideArea"));
                view.BarFill = RectOf(Child(transform, Bar + "/FillArea/Fill"));
                view.Handle = RectOf(Child(transform, Bar + "/HandleSlideArea/Handle"));
                WriteSlider(view, slider, slider.Get());
            }
            view.Highlight.SetActive(false);
            view.Arrow.SetActive(false);
            return view;
        }

        private static void ShowTab(Live target, int index)
        {
            tab = index;
            dragging = null;
            for (int other = 0; other < target.Tabs.Length; other++)
            {
                foreach (var part in target.Tabs[other].Objects)
                    part.SetActive(other == index);
                target.Tabs[other].Selected = other == index ? 1f : 0f;
            }
            selected = target.Tabs[index].Rows.FindIndex(view => view.Row is not Section);
            target.ShownSelected = -1;
        }

        private static List<RowView> CurrentRows => live.Tabs[tab].Rows;

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
                ShowTab(live, (tab + tabs - 1) % tabs);
            if (PadRB.Pressed(now, before))
                ShowTab(live, (tab + 1) % tabs);

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
            var point = new Vector2(mouse.x, mouse.y);
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

        // Rows scrolled out of the list's viewport do not count.
        private static int RowAt(Vector2 point)
            => Contains(live.RowsViewport, point)
                ? CurrentRows.FindIndex(view => view.Row is not Section && Contains(view.Rect, point))
                : -1;

        private static void Click(Vector2 point, int row)
        {
            int tabIndex = Array.FindIndex(live.Tabs, view => Contains(view.Button, point));
            bool back = live.Back.activeSelf && Contains(live.BackButton, point);
            if (tabIndex < 0 && !back && row < 0)
                return;
            CameraTools.LogOnce("UI: CameraTools handled a mouse click on its panel.");
            if (tabIndex >= 0)
            {
                if (tabIndex != tab)
                    ShowTab(live, tabIndex);
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
            else if (Contains(view.Track, point))
                dragging = view;
        }

        // The interop passes ScreenPointToLocalPointInRectangle's out value by value, so the handle's travel is projected to
        // the screen instead.
        private static void Drag(RowView view, Vector2 point)
        {
            var rect = view.HandleArea;
            var area = rect.rect;
            float left = RectTransformUtility.WorldToScreenPoint(null, rect.TransformPoint(new Vector3(area.xMin, 0f, 0f))).x;
            float right = RectTransformUtility.WorldToScreenPoint(null, rect.TransformPoint(new Vector3(area.xMax, 0f, 0f))).x;
            if (right <= left)
                return;
            float t = Math.Clamp((point.x - left) / (right - left), 0f, 1f);
            var slider = (SliderRow)view.Row;
            slider.Set(slider.Min + (slider.Max - slider.Min) * t);
        }

        // The game's layout adaptor insets /Canvas/Pages, and the game changes its canvas scaling with the input device it
        // shows hints for. CameraTools' canvas follows both, and keeps the recorded values until the game's canvas exists.
        private static void Follow()
        {
            if (Time.unscaledTime < live.NextFollow)
                return;
            live.NextFollow = Time.unscaledTime + FollowInterval;
            var canvas = GameObject.Find("/Canvas");
            var source = canvas ? canvas.transform.Find("Pages") : null;
            var scaler = canvas ? canvas.GetComponent<CanvasScaler>() : null;
            if (!source || !scaler)
                return;
            var pages = RectOf(source);
            Vector2 anchorMin = pages.anchorMin, anchorMax = pages.anchorMax, pivot = pages.pivot;
            Vector2 position = pages.anchoredPosition, size = pages.sizeDelta, reference = scaler.referenceResolution;
            var mode = scaler.screenMatchMode;
            float match = scaler.matchWidthOrHeight;
            string followed = $"reference {reference.x}x{reference.y} ({mode}, match {match}); pages anchors ({anchorMin.x}, "
                + $"{anchorMin.y})-({anchorMax.x}, {anchorMax.y}), pivot ({pivot.x}, {pivot.y}), position ({position.x}, "
                + $"{position.y}), size delta ({size.x}, {size.y})";
            if (followed == live.Followed)
                return;
            live.Followed = followed;
            CameraTools.LogOnce($"UI: following the game's layout: {followed}.");
            live.Scaler.referenceResolution = reference;
            live.Scaler.screenMatchMode = mode;
            live.Scaler.matchWidthOrHeight = match;
            live.Pages.anchorMin = anchorMin;
            live.Pages.anchorMax = anchorMax;
            live.Pages.pivot = pivot;
            live.Pages.anchoredPosition = position;
            live.Pages.sizeDelta = size;
        }

        private static void Render()
        {
            var view = View;
            bool opened = live.Shown != view && view == View.Panel;
            if (live.Shown != view)
            {
                foreach (var part in live.Hud)
                    part.SetActive(view == View.Hud);
                foreach (var part in live.Panel)
                    part.SetActive(view == View.Panel);
                live.Shown = view;
            }
            var layout = Controls.Layout;
            if (live.ShownLayout != (layout, Assets.Version))
            {
                // A glyph the game had not loaded at the last scan may be loaded now.
                if (layout == InputDevice.Pad && live.ShownLayout?.Item1 != InputDevice.Pad)
                    Assets.ScanSoon();
                live.ShownLayout = (layout, Assets.Version);
                RenderLayout(layout);
            }
            if (view == View.Hud)
            {
                var fov = CameraTools.settings.Fov;
                if (live.FovShown != fov.Value)
                {
                    // BtnZoomNear sits at the bar's top, so a narrow field of view moves the handle up.
                    float value = (fov.Max - fov.Value) / (fov.Max - fov.Min);
                    var box = live.ZoomHandleBox;
                    live.ZoomHandle.anchorMin = new Vector2 { x = box.AnchorMin.x, y = value };
                    live.ZoomHandle.anchorMax = new Vector2 { x = box.AnchorMax.x, y = value };
                    live.FovShown = fov.Value;
                }
            }
            if (view == View.Panel)
            {
                // Values that changed while the panel or their tab was hidden show without animating.
                float step = opened ? 1f : Time.unscaledDeltaTime / FadeTime;
                float rowStep = live.RenderedTab != tab ? 1f : step;
                live.RenderedTab = tab;
                foreach (var row in CurrentRows)
                    Sync(row, rowStep);
                foreach (var tabView in live.Tabs)
                {
                    if (tabView.Shown == tabView.Selected)
                        continue;
                    tabView.Shown = MoveTowards(tabView.Shown, tabView.Selected, step);
                    ApplyTab(live, tabView);
                }
                RenderSelection();
            }

            bool showToast = !CameraTools.uiHidden && Time.unscaledTime < toastUntil;
            if (showToast && live.ToastText != toast)
            {
                live.ToastLabel.text = toast;
                live.ToastText = toast;
            }
            if (live.ToastShown != showToast)
            {
                live.Toast.SetActive(showToast);
                live.ToastShown = showToast;
            }
        }

        // Like Genshin on PC, the keyboard layout has a back button and no key hints in the panel.
        private static void RenderLayout(InputDevice layout)
        {
            bool pad = layout == InputDevice.Pad;
            live.TabKeys.SetActive(pad);
            live.Footer.SetActive(pad);
            live.Back.SetActive(!pad);
            if (pad)
            {
                ShowTabKey(live.TabKeyLeft, PadButtons.LB);
                ShowTabKey(live.TabKeyRight, PadButtons.RB);
                ShowPadKey(live.Return, PadButtons.B, "B", "Return");
            }
            foreach (var legend in live.Legends)
            {
                var (key, binding) = Controls.Binding(legend.Hint.Action);
                legend.Keyboard.Root.SetActive(!pad && key != KeyCode.None);
                legend.Pad.Root.SetActive(pad && binding != default);
                if (pad)
                    ShowPadKey(legend.Pad, GlyphButton(binding), binding.ToString(), legend.Hint.Label);
            }
            // The footer's A glyph may have changed.
            live.ChangeText = "";
        }

        // CameraTools owns every value, so the widgets only show them.
        private static void Sync(RowView view, float step)
        {
            switch (view.Row)
            {
                case ToggleRow toggle:
                {
                    float target = toggle.Get() ? 1f : 0f;
                    if (view.Shown == target)
                        break;
                    view.Shown = MoveTowards(view.Shown, target, step);
                    ApplySwitch(live, view);
                    break;
                }
                case SliderRow slider:
                {
                    float value = slider.Get();
                    if (value != view.Value)
                        WriteSlider(view, slider, value);
                    break;
                }
            }
        }

        // Hotkeys can push game speed past the row's range; the bar stops at its end and the label shows the real value.
        private static void WriteSlider(RowView view, SliderRow slider, float value)
        {
            view.Value = value;
            float t = Math.Clamp((value - slider.Min) / (slider.Max - slider.Min), 0f, 1f);
            view.BarFill.anchorMin = new Vector2 { x = 0f, y = 0f };
            view.BarFill.anchorMax = new Vector2 { x = t, y = 1f };
            view.Handle.anchorMin = new Vector2 { x = t, y = 0f };
            view.Handle.anchorMax = new Vector2 { x = t, y = 1f };
            string text = $"{slider.Label}  {value.ToString(slider.Format)}";
            if (text != view.Text)
            {
                view.Label.text = text;
                view.Text = text;
            }
        }

        // The switch slides between the recorded off and on rows.
        private static void ApplySwitch(Live target, RowView view)
        {
            float t = Ease(view.Shown);
            var off = target.SwitchOff;
            var on = target.SwitchOn;
            view.Knob.anchorMin = Lerp(off.Knob.AnchorMin, on.Knob.AnchorMin, t);
            view.Knob.anchorMax = Lerp(off.Knob.AnchorMax, on.Knob.AnchorMax, t);
            view.Knob.pivot = Lerp(off.Knob.Pivot, on.Knob.Pivot, t);
            view.Knob.anchoredPosition = Lerp(off.Knob.Position, on.Knob.Position, t);
            view.Fill.color = Lerp(off.Fill, on.Fill, t);
            view.Light.color = Lerp(off.Light, on.Light, t);
            view.IconOn.localScale = Lerp(off.IconOn, on.IconOn, t);
            view.IconOff.localScale = Lerp(off.IconOff, on.IconOff, t);
        }

        // Tabs fade between the recorded selected and unselected tab.
        private static void ApplyTab(Live target, TabView view)
        {
            float t = Ease(view.Shown);
            var other = target.OtherTab;
            var selectedTab = target.SelectedTab;
            view.Bg.color = Lerp(other.Bg, selectedTab.Bg, t);
            view.Bg.rectTransform.localScale = Lerp(other.BgScale, selectedTab.BgScale, t);
            view.Icon.color = Lerp(other.Icon, selectedTab.Icon, t);
            view.Icon.rectTransform.localScale = Lerp(other.IconScale, selectedTab.IconScale, t);
            view.Line.color = Lerp(other.Line, selectedTab.Line, t);
            view.Line.rectTransform.sizeDelta = Lerp(other.LineSize, selectedTab.LineSize, t);
        }

        private static void RenderSelection()
        {
            var rows = CurrentRows;
            if (live.ShownSelected != selected)
            {
                for (int index = 0; index < rows.Count; index++)
                {
                    var row = rows[index];
                    if (row.Row is Section)
                        continue;
                    row.Highlight.SetActive(index == selected);
                    row.Arrow.SetActive(index == selected);
                }
                live.ShownSelected = selected;
            }
            // The footer names what A does to the selected row.
            string change = selected >= 0 && rows[selected].Row is ToggleRow toggle ? toggle.Get() ? "Off" : "On" : null;
            if (change != live.ChangeText)
            {
                live.Change.Root.SetActive(change != null);
                if (change != null)
                    ShowPadKey(live.Change, PadButtons.A, "A", change);
                live.ChangeText = change;
            }
        }

        // A glyph stands in for a binding that is one button with a known glyph; any other binding is spelled out.
        private static void ShowPadKey(PadEntry entry, PadButtons button, string binding, string label)
        {
            var glyph = Assets.Glyph(button);
            entry.Key.SetActive(glyph);
            if (glyph)
            {
                entry.Glyph.sprite = glyph;
                entry.Glyph.enabled = true;
            }
            entry.Label.text = glyph ? label : $"{binding}  {label}";
        }

        private static PadButtons GlyphButton(PadBinding pad) => pad.Axis switch
        {
            PadAxis.LT => PadButtons.LT,
            PadAxis.RT => PadButtons.RT,
            PadAxis.None when System.Numerics.BitOperations.IsPow2((uint)pad.Chord) => pad.Chord,
            _ => PadButtons.None,
        };

        private static float MoveTowards(float current, float target, float step)
            => current < target ? Math.Min(current + step, target) : Math.Max(current - step, target);

        private static float Ease(float t) => t * t * (3f - 2f * t);

        // Unity's own Lerp and struct constructors are IL2CPP calls; these are plain arithmetic.
        private static float Lerp(float a, float b, float t) => a + (b - a) * t;

        private static Vector2 Lerp(Vector2 a, Vector2 b, float t) => new() { x = Lerp(a.x, b.x, t), y = Lerp(a.y, b.y, t) };

        private static Vector3 Lerp(Vector3 a, Vector3 b, float t)
            => new() { x = Lerp(a.x, b.x, t), y = Lerp(a.y, b.y, t), z = Lerp(a.z, b.z, t) };

        private static Color Lerp(Color a, Color b, float t)
            => new() { r = Lerp(a.r, b.r, t), g = Lerp(a.g, b.g, t), b = Lerp(a.b, b.b, t), a = Lerp(a.a, b.a, t) };

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

        // The recorded rows the settings are built from. The slider rows sit in a container of their own in the game, so the
        // slider row takes the toggle row's width and anchors. The game's Animators fade a selected row's highlight and arrow
        // in; CameraTools switches them on, with the highlight at the recorded selected row's size and a faint strength.
        private sealed record RowTemplates(Node Section, Node Space, Node ToggleOff, Node ToggleOn, Node Slider)
        {
            private const string Highlight = "Content/ImgHighlight";

            public static RowTemplates Of(Node rows)
            {
                var selected = rows.Find("SetUp_02/" + Highlight).Box;
                var toggle = Row(rows.Find("SetUp_05"), selected);
                var slider = Row(rows.Find("SetUp_0304/SetUp_03"), selected);
                var box = slider.Box with
                {
                    AnchorMin = toggle.Box.AnchorMin,
                    AnchorMax = toggle.Box.AnchorMax,
                    Pivot = toggle.Box.Pivot,
                    SizeDelta = new Vector2 { x = toggle.Box.SizeDelta.x, y = slider.Box.SizeDelta.y },
                };
                return new RowTemplates(rows.Find("Text"), rows.Find("Space"), toggle,
                    Row(rows.Find("SetUp_0304/SetUp_08"), selected), slider with { Box = box });
            }

            private static Node Row(Node row, Box selected) => (row with { Active = true })
                .With(Highlight, highlight => highlight with
                {
                    Box = highlight.Box with { Scale = selected.Scale },
                    Image = highlight.Image with { Color = highlight.Image.Color with { a = 0.15f } },
                })
                .With("Content/Selected_Arrow/ClickTips", arrow => arrow with { GroupAlpha = 1f });
        }

        private sealed record TabLook(Color Bg, Vector3 BgScale, Color Icon, Vector3 IconScale, Color Line, Vector2 LineSize)
        {
            public static TabLook Of(Node button)
            {
                Node bg = button.Find("ImgBg"), icon = button.Find("Icon"), line = button.Find("ImgLine");
                return new TabLook(bg.Image.Color, bg.Box.Scale, icon.Image.Color, icon.Box.Scale, line.Image.Color, line.Box.SizeDelta);
            }
        }

        private sealed record SwitchLook(Box Knob, Color Fill, Color Light, Vector3 IconOn, Vector3 IconOff)
        {
            public static SwitchLook Of(Node content) => new(content.Find("Image").Box, content.Find("ImgColor").Image.Color,
                content.Find("Image/ImgLight").Image.Color, content.Find("Image/IconOn").Box.Scale, content.Find("Image/IconOff").Box.Scale);
        }

        // A pad hint: a glyph in Content/Key and a label.
        private sealed record PadEntry(GameObject Root, GameObject Key, Image Glyph, Text Label)
        {
            public static PadEntry Of(GameObject root)
            {
                var content = Child(root.transform, "Content");
                return new PadEntry(root, Child(content, "Key").gameObject, Get<Image>(content, "Key/ImgKey"), Get<Text>(content, "TextLabel"));
            }
        }

        // A keyboard hint: a keycap with the key's name and a label.
        private sealed record KeyEntry(GameObject Root, Text Key, Text Label)
        {
            public static KeyEntry Of(GameObject root)
            {
                var slot = Child(root.transform, "Content/SlotKey/Content");
                ShowOnly(slot, "Key_PC");
                ShowOnly(Child(slot, "Key_PC"), "Text");
                return new KeyEntry(root, Get<Text>(slot, "Key_PC/Text"), Get<Text>(root.transform, "Content/TextLabel"));
            }
        }

        private sealed record Legend(Hint Hint, PadEntry Pad, KeyEntry Keyboard);

        private sealed class RowView
        {
            public Row Row;
            public GameObject Root;
            public RectTransform Rect;
            public GameObject Highlight;
            public GameObject Arrow;
            public Text Label;
            public RectTransform Knob;
            public Image Fill;
            public Image Light;
            public Transform IconOn;
            public Transform IconOff;
            public float Shown;
            public RectTransform Track;
            public RectTransform HandleArea;
            public RectTransform BarFill;
            public RectTransform Handle;
            public float Value;
            public string Text;
        }

        private sealed class TabView
        {
            public RectTransform Button;
            public Image Bg;
            public Image Icon;
            public Image Line;
            public readonly List<GameObject> Objects = new();
            public readonly List<RowView> Rows = new();
            public float Selected;
            // Follows Selected, from 0 for unselected to 1 for selected.
            public float Shown;
        }

        private sealed class Live
        {
            public GameObject Root;
            public RectTransform Pages;
            public CanvasScaler Scaler;
            public GameObject[] Hud;
            public GameObject[] Panel;
            public TabView[] Tabs;
            public RectTransform RowsViewport;
            public TabLook SelectedTab;
            public TabLook OtherTab;
            public SwitchLook SwitchOff;
            public SwitchLook SwitchOn;
            public GameObject TabKeys;
            public Transform TabKeyLeft;
            public Transform TabKeyRight;
            public GameObject Back;
            public RectTransform BackButton;
            public GameObject Footer;
            public PadEntry Change;
            public PadEntry Return;
            public GameObject Toast;
            public Text ToastLabel;
            public RectTransform ZoomHandle;
            public Box ZoomHandleBox;
            public Legend[] Legends;
            public View? Shown;
            public (InputDevice, int)? ShownLayout;
            public float FovShown = float.NaN;
            public int ShownSelected = -1;
            public string ChangeText = "";
            public string ToastText;
            public bool ToastShown;
            public int RenderedTab = -1;
            public float NextFollow;
            public string Followed;
        }
    }
}
