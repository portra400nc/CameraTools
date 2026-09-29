using Il2CppInterop.Runtime;
using UnityEngine;
using UnityEngine.UI;
using static CameraTools.UiTemplates;
using Object = UnityEngine.Object;

namespace CameraTools
{
    // CameraTools' UI is a copy of Genshin's photo mode page under /Canvas, so hiding /UICamera hides it with the HUD.
    // Genshin cannot run custom MonoBehaviours, so nothing listens to the widgets: CameraTools disables them, reads the
    // mouse and the pad itself, and writes each setting's current value to its widget every frame.
    internal static class CameraUi
    {
        private const string LoadHint = "Open photo mode once to load the CameraTools UI";
        private const string ReloadHint = "The CameraTools UI failed; open photo mode to reload it";
        private const float RepeatDelay = 0.4f;
        private const float RepeatInterval = 0.08f;

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
        private static bool panelOpen;
        private static int tab;
        private static int selected;
        private static string toast;
        private static float toastUntil;
        private static Vector2 lastMouse;
        private static RowView dragging;

        public static View View => !CameraTools.freecamActive || CameraTools.uiHidden ? View.Hidden : panelOpen ? View.Panel : View.Hud;

        // Drawn with IMGUI until the page has been copied.
        public static string FallbackToast => live == null && Time.unscaledTime < toastUntil ? toast : null;

        public static void Toast(string message)
        {
            toast = message;
            toastUntil = Time.unscaledTime + 2f;
        }

        private static string MissingHint => UiTemplates.AwaitingPhotoMode ? ReloadHint : LoadHint;

        public static void FreecamChanged(bool active)
        {
            panelOpen = false;
            if (active && !Page)
                Toast(MissingHint);
        }

        public static void Update()
        {
            UiTemplates.Update();
            string step = "build";
            try
            {
                if (!Ensure())
                {
                    if (CameraTools.freecamActive && Controls.Pressed(CamAction.ToggleGUI))
                        Toast(MissingHint);
                    return;
                }
                step = "input";
                HandleInput();
                step = "render";
                Render();
            }
            catch (Exception e)
            {
                failures++;
                CameraTools.LogOnce($"UI: {step} failed (failure {failures}, {UiTemplates.Health()}); CameraTools reloads its UI "
                    + $"the next time photo mode opens. {e}");
                if (live != null && live.Root)
                    Object.Destroy(live.Root);
                live = null;
                ClosePanel();
                UiTemplates.Discard();
                Toast(ReloadHint);
            }
        }

        // Scene changes destroy /Canvas and the live root with it.
        private static bool Ensure()
        {
            if (live != null && live.Root)
                return true;
            live = null;
            ClosePanel();
            if (!Page)
                return false;
            // Builds 69 and 70 failed rebuilding mid-teleport, before the level's HUD page existed.
            var canvas = GameObject.Find("/Canvas");
            if (!canvas || !canvas.transform.Find("Pages/InLevelMainPage"))
                return false;
            live = Build(canvas);
            builds++;
            CameraTools.LogOnce($"UI: built the CameraTools UI under /Canvas (build {builds}).");
            return true;
        }

