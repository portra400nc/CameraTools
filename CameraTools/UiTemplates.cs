using Il2CppInterop.Runtime;
using MoleMole;
using UnityEngine;
using UnityEngine.Events;
using UnityEngine.UI;
using Object = UnityEngine.Object;

namespace CameraTools
{
    // Genshin's photo mode page exists once the player has opened photo mode in the session. CameraTools copies it once
    // into an inactive holder, where no copied script wakes, strips the game's scripts, and builds its UI from the copy.
    internal static class UiTemplates
    {
        private const string SetUp = "GrpCom/GrpLeft/GrpSetUp";
        private const string Rows = SetUp + "/Content/ScrollView/Content";

        private static float nextLook;

        private static GameObject holder;
        private static int copies;

        public static GameObject Page { get; private set; }
        // Set after the UI failed: the next copy waits until the player opens photo mode again, so a failure is not
        // retried every second and the copy starts from a page the game has just refreshed.
        public static bool AwaitingPhotoMode { get; private set; }
        private static GameObject Section;
        private static GameObject Space;
        private static GameObject ToggleRow;
        private static GameObject SliderRow;
        private static float copiedAt;

        public static void Update()
        {
            if (Time.unscaledTime < nextLook)
                return;
            nextLook = Time.unscaledTime + 1f;
            if (holder != null)
            {
                Watch();
                return;
            }
            var pages = GameObject.Find("/Canvas/Pages");
            var source = pages ? pages.transform.Find("InLevelPhotographContext") : null;
            if (source && (!AwaitingPhotoMode || source.gameObject.activeInHierarchy))
                Capture(source.gameObject);
        }

        // In builds 66 and 67 the UI failed 70 to 90 s after the copy, while instantiating a row template from it. The copy
        // is checked every second, and the first part found destroyed names what the game removes and when.
        private static void Watch()
        {
            var lost = Lost();
            if (lost.Length == 0)
            {
                // A quiet log cannot tell a surviving copy from a short session.
                if (Time.unscaledTime - copiedAt >= 120f)
                    CameraTools.LogOnce($"UI: copy {copies} is still intact 120 s after the copy.");
                return;
            }
            CameraTools.LogOnce($"UI: {string.Join(", ", lost)} of copy {copies} was destroyed "
                + $"{Time.unscaledTime - copiedAt:0} s after the copy; the page is copied again the next time photo mode opens.");
            Discard();
        }

        // For a failure report: whether the kept copy or only the new copy made from it lacks a part.
        public static string Health()
        {
            if (holder == null)
                return "no copy is kept";
            var lost = Lost();
            return lost.Length == 0 ? $"copy {copies} is intact" : $"copy {copies} has lost {string.Join(", ", lost)}";
        }

        private static string[] Lost()
        {
            var parts = new (string Name, bool Alive)[]
            {
                ("the holder", holder), ("the page", Page), ("the section template", Section), ("the space template", Space),
                ("the toggle row template", ToggleRow), ("the slider row template", SliderRow),
                ("the zoom bar's Slider", Has<Slider>(Page, "GrpCom/GrpMain/Zoom_Slider")),
                ("the toggle row's Toggle", Has<Toggle>(ToggleRow, "Content/GrpToggle/Btn_Toggle/Content")),
                ("the slider row's Slider", Has<Slider>(SliderRow, "Content/Slider_W/Content")),
            };
            return parts.Where(part => !part.Alive).Select(part => part.Name).ToArray();
        }

        private static bool Has<T>(GameObject root, string path) where T : Component
        {
            var child = root ? root.transform.Find(path) : null;
            return child && (bool)child.GetComponent(Il2CppType.Of<T>());
        }

        public static void Discard()
        {
            if (holder)
                Object.Destroy(holder);
            holder = null;
            Page = null;
            AwaitingPhotoMode = true;
        }

