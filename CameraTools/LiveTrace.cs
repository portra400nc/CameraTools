using CameraToolsEnhancements;
using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // Path tracing while the free camera flies through a paused world: the scene is captured once, and the add-on traces
    // it frame after frame, carrying its samples over each move of the camera, so the traced light settles wherever the
    // camera stops. It ends when the game runs again, the free camera is off, the add-on goes, or a screenshot starts.
    internal static class LiveTrace
    {
        // GPU time a frame for the rays, so the camera still answers between frames.
        public const float BudgetMs = 60f;

        public static bool On { get; private set; }

        private static uint asked;

        // Starts or ends live tracing. Starting needs the free camera, the add-on and a paused game.
        public static void Toggle()
        {
            if (On)
            {
                Stop("turned off");
                return;
            }
            string why = !CameraTools.freecamActive ? "the free camera is off"
                : !Enhancements.Connected ? "the Enhancements add-on is not loaded"
                : Time.timeScale != 0f ? "the game is not paused"
                : Screenshot.State is not ShotState.Idle ? "a screenshot is being taken"
                : null;
            if (why != null)
            {
                Log($"not started: {why}.");
                return;
            }
            var eye = CameraTools.maincam ? CameraTools.maincam.transform.position : Vector3.zero;
            Enhancements.SetGameLights(EnhancementsShot.GameLightsNear(eye));
            asked = Enhancements.Status.TraceGeneration;
            if (!Enhancements.CaptureScene(new EnhancementsSceneRequest
                {
                    Faces = TracedShot.AllFaces, Settle = TracedShot.FaceSettle,
                    Flags = EnhancementsSceneFlags.Trace | EnhancementsSceneFlags.Light | EnhancementsSceneFlags.NoMeshes | EnhancementsSceneFlags.NoFiles,
                }))
            {
                Log("not started: the add-on is still busy with a capture.");
                return;
            }
            Enhancements.SetShot(true, 0, BudgetMs, EnhancementsShot.BounceStrength,
                EnhancementsShot.TracedSunShadows ? EnhancementsTraceOutputs.Ao | EnhancementsTraceOutputs.Sun : EnhancementsTraceOutputs.Ao,
                false, bounces: (uint)EnhancementsShot.Bounces, live: true);
            On = true;
            Log($"on: capturing the scene, then tracing at {BudgetMs:0} ms a frame while the camera moves.");
        }

        // Every frame, before the screenshot's update.
        public static void Update()
        {
            if (!On)
                return;
            string why = !CameraTools.freecamActive ? "the free camera is off"
                : !Enhancements.Connected ? "the Enhancements add-on was unloaded"
                : Time.timeScale != 0f ? "the game was unpaused"
                : Screenshot.State is not ShotState.Idle ? "a screenshot started"
                : null;
            if (why != null)
                Stop(why);
        }

        private static void Stop(string why)
        {
            On = false;
            if (Enhancements.Connected)
            {
                Enhancements.SetShot(false);
                Enhancements.ReleaseScene();
            }
            Log($"off: {why}.");
        }

        private static void Log(string message) => Melon<CameraTools>.Logger.Msg($"Live trace: {message}");
    }
}
