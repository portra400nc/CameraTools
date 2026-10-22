using UnityEngine;

namespace CameraTools
{
    // Edit replay: the replay holds still at a playhead on a timeline while the free camera frames the shot, and Place
    // node pins the camera's view to the replay's camera path at that moment, so playing the path reaches that framing
    // then. The pad's buttons and the keys are fixed, like Pose joints'; while editing they replace the actions bound to
    // the same buttons and keys, and the free camera's movement and look keep working.
    internal static class ReplayEditor
    {
        // A held frame step starts scrubbing after this long, like the panel's held D-pad.
        private const float ScrubDelay = 0.4f;
        // A step on a replay without character frames.
        private const float FallbackStep = 1f / 30f;

        private static readonly PadBinding StepBack = new(PadButtons.DpadLeft, PadAxis.None);
        private static readonly PadBinding StepOn = new(PadButtons.DpadRight, PadAxis.None);
        private static readonly PadBinding NodeBack = new(PadButtons.Back | PadButtons.DpadLeft, PadAxis.None);
        private static readonly PadBinding NodeOn = new(PadButtons.Back | PadButtons.DpadRight, PadAxis.None);
        private static readonly PadBinding PlayPad = new(PadButtons.X, PadAxis.None);
        private static readonly PadBinding PlacePad = new(PadButtons.Y, PadAxis.None);
        private static readonly PadBinding DeletePad = new(PadButtons.Back | PadButtons.Y, PadAxis.None);
        private static readonly PadBinding LookPad = new(PadButtons.A, PadAxis.None);
        private static readonly PadBinding DonePad = new(PadButtons.B, PadAxis.None);

        public static readonly PadBinding[] Buttons = { StepBack, StepOn, NodeBack, NodeOn, PlayPad, PlacePad, DeletePad, LookPad, DonePad };

        // Left Shift, a modifier here, stays fast movement. Enter plays, because Space raises the camera.
        public static readonly KeyCode[] Keys =
        {
            KeyCode.LeftArrow, KeyCode.RightArrow, KeyCode.RightShift, KeyCode.Return, KeyCode.N, KeyCode.Backspace, KeyCode.V,
            KeyCode.Escape,
        };

        // The held frame step's direction, and since when it has been held.
        private static int scrubbing;
        private static float scrubFrom;
        private static (CameraPath Path, int Revision, PathSampler Sampler) cached;

        public static bool Editing => ReplayPlayer.Editing != null;

        // Whether editing reads a key or a pad binding itself, so the action bound to it must not also fire.
        public static bool Reads(KeyCode key) => Editing && Array.IndexOf(Keys, key) >= 0;

        public static bool Reads(PadBinding binding) => Editing && binding.Axis == PadAxis.None && Array.IndexOf(Buttons, binding) >= 0;

        public static string Closed(CameraPath path) => $"Edit closed · {EditTimeline.Nodes(path?.Nodes.Count ?? 0)} on the path";

        // The selected replay's path, or a new one as long as the replay when it has none.
        public static void Start()
        {
            if (ReplayRecorder.Active)
                ReplayRecorder.Stop();
            if (Replays.Current is not { } replay || !CameraPaths.FreecamOn())
                return;
            PathPlayback.Stop(null);
            if (Replays.PathNumber(replay) is int number)
                CameraPaths.SelectPath(number - 1);
            else
            {
                var path = CameraPaths.NewPath(replay.Name, replay.Duration);
                replay.PathId = path.Id;
                replay.Offset = 0f;
                CameraUi.Toast($"{replay.Name} now has {CameraPaths.CurrentLabel} for its camera nodes");
            }
            CameraUi.ClosePanel();
            scrubbing = 0;
            ReplayPlayer.Edit(replay);
        }

        // Before the replay player's frame, so a step shows in the same frame.
        public static void Update()
        {
            if (ReplayPlayer.Editing is not { } editing)
                return;
            var path = PathOf(editing.Replay);
            if (path == null)
            {
                ReplayPlayer.Stop("Edit closed: the replay's camera path is gone");
                return;
            }
            if (editing.Look && path.Nodes.Count < 2)
                editing.Look = false;
            var now = Gamepad.Current;
            var before = Gamepad.Previous;
            bool pad = Controls.Owner == InputOwner.CameraTools;
            bool shift = Input.GetKey(KeyCode.LeftShift) || Input.GetKey(KeyCode.RightShift);
            bool Pressed(PadBinding binding, KeyCode key = KeyCode.None) => pad && binding.Pressed(now, before) || key != KeyCode.None && Input.GetKeyDown(key);

            if (Pressed(DonePad, KeyCode.Escape))
            {
                ReplayPlayer.Stop(Closed(path));
                CameraUi.OpenPanel();
                return;
            }
            if (Pressed(NodeBack) || shift && Input.GetKeyDown(KeyCode.LeftArrow))
                Jump(editing, path, -1);
            else if (Pressed(NodeOn) || shift && Input.GetKeyDown(KeyCode.RightArrow))
                Jump(editing, path, 1);
            else
                Scrub(editing, pad && StepBack.Held(now) || !shift && Input.GetKey(KeyCode.LeftArrow),
                    pad && StepOn.Held(now) || !shift && Input.GetKey(KeyCode.RightArrow));
            if (Pressed(PlayPad, KeyCode.Return))
            {
                if (!editing.Plays && editing.T >= editing.Replay.Duration)
                    editing.T = 0f;
                editing.Plays = !editing.Plays;
            }
            if (Pressed(DeletePad, KeyCode.Backspace))
                Delete(editing, path);
            else if (Pressed(PlacePad, KeyCode.N))
                Place(editing, path);
            if (Pressed(LookPad, KeyCode.V))
                Look(editing, path);
            if (editing.Plays)
            {
                editing.T = Math.Min(editing.T + Time.unscaledDeltaTime, editing.Replay.Duration);
                editing.Plays = editing.T < editing.Replay.Duration;
            }
        }

