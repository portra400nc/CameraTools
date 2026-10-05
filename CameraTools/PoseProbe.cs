using System.Text;
using System.Text.RegularExpressions;
using Il2CppInterop.Runtime;
using MelonLoader;
using miHoYoEmotion;
using UnityEngine;
using NumericsQuaternion = System.Numerics.Quaternion;
using NumericsVector3 = System.Numerics.Vector3;

namespace CameraTools
{
    // A throwaway test of character posing, before the Pose tab is designed. Back+D-pad right (Numpad 0) starts it and
    // moves to the next step; Back+D-pad left (Numpad .) ends it and gives the character back to the game. Starting logs
    // the character's bones, face blend shapes, expressions and scripts. Each step then logs what it read back at each
    // write point, so the log and a screenshot per step settle which way of posing holds.
    internal static class PoseProbe
    {
        [Flags]
        private enum Hook { None = 0, LateUpdate = 1, Canvases = 2, PreCull = 4 }

        private enum Freeze { Paused, SpeedZero, Both }

        private sealed record Step(string Name, Freeze Freeze, Hook Writes, Action Enter);

        private static readonly Hook[] Hooks = { Hook.LateUpdate, Hook.Canvases, Hook.PreCull };
        private const int MeasureFrom = 10;
        private const int ReportAt = 60;
        private const float ArmAngle = -70f;
        private const float HeadAngle = -40f;

        private static readonly Step[] Steps =
        {
            new("Frozen by isAnimationPaused", Freeze.Paused, Hook.None, () => { }),
            new("Frozen by speed 0", Freeze.SpeedZero, Hook.None, () => { }),
            new("Left arm out, written in late update", Freeze.Both, Hook.LateUpdate, () => TurnBone(LeftUpperArm, Forward, ArmAngle)),
            new("Left arm out, written before canvases", Freeze.Both, Hook.Canvases, () => TurnBone(LeftUpperArm, Forward, ArmAngle)),
            new("Left arm out, written in pre-cull", Freeze.Both, Hook.PreCull, () => TurnBone(LeftUpperArm, Forward, ArmAngle)),
            new("Head turned left, written in late update and before canvases", Freeze.Both, Hook.LateUpdate | Hook.Canvases,
                () => TurnBone(Head, Up, HeadAngle)),
            new("Left eye shape at 100, game face on", Freeze.Both, Hook.LateUpdate | Hook.Canvases, () => SetShape(winkShape)),
            new("Left eye shape at 100, game face paused", Freeze.Both, Hook.LateUpdate | Hook.Canvases, () =>
            {
                PauseFace();
                SetShape(winkShape);
            }),
            new("Game expression", Freeze.Both, Hook.None, () => PlayEmotion()),
            new("Game expression and left eye shape at 100", Freeze.Both, Hook.LateUpdate | Hook.Canvases, () =>
            {
                PlayEmotion();
                SetShape(winkShape);
            }),
            new("Eyes on the camera", Freeze.Both, Hook.None, () => LookAtCamera()),
        };

        private const string LeftUpperArm = "Bip001 L UpperArm";
        private const string Head = "Bip001 Head";
        private static readonly NumericsVector3 Forward = NumericsVector3.UnitZ;
        private static readonly NumericsVector3 Up = NumericsVector3.UnitY;
        private static readonly Regex LeftSide = new(@"(^|[_ .])L($|[_ .\d])|left", RegexOptions.IgnoreCase);

        private static readonly PadBinding NextPad = new(PadButtons.Back | PadButtons.DpadRight, PadAxis.None);
        private static readonly PadBinding StopPad = new(PadButtons.Back | PadButtons.DpadLeft, PadAxis.None);
        private static readonly Action canvases = OnWillRenderCanvases;
        private static readonly Action<Camera> preCull = OnPreCull;
        private static bool callbacksRegistered;

        private static Transform avatarRoot;
        private static Transform avatar;
        private static readonly Dictionary<string, Transform> bones = new();
        private static readonly List<Transform> allBones = new();
        private static readonly List<(Animator Animator, float Speed, bool Paused)> animators = new();
        private static EmoSync emoSync;
        private static EyeCtrl eyeCtrl;
        private static SkinnedMeshRenderer face;
        private static int winkShape = -1;
        private static string emotion;
        private static Transform savedViewTarget;
        private static bool savedTargetEnabled;

        private static int step = -1;
        private static int stepFrame;
        private static int lastPreCullFrame;
        private static bool facePaused;
        private static bool lookingAtCamera;
        private static Quaternion[] startRotations;

