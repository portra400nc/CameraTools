using Cinemachine;
using MelonLoader;
using MoleMole;
using UnityEngine;

namespace CameraTools
{
    public class CameraTools : MelonMod
    {
        public static readonly CameraSettings settings = new();
        // The speed that unpausing returns to; it is never 0.
        public static float lastTimeScale = 1.0f;
        public static GameObject uid;
        // Hide UI clears the screen: the game HUD with the free camera off, CameraTools' UI with it on (the free camera
        // already hides the HUD), and the UID either way.
        public static bool uiHidden;
        public GameObject hp;
        public static GameObject damage;
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
        private static MelonPreferences_Entry<bool> pauseInFreecam;
        // Set while the game is paused because the free camera started, so leaving it resumes only that pause.
        private static bool freecamPaused;

        public static bool PauseInFreecam => pauseInFreecam.Value;

        public override void OnInitializeMelon()
        {
            Controls.Load();
            Graphics.Load();
            CameraPaths.Load();
            Lights.Load();
            Poses.Load();
            Screenshot.Load();
            DepthOfField.Load();
            FrameGuide.Load();
            pauseInFreecam = MelonPreferences.CreateCategory("CameraTools").CreateEntry("PauseInFreeCamera", false,
                description: "Pause the game when the free camera starts, and resume it when the free camera ends.");
            MelonPreferences.Save();
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
            Controls.Update();
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
            if (Controls.Pressed(CamAction.SetResolutionTo4K))
            {
                Graphics.ApplySlot(2);
            }
            if (Controls.Pressed(CamAction.SetResolutionTo1080p))
            {
                Graphics.ApplySlot(1);
            }
            if (Controls.Pressed(CamAction.ToggleHUD))
            {
                SetUiHidden(!uiHidden);
            }
            if (Controls.Pressed(CamAction.RemoveHP))
            {
                RemoveHP();
            }
            if (Controls.Pressed(CamAction.ToggleDamage))
            {
                DamageNumbers = !DamageNumbers;
            }
            if (Controls.Pressed(CamAction.ToggleFreecam))
            {
                SetFreecam(!freecamActive);
            }
            float speed = Time.timeScale;
            if (Controls.Pressed(CamAction.TogglePause))
            {
                SetPaused(Time.timeScale != 0.0f);
            }
            if (Controls.Pressed(CamAction.ResetSpeed))
            {
                SetGameSpeed(1.0f);
            }
            if (Controls.Pressed(CamAction.ToggleSpeedTo5))
            {
                Time.timeScale = Time.timeScale != 5.0f ? 5.0f : lastTimeScale;
            }
            if (Controls.Pressed(CamAction.SpeedInc1))
            {
                SetGameSpeed(Time.timeScale + 0.1f);
            }
            if (Controls.Pressed(CamAction.SpeedDec1))
            {
                SetGameSpeed(Time.timeScale - 0.1f);
            }
            if (Controls.Pressed(CamAction.SpeedDec5))
            {
                SetGameSpeed(Time.timeScale - 0.5f);
            }
            if (Controls.Pressed(CamAction.SpeedInc5))
            {
                SetGameSpeed(Time.timeScale + 0.5f);
            }
            if (Time.timeScale != speed)
                CameraUi.Toast(Time.timeScale == 0.0f ? "Paused" : $"Game speed {Time.timeScale:0.##}x");
            // After the speed notice, so unpausing the game for playback does not show one.
            PathPlayback.Update();
            ReShade.Update();
            DepthOfField.Update();

            // After the hotkeys, so the key that closes the settings panel does not also fire its camera action.
            CameraUi.Update();
            // After the UI, so the B press that cancels a screenshot does not also close the panel that cancelling reopens.
            Screenshot.Update();
            GameHud.Update(freecamActive || uiHidden);
            Lod.Update();
            Graphics.Update();
            TimeOfDay.Update();
            Weather.Update();

            if (freecamActive && !camera)
            {
                freecamActive = false;
                Lod.Follow(false);
                GameCamera.Attach();
            }
            if (freecamActive)
            {
                GameCamera.Update(maincam);
                // Before playback poses the camera, which already accounts for the current shift.
                freecam.FollowWorldShift();
                // While the sticks move a light or turn a joint, the camera stays put.
                if (!PathPlayback.Drive(freecam) && !Lights.Moving && !Posing.Editing)
                    freecam.Update();
                freecam.LateUpdate();
            }
            Lights.Update();
            Posing.Update();
        }

        public override void OnGUI()
        {
            string toast = CameraUi.FallbackToast;
            if (toast != null)
                GUI.Box(new Rect(Screen.width / 2f - 180f, 40f, 360f, 28f), toast);
        }

        internal static void SetUiHidden(bool hidden)
        {
            uiHidden = hidden;
            if (!uid)
                uid = Find("/BetaWatermarkCanvas(Clone)/Panel");
            if (uid)
                uid.SetActive(!hidden);
        }

        internal static void SetGameSpeed(float speed)
        {
            Time.timeScale = Math.Max(speed, 0.0f);
            if (Time.timeScale > 0.0f)
                lastTimeScale = Time.timeScale;
        }

        internal static void SetPaused(bool paused)
        {
            Time.timeScale = paused ? 0.0f : lastTimeScale;
        }

        internal static void SetPauseInFreecam(bool on)
        {
            if (pauseInFreecam.Value == on)
                return;
            pauseInFreecam.Value = on;
            MelonPreferences.Save();
        }

        private void RemoveHP()
        {
            for (hp = GameObject.Find("AvatarBoardCanvasV2(Clone)"); hp; hp = GameObject.Find("AvatarBoardCanvasV2(Clone)"))
                hp.SetActive(false);
        }

        // Reads as shown until the container is found, because the game shows damage numbers by default.
        internal static bool DamageNumbers
        {
            get => !damage || damage.activeSelf;
            set
            {
                if (!damage)
                    damage = Find("/Canvas/Pages/InLevelMainPage/GrpMainPage/ParticleDamageTextContainer");
                if (damage)
                    damage.SetActive(value);
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
            CameraUi.FreecamChanged(freecamActive);
            if (freecamActive)
            {
                freecamPaused = PauseInFreecam && Time.timeScale != 0f;
                if (freecamPaused)
                    SetPaused(true);
            }
            else if (freecamPaused)
            {
                freecamPaused = false;
                if (Time.timeScale == 0f)
                    SetPaused(false);
            }
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
            SetPlayerInput(owner == PadOwner.Game, "Controller");
            Gamepad.Rumble();
            CameraUi.Toast($"Controller: {owner}");
        }

        internal static void SetPlayerInput(bool on, string why)
        {
            try
            {
                ActorUtils.EnablePlayerInput(on, false);
                LogOnce($"{why}: EnablePlayerInput({on}, false) succeeded.");
            }
            catch (Exception e)
            {
                LogOnce($"{why}: EnablePlayerInput({on}, false) failed: {e.Message}");
            }
        }

        internal static void LogOnce(string message)
        {
            if (logged.Add(message))
                Melon<CameraTools>.Logger.Msg(message);
        }

        private void InjectFreecam()
        {
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
            Posing.OnWillRenderCanvases();
        }

        // These paths date from an older game version; name any that no longer exist.
        private static GameObject Find(string path)
        {
            GameObject found = GameObject.Find(path);
            if (!found)
                Melon<CameraTools>.Logger.Warning($"{path} was not found in the current scene.");
            return found;
        }
    }
}
