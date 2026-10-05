using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // A throwaway test of the Light Shafts shader, before its controls are designed. Back+D-pad right (Numpad 0) shows the
    // next state with Light Shafts running alone in ReShade, and the step after the last puts ReShade back as it was;
    // Back+D-pad left (Numpad .) goes back a state. The light stays at the shader's default spot, near the top middle of
    // the screen and at the sky, so the camera frames the sun through trees. Each state's frame rate is logged.
    internal static class ShaftsTest
    {
        private const string Effect = "LightShafts.fx";
        private const float FrameRateWindow = 3f;

        private sealed record State(string Name, float Intensity, float Haze = 0f, float Length = 0.8f, float Fade = 0.4f, int Debug = 0);

        private static readonly State[] States =
        {
            new("Intensity 1", 1f),
            new("Intensity 2", 2f),
            new("Intensity 4", 4f),
            new("Intensity 2, haze 0.3", 2f, 0.3f),
            new("Intensity 3, haze 0.6, long rays", 3f, 0.6f, 1f, 0.2f),
            new("Light source view", 2f, Debug: 1),
            new("Rays only view", 3f, 0.6f, 1f, 0.2f, Debug: 2),
        };

        private static readonly PadBinding NextPad = new(PadButtons.Back | PadButtons.DpadRight, PadAxis.None);
        private static readonly PadBinding PreviousPad = new(PadButtons.Back | PadButtons.DpadLeft, PadAxis.None);
        private static readonly EffectName IntensityName = new(Effect, "Intensity");
        private static readonly EffectName HazeName = new(Effect, "Haze");
        private static readonly EffectName LengthName = new(Effect, "RayLength");
        private static readonly EffectName FadeName = new(Effect, "Fade");
        private static readonly EffectName DebugName = new(Effect, "DebugView");

        private static int state = -1;
        private static bool? effectsBefore;
        private static float windowTime;
        private static int windowFrames;
        private static bool windowLogged = true;

        public static void Update()
        {
            bool pad = Controls.Owner == PadOwner.CameraTools && CameraUi.View is View.Hud;
            bool keys = !Controls.TextCapture;
            if (keys && Input.GetKeyDown(KeyCode.Keypad0) || pad && NextPad.Pressed(Gamepad.Current, Gamepad.Previous))
                Show(state + 1);
            else if (keys && Input.GetKeyDown(KeyCode.KeypadPeriod) || pad && PreviousPad.Pressed(Gamepad.Current, Gamepad.Previous))
                Show(Math.Max(state - 1, 0));
            CountFrames();
        }

        private static void Show(int next)
        {
            if (!ReShade.Connected)
            {
                CameraUi.Toast("Light Shafts test: " + ReShade.Missing);
                return;
            }
            var log = Melon<CameraTools>.Logger;
            if (next >= States.Length)
            {
                state = -1;
                ReShade.SetOverride(Array.Empty<string>(), false);
                if (effectsBefore is bool before)
                    ReShade.SetEffects(before);
                effectsBefore = null;
                CameraUi.Toast("Light Shafts test finished");
                log.Msg("Light Shafts test: finished; ReShade is back as it was.");
                StartWindow();
                return;
            }
            if (state < 0)
            {
                effectsBefore = ReShade.Status.EffectsEnabled == 1;
                ReShade.SetEffects(true);
                ReShade.SetOverride(new[] { Effect }, true);
            }
            state = next;
            var shown = States[state];
            ReShade.SetFloat(IntensityName, shown.Intensity);
            ReShade.SetFloat(HazeName, shown.Haze);
            ReShade.SetFloat(LengthName, shown.Length);
            ReShade.SetFloat(FadeName, shown.Fade);
            ReShade.SetInt(DebugName, shown.Debug);
            string label = $"Light Shafts {state + 1}/{States.Length}: {shown.Name}";
            CameraUi.Toast(label);
            log.Msg($"{label}. {shown}.");
            StartWindow();
        }

        private static void StartWindow()
        {
            windowTime = 0f;
            windowFrames = 0;
            windowLogged = false;
        }

        // Light Shafts compiles when first switched on, so the frame rate is logged after the window, not from the first frame.
        private static void CountFrames()
        {
            if (windowLogged)
                return;
            windowTime += Time.unscaledDeltaTime;
            windowFrames++;
            if (windowTime < FrameRateWindow)
                return;
            windowLogged = true;
            string shown = state >= 0 ? States[state].Name : "Light Shafts off";
            Melon<CameraTools>.Logger.Msg($"Light Shafts test: {windowFrames / windowTime:0.0} fps over {windowTime:0.0} s with {shown}; "
                + $"Light Shafts loaded {ReShade.TryGetTechnique(new EffectName(Effect, "LightShafts"), out _)}.");
        }
    }
}