        // Looking through the path, the camera is posed from it at the playhead, as playing the path poses it there.
        public static bool Drive(Freecam freecam)
        {
            if (ReplayPlayer.Editing is not { Look: true } editing || PathOf(editing.Replay) is not { Nodes.Count: >= 2 } path)
                return false;
            var sampler = Sampler(path);
            PathPlayback.Pose(freecam, sampler.Sample(sampler.Progress(PathTime(editing) / path.Duration)));
            return true;
        }

        public static void Seek(float t)
        {
            if (ReplayPlayer.Editing is { } editing)
                editing.T = Math.Clamp(t, 0f, editing.Replay.Duration);
        }

        public static void JumpTo(int node)
        {
            if (ReplayPlayer.Editing is { } editing && PathOf(editing.Replay) is { } path && node >= 0 && node < path.Nodes.Count)
                Seek(Moments(editing, path)[node]);
        }

        public static CameraPath PathOf(Replay replay) => replay?.PathId == null ? null : CameraPaths.Paths.Find(path => path.Id == replay.PathId);

        // Each node's moment on the timeline, in replay seconds: when playing the path reaches it, less the replay's
        // offset into the path.
        public static float[] Moments(PlayState.Editing editing, CameraPath path)
        {
            if (path.Nodes.Count == 0)
                return Array.Empty<float>();
            return Sampler(path).NodeTimes().Select(share => share * path.Duration - editing.Replay.Offset).ToArray();
        }

        private static float PathTime(PlayState.Editing editing) => editing.T + editing.Replay.Offset;

        // Changing the options saves them, which raises the revision.
        private static PathSampler Sampler(CameraPath path)
        {
            var options = CameraPaths.Options;
            if (cached.Path != path || cached.Revision != CameraPaths.Revision)
                cached = (path, CameraPaths.Revision, new PathSampler(path.Nodes, options.ConstantSpeed, path.Duration, options.EaseIn, options.EaseOut));
            return cached.Sampler;
        }

        // A press steps one recorded frame; held, it scrubs faster and faster in real time.
        private static void Scrub(PlayState.Editing editing, bool back, bool on)
        {
            int direction = back == on ? 0 : back ? -1 : 1;
            if (direction != scrubbing)
            {
                scrubbing = direction;
                scrubFrom = Time.unscaledTime;
                if (direction != 0)
                    Seek(Step(editing, direction));
                return;
            }
            float held = Time.unscaledTime - scrubFrom - ScrubDelay;
            if (direction != 0 && held > 0f)
                Seek(editing.T + direction * EditTimeline.ScrubRate(held) * Time.unscaledDeltaTime);
        }

        private static float Step(PlayState.Editing editing, int direction)
        {
            var frames = editing.Replay.Of(TrackKind.Character).Select(track => track.Poses).Where(poses => poses.Count > 0).ToArray();
            return frames.Length > 0 ? EditTimeline.Step(frames, editing.T, direction) : editing.T + direction * FallbackStep;
        }

        private static void Jump(PlayState.Editing editing, CameraPath path, int direction)
        {
            var moments = Moments(editing, path);
            int node = EditTimeline.Neighbour(moments, editing.T, direction);
            if (node < 0)
                CameraUi.Toast(direction > 0 ? "No node after the playhead" : "No node before the playhead");
            else
                Seek(moments[node]);
        }

        // A node at the playhead takes the view in its place.
        private static void Place(PlayState.Editing editing, CameraPath path)
        {
            int here = EditTimeline.NodeAt(Moments(editing, path), editing.T);
            if (CameraPaths.PlaceView(path, PathTime(editing), here >= 0 ? here : null) is not int index)
                return;
            CameraUi.Toast(here >= 0 ? $"Node {index + 1} now uses this view at {editing.T:0.00} s" : $"Node {index + 1} placed at {editing.T:0.00} s");
        }

        private static void Delete(PlayState.Editing editing, CameraPath path)
        {
            int here = EditTimeline.NodeAt(Moments(editing, path), editing.T);
            if (here < 0)
            {
                CameraUi.Toast("No node here");
                return;
            }
            CameraPaths.RemoveNode(path, here);
            CameraUi.Toast($"Node {here + 1} deleted");
        }

        private static void Look(PlayState.Editing editing, CameraPath path)
        {
            if (!editing.Look && path.Nodes.Count < 2)
            {
                CameraUi.Toast("Place 2 nodes to look through the path");
                return;
            }
            editing.Look = !editing.Look;
            CameraUi.Toast(editing.Look ? "Looking through the path: the camera flies it as you scrub" : "Free camera");
        }
    }
}
