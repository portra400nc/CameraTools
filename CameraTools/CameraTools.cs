using Cinemachine;
using MelonLoader;
using MoleMole;
using UnityEngine;

namespace CameraTools
{
    public class CameraTools : MelonMod
    {
        public static float lastTimeScale = 1.0f;
        public GameObject hud;
        public GameObject uid;
        public GameObject hp;
        public GameObject damage;
        public static GameObject camera;
        public static CinemachineBrain brain;
        public static Camera maincam;
        public static Camera cam;
        public static Freecam freecam;
        public static bool freecamActive;
        private static readonly Action<Camera> preCull = OnPreCull;
        private static readonly Action willRenderCanvases = OnWillRenderCanvases;
        private static bool callbacksRegistered;
        private static bool preCullSeen;
        private static readonly HashSet<string> logged = new();
        private static string toast;
        private static float toastUntil;

        public override void OnInitializeMelon()
        {
            Controls.Load();
        }

        public override void OnApplicationQuit()
        {
            Gamepad.StopRumble();
        }

        // Genshin's OnUpdate comes from a frame hook that appears to stop while the game speed is 0, and the game moves
        // the camera after it. OnLateUpdate comes from a real LateUpdate, which Unity calls every frame.
        public override void OnLateUpdate()
        {
            Gamepad.Poll();
            if (Controls.OwnerSwitchPressed)
                SetPadOwner(Controls.Owner == PadOwner.Game ? PadOwner.CameraTools : PadOwner.Game);
            // A pad that disconnects while CameraTools owns it would leave the game's player input switched off.
            if (Controls.Owner == PadOwner.CameraTools && !Gamepad.Connected)
                SetPadOwner(PadOwner.Game);
            if (Controls.Pressed(CamAction.Inject))
            {
                if (camera)
                    LoggerInstance.Msg("Free camera is already injected.");
                else
                    InjectFreecam();
            }
            if (Controls.Pressed(CamAction.SetResolutionTo4K) && maincam)
            {
                Screen.SetResolution(3840, 2160, false);
                maincam.rect = new Rect(0, 0, 3840, 2160);
            }
            if (Controls.Pressed(CamAction.SetResolutionTo1080p) && maincam)
            {
                Screen.SetResolution(1920, 1080, false);
                maincam.rect = new Rect(0, 0, 1920, 1080);
            }
            if (Controls.Pressed(CamAction.ToggleHUD))
            {
                if (hud)
                    hud.SetActive(!hud.activeInHierarchy);
                if (uid)
                    uid.SetActive(!uid.activeInHierarchy);
            }
            if (Controls.Pressed(CamAction.RemoveHP))
            {
                RemoveHP();
            }
            if (Controls.Pressed(CamAction.ToggleDamage))
            {
                ToggleDamage();
            }
            if (Controls.Pressed(CamAction.ToggleFreecam))
            {
                SetFreecam(!freecamActive);
            }
            if (Controls.Pressed(CamAction.ToggleMaxDetail))
            {
                Lod.SetMaxDetail(!Lod.MaxDetail);
            }
            if (Controls.Pressed(CamAction.TogglePause))
            {
                Time.timeScale = Time.timeScale != 0.0f ? 0.0f : lastTimeScale;
            }
            if (Controls.Pressed(CamAction.ResetSpeed))
            {
                Time.timeScale = 1.0f;
                lastTimeScale = Time.timeScale;
            }
            if (Controls.Pressed(CamAction.ToggleSpeedTo5))
            {
                Time.timeScale = Time.timeScale != 5.0f ? 5.0f : lastTimeScale;
            }
            if (Controls.Pressed(CamAction.SpeedInc1))
            {
                Time.timeScale += 0.1f;
                lastTimeScale = Time.timeScale;
            }
            if (Controls.Pressed(CamAction.SpeedDec1))
            {
                Time.timeScale -= 0.1f;
                lastTimeScale = Time.timeScale;
            }
            if (Controls.Pressed(CamAction.SpeedDec5))
            {
                Time.timeScale -= 0.5f;
                lastTimeScale = Time.timeScale;
            }
            if (Controls.Pressed(CamAction.SpeedInc5))
            {
                Time.timeScale += 0.5f;
                lastTimeScale = Time.timeScale;
            }
            if (Time.timeScale < 0)
                Time.timeScale = 0;

            Lod.Update();

            if (freecamActive && !camera)
            {
                freecamActive = false;
                Lod.Follow(false);
                GameCamera.Attach();
            }
            if (freecamActive)
            {
                GameCamera.Update(maincam);
                freecam.Update();
                freecam.LateUpdate();
            }
        }

