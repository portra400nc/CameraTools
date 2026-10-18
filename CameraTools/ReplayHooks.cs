using System.Runtime.InteropServices;
using Il2CppInterop.Runtime;
using MelonLoader;
using MelonLoader.NativeUtils;
using UnityEngine;

namespace CameraTools
{
    // A call the game made on one material slot of a recorded object, or, for the renderer kinds and MeshSwap, on one of
    // its renderers.
    internal sealed class LookCall
    {
        public float Time;
        public int Slot;
        public CallKind Kind;
        public int Id;
        public int Int;
        public Vector4 Value;
        public string Text;
        public Shader Shader;
        public Material Snapshot;
        public Texture Texture;
        public Mesh Mesh;
    }

    // Native hooks on the engine bindings behind Material's setters and Renderer.SetCustomPropertyFloat. The managed
    // setters are virtual and reach the engine through these, so a direct-call search finds no callers, and reading
    // properties back needs their names, which the engine cannot list. While a recording runs, the calls aimed at the
    // materials and renderers recorded the frame before are buffered for the recorder.
    internal static class ReplayHooks
    {
        public struct Raw
        {
            public IntPtr Self;
            public CallKind Kind;
            public int Id;
            public int Int;
            public Vector4 Value;
            public string Text;
            public IntPtr Ref;
        }

        // Genshin's renderer settings that have a setter and no getter. Instantiate does not carry what the game set at
        // runtime, and nothing can read it back, so the hooks keep each renderer's last value.
        public readonly record struct Setter(string Name, int Rva, string Bytes, bool Float);