        private static Transform bone;
        private static Quaternion boneFrozen;
        private static Quaternion boneTarget;
        private static int shape = -1;
        private static float shapeOriginal;
        private static readonly float[] worstRead = new float[3];
        private static readonly bool[] reached = new bool[3];

        public static void Update()
        {
            bool pad = Controls.Owner == PadOwner.CameraTools && CameraUi.View != View.Panel;
            bool keys = !Controls.TextCapture;
            if (keys && Input.GetKeyDown(KeyCode.Keypad0) || pad && NextPad.Pressed(Gamepad.Current, Gamepad.Previous))
                Run("next", Next);
            else if (step >= 0 && (keys && Input.GetKeyDown(KeyCode.KeypadPeriod) || pad && StopPad.Pressed(Gamepad.Current, Gamepad.Previous)))
                Run("stop", () => Stop("stopped"));
            if (step < 0)
                return;
            if (!avatar || ActiveAvatar()?.Pointer != avatar.Pointer)
            {
                Run("character change", () => Stop("the character changed"));
                return;
            }
            Run("late update", () => Tick(Hook.LateUpdate));
            stepFrame++;
            if (stepFrame == ReportAt)
                Run("report", Report);
        }

        private static void Run(string what, Action action)
        {
            try
            {
                action();
            }
            catch (Exception e)
            {
                Melon<CameraTools>.Logger.Error($"Pose test: {what} failed in step {step + 1}: {e}");
                CameraUi.Toast($"Pose test {step + 1}: {what} failed, see the log");
            }
        }

        private static void Next()
        {
            if (step < 0 && !Start())
                return;
            if (step == Steps.Length - 1)
            {
                Stop("finished");
                return;
            }
            Leave();
            step++;
            var current = Steps[step];
            SetFreeze(current.Freeze);
            current.Enter();
            Array.Clear(worstRead);
            Array.Clear(reached);
            stepFrame = 0;
            startRotations = allBones.Select(each => each ? each.localRotation : default).ToArray();
            string detail = current.Name switch
            {
                _ when current.Name.StartsWith("Game expression") => $" ({emotion ?? "none found"})",
                _ when current.Name.Contains("eye shape") => $" ({ShapeName(winkShape) ?? "none found"})",
                _ => "",
            };
            Melon<CameraTools>.Logger.Msg($"Pose test {step + 1}/{Steps.Length}: {current.Name}{detail}");
            CameraUi.Toast($"Pose test {step + 1}/{Steps.Length}: {current.Name}");
        }

        private static bool Start()
        {
            avatar = ActiveAvatar();
            if (!avatar)
            {
                CameraUi.Toast("Pose test: no character found");
                return false;
            }
            if (!callbacksRegistered)
            {
                Canvas.add_willRenderCanvases(canvases);
                Camera.CameraCallback callback = preCull;
                Camera.onPreCull = Camera.onPreCull == null ? callback : Camera.onPreCull + callback;
                callbacksRegistered = true;
            }
            bones.Clear();
            allBones.Clear();
            animators.Clear();
            var log = new StringBuilder();
            log.AppendLine($"Pose test: character {avatar.name}");
            Run("bone tree", () => LogTree(avatar, 0, log));
            Run("animators", () => FindAnimators(log));
            Run("scripts", () => LogScripts(log));
            Run("face", () => FindFace(log));
            Run("expressions", () => FindExpressions(log));
            Melon<CameraTools>.Logger.Msg(log.ToString());
            return true;
        }

        private static void Stop(string why)
        {
            Leave();
            if (facePaused)
                ResumeFace();
            foreach (var (animator, speed, paused) in animators)
            {
                if (!animator)
                    continue;
                animator.speed = speed;
                animator.isAnimationPaused = paused;
            }
            animators.Clear();
            step = -1;
            avatar = null;
            Melon<CameraTools>.Logger.Msg($"Pose test {why}; the character is back to the game.");
            CameraUi.Toast($"Pose test {why}");
        }

        // Puts back what the step changed, so one step cannot leak into the next.
        private static void Leave()
        {
            if (bone)
                bone.rotation = boneFrozen;
            bone = null;
            if (shape >= 0 && face)
                face.SetBlendShapeWeight(shape, shapeOriginal);
            shape = -1;
            if (lookingAtCamera && eyeCtrl)
            {
                eyeCtrl.viewTarget = savedViewTarget;
                eyeCtrl.targetEnabled = savedTargetEnabled;
                eyeCtrl.ClearLookat();
            }
            lookingAtCamera = false;
            if (emoSync && step >= 0 && Steps[step].Name.StartsWith("Game expression"))
                emoSync.SetEmotion(emoSync.defaultEmotion ?? EmoSync.DEFAULT_EMOTION, 0.2f);
            if (facePaused)
                ResumeFace();
        }

