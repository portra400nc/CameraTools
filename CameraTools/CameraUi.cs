using Il2CppInterop.Runtime;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.UI;
using static CameraTools.UiTemplates;
using Object = UnityEngine.Object;

namespace CameraTools
{
    // CameraTools' UI is a copy of Genshin's photo mode page under /Canvas, so hiding /UICamera hides it with the HUD.
    // Genshin cannot run custom MonoBehaviours, so nothing listens to the widgets: every frame each widget either reports
    // a click (its value moved away from what CameraTools last wrote) or takes the setting's current value.
    internal static class CameraUi
    {
        private const string LoadHint = "Open photo mode once to load the CameraTools UI";
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

        private static readonly Repeater up = new(), down = new(), left = new(), right = new();
        private static Live live;
        private static GameObject failedCanvas;
        private static int builds;
        private static bool panelOpen;
        private static int tab;
        private static int selected;
        private static string toast;
        private static float toastUntil;
        private static bool pollSelection = true;
        private static IntPtr lastPicked;
        private static bool clickSeen;

        public static View View => !CameraTools.freecamActive || CameraTools.uiHidden ? View.Hidden : panelOpen ? View.Panel : View.Hud;

        // Drawn with IMGUI until the page has been copied.
        public static string FallbackToast => live == null && Time.unscaledTime < toastUntil ? toast : null;

        public static void Toast(string message)
        {
            toast = message;
            toastUntil = Time.unscaledTime + 2f;
        }

