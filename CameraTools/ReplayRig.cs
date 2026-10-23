using Il2CppInterop.Runtime;
using Il2CppInterop.Runtime.InteropTypes.Arrays;
using UnityEngine;

namespace CameraTools
{
    // One frame of a recorded hierarchy: the root's world pose, every transform below it, which of them are on, which
    // renderers draw, their property blocks, the face shapes and each material's dissolve value.
    internal sealed class RigPose
    {
        public Vector3 Position;
        public Quaternion Rotation;
        public Vector3[] Local, Scale;
        public Quaternion[] Turn;
        public bool[] On, Drawn;
        public MaterialPropertyBlock[] Blocks;
        public float[] Shapes;
        public float[] Dissolve;
        public MaterialPropertyBlock[] SlotBlocks;
        // Each line renderer's points, null for other renderers and for rigs without lines.
        public Vector3[][] Points;
        // The live particles of each world-space system that draws trails, and that system's place among the rig's
        // particle systems; null without such systems.
        public Il2CppStructArray<ParticleSystem.Particle>[] Particles;
        public int[] ParticleSystems;
    }

    // A recorded hierarchy: a character, a weapon, an effect or an enemy. A copy made by ReplayCopies.Strip has the same
    // transforms and renderers in the same order, or, for a character, matched to the recorded ones part for part, so a
    // pose read from the original writes onto the copy.
    internal sealed class Rig
    {
        public const string BaseShader = "miHoYo/Character/Character_Base_Uber", DissolveShader = "miHoYo/Uber/Character/Effect_Dissolve";

        private static readonly int DissolveValue = Shader.PropertyToID("_DissolveValue");
        private static readonly int Dither = Shader.PropertyToID("_DitherAlpha");
        private static readonly int MainTex = Shader.PropertyToID("_MainTex");
        private static readonly MaterialPropertyBlock scratch = new();

        public readonly string Name;
        public readonly Transform Root;
        public readonly Transform[] Bones;
        public readonly GameObject[] Objects;
        public readonly Renderer[] Renderers;
        public readonly SkinnedMeshRenderer[] Shaped;
        public readonly int[] ShapeCounts;
        private readonly int shapeTotal;

        // Every material of every renderer, as (renderer, index in its materials), and the dissolve shader last seen in
        // each. Spawn, death and switch fades run on a material's _DissolveValue, which property blocks do not carry.
        public readonly (int Renderer, int Material)[] Slots;
        public readonly Shader[] SeenDissolve;

        // Each renderer's mesh filter, none for skinned renderers.
        private readonly MeshFilter[] filters;

        // Particle systems simulated in world space that draw trails or ribbons, whose particles are recorded each frame.
        // A copy re-simulates its particles at its current place, so particles the original laid along a path, as
        // Neuvillette's charged attack beam is a ribbon through, all started from one point there and drew nothing.
        private readonly ParticleSystem[] systems;
        private readonly int[] traced;
        private const int MaxParticles = 512;

        // Diagnostic: the names of the systems whose particles are recorded.
        public string TracedNames => string.Join(", ", traced.Select(i => systems[i] ? systems[i].name : "?"));

        // Line renderers, whose points the game sets from code each frame: Neuvillette's charged attack beam is three lines
        // from his hand to its target, and a copy kept the points it was made with.
        private readonly LineRenderer[] lines;
        private readonly bool hasLines;
        private const int MaxPoints = 64;

        // Renderers that keep a property block per material, which the copy gets each frame too.
        private readonly bool[] perMaterial;

        // The material in each slot at the last Read, for matching the game's setter calls to slots.
        public readonly IntPtr[] SlotPointers;
        public readonly Material[] SlotMaterials;

        // Whether the hooks' calls are filed under this rig: characters, weapons and enemies.
        public bool Watched;

        public Rig(Transform root)
            : this(root, ReplayScene.All<Transform>(root).Where(bone => bone.Pointer != root.Pointer).ToArray(), ReplayScene.All<Renderer>(root), null)
        {
        }

