using System.Runtime.InteropServices;
using Il2CppInterop.Runtime;
using UnityEngine;
using Object = UnityEngine.Object;

namespace CameraTools
{
    // The scene roots a replay records from. The game keeps them for the session, so each is looked up once it exists.
    internal static class ReplayScene
    {
        private static Transform avatars, equips, effects, monsters, monsterEquips, gadgets, bullets;

        public static Transform AvatarRoot => Find(ref avatars, "/EntityRoot/AvatarRoot");

        public static Transform EquipRoot => Find(ref equips, "/EntityRoot/EquipRoot");

        // Pooled effects are its direct children, kept active and played again by the pool.
        public static Transform EffectPool => Find(ref effects, "/EffectPool");

        public static Transform MonsterRoot => Find(ref monsters, "/EntityRoot/MonsterRoot");

        public static Transform MonsterEquipRoot => Find(ref monsterEquips, "/EntityRoot/MonsterEquipRoot");

        // Gadgets and bullets, such as Lisa's burst, are entities of their own rather than pooled effects.
        public static IEnumerable<Transform> Gadgets()
            => Children(Find(ref gadgets, "/EntityRoot/OtherGadgetRoot")).Concat(Children(Find(ref bullets, "/EntityRoot/BulletRoot")));

        private static Transform Find(ref Transform cached, string path)
        {
            if (!cached)
                cached = GameObject.Find(path)?.transform;
            return cached;
        }

        public static IEnumerable<Transform> Children(Transform parent)
        {
            if (!parent)
                yield break;
            for (int i = 0; i < parent.childCount; i++)
                yield return parent.GetChild(i);
        }

        public static T[] All<T>(Transform root) where T : Component
        {
            var found = root.GetComponentsInChildren(Il2CppType.Of<T>(), true);
            var result = new List<T>(found.Length);
            for (int i = 0; i < found.Length; i++)
                if (found[i].TryCast<T>() is { } each && each)
                    result.Add(each);
            return result.ToArray();
        }

        // The particle systems at the top of a hierarchy, which play the ones below them.
        public static ParticleSystem[] TopSystems(Transform root) => All<ParticleSystem>(root)
            .Where(system => !(system.transform.parent && system.transform.parent.GetComponent(Il2CppType.Of<ParticleSystem>()))).ToArray();

        public static string TypeName(Il2CppSystem.Object value)
            => Marshal.PtrToStringAnsi(IL2CPP.il2cpp_class_get_name(IL2CPP.il2cpp_object_get_class(value.Pointer)));
    }

    // Copies of game objects without the game's scripts: only transforms, renderers, particle systems, animators and the
    // engine's mesh LOD components stay. A copy is made under an inactive holder, so no copied script wakes up before it is
    // removed. Copies without the LOD components showed the characters' low-detail meshes.
    internal static class ReplayCopies
    {
        private static readonly HashSet<string> Kept = new()
        {
            "Transform", "RectTransform", "ParticleSystem", "ParticleSystemRenderer", "MeshRenderer", "MeshFilter",
            "SkinnedMeshRenderer", "TrailRenderer", "LineRenderer", "Animator", "MiHoYoLodLoader", "MiHoYoLodMeshGroup",
        };

        private static GameObject holder, shown;

        private static Transform Shown
        {
            get
            {
                if (!shown)
                    shown = new GameObject("CameraTools Replays");
                return shown.transform;
            }
        }

        public static GameObject Strip(GameObject source)
        {
            if (!holder)
            {
                holder = new GameObject("CameraTools Replay Copies");
                holder.SetActive(false);
            }
            var copy = Object.Instantiate(source, holder.transform).TryCast<GameObject>();
            copy.name = source.name;
            // Some components need others removed first, so a few passes clear them all.
            for (int pass = 0; pass < 3; pass++)
            {
                var found = copy.GetComponentsInChildren(Il2CppType.Of<Component>(), true);
                bool any = false;
                for (int i = 0; i < found.Length; i++)
                {
                    var component = found[i];
                    if (!component || Kept.Contains(ReplayScene.TypeName(component)))
                        continue;
                    any = true;
                    try
                    {
                        Object.DestroyImmediate(component);
                    }
                    catch
                    {
                        // Left for the next pass.
                    }
                }
                if (!any)
                    break;
            }
            return copy;
        }

