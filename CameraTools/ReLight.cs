using UnityEngine;

namespace CameraTools
{
    // Sphere lights drawn by iMMERSE ReLight, which path-traces them over the finished frame. ReLight keeps its lights in
    // screen space; CameraTools keeps a sphere at a world position and projects it through the game camera every frame,
    // so the sphere stays put while the camera moves. A sphere outside the view cannot be expressed and gives no light.
    // ReLight's own add-on writes the same slots and must not be loaded alongside.
    internal static class ReLight
    {
        private const string Effect = "MartysMods_RELIGHT.fx";

        private static readonly EffectName Technique = new(Effect, "MartysMods_RELIGHT");
        // ReLight takes its normals from Launchpad.
        private static readonly EffectName Launchpad = new("MartysMods_LAUNCHPAD.fx", "MartysMods_Launchpad");
        private static readonly EffectName Slot0 = new(Effect, "RELIGHT_LIGHT_DATA_0");
        private static readonly EffectName ActiveLights = new(Effect, "NUM_ACTIVE_LIGHTS");
        private static readonly EffectName SelectedLight = new(Effect, "SELECTED_LIGHT_INDEX");
        private static readonly EffectName OverlayBehavior = new(Effect, "LIGHT_OVERLAY_BEHAVIOR");
        private static readonly EffectName Ambient = new(Effect, "AMBIENT_INTENSITY");
        private static readonly EffectName[] DepthDefinitions =
        {
            new(Effect, "RESHADE_DEPTH_MULTIPLIER"),
            new(Effect, "RESHADE_DEPTH_INPUT_IS_LOGARITHMIC"),
            new(Effect, "RESHADE_DEPTH_INPUT_IS_REVERSED"),
            new(Effect, "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE"),
        };

        // Where a sphere landed on screen and in ReLight's space, for the log.
        public readonly record struct Placement(float U, float V, float ViewZ, float Linear, float ProjectedZ, float Radius, (uint X, uint Y, uint Z, uint W) Packed);

        // The effects switch from before the first sphere; null while no sphere shows.
        private static bool? effectsBefore;
        private static readonly string[] definitionValues = new string[4];

        public static bool Showing => effectsBefore.HasValue;

        public static DepthSettings? Depth { get; private set; }

        // Shows one sphere for this frame. Returns where it landed, or null while ReShade's depth settings are unread or
        // the sphere is outside the view.
        public static Placement? Show(Camera camera, Vector3 position, float radius, Color color, float intensity, float ambient)
        {
            if (!effectsBefore.HasValue)
            {
                effectsBefore = ReShade.Status.EffectsEnabled != 0;
                ReShade.SetEffects(true);
                ReShade.SetTechnique(Launchpad, true);
                ReShade.SetTechnique(Technique, true);
                ReShade.SetInt(SelectedLight, -1);
                ReShade.SetInt(OverlayBehavior, 0);
            }
            Depth ??= ReadDepth();
            var placement = Depth is DepthSettings depth ? Place(camera, position, radius, color, intensity, depth) : null;
            if (placement is Placement p)
            {
                ReShade.SetInt4(Slot0, p.Packed.X, p.Packed.Y, p.Packed.Z, p.Packed.W);
                ReShade.SetFloat(Ambient, ambient);
            }
            ReShade.SetInt(ActiveLights, placement.HasValue ? 1 : 0);
            return placement;
        }

        public static void Hide()
        {
            if (!effectsBefore.HasValue)
                return;
            ReShade.SetInt(ActiveLights, 0);
            ReShade.SetFloat(Ambient, 1f);
            ReShade.SetTechnique(Technique, false);
            ReShade.SetTechnique(Launchpad, false);
            ReShade.SetEffects(effectsBefore.Value);
            effectsBefore = null;
        }

        public static bool TechniqueLoaded => ReShade.TryGetTechnique(Technique, out _);

        private static DepthSettings? ReadDepth()
        {
            for (int i = 0; i < DepthDefinitions.Length; i++)
                if (!ReShade.TryGetDefinition(DepthDefinitions[i], out definitionValues[i]))
                    return null;
            return DepthSettings.Parse(definitionValues[0], definitionValues[1], definitionValues[2], definitionValues[3]);
        }

        // The wrappers have no WorldToViewportPoint, so this projects through the matrices. Unity's projection matrix is
        // OpenGL-style; on D3D11 the depth buffer holds reversed Z, which is (1 - ndc z) / 2.
        private static Placement? Place(Camera camera, Vector3 position, float radius, Color color, float intensity, DepthSettings depth)
        {
            var view = camera.worldToCameraMatrix;
            float vx = view.m00 * position.x + view.m01 * position.y + view.m02 * position.z + view.m03;
            float vy = view.m10 * position.x + view.m11 * position.y + view.m12 * position.z + view.m13;
            float vz = view.m20 * position.x + view.m21 * position.y + view.m22 * position.z + view.m23;
            float viewZ = -vz;
            if (viewZ <= camera.nearClipPlane)
                return null;
            var proj = camera.nonJitteredProjectionMatrix;
            float cx = proj.m00 * vx + proj.m01 * vy + proj.m02 * vz + proj.m03;
            float cy = proj.m10 * vx + proj.m11 * vy + proj.m12 * vz + proj.m13;
            float cz = proj.m20 * vx + proj.m21 * vy + proj.m22 * vz + proj.m23;
            float cw = proj.m30 * vx + proj.m31 * vy + proj.m32 * vz + proj.m33;
            float u = (cx / cw + 1f) * 0.5f, v = (1f - cy / cw) * 0.5f;
            if (u < 0f || u > 1f || v < 0f || v > 1f)
                return null;
            float linear = depth.Linearize((1f - cz / cw) * 0.5f);
            float projectedZ = depth.ProjectedZ(linear);
            // ReLight's space is close to the view scaled along its depth, so a radius in metres scales the same way.
            float scaled = radius * projectedZ / viewZ;
            var packed = ReLightPacking.Pack(u, v, linear, scaled, Byte(color.r), Byte(color.g), Byte(color.b), intensity);
            return new Placement(u, v, viewZ, linear, projectedZ, scaled, packed);
        }

        private static byte Byte(float channel) => (byte)MathF.Round(Math.Clamp(channel, 0f, 1f) * 255f);
    }
}
