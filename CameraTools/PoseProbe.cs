using System.Text;
using Il2CppInterop.Runtime;
using MelonLoader;
using miHoYoEmotion;
using UnityEngine;
using VerletEngine;
using NumericsQuaternion = System.Numerics.Quaternion;
using NumericsVector3 = System.Numerics.Vector3;

namespace CameraTools
{
    // A throwaway test of character posing, before the Pose tab is designed. Back+D-pad right (Numpad 0) starts it and
    // moves to the next step; Back+D-pad left (Numpad .) ends it and gives the character back to the game. Starting logs
    // the character's bones, face blend shapes, expressions and scripts. Each step then logs what it read back at each
    // point in the frame, so the log and a screenshot per step settle which way of posing holds. Build 147 settled the
    // freeze and the bone writes; this round tests the face, the eyes and the hair.
    internal static class PoseProbe
    {
        private enum Hook { LateUpdate, Canvases, PreCull }

        private sealed record Step(string Name, Action Enter, bool Frozen = true, Action Frame = null);

        private sealed record BoneTarget(Transform Bone, Func<Quaternion> Rotation, Quaternion Original);

        private sealed record ShapeTarget(SkinnedMeshRenderer Renderer, int Index, float Weight, float Original);

        private const int MeasureFrom = 10;
        private const int JitterFrom = 30;
        private const int ReportAt = 60;
        private const int SettleFrames = 30;
        private const float HeadAngle = -40f;
        private const float EyeLimit = 17f;

        private static readonly Step[] Steps =
        {
            new("Left wink, game face on", () => Shape("Eye_WinkA_L", 100f)),
            new("Left wink, blinking off", () =>
            {
                eyeCtrl?.ToggleBlink(false);
                blinkPaused = true;
                Shape("Eye_WinkA_L", 100f);
            }),
            new("Left wink, game face paused", () =>
            {
                PauseFace();
                Shape("Eye_WinkA_L", 100f);
            }),
            new("Left wink and smile, game face paused", () =>
            {
                PauseFace();
                Shape("Eye_WinkA_L", 100f);
                Shape("Mouth_Smile01", 100f);
            }),
            new("Expression Gentle_01, face system switched on", () =>
            {
                emoSync?.Toggle(true, true);
                emoSync?.SetEmotion(Emotion("Gentle_01"), 0.2f);
            }),
            new("Expression HiClosed_01 with phoneme P_Smile_01", () =>
                emoSync?.SetPhonemeAndEmotion(Phoneme("P_Smile_01"), Emotion("HiClosed_01"), 0.2f, true)),
            new("Expression HiClosed_01 with phoneme P_Smile_01, not frozen", () =>
                emoSync?.SetPhonemeAndEmotion(Phoneme("P_Smile_01"), Emotion("HiClosed_01"), 0.2f, true), Frozen: false),
            new("Eyes on the camera, aimed by CameraTools", () =>
            {
                AimEye("+EyeBone L A01");
                AimEye("+EyeBone R A01");
            }),
            new("Eyes on the camera, by the game's eye controller", () => GameLookAt()),
            new("Head turned left, hair physics on", () => TurnHead()),
            new("Head turned left, hair physics paused", () =>
            {
                TurnHead();
                SetHair(false);
            }),
            new("Head turned left, hair settles for half a second, then paused", () => TurnHead(),
                Frame: () =>
                {
                    if (stepFrame == SettleFrames)
                        SetHair(false);
                }),
        };

        private static readonly PadBinding NextPad = new(PadButtons.Back | PadButtons.DpadRight, PadAxis.None);
        private static readonly PadBinding StopPad = new(PadButtons.Back | PadButtons.DpadLeft, PadAxis.None);
        private static readonly Action canvases = OnWillRenderCanvases;
        private static readonly Action<Camera> preCull = OnPreCull;
        private static bool callbacksRegistered;

        private static Transform avatarRoot;
        private static Transform avatar;
        private static readonly Dictionary<string, Transform> bones = new();
        private static readonly List<Transform> allBones = new();
        private static readonly List<(Animator Animator, bool Paused)> animators = new();
        private static readonly List<SkinnedMeshRenderer> faceRenderers = new();
        private static readonly List<DynamicBoneArray> hair = new();
        private static string[] emotions = Array.Empty<string>();
        private static string[] phonemes = Array.Empty<string>();
        private static EmoSync emoSync;
        private static EyeCtrl eyeCtrl;
        private static EyeKey eyeKey;