        private static Live Build(GameObject canvas)
        {
            var root = Clone(Page, canvas.transform);
            try
            {
                root.name = "CameraTools";
                root.transform.SetAsLastSibling();
                var page = root.transform;
                ShowOnly(page, "GrpCom");
                var com = Child(page, "GrpCom");
                ShowOnly(com, "Reminder_1", "GrpMain", "GrpLeft", "GrpTab", "GrpActionTop_PS4", "GrpAction_PS4", "GrpAction_PC");
                ShowOnly(Child(com, "GrpMain"), "Zoom_Slider");
                ShowOnly(Child(com, "GrpLeft"), "GrpSetUp");
                var setUp = Child(com, "GrpLeft/GrpSetUp");
                ShowOnly(setUp, "GrpBg", "Content", "GrpAction_PS4");
                ShowOnly(Child(setUp, "GrpAction_PS4"), "BtnChange_PS4", "BtnReturn_PS4");
                ShowOnly(Child(com, "GrpTab"), "Tab", "BtnBack");
                var tabKeys = Child(com, "GrpTab/Tab/Tab_Key");
                ShowOnly(Child(com, "Reminder_1"), "ShowPanel");
                var toastPanel = Child(com, "Reminder_1/ShowPanel").gameObject;
                toastPanel.SetActive(false);

                var tabRow = Child(com, "GrpTab/Tab/Viewport/Tab");
                if (UiModel.Tabs.Length > TabButtons.Length)
                    throw new InvalidOperationException($"The page has {TabButtons.Length} tab buttons for {UiModel.Tabs.Length} tabs.");
                ShowOnly(tabRow, TabButtons.Take(UiModel.Tabs.Length).ToArray());
                var tabs = UiModel.Tabs.Select((_, index) => Child(tabRow, TabButtons[index] + "/Content")).ToArray();

                var fovBar = Get<Slider>(com, "GrpMain/Zoom_Slider");
                fovBar.interactable = false;

                var padBottom = Child(com, "GrpAction_PS4");
                var padTop = Child(com, "GrpActionTop_PS4");
                var keyBottom = Child(com, "GrpAction_PC");
                var padBottomTemplate = Child(padBottom, "BtnHideUI_PS4").gameObject;
                var padTopTemplate = Child(padTop, "BtnCameraPush_PS4").gameObject;
                var keyTemplate = Child(keyBottom, "BtnHideUI_PC").gameObject;
                foreach (var group in new[] { padBottom, padTop, keyBottom })
                    ShowOnly(group);

                // The live copy carries its own row templates. They move to a hidden shelf, because the rows are rebuilt from
                // them on every tab switch; copying them from the kept page failed in builds 66 and 67 (see UiTemplates.Watch).
                var rows = Child(setUp, "Content/ScrollView/Content");
                var shelf = new GameObject("RowTemplates");
                shelf.transform.SetParent(root.transform, false);
                shelf.SetActive(false);
                var templates = new[] { "Text", "Space", "SetUp_05", "SetUp_0304/SetUp_03" }.Select(path => Child(rows, path)).ToArray();
                foreach (var template in templates)
                    template.SetParent(shelf.transform, false);

                var built = new Live
                {
                    Root = root,
                    SectionTemplate = templates[0].gameObject,
                    SpaceTemplate = templates[1].gameObject,
                    ToggleTemplate = templates[2].gameObject,
                    SliderTemplate = templates[3].gameObject,
                    RenderCanvas = canvas.GetComponent<Canvas>(),
                    Hud = new[] { Child(com, "GrpMain").gameObject, padTop.gameObject, padBottom.gameObject, keyBottom.gameObject },
                    Panel = new[] { Child(com, "GrpLeft").gameObject, Child(com, "GrpTab").gameObject },
                    Rows = rows,
                    RowsViewport = RectOf(Child(setUp, "Content/ScrollView")),
                    TabButtons = tabs.Select(RectOf).ToArray(),
                    TabLines = tabs.Select(button => Child(button, "ImgLine").gameObject).ToArray(),
                    TabIcons = tabs.Select(button => Get<Image>(button, "Icon")).ToArray(),
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
                    FovBar = fovBar,
                    Legends = Legends(UiModel.BottomHints, padBottomTemplate, padBottom, keyTemplate, keyBottom)
                        .Concat(Legends(UiModel.TopHints, padTopTemplate, padTop, keyTemplate, padTop)).ToArray(),
                };
                root.SetActive(true);
                ShowTab(built, Math.Min(tab, UiModel.Tabs.Length - 1));
                SetWidgets(root, false);
                // A Slider finds its handle and fill in OnEnable, so a row copied from a disabled template never draws its value.
                SetWidgets(shelf, true);
                return built;
            }
            catch
            {
                Object.Destroy(root);
                throw;
            }
        }

        private static void SetAlpha(Graphic graphic, float alpha)
        {
            if (!graphic)
                return;
            var color = graphic.color;
            color.a = alpha;
            graphic.color = color;
        }

        private static RectTransform RectOf(Transform transform) => transform.TryCast<RectTransform>();

