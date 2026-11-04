using System.Text.RegularExpressions;
using Il2CppInterop.Runtime;
using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // The game's hand-placed fake bounce: lights named as fill, bounce or GI, which a traced shot's real bounce replaces. Only
    // SetActive turns them off, as lamp scripts set enabled and intensity again every frame.
    internal static class FakeLights
    {
        // Lights that stand in for a real source stay: the signboard's (ZP, ZhaoPai) and a lamp's own spill.
        private const string Default = @"(?<!zp_)(?<!zhaopai_)(?<!lamp_)fill|bounce|(^|[ _])gi($|[ _])|(^|[ _])far($|[ _])|distant|(?<!zp_)(?<!zhaopai_)fog";
        private static readonly List<GameObject> held = new();

        // The rule, from UserData/CameraTools/fake-lights.txt's first line when it has one, so a test need not restart the game.
        private static Regex Rule()
        {
            string path = Path.Combine(MelonLoader.Utils.MelonEnvironment.UserDataDirectory, "CameraTools", "fake-lights.txt");
            string text = File.Exists(path) ? File.ReadLines(path).FirstOrDefault()?.Trim() : null;
            return new Regex(string.IsNullOrEmpty(text) ? Default : text, RegexOptions.IgnoreCase);
        }

        public static int Hold()
        {
            var rule = Rule();
            foreach (var o in UnityEngine.Object.FindObjectsOfType(Il2CppType.Of<Light>()))
            {
                var light = o.Cast<Light>();
                if (light.isActiveAndEnabled && !Lights.IsOurs(light) && rule.IsMatch(Regex.Replace(light.name, @"\s*\(\d+\)$", "")))
                {
                    held.Add(light.gameObject);
                    light.gameObject.SetActive(false);
                }
            }
            return held.Count;
        }

        public static void Release()
        {
            foreach (var o in held)
                if (o)
                    o.SetActive(true);
            held.Clear();
        }
    }
}