        private static int step = -1;
        private static int stepFrame;
        private static int lastPreCullFrame;
        private static bool facePaused;
        private static bool blinkPaused;
        private static bool hairPaused;
        private static (Transform ViewTarget, bool Enabled, EyeKey.EyeKeyController Controller)? savedLookAt;
        private static Quaternion[] startRotations;
        private static Quaternion[] lastRotations;
        private static float jitter;
        private static string jitterBone = "none";

        private static readonly List<BoneTarget> boneTargets = new();
        private static readonly List<ShapeTarget> shapeTargets = new();
        private static readonly float[] boneRead = new float[3];
        private static readonly float[] shapeRead = new float[3];
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
            Run("step frame", () => Steps[step].Frame?.Invoke());
            Run("jitter", MeasureJitter);
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
            SetFrozen(current.Frozen);
            Array.Clear(boneRead);
            Array.Clear(shapeRead);
            Array.Clear(reached);
            stepFrame = 0;
            jitter = 0f;
            jitterBone = "none";
            startRotations = allBones.Select(each => each ? each.localRotation : default).ToArray();
            lastRotations = (Quaternion[])startRotations.Clone();
            current.Enter();
            Melon<CameraTools>.Logger.Msg($"Pose test {step + 1}/{Steps.Length}: {current.Name}");
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
            faceRenderers.Clear();
            hair.Clear();
            var log = new StringBuilder();
            log.AppendLine($"Pose test: character {avatar.name}");
            Run("bone tree", () => LogTree(avatar, 0, log));
            Run("animators", () => FindAnimators(log));
            Run("scripts", () => LogScripts(log));
            Run("face", () => FindFace(log));
            Run("expressions", () => FindExpressions(log));
            Run("hair", () => FindHair(log));
            Melon<CameraTools>.Logger.Msg(log.ToString());
            return true;
        }

        private static void Stop(string why)
        {
            Leave();
            foreach (var (animator, paused) in animators)
                if (animator)
                    animator.isAnimationPaused = paused;
            animators.Clear();
            step = -1;
            avatar = null;
            Melon<CameraTools>.Logger.Msg($"Pose test {why}; the character is back to the game.");
            CameraUi.Toast($"Pose test {why}");
        }

        // Puts back what the step changed, so one step cannot leak into the next.
        private static void Leave()
        {
            foreach (var target in boneTargets)
                if (target.Bone)
                    target.Bone.rotation = target.Original;
            boneTargets.Clear();
            foreach (var target in shapeTargets)
                if (target.Renderer)
                    target.Renderer.SetBlendShapeWeight(target.Index, target.Original);
            shapeTargets.Clear();
            if (savedLookAt is { } saved && eyeCtrl)
            {
                eyeCtrl.viewTarget = saved.ViewTarget;
                eyeCtrl.targetEnabled = saved.Enabled;
                eyeCtrl.ClearLookat();
                if (eyeKey)
                    eyeKey.currentController = saved.Controller;
            }
            savedLookAt = null;
            if (step >= 0 && Steps[step].Name.StartsWith("Expression") && emoSync)
                emoSync.SetPhonemeAndEmotion(Phoneme("P_Default_01"), emoSync.defaultEmotion, 0.2f, true);
            if (facePaused)
            {
                emoSync?.Toggle(true, false);
                facePaused = false;
            }
            if (blinkPaused)
            {
                eyeCtrl?.ToggleBlink(true);
                blinkPaused = false;
            }
            if (hairPaused)
                SetHair(true);
        }

        private static void SetFrozen(bool frozen)
        {
            foreach (var (animator, paused) in animators)
                if (animator)
                    animator.isAnimationPaused = frozen || paused;
        }

        private static void PauseFace()
        {
            emoSync?.Toggle(false, false);
            facePaused = true;
            eyeCtrl?.ToggleBlink(false);
            blinkPaused = true;
        }

        private static void Shape(string name, float weight)
        {
            foreach (var renderer in faceRenderers)
            {
                var mesh = renderer.sharedMesh;
                for (int i = 0; i < mesh.blendShapeCount; i++)
                {
                    if (mesh.GetBlendShapeName(i) != name)
                        continue;
                    shapeTargets.Add(new ShapeTarget(renderer, i, weight, renderer.GetBlendShapeWeight(i)));
                    return;
                }
            }
            Melon<CameraTools>.Logger.Warning($"Pose test: blend shape {name} not found");
        }

        private static string Emotion(string wanted) => Pick(emotions, wanted, wanted[..wanted.IndexOf('_')]);

