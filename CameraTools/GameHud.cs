using UnityEngine;

namespace CameraTools
{
    // Genshin's HUD is InLevelMainPage/GrpMainPage. Fading its CanvasGroup, instead of turning the page off, keeps the
    // page's scripts running. The group's Animator writes the same values, so the fade is applied every frame and the
    // group's own values are put back when the HUD shows again. CameraTools' UI is not under the page.
    internal static class GameHud
    {
        private const string MainPage = "/Canvas/Pages/InLevelMainPage/GrpMainPage";
        private const float LookInterval = 1f;

        private static CanvasGroup group;
        private static (float Alpha, bool Raycasts)? saved;
        private static float nextLook;

        public static void Update(bool hidden)
        {
            if (!group)
            {
                saved = null;
                if (!hidden || Time.unscaledTime < nextLook)
                    return;
                nextLook = Time.unscaledTime + LookInterval;
                var page = GameObject.Find(MainPage);
                group = page ? page.GetComponent<CanvasGroup>() : null;
                if (!group)
                {
                    CameraTools.LogOnce($"HUD: {MainPage} or its CanvasGroup was not found; the game HUD stays visible.");
                    return;
                }
                // Damage numbers sit inside the HUD but have their own switch in the World tab.
                var damage = page.transform.Find("ParticleDamageTextContainer");
                if (damage)
                {
                    var own = damage.GetComponent<CanvasGroup>();
                    if (!own)
                        own = damage.gameObject.AddComponent<CanvasGroup>();
                    own.ignoreParentGroups = true;
                }
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
