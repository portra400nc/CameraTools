using UnityEngine;

namespace CameraTools
{
    // The game's rain, hidden while ours falls: the renderers of the weather's rain effects under EffectPool, such as
    // Eff_Weather_Rain_Light/Rain_Particle, a particle system drawn with miHoYo/Enviro/RainDrop. The rings rain makes on
    // water (Eff_WaterRipple_*) stay. The pool hands its effects out again, so they are looked for twice a second.
    internal static class GameRain
    {
        private static readonly string[] Effects = { "Eff_Weather_Rain", "Eff_Weather_Storm", "Eff_Weather_Thunder" };
        private static readonly List<Renderer> hidden = new();
        private static float nextLook;

        public static void Update(bool hide)
        {
            if (!hide)
            {
                Restore();
                return;
            }
            if (Time.unscaledTime < nextLook)
                return;
            nextLook = Time.unscaledTime + 0.5f;
            var pool = GameObject.Find("EffectPool");
            if (pool == null)
                return;
            var root = pool.transform;
            for (int i = 0; i < root.childCount; i++)
            {
                var effect = root.GetChild(i);
                if (!Effects.Any(name => effect.name.StartsWith(name, StringComparison.Ordinal)))
                    continue;
                foreach (var renderer in effect.GetComponentsInChildren<Renderer>(true))
                    if (renderer.enabled)
                    {
                        renderer.enabled = false;
                        hidden.Add(renderer);
                    }
            }
        }

        private static void Restore()
        {
            foreach (var renderer in hidden)
                if (renderer != null)
                    renderer.enabled = true;
            hidden.Clear();
        }
    }
}
