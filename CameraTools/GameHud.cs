using UnityEngine;
using UnityEngine.SceneManagement;

namespace CameraTools
{
    // Hides the game's HUD while the free camera is on or the UI is hidden. Fading CanvasGroups, instead of turning pages
    // off, keeps the pages' scripts running. CameraTools' UI is not under any of these.
    internal static class GameHud
    {
        private const float LookInterval = 1f;
        private const float BoardScanInterval = 0.25f;
        private const string BoardRoot = "AvatarBoardCanvasV2(Clone)";

        // The pages photo mode hides (both carry the game's MonoSpecificUIHide). Their Animators write the same CanvasGroup,
        // so the fade is applied every frame and the group's own values are put back when the HUD shows again.
        private static readonly Faded[] Pages =
        {
            new("/Canvas/Pages/InLevelMainPage/GrpMainPage", KeepDamageNumbers),
            new("/Canvas/Dialogs/DialogLayer(Clone)/InLevelQuestHintDialog", null),
        };

        // Enemy levels and HP bars and NPC quest icons are world-space canvases the game creates as they come into view, so
        // the scene's roots are scanned while the HUD is hidden. Each board gets a CanvasGroup of its own, which the game
        // never writes.
        private static readonly Dictionary<IntPtr, CanvasGroup> boards = new();
        private static float nextBoardScan;

        public static void Update(bool hidden)
        {
            foreach (var page in Pages)
                page.Update(hidden);
            UpdateBoards(hidden);
        }

        // Damage numbers sit inside the HUD but have their own switch in the World tab.
        private static void KeepDamageNumbers(GameObject page)
        {
            var damage = page.transform.Find("ParticleDamageTextContainer");
            if (damage)
                OwnGroup(damage.gameObject).ignoreParentGroups = true;
        }

        private static void UpdateBoards(bool hidden)
        {
            if (!hidden)
            {
                foreach (var group in boards.Values)
                    if (group)
                        group.alpha = 1f;
                boards.Clear();
                return;
            }
            if (Time.unscaledTime < nextBoardScan)
                return;
            nextBoardScan = Time.unscaledTime + BoardScanInterval;
            foreach (var root in SceneManager.GetActiveScene().GetRootGameObjects())
            {
                if (root.name != BoardRoot)
                    continue;
                var canvas = root.transform.Find("Canvas");
                if (!canvas)
                    continue;
                var group = OwnGroup(canvas.gameObject);
                group.alpha = 0f;
                boards[group.Pointer] = group;
            }
            if (boards.Count > 0)
                CameraTools.LogOnce($"HUD: hiding enemy and NPC boards ({boards.Count} at first).");
        }

        private static CanvasGroup OwnGroup(GameObject target)
        {
            var group = target.GetComponent<CanvasGroup>();
            return group ? group : target.AddComponent<CanvasGroup>();
        }

        private sealed class Faded
        {
            private readonly string path;
            private readonly Action<GameObject> found;
            private CanvasGroup group;
            private (float Alpha, bool Raycasts)? saved;
            private float nextLook;

            public Faded(string path, Action<GameObject> found)
            {
                this.path = path;
                this.found = found;
            }

            public void Update(bool hidden)
            {
                if (!group)
                {
                    saved = null;
                    if (!hidden || Time.unscaledTime < nextLook)
                        return;
                    nextLook = Time.unscaledTime + LookInterval;
                    var target = GameObject.Find(path);
                    group = target ? target.GetComponent<CanvasGroup>() : null;
                    if (!group)
                        return;
                    CameraTools.LogOnce($"HUD: hiding {path}.");
                    found?.Invoke(target);
                }
                if (hidden)
                {
                    saved ??= (group.alpha, group.blocksRaycasts);
                    group.alpha = 0f;
                    group.blocksRaycasts = false;
                }
                else if (saved is var (alpha, raycasts))
                {
                    group.alpha = alpha;
                    group.blocksRaycasts = raycasts;
                    saved = null;
                }
            }
        }
    }
}
