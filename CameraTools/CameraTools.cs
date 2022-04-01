using MelonLoader;
using UnityEngine;
using UnhollowerRuntimeLib;

namespace CameraTools
{
    public class CameraTools : MelonMod
    {

        public static float lastTimeScale = 1.0f;
        public GameObject hud;
        public GameObject uid;
        public GameObject hp;
        public GameObject camera;
        public GameObject freecamera;
        public GameObject damage;
        Camera maincam;

        public override void OnApplicationStart()
        {
            ClassInjector.RegisterTypeInIl2Cpp<Freecam>();
        }
        public override void OnUpdate()
        {
            if (Input.GetKeyDown(KeyCode.F9))
            {
                if (freecamera)
                {
                    LoggerInstance.Msg("Free camera is already injected.");
                }
                else
                {
                    hud = GameObject.Find("/UICamera");
                    uid = GameObject.Find("/BetaWatermarkCanvas(Clone)/Panel");
                    camera = GameObject.Find("/EntityRoot/MainCamera(Clone)");
                    maincam = GameObject.Find("/EntityRoot/MainCamera(Clone)").GetComponent<Camera>();
                    freecamera = GameObject.Instantiate(camera);
                    camera.SetActive(false);
                    camera.SetActive(true);
                    freecamera.AddComponent<Freecam>();
                    freecamera.SetActive(false);
                    LoggerInstance.Msg("Free camera injected.");
                }
            }
            if (Input.GetKeyDown(KeyCode.Equals))
            {
                Screen.SetResolution(3840, 2160, false);
                maincam.rect = new Rect(0, 0, 3840, 2160);
            }
            if (Input.GetKeyDown(KeyCode.Minus))
            {
                Screen.SetResolution(1920, 1080, false);
                maincam.rect = new Rect(0, 0, 1920, 1080);
            }
            if (Input.GetKeyDown(KeyCode.PageDown))
            {
                if (hud.activeInHierarchy == true)
                    hud.SetActive(false);
                else
                    hud.SetActive(true);
                if (uid.activeInHierarchy == true)
                    uid.SetActive(false);
                else
                    uid.SetActive(true);
                
            }
            if (Input.GetKeyDown(KeyCode.LeftBracket))
            {
                hp = GameObject.Find("AvatarBoardCanvasV2(Clone)");
                do
                {
                    hp.SetActive(false);
                    hp = GameObject.Find("AvatarBoardCanvasV2(Clone)");
                } while (hp != null);
            }
            if (Input.GetKeyDown(KeyCode.RightBracket))
            {
                if (damage)
                {
                    if (damage.activeInHierarchy == true)
                        damage.SetActive(false);
                    else
                        damage.SetActive(true);
                }
                else
                {
                    damage = GameObject.Find("/Canvas/Pages/InLevelMainPage/GrpMainPage/ParticleDamageTextContainer");
                    if (damage.activeInHierarchy == true)
                        damage.SetActive(false);
                    else
                        damage.SetActive(true);
                }
            }
            if (Input.GetKeyDown(KeyCode.Insert))
            {
                if (freecamera)
                {
                    if (camera.activeInHierarchy == true)
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
            if (Input.GetKeyDown(KeyCode.Delete))
            {
                if (Time.timeScale != 0.0f)
                    Time.timeScale = 0.0f;
                else
                    Time.timeScale = lastTimeScale;
            }
            if (Input.GetKeyDown(KeyCode.End))
            {
                Time.timeScale = 1.0f;
                lastTimeScale = Time.timeScale;
            }
            if (Input.GetKeyDown(KeyCode.CapsLock))
            {
                if (Time.timeScale != 5.0f)
                    Time.timeScale = 5.0f;
                else
                    Time.timeScale = lastTimeScale;
            }
            if (Input.GetKeyDown(KeyCode.UpArrow))
            {
                Time.timeScale += 0.1f;
                lastTimeScale = Time.timeScale;
            }
            if (Input.GetKeyDown(KeyCode.DownArrow))
            {
                Time.timeScale -= 0.1f;
                lastTimeScale = Time.timeScale;
            }
            if (Input.GetKeyDown(KeyCode.LeftArrow))
            {
                Time.timeScale -= 0.5f;
                lastTimeScale = Time.timeScale;
            }
            if (Input.GetKeyDown(KeyCode.RightArrow))
            {
                Time.timeScale += 0.5f;
                lastTimeScale = Time.timeScale;
            }
            if (Time.timeScale < 0)
                Time.timeScale = 0;
        }
    }
}
