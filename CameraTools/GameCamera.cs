using miHoYoCamera;
using UnityEngine;

namespace CameraTools
{
    // Genshin's camera system (CameraStateMgr.Flush) writes the gameplay pose onto its bound camera after every script
    // LateUpdate, and scenery streaming and culling read the main camera after that. While the free camera is on, the
    // camera system is bound to a hidden camera, so the main camera keeps the free camera's pose for the whole frame.
    internal static class GameCamera
    {
        private const float ResolveInterval = 2f;

        public static bool Enabled { get; set; } = true;

        // Binding null instead would leave Flush without a camera to write to every frame; the hidden camera takes the
        // gameplay pose and never renders.
        private static Camera hidden;
        private static CameraStateMgr detached;
        private static Camera restore;
        private static float nextResolve;

        public static void Detach(Camera main)
        {
            if (!Enabled)
            {
                CameraTools.LogOnce("Game camera: DetachGameCamera is off; the game camera stays on the main camera.");
                return;
            }
            try
            {
                if (!hidden)
                {
                    var owner = new GameObject("CameraTools GameCamera");
                    UnityEngine.Object.DontDestroyOnLoad(owner);
                    hidden = owner.AddComponent<Camera>();
                    hidden.enabled = false;
                }
                hidden.nearClipPlane = main.nearClipPlane;
                hidden.farClipPlane = main.farClipPlane;
                hidden.fieldOfView = main.fieldOfView;
                hidden.aspect = main.aspect;
                var from = main.transform;
                hidden.transform.SetPositionAndRotation(from.position, from.rotation);
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Game camera: creating the hidden camera failed: {e.Message}");
                return;
            }
            Converge(main);
        }

        public static void Update(Camera main)
        {
            if (Enabled && hidden && Time.unscaledTime >= nextResolve)
                Converge(main);
        }

        // A scene change or a new avatar can replace the state manager or rebind the main camera, so this runs again
        // every two seconds while the free camera is on.
        private static void Converge(Camera main)
        {
            nextResolve = Time.unscaledTime + ResolveInterval;
            try
            {
                if (hidden.enabled)
                {
                    hidden.enabled = false;
                    CameraTools.LogOnce("Game camera: the game enabled the hidden camera; disabled it again.");
                }
                var (manager, path) = Resolve();
                if (manager == null)
                {
                    CameraTools.LogOnce("Game camera: no CameraStateMgr found; the game camera stays on the main camera.");
                    return;
                }
                var bound = manager._camera;
                bool sameManager = detached != null && detached.Pointer == manager.Pointer;
                if (sameManager && bound && bound.Pointer == hidden.Pointer)
                    return;
                if (!bound || bound.Pointer != hidden.Pointer)
                {
                    restore = bound;
                    CameraTools.LogOnce(bound && main && bound.Pointer == main.Pointer
                        ? "Game camera: the camera system was bound to the main camera CameraTools found."
                        : $"Game camera: the camera system was bound to {(bound ? $"{bound.name} 0x{bound.Pointer:x}" : "null")}, "
                            + $"not the main camera CameraTools found ({(main ? $"0x{main.Pointer:x}" : "null")}).");
                }
                string was = detached == null ? "detached" : sameManager ? "rebound (the game bound another camera)" : "rebound (new CameraStateMgr)";
                manager.BindCamera(hidden);
                detached = manager;
                CameraTools.LogOnce($"Game camera: {was}; CameraStateMgr 0x{manager.Pointer:x} (via {path}) now drives the hidden camera.");
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Game camera: detach failed: {e.Message}");
            }
        }

        public static void Attach()
        {
            if (detached == null)
                return;
            var manager = detached;
            var target = restore ? restore : CameraTools.maincam;
            detached = null;
            restore = null;
            if (!target)
            {
                CameraTools.LogOnce("Game camera: attach skipped; the camera it was bound to and the main camera are both gone.");
                return;
            }
            try
            {
                manager.BindCamera(target);
                CameraTools.LogOnce($"Game camera: attached; CameraStateMgr 0x{manager.Pointer:x} drives {target.name} again.");
            }
            catch (Exception e)
            {
                CameraTools.LogOnce($"Game camera: attach failed: {e.Message}");
            }
        }

        private static (CameraStateMgr manager, string path) Resolve()
        {
            var controller = DFNJDCCAGDN.BOGHODCPFEP.OMOBMJMJIEC();
            string path = "OMOBMJMJIEC";
            if (controller == null)
            {
                controller = DFNJDCCAGDN.BOGHODCPFEP.FECLLEJFLLD()?.OFCNFCMCGHP;
                path = "FECLLEJFLLD.OFCNFCMCGHP";
            }
            return (controller?.JECIECJGGML, path);
        }
    }
}