        private static void SetFreeze(Freeze freeze)
        {
            foreach (var (animator, speed, _) in animators)
            {
                if (!animator)
                    continue;
                animator.isAnimationPaused = freeze != Freeze.SpeedZero;
                animator.speed = freeze == Freeze.Paused ? speed : 0f;
            }
        }

        private static void TurnBone(string name, NumericsVector3 localAxis, float degrees)
        {
            if (!bones.TryGetValue(name, out var found))
            {
                Melon<CameraTools>.Logger.Warning($"Pose test: {name} not found");
                return;
            }
            bone = found;
            boneFrozen = found.rotation;
            var axis = NumericsVector3.Transform(localAxis, avatar.rotation.ToNumerics());
            var turn = NumericsQuaternion.CreateFromAxisAngle(axis, degrees * MathF.PI / 180f);
            boneTarget = NumericsQuaternion.Normalize(turn * boneFrozen.ToNumerics()).ToUnity();
        }

        private static void SetShape(int index)
        {
            if (!face || index < 0)
            {
                Melon<CameraTools>.Logger.Warning("Pose test: no left eye shape found");
                return;
            }
            shape = index;
            shapeOriginal = face.GetBlendShapeWeight(index);
        }

        private static void PauseFace()
        {
            emoSync?.Toggle(false, false);
            eyeCtrl?.ToggleBlink(false);
            facePaused = true;
        }

        private static void ResumeFace()
        {
            emoSync?.Toggle(true, false);
            eyeCtrl?.ToggleBlink(true);
            facePaused = false;
        }

        private static void PlayEmotion()
        {
            if (emoSync && emotion != null)
                emoSync.SetEmotion(emotion, 0.2f);
        }

        private static void LookAtCamera()
        {
            var camera = Camera.main;
            if (!eyeCtrl || !camera)
            {
                Melon<CameraTools>.Logger.Warning("Pose test: no eye controller or camera for the look-at step");
                return;
            }
            savedViewTarget = eyeCtrl.viewTarget;
            savedTargetEnabled = eyeCtrl.targetEnabled;
            eyeCtrl.viewTarget = camera.transform;
            eyeCtrl.targetEnabled = true;
            eyeCtrl.ForceUpdateLookTarget(0.3f);
            lookingAtCamera = true;
        }

        // Reads the pose before writing it, so the report shows whether anything rewrote it since the last write.
        private static void Tick(Hook hook)
        {
            if (step < 0)
                return;
            int slot = Array.IndexOf(Hooks, hook);
            reached[slot] = true;
            if (stepFrame >= MeasureFrom)
                worstRead[slot] = Math.Max(worstRead[slot], Distance());
            if ((Steps[step].Writes & hook) == 0)
                return;
            if (bone)
                bone.rotation = boneTarget;
            if (shape >= 0 && face)
                face.SetBlendShapeWeight(shape, 100f);
        }

        private static float Distance()
        {
            if (bone)
                return Degrees(bone.rotation, boneTarget);
            if (shape >= 0 && face)
                return Math.Abs(face.GetBlendShapeWeight(shape) - 100f);
            return 0f;
        }

        private static void OnWillRenderCanvases()
        {
            if (step >= 0)
                Run("canvases", () => Tick(Hook.Canvases));
        }

        private static void OnPreCull(Camera rendering)
        {
            if (step < 0 || lastPreCullFrame == Time.frameCount)
                return;
            lastPreCullFrame = Time.frameCount;
            Run("pre-cull", () => Tick(Hook.PreCull));
        }

        private static void Report()
        {
            var log = new StringBuilder($"Pose test {step + 1} after {ReportAt} frames:");
            if (bone || shape >= 0)
            {
                string unit = bone ? "°" : " weight";
                for (int i = 0; i < Hooks.Length; i++)
                    log.Append(reached[i] ? $" {Hooks[i]} read {worstRead[i]:0.##}{unit} from the target;" : $" {Hooks[i]} never ran;");
            }
            var (rigBone, rigMoved, otherBone, otherMoved) = Movement();
            log.Append($" rig bones moved up to {rigMoved:0.##}° ({rigBone}), other bones up to {otherMoved:0.##}° ({otherBone}).");
            Melon<CameraTools>.Logger.Msg(log.ToString());
        }