        // A copy's rig with the parts of like, a rig of the original read earlier, at the same places in the copy: the game
        // can add parts to a character after its recording began, such as Mavuika's camera targets, and the copy, taken
        // later, then had more parts than the recording, and played the wrong poses on them. Parts like no longer has are
        // left out; the copy's parts that like lacks stay as they are.
        public static Rig Matching(Transform copy, Rig like)
        {
            var bones = like.Bones.Select(bone => Find(copy, like.Root, bone)).ToArray();
            var renderers = like.Renderers.Select(renderer =>
                renderer && Find(copy, like.Root, renderer.transform) is { } at ? at.GetComponent(renderer.GetIl2CppType())?.TryCast<Renderer>() : null).ToArray();
            return new Rig(copy, bones, renderers, like);
        }

        // The transform at part's place under from, found under to: the same names down the hierarchy, each the same
        // one among siblings of its name.
        private static Transform Find(Transform to, Transform from, Transform part)
        {
            if (!part)
                return null;
            var steps = new List<(string Name, int Among)>();
            for (var at = part; at && at.Pointer != from.Pointer; at = at.parent)
            {
                var parent = at.parent;
                if (!parent)
                    return null;
                int among = 0;
                for (int i = 0; i < at.GetSiblingIndex(); i++)
                    if (parent.GetChild(i).name == at.name)
                        among++;
                steps.Add((at.name, among));
            }
            var found = to;
            for (int s = steps.Count - 1; s >= 0 && found; s--)
            {
                var (name, among) = steps[s];
                Transform next = null;
                for (int i = 0; i < found.childCount && next == null; i++)
                    if (found.GetChild(i) is var child && child.name == name && among-- == 0)
                        next = child;
                found = next;
            }
            return found;
        }

        private Rig(Transform root, Transform[] bones, Renderer[] renderers, Rig like)
        {
            Name = root.name;
            Root = root;
            Bones = bones;
            Objects = Bones.Select(bone => bone ? bone.gameObject : null).ToArray();
            Renderers = renderers;
            var shaped = new List<SkinnedMeshRenderer>();
            var counts = new List<int>();
            if (like != null)
            {
                // The same renderers and counts as like, so the recorded face shapes land on the right meshes, whatever
                // meshes the copy holds before it takes the original's.
                for (int s = 0; s < like.Shaped.Length; s++)
                {
                    int r = Array.FindIndex(like.Renderers, renderer => renderer && like.Shaped[s] && renderer.Pointer == like.Shaped[s].Pointer);
                    shaped.Add(r >= 0 && Renderers[r] ? Renderers[r].TryCast<SkinnedMeshRenderer>() : null);
                    counts.Add(like.ShapeCounts[s]);
                }
            }
            else
                foreach (var renderer in Renderers)
                    if (renderer && renderer.TryCast<SkinnedMeshRenderer>() is { } skinned && skinned.sharedMesh is { } mesh && mesh.blendShapeCount > 0)
                    {
                        shaped.Add(skinned);
                        counts.Add(mesh.blendShapeCount);
                    }
            Shaped = shaped.ToArray();
            ShapeCounts = counts.ToArray();
            shapeTotal = counts.Sum();
            var slots = new List<(int, int)>();
            for (int i = 0; i < Renderers.Length; i++)
            {
                int count = like != null ? (like.Renderers[i] ? like.Renderers[i].sharedMaterials.Length : 0) : Renderers[i] ? Renderers[i].sharedMaterials.Length : 0;
                for (int j = 0; j < count; j++)
                    slots.Add((i, j));
            }
            Slots = slots.ToArray();
            SeenDissolve = new Shader[Slots.Length];
            SlotPointers = new IntPtr[Slots.Length];
            SlotMaterials = new Material[Slots.Length];
            filters = Renderers.Select(renderer => renderer && !renderer.TryCast<SkinnedMeshRenderer>() ? renderer.GetComponent(Il2CppType.Of<MeshFilter>())?.TryCast<MeshFilter>() : null).ToArray();
            perMaterial = Renderers.Select(renderer => renderer && renderer.AllowPerMaterialProp()).ToArray();
            lines = Renderers.Select(renderer => renderer ? renderer.TryCast<LineRenderer>() : null).ToArray();
            systems = ReplayScene.All<ParticleSystem>(root);
            traced = Enumerable.Range(0, systems.Length)
                .Where(i => systems[i].main.simulationSpace == ParticleSystemSimulationSpace.World && systems[i].trails.enabled).ToArray();
            hasLines = lines.Any(line => line != null);
        }