        private static void ShowOnly(Transform parent, params string[] names)
        {
            for (int index = 0; index < parent.childCount; index++)
            {
                var child = parent.GetChild(index).gameObject;
                child.SetActive(names.Contains(child.name));
            }
        }

        // The game's EventSystem skips disabled widgets, so only CameraTools changes them, and their graphics still take
        // raycasts, so the game still sees the pointer over its UI. A disabled Toggle or Slider still shows the values
        // CameraTools writes. A Slider finds its fill and handle in OnEnable, so a widget is disabled once it has been active.
        private static void SetWidgets(GameObject root, bool enabled)
        {
            foreach (var widget in root.GetComponentsInChildren(Il2CppType.Of<Selectable>(), true))
                widget.TryCast<Behaviour>().enabled = enabled;
        }

        // Each hint gets a copy of the page's pad legend entry and one of its keyboard entry; the page's entries stay hidden.
        private static IEnumerable<Legend> Legends(Hint[] hints, GameObject pad, Transform padGroup, GameObject keyboard, Transform keyGroup)
            => hints.Select(hint =>
            {
                var legend = new Legend(hint, PadEntry.Of(Clone(pad, padGroup)), KeyEntry.Of(Clone(keyboard, keyGroup)));
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
            var glyph = Glyphs.Get(button);
            Child(content, "Key_Group").gameObject.SetActive(glyph);
            Child(content, "Key_PC").gameObject.SetActive(!glyph);
            if (glyph)
                Get<Image>(content, "Key_Group/Icon1").sprite = glyph;
            else
                Get<Text>(content, "Key_PC/Text").text = button.ToString();
        }

        // Rows are built while the panel shows, so their widgets have been active before they are disabled.
        private static void ShowTab(Live target, int index)
        {
            tab = index;
            dragging = null;
            var rows = target.Rows;
            for (int child = rows.childCount - 1; child >= 0; child--)
                Object.DestroyImmediate(rows.GetChild(child).gameObject);
            target.RowViews.Clear();
            foreach (var row in UiModel.Tabs[index].Rows)
            {
                if (row is Section && target.RowViews.Count > 0)
                    Clone(target.SpaceTemplate, rows).SetActive(true);
                target.RowViews.Add(Bind(target, row));
            }
            SetWidgets(rows.gameObject, false);
            // The tab's Animator, which is disabled, dims the other tabs and fades the underline in.
            for (int other = 0; other < target.TabLines.Length; other++)
            {
                target.TabLines[other].SetActive(other == index);
                SetAlpha(target.TabLines[other].GetComponent<Image>(), 1f);
                SetAlpha(target.TabIcons[other], other == index ? 1f : 0.4f);
            }
            selected = target.RowViews.FindIndex(view => view.Row is not Section);
            target.ShownSelected = -1;
        }

        private static RowView Bind(Live target, Row row)
        {
            var template = row switch
            {
                Section => target.SectionTemplate,
                ToggleRow => target.ToggleTemplate,
                SliderRow => target.SliderTemplate,
                _ => throw new InvalidOperationException($"No template for {row.GetType().Name}."),
            };
            var root = Clone(template, target.Rows);
            root.SetActive(true);
            var transform = root.transform;
            if (row is Section)
            {
                var header = root.GetComponent(Il2CppType.Of<Text>()).TryCast<Text>();
                header.text = row.Label;
                return new RowView { Row = row, Root = root };
            }

            var view = new RowView
            {
                Row = row,
                Root = root,
                Rect = RectOf(transform),
                Highlight = Child(transform, "Content/ImgHighlight").gameObject,
                // Selected_Arrow is an empty container; the page switches the arrow images in ClickTips on and off.
                Arrow = Child(transform, "Content/Selected_Arrow/ClickTips").gameObject,
                Label = Get<Text>(transform, "Content/Text"),
            };
            if (row is ToggleRow toggle)
            {
                view.Toggle = Get<Toggle>(transform, "Content/GrpToggle/Btn_Toggle/Content");
                view.Switch = Get<Animator>(transform, "Content/GrpToggle/Btn_Toggle/Content");
                view.Toggle.isOn = toggle.Get();
                view.Label.text = row.Label;
            }
            else if (row is SliderRow slider)
            {
                view.Slider = Get<Slider>(transform, "Content/Slider_W/Content");
                view.SliderRect = RectOf(view.Slider.transform);
                view.Slider.wholeNumbers = false;
                view.Slider.minValue = slider.Min;
                view.Slider.maxValue = slider.Max;
                WriteSlider(view, slider, slider.Get());
            }
            view.Highlight.SetActive(false);
            view.Arrow.SetActive(false);
            return view;
        }

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
            switch (live.RowViews[selected].Row)
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
            for (int index = selected + direction; index >= 0 && index < live.RowViews.Count; index += direction)
                if (live.RowViews[index].Row is not Section)
                    return index;
            return selected;
        }

