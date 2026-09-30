using UnityEngine;
using static CameraTools.CameraTools;

namespace CameraTools
{
    // WasHidden is whether the UI was hidden before playback, which stopping puts back.
    internal abstract record PlaybackState
    {
        public sealed record Idle : PlaybackState;

        // Shown is the number last shown in the countdown, 0 before the first.
        public sealed record Countdown(float Until, int Shown, bool WasHidden) : PlaybackState;

        // Everything that shapes the flight is fixed when it starts; only the shake follows the settings while it plays.
        // Repause: playback unpaused the game, so stopping pauses it again.
        public sealed record Playing(PathSampler Sampler, int Number, float Duration, bool Loop, bool EaseIn, bool EaseOut,
            float Started, bool WasHidden, bool Repause) : PlaybackState
        {
            public float Elapsed => Time.unscaledTime - Started;

            public float PassTime => PathSampler.PassTime(Elapsed, Duration, Loop);

            public bool Finished => !Loop && Elapsed >= Duration;

            public CameraNode Sample() => Sampler.Sample(PathSampler.Ease(PassTime / Duration, EaseIn, EaseOut));
        }
    }

    // Plays the active camera path through the free camera, on unscaled time so a paused game still plays it.
    internal static class PathPlayback
    {
        private const float CountdownSeconds = 3f;
        // Metres and degrees at shake strength 1 at the noise's peak, so OPM's suggested 0.5 moves the camera a few
        // centimetres and turns it a fraction of a degree, like a handheld camera, and 5 is strong.
        private const float MoveShake = 0.1f;
        private const float RotateShake = 1f;

        public static PlaybackState State { get; private set; } = new PlaybackState.Idle();

        public static bool Idle => State is PlaybackState.Idle;

        public static void Update()
        {
            if (!Idle && !freecamActive)
                Stop("Playback stopped");
            if (Controls.Pressed(CamAction.PlayPath))
            {
                if (Idle)
                    Play();
                else
                    Stop("Playback stopped");
            }
            if (Controls.Pressed(CamAction.AddPathNode) && Idle)
                CameraPaths.AddNode();
            for (int number = 1; number <= 9; number++)
                if (Controls.Pressed(CamAction.SelectPath1 + number - 1))
                    SelectPath(number);
            if (State is PlaybackState.Countdown countdown)
                Count(countdown);
        }

        // Playing again restarts the active path from its first node.
        public static void Play()
        {
            if (!CameraPaths.FreecamOn() || !Playable())
                return;
            Stop(null);
            CameraUi.ClosePanel();
            bool wasHidden = uiHidden;
            if (!CameraPaths.Options.Countdown)
            {
                Begin(wasHidden);
                return;
            }
            if (uiHidden)
                SetUiHidden(false);
            State = new PlaybackState.Countdown(Time.unscaledTime + CountdownSeconds, 0, wasHidden);
            Count((PlaybackState.Countdown)State);
        }

        // message is the toast to show, or null for none.
        public static void Stop(string message)
        {
            switch (State)
            {
                case PlaybackState.Countdown countdown:
                    RestoreUi(countdown.WasHidden);
                    break;
                case PlaybackState.Playing playing:
                    RestoreUi(playing.WasHidden);
                    if (playing.Repause)
                        SetPaused(true);
                    break;
                default:
                    return;
            }
            State = new PlaybackState.Idle();
            if (message != null)
                CameraUi.Toast(message);
        }

        // Poses the free camera while a path plays, and returns whether it did, in which case camera input is ignored.
        public static bool Drive(Freecam freecam)
        {
            if (State is not PlaybackState.Playing playing)
                return false;
            var node = playing.Sample();
            var position = WorldShift.Relative(node.Position.ToUnity());
            var rotation = node.Rotation.ToUnity();
            var options = CameraPaths.Options;
            float now = Time.unscaledTime;
            if (options.MoveShakeStrength > 0f)
            {
                float scale = options.MoveShakeStrength * MoveShake;
                float at = now * options.MoveShakeFrequency;
                position += rotation * new Vector3 { x = Noise(at, 0) * scale, y = Noise(at, 1) * scale, z = Noise(at, 2) * scale };
            }
            if (options.RotateShakeStrength > 0f)
            {
                float scale = options.RotateShakeStrength * RotateShake;
                float at = now * options.RotateShakeFrequency;
                rotation *= Quaternion.Euler(Noise(at, 3) * scale, Noise(at, 4) * scale, Noise(at, 5) * scale);
            }
            freecam.Snap(position, rotation, node.Fov);
            if (playing.Finished)
                Stop("Playback finished");
            return true;
        }

        // During playback, a path hotkey switches playback to that path from its start.
        private static void SelectPath(int number)
        {
            if (number > CameraPaths.Paths.Count)
            {
                CameraUi.Toast($"There is no path {number}");
                return;
            }
            CameraPaths.SelectPath(number - 1);
            if (State is PlaybackState.Playing playing)
            {
                if (Playable())
                    State = Start(playing.WasHidden, playing.Repause);
                return;
            }
            CameraUi.Toast($"Path {number} selected");
        }

        private static void Count(PlaybackState.Countdown countdown)
        {
            float remaining = countdown.Until - Time.unscaledTime;
            if (remaining <= 0f)
            {
                Begin(countdown.WasHidden);
                return;
            }
            int shown = (int)MathF.Ceiling(remaining);
            if (shown == countdown.Shown)
                return;
            CameraUi.Toast(countdown.Shown == 0 ? $"Playing in {shown}" : $"{shown}");
            State = countdown with { Shown = shown };
        }

        // The path can change during the countdown, so it is checked again here.
        private static void Begin(bool wasHidden)
        {
            if (!Playable())
            {
                RestoreUi(wasHidden);
                State = new PlaybackState.Idle();
                return;
            }
            var options = CameraPaths.Options;
            SetUiHidden(options.HideUi);
            bool repause = options.UnpauseGame && Time.timeScale == 0f;
            if (repause)
                SetPaused(false);
            State = Start(wasHidden, repause);
        }

        private static PlaybackState.Playing Start(bool wasHidden, bool repause)
        {
            var path = CameraPaths.Current;
            var options = CameraPaths.Options;
            return new PlaybackState.Playing(new PathSampler(path.Nodes, options.ConstantSpeed), CameraPaths.Active + 1, path.Duration,
                options.Loop, options.EaseIn, options.EaseOut, Time.unscaledTime, wasHidden, repause);
        }

        private static bool Playable()
        {
            if (CameraPaths.CanPlay)
                return true;
            CameraUi.Toast(CameraPaths.HasPath ? $"Path {CameraPaths.Active + 1} needs at least 2 nodes" : "Add 2 nodes to play a path");
            return false;
        }

        private static void RestoreUi(bool wasHidden)
        {
            if (uiHidden != wasHidden)
                SetUiHidden(wasHidden);
        }

        // Perlin noise from -1 to 1; each channel reads its own row of the noise, so the axes move independently.
        private static float Noise(float at, int channel) => (Mathf.PerlinNoise(at, 7.31f * channel + 0.5f) - 0.5f) * 2f;
    }
}
