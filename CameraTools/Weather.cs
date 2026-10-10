using MelonLoader;
using MoleMole;
using Climate = NEHLNJHKNOG;
using WeatherManager = NOIBMODMMPI;
using WeatherSource = NOIBMODMMPI.LCDLJEDCEFJ;

namespace CameraTools
{
    // The World tab's weather. It asks the game's weather manager for a preset under a source of its own, which only
    // changes rendering; the manager's gameplay climate, the world, and the server never see it. Not saved.
    internal static class Weather
    {
        private sealed record Option(string Label, Climate? Climate);

        // A preset asked for, and the manager and sky it was asked on.
        private sealed record Held(int Choice, string Preset, IntPtr Manager, IntPtr Sky);

        private static readonly Option[] Options =
        {
            new("Game", null),
            new("Sunny", Climate.CLIMATE_SUNNY),
            new("Cloudy", Climate.CLIMATE_CLOUDY),
            new("Rain", Climate.CLIMATE_RAIN),
            new("Thunderstorm", Climate.CLIMATE_THUNDERSTORM),
            new("Snow", Climate.CLIMATE_SNOW),
            new("Mist", Climate.CLIMATE_MIST),
        };

        public static readonly string[] Labels = Options.Select(option => option.Label).ToArray();

        // The game's sources run from Enviro (0) to Cutscene (6). The latest source added wins, so ours sits above the
        // server's weather, and quest or cutscene weather goes above ours and hands back to it after.
        private const WeatherSource Source = (WeatherSource)7;
        // The request's two floats read as a transition; what each does is still to be judged on the Deck.
        private const float TransitionFirst = 2f;
        private const float TransitionSecond = 0f;

        // Null while the game's weather shows.
        private static Held held;

        public static int Choice => held?.Choice ?? 0;

        // Whether the weather asked for rains, or null while the game's weather shows.
        public static bool? Raining => held == null ? null : Options[held.Choice].Climate is Climate.CLIMATE_RAIN or Climate.CLIMATE_THUNDERSTORM;

        public static void SetChoice(int index)
        {
            try
            {
                if (Options[index].Climate is Climate climate)
                    Choose(index, climate);
                else
                    Clear();
            }
            catch (Exception e)
            {
                Fail(e);
            }
        }

        // A teleport or a domain brings a new manager or sky, which has not seen our request.
        public static void Update()
        {
            if (held == null)
                return;
            try
            {
                var manager = Manager();
                var sky = EnviroSky.Instance;
                if (manager == null || sky == null || (manager.Pointer == held.Manager && sky.Pointer == held.Sky))
                    return;
                Log($"the weather manager or sky changed; asking for \"{held.Preset}\" again.");
                Request(manager, sky, held.Choice, held.Preset);
            }
            catch (Exception e)
            {
                Fail(e);
            }
        }

        private static void Choose(int index, Climate climate)
        {
            var manager = Manager();
            var sky = EnviroSky.Instance;
            if (manager == null || sky == null)
            {
                Log($"no weather manager or sky yet; {Options[index].Label} not applied.");
                return;
            }
            string preset = sky.Climate2WeatherName(climate, null);
            if (string.IsNullOrEmpty(preset))
            {
                Log($"no {Options[index].Label} weather in this area.");
                return;
            }
            Request(manager, sky, index, preset);
        }

        private static void Request(WeatherManager manager, EnviroSky sky, int index, string preset)
        {
            var before = manager.LCMAHHGNHBJ();
            bool accepted = manager.EBEIIKLMIAM(preset, Source, TransitionFirst, TransitionSecond, null, false);
            held = new Held(index, preset, manager.Pointer, sky.Pointer);
            Log($"asked for \"{preset}\" ({Options[index].Label}) under source {(int)Source} with ({TransitionFirst}, {TransitionSecond}); "
                + $"returned {accepted}; manager climate {before} -> {manager.LCMAHHGNHBJ()}; sky heading to \"{sky.targetWeatherPresetName}\".");
        }

        private static void Clear()
        {
            if (held == null)
                return;
            held = null;
            var manager = Manager();
            if (manager == null)
            {
                Log("back to the game's weather; no weather manager to clear.");
                return;
            }
            var before = manager.LCMAHHGNHBJ();
            bool cleared = manager.DJIJAFKONCH(Source, TransitionFirst);
            var sky = EnviroSky.Instance;
            Log($"cleared source {(int)Source} with ({TransitionFirst}); returned {cleared}; manager climate {before} -> {manager.LCMAHHGNHBJ()}; "
                + $"sky heading to \"{(sky != null ? sky.targetWeatherPresetName : "no sky")}\".");
        }

        // The stored instance, not Instance, which may create a second manager when there is none.
        private static WeatherManager Manager()
        {
            try
            {
                var manager = Singleton<WeatherManager>._instance;
                if (manager == null)
                    CameraTools.LogOnce("Weather: Singleton<NOIBMODMMPI>._instance is null; no weather manager yet.");
                return manager;
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Weather: Singleton<NOIBMODMMPI>._instance failed ({e.GetType().Name}: {e.Message}).");
                return null;
            }
        }

        // A failing game call drops back to the game's weather, so it is not repeated every frame.
        private static void Fail(Exception e)
        {
            held = null;
            Melon<CameraTools>.Logger.Warning($"Weather: a game call failed ({e.GetType().Name}: {e.Message}); back to the game's weather.");
        }

        private static void Log(string message) => Melon<CameraTools>.Logger.Msg($"Weather: {message}");
    }
}