        private static Il2CppStructArray<ParticleSystem.Particle> Trim(Il2CppStructArray<ParticleSystem.Particle> particles, int count)
        {
            var trimmed = new Il2CppStructArray<ParticleSystem.Particle>(count);
            for (int i = 0; i < count; i++)
                trimmed[i] = particles[i];
            return trimmed;
        }

        // Puts a copy's traced systems' particles where the recording had them, after its simulation made its own.
        public void WriteParticles(RigPose pose)
        {
            if (pose.Particles == null)
                return;
            for (int k = 0; k < pose.Particles.Length; k++)
                if (pose.ParticleSystems[k] < systems.Length && systems[pose.ParticleSystems[k]] is { } system && system && pose.Particles[k] is { } particles)
                    system.SetParticles(particles, particles.Length);
        }

        // Diagnostic: a line renderer's points, width, colour and space.
        public string DescribeLine(int r)
        {
            if (r >= lines.Length || lines[r] is not { } line || !line)
                return "no line";
            var points = Enumerable.Range(0, Math.Min(line.positionCount, 4)).Select(p => line.GetPosition(p)).Select(p => $"({p.x:0.#},{p.y:0.#},{p.z:0.#})");
            return $"{line.positionCount} points {string.Join(" ", points)} width {line.widthMultiplier:0.###} alpha {line.startColor.a:0.##}/{line.endColor.a:0.##} "
                + $"{(line.useWorldSpace ? "world" : "local")} {(line.gameObject.activeInHierarchy ? "on" : "off")}/{(line.enabled ? "drawn" : "hidden")}";
        }

        // Diagnostic: one renderer's state, for comparing an original with its copy.
        public string Describe(int r)
        {
            var renderer = Renderers[r];
            if (!renderer)
                return "gone";
            var mesh = MeshOf(r);
            var shaders = renderer.sharedMaterials.Select(material => !material ? "null" : !material.shader ? "no shader"
                : material.shader.name.Split('/').Last() + (material.HasProperty(DissolveValue) ? $":{material.GetFloat(DissolveValue):0.##}" : "")
                    + (material.HasProperty(MainTex) ? material.mainTexture is { } texture && texture ? $" tex {texture.name}/{texture.width}" : " tex none" : ""));
            return $"{(renderer.gameObject.activeInHierarchy ? "on" : "off")}/{(renderer.enabled ? "drawn" : "hidden")} "
                + $"mesh {(mesh ? $"{mesh.name}:{mesh.vertexCount}" : "none")} [{string.Join(", ", shaders)}]";
        }

        public Mesh MeshOf(int r) => !Renderers[r] ? null
            : Renderers[r].TryCast<SkinnedMeshRenderer>() is { } skinned ? skinned.sharedMesh : filters[r] ? filters[r].sharedMesh : null;

        public void SetMesh(int r, Mesh mesh)
        {
            if (!Renderers[r])
                return;
            if (Renderers[r].TryCast<SkinnedMeshRenderer>() is { } skinned)
                skinned.sharedMesh = mesh;
            else if (filters[r])
                filters[r].sharedMesh = mesh;
        }

        // Copies the engine's renderer flags from the original, as Instantiate may not carry them.
        public void TakeRendererState(Rig from)
        {
            for (int i = 0; i < Renderers.Length && i < from.Renderers.Length; i++)
                if (Renderers[i] && from.Renderers[i])
                    CopyRendererState(from.Renderers[i], Renderers[i]);
        }

        private static void CopyRendererState(Renderer from, Renderer to)
        {
            to.skipGPUCulling = from.skipGPUCulling;
            to.matLayer = from.matLayer;
            to.enableDither = from.enableDither;
            to.cullingBoundGroupID = from.cullingBoundGroupID;
            to.perObjectShadowGroupID = from.perObjectShadowGroupID;
            to.characterGroupRendererType = from.characterGroupRendererType;
            to.allowHalfResolution = from.allowHalfResolution;
            to.viewDistanceRatio = from.viewDistanceRatio;
            to.receiveDecals = from.receiveDecals;
            to.SetCustomPropertyInstancingMode(from.GetCustomPropertyInstancingMode());
            to.SetAllowPerMaterialProp(from.AllowPerMaterialProp());
        }

