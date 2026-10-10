using UnityEngine;

namespace CameraTools
{
    // A world position on screen: U and V from the top left, the distance along the view, and the depth buffer's value.
    internal readonly record struct ScreenPoint(float U, float V, float ViewZ, float Depth)
    {
        public bool OnScreen => U >= 0f && U <= 1f && V >= 0f && V <= 1f;

        // The wrappers have no WorldToViewportPoint, so this projects through the matrices. Unity's projection matrix is
        // OpenGL-style; on D3D11 the depth buffer holds reversed Z, which is (1 - ndc z) / 2. Null behind the camera.
        public static ScreenPoint? Of(Camera camera, Vector3 position)
        {
            var view = camera.worldToCameraMatrix;
            float vx = view.m00 * position.x + view.m01 * position.y + view.m02 * position.z + view.m03;
            float vy = view.m10 * position.x + view.m11 * position.y + view.m12 * position.z + view.m13;
            float vz = view.m20 * position.x + view.m21 * position.y + view.m22 * position.z + view.m23;
            if (-vz <= camera.nearClipPlane)
                return null;
            var proj = camera.nonJitteredProjectionMatrix;
            float cx = proj.m00 * vx + proj.m01 * vy + proj.m02 * vz + proj.m03;
            float cy = proj.m10 * vx + proj.m11 * vy + proj.m12 * vz + proj.m13;
            float cz = proj.m20 * vx + proj.m21 * vy + proj.m22 * vz + proj.m23;
            float cw = proj.m30 * vx + proj.m31 * vy + proj.m32 * vz + proj.m33;
            return new ScreenPoint((cx / cw + 1f) * 0.5f, (1f - cy / cw) * 0.5f, -vz, (1f - cz / cw) * 0.5f);
        }
    }

    // Sphere lights drawn by iMMERSE ReLight, which path-traces them over the finished frame. ReLight keeps its lights in
    // screen space, so every frame each sphere's world position is projected through the game camera and written into
    // one of its light slots. A sphere outside the view cannot be expressed and gives no light. ReLight's own add-on
    // writes the same slots and must not be loaded alongside.
    internal static class ReLight
    {
        public const string Effect = "MartysMods_RELIGHT.fx";
        // ReLight takes its normals from Launchpad, so both run while spheres show.
        public static readonly string[] Effects = { Effect, "MartysMods_LAUNCHPAD.fx" };
        private const int Slots = 64;
        // ReLight's outline is a shaded ball; see-through, it marks the sphere without hiding what it lights.
        private const float OutlineOpacity = 0.3f;

        private static readonly EffectName Technique = new(Effect, "MartysMods_RELIGHT");
        private static readonly EffectName[] SlotNames = Enumerable.Range(0, Slots).Select(i => new EffectName(Effect, $"RELIGHT_LIGHT_DATA_{i}")).ToArray();
        private static readonly EffectName ActiveLights = new(Effect, "NUM_ACTIVE_LIGHTS");
        private static readonly EffectName SelectedLight = new(Effect, "SELECTED_LIGHT_INDEX");
        private static readonly EffectName OverlayBehavior = new(Effect, "LIGHT_OVERLAY_BEHAVIOR");
        private static readonly EffectName OverlayOpacity = new(Effect, "LIGHT_OVERLAY_OPACITY");
        private static readonly EffectName Ambient = new(Effect, "AMBIENT_INTENSITY");
        private static readonly string[] DepthDefinitions =
        {
            "RESHADE_DEPTH_MULTIPLIER", "RESHADE_DEPTH_INPUT_IS_LOGARITHMIC", "RESHADE_DEPTH_INPUT_IS_REVERSED", "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE",
        };

        public readonly record struct Sphere(Vector3 Position, float Radius, Color Color, float Intensity);

        private static readonly string[] definitionValues = new string[DepthDefinitions.Length];
        private static DepthSettings? depth;
        private static int depthConnection = -1;

        // ReLight loaded and compiled, as far as the bridge can tell.
        public static bool Loaded => ReShade.TryGetTechnique(Technique, out _);

        // Writes this frame's spheres into ReLight. selected is the index in spheres of the one ReLight highlights, or
        // -1. Returns how many landed in the view, or -1 while ReShade's depth settings are unread.
        public static int Write(Camera camera, IReadOnlyList<Sphere> spheres, int selected, bool outlines, float ambient)
        {
            if (depthConnection != ReShade.Connections)
            {
                depthConnection = ReShade.Connections;
                depth = null;
            }
            depth ??= ReadDepth(Effect);
            if (depth is not DepthSettings settings)
                return -1;
            int count = 0, highlighted = -1;
            for (int i = 0; i < spheres.Count && count < Slots; i++)
            {
                var sphere = spheres[i];
                if (ScreenPoint.Of(camera, sphere.Position) is not { OnScreen: true } point)
                    continue;
                float linear = settings.Linearize(point.Depth);
                // ReLight's space is close to the view scaled along its depth, so a radius in metres scales the same way.
                float radius = sphere.Radius * settings.ProjectedZ(linear) / point.ViewZ;
                var c = sphere.Color;
                var packed = ReLightPacking.Pack(point.U, point.V, linear, radius, Colors.Byte(c.r), Colors.Byte(c.g), Colors.Byte(c.b), sphere.Intensity);
                ReShade.SetInt4(SlotNames[count], packed.X, packed.Y, packed.Z, packed.W);
                if (i == selected)
                    highlighted = count;
                count++;
            }
            ReShade.SetInt(ActiveLights, count);
            ReShade.SetInt(SelectedLight, highlighted);
            ReShade.SetInt(OverlayBehavior, outlines ? 3 : 0);
            ReShade.SetFloat(OverlayOpacity, OutlineOpacity);
            ReShade.SetFloat(Ambient, ambient);
            return count;
        }

        public static void Clear()
        {
            ReShade.SetInt(ActiveLights, 0);
            ReShade.SetInt(OverlayBehavior, 0);
            ReShade.SetFloat(Ambient, 1f);
        }

        // ReShade's depth settings as an effect sees them, or null while the bridge cannot read them.
        public static DepthSettings? ReadDepth(string effect)
        {
            for (int i = 0; i < DepthDefinitions.Length; i++)
                if (!ReShade.TryGetDefinition(new EffectName(effect, DepthDefinitions[i]), out definitionValues[i]))
                    return null;
            var read = DepthSettings.Parse(definitionValues[0], definitionValues[1], definitionValues[2], definitionValues[3]);
            CameraTools.LogOnce($"ReShade's depth settings for {effect} are {read}.");
            return read;
        }
    }
}