        private static void Capture(GameObject source)
        {
            holder = new GameObject("CameraTools Templates");
            try
            {
                Object.DontDestroyOnLoad(holder);
                holder.SetActive(false);
                var page = Clone(source, holder.transform);
                page.SetActive(false);
                copies++;
                copiedAt = Time.unscaledTime;
                CameraTools.LogOnce($"UI: copied the photo mode page ({Components<Transform>(page).Length} objects, copy {copies}).");
                FillContainers(page);
                Strip(page);
                var root = page.transform;
                Section = Child(root, Rows + "/Text").gameObject;
                Space = Child(root, Rows + "/Space").gameObject;
                ToggleRow = Child(root, Rows + "/SetUp_05").gameObject;
                SliderRow = Child(root, Rows + "/SetUp_0304/SetUp_03").gameObject;
                FitSliderRow();
                Page = page;
                AwaitingPhotoMode = false;
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"UI: copying the photo mode page failed (copy {copies}): {e}");
                Discard();
            }
        }

        // Only UnityEngine components and two harmless game scripts stay. Persistent calls survive in the copy and can
        // still target the real page, so each one is switched off. Animators could fade the copy mid-animation, except
        // the ones that animate a toggle's switch.
        private static void Strip(GameObject page)
        {
            var found = Named(page);
            var toggleHosts = found.Where(c => c.Name == "MoleMole.MonoToggleAnimator")
                .Select(c => c.Component.gameObject.Pointer).ToHashSet();
            int animations = 0;
            foreach (var (component, name) in found)
            {
                if (name is "UnityEngine.Animator" or "UnityEngine.Animation" && !toggleHosts.Contains(component.gameObject.Pointer))
                {
                    component.TryCast<Behaviour>().enabled = false;
                    animations++;
                }
            }

            var doomed = found.Where(c => !Keeps(c.Name)).ToList();
            var counts = doomed.GroupBy(c => c.Name).ToDictionary(group => group.Key, group => group.Count());
            // A script another script requires cannot go first, so a later pass removes it.
            for (int pass = 0; pass < 3 && doomed.Count > 0; pass++)
            {
                foreach (var (component, _) in doomed)
                    Object.DestroyImmediate(component);
                doomed = Named(page).Where(c => !Keeps(c.Name)).ToList();
            }
            foreach (var group in doomed.GroupBy(c => c.Name))
                counts[group.Key] -= group.Count();
            CameraTools.LogOnce($"UI: stripped {counts.Values.Sum()} game components: "
                + string.Join(", ", counts.OrderByDescending(entry => entry.Value).Select(entry => $"{entry.Key} x{entry.Value}")) + ".");
            if (doomed.Count > 0)
                CameraTools.LogOnce($"UI: {doomed.Count} game components could not be stripped: "
                    + string.Join(", ", doomed.GroupBy(c => c.Name).Select(group => $"{group.Key} x{group.Count()}")) + ".");
            CameraTools.LogOnce($"UI: disabled {animations} animators and animations; kept {toggleHosts.Count} toggle animators.");

            int events = 0, listeners = 0;
            foreach (var (component, name) in Named(page))
            {
                UnityEventBase unityEvent = name switch
                {
                    "UnityEngine.UI.Button" => component.TryCast<Button>().onClick,
                    "UnityEngine.UI.Toggle" => component.TryCast<Toggle>().onValueChanged,
                    "UnityEngine.UI.Slider" => component.TryCast<Slider>().onValueChanged,
                    _ => null,
                };
                if (unityEvent == null)
                    continue;
                events++;
                int count = unityEvent.GetPersistentEventCount();
                for (int index = 0; index < count; index++)
                    unityEvent.SetPersistentListenerState(index, UnityEventCallState.Off);
                listeners += count;
            }
            CameraTools.LogOnce($"UI: switched off {listeners} persistent listeners on {events} Button, Toggle, and Slider events.");

            // The page may have been copied mid-fade.
            var groups = Components<CanvasGroup>(page);
            foreach (var group in groups)
            {
                group.alpha = 1f;
                group.interactable = true;
                group.blocksRaycasts = true;
            }
        }