        public static void FreecamChanged(bool active)
        {
            panelOpen = false;
            if (active && !Page)
                Toast(LoadHint);
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
                        Toast(Page ? "The CameraTools UI failed to load; see Latest.log" : LoadHint);
                    return;
                }
                step = "input";
                HandleInput();
                step = "render";
                Render();
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"UI: {step} failed; CameraTools drops its UI until the next scene. {e}");
                if (live != null)
                {
                    failedCanvas = live.Canvas;
                    if (live.Root)
                        Object.Destroy(live.Root);
                }
                live = null;
                ClosePanel();
            }
        }

        // Scene changes destroy /Canvas and the live root with it. A canvas the build failed on is not tried again.
        private static bool Ensure()
        {
            if (live != null && live.Root)
                return true;
            live = null;
            ClosePanel();
            if (!Page)
                return false;
            var canvas = GameObject.Find("/Canvas");
            if (!canvas || failedCanvas && failedCanvas.Pointer == canvas.Pointer)
                return false;
            failedCanvas = canvas;
            live = Build(canvas);
            failedCanvas = null;
            builds++;
            CameraTools.LogOnce(builds == 1 ? "UI: built the CameraTools UI under /Canvas."
                : $"UI: rebuilt the CameraTools UI under a new /Canvas (build {builds}).");
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
                ShowOnly(com, "Reminder_1", "GrpMain", "GrpLeft", "GrpTab", "GrpActionTop_PS4", "GrpAction_PS4");
                ShowOnly(Child(com, "GrpMain"), "Zoom_Slider");
                ShowOnly(Child(com, "GrpLeft"), "GrpSetUp");
                var setUp = Child(com, "GrpLeft/GrpSetUp");
                ShowOnly(setUp, "GrpBg", "Content", "GrpAction_PS4");
                ShowOnly(Child(setUp, "GrpAction_PS4"), "BtnChange_PS4", "BtnReturn_PS4");
                // These hints may be slots CameraTools filled, which hold the prefab's placeholder glyph and label.
                SetGlyph(Get<Image>(setUp, "GrpAction_PS4/BtnChange_PS4/Content/Key/ImgKey"), PadButtons.A);
                SetGlyph(Get<Image>(setUp, "GrpAction_PS4/BtnReturn_PS4/Content/Key/ImgKey"), PadButtons.B);
                Get<Text>(setUp, "GrpAction_PS4/BtnReturn_PS4/Content/TextLabel").text = "Return";
                ShowOnly(Child(com, "GrpTab"), "Tab");
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

                var built = new Live
                {
                    Root = root,
                    Canvas = canvas,
                    Hud = new[] { Child(com, "GrpMain").gameObject, Child(com, "GrpActionTop_PS4").gameObject, Child(com, "GrpAction_PS4").gameObject },
                    Panel = new[] { Child(com, "GrpLeft").gameObject, Child(com, "GrpTab").gameObject },
                    Rows = Child(setUp, "Content/ScrollView/Content"),
                    TabButtons = tabs.Select(button => button.gameObject.Pointer).ToArray(),
                    TabLines = tabs.Select(button => Child(button, "ImgLine").gameObject).ToArray(),
                    TabIcons = tabs.Select(button => Get<Image>(button, "Icon")).ToArray(),
                    ChangeHint = Child(setUp, "GrpAction_PS4/BtnChange_PS4").gameObject,
                    ChangeLabel = Get<Text>(setUp, "GrpAction_PS4/BtnChange_PS4/Content/TextLabel"),
                    Toast = toastPanel,
                    ToastLabel = Get<Text>(toastPanel.transform, "Desc/Text"),
                    FovBar = fovBar,
                    Bottom = Legend(Child(com, "GrpAction_PS4"), "BtnHideUI_PS4", UiModel.BottomHints),
                    Top = Legend(Child(com, "GrpActionTop_PS4"), "BtnCameraPush_PS4", UiModel.TopHints),
                };
                ShowTab(built, Math.Min(tab, UiModel.Tabs.Length - 1));
                root.SetActive(true);
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

        private static void SetGlyph(Image image, PadButtons button)
        {
            if (Glyphs.TryGetValue(button, out var sprite))
                image.sprite = sprite;
        }

        private static void ShowOnly(Transform parent, params string[] names)
        {
            for (int index = 0; index < parent.childCount; index++)
            {
                var child = parent.GetChild(index).gameObject;
                child.SetActive(names.Contains(child.name));
            }
        }

        // One copy of the page's own legend entry per hint; the page's entries stay hidden.
        private static LegendEntry[] Legend(Transform group, string template, Hint[] hints)
        {
            var source = Child(group, template).gameObject;
            ShowOnly(group);
            return hints.Select(hint =>
            {
                var entry = Clone(source, group);
                entry.SetActive(true);
                var content = Child(entry.transform, "Content");
                return new LegendEntry(hint, entry, Child(content, "Key").gameObject, Get<Image>(content, "Key/ImgKey"),
                    Get<Text>(content, "TextLabel"));
            }).ToArray();
        }

        private static void ShowTab(Live target, int index)
        {
            tab = index;
            var rows = target.Rows;
            for (int child = rows.childCount - 1; child >= 0; child--)
                Object.DestroyImmediate(rows.GetChild(child).gameObject);
            target.RowViews.Clear();
            foreach (var row in UiModel.Tabs[index].Rows)
            {
                if (row is Section && target.RowViews.Count > 0)
                    Clone(UiTemplates.Space, rows).SetActive(true);
                target.RowViews.Add(Bind(row, rows));
            }
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

        private static RowView Bind(Row row, Transform rows)
        {
            var template = row switch
            {
                Section => UiTemplates.Section,
                ToggleRow => UiTemplates.ToggleRow,
                SliderRow => UiTemplates.SliderRow,
                _ => throw new InvalidOperationException($"No template for {row.GetType().Name}."),
            };
            var root = Clone(template, rows);
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
                Highlight = Child(transform, "Content/ImgHighlight").gameObject,
                // Selected_Arrow is an empty container; the page switches the arrow images in ClickTips on and off.
                Arrow = Child(transform, "Content/Selected_Arrow/ClickTips").gameObject,
                Label = Get<Text>(transform, "Content/Text"),
            };
            if (row is ToggleRow toggle)
            {
                view.Toggle = Get<Toggle>(transform, "Content/GrpToggle/Btn_Toggle/Content");
                view.Switch = Get<Animator>(transform, "Content/GrpToggle/Btn_Toggle/Content");
                view.WrittenOn = toggle.Get();
                view.Toggle.isOn = view.WrittenOn;
                view.Label.text = row.Label;
            }
            else if (row is SliderRow slider)
            {
                view.Slider = Get<Slider>(transform, "Content/Slider_W/Content");
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
            if (!panelOpen)
                return;
            PollSelection();
            if (padOwned)
                Navigate();
        }

        private static void SetPanel(bool open)
        {
            panelOpen = open;
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
                    slider.Set(Math.Clamp(slider.Get() + (stepRight ? slider.Step : -slider.Step), slider.Min, slider.Max));
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

        // Clicking a row or tab selects it in the game's EventSystem. The selection is cleared again so the game's own
        // pad navigation never moves CameraTools' widgets.
        private static void PollSelection()
        {
            if (!pollSelection)
                return;
            try
            {
                var events = EventSystem.current;
                var picked = events ? events.currentSelectedGameObject : null;
                if (!picked || picked.Pointer == lastPicked)
                    return;
                lastPicked = picked.Pointer;
                var stop = live.Root.transform;
                for (var node = picked.transform; node && node.Pointer != stop.Pointer; node = node.parent)
                {
                    IntPtr pointer = node.gameObject.Pointer;
                    int tabIndex = Array.IndexOf(live.TabButtons, pointer);
                    int rowIndex = live.RowViews.FindIndex(view => view.Row is not Section && view.Root.Pointer == pointer);
                    if (tabIndex < 0 && rowIndex < 0)
                        continue;
                    CameraTools.LogOnce("UI: the EventSystem selected a CameraTools row or tab; clicks select them.");
                    if (tabIndex >= 0 && tabIndex != tab)
                        ShowTab(live, tabIndex);
                    else if (rowIndex >= 0)
                        selected = rowIndex;
                    events.SetSelectedGameObject(null);
                    lastPicked = IntPtr.Zero;
                    return;
                }
            }
            catch (Exception e)
            {
                pollSelection = false;
                CameraTools.LogOnce($"UI: reading the EventSystem selection failed; tabs switch with LB and RB only. {e}");
            }
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
            if (view == View.Hud)
            {
                if (live.LegendOwner != Controls.Owner)
                {
                    live.LegendOwner = Controls.Owner;
                    foreach (var entry in live.Bottom.Concat(live.Top))
                        RenderHint(entry);
                }
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

        private static void Sync(RowView view)
        {
            switch (view.Row)
            {
                case ToggleRow toggle:
                {
                    bool shown = view.Toggle.isOn;
                    if (shown != view.WrittenOn)
                    {
                        ClickSeen();
                        toggle.Set(shown);
                    }
                    bool wanted = toggle.Get();
                    if (shown != wanted)
                        view.Toggle.isOn = wanted;
                    view.WrittenOn = wanted;
                    break;
                }
                case SliderRow slider:
                {
                    float shown = view.Slider.value;
                    bool clicked = shown != view.Written;
                    if (clicked)
                    {
                        ClickSeen();
                        slider.Set(shown);
                    }
                    float wanted = slider.Get();
                    if (clicked || wanted != view.Value)
                        WriteSlider(view, slider, wanted);
                    break;
                }
            }
        }

        // Slider.value clamps to the slider's range, which is the row's, so the clamped value is what it reads back. Hotkeys
        // can push game speed past the row's range; the label shows the real value.
        private static void WriteSlider(RowView view, SliderRow slider, float value)
        {
            view.Value = value;
            view.Written = Math.Clamp(value, slider.Min, slider.Max);
            view.Slider.value = view.Written;
            string text = $"{slider.Label}  {value.ToString(slider.Format)}";
            if (text != view.Text)
            {
                view.Label.text = text;
                view.Text = text;
            }
        }

        private static void ClickSeen()
        {
            if (clickSeen)
                return;
            clickSeen = true;
            CameraTools.LogOnce("UI: a click or drag changed a CameraTools widget; mouse input reaches the copied page.");
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
            string change = selected >= 0 && live.RowViews[selected].Row is ToggleRow ? live.RowViews[selected].WrittenOn ? "Off" : "On" : null;
            if (change != live.ChangeText)
            {
                live.ChangeHint.SetActive(change != null);
                if (change != null)
                    live.ChangeLabel.text = change;
                live.ChangeText = change;
            }
        }

        // A glyph stands in for a binding that is one button the page has a glyph for; any other binding is spelled out.
        private static void RenderHint(LegendEntry entry)
        {
            var (key, pad) = Controls.Binding(entry.Hint.Action);
            bool padOwned = Controls.Owner == PadOwner.CameraTools;
            Sprite glyph = padOwned && Glyphs.TryGetValue(GlyphButton(pad), out var sprite) ? sprite : null;
            string binding = padOwned ? pad.ToString() : key == KeyCode.None ? "" : key.ToString();
            entry.Root.SetActive(glyph || binding.Length > 0);
            entry.Key.SetActive(glyph);
            if (glyph)
                entry.Glyph.sprite = glyph;
            entry.Label.text = glyph ? entry.Hint.Label : $"{binding}  {entry.Hint.Label}";
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

        private sealed record LegendEntry(Hint Hint, GameObject Root, GameObject Key, Image Glyph, Text Label);

        private sealed class RowView
        {
            public Row Row;
            public GameObject Root;
            public GameObject Highlight;
            public GameObject Arrow;
            public Text Label;
            public Toggle Toggle;
            public Animator Switch;
            public Slider Slider;
            public bool WrittenOn;
            public float Value;
            public float Written;
            public string Text;
        }

        private sealed class Live
        {
            public GameObject Root;
            public GameObject Canvas;
            public GameObject[] Hud;
            public GameObject[] Panel;
            public Transform Rows;
            public readonly List<RowView> RowViews = new();
            public IntPtr[] TabButtons;
            public GameObject[] TabLines;
            public Image[] TabIcons;
            public GameObject ChangeHint;
            public Text ChangeLabel;
            public GameObject Toast;
            public Text ToastLabel;
            public Slider FovBar;
            public LegendEntry[] Bottom;
            public LegendEntry[] Top;
            public View? Shown;
            public PadOwner? LegendOwner;
            public float FovShown = float.NaN;
            public int ShownSelected = -1;
            public string ChangeText = "";
            public string ToastText;
            public bool ToastShown;
        }
    }
}
