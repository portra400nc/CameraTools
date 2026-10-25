namespace CameraTools
{
    // Which window Windows has in the foreground, as the game's window sees it: the game's own, another window of the game's
    // process such as MelonLoader's console, none, or another program's.
    internal enum FocusHolder { Game, OwnWindow, Nobody, OtherApp }

    // Unity's Application.isFocused, and the foreground's holder.
    internal readonly record struct FocusView(bool Focused, FocusHolder Holder)
    {
        public bool Whole => Focused && Holder == FocusHolder.Game;
    }

    // A repair of the game window's focus that CameraTools' own change asked for: why, until when it may try, when it tries
    // next, and how often it has tried.
    internal readonly record struct Refocus(string Reason, float Until, float NextTry, int Tries);

    internal enum RefocusStep { Wait, Activate, Done, GiveUp, Leave }

    // The repair's rules, apart from Win32 and Unity, so tests run them. It watches for a few seconds after CameraTools
    // resized the window or turned the free camera off, as Wine applies a resize frames later, activates the game's window
    // whenever it is not whole, at most every Interval, and reports at the end. It stops at once while another program
    // holds the foreground, so a game the user sent to the background stays there.
    internal static class RefocusRules
    {
        public const float Seconds = 6f;
        public const float Interval = 0.5f;

        public static Refocus Start(string reason, float now) => new(reason, now + Seconds, now, 0);

        public static RefocusStep Step(Refocus refocus, FocusView view, float now)
            => view.Holder == FocusHolder.OtherApp ? RefocusStep.Leave
            : now >= refocus.Until ? view.Whole ? RefocusStep.Done : RefocusStep.GiveUp
            : !view.Whole && now >= refocus.NextTry ? RefocusStep.Activate
            : RefocusStep.Wait;

        public static Refocus Tried(Refocus refocus, float now) => refocus with { NextTry = now + Interval, Tries = refocus.Tries + 1 };
    }
}
