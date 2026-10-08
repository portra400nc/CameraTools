using MelonLoader;
using UnityEngine;
using VerletEngine;
using NumericsQuaternion = System.Numerics.Quaternion;

namespace CameraTools
{
    // A throwaway test of the hair physics on a posed character, to find the setting that lets the hair hang and collide
    // instead of shaking or holding a shape that cuts into the body. While posing, Back+D-pad right (Numpad 0) moves to the
    // next setting and Back+D-pad left (Numpad .) stops the test, leaving the hair held as before. Each setting logs how far
    // the hair turned between frames (shaking) and since it settled (drifting).
    internal static class HairTest
    {
        private sealed record Mode(string Name, bool Physics, Action<DynamicBoneArray> Set);

        private const int SettleFrames = 30;
        private const int ReportAt = 120;

        private static readonly Mode[] Modes =
        {
            new("Hair held, as build 153", false, _ => { }),
            new("Hair physics as the game sets it", true, _ => { }),
            new("Hair physics, no-animator mode", true, physics => physics.noAnimatorMode = true),
            new("Hair physics, pose captured, no-animator mode", true, physics =>
            {
                physics.ManualUpdateInitTrans();
                physics.noAnimatorMode = true;
            }),
            new("Hair physics, keep initial pose", true, physics => physics.keepInitPose = true),
            new("Hair physics, bones reset every update", true, _ => DynamicBoneArray.bResetBoneInUpdate = true),
        };

        private static readonly PadBinding NextPad = new(PadButtons.Back | PadButtons.DpadRight, PadAxis.None);
        private static readonly PadBinding StopPad = new(PadButtons.Back | PadButtons.DpadLeft, PadAxis.None);

        private static List<(DynamicBoneArray Physics, bool NoAnimator, bool KeepInit, DynamicBoneArray.UpdateMode Mode)> saved;
        private static bool savedReset;
        private static List<Transform> bones;
        private static NumericsQuaternion[] last;
        private static NumericsQuaternion[] settled;
        private static int mode = -1;
        private static int frame;
        private static float shake;
        private static string shakeBone;

        public static bool Active => mode >= 0;

        public static void Update(IReadOnlyList<DynamicBoneArray> hair)
        {
            bool pad = Controls.Owner == PadOwner.CameraTools && CameraUi.View != View.Panel;
            bool keys = !Controls.TextCapture;
            try
            {
                if (keys && Input.GetKeyDown(KeyCode.Keypad0) || pad && NextPad.Pressed(Gamepad.Current, Gamepad.Previous))
                    Next(hair);
                else if (Active && (keys && Input.GetKeyDown(KeyCode.KeypadPeriod) || pad && StopPad.Pressed(Gamepad.Current, Gamepad.Previous)))
                    Stop("stopped");
                if (Active)
                    Measure();
            }
            catch (Exception e)
            {
                Melon<CameraTools>.Logger.Error($"Hair test: setting {mode + 1} failed: {e}");
                CameraUi.Toast("Hair test failed, see the log");
            }
        }

        // Puts every setting back and holds the hair, as posing does without the test.
        public static void Stop(string why)
        {
            if (!Active)
                return;
            Restore(false);
            mode = -1;
            saved = null;
            Melon<CameraTools>.Logger.Msg($"Hair test {why}; the hair is held again.");
            CameraUi.Toast($"Hair test {why}");
        }

        private static void Next(IReadOnlyList<DynamicBoneArray> hair)
        {
            if (!Active)
            {
                saved = hair.Where(each => each).Select(each => (each, each.noAnimatorMode, each.keepInitPose, each.m_UpdateMode)).ToList();
                savedReset = DynamicBoneArray.bResetBoneInUpdate;
                bones = saved.SelectMany(each => Below(each.Physics.m_RootList)).ToList();
            }
            if (mode == Modes.Length - 1)
            {
                Stop("finished");
                return;
            }
            mode++;
            var current = Modes[mode];
            Restore(false);
            foreach (var (physics, _, _, _) in saved)
            {
                if (!physics)
                    continue;
                current.Set(physics);
                if (!current.Physics)
                    continue;
                // Unscaled time, so the hair moves with the game paused too.
                physics.m_UpdateMode = DynamicBoneArray.UpdateMode.UnscaledTime;
                physics.enabled = true;
            }
            frame = 0;
            shake = 0f;
            shakeBone = "none";
            last = Rotations();
            settled = null;
            Melon<CameraTools>.Logger.Msg($"Hair test {mode + 1}/{Modes.Length}: {current.Name} ({bones.Count} hair bones)");
            CameraUi.Toast($"Hair test {mode + 1}/{Modes.Length}: {current.Name}");
        }

        private static void Restore(bool enabled)
        {
            foreach (var (physics, noAnimator, keepInit, updateMode) in saved)
            {
                if (!physics)
                    continue;
                physics.enabled = enabled;
                physics.noAnimatorMode = noAnimator;
                physics.keepInitPose = keepInit;
                physics.m_UpdateMode = updateMode;
            }
            DynamicBoneArray.bResetBoneInUpdate = savedReset;
        }

        // Shaking is the largest turn of any hair bone between two frames once the hair has had time to fall; drifting is
        // how far it has turned since then.
        private static void Measure()
        {
            var now = Rotations();
            frame++;
            if (frame == SettleFrames)
                settled = now;
            if (frame > SettleFrames)
            {
                for (int i = 0; i < now.Length; i++)
                {
                    float turn = Degrees(now[i], last[i]);
                    if (turn > shake)
                        (shake, shakeBone) = (turn, bones[i] ? bones[i].name : "?");
                }
            }
            last = now;
            if (frame != ReportAt)
                return;
            float drift = settled == null ? 0f : now.Select((rotation, i) => Degrees(rotation, settled[i])).DefaultIfEmpty(0f).Max();
            Melon<CameraTools>.Logger.Msg($"Hair test {mode + 1} after {ReportAt} frames: shook up to {shake:0.##}° a frame ({shakeBone}),"
                + $" drifted up to {drift:0.##}° in the last {ReportAt - SettleFrames} frames.");
        }

        private static NumericsQuaternion[] Rotations() => bones.Select(bone => bone ? bone.localRotation.ToNumerics() : NumericsQuaternion.Identity).ToArray();

        private static IEnumerable<Transform> Below(Il2CppInterop.Runtime.InteropTypes.Arrays.Il2CppReferenceArray<Transform> roots)
        {
            if (roots == null)
                yield break;
            var stack = new Stack<Transform>();
            for (int i = 0; i < roots.Length; i++)
                if (roots[i])
                    stack.Push(roots[i]);
            while (stack.Count > 0)
            {
                var node = stack.Pop();
                yield return node;
                for (int i = 0; i < node.childCount; i++)
                    stack.Push(node.GetChild(i));
            }
        }

        private static float Degrees(NumericsQuaternion a, NumericsQuaternion b)
        {
            float dot = Math.Abs(NumericsQuaternion.Dot(a, b));
            return 2f * MathF.Acos(Math.Min(dot, 1f)) * 180f / MathF.PI;
        }
    }
}