        private static string Phoneme(string wanted) => Pick(phonemes, wanted, wanted[..wanted.LastIndexOf('_')]);

        // The wanted name, else the first of the same family, so the step still runs on a character with other names.
        private static string Pick(string[] names, string wanted, string family)
        {
            string found = names.Contains(wanted) ? wanted : names.FirstOrDefault(name => name.StartsWith(family)) ?? wanted;
            if (found != wanted)
                Melon<CameraTools>.Logger.Msg($"Pose test: {wanted} not found, using {found}");
            return found;
        }

        private static void TurnHead()
        {
            if (!bones.TryGetValue("Bip001 Head", out var head))
                return;
            var frozen = head.rotation;
            var axis = NumericsVector3.Transform(NumericsVector3.UnitY, avatar.rotation.ToNumerics());
            var turn = NumericsQuaternion.CreateFromAxisAngle(axis, HeadAngle * MathF.PI / 180f);
            var target = NumericsQuaternion.Normalize(turn * frozen.ToNumerics()).ToUnity();
            boneTargets.Add(new BoneTarget(head, () => target, frozen));
        }

        // Turns the eye from its frozen rotation by the turn that takes the character's forward onto the camera, up to the
        // game's own horizontal eye range.
        private static void AimEye(string name)
        {
            if (!bones.TryGetValue(name, out var eye))
                return;
            var frozen = eye.rotation;
            boneTargets.Add(new BoneTarget(eye, () =>
            {
                var camera = Camera.main;
                if (!camera)
                    return frozen;
                var rest = NumericsVector3.Transform(NumericsVector3.UnitZ, avatar.rotation.ToNumerics());
                var look = NumericsVector3.Normalize(camera.transform.position.ToNumerics() - eye.position.ToNumerics());
                var axis = NumericsVector3.Cross(rest, look);
                if (axis.LengthSquared() < 1e-8f)
                    return frozen;
                float angle = MathF.Min(MathF.Acos(Math.Clamp(NumericsVector3.Dot(rest, look), -1f, 1f)), EyeLimit * MathF.PI / 180f);
                var turn = NumericsQuaternion.CreateFromAxisAngle(NumericsVector3.Normalize(axis), angle);
                return NumericsQuaternion.Normalize(turn * frozen.ToNumerics()).ToUnity();
            }, frozen));
        }

        private static void GameLookAt()
        {
            var camera = Camera.main;
            if (!eyeCtrl || !camera)
            {
                Melon<CameraTools>.Logger.Warning("Pose test: no eye controller or camera for the look-at step");
                return;
            }
            savedLookAt = (eyeCtrl.viewTarget, eyeCtrl.targetEnabled, eyeKey ? eyeKey.currentController : default);
            if (eyeKey)
                eyeKey.currentController = EyeKey.EyeKeyController.LookAtEyeCtrl;
            eyeCtrl.viewTarget = camera.transform;
            eyeCtrl.targetEnabled = true;
            eyeCtrl.ForceUpdateLookTarget(0.3f);
        }

        private static void SetHair(bool on)
        {
            foreach (var each in hair)
                if (each)
                    each.enabled = on;
            hairPaused = !on;
        }

        // Reads the pose before writing it, so the report shows whether anything rewrote it since the last write.
        private static void Tick(Hook hook)
        {
            if (step < 0)
                return;
            int slot = (int)hook;
            reached[slot] = true;
            if (stepFrame >= MeasureFrom)
            {
                foreach (var target in boneTargets)
                    if (target.Bone)
                        boneRead[slot] = Math.Max(boneRead[slot], Degrees(target.Bone.rotation, target.Rotation()));
                foreach (var target in shapeTargets)
                    if (target.Renderer)
                        shapeRead[slot] = Math.Max(shapeRead[slot], Math.Abs(target.Renderer.GetBlendShapeWeight(target.Index) - target.Weight));
            }
            if (hook == Hook.PreCull)
                return;
            foreach (var target in boneTargets)
                if (target.Bone)
                    target.Bone.rotation = target.Rotation();
            foreach (var target in shapeTargets)
                if (target.Renderer)
                    target.Renderer.SetBlendShapeWeight(target.Index, target.Weight);
        }

