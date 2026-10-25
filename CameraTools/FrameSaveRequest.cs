using CameraToolsEnhancements;
using MelonLoader;
using MelonLoader.Utils;
using UnityEngine;

namespace CameraTools
{
    // A developer's way to save a frame for the Mac's depth of field harness without any UI: creating the file
    // UserData/CameraTools/save-frame.request (over SSH, say) makes the add-on save the next frame's HDR scene, depth and
    // camera to <ReShade base>\Enhancements\frame-<time>\, and the file is deleted.
    internal static class FrameSaveRequest
    {
        private const float PollSeconds = 1f;
        private static readonly string Request = Path.Combine(MelonEnvironment.UserDataDirectory, "CameraTools", "save-frame.request");
        private static float nextPoll;

        public static void Update()
        {
            if (Time.unscaledTime < nextPoll)
                return;
            nextPoll = Time.unscaledTime + PollSeconds;
            if (!File.Exists(Request))
                return;
            try
            {
                File.Delete(Request);
            }
            catch (IOException)
            {
                return;
            }
            bool saving = Enhancements.SaveFrame();
            Melon<CameraTools>.Logger.Msg(saving ? "Frame save: requested, the add-on saves the next frame." : "Frame save: the add-on refused (not loaded, or a save is still waiting).");
        }
    }
}