        public RigPose Read()
        {
            var pose = new RigPose
            {
                Position = WorldShift.Absolute(Root.position),
                Rotation = Root.rotation,
                Local = new Vector3[Bones.Length],
                Scale = new Vector3[Bones.Length],
                Turn = new Quaternion[Bones.Length],
                On = new bool[Bones.Length],
                Drawn = new bool[Renderers.Length],
                Blocks = new MaterialPropertyBlock[Renderers.Length],
                Shapes = new float[shapeTotal],
                Dissolve = new float[Slots.Length],
                SlotBlocks = new MaterialPropertyBlock[Slots.Length],
            };
            for (int i = 0; i < Bones.Length; i++)
            {
                var bone = Bones[i];
                if (!bone)
                    continue;
                pose.Local[i] = bone.localPosition;
                pose.Turn[i] = bone.localRotation;
                pose.Scale[i] = bone.localScale;
                pose.On[i] = Objects[i].activeSelf;
            }
            for (int i = 0; i < Renderers.Length; i++)
            {
                var renderer = Renderers[i];
                if (!renderer)
                    continue;
                pose.Drawn[i] = renderer.enabled;
                var block = new MaterialPropertyBlock();
                renderer.GetPropertyBlock(block);
                pose.Blocks[i] = block;
            }
            int k = 0;
            for (int s = 0; s < Shaped.Length; s++)
                for (int j = 0; j < ShapeCounts[s]; j++, k++)
                    pose.Shapes[k] = Shaped[s] ? Shaped[s].GetBlendShapeWeight(j) : 0;
            for (int slot = 0; slot < Slots.Length; slot++)
            {
                var (r, m) = Slots[slot];
                if (!perMaterial[r] || !Renderers[r])
                    continue;
                var block = new MaterialPropertyBlock();
                Renderers[r].GetPerMaterialPropertyBlock(block, m);
                pose.SlotBlocks[slot] = block;
            }
            // NaN where the material has no dissolve value, which plays as fully shown.
            Il2CppReferenceArray<Material> materials = null;
            int current = -1;
            for (int slot = 0; slot < Slots.Length; slot++)
            {
                var (r, m) = Slots[slot];
                if (r != current)
                {
                    current = r;
                    materials = Renderers[r] ? Renderers[r].sharedMaterials : null;
                }
                var material = materials != null && m < materials.Length ? materials[m] : null;
                SlotMaterials[slot] = material;
                SlotPointers[slot] = material ? material.Pointer : IntPtr.Zero;
                if (material && material.HasProperty(DissolveValue))
                {
                    pose.Dissolve[slot] = material.GetFloat(DissolveValue);
                    SeenDissolve[slot] = material.shader;
                }
                else
                    pose.Dissolve[slot] = float.NaN;
            }
            if (traced.Length > 0)
            {
                pose.Particles = new Il2CppStructArray<ParticleSystem.Particle>[traced.Length];
                pose.ParticleSystems = traced;
                for (int t = 0; t < traced.Length; t++)
                    if (systems[traced[t]] is { } system && system)
                    {
                        var particles = new Il2CppStructArray<ParticleSystem.Particle>(Math.Min(system.particleCount, MaxParticles));
                        int count = particles.Length > 0 ? system.GetParticles(particles) : 0;
                        pose.Particles[t] = count == particles.Length ? particles : Trim(particles, count);
                    }
            }
            if (hasLines)
            {
                pose.Points = new Vector3[lines.Length][];
                for (int r = 0; r < lines.Length; r++)
                    if (lines[r] is { } line && line)
                    {
                        var points = new Vector3[Math.Min(line.positionCount, MaxPoints)];
                        bool world = line.useWorldSpace;
                        for (int p = 0; p < points.Length; p++)
                            points[p] = world ? WorldShift.Absolute(line.GetPosition(p)) : line.GetPosition(p);
                        pose.Points[r] = points;
                    }
            }
            return pose;
        }