        private static (string, float, string, float) Movement()
        {
            string rigBone = "none", otherBone = "none";
            float rigMoved = 0f, otherMoved = 0f;
            for (int i = 0; i < allBones.Count; i++)
            {
                var each = allBones[i];
                if (!each || each == bone)
                    continue;
                float moved = Degrees(each.localRotation, startRotations[i]);
                if (each.name.StartsWith("Bip001"))
                {
                    if (moved > rigMoved)
                        (rigMoved, rigBone) = (moved, each.name);
                }
                else if (moved > otherMoved)
                    (otherMoved, otherBone) = (moved, each.name);
            }
            return (rigBone, rigMoved, otherBone, otherMoved);
        }

        private static float Degrees(Quaternion a, Quaternion b)
        {
            float dot = Math.Abs(NumericsQuaternion.Dot(a.ToNumerics(), b.ToNumerics()));
            return 2f * MathF.Acos(Math.Min(dot, 1f)) * 180f / MathF.PI;
        }

        private static void LogTree(Transform node, int depth, StringBuilder log)
        {
            bones.TryAdd(node.name, node);
            allBones.Add(node);
            log.Append(' ', depth * 2).AppendLine(node.name);
            for (int i = 0; i < node.childCount; i++)
                LogTree(node.GetChild(i), depth + 1, log);
        }

        private static void FindAnimators(StringBuilder log)
        {
            var found = avatar.GetComponentsInChildren(Il2CppType.Of<Animator>(), true);
            for (int i = 0; i < found.Length; i++)
            {
                var animator = found[i].TryCast<Animator>();
                if (!animator)
                    continue;
                animators.Add((animator, animator.speed, animator.isAnimationPaused));
                log.AppendLine($"Animator on {Path(animator.transform)}: human {animator.isHuman}, speed {animator.speed}, " +
                    $"paused {animator.isAnimationPaused}, culling {animator.cullingMode}");
            }
        }

        // Names every script on the character by its IL2CPP class, so the log shows what else writes bones or the face.
        private static void LogScripts(StringBuilder log)
        {
            var found = avatar.GetComponentsInChildren(Il2CppType.Of<MonoBehaviour>(), true);
            var counts = new SortedDictionary<string, int>(StringComparer.Ordinal);
            for (int i = 0; i < found.Length; i++)
            {
                string text = found[i].ToString();
                int open = text.LastIndexOf('(');
                string type = open >= 0 ? text[(open + 1)..].TrimEnd(')') : text;
                counts[type] = counts.GetValueOrDefault(type) + 1;
            }
            log.AppendLine($"Scripts: {string.Join(", ", counts.Select(each => $"{each.Key} x{each.Value}"))}");
        }

        private static void FindFace(StringBuilder log)
        {
            var found = avatar.GetComponentsInChildren(Il2CppType.Of<SkinnedMeshRenderer>(), true);
            int most = 0;
            for (int i = 0; i < found.Length; i++)
            {
                var renderer = found[i].TryCast<SkinnedMeshRenderer>();
                var mesh = renderer ? renderer.sharedMesh : null;
                int count = mesh ? mesh.blendShapeCount : 0;
                log.AppendLine($"Renderer {Path(renderer.transform)}: mesh {(mesh ? mesh.name : "none")}, {count} blend shapes");
                for (int s = 0; s < count; s++)
                    log.AppendLine($"  {s}: {mesh.GetBlendShapeName(s)} = {renderer.GetBlendShapeWeight(s):0.##}");
                if (count > most)
                    (most, face) = (count, renderer);
            }
            winkShape = PickWink();
            log.AppendLine($"Face renderer: {(face ? Path(face.transform) : "none")}; left eye shape: {ShapeName(winkShape) ?? "none"}");
        }

        // The first left wink, else any wink, else any closed or blinking eye shape.
        private static int PickWink()
        {
            if (!face)
                return -1;
            var names = Enumerable.Range(0, face.sharedMesh.blendShapeCount).Select(face.sharedMesh.GetBlendShapeName).ToArray();
            int Find(Func<string, bool> match) => Array.FindIndex(names, name => match(name));
            bool Eye(string name) => name.Contains("eye", StringComparison.OrdinalIgnoreCase);
            int index = Find(name => name.Contains("wink", StringComparison.OrdinalIgnoreCase) && LeftSide.IsMatch(name));
            if (index < 0)
                index = Find(name => name.Contains("wink", StringComparison.OrdinalIgnoreCase));
            if (index < 0)
                index = Find(name => Eye(name) && (name.Contains("close", StringComparison.OrdinalIgnoreCase) ||
                    name.Contains("blink", StringComparison.OrdinalIgnoreCase)));
            return index;
        }