        // The largest turn any non-rig bone made between two frames, once the step has settled: hair that shakes in place
        // turns a lot from frame to frame while drifting little overall.
        private static void MeasureJitter()
        {
            for (int i = 0; i < allBones.Count; i++)
            {
                var each = allBones[i];
                if (!each)
                    continue;
                var now = each.localRotation;
                if (stepFrame >= JitterFrom && !each.name.StartsWith("Bip001") && !each.name.StartsWith("+EyeBone"))
                {
                    float moved = Degrees(now, lastRotations[i]);
                    if (moved > jitter)
                        (jitter, jitterBone) = (moved, each.name);
                }
                lastRotations[i] = now;
            }
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
            for (int i = 0; i < 3; i++)
            {
                if (!reached[i])
                    log.Append($" {(Hook)i} never ran;");
                else if (boneTargets.Count > 0 || shapeTargets.Count > 0)
                    log.Append($" {(Hook)i} read bones {boneRead[i]:0.##}° and shapes {shapeRead[i]:0.##} from the target;");
            }
            var (rigBone, rigMoved, otherBone, otherMoved) = Movement();
            log.Append($" rig bones moved up to {rigMoved:0.##}° ({rigBone}), other bones up to {otherMoved:0.##}° ({otherBone}),");
            log.Append($" shook up to {jitter:0.##}° a frame ({jitterBone}). Face now: {FaceWeights()}");
            Melon<CameraTools>.Logger.Msg(log.ToString());
        }

        private static string FaceWeights()
        {
            var set = new List<string>();
            foreach (var renderer in faceRenderers)
            {
                var mesh = renderer.sharedMesh;
                for (int i = 0; i < mesh.blendShapeCount; i++)
                {
                    float weight = renderer.GetBlendShapeWeight(i);
                    if (weight > 0.5f)
                        set.Add($"{mesh.GetBlendShapeName(i)} {weight:0}");
                }
            }
            return set.Count == 0 ? "all shapes at 0" : string.Join(", ", set);
        }

        private static (string, float, string, float) Movement()
        {
            string rigBone = "none", otherBone = "none";
            float rigMoved = 0f, otherMoved = 0f;
            var turned = boneTargets.Select(target => target.Bone?.Pointer ?? IntPtr.Zero).ToHashSet();
            for (int i = 0; i < allBones.Count; i++)
            {
                var each = allBones[i];
                if (!each || turned.Contains(each.Pointer))
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
                animators.Add((animator, animator.isAnimationPaused));
                log.AppendLine($"Animator on {Path(animator.transform)}: human {animator.isHuman}, paused {animator.isAnimationPaused}");
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
            for (int i = 0; i < found.Length; i++)
            {
                var renderer = found[i].TryCast<SkinnedMeshRenderer>();
                var mesh = renderer ? renderer.sharedMesh : null;
                int count = mesh ? mesh.blendShapeCount : 0;
                if (count == 0)
                    continue;
                faceRenderers.Add(renderer);
                log.AppendLine($"Renderer {Path(renderer.transform)}: {string.Join(", ", Enumerable.Range(0, count).Select(mesh.GetBlendShapeName))}");
            }
        }

        private static void FindExpressions(StringBuilder log)
        {
            emoSync = avatar.GetComponentInChildren(Il2CppType.Of<EmoSync>(), true)?.TryCast<EmoSync>();
            eyeCtrl = avatar.GetComponentInChildren(Il2CppType.Of<EyeCtrl>(), true)?.TryCast<EyeCtrl>();
            eyeKey = avatar.GetComponentInChildren(Il2CppType.Of<EyeKey>(), true)?.TryCast<EyeKey>();
            log.AppendLine($"EmoSync {(emoSync ? "found" : "missing")}, EyeCtrl {(eyeCtrl ? "found" : "missing")}, EyeKey {(eyeKey ? $"controller {eyeKey.currentController}" : "missing")}");
            var data = emoSync ? emoSync.setData : null;
            if (!data)
                return;
            emotions = data.emotionSet?.ToArray() ?? Array.Empty<string>();
            phonemes = data.phonemeSet?.ToArray() ?? Array.Empty<string>();
            log.AppendLine($"Emotions ({emotions.Length}): {string.Join(", ", emotions)}");
            log.AppendLine($"Phonemes ({phonemes.Length}): {string.Join(", ", phonemes)}");
        }

        private static void FindHair(StringBuilder log)
        {
            var found = avatar.GetComponentsInChildren(Il2CppType.Of<DynamicBoneArray>(), true);
            for (int i = 0; i < found.Length; i++)
            {
                var each = found[i].TryCast<DynamicBoneArray>();
                if (each)
                    hair.Add(each);
            }
            log.AppendLine($"Hair physics: {string.Join(", ", hair.Select(each => $"{each.name} ({(each.enabled ? "on" : "off")})"))}");
        }

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
