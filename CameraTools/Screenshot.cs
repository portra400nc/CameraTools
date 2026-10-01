using MelonLoader;
using UnityEngine;
using static CameraTools.CameraTools;

namespace CameraTools
{
    // What the screenshot button changes, as it was when the button was pressed.
    internal sealed record ShotBefore(bool PanelOpen, bool UiHidden, GraphicsState Graphics, ScreenSize Window);

    internal abstract record ShotState
    {
        public sealed record Idle : ShotState;

        // Resized: the button asked for another window size, which may not have arrived yet. Started is when the button
        // changed the settings.
        public abstract record Busy(ShotBefore Before, bool Resized, float Started) : ShotState;

        // Shown is the number last shown in the countdown, 0 before the first.
        public sealed record Countdown(ShotBefore Before, bool Resized, float Started, float Until, int Shown) : Busy(Before, Resized, Started);

        // Since is when this wait began. Dirty is the bridge's present count when CameraTools last had something on screen.
        public sealed record Settling(ShotBefore Before, bool Resized, float Started, float Since, ulong Dirty) : Busy(Before, Resized, Started);

        // Shots is the bridge's screenshot count when the capture was requested.
        public sealed record Saving(ShotBefore Before, bool Resized, float Started, float Requested, ulong Shots) : Busy(Before, Resized, Started);
    }

    // The ReShade tab's one-button screenshot: hide everything, raise the quality and the resolution, turn ReShade's
    // effects on, capture through ReShade once the picture has settled, and put everything back. It runs on unscaled
    // time, so a paused game still takes its screenshot.
    internal static class Screenshot
    {
        private const float CountdownSeconds = 3f;
        private const float SettleTimeout = 60f;
        private const float SaveTimeout = 15f;
        private const float MaxSettle = 30f;
        // Presents after the last CameraTools element left the screen, so the countdown is not in the capture.
        private const ulong CleanFrames = 3;
        // Presents between asking ReShade to save its preset and resizing. A resize destroys ReShade's runtime and loads
        // the effects from the preset again, so a save still queued then would lose the depth of field's last changes.
        private const ulong SaveFrames = 3;

        // Name completes "timed out waiting for ...".
        private sealed record Condition(string Name, Func<ShotState.Settling, BridgeStatus, bool> Met);

        // In the order they are reported: the first unmet one is named when the wait times out. A new window size makes
        // ReShade build its runtime and compile its effects again, which can take many seconds the first time at a size.
        private static readonly Condition[] Conditions =
        {
            new("the CameraTools UI to leave the screen", (_, _) => !CameraUi.OnScreen),
            new("ReShade to create its runtime", (_, bridge) => bridge.Runtime == 1),
            new("ReShade to compile its effects", (_, bridge) => bridge.EffectsReady == 1),
            new("ReShade to turn its effects on", (_, bridge) => bridge.EffectsEnabled == 1),
            new("the depth of field's focus paint to clear", (_, _) => !DepthOfField.Painting),
            new("ReShade to save its preset before the resize", (_, _) => resize == null),
            new("ReShade to reach the window's size", (_, bridge) => bridge.Width == Screen.width && bridge.Height == Screen.height),
            new("the render resolution to follow the window", (_, _) => !Graphics.SizePending),
            new("the graphics settings to settle",
                (settling, _) => Time.unscaledTime - Math.Max(settling.Started, Graphics.Changed) >= SettleSeconds),
            new($"{CleanFrames} frames without the CameraTools UI", (settling, bridge) => bridge.Presents >= settling.Dirty + CleanFrames),
        };

        // Each runs even if an earlier one throws.
        private static readonly (string What, Action<ShotState.Busy> Run)[] Restores =
        {
            ("the graphics settings", busy => Graphics.PutBack(busy.Before.Graphics)),
            ("the resolution", busy =>
            {
                if (busy.Resized || Window != busy.Before.Window)
                    Graphics.Resize(busy.Before.Window, quiet: true);
            }),
            // Off after the shot whatever they were before, as the user asked.
            ("ReShade's effects", _ => ReShade.SetEffects(false)),
            ("the UI", busy =>
            {
                if (uiHidden != busy.Before.UiHidden)
                    SetUiHidden(busy.Before.UiHidden);
                if (busy.Before.PanelOpen && freecamActive)
                    CameraUi.OpenPanel();
            }),
        };

        private static MelonPreferences_Entry<bool> countdown;
        private static MelonPreferences_Entry<float> settle;
        // The size to change to once the bridge has presented After frames.
        private static (ScreenSize Size, ulong After)? resize;

        public static ShotState State { get; private set; } = new ShotState.Idle();

        public static bool Countdown
        {
            get => countdown.Value;
            set
            {
                countdown.Value = value;
                MelonPreferences.Save();
            }
        }

        private static float SettleSeconds => Math.Clamp(settle.Value, 0f, MaxSettle);

        private static ScreenSize Window => new(Screen.width, Screen.height);

        public static void Load()
        {
            var category = MelonPreferences.CreateCategory("CameraToolsReShade");
            countdown = category.CreateEntry("ScreenshotCountdown", true,
                description: "Count down 3 seconds before the screenshot, while the settings settle.");
            settle = category.CreateEntry("ScreenshotSettleSeconds", 2f,
                description: $"The least time, in seconds, between changing the graphics settings and taking the screenshot, from 0 to {MaxSettle:0}.");
            MelonPreferences.Save();
        }

