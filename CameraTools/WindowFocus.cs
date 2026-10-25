using System.Runtime.InteropServices;
using System.Text;
using MelonLoader;
using UnityEngine;
using static CameraTools.CameraTools;

namespace CameraTools
{
    // The game window's focus. On the Deck, after screenshots resized the window past the screen and back and the free
    // camera turned off, the game ignored the controller while gamescope still showed it focused, until a click on the
    // window activated it again: Wine had moved Windows' activation off the game's window. Each change of Unity's focus and
    // of the foreground is logged with what CameraTools was doing, and after its own resize or the free camera turning off,
    // CameraTools activates the game's window again from the game's thread, which Wine lets a process do for its own window.
    internal static class WindowFocus
    {
        private static IntPtr window;
        private static bool searched;
        private static FocusView? seen;
        private static Refocus? refocus;

        // Once a frame, on the game's main thread.
        public static void Update()
        {
            var view = Read();
            if (view != seen)
            {
                Melon<CameraTools>.Logger.Msg($"Focus: Unity is {(view.Focused ? "focused" : "unfocused")} and the foreground is {Describe(GetForegroundWindow())}"
                    + $"{(seen is null ? "" : $", was {(seen.Value.Focused ? "focused" : "unfocused")} with the {seen.Value.Holder}")}; {Doing}.");
                seen = view;
            }
            if (refocus is not { } current)
                return;
            float now = Time.unscaledTime;
            var step = RefocusRules.Step(current, view, now);
            switch (step)
            {
                case RefocusStep.Activate:
                    refocus = RefocusRules.Tried(current, now);
                    Activate(current.Tries + 1);
                    return;
                case RefocusStep.Done:
                    Melon<CameraTools>.Logger.Msg(current.Tries == 0 ? $"Focus: after {current.Reason}, the game kept its focus."
                        : $"Focus: after {current.Reason}, the game has its focus after {current.Tries} activation{(current.Tries == 1 ? "" : "s")}.");
                    break;
                case RefocusStep.Leave:
                    Melon<CameraTools>.Logger.Msg($"Focus: after {current.Reason}, another program holds the foreground, {Describe(GetForegroundWindow())}; "
                        + "CameraTools leaves it there.");
                    break;
                case RefocusStep.GiveUp:
                    Melon<CameraTools>.Logger.Warning($"Focus: after {current.Reason}, {current.Tries} activations in {RefocusRules.Seconds:0} s left Unity "
                        + $"{(view.Focused ? "focused" : "unfocused")} with the foreground {Describe(GetForegroundWindow())}.");
                    break;
                default:
                    return;
            }
            refocus = null;
        }

        // After CameraTools' own change that may have moved the activation off the game's window.
        public static void Repair(string reason)
        {
            refocus = RefocusRules.Start(reason, Time.unscaledTime);
        }

        private static string Doing => $"screenshot {Screenshot.State.GetType().Name}, free camera {(freecamActive ? "on" : "off")}, window {Screen.width}x{Screen.height}";

        private static FocusView Read() => new(Application.isFocused, Holder(GetForegroundWindow()));

        private static FocusHolder Holder(IntPtr foreground)
        {
            if (foreground == IntPtr.Zero)
                return FocusHolder.Nobody;
            if (foreground == Game)
                return FocusHolder.Game;
            GetWindowThreadProcessId(foreground, out uint process);
            return process == (uint)Environment.ProcessId || foreground == GetConsoleWindow() ? FocusHolder.OwnWindow : FocusHolder.OtherApp;
        }

        // The process's Unity window, found once by its class.
        private static IntPtr Game
        {
            get
            {
                if (searched)
                    return window;
                searched = true;
                for (IntPtr each = FindWindowExW(IntPtr.Zero, IntPtr.Zero, "UnityWndClass", null); each != IntPtr.Zero;
                    each = FindWindowExW(IntPtr.Zero, each, "UnityWndClass", null))
                {
                    uint thread = GetWindowThreadProcessId(each, out uint process);
                    if (process != (uint)Environment.ProcessId)
                        continue;
                    window = each;
                    Melon<CameraTools>.Logger.Msg($"Focus: the game's window is {Describe(each)}, on thread {thread}; CameraTools runs on thread {GetCurrentThreadId()}.");
                    break;
                }
                if (window == IntPtr.Zero)
                    Melon<CameraTools>.Logger.Warning("Focus: no UnityWndClass window of this process was found.");
                return window;
            }
        }

        private static void Activate(int attempt)
        {
            var game = Game;
            if (game == IntPtr.Zero)
                return;
            var before = GetForegroundWindow();
            bool top = BringWindowToTop(game);
            bool foreground = SetForegroundWindow(game);
            var active = SetActiveWindow(game);
            var focus = SetFocus(game);
            int error = Marshal.GetLastWin32Error();
            Melon<CameraTools>.Logger.Msg($"Focus: activation {attempt}: the foreground was {Describe(before)}; BringWindowToTop {top}, SetForegroundWindow "
                + $"{foreground}, SetActiveWindow had 0x{active:x}, SetFocus had 0x{focus:x}{(error != 0 ? $", last error {error}" : "")}; now {Describe(GetForegroundWindow())}.");
        }

        private static string Describe(IntPtr handle)
        {
            if (handle == IntPtr.Zero)
                return "none";
            var title = new StringBuilder(256);
            GetWindowTextW(handle, title, title.Capacity);
            var kind = new StringBuilder(256);
            GetClassNameW(handle, kind, kind.Capacity);
            GetWindowThreadProcessId(handle, out uint process);
            string owner = process == (uint)Environment.ProcessId ? "this process" : $"process {process}";
            return $"0x{handle:x} \"{title}\" ({kind}, {owner})";
        }

        [DllImport("user32")]
        private static extern IntPtr GetForegroundWindow();

        [DllImport("user32", SetLastError = true)]
        private static extern bool SetForegroundWindow(IntPtr window);

        [DllImport("user32", SetLastError = true)]
        private static extern IntPtr SetActiveWindow(IntPtr window);

        [DllImport("user32", SetLastError = true)]
        private static extern IntPtr SetFocus(IntPtr window);

        [DllImport("user32", SetLastError = true)]
        private static extern bool BringWindowToTop(IntPtr window);

        [DllImport("user32")]
        private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);

        [DllImport("user32", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string kind, string title);

        [DllImport("user32", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern int GetWindowTextW(IntPtr window, StringBuilder text, int capacity);

        [DllImport("user32", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern int GetClassNameW(IntPtr window, StringBuilder text, int capacity);

        [DllImport("kernel32")]
        private static extern IntPtr GetConsoleWindow();

        [DllImport("kernel32")]
        private static extern uint GetCurrentThreadId();
    }
}
