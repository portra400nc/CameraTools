using Cinemachine;
using MelonLoader;
using UnhollowerRuntimeLib;
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
        public static GameObject freecamera;
        public static Camera maincam;
        public static Camera cam;

        // Main
        public static KeyCode keyInject;
        public static KeyCode keyFreecam;
        public static KeyCode keyRes4k;
        public static KeyCode keyRes1080p;
        public static KeyCode keyHUD;
        public static KeyCode keyHP;
        public static KeyCode keyDamage;
        public static KeyCode keyTimeAdd1;
        public static KeyCode keyTimeAdd5;
        public static KeyCode keyTimeSub1;
        public static KeyCode keyTimeSub5;
        public static KeyCode keyTimeToggle5;
        public static KeyCode keyTimePause;
        public static KeyCode keyTimeReset;

        // Freecam
        public static KeyCode keyGUI;
        public static KeyCode keyFocus;
        public static KeyCode keyRollLeft;
        public static KeyCode keyRollRight;
        public static KeyCode keyRollReset;
        public static KeyCode keyForward;
        public static KeyCode keyBack;
        public static KeyCode keyLeft;
        public static KeyCode keyRight;
        public static KeyCode keyUp;
        public static KeyCode keyDown;
        public static KeyCode keyFovInc;
        public static KeyCode keyFovDec;
        public static KeyCode keyFovReset;
        public static KeyCode keyFast;
        public static KeyCode keySlow;

        public static MelonPreferences_Category cameraToolsHotkeys;
        public static MelonPreferences_Entry<KeyCode> keyFreecampref;
        public static MelonPreferences_Entry<KeyCode> keyInjectpref;
        public static MelonPreferences_Entry<KeyCode> keyRes4kpref;
        public static MelonPreferences_Entry<KeyCode> keyRes1080ppref;
        public static MelonPreferences_Entry<KeyCode> keyHUDpref;
        public static MelonPreferences_Entry<KeyCode> keyHPpref;
        public static MelonPreferences_Entry<KeyCode> keyDamagepref;
        public static MelonPreferences_Entry<KeyCode> keyTimeAdd1pref;
        public static MelonPreferences_Entry<KeyCode> keyTimeAdd5pref;
        public static MelonPreferences_Entry<KeyCode> keyTimeSub1pref;
        public static MelonPreferences_Entry<KeyCode> keyTimeSub5pref;
        public static MelonPreferences_Entry<KeyCode> keyTimeToggle5pref;
        public static MelonPreferences_Entry<KeyCode> keyTimePausepref;
        public static MelonPreferences_Entry<KeyCode> keyTimeResetpref;

        public static MelonPreferences_Entry<KeyCode> keyGUIpref;
        public static MelonPreferences_Entry<KeyCode> keyFocuspref;
        public static MelonPreferences_Entry<KeyCode> keyRollLeftpref;
        public static MelonPreferences_Entry<KeyCode> keyRollRightpref;
        public static MelonPreferences_Entry<KeyCode> keyRollResetpref;
        public static MelonPreferences_Entry<KeyCode> keyForwardpref;
        public static MelonPreferences_Entry<KeyCode> keyBackpref;
        public static MelonPreferences_Entry<KeyCode> keyLeftpref;
        public static MelonPreferences_Entry<KeyCode> keyRightpref;
        public static MelonPreferences_Entry<KeyCode> keyUppref;
        public static MelonPreferences_Entry<KeyCode> keyDownpref;
        public static MelonPreferences_Entry<KeyCode> keyFovIncpref;
        public static MelonPreferences_Entry<KeyCode> keyFovDecpref;
        public static MelonPreferences_Entry<KeyCode> keyFovResetpref;
        public static MelonPreferences_Entry<KeyCode> keyFastpref;
        public static MelonPreferences_Entry<KeyCode> keySlowpref;
        public override void OnApplicationStart()
        {
            ClassInjector.RegisterTypeInIl2Cpp<Freecam>();

            cameraToolsHotkeys = MelonPreferences.CreateCategory("CameraTools");
            keyInjectpref = cameraToolsHotkeys.CreateEntry("Inject", KeyCode.F9);
            keyInject = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "Inject");
            keyFreecampref = cameraToolsHotkeys.CreateEntry("ToggleFreecam", KeyCode.Insert);
            keyFreecam = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "ToggleFreecam");
            keyRes4kpref = cameraToolsHotkeys.CreateEntry("SetResolutionTo4K", KeyCode.Equals);
            keyRes4k = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "SetResolutionTo4K");
            keyRes1080ppref = cameraToolsHotkeys.CreateEntry("SetResolutionTo1080p", KeyCode.Minus);
            keyRes1080p = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "SetResolutionTo1080p");
            keyHUDpref = cameraToolsHotkeys.CreateEntry("ToggleHUD", KeyCode.PageDown);
            keyHUD = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "ToggleHUD");
            keyHPpref = cameraToolsHotkeys.CreateEntry("RemoveHP", KeyCode.LeftBracket);
            keyHP = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "RemoveHP");
            keyDamagepref = cameraToolsHotkeys.CreateEntry("ToggleDamage", KeyCode.RightBracket);
            keyDamage = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "ToggleDamage");
            keyTimeAdd1pref = cameraToolsHotkeys.CreateEntry("SpeedInc1", KeyCode.UpArrow);
            keyTimeAdd1 = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "SpeedInc1");
            keyTimeAdd5pref = cameraToolsHotkeys.CreateEntry("SpeedInc5", KeyCode.RightArrow);
            keyTimeAdd5 = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "SpeedInc5");
            keyTimeSub1pref = cameraToolsHotkeys.CreateEntry("SpeedDec1", KeyCode.DownArrow);
            keyTimeSub1 = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "SpeedDec1");
            keyTimeSub5pref = cameraToolsHotkeys.CreateEntry("SpeedDec5", KeyCode.LeftArrow);
            keyTimeSub5 = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "SpeedDec5");
            keyTimeToggle5pref = cameraToolsHotkeys.CreateEntry("ToggleSpeedTo5", KeyCode.CapsLock);
            keyTimeToggle5 = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "ToggleSpeedTo5");
            keyTimePausepref = cameraToolsHotkeys.CreateEntry("TogglePause", KeyCode.Delete);
            keyTimePause = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "TogglePause");
            keyTimeResetpref = cameraToolsHotkeys.CreateEntry("ResetSpeed", KeyCode.End);
            keyTimeReset = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "ResetSpeed");
            keyGUIpref = cameraToolsHotkeys.CreateEntry("ToggleGUI", KeyCode.F10);
            keyGUI = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "ToggleGUI");
            keyFocuspref = cameraToolsHotkeys.CreateEntry("ToggleCursorFocus", KeyCode.Quote);
            keyFocus = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "ToggleCursorFocus");
            keyRollLeftpref = cameraToolsHotkeys.CreateEntry("RollLeft", KeyCode.Comma);
            keyRollLeft = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "RollLeft");
            keyRollRightpref = cameraToolsHotkeys.CreateEntry("RollRight", KeyCode.Period);
            keyRollRight = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "RollRight");
            keyRollResetpref = cameraToolsHotkeys.CreateEntry("ResetRoll", KeyCode.RightShift);
            keyRollReset = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "ResetRoll");
            keyForwardpref = cameraToolsHotkeys.CreateEntry("Forward", KeyCode.I);
            keyForward = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "Forward");
            keyBackpref = cameraToolsHotkeys.CreateEntry("Back", KeyCode.K);
            keyBack = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "Back");
            keyLeftpref = cameraToolsHotkeys.CreateEntry("Left", KeyCode.J);
            keyLeft = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "Left");
            keyRightpref = cameraToolsHotkeys.CreateEntry("Right", KeyCode.L);
            keyRight = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "Right");
            keyUppref = cameraToolsHotkeys.CreateEntry("Up", KeyCode.O);
            keyUp = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "Up");
            keyDownpref = cameraToolsHotkeys.CreateEntry("Down", KeyCode.U);
            keyDown = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "Down");
            keyFovIncpref = cameraToolsHotkeys.CreateEntry("IncreaseFOV", KeyCode.Alpha9);
            keyFovInc = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "IncreaseFOV");
            keyFovDecpref = cameraToolsHotkeys.CreateEntry("DecreaseFOV", KeyCode.Alpha8);
            keyFovDec = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "DecreaseFOV");
            keyFovResetpref = cameraToolsHotkeys.CreateEntry("ResetFOV", KeyCode.Alpha0);
            keyFovReset = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "ResetFOV");
            keyFastpref = cameraToolsHotkeys.CreateEntry("FastMovement", KeyCode.RightAlt);
            keyFast = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "FastMovement");
            keySlowpref = cameraToolsHotkeys.CreateEntry("SlowMovement", KeyCode.Semicolon);
            keySlow = MelonPreferences.GetEntryValue<KeyCode>("CameraTools", "SlowMovement");

            MelonPreferences.Save();
        }
        public override void OnUpdate()
        {
            if (Input.GetKeyDown(keyInject))
            {
                if (freecamera)
                    LoggerInstance.Msg("Free camera is already injected.");
                else
                    InjectFreecam();
            }
            if (Input.GetKeyDown(keyRes4k))
            {
                Screen.SetResolution(3840, 2160, false);
                maincam.rect = new Rect(0, 0, 3840, 2160);
            }
            if (Input.GetKeyDown(keyRes1080p))
            {
                Screen.SetResolution(1920, 1080, false);
                maincam.rect = new Rect(0, 0, 1920, 1080);
            }
            if (Input.GetKeyDown(keyHUD))
            {
                hud.SetActive(!hud.activeInHierarchy);
                uid.SetActive(!uid.activeInHierarchy);
            }
            if (Input.GetKeyDown(keyHP))
            {
                RemoveHP();
            }
            if (Input.GetKeyDown(keyDamage))
            {
                ToggleDamage();
            }
            if (Input.GetKeyDown(keyFreecam))
            {
                ToggleFreecam();
            }
            if (Input.GetKeyDown(keyTimePause))
            {
                Time.timeScale = Time.timeScale != 0.0f ? 0.0f : lastTimeScale;
            }
            if (Input.GetKeyDown(keyTimeReset))
            {
                Time.timeScale = 1.0f;
                lastTimeScale = Time.timeScale;
            }
            if (Input.GetKeyDown(keyTimeToggle5))
            {
                Time.timeScale = Time.timeScale != 5.0f ? 5.0f : lastTimeScale;
            }
            if (Input.GetKeyDown(keyTimeAdd1))
            {
                Time.timeScale += 0.1f;
                lastTimeScale = Time.timeScale;
            }
            if (Input.GetKeyDown(keyTimeSub1))
            {
                Time.timeScale -= 0.1f;
                lastTimeScale = Time.timeScale;
            }
            if (Input.GetKeyDown(keyTimeSub5))
            {
                Time.timeScale -= 0.5f;
                lastTimeScale = Time.timeScale;
            }
            if (Input.GetKeyDown(keyTimeAdd5))
            {
                Time.timeScale += 0.5f;
                lastTimeScale = Time.timeScale;
            }
            if (Time.timeScale < 0)
                Time.timeScale = 0;
        }

        private void RemoveHP()
        {
            hp = GameObject.Find("AvatarBoardCanvasV2(Clone)");
            do
            {
                hp.SetActive(false);
                hp = GameObject.Find("AvatarBoardCanvasV2(Clone)");
            } while (hp != null);
        }

        private void ToggleDamage()
        {
            if (damage)
            {
                damage.SetActive(!damage.activeInHierarchy);
            }
            else
            {
                damage = GameObject.Find("/Canvas/Pages/InLevelMainPage/GrpMainPage/ParticleDamageTextContainer");
                damage.SetActive(!damage.activeInHierarchy);
            }
        }

        private void ToggleFreecam()
        {
            if (freecamera)
            {
                if (camera.activeInHierarchy)
                {
                    freecamera.SetActive(true);
                    camera.SetActive(false);
                }
                else
                {
                    camera.SetActive(true);
                    freecamera.SetActive(false);
                }
            }
            else
            {
                LoggerInstance.Msg("Free camera not found. Please inject it by pressing F9.");
            }
        }

        private void InjectFreecam()
        {
            hud = GameObject.Find("/UICamera");
            uid = GameObject.Find("/BetaWatermarkCanvas(Clone)/Panel");
            camera = GameObject.Find("/EntityRoot/MainCamera(Clone)");
            maincam = GameObject.Find("/EntityRoot/MainCamera(Clone)").GetComponent<Camera>();
            freecamera = Object.Instantiate(camera);
            camera.SetActive(false);
            camera.SetActive(true);
            cam = freecamera.GetComponent<Camera>();
            freecamera.AddComponent<Freecam>();
            Object.Destroy(freecamera.GetComponent<CinemachineBrain>());
            Object.Destroy(freecamera.GetComponent<CinemachineExternalCamera>());
            freecamera.SetActive(false);
            LoggerInstance.Msg("Free camera injected.");
        }
    }
}