        // The hierarchy as it is now: every transform's name below the root. Material swaps are recorded as calls, so
        // only parts the game adds or removes start a new track.
        public static (int Hash, string[] Parts) Structure(Transform root)
        {
            var parts = new List<string>();
            foreach (var transform in ReplayScene.All<Transform>(root))
                if (transform.Pointer != root.Pointer)
                    parts.Add(transform.name);
            int hash = 17;
            foreach (var part in parts)
                hash = hash * 31 + part.GetHashCode();
            return (hash, parts.ToArray());
        }

        // The transforms, blended between two poses of the same run.
        public void WriteTransforms(RigPose a, RigPose b, float blend)
        {
            Root.SetPositionAndRotation(WorldShift.Relative(Vector3.Lerp(a.Position, b.Position, blend)), Quaternion.Slerp(a.Rotation, b.Rotation, blend));
            for (int i = 0; i < Bones.Length; i++)
            {
                var bone = Bones[i];
                if (!bone)
                    continue;
                if (Objects[i].activeSelf != a.On[i])
                    Objects[i].SetActive(a.On[i]);
                bone.localPosition = Vector3.Lerp(a.Local[i], b.Local[i], blend);
                bone.localRotation = Quaternion.Slerp(a.Turn[i], b.Turn[i], blend);
                bone.localScale = Vector3.Lerp(a.Scale[i], b.Scale[i], blend);
            }
        }

        // What the game's scripts may overwrite late in the frame: renderers on or off, property blocks, per-material
        // blocks and face shapes. With opaque set, the recorded _DitherAlpha is replaced by 1: the game fades a character
        // near or behind its camera, which was the recording's camera, not the one filming the playback.
        public void WriteLooks(RigPose a, RigPose b, float blend, bool opaque)
        {
            for (int i = 0; i < Renderers.Length; i++)
            {
                var renderer = Renderers[i];
                if (!renderer)
                    continue;
                if (renderer.enabled != a.Drawn[i])
                    renderer.enabled = a.Drawn[i];
                if (a.Blocks[i] == null)
                    continue;
                renderer.SetPropertyBlock(a.Blocks[i]);
                if (opaque && a.Blocks[i].GetFloat(Dither) < 1)
                {
                    renderer.GetPropertyBlock(scratch);
                    scratch.SetFloat(Dither, 1);
                    renderer.SetPropertyBlock(scratch);
                }
            }
            for (int slot = 0; slot < Slots.Length && slot < a.SlotBlocks.Length; slot++)
                if (a.SlotBlocks[slot] is { } block && Renderers[Slots[slot].Renderer])
                    Renderers[Slots[slot].Renderer].SetPerMaterialPropertyBlock(block, Slots[slot].Material);
            int k = 0;
            for (int s = 0; s < Shaped.Length; s++)
                for (int j = 0; j < ShapeCounts[s]; j++, k++)
                    if (Shaped[s])
                        Shaped[s].SetBlendShapeWeight(j, Mathf.Lerp(a.Shapes[k], b.Shapes[k], blend));
            if (!hasLines || a.Points == null)
                return;
            for (int r = 0; r < lines.Length && r < a.Points.Length; r++)
            {
                if (lines[r] is not { } line || !line || a.Points[r] is not { } from)
                    continue;
                var to = b.Points?.ElementAtOrDefault(r) is { } next && next.Length == from.Length ? next : from;
                bool world = line.useWorldSpace;
                if (line.positionCount != from.Length)
                    line.positionCount = from.Length;
                for (int p = 0; p < from.Length; p++)
                {
                    var point = Vector3.Lerp(from[p], to[p], blend);
                    line.SetPosition(p, world ? WorldShift.Relative(point) : point);
                }
            }
        }

        // Writes the recorded dissolve values into a copy's own materials.
        public void WriteDissolve(RigPose a, RigPose b, float blend, Material[][] materials)
        {
            for (int slot = 0; slot < Slots.Length && slot < a.Dissolve.Length; slot++)
            {
                var (r, m) = Slots[slot];
                var row = r < materials.Length ? materials[r] : null;
                var material = row != null && m < row.Length ? row[m] : null;
                if (!material || !material.HasProperty(DissolveValue))
                    continue;
                float from = a.Dissolve[slot], to = b.Dissolve[slot];
                float value = float.IsNaN(from) ? 0 : float.IsNaN(to) ? from : Mathf.Lerp(from, to, blend);
                material.SetFloat(DissolveValue, value);
            }
        }