        private static void Step(SliderRow slider, int direction)
            => slider.Set(Math.Clamp(slider.Get() + direction * slider.Step, slider.Min, slider.Max));

        // Genshin's EventSystem reaches the copy in some runs and not in others, so CameraTools hit-tests the mouse itself.
        private static void HandleMouse()
        {
            var canvas = live.RenderCanvas;
            var camera = canvas.renderMode == RenderMode.ScreenSpaceOverlay ? null : canvas.worldCamera;
            CameraTools.LogOnce($"UI: /Canvas renders in {canvas.renderMode} mode with {(camera ? $"camera {camera.name}" : "no camera")}.");
            var mouse = Input.mousePosition;
            var point = new Vector2(mouse.x, mouse.y);
            bool moved = point.x != lastMouse.x || point.y != lastMouse.y;
            lastMouse = point;

            int row = RowAt(point, camera);
            if (moved && row >= 0 && dragging == null)
                selected = row;
            float wheel = Input.mouseScrollDelta.y;
            if (wheel != 0f && row >= 0 && live.RowViews[row].Row is SliderRow wheeled)
                Step(wheeled, wheel > 0f ? 1 : -1);
            if (Input.GetMouseButtonDown(0))
                Click(point, camera, row);
            if (dragging == null)
                return;
            if (Input.GetMouseButton(0))
                Drag(dragging, point, camera);
            else
                dragging = null;
        }

        private static bool Contains(RectTransform rect, Vector2 point, Camera camera)
            => RectTransformUtility.RectangleContainsScreenPoint(rect, point, camera);

        // Rows scrolled out of the list's viewport do not count.
        private static int RowAt(Vector2 point, Camera camera)
            => Contains(live.RowsViewport, point, camera)
                ? live.RowViews.FindIndex(view => view.Row is not Section && Contains(view.Rect, point, camera))
                : -1;

        private static void Click(Vector2 point, Camera camera, int row)
        {
            int tabIndex = Array.FindIndex(live.TabButtons, button => Contains(button, point, camera));
            bool back = live.Back.activeSelf && Contains(live.BackButton, point, camera);
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
            var view = live.RowViews[row];
            if (view.Row is ToggleRow toggle)
                toggle.Set(!toggle.Get());
            else if (Contains(view.SliderRect, point, camera))
                dragging = view;
        }

        // The interop passes ScreenPointToLocalPointInRectangle's out value by value, so the slider's ends are projected to
        // the screen instead.
        private static void Drag(RowView view, Vector2 point, Camera camera)
        {
            var rect = view.SliderRect;
            var area = rect.rect;
            float left = RectTransformUtility.WorldToScreenPoint(camera, rect.TransformPoint(new Vector3(area.xMin, 0f, 0f))).x;
            float right = RectTransformUtility.WorldToScreenPoint(camera, rect.TransformPoint(new Vector3(area.xMax, 0f, 0f))).x;
            if (right <= left)
                return;
            float t = Math.Clamp((point.x - left) / (right - left), 0f, 1f);
            var slider = (SliderRow)view.Row;
            slider.Set(slider.Min + (slider.Max - slider.Min) * t);
        }