        // A MonoUIContainer instantiates its templatePrefab as "Content" the first time it shows, so a panel the player
        // never opened is copied with empty slots. Filling them here, before the game's scripts are stripped from what
        // they bring in, makes the copy independent of which panels were opened. A filled template can hold containers.
        private static void FillContainers(GameObject page)
        {
            int filled = 0, added;
            do
            {
                added = 0;
                foreach (var container in Components<MonoUIContainer>(page))
                {
                    var prefab = container.templatePrefab;
                    if (!prefab || container.transform.Find("Content"))
                        continue;
                    Clone(prefab, container.transform).name = "Content";
                    added++;
                }
                filled += added;
            } while (added > 0 && filled < 1000);
            CameraTools.LogOnce($"UI: filled {filled} empty MonoUIContainer slots from their templates.");
        }

        private static bool Keeps(string name)
            => name.StartsWith("UnityEngine") || name is "MoleMole.MihoyoText" or "MoleMole.MonoToggleAnimator";

        // The slider rows sit in a container of their own, so their size is relative to it. The layout group places rows
        // by their size, so the slider row takes the toggle row's width and anchors and keeps its own height.
        private static void FitSliderRow()
        {
            var toggle = ToggleRow.transform.TryCast<RectTransform>();
            var slider = SliderRow.transform.TryCast<RectTransform>();
            float height = slider.rect.height;
            float highlight = Get<Image>(ToggleRow.transform, "Content/ImgHighlight").color.a;
            CameraTools.LogOnce($"UI: row templates: toggle {toggle.sizeDelta.x:0}x{toggle.sizeDelta.y:0}, slider height {height:0}, "
                + $"toggle highlight alpha {highlight:0.00}.");
            // The row's Animator, which is disabled, fades the highlight in on selection. Photo mode's selected row is a
            // faint band behind light text; the full-strength highlight is cream and hides the text.
            foreach (var row in new[] { ToggleRow, SliderRow })
            {
                var image = Get<Image>(row.transform, "Content/ImgHighlight");
                var color = image.color;
                color.a = 0.15f;
                image.color = color;
            }
            slider.anchorMin = toggle.anchorMin;
            slider.anchorMax = toggle.anchorMax;
            slider.pivot = toggle.pivot;
            slider.sizeDelta = new Vector2(toggle.sizeDelta.x, height > 1f ? height : toggle.sizeDelta.y);
        }

        // Instantiate<T> has no native code in Genshin's build; the cast selects the Object overload.
        // Instantiate returns nothing for a destroyed source instead of throwing, so this names the cause.
        public static GameObject Clone(GameObject source, Transform parent)
        {
            var copy = Object.Instantiate((Object)source, parent, false);
            return copy ? copy.TryCast<GameObject>()
                : throw new InvalidOperationException($"Copying {(source ? source.name : "a destroyed object")} under "
                    + $"{(parent ? parent.name : "a destroyed parent")} returned nothing.");
        }

        public static Transform Child(Transform root, string path)
        {
            var child = root.Find(path);
            return child ? child : throw new InvalidOperationException($"{root.name}/{path} is missing.");
        }

        public static T Get<T>(Transform root, string path) where T : Component
        {
            var component = Child(root, path).GetComponent(Il2CppType.Of<T>());
            return component ? component.TryCast<T>() : throw new InvalidOperationException($"{root.name}/{path} has no {typeof(T).Name}.");
        }

        private static T[] Components<T>(GameObject root) where T : Component
            => root.GetComponentsInChildren(Il2CppType.Of<T>(), true).Select(found => found.TryCast<T>()).ToArray();

        // A missing script shows up as null.
        private static List<(Component Component, string Name)> Named(GameObject root)
            => root.GetComponentsInChildren(Il2CppType.Of<Component>(), true)
                .Where(component => component != null)
                .Select(component => (component, TypeName(component)))
                .ToList();

        private static string TypeName(Component component)
        {
            IntPtr klass = IL2CPP.il2cpp_object_get_class(component.Pointer);
            string ns = IL2CPP.il2cpp_class_get_namespace_(klass) ?? "";
            return (ns.Length > 0 ? ns + "." : "") + IL2CPP.il2cpp_class_get_name_(klass);
        }
    }
}