        // The dissolve shader of materials captured from a character just switched in, else the one seen last, else any
        // loaded shader of that name. A recording of one character has no switch, Shader.Find does not find the game's
        // shaders, and a copy left on the base shader drew white blobs on Neuvillette's torso.
        public static Shader FindShader(IEnumerable<Material[][]> captured, string name)
        {
            foreach (var materials in captured)
                foreach (var row in materials)
                    foreach (var material in row ?? Array.Empty<Material>())
                        if (material && material.shader && material.shader.name == name)
                            return Remember(material.shader);
            if (dissolve && dissolve.name == name)
                return dissolve;
            if (Shader.Find(name) is { } found && found)
                return Remember(found);
            var loaded = Resources.FindObjectsOfTypeAll(Il2CppType.Of<Shader>());
            for (int i = 0; i < loaded.Length; i++)
                if (loaded[i] && loaded[i].name == name && loaded[i].TryCast<Shader>() is { } shader)
                    return Remember(shader);
            CameraTools.LogOnce($"Replays: no loaded shader is called {name}; character copies keep their own shaders.");
            return null;
        }

        private static Shader dissolve;

        public static Shader Remember(Shader shader)
        {
            if (shader && shader.name == DissolveShader)
                dissolve = shader;
            return shader;
        }

        // Puts captured materials on the dissolve shader, fully shown. A copy of a character long on the field has its
        // materials on the base shader, and such a copy rarely draws; a copy taken 1 s after a switch still has the
        // dissolve shader, and draws. Each slot takes the dissolve shader seen on it during the recording, else fallback.
        public void UseDissolve(Material[][] materials, Shader fallback)
        {
            for (int slot = 0; slot < Slots.Length; slot++)
            {
                var (r, m) = Slots[slot];
                var row = r < materials.Length ? materials[r] : null;
                var material = row != null && m < row.Length ? row[m] : null;
                if (!material || !material.shader || material.HasProperty(DissolveValue))
                    continue;
                var target = SeenDissolve[slot] ? SeenDissolve[slot] : material.shader.name == BaseShader ? fallback : null;
                if (!target)
                    continue;
                material.shader = target;
                material.SetFloat(DissolveValue, 0);
            }
        }

        // Independent copies of every renderer's materials, so later changes the game makes to the originals do not reach
        // whatever uses them: copies that shared their materials with off-field characters did not draw.
        public Material[][] TakeMaterials() => Renderers.Select(renderer =>
            renderer ? renderer.sharedMaterials.Select(material => material ? new Material(material) : null).ToArray() : null).ToArray();

        public void UseMaterials(Material[][] materials)
        {
            for (int i = 0; i < Renderers.Length && i < materials.Length; i++)
                if (Renderers[i] && materials[i] != null)
                    Renderers[i].sharedMaterials = materials[i];
        }

        // Holds a character copy's LOD loaders at full detail, level 0, wherever the camera is. Left to choose, a copy's
        // loader chose its lowest level, which leaves out whole parts of the body: Skirk's arms and legs, Mavuika's bike.
        public void HoldFullDetail()
        {
            foreach (var loader in ReplayScene.All<MiHoYoLodLoader>(Root))
            {
                loader.PauseLodLoaderWithSpecificLodLevel(0);
                loader.TryApplySpecificLodLevelImmdiate(false);
            }
        }

        // Starts the engine's mesh LOD streaming again on a copy of an off-field object, which keeps it paused at low
        // detail.
        public void ResumeLods()
        {
            foreach (var loader in ReplayScene.All<MiHoYoLodLoader>(Root))
            {
                loader.ResumeLodLoader();
                loader.TryApplySpecificLodLevelImmdiate(false);
            }
        }

        // An enemy copy's meshes come from the recorded mesh swaps, so its own LOD components must not replace them.
        public void StopLods()
        {
            foreach (var loader in ReplayScene.All<MiHoYoLodLoader>(Root))
                loader.enabled = false;
            foreach (var group in ReplayScene.All<MiHoYoLodMeshGroup>(Root))
                group.enabled = false;
        }
    }
}
