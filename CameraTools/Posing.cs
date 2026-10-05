using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // The Pose tab's runtime. Posing is either off or a session on the active character, which Start and End alone begin
    // and finish. A session freezes the character and writes the pose over the frozen game pose every frame. It needs the
    // free camera, so leaving the free camera ends it, and so does switching character. Ending keeps the pose for Bring
    // back last pose. Inside a session, Pose joints closes the panel while the sticks turn one joint at a time.
    internal static class Posing
    {
        // Degrees a second at full stick, on unscaled time so a paused game still poses.
        private const float TurnSpeed = 90f;
        // Hair physics shakes on a posed head, so it runs this many frames for the hair to fall, then holds.
        private const int SettleFrames = 30;
        // A click this far from a marker, as a share of the screen's height, picks its joint: 24 units of the 800 tall canvas.
        private const float PickRadius = 0.03f;

        private static readonly PadBinding PadA = new(PadButtons.A, PadAxis.None);
        private static readonly PadBinding PadB = new(PadButtons.B, PadAxis.None);
        private static readonly PadBinding PadX = new(PadButtons.X, PadAxis.None);
        private static readonly PadBinding PadY = new(PadButtons.Y, PadAxis.None);
        private static readonly PadBinding PadLB = new(PadButtons.LB, PadAxis.None);
        private static readonly PadBinding PadRB = new(PadButtons.RB, PadAxis.None);
        private static readonly PadBinding DpadUp = new(PadButtons.DpadUp, PadAxis.None);
        private static readonly PadBinding DpadDown = new(PadButtons.DpadDown, PadAxis.None);
        private static readonly PadBinding DpadLeft = new(PadButtons.DpadLeft, PadAxis.None);
        private static readonly PadBinding DpadRight = new(PadButtons.DpadRight, PadAxis.None);

        // What the rows show while posing is off; nothing edits it.
        private static readonly PoseSetup Blank = new();
        private static readonly string[] NoExpressions = { "Game's" };

        private static Session session;

        public static bool On => session != null;

        public static bool Editing => session?.Edit != null;

        // The pose has changes since it was last saved, loaded or reset.
        public static bool Changed { get; private set; }

        public static PoseSetup Current => session?.Pose ?? Blank;

        // The Joint row's and Pose joints' joint, and the Finger row's finger.
        public static int Joint { get; private set; } = (int)PoseJoint.LeftUpperArm;

        public static int Finger { get; private set; } = 1;

        public static PoseJoint SelectedJoint => (PoseJoint)Joint;

        public static string[] ExpressionNames => session?.Rig.ExpressionNames ?? NoExpressions;

        public static string[] Emotions => session?.Rig.Emotions ?? Array.Empty<string>();

        // While posing is off nothing is missing, since the rows are dimmed anyway.
        public static bool HasJoint(PoseJoint joint) => session?.Rig.Has(joint) ?? true;

        public static bool HasHand(Side side) => session?.Rig.HasHand(side) ?? true;

        public static bool HasShape(string name) => session?.Rig.HasShape(name) ?? true;

        public static bool HasBrows(string pair) => session?.Rig.HasBrows(pair) ?? true;

        public static bool HasEyes => session?.Rig.HasEyes ?? true;

        // The head waits while it turns toward the camera.
        public static bool CanTurnJoint => On && HasJoint(SelectedJoint) && !(SelectedJoint == PoseJoint.Head && Current.Gaze.HeadAtCamera);

        // Every change to the pose goes through here, so it counts as unsaved.
        public static void Edit(Action<PoseSetup> change)
        {
            if (session == null)
                return;
            change(session.Pose);
            Changed = true;
        }

        public static void SelectJoint(int index) => Joint = Math.Clamp(index, 0, Joints.All.Length - 1);

        public static void SelectFinger(int index) => Finger = Math.Clamp(index, 0, Fingers.Selectable - 1);

        public static void SetOn(bool on)
        {
            if (on == On)
                return;
            if (on && Start())
                CameraUi.Toast("Posing: the character holds still");
            else if (!on)
                End("Posing ended: the character moves again");
        }

        public static void ResetPose()
        {
            if (session == null)
                return;
            session.Pose = new PoseSetup();
            Changed = false;
            Settle(false);
            CameraUi.Toast("The character is back in the game's pose");
        }

        public static void RestoreLast()
        {
            var last = Poses.Last;
            if (last == null || !Start())
                return;
            Poses.Last = null;
            int skipped = Use(last);
            Changed = true;
            CameraUi.Toast(WithSkipped("Last pose brought back", skipped));
        }

        public static void LoadSaved()
        {
            var saved = Poses.Current;
            if (saved == null || !Start())
                return;
            int skipped = Use(saved.Setup);
            Changed = false;
            CameraUi.Toast(WithSkipped($"Loaded {saved.Name}", skipped));
        }

        public static void SaveNew()
        {
            if (session == null)
                return;
            string name = Poses.Add(session.Pose.Clone());
            Changed = false;
            CameraUi.Toast($"Saved as {name}");
        }

        public static void SaveOver()
        {
            if (session == null || Poses.Overwrite(session.Pose.Clone()) is not { } name)
                return;
            Changed = false;
            CameraUi.Toast($"{name} saved");
        }

        public static void DeleteSaved()
        {
            if (Poses.Delete() is { } name)
                CameraUi.Toast($"{name} deleted");
        }

        public static void MirrorJoint()
        {
            var joint = SelectedJoint;
            if (Joints.Of(joint).Mirror is not { } other)
                return;
            Edit(pose => pose.SetTurn(other, pose.Turn(joint)));
            CameraUi.Toast($"Copied to the {Joints.Of(other).Name.ToLowerInvariant()}");
        }

        public static void ResetJoint()
        {
            var joint = SelectedJoint;
            Edit(pose => pose.SetTurn(joint, default));
            CameraUi.Toast($"{Joints.Of(joint).Name} reset");
        }

        public static void SettleHair()
        {
            if (session != null)
                Settle(true);
        }

        public static void StartEditing()
        {
            if (session == null)
                return;
            session.Edit = new JointEdit(session.Pose.Clone(), Changed, Time.frameCount);
            CameraUi.ClosePanel();
            // The mouse picks joints, so the cursor stays free.
            Freecam.Focused = false;
            CameraUi.Toast($"Posing the {Joints.Of(SelectedJoint).Name.ToLowerInvariant()}");
        }

        public static ScreenPoint? JointPoint(int joint, Camera camera) => session?.Rig.Point((PoseJoint)joint, camera);

        public static (string Name, string Angles) Readout
        {
            get
            {
                var turn = Current.Turn(SelectedJoint);
                return (Joints.Of(SelectedJoint).Name, $"Bend {Whole(turn.Bend)}° · Turn {Whole(turn.Turn)}° · Twist {Whole(turn.Twist)}°");
            }
        }

        // After the free camera has moved, so the head and the eyes aim where the camera is this frame.
        public static void Update()
        {
            try
            {
                if (session != null)
                    Tick(session);
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Posing: updating the pose failed: {e}");
            }
            Poses.Update();
        }

        // The game's face system and eye controller write after the late update; a write here comes after theirs and shows.
        public static void OnWillRenderCanvases()
        {
            if (session == null)
                return;
            try
            {
                session.Rig.WriteFace(session.Pose.Face);
                session.Rig.WriteEyes(session.Pose.Gaze);
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Posing: writing the face before the canvases failed: {e}");
            }
        }

        private static void Tick(Session current)
        {
            if (!CameraTools.freecamActive)
            {
                End("Posing ended with the free camera");
                return;
            }
            var avatar = Character.Active();
            if (!current.Rig.Avatar || !avatar || avatar.Pointer != current.Rig.Avatar.Pointer)
            {
                End("Posing ended: the character changed");
                return;
            }
            if (current.Edit != null)
                EditJoints(current);
            if (current.SettleLeft > 0 && --current.SettleLeft == 0)
            {
                current.Rig.SetHair(false);
                if (current.SettleToast)
                    CameraUi.Toast("Hair settled and holds still again");
            }
            var camera = Lights.Camera();
            var view = camera ? camera.transform : null;
            var pose = current.Pose;
            current.Rig.Apply(pose.Face, pose.Gaze, view);
            current.Rig.WriteBody(pose, view);
            current.Rig.WriteFace(pose.Face);
            current.Rig.WriteEyes(pose.Gaze);
        }

        private static bool Start()
        {
            if (session != null)
                return true;
            var avatar = CameraTools.freecamActive ? Character.Active() : null;
            if (!avatar)
            {
                CameraUi.Toast("No character to pose");
                return false;
            }
            try
            {
                session = new Session(new CharacterRig(avatar));
            }
            catch (Exception e)
            {
                Melon<CameraTools>.Logger.Error($"Posing: reading {avatar.name} failed: {e}");
                CameraUi.Toast("Posing failed, see the log");
                return false;
            }
            Changed = false;
            Settle(false);
            return true;
        }

        private static void End(string toast)
        {
            var ending = session;
            if (ending == null)
                return;
            session = null;
            Changed = false;
            Poses.Last = ending.Pose;
            ending.Rig.Restore();
            CameraUi.Toast(toast);
        }

        private static int Use(PoseSetup pose)
        {
            session.Pose = session.Rig.Fit(pose, out int skipped);
            Settle(false);
            return skipped;
        }

        // toast: say when the hair holds again, for the Let hair settle row.
        private static void Settle(bool toast)
        {
            session.SettleLeft = SettleFrames;
            session.SettleToast = toast;
            session.Rig.SetHair(true);
        }

        private static string WithSkipped(string message, int skipped)
            => skipped == 0 ? message : $"{message}, without {skipped} part{(skipped == 1 ? "" : "s")} this character lacks";

        // The pad's buttons are fixed here, like the panel's. The keyboard turns with the free camera's movement keys, which
        // fire nothing else while Pose joints reads them.
        private static void EditJoints(Session current)
        {
            if (Time.frameCount == current.Edit.Frame)
                return;
            var now = Gamepad.Current;
            var before = Gamepad.Previous;
            bool pad = Controls.Owner == PadOwner.CameraTools;
            bool Pressed(PadBinding binding, KeyCode key) => pad && binding.Pressed(now, before) || Input.GetKeyDown(key);
            if (Pressed(PadA, KeyCode.Return) || Input.GetKeyDown(KeyCode.KeypadEnter))
            {
                FinishEditing(current, true);
                return;
            }
            if (Pressed(PadB, KeyCode.Escape))
            {
                FinishEditing(current, false);
                return;
            }
            int count = Joints.All.Length;
            if (Pressed(DpadUp, KeyCode.UpArrow))
                Joint = (Joint + count - 1) % count;
            else if (Pressed(DpadDown, KeyCode.DownArrow))
                Joint = (Joint + 1) % count;
            else if ((Pressed(DpadLeft, KeyCode.LeftArrow) || Pressed(DpadRight, KeyCode.RightArrow)) && Joints.Of(SelectedJoint).Mirror is { } other)
                Joint = (int)other;
            if (Pressed(PadY, KeyCode.Backspace))
                ResetJoint();
            if (Pressed(PadX, KeyCode.M))
                MirrorJoint();
            if (Input.GetMouseButtonDown(0))
                Pick(current);
            if (!CanTurnJoint)
                return;
            float bend = (pad ? now.LeftY : 0f) + Key(CamAction.Forward) - Key(CamAction.Back);
            float turn = (pad ? now.LeftX : 0f) + Key(CamAction.Right) - Key(CamAction.Left);
            float twist = (pad && PadRB.Held(now) ? 1f : 0f) - (pad && PadLB.Held(now) ? 1f : 0f) + Key(CamAction.Up) - Key(CamAction.Down);
            if (bend == 0f && turn == 0f && twist == 0f)
                return;
            float step = TurnSpeed * Time.unscaledDeltaTime;
            var joint = SelectedJoint;
            Edit(pose =>
            {
                var was = pose.Turn(joint);
                pose.SetTurn(joint, new JointTurn(was.Bend + bend * step, was.Turn + turn * step, was.Twist + twist * step));
            });
        }

        private static float Key(CamAction action) => Input.GetKey(Controls.Binding(action).Key) ? 1f : 0f;

        // Keeping leaves the pose as it is; cancelling puts back the pose from before Pose joints.
        private static void FinishEditing(Session current, bool keep)
        {
            var edit = current.Edit;
            current.Edit = null;
            if (!keep)
            {
                current.Pose = edit.Before;
                Changed = edit.Changed;
            }
            CameraUi.OpenPanel();
            CameraUi.Toast(keep ? "Pose kept" : "Pose change cancelled");
        }

        private static void Pick(Session current)
        {
            var camera = Lights.Camera();
            if (!camera)
                return;
            var mouse = Input.mousePosition;
            float width = Screen.width, height = Screen.height;
            float best = PickRadius * height;
            for (int i = 0; i < Joints.All.Length; i++)
            {
                if (current.Rig.Point((PoseJoint)i, camera) is not { } point)
                    continue;
                float dx = point.U * width - mouse.x, dy = (1f - point.V) * height - mouse.y;
                float distance = MathF.Sqrt(dx * dx + dy * dy);
                if (distance >= best)
                    continue;
                best = distance;
                Joint = i;
            }
        }

        private static int Whole(float degrees) => (int)MathF.Round(degrees);

        private sealed class Session
        {
            public readonly CharacterRig Rig;
            public PoseSetup Pose = new();
            public JointEdit Edit;
            // Frames the hair physics still runs before it holds.
            public int SettleLeft;
            public bool SettleToast;

            public Session(CharacterRig rig)
            {
                Rig = rig;
            }
        }

        // Before is the pose Pose joints started from, which cancelling puts back with its unsaved state. Frame is the
        // frame it started on, whose A press must not also finish it.
        private sealed record JointEdit(PoseSetup Before, bool Changed, int Frame);
    }
}