        public static readonly Setter[] Setters =
        {
            new("set_useHIZAABBScale", 0x144B6D0, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_perObjectHIZBoundScale", 0x144B710, "40534883ec300f297424200f28f1488b", true),
            new("set_skipGPUCulling", 0x144B780, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_enableBlockFlare", 0x144B7D0, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_useSpecialOrderWhenInSortingGroup", 0x144B820, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_kodamaShadowVisibilityMask", 0x144B870, "48895c2408574883ec208bfa488bd948", false),
            new("set_forceDisableEarlyZ", 0x144B8C0, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_inWaterShadowCullingOption", 0x144B940, "48895c2408574883ec208bfa488bd948", false),
            new("set_streamingUseHighestMip", 0x144C000, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_receiveDecals", 0x144CA70, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_enableDither", 0x144CB30, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_matLayer", 0x144CBB0, "48895c2408574883ec208bfa488bd948", false),
            new("set_cullingBoundGroupID", 0x144CC20, "48895c2408574883ec208bfa488bd948", false),
            new("set_customeRenderingObjectID", 0x144CC70, "48895c2408574883ec208bfa488bd948", false),
            new("set_perObjectShadowGroupID", 0x144CCF0, "48895c2408574883ec208bfa488bd948", false),
            new("set_characterGroupID", 0x144CD40, "48895c2408574883ec208bfa488bd948", false),
            new("set_characterGroupRendererType", 0x144CDB0, "48895c2408574883ec208bfa488bd948", false),
            new("set_allowHalfResolution", 0x144CE30, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_enableDragonizeOutline", 0x144CE80, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_enableBrowStencilDraw", 0x144CED0, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_viewDistanceRatio", 0x144CF50, "40534883ec300f297424200f28f1488b", true),
            new("set_enableReflectionAndRefraction", 0x144CF90, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_disableShadowRendering", 0x144CFE0, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_reflectionAndRefractionMode", 0x144D030, "48895c2408574883ec208bda488bf948", false),
            new("set_enableRenderToReflectionAndRefractionTexture", 0x144D080, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_renderDecalAfterLocalLightMobile", 0x144D0D0, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_enablePerMaterialCustomPropBatching", 0x144D120, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_useScaledThreshForWaterTextures", 0x144D170, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_enableRenderingObjectMask", 0x144D1C0, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_renderingObjectMaskMode", 0x144D220, "48895c2408574883ec208bfa488bd948", false),
            new("set_renderingObjectBehindOccMode", 0x144D280, "48895c2408574883ec208bda488bf948", false),
            new("set_disableOutputDepthWithSSR", 0x144D2F0, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_castHeightfieldDeformation", 0x144D340, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_receiveHeightfieldDeformation", 0x144D390, "48895c2408574883ec200fb6fa488bd9", false),
            new("set_perObjectShadowZExtendScale", 0x144D3E0, "40534883ec400f297424300f28f1488b", true),
        };

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void IntSetter(IntPtr self, int value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void FloatSetter(IntPtr self, float value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void EnableCall(IntPtr renderers, byte enable);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void EnableArrayCall(IntPtr renderers, IntPtr enables);

        private static readonly NativeHook<IntSetter>[] intSetters = new NativeHook<IntSetter>[Setters.Length];
        private static readonly NativeHook<FloatSetter>[] floatSetters = new NativeHook<FloatSetter>[Setters.Length];
        private static NativeHook<EnableCall> enableProperty;
        private static NativeHook<EnableArrayCall> enablePropertyArray;

        // Each renderer's last value per setter, and its last enable property, from every call since the hooks attached.
        public static readonly Dictionary<IntPtr, Dictionary<int, (int Int, float Float)>> Settings = new();
        public static readonly Dictionary<IntPtr, bool> Enabled = new();
        public static readonly Dictionary<IntPtr, Dictionary<int, float>> CustomFloats = new();

        // The setters may in principle run off the main thread, so the shared tables and buffer take a lock.
        private static readonly object gate = new();

        private static void Remember(IntPtr renderer, int setter, int i, float f)
        {
            lock (gate)
            {
                if (!Settings.TryGetValue(renderer, out var values))
                    Settings[renderer] = values = new Dictionary<int, (int, float)>();
                values[setter] = (i, f);
            }
        }

        // Calls a setter's original engine code on another renderer, such as a copy.
        public static void Apply(int setter, IntPtr renderer, int i, float f)
        {
            if (Setters[setter].Float)
                floatSetters[setter]?.Trampoline(renderer, f);
            else
                intSetters[setter]?.Trampoline(renderer, i);
        }

        // What the game gave a renderer, as far as the hooks saw: its settings, custom floats and enable property.
        public sealed record Given(Dictionary<int, (int Int, float Float)> Values, Dictionary<int, float> Floats, bool? Enabled);

        public static Given Remembered(Renderer from)
        {
            lock (gate)
                return new Given(Settings.TryGetValue(from.Pointer, out var values) ? new Dictionary<int, (int, float)>(values) : null,
                    CustomFloats.TryGetValue(from.Pointer, out var floats) ? new Dictionary<int, float>(floats) : null,
                    Enabled.TryGetValue(from.Pointer, out bool on) ? on : null);
        }

        // Gives another renderer, such as a copy, what the game gave the original.
        public static void Give(Given given, Renderer to)
        {
            if (given.Values != null)
                foreach (var (setter, (i, f)) in given.Values)
                    Apply(setter, to.Pointer, i, f);
            if (given.Floats != null)
                foreach (var (id, value) in given.Floats)
                    to.SetCustomPropertyFloat(id, value, true);
            if (given.Enabled is bool on)
                SetEnableProperty(to, on);
        }

        public static void SetEnableProperty(Renderer renderer, bool on)
        {
            var array = new Il2CppInterop.Runtime.InteropTypes.Arrays.Il2CppReferenceArray<Renderer>(1);
            array[0] = renderer;
            if (enableProperty != null)
                enableProperty.Trampoline(array.Pointer, on ? (byte)1 : (byte)0);
            else
                Renderer.SetRenderersEnableProperty(array, on);
        }

        // An IL2CPP array's length sits at +0x18 and its elements from +0x20.
        private static IEnumerable<IntPtr> Elements(IntPtr array)
        {
            if (array == IntPtr.Zero)
                yield break;
            long length = Marshal.ReadInt64(array, 0x18);
            for (int i = 0; i < length && i < 4096; i++)
                yield return Marshal.ReadIntPtr(array, 0x20 + i * IntPtr.Size);
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void FloatCall(IntPtr self, int id, float value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void IntCall(IntPtr self, int id, int value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void PointerCall(IntPtr self, int id, IntPtr value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void ObjectCall(IntPtr self, IntPtr value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void KeywordIdCall(IntPtr self, int id, byte didModify);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void CustomCall(IntPtr self, int id, float value, byte updateHash);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr GetModuleHandleW(string name);

        private const string MainCameraPath = "/EntityRoot/MainCamera(Clone)";
        private const int BufferLimit = 50000;
        private static readonly List<Raw> buffer = new();
        private static readonly List<object> keep = new();
        private static float nextLook;
        public static volatile bool Recording;
        public static bool Attached { get; private set; }

        private static NativeHook<FloatCall> setFloat;
        private static NativeHook<IntCall> setInt;
        private static NativeHook<PointerCall> setColor, setVector, setTexture;
        private static NativeHook<ObjectCall> setShader, enableKeyword, disableKeyword;
        private static NativeHook<KeywordIdCall> enableKeywordId, disableKeywordId;
        private static NativeHook<CustomCall> setCustom;

        public static List<Raw> Drain()
        {
            lock (gate)
            {
                var drained = new List<Raw>(buffer);
                buffer.Clear();
                return drained;
            }
        }

        // The pointers of the recorded objects' materials and renderers, set by the recorder each frame. Keeping every
        // call in the open world, thousands a frame, cost about 24 ms a frame to sort.
        public static volatile HashSet<IntPtr> Watched;

        private static void Add(Raw raw)
        {
            if (!Recording || Watched is not { } watched || !watched.Contains(raw.Self))
                return;
            lock (gate)
                if (buffer.Count < BufferLimit)
                    buffer.Add(raw);
        }

        // Attached from the first frame the game camera exists, so the settings the game gives renderers when it makes
        // them, before any recording, are already remembered.
        public static void Update()
        {
            if (Attached || Time.unscaledTime < nextLook)
                return;
            nextLook = Time.unscaledTime + 1f;
            if (GameObject.Find(MainCameraPath))
                Attach();
        }

        private static void Attach()
        {
            Attached = true;
            IntPtr module = GetModuleHandleW("GenshinImpact.exe");
            var report = new List<string>();
            var settable = new List<(string, object)>();
            for (int k = 0; k < Setters.Length; k++)
            {
                int index = k;
                var setter = Setters[k];
                if (setter.Float)
                    settable.Add((setter.Name, floatSetters[k] = Hook<FloatSetter>(module, setter.Rva, setter.Bytes, setter.Name, (self, value) =>
                    {
                        floatSetters[index].Trampoline(self, value);
                        Remember(self, index, 0, value);
                        Add(new Raw { Self = self, Kind = CallKind.RendererSet, Id = index, Value = new Vector4(value, 0, 0, 0) });
                    }, report)));
                else
                    settable.Add((setter.Name, intSetters[k] = Hook<IntSetter>(module, setter.Rva, setter.Bytes, setter.Name, (self, value) =>
                    {
                        intSetters[index].Trampoline(self, value);
                        Remember(self, index, value, 0);
                        Add(new Raw { Self = self, Kind = CallKind.RendererSet, Id = index, Int = value });
                    }, report)));
            }
            enableProperty = Hook<EnableCall>(module, 0x144C920, "48895c241048896c2418488974242057", "SetRenderersEnableProperty", (renderers, enable) =>
            {
                enableProperty.Trampoline(renderers, enable);
                foreach (var renderer in Elements(renderers))
                {
                    lock (gate)
                        Enabled[renderer] = enable != 0;
                    Add(new Raw { Self = renderer, Kind = CallKind.EnableProperty, Int = enable });
                }
            }, report);
            enablePropertyArray = Hook<EnableArrayCall>(module, 0x144C9A0, "48896c24184889742420574883ec2048", "SetRenderersEnablePropertyArray", (renderers, enables) =>
            {
                enablePropertyArray.Trampoline(renderers, enables);
                int i = 0;
                foreach (var renderer in Elements(renderers))
                {
                    bool on = enables != IntPtr.Zero && i < Marshal.ReadInt64(enables, 0x18) && Marshal.ReadByte(enables, 0x20 + i) != 0;
                    lock (gate)
                        Enabled[renderer] = on;
                    Add(new Raw { Self = renderer, Kind = CallKind.EnableProperty, Int = on ? 1 : 0 });
                    i++;
                }
            }, report);
            settable.Add(("SetRenderersEnableProperty", enableProperty));
            settable.Add(("SetRenderersEnablePropertyArray", enablePropertyArray));
            setFloat = Hook<FloatCall>(module, 0x143CA50, "48895c2408574883ec300f297424200f", "SetFloat", (self, id, value) =>
            {
                setFloat.Trampoline(self, id, value);
                Add(new Raw { Self = self, Kind = CallKind.Float, Id = id, Value = new Vector4(value, 0, 0, 0) });
            }, report);
            setInt = Hook<IntCall>(module, 0x143CAA0, "48895c24084889742410574883ec2041", "SetInt", (self, id, value) =>
            {
                setInt.Trampoline(self, id, value);
                Add(new Raw { Self = self, Kind = CallKind.Int, Id = id, Value = new Vector4(value, 0, 0, 0) });
            }, report);
            setColor = Hook<PointerCall>(module, 0x143CAF0, "48895c24084889742410574883ec2049", "SetColor", (self, id, value) =>
            {
                setColor.Trampoline(self, id, value);
                Add(new Raw { Self = self, Kind = CallKind.Color, Id = id, Value = Marshal.PtrToStructure<Vector4>(value) });
            }, report);
            setVector = Hook<PointerCall>(module, 0x143CB40, "48895c2408574883ec308bfa488bd941", "SetVector", (self, id, value) =>
            {
                setVector.Trampoline(self, id, value);
                Add(new Raw { Self = self, Kind = CallKind.Vector, Id = id, Value = Marshal.PtrToStructure<Vector4>(value) });
            }, report);
            setTexture = Hook<PointerCall>(module, 0x143CBE0, "48895c24084889742410574883ec208b", "SetTexture", (self, id, value) =>
            {
                setTexture.Trampoline(self, id, value);
                Add(new Raw { Self = self, Kind = CallKind.Texture, Id = id, Ref = value });
            }, report);
            setShader = Hook<ObjectCall>(module, 0x143C9F0, "48895c2408574883ec20488bd94885d2", "set_shader", (self, value) =>
            {
                setShader.Trampoline(self, value);
                Add(new Raw { Self = self, Kind = CallKind.Shader, Ref = value });
            }, report);
            enableKeyword = Hook<ObjectCall>(module, 0x143E520, "40534883ec50488bd94889542460488d", "EnableKeyword", (self, value) =>
            {
                enableKeyword.Trampoline(self, value);
                if (Recording)
                    Add(new Raw { Self = self, Kind = CallKind.KeywordOn, Text = IL2CPP.Il2CppStringToManaged(value) });
            }, report);
            disableKeyword = Hook<ObjectCall>(module, 0x143E590, "40534883ec50488bd94889542460488d", "DisableKeyword", (self, value) =>
            {
                disableKeyword.Trampoline(self, value);
                if (Recording)
                    Add(new Raw { Self = self, Kind = CallKind.KeywordOff, Text = IL2CPP.Il2CppStringToManaged(value) });
            }, report);
            enableKeywordId = Hook<KeywordIdCall>(module, 0x143E680, "48895c24084889742410574883ec2041", "EnableKeywordID", (self, id, didModify) =>
            {
                enableKeywordId.Trampoline(self, id, didModify);
                Add(new Raw { Self = self, Kind = CallKind.KeywordIdOn, Id = id });
            }, report);
            disableKeywordId = Hook<KeywordIdCall>(module, 0x143E6D0, "48895c24084889742410574883ec2041", "DisableKeywordID", (self, id, didModify) =>
            {
                disableKeywordId.Trampoline(self, id, didModify);
                Add(new Raw { Self = self, Kind = CallKind.KeywordIdOff, Id = id });
            }, report);
            setCustom = Hook<CustomCall>(module, 0x144C460, "48895c24084889742410574883ec300f", "Renderer.SetCustomPropertyFloat", (self, id, value, updateHash) =>
            {
                setCustom.Trampoline(self, id, value, updateHash);
                lock (gate)
                {
                    if (!CustomFloats.TryGetValue(self, out var floats))
                        CustomFloats[self] = floats = new Dictionary<int, float>();
                    floats[id] = value;
                }
                Add(new Raw { Self = self, Kind = CallKind.CustomFloat, Id = id, Value = new Vector4(value, 0, 0, 0) });
            }, report);
            // Attached only once every field holds its hook, so no detour runs before its trampoline is reachable.
            int attached = 0;
            foreach (var (name, hook) in new (string, object)[]
            {
                ("SetFloat", setFloat), ("SetInt", setInt), ("SetColor", setColor), ("SetVector", setVector), ("SetTexture", setTexture), ("set_shader", setShader),
                ("EnableKeyword", enableKeyword), ("DisableKeyword", disableKeyword), ("EnableKeywordID", enableKeywordId),
                ("DisableKeywordID", disableKeywordId), ("Renderer.SetCustomPropertyFloat", setCustom),
            }.Concat(settable))
            {
                if (hook == null)
                    continue;
                try
                {
                    hook.GetType().GetMethod("Attach", Type.EmptyTypes)!.Invoke(hook, null);
                    attached++;
                }
                catch (Exception e)
                {
                    report.Add($"{name} failed to attach ({e.InnerException?.Message ?? e.Message})");
                }
            }
            Melon<CameraTools>.Logger.Msg($"Replays: {attached} engine hooks attached{(report.Count > 0 ? $"; {string.Join(", ", report)}" : "")}.");
        }

        // Hooks one engine binding after checking its first bytes against the shipped game, so a different build is
        // left alone instead of patched at the wrong place.
        private static NativeHook<T> Hook<T>(IntPtr module, int rva, string expected, string name, T detour, List<string> report) where T : Delegate
        {
            try
            {
                IntPtr target = module + rva;
                var bytes = new byte[expected.Length / 2];
                Marshal.Copy(target, bytes, 0, bytes.Length);
                string found = Convert.ToHexString(bytes).ToLowerInvariant();
                if (found != expected)
                {
                    report.Add($"{name} skipped (bytes {found})");
                    return null;
                }
                keep.Add(detour);
                return new NativeHook<T>(target, Marshal.GetFunctionPointerForDelegate(detour));
            }
            catch (Exception e)
            {
                report.Add($"{name} failed ({e.Message})");
                return null;
            }
        }
    }
}