        private static string ShapeName(int index) => face && index >= 0 ? face.sharedMesh.GetBlendShapeName(index) : null;

        private static void FindExpressions(StringBuilder log)
        {
            emoSync = avatar.GetComponentInChildren(Il2CppType.Of<EmoSync>(), true)?.TryCast<EmoSync>();
            eyeCtrl = avatar.GetComponentInChildren(Il2CppType.Of<EyeCtrl>(), true)?.TryCast<EyeCtrl>();
            var eyeKey = avatar.GetComponentInChildren(Il2CppType.Of<EyeKey>(), true)?.TryCast<EyeKey>();
            log.AppendLine($"EmoSync: {(emoSync ? Path(emoSync.transform) : "none")}; EyeCtrl: {(eyeCtrl ? Path(eyeCtrl.transform) : "none")}; " +
                $"EyeKey: {(eyeKey ? Path(eyeKey.transform) : "none")}");
            if (eyeCtrl)
                log.AppendLine($"EyeCtrl: auto-blink {eyeCtrl.autoBlinkingEnabled}, target {eyeCtrl.targetEnabled}, view target " +
                    $"{(eyeCtrl.viewTarget ? eyeCtrl.viewTarget.name : "none")}, head {(eyeCtrl.headTransform ? eyeCtrl.headTransform.name : "none")}, " +
                    $"range x {eyeCtrl.eyeRotationRangeX.x}..{eyeCtrl.eyeRotationRangeX.y}, y {eyeCtrl.eyeRotationRangeY.x}..{eyeCtrl.eyeRotationRangeY.y}");
            if (eyeKey)
                log.AppendLine($"EyeKey: controller {eyeKey.currentController}, eyes {Name(eyeKey.leftEyeBone)}/{Name(eyeKey.rightEyeBone)}, " +
                    $"balls {Name(eyeKey.leftEyeBallBone)}/{Name(eyeKey.rightEyeBallBone)}, teeth {Name(eyeKey.teethUpBone)}/{Name(eyeKey.teethDownBone)}");
            if (!emoSync)
                return;
            var data = emoSync.setData;
            log.AppendLine($"EmoSync: default emotion {emoSync.defaultEmotion}, set data {(data ? data.name : "none")}");
            if (!data)
                return;
            var emotions = data.emotionSet?.ToArray() ?? Array.Empty<string>();
            log.AppendLine($"Emotions ({emotions.Length}): {string.Join(", ", emotions)}");
            log.AppendLine($"Phonemes ({data.phonemeSet?.Length ?? 0}): {string.Join(", ", data.phonemeSet?.ToArray() ?? Array.Empty<string>())}");
            log.AppendLine($"Eye controls ({data.eyeCtrlSet?.Length ?? 0}): {string.Join(", ", data.eyeCtrlSet?.ToArray() ?? Array.Empty<string>())}");
            emotion = emotions.FirstOrDefault(name => name.Contains("smile", StringComparison.OrdinalIgnoreCase))
                ?? emotions.FirstOrDefault(name => !name.StartsWith(EmoSync.DEFAULT_EMOTION, StringComparison.OrdinalIgnoreCase));
            log.AppendLine($"Expression for the test: {emotion ?? "none"}");
        }

        private static string Name(Transform transform) => transform ? transform.name : "none";

        private static string Path(Transform transform)
        {
            var parts = new List<string>();
            for (var node = transform; node && node != avatar.parent; node = node.parent)
                parts.Add(node.name);
            parts.Reverse();
            return string.Join("/", parts);
        }

        // The active character is the one active child of the avatar root.
        private static Transform ActiveAvatar()
        {
            if (!avatarRoot)
                avatarRoot = GameObject.Find("/EntityRoot/AvatarRoot")?.transform;
            if (!avatarRoot)
                return null;
            for (int i = 0; i < avatarRoot.childCount; i++)
            {
                var child = avatarRoot.GetChild(i);
                if (child.gameObject.activeInHierarchy)
                    return child;
            }
            return null;
        }
    }
}