        private static void Render()
        {
            var view = View;
            if (live.Shown != view)
            {
                foreach (var part in live.Hud)
                    part.SetActive(view == View.Hud);
                foreach (var part in live.Panel)
                    part.SetActive(view == View.Panel);
                live.Shown = view;
            }
            var layout = Controls.Layout;
            if (layout == InputDevice.Pad && view != View.Hidden)
                Glyphs.Update();
            if (live.ShownLayout != (layout, Glyphs.Count))
            {
                live.ShownLayout = (layout, Glyphs.Count);
                RenderLayout(layout);
            }
            if (view == View.Hud)
            {
                var fov = CameraTools.settings.Fov;
                if (live.FovShown != fov.Value)
                {
                    // BtnZoomNear sits at the bar's top, so a narrow field of view fills the bar.
                    live.FovBar.normalizedValue = (fov.Max - fov.Value) / (fov.Max - fov.Min);
                    live.FovShown = fov.Value;
                }
            }
            if (view == View.Panel)
            {
                foreach (var row in live.RowViews)
                    Sync(row);
                RenderSelection();
                // Genshin strips Animator.updateMode, so the switches animate on game time and would hold their old pose
                // while the game is slowed or paused. They get the time the game speed took away.
                float lost = Time.unscaledDeltaTime - Time.deltaTime;
                if (lost > 0f)
                    foreach (var row in live.RowViews)
                        if (row.Switch)
                            row.Switch.Update(lost);
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
        private static void Sync(RowView view)
        {
            switch (view.Row)
            {
                case ToggleRow toggle:
                {
                    bool on = toggle.Get();
                    if (view.Toggle.isOn != on)
                        view.Toggle.isOn = on;
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

        // Slider.value clamps to the slider's range, which is the row's. Hotkeys can push game speed past the row's range;
        // the label shows the real value.
        private static void WriteSlider(RowView view, SliderRow slider, float value)
        {
            view.Value = value;
            view.Slider.value = value;
            string text = $"{slider.Label}  {value.ToString(slider.Format)}";
            if (text != view.Text)
            {
                view.Label.text = text;
                view.Text = text;
            }
        }

        private static void RenderSelection()
        {
            if (live.ShownSelected != selected)
            {
                for (int index = 0; index < live.RowViews.Count; index++)
                {
                    var row = live.RowViews[index];
                    if (row.Row is Section)
                        continue;
                    row.Highlight.SetActive(index == selected);
                    row.Arrow.SetActive(index == selected);
                }
                live.ShownSelected = selected;
            }
            // The footer names what A does to the selected row.
            string change = selected >= 0 && live.RowViews[selected].Row is ToggleRow toggle ? toggle.Get() ? "Off" : "On" : null;
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
            var glyph = Glyphs.Get(button);
            entry.Key.SetActive(glyph);
            if (glyph)
                entry.Glyph.sprite = glyph;
            entry.Label.text = glyph ? label : $"{binding}  {label}";
        }

        private static PadButtons GlyphButton(PadBinding pad) => pad.Axis switch
        {
            PadAxis.LT => PadButtons.LT,
            PadAxis.RT => PadButtons.RT,
            PadAxis.None when System.Numerics.BitOperations.IsPow2((uint)pad.Chord) => pad.Chord,
            _ => PadButtons.None,
        };

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
            public Toggle Toggle;
            public Animator Switch;
            public Slider Slider;
            public RectTransform SliderRect;
            public float Value;
            public string Text;
        }

        private sealed class Live
        {
            public GameObject Root;
            public GameObject SectionTemplate;
            public GameObject SpaceTemplate;
            public GameObject ToggleTemplate;
            public GameObject SliderTemplate;
            public Canvas RenderCanvas;
            public GameObject[] Hud;
            public GameObject[] Panel;
            public Transform Rows;
            public RectTransform RowsViewport;
            public readonly List<RowView> RowViews = new();
            public RectTransform[] TabButtons;
            public GameObject[] TabLines;
            public Image[] TabIcons;
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
            public Slider FovBar;
            public Legend[] Legends;
            public View? Shown;
            public (InputDevice, int)? ShownLayout;
            public float FovShown = float.NaN;
            public int ShownSelected = -1;
            public string ChangeText = "";
            public string ToastText;
            public bool ToastShown;
        }
    }
}