        public static void Take()
        {
            if (State is not ShotState.Idle || !freecamActive || !ReShade.Connected)
                return;
            float now = Time.unscaledTime;
            var before = new ShotBefore(CameraUi.PanelOpen, uiHidden, Graphics.Capture(), Window);
            var slot = Graphics.Slot(1);
            bool resized = slot != before.Window;
            var bridge = ReShade.Status;
            ShotState.Busy busy = Countdown
                ? new ShotState.Countdown(before, resized, now, now + CountdownSeconds, 0)
                : new ShotState.Settling(before, resized, now, now, bridge.Presents);
            // Before the first change, so a failure below puts back whatever was changed.
            State = busy;
            try
            {
                DepthOfField.Flush();
                CameraUi.ClosePanel();
                SetUiHidden(true);
                string preset = Graphics.ApplyQuietly(Graphics.Screenshot);
                if (resized)
                    resize = (slot, bridge.Presents + SaveFrames);
                ReShade.SetEffects(true);
                Melon<CameraTools>.Logger.Msg($"Screenshot: started. Window {before.Window}, slot 1 {slot}{(resized ? "" : " (no resize)")}; {preset}; "
                    + $"countdown {(Countdown ? "on" : "off")}, settle {SettleSeconds:0.##} s; {bridge}.");
            }
            catch (Exception e)
            {
                Finish(busy, "Screenshot failed; see the log", $"starting failed: {e}");
            }
        }

        public static void Update()
        {
            if (State is not ShotState.Busy busy)
                return;
            try
            {
                // Hide UI, opening the settings panel and a camera path's countdown all show the UI again.
                string stopped = !freecamActive ? "Screenshot cancelled: the free camera is off"
                    : !uiHidden ? "Screenshot cancelled"
                    : !ReShade.Connected ? "Screenshot failed: ReShade was unloaded"
                    : null;
                if (stopped != null)
                {
                    Finish(busy, stopped);
                    return;
                }
                if (resize is var (size, after) && ReShade.Status.Presents >= after)
                {
                    resize = null;
                    Graphics.Resize(size, quiet: true);
                }
                switch (busy)
                {
                    case ShotState.Countdown counting:
                        Count(counting);
                        break;
                    case ShotState.Settling settling:
                        Settle(settling);
                        break;
                    case ShotState.Saving saving:
                        Save(saving);
                        break;
                }
            }
            catch (Exception e)
            {
                Finish(busy, "Screenshot failed; see the log", $"failed while {busy.GetType().Name}: {e}");
            }
        }

        private static void Count(ShotState.Countdown counting)
        {
            float now = Time.unscaledTime;
            float remaining = counting.Until - now;
            if (remaining <= 0f)
            {
                CameraUi.ClearToast();
                State = new ShotState.Settling(counting.Before, counting.Resized, counting.Started, now, ReShade.Status.Presents);
                return;
            }
            int shown = (int)MathF.Ceiling(remaining);
            if (shown == counting.Shown)
                return;
            CameraUi.Toast(counting.Shown == 0 ? $"Screenshot in {shown}" : $"{shown}", whileHidden: true);
            State = counting with { Shown = shown };
        }

        private static void Settle(ShotState.Settling settling)
        {
            float now = Time.unscaledTime;
            var bridge = ReShade.Status;
            // A runtime that ReShade created again for the new size may come back with its switch off.
            if (bridge.Runtime == 1 && bridge.EffectsEnabled == 0)
                ReShade.SetEffects(true);
            if (CameraUi.OnScreen)
            {
                settling = settling with { Dirty = bridge.Presents };
                State = settling;
            }
            var unmet = Array.Find(Conditions, condition => !condition.Met(settling, bridge));
            if (unmet == null)
            {
                ReShade.SaveScreenshot();
                Melon<CameraTools>.Logger.Msg($"Screenshot: capture requested {now - settling.Started:0.0} s after the button, "
                    + $"{now - settling.Since:0.0} s of them waiting after the countdown; window {Window}; {bridge}.");
                State = new ShotState.Saving(settling.Before, settling.Resized, settling.Started, now, bridge.Screenshots);
                return;
            }
            if (now - settling.Since >= SettleTimeout)
                Finish(settling, $"Screenshot failed: timed out waiting for {unmet.Name}",
                    $"timed out after {SettleTimeout:0} s waiting for {unmet.Name}; window {Window}; {bridge}");
        }

        private static void Save(ShotState.Saving saving)
        {
            var bridge = ReShade.Status;
            if (bridge.Screenshots > saving.Shots)
            {
                string path = ReShade.LastScreenshot;
                Finish(saving, path.Length > 0 ? $"Saved {Path.GetFileName(path)}" : "Screenshot saved",
                    $"saved {(path.Length > 0 ? path : "a file whose path the bridge did not return")} "
                    + $"{Time.unscaledTime - saving.Started:0.0} s after the button");
                return;
            }
            if (Time.unscaledTime - saving.Requested >= SaveTimeout)
                Finish(saving, $"Screenshot failed: ReShade did not save it within {SaveTimeout:0} s",
                    $"ReShade did not save the capture within {SaveTimeout:0} s; {bridge}");
        }

        // The one way out of every case. detail is the log's version of the toast, where it says more.
        private static void Finish(ShotState.Busy busy, string toast, string detail = null)
        {
            State = new ShotState.Idle();
            resize = null;
            var log = Melon<CameraTools>.Logger;
            foreach (var (what, run) in Restores)
            {
                try
                {
                    run(busy);
                }
                catch (Exception e)
                {
                    log.Warning($"Screenshot: putting back {what} failed: {e}");
                }
            }
            log.Msg($"Screenshot: {detail ?? toast}.");
            CameraUi.Toast(toast);
        }
    }
}
