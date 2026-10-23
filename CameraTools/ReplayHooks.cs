using System.Runtime.InteropServices;
using Il2CppInterop.Runtime;
using Il2CppInterop.Runtime.InteropTypes.Arrays;
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
        public LineMesh Line;
    }

    // A mesh the game gave a line renderer through Genshin's LineRenderer.SetCustomMeshDataWithSize, as raw element bytes:
    // Neuvillette's charged attack beam is such a mesh, built by the game's code, on line renderers whose own points are a
    // stub at the world origin.
    internal sealed class LineMesh
    {
        public byte[] Triangles, Positions, Normals, Uv0, Colors, Uv1;
        public int TriangleCount, PositionCount, NormalCount, Uv0Count, ColorCount, Uv1Count;
        private Il2CppStructArray<ushort> triangles;
        private Il2CppStructArray<Vector3> positions, normals;
        private Il2CppStructArray<Vector2> uv0, uv1;
        private Il2CppStructArray<Color32> colors;

        public static LineMesh Read(IntPtr triangles, int triangleCount, IntPtr positions, int positionCount, IntPtr normals, int normalCount,
            IntPtr uv0, int uv0Count, IntPtr colors, int colorCount, IntPtr uv1, int uv1Count) => new()
        {
            Triangles = Bytes(triangles, triangleCount, 2), TriangleCount = triangleCount,
            Positions = Bytes(positions, positionCount, 12), PositionCount = positionCount,
            Normals = Bytes(normals, normalCount, 12), NormalCount = normalCount,
            Uv0 = Bytes(uv0, uv0Count, 8), Uv0Count = uv0Count,
            Colors = Bytes(colors, colorCount, 4), ColorCount = colorCount,
            Uv1 = Bytes(uv1, uv1Count, 8), Uv1Count = uv1Count,
        };

        // An IL2CPP array's length sits at +0x18 and its elements from +0x20; count can be less than the length.
        private static byte[] Bytes(IntPtr array, int count, int size)
        {
            if (array == IntPtr.Zero || count <= 0)
                return null;
            count = (int)Math.Min(count, Marshal.ReadInt64(array, 0x18));
            var bytes = new byte[count * size];
            Marshal.Copy(array + 0x20, bytes, 0, bytes.Length);
            return bytes;
        }

        private static Il2CppStructArray<T> Array<T>(byte[] bytes, int size) where T : unmanaged
        {
            if (bytes == null)
                return null;
            var array = new Il2CppStructArray<T>(bytes.Length / size);
            Marshal.Copy(bytes, 0, array.Pointer + 0x20, bytes.Length);
            return array;
        }

        public void Give(Renderer to)
        {
            if (to.TryCast<LineRenderer>() is not { } line)
                return;
            triangles ??= Array<ushort>(Triangles, 2);
            positions ??= Array<Vector3>(Positions, 12);
            normals ??= Array<Vector3>(Normals, 12);
            uv0 ??= Array<Vector2>(Uv0, 8);
            colors ??= Array<Color32>(Colors, 4);
            uv1 ??= Array<Vector2>(Uv1, 8);
            line.SetCustomMeshDataWithSize(triangles, triangles == null ? 0 : TriangleCount, positions, positions == null ? 0 : PositionCount,
                normals, normals == null ? 0 : NormalCount, uv0, uv0 == null ? 0 : Uv0Count, colors, colors == null ? 0 : ColorCount,
                uv1, uv1 == null ? 0 : Uv1Count);
        }
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
            public LineMesh Line;
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
            // Genshin's LineRenderer: whether it draws a mesh the game gives it instead of its points, and how.
            new("LineRenderer.set_useCustomMeshData", 0x144DA00, "48895c2408574883ec200fb6fa488bd9", false),
            new("LineRenderer.set_customUVMode", 0x144DA40, "48895c2408574883ec208bfa488bd948", false),
            new("LineRenderer.set_customAddSegmentNum", 0x144DA80, "48895c2408574883ec208bfa488bd948", false),
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
        // Each renderer's last custom vector, colour, texture and per-material value, by kind, material (-1 for the whole
        // renderer) and property: the game gives characters some of these once, as it makes them, and a copy without
        // them drew the game's snow cover on Neuvillette's hair.
        public static readonly Dictionary<IntPtr, Dictionary<(CallKind Kind, int Material, int Id), (Vector4 Value, Texture Texture)>> CustomProps = new();
        // Each line renderer's last custom mesh.
        public static readonly Dictionary<IntPtr, LineMesh> LineMeshes = new();

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
        public sealed record Given(Dictionary<int, (int Int, float Float)> Values, Dictionary<int, float> Floats, bool? Enabled,
            Dictionary<(CallKind Kind, int Material, int Id), (Vector4 Value, Texture Texture)> Props = null, LineMesh Line = null);

        public static Given Remembered(Renderer from)
        {
            lock (gate)
                return new Given(Settings.TryGetValue(from.Pointer, out var values) ? new Dictionary<int, (int, float)>(values) : null,
                    CustomFloats.TryGetValue(from.Pointer, out var floats) ? new Dictionary<int, float>(floats) : null,
                    Enabled.TryGetValue(from.Pointer, out bool on) ? on : null,
                    CustomProps.TryGetValue(from.Pointer, out var props) ? new Dictionary<(CallKind, int, int), (Vector4, Texture)>(props) : null,
                    LineMeshes.TryGetValue(from.Pointer, out var line) ? line : null);
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
            if (given.Props != null)
                foreach (var ((kind, material, id), (value, texture)) in given.Props)
                    SetCustom(to, kind, material, id, value, texture);
            given.Line?.Give(to);
        }

        // Sets one custom property on a renderer, the whole renderer's for material -1, else that material's.
        public static void SetCustom(Renderer to, CallKind kind, int material, int id, Vector4 value, Texture texture)
        {
            var color = new Color(value.x, value.y, value.z, value.w);
            switch (kind)
            {
                case CallKind.CustomFloat when material < 0:
                    to.SetCustomPropertyFloat(id, value.x, true);
                    break;
                case CallKind.CustomFloat:
                    to.SetPerMaterialCustomPropertyFloat(material, id, value.x, true);
                    break;
                case CallKind.CustomVector when material < 0:
                    to.SetCustomPropertyVector(id, value, true);
                    break;
                case CallKind.CustomVector:
                    to.SetPerMaterialCustomPropertyVector(material, id, value, true);
                    break;
                case CallKind.CustomColor when material < 0:
                    to.SetCustomPropertyColor(id, color, true);
                    break;
                case CallKind.CustomColor:
                    to.SetPerMaterialCustomPropertyColor(material, id, color, true);
                    break;
                case CallKind.CustomTexture when material < 0:
                    to.SetCustomPropertyTexture(id, texture, true);
                    break;
                case CallKind.CustomTexture:
                    to.SetPerMaterialCustomPropertyTexture(material, id, texture, true);
                    break;
            }
        }

        // A texture is kept as its object, which holds it alive for the copies given it later.
        private static void RememberCustom(IntPtr self, CallKind kind, int material, int id, Vector4 value, IntPtr texture)
        {
            var kept = texture != IntPtr.Zero ? new Texture(texture) : null;
            lock (gate)
            {
                if (!CustomProps.TryGetValue(self, out var props))
                    CustomProps[self] = props = new Dictionary<(CallKind, int, int), (Vector4, Texture)>();
                props[(kind, material, id)] = (value, kept);
            }
            Add(new Raw { Self = self, Kind = kind, Id = id, Int = material, Value = value, Ref = texture });
        }

        private static Vector4 ReadVector2(IntPtr at)
        {
            if (at == IntPtr.Zero)
                return default;
            var floats = new float[2];
            Marshal.Copy(at, floats, 0, 2);
            return new Vector4(floats[0], floats[1], 0, 0);
        }

        private static Vector4 ReadVector(IntPtr at)
        {
            if (at == IntPtr.Zero)
                return default;
            var floats = new float[4];
            Marshal.Copy(at, floats, 0, 4);
            return new Vector4(floats[0], floats[1], floats[2], floats[3]);
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
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void LineMeshCall(IntPtr self, IntPtr triangles, int triangleCount, IntPtr positions,
            int positionCount, IntPtr normals, int normalCount, IntPtr uv0, int uv0Count, IntPtr colors, int colorCount, IntPtr uv1, int uv1Count);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void CustomPointerCall(IntPtr self, int id, IntPtr value, byte updateHash);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void MaterialFloatCall(IntPtr self, int material, int id, float value, byte updateHash);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void MaterialPointerCall(IntPtr self, int material, int id, IntPtr value, byte updateHash);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr GetModuleHandleW(string name);

        private const int BufferLimit = 50000;
        private static readonly List<Raw> buffer = new();
        private static readonly List<object> keep = new();
        private static float nextLook;
        public static volatile bool Recording;
        public static bool Attached { get; private set; }

        private static NativeHook<FloatCall> setFloat;
        private static NativeHook<IntCall> setInt;
        private static NativeHook<PointerCall> setColor, setVector, setTexture, setTextureOffset, setTextureScale;
        private static NativeHook<ObjectCall> setShader, enableKeyword, disableKeyword;
        private static NativeHook<KeywordIdCall> enableKeywordId, disableKeywordId;
        private static NativeHook<CustomCall> setCustom;
        private static NativeHook<CustomPointerCall> setCustomVector, setCustomColor, setCustomTexture;
        private static NativeHook<MaterialFloatCall> setMaterialFloat;
        private static NativeHook<LineMeshCall> setLineMesh;

        private static NativeHook<MaterialPointerCall> setMaterialVector, setMaterialColor, setMaterialTexture;

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

        // Attached as soon as the game's code is in place, so the settings the game gives renderers when it makes them,
        // before any recording, are remembered. Waiting for the game camera missed those of characters set up before it,
        // and their copies drew the game's snow cover on Neuvillette's hair.
        public static void Update()
        {
            if (Attached || Time.unscaledTime < nextLook)
                return;
            nextLook = Time.unscaledTime + 0.25f;
            if (Ready())
                Attach();
        }

        // Whether the SetFloat binding holds the bytes the hooks expect, which they do once the game has unpacked its code.
        private static bool Ready()
        {
            try
            {
                var bytes = new byte[16];
                Marshal.Copy(GetModuleHandleW("GenshinImpact.exe") + 0x143CA50, bytes, 0, bytes.Length);
                return Convert.ToHexString(bytes).ToLowerInvariant() == "48895c2408574883ec300f297424200f";
            }
            catch
            {
                return false;
            }
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
            // A texture's offset and scale, which some effects scroll from code each frame.
            // Found with REA behind Material's INTERNAL_CALL_SetTextureOffsetImpl and SetTextureScaleImpl; the value comes
            // by pointer.
            setTextureOffset = Hook<PointerCall>(module, 0x143DA70, "48895c24104889742418574883ec3049", "SetTextureOffset", (self, id, value) =>
            {
                var read = ReadVector2(value);
                setTextureOffset.Trampoline(self, id, value);
                Add(new Raw { Self = self, Kind = CallKind.TextureOffset, Id = id, Value = read });
            }, report);
            setTextureScale = Hook<PointerCall>(module, 0x143DB20, "48895c24104889742418574883ec3049", "SetTextureScale", (self, id, value) =>
            {
                var read = ReadVector2(value);
                setTextureScale.Trampoline(self, id, value);
                Add(new Raw { Self = self, Kind = CallKind.TextureScale, Id = id, Value = read });
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
                Add(new Raw { Self = self, Kind = CallKind.CustomFloat, Id = id, Int = -1, Value = new Vector4(value, 0, 0, 0) });
            }, report);
            // The engine bindings behind Renderer's other custom property setters, found with REA from the managed methods'
            // jumps. Vectors and colours come by pointer, textures as the object.
            const string Prologue = "48895c240848896c2410488974241857";
            setCustomVector = Hook<CustomPointerCall>(module, 0x144C600, Prologue, "Renderer.SetCustomPropertyVector", (self, id, value, updateHash) =>
            {
                var read = ReadVector(value);
                setCustomVector.Trampoline(self, id, value, updateHash);
                RememberCustom(self, CallKind.CustomVector, -1, id, read, IntPtr.Zero);
            }, report);
            setCustomColor = Hook<CustomPointerCall>(module, 0x144C740, Prologue, "Renderer.SetCustomPropertyColor", (self, id, value, updateHash) =>
            {
                var read = ReadVector(value);
                setCustomColor.Trampoline(self, id, value, updateHash);
                RememberCustom(self, CallKind.CustomColor, -1, id, read, IntPtr.Zero);
            }, report);
            setCustomTexture = Hook<CustomPointerCall>(module, 0x144C830, Prologue, "Renderer.SetCustomPropertyTexture", (self, id, value, updateHash) =>
            {
                setCustomTexture.Trampoline(self, id, value, updateHash);
                RememberCustom(self, CallKind.CustomTexture, -1, id, default, value);
            }, report);
            setMaterialFloat = Hook<MaterialFloatCall>(module, 0x144C1D0, "48895c24084889742410574883ec400f", "Renderer.SetPerMaterialCustomPropertyFloat",
                (self, material, id, value, updateHash) =>
                {
                    setMaterialFloat.Trampoline(self, material, id, value, updateHash);
                    RememberCustom(self, CallKind.CustomFloat, material, id, new Vector4(value, 0, 0, 0), IntPtr.Zero);
                }, report);
            setMaterialVector = Hook<MaterialPointerCall>(module, 0x144C240, Prologue, "Renderer.SetPerMaterialCustomPropertyVector", (self, material, id, value, updateHash) =>
            {
                var read = ReadVector(value);
                setMaterialVector.Trampoline(self, material, id, value, updateHash);
                RememberCustom(self, CallKind.CustomVector, material, id, read, IntPtr.Zero);
            }, report);
            setMaterialColor = Hook<MaterialPointerCall>(module, 0x144C2B0, Prologue, "Renderer.SetPerMaterialCustomPropertyColor", (self, material, id, value, updateHash) =>
            {
                var read = ReadVector(value);
                setMaterialColor.Trampoline(self, material, id, value, updateHash);
                RememberCustom(self, CallKind.CustomColor, material, id, read, IntPtr.Zero);
            }, report);
            setMaterialTexture = Hook<MaterialPointerCall>(module, 0x144C320, Prologue, "Renderer.SetPerMaterialCustomPropertyTexture", (self, material, id, value, updateHash) =>
            {
                setMaterialTexture.Trampoline(self, material, id, value, updateHash);
                RememberCustom(self, CallKind.CustomTexture, material, id, default, value);
            }, report);
            // Both SetCustomMeshDataWithSize overloads reach this binding, found with REA.
            setLineMesh = Hook<LineMeshCall>(module, 0x144DAC0, "488bc4535556574881ec88000000488b", "LineRenderer.SetCustomMeshDataWithSize",
                (self, triangles, triangleCount, positions, positionCount, normals, normalCount, uv0, uv0Count, colors, colorCount, uv1, uv1Count) =>
                {
                    var mesh = LineMesh.Read(triangles, triangleCount, positions, positionCount, normals, normalCount, uv0, uv0Count, colors, colorCount, uv1, uv1Count);
                    setLineMesh.Trampoline(self, triangles, triangleCount, positions, positionCount, normals, normalCount, uv0, uv0Count, colors, colorCount, uv1, uv1Count);
                    lock (gate)
                        LineMeshes[self] = mesh;
                    Add(new Raw { Self = self, Kind = CallKind.LineMesh, Line = mesh });
                }, report);
            // Attached only once every field holds its hook, so no detour runs before its trampoline is reachable.
            int attached = 0;
            foreach (var (name, hook) in new (string, object)[]
            {
                ("SetFloat", setFloat), ("SetInt", setInt), ("SetColor", setColor), ("SetVector", setVector), ("SetTexture", setTexture), ("set_shader", setShader),
                ("SetTextureOffset", setTextureOffset), ("SetTextureScale", setTextureScale),
                ("EnableKeyword", enableKeyword), ("DisableKeyword", disableKeyword), ("EnableKeywordID", enableKeywordId),
                ("DisableKeywordID", disableKeywordId), ("Renderer.SetCustomPropertyFloat", setCustom),
                ("Renderer.SetCustomPropertyVector", setCustomVector), ("Renderer.SetCustomPropertyColor", setCustomColor),
                ("Renderer.SetCustomPropertyTexture", setCustomTexture), ("Renderer.SetPerMaterialCustomPropertyFloat", setMaterialFloat),
                ("Renderer.SetPerMaterialCustomPropertyVector", setMaterialVector), ("Renderer.SetPerMaterialCustomPropertyColor", setMaterialColor),
                ("Renderer.SetPerMaterialCustomPropertyTexture", setMaterialTexture),
                ("LineRenderer.SetCustomMeshDataWithSize", setLineMesh),
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