        public override void OnGUI()
        {
            if (freecamActive)
                freecam.OnGUI();
            if (Time.unscaledTime < toastUntil)
                GUI.Box(new Rect(Screen.width / 2f - 120f, 40f, 240f, 28f), toast);
        }

        private void RemoveHP()
        {
            for (hp = GameObject.Find("AvatarBoardCanvasV2(Clone)"); hp; hp = GameObject.Find("AvatarBoardCanvasV2(Clone)"))
                hp.SetActive(false);
        }

        private void ToggleDamage()
        {
            if (damage)
            {
                damage.SetActive(!damage.activeInHierarchy);
            }
            else
            {
                damage = Find("/Canvas/Pages/InLevelMainPage/GrpMainPage/ParticleDamageTextContainer");
                if (damage)
                    damage.SetActive(!damage.activeInHierarchy);
            }
        }

        private void SetFreecam(bool active)
        {
            if (!camera)
            {
                LoggerInstance.Msg("Free camera not found. Please inject it by pressing F9.");
                return;
            }
            if (active == freecamActive)
                return;
            freecamActive = active;
            if (freecamActive)
                freecam.OnEnable();
            else
                freecam.OnDisable();
            // Cinemachine moves the game camera every frame; pausing it hands the camera to the free camera,
            // and resuming it puts the camera back where the game wants it.
            if (brain)
                brain.enabled = !freecamActive;
            Lod.Follow(freecamActive);
            if (freecamActive)
                GameCamera.Detach(maincam);
            else
                GameCamera.Attach();
        }

        // Taking the pad turns the free camera on; giving it back leaves the free camera where it is, so the
        // character can be walked through a fixed shot.
        private void SetPadOwner(PadOwner owner)
        {
            if (owner == PadOwner.CameraTools)
            {
                if (!camera)
                    InjectFreecam();
                if (camera)
                    SetFreecam(true);
            }
            Controls.Owner = owner;
            bool gameInput = owner == PadOwner.Game;
            try
            {
                ActorUtils.EnablePlayerInput(gameInput, false);
                LogOnce($"Controller: EnablePlayerInput({gameInput}, false) succeeded.");
            }
            catch (Exception e)
            {
                LogOnce($"Controller: EnablePlayerInput({gameInput}, false) failed: {e.Message}");
            }
            Gamepad.Rumble();
            toast = $"Controller: {owner}";
            toastUntil = Time.unscaledTime + 2f;
        }

        internal static void LogOnce(string message)
        {
            if (logged.Add(message))
                Melon<CameraTools>.Logger.Msg(message);
        }

        private void InjectFreecam()
        {
            hud = Find("/UICamera");
            uid = Find("/BetaWatermarkCanvas(Clone)/Panel");
            camera = Find("/EntityRoot/MainCamera(Clone)");
            if (!camera)
                return;
            // Genshin's renderer is tied to its own main camera, so a cloned camera renders grey. The free camera
            // drives the game camera instead.
            maincam = camera.GetComponent<Camera>();
            cam = maincam;
            Lod.Attach(maincam);
            brain = camera.GetComponent<CinemachineBrain>();
            if (!brain)
                LoggerInstance.Warning("The main camera has no CinemachineBrain; the game may keep moving the camera.");
            freecam = new Freecam(camera.transform);
            freecamActive = false;
            if (!callbacksRegistered)
            {
                Camera.CameraCallback callback = preCull;
                Camera.onPreCull = Camera.onPreCull == null ? callback : Camera.onPreCull + callback;
                callbacksRegistered = true;
                try
                {
                    Canvas.add_willRenderCanvases(willRenderCanvases);
                }
                catch (Exception e)
                {
                    LoggerInstance.Warning($"Canvas.willRenderCanvases could not be registered: {e.Message}");
                }
            }
            LoggerInstance.Msg("Free camera injected.");
        }

        private static void OnPreCull(Camera rendering)
        {
            if (!preCullSeen)
            {
                preCullSeen = true;
                Melon<CameraTools>.Logger.Msg("Camera pre-cull callback reached.");
            }
            if (freecamActive && camera)
                freecam.Apply();
        }

        // Genshin's camera system writes the gameplay pose after every script LateUpdate, and scenery culling reads the
        // camera before Camera.onPreCull. willRenderCanvases fires in PostLateUpdate, possibly ahead of those readers.
        private static void OnWillRenderCanvases()
        {
            if (!freecamActive || !camera)
                return;
            freecam.Apply();
        }

        // These paths date from an older game version; name any that no longer exist.
        private GameObject Find(string path)
        {
            GameObject found = GameObject.Find(path);
            if (!found)
                LoggerInstance.Warning($"{path} was not found in the current scene.");
            return found;
        }
    }
}