        // Shown copies are driven by the recording alone, so their animators are switched off.
        public static void Show(GameObject copy)
        {
            copy.transform.SetParent(Shown, true);
            foreach (var animator in ReplayScene.All<Animator>(copy.transform))
                animator.enabled = false;
        }
    }

    // The script-free copy of one game object, made once and played by every replay and every play of it, or made for one
    // track alone. A copy for each play of a pooled effect, about twelve a second in a fight, dropped the recording to 8
    // frames a second.
    internal sealed class StandIn
    {
        public Rig Rig;
        // Playback simulates the systems at the top of the copy, with every system's seed fixed.
        public ParticleSystem[] Tops = Array.Empty<ParticleSystem>();
        public TrailRenderer[] Trails = Array.Empty<TrailRenderer>();
        public bool Shown;
        // The track that last played this copy, so a new play can set its materials and clear its trails.
        public Track Owner;
        // Made for one track, and destroyed with it.
        public bool Own;

        public bool Usable => Rig != null && Rig.Root;

        public void Destroy()
        {
            if (Rig?.Root)
                Object.Destroy(Rig.Root.gameObject);
        }
    }

    internal static class StandIns
    {
        private const int Limit = 1000;
        private static readonly Dictionary<IntPtr, StandIn> made = new();

        // The copy shared by every replay, made on first use.
        public static StandIn Shared(Transform root, TrackKind kind)
        {
            if (made.TryGetValue(root.Pointer, out var known) && known.Usable)
                return known;
            if (made.Count >= Limit)
                return null;
            var copy = Make(root, kind);
            made[root.Pointer] = copy;
            return copy;
        }

        // A copy for one track alone: a character's, taken once it has settled on the field, and an enemy's, taken as its
        // structure is in that frame.
        public static StandIn Own(Transform root, TrackKind kind)
        {
            var copy = Make(root, kind);
            copy.Own = true;
            return copy;
        }

        private static StandIn Make(Transform root, TrackKind kind)
        {
            var copy = new StandIn();
            try
            {
                var source = new Rig(root);
                var rig = new Rig(ReplayCopies.Strip(root.gameObject).transform);
                if (rig.Bones.Length != source.Bones.Length || rig.Renderers.Length != source.Renderers.Length)
                {
                    CameraTools.LogOnce($"Replays: the copy of {root.name} does not match it ({rig.Bones.Length} of {source.Bones.Length} transforms, "
                        + $"{rig.Renderers.Length} of {source.Renderers.Length} renderers) and is not used.");
                    Object.Destroy(rig.Root.gameObject);
                    return copy;
                }
                if (kind is not (TrackKind.Effect or TrackKind.Gadget))
                {
                    rig.TakeRendererState(source);
                    for (int i = 0; i < rig.Renderers.Length; i++)
                        if (rig.Renderers[i] && source.Renderers[i])
                            ReplayHooks.Transplant(source.Renderers[i], rig.Renderers[i]);
                }
                var systems = ReplayScene.All<ParticleSystem>(rig.Root);
                var originals = ReplayScene.All<ParticleSystem>(root);
                for (int i = 0; i < systems.Length; i++)
                {
                    systems[i].useAutoRandomSeed = false;
                    systems[i].randomSeed = i < originals.Length ? originals[i].randomSeed : (uint)i + 1;
                }
                copy.Tops = ReplayScene.TopSystems(rig.Root);
                copy.Trails = ReplayScene.All<TrailRenderer>(rig.Root);
                if (kind is TrackKind.Effect or TrackKind.Gadget)
                    rig.UseMaterials(source.TakeMaterials());
                copy.Rig = rig;
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Replays: copying {root.name} failed: {e.Message}");
            }
            return copy;
        }
    }
}
