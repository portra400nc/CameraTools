using CameraToolsPhotoreal;
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

        // Lens depth of field, after Settling: waits for the shader to switch off, the Photoreal add-on to empty its sum for
        // Generation, and the focus distance. Since is when this wait began.
        public sealed record Focusing(ShotBefore Before, bool Resized, float Started, float Since, uint Generation) : Busy(Before, Resized, Started);

        // Index is the lens sample on screen, and Frame how many frames it has been there.
        public sealed record Sampling(ShotBefore Before, bool Resized, float Started, uint Generation, LensPlan Plan, int Index, int Frame)
            : Busy(Before, Resized, Started);

        // The camera is back at the centre, and the add-on was asked to present the average at the bridge's present count
        // Presents, at the time Since.
        public sealed record Presenting(ShotBefore Before, bool Resized, float Started, float Since, uint Generation, LensPlan Plan, ulong Presents)
            : Busy(Before, Resized, Started);

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
        private const float LensTimeout = 15f;
        // Frames each lens sample stays on screen. The first is marked: whether the add-on takes the camera push before the
        // frame renders or one frame later, the frame it adds then shows that sample.
        private const int HoldFrames = 2;
        // Presents after the add-on reports present mode before the capture, so the average reaches ReShade's frame.
        private const ulong PresentFrames = 2;

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
            new("the depth of field to focus", (_, _) => focusing is float since && Time.unscaledTime - since >= DepthOfField.FocusSeconds),
            new("the graphics settings to settle",
                (settling, _) => Time.unscaledTime - Math.Max(settling.Started, Graphics.Changed) >= SettleSeconds),
            new($"{CleanFrames} frames without the CameraTools UI", (settling, bridge) => bridge.Presents >= settling.Dirty + CleanFrames),
        };

        // Each runs even if an earlier one throws.
        private static readonly (string What, Action<ShotState.Busy> Run)[] Restores =
        {
            ("the lens samples", _ =>
            {
                LensDepthOfField.Centre();
                LensDepthOfField.Accumulate(PhotorealAccumulateMode.Off, 0);
                if (shaderSuspended)
                    DepthOfField.Resume();
                shaderSuspended = false;
            }),
            ("the graphics settings", busy => Graphics.PutBack(busy.Before.Graphics)),
            ("the resolution", busy =>
            {
                if (busy.Resized || Window != busy.Before.Window)
                    Graphics.Resize(busy.Before.Window, quiet: true);
            }),
            // Off after the shot whatever they were before, as the user asked, unless sphere lights need them.
            ("ReShade's effects", _ => ReShade.SetEffects(Lights.KeepsEffectsOn)),
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
        // Since when ReShade has drawn its effects at the screenshot's size without a break. The depth of field's
        // autofocus only moves while it is drawn, and starts again when ReShade loads its effects again.
        private static float? focusing;
        // The lens samples switched iMMERSE's depth of field off, and Finish switches it on again.
        private static bool shaderSuspended;

        public static ShotState State { get; private set; } = new ShotState.Idle();

        // The lens samples move the camera, so the user's input must not.
        public static bool HoldsCamera => State is ShotState.Focusing or ShotState.Sampling or ShotState.Presenting;

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
            if (LensDepthOfField.Enabled && !Photoreal.Connected)
            {
                CameraUi.Toast("Lens depth of field needs the Photoreal add-on");
                return;
            }
            float now = Time.unscaledTime;
            var before = new ShotBefore(CameraUi.PanelOpen, uiHidden, Graphics.Capture(), Window);
            var slot = Graphics.Slot(1);
            bool resized = slot != before.Window;
            focusing = null;
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
                if (LensDepthOfField.Enabled)
                    preset += $"; lens depth of field, {Graphics.AntiAliasingOff()}";
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
                    : HoldsCamera && !Photoreal.Connected ? "Screenshot failed: the Photoreal add-on was unloaded"
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
                var bridge = ReShade.Status;
                if (resize != null || bridge.Runtime != 1 || bridge.EffectsReady != 1 || bridge.EffectsEnabled != 1
                    || bridge.Width != Screen.width || bridge.Height != Screen.height)
                    focusing = null;
                else
                    focusing ??= Time.unscaledTime;
                switch (busy)
                {
                    case ShotState.Countdown counting:
                        Count(counting);
                        break;
                    case ShotState.Settling settling:
                        Settle(settling);
                        break;
                    case ShotState.Focusing lensFocusing:
                        Focus(lensFocusing);
                        break;
                    case ShotState.Sampling sampling:
                        Sample(sampling);
                        break;
                    case ShotState.Presenting presenting:
                        Present(presenting);
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
                Melon<CameraTools>.Logger.Msg($"Screenshot: settled {now - settling.Started:0.0} s after the button, "
                    + $"{now - settling.Since:0.0} s of them waiting after the countdown; window {Window}; {bridge}.");
                if (LensDepthOfField.Enabled)
                    StartLens(settling, now);
                else
                    Capture(settling, now, bridge);
                return;
            }
            if (now - settling.Since >= SettleTimeout)
                Finish(settling, $"Screenshot failed: timed out waiting for {unmet.Name}",
                    $"timed out after {SettleTimeout:0} s waiting for {unmet.Name}; window {Window}; {bridge}");
        }

        private static void Capture(ShotState.Busy busy, float now, BridgeStatus bridge)
        {
            ReShade.SaveScreenshot();
            Melon<CameraTools>.Logger.Msg($"Screenshot: capture requested {now - busy.Started:0.0} s after the button; window {Window}; {bridge}.");
            State = new ShotState.Saving(busy.Before, busy.Resized, busy.Started, now, bridge.Screenshots);
        }

        // A generation the add-on's sum does not have yet, so it empties before the first sample.
        private static void StartLens(ShotState.Settling settling, float now)
        {
            uint generation = Photoreal.Status.AccumGeneration + 1;
            shaderSuspended = DepthOfField.Suspend();
            LensDepthOfField.Accumulate(PhotorealAccumulateMode.Add, generation);
            State = new ShotState.Focusing(settling.Before, settling.Resized, settling.Started, now, generation);
        }

        private static void Focus(ShotState.Focusing focusing)
        {
            float now = Time.unscaledTime;
            var focus = DepthOfField.LensFocus(cam.nearClipPlane, cam.farClipPlane);
            string waiting = !DepthOfField.Off ? "the depth of field shader to switch off"
                : Photoreal.Status.AccumGeneration != focusing.Generation ? "the Photoreal add-on to start accumulating"
                : focus == null ? "the focus distance"
                : null;
            if (waiting == null)
            {
                var (metres, from) = focus.Value;
                float fNumber = DepthOfField.FNumber;
                var plan = LensDepthOfField.Plan(metres, fNumber);
                Melon<CameraTools>.Logger.Msg($"Screenshot: lens depth of field focused at {metres:0.00} m from {from}; f/{fNumber:0.##}, "
                    + $"aperture radius {plan.Radius * 1000f:0.00} mm, {plan.Count} samples, "
                    + $"{(plan.Blades >= 3 ? $"{plan.Blades} blades at {plan.Rotation:0.#} degrees" : "round")}, cat's eye {LensDepthOfField.CatEye:0.##}.");
                var sampling = new ShotState.Sampling(focusing.Before, focusing.Resized, focusing.Started, focusing.Generation, plan, 0, 0);
                State = sampling;
                Sample(sampling);
                return;
            }
            if (now - focusing.Since >= LensTimeout)
                Finish(focusing, $"Screenshot failed: timed out waiting for {waiting}",
                    $"timed out after {LensTimeout:0} s waiting for {waiting}; {Photoreal.Describe()}");
        }

        // One frame of one sample. Runs before the free camera applies its pose and pushes the camera this frame.
        private static void Sample(ShotState.Sampling sampling)
        {
            if (sampling.Index == sampling.Plan.Count)
            {
                LensDepthOfField.Centre();
                LensDepthOfField.Accumulate(PhotorealAccumulateMode.Present, sampling.Generation);
                State = new ShotState.Presenting(sampling.Before, sampling.Resized, sampling.Started, Time.unscaledTime, sampling.Generation,
                    sampling.Plan, ReShade.Status.Presents);
                return;
            }
            LensDepthOfField.Show(sampling.Plan, sampling.Index, mark: sampling.Frame == 0);
            State = sampling.Frame + 1 < HoldFrames ? sampling with { Frame = sampling.Frame + 1 } : sampling with { Index = sampling.Index + 1, Frame = 0 };
        }

        private static void Present(ShotState.Presenting presenting)
        {
            float now = Time.unscaledTime;
            var status = Photoreal.Status;
            var bridge = ReShade.Status;
            if (status.AccumMode == PhotorealAccumulateMode.Present && status.AccumGeneration == presenting.Generation
                && bridge.Presents >= presenting.Presents + PresentFrames)
            {
                if (status.AccumSamples == 0)
                {
                    Finish(presenting, "Screenshot failed: no lens sample reached the Photoreal add-on",
                        $"the add-on added no lens sample; {Photoreal.Describe()}");
                    return;
                }
                Melon<CameraTools>.Logger.Msg($"Screenshot: the add-on averaged {status.AccumSamples} of {presenting.Plan.Count} lens samples "
                    + $"in {now - presenting.Started:0.0} s since the button; {Photoreal.Describe()}");
                Capture(presenting, now, bridge);
                return;
            }
            if (now - presenting.Since >= LensTimeout)
                Finish(presenting, "Screenshot failed: the Photoreal add-on did not present the lens samples",
                    $"timed out after {LensTimeout:0} s waiting for the add-on to present generation {presenting.Generation}; {Photoreal.Describe()}");
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
            focusing = null;
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
