using System;
using UnhollowerRuntimeLib;
using UnityEngine;

using static CameraTools.CameraTools;

namespace CameraTools
{
    public class Freecam : MonoBehaviour
    {
        public Freecam(IntPtr ptr) : base(ptr) { }
        public Freecam() : base(ClassInjector.DerivedConstructorPointer<Freecam>())
        {
            ClassInjector.DerivedConstructorBody(this);
        }
        public float accSprintMultiplier = 10;
        public float lookSensitivity = 1;
        public float fov = 0.5f;
        public float targetFov = 45f;
        public bool focusOnEnable = true;
        public bool panelVisible = false;
        public bool rememberPos = false;
        public Rect MenuRect = new Rect(20, (Screen.height - 600) / 2, 340, 20);

        private float translationSpeed = 0.5f;
        private float rollSpeed = 1f;
        private Vector3 targetPosition;
        private Vector3 smoothPosition;
        private Vector3 lastPosition;
        private Vector3 lastRotation;
        public float smoothFOV;
        private float smoothSpeed = 1.0f;

        private float lasttranslationSpeed;
        private float lastlookSensitivity;
        private float lastrollSpeed;
        private float lastfov;

        class CameraRotation
        {
            public float yaw, pitch, roll;

            public void InitializeFromTransform(Transform t)
            {
                pitch = t.eulerAngles.x;
                yaw = t.eulerAngles.y;
                roll = t.eulerAngles.z;
            }

            public void LerpTowards(CameraRotation target, float rotationLerpPct)
            {
                yaw = Mathf.Lerp(yaw, target.yaw, rotationLerpPct);
                pitch = Mathf.Lerp(pitch, target.pitch, rotationLerpPct);
                roll = Mathf.Lerp(roll, target.roll, rotationLerpPct);
            }

            public void UpdateTransform(Transform t)
            {
                t.eulerAngles = new Vector3(pitch, yaw, roll);
            }
        }
        CameraRotation targetRotation = new CameraRotation();
        CameraRotation currentRotation = new CameraRotation();
        static bool Focused
        {
            get => Cursor.lockState == CursorLockMode.Locked;
            set
            {
                Cursor.lockState = value ? CursorLockMode.Locked : CursorLockMode.None;
                Cursor.visible = value == false;
            }
        }
        public void OnGUI()
        {
            if (panelVisible == true)
            {
                MenuRect = GUILayout.Window(0, MenuRect, (GUI.WindowFunction)CameraWindow, "Camera Tools by portra", new GUILayoutOption[0]);
            }
        }
        public void CameraWindow(int id)
        {
            if (id == 0)
            {
                var buttonStyle = new GUILayoutOption[]
                {
                    GUILayout.Width(80)
                };
                var buttonStyle2 = new GUILayoutOption[]
                {
                    GUILayout.Width(50)
                };

                GUILayout.Space(20);

                // Movement Speed
                GUILayout.Label($"Movement Speed: {translationSpeed.ToString("F3")}", new GUILayoutOption[0]);
                translationSpeed = GUILayout.HorizontalSlider(translationSpeed, 0.001f, 10.0f, new GUILayoutOption[0]);
                GUILayout.BeginHorizontal(new GUILayoutOption[0]);
                if (GUILayout.Button("Reset", buttonStyle))
                    translationSpeed = 1.0f;
                if (GUILayout.Button("-", buttonStyle2))
                    translationSpeed -= 0.1f;
                if (GUILayout.Button("+", buttonStyle2))
                    translationSpeed += 0.1f;
                GUILayout.EndHorizontal();

                GUILayout.Space(20);

                // FOV speed
                GUILayout.Label($"FOV Speed: {fov.ToString("F2")}", new GUILayoutOption[0]);
                fov = GUILayout.HorizontalSlider(fov, 0.01f, 10.0f, new GUILayoutOption[0]);
                GUILayout.BeginHorizontal(new GUILayoutOption[0]);
                if (GUILayout.Button("Reset", buttonStyle))
                    fov = 1.0f;
                if (GUILayout.Button("-", buttonStyle2))
                    fov -= 0.1f;
                if (GUILayout.Button("+", buttonStyle2))
                    fov += 0.1f;
                GUILayout.EndHorizontal();

                GUILayout.Space(20);

                // Roll speed
                GUILayout.Label($"Roll Speed: {rollSpeed.ToString("F1")}", new GUILayoutOption[0]);
                rollSpeed = GUILayout.HorizontalSlider(rollSpeed, 0.1f, 10.0f, new GUILayoutOption[0]);
                GUILayout.BeginHorizontal(new GUILayoutOption[0]);
                if (GUILayout.Button("Reset", buttonStyle))
                    rollSpeed = 1.0f;
                if (GUILayout.Button("-", buttonStyle2))
                    rollSpeed -= 0.1f;
                if (GUILayout.Button("+", buttonStyle2))
                    rollSpeed += 0.1f;
                GUILayout.EndHorizontal();

                GUILayout.Space(20);

                // Mouse sensitivity
                GUILayout.Label($"Mouse Sensitivity: {lookSensitivity.ToString("F1")}", new GUILayoutOption[0]);
                lookSensitivity = GUILayout.HorizontalSlider(lookSensitivity, 0.1f, 10.0f, new GUILayoutOption[0]);
                GUILayout.BeginHorizontal(new GUILayoutOption[0]);
                if (GUILayout.Button("Reset", buttonStyle))
                    lookSensitivity = 1.0f;
                if (GUILayout.Button("-", buttonStyle2))
                    lookSensitivity -= 0.1f;
                if (GUILayout.Button("+", buttonStyle2))
                    lookSensitivity += 0.1f;
                GUILayout.EndHorizontal();

                GUILayout.Space(20);

                // Game speed
                GUILayout.Label($"Game Speed: {Time.timeScale.ToString("F2")}", new GUILayoutOption[0]);
                Time.timeScale = GUILayout.HorizontalSlider(Time.timeScale, 0.0f, 10.0f, new GUILayoutOption[0]);
                GUILayout.BeginHorizontal(new GUILayoutOption[0]);
                if (GUILayout.Button("Reset", buttonStyle))
                    Time.timeScale = 1.0f;
                if (GUILayout.Button("-", buttonStyle2))
                    Time.timeScale -= 1f;
                if (GUILayout.Button("+", buttonStyle2))
                    Time.timeScale += 1f;
                GUILayout.EndHorizontal();

                GUILayout.Space(20);

                // Movement smoothness
                GUILayout.Label($"Camera Damping: {smoothSpeed.ToString("F2")}", new GUILayoutOption[0]);
                smoothSpeed = GUILayout.HorizontalSlider(smoothSpeed, 0.01f, 1f, new GUILayoutOption[0]);
                GUILayout.BeginHorizontal(new GUILayoutOption[0]);
                if (GUILayout.Button("Reset", buttonStyle))
                    smoothSpeed = 1.0f;
                if (GUILayout.Button("-", buttonStyle2))
                    smoothSpeed -= 0.05f;
                if (GUILayout.Button("+", buttonStyle2))
                    smoothSpeed += 0.05f;
                GUILayout.EndHorizontal();

                GUILayout.Space(20);

                // FOV
                GUILayout.Label($"Field of View: {targetFov.ToString("F3")}", new GUILayoutOption[0]);
                targetFov = GUILayout.HorizontalSlider(targetFov, 1f, 160f, new GUILayoutOption[0]);
                GUILayout.BeginHorizontal(new GUILayoutOption[0]);
                if (GUILayout.Button("Reset", buttonStyle))
                    targetFov = 45.0f;
                if (GUILayout.Button("-", buttonStyle2))
                    targetFov -= 1f;
                if (GUILayout.Button("+", buttonStyle2))
                    targetFov += 1f;
                GUILayout.EndHorizontal();

                GUILayout.Space(20);

                rememberPos = GUILayout.Toggle(rememberPos, "Remember last position", new GUILayoutOption[0]);

                GUILayout.Space(20);
            }
            GUI.DragWindow();
        }
        public void OnEnable()
        {
            targetRotation.InitializeFromTransform(transform);
            currentRotation.InitializeFromTransform(transform);
            if (focusOnEnable) Focused = true;
            cam.CopyFrom(maincam);
            if (rememberPos)
            {
                transform.eulerAngles = lastRotation;
                transform.position = lastPosition;
            }
            else
            {
                targetRotation.pitch = maincam.transform.eulerAngles.x;
                targetRotation.yaw = maincam.transform.eulerAngles.y;
                targetRotation.roll = maincam.transform.eulerAngles.z;
            }
            targetPosition = transform.position;
        }

        public void OnDisable()
        {
            lastPosition = transform.position;
            lastRotation = transform.eulerAngles;
        }

        public void Update()
        {
            // Input
            if (Focused)
                UpdateInput();
            if (Input.GetKeyDown(keyFocus))
            {
                if (Focused == false)
                    Focused = true;
                else Focused = false;
            }
            if (Input.GetKeyDown(keyGUI))
            {
                if (panelVisible == true)
                {
                    panelVisible = false;
                    Focused = true;
                }
                else
                {
                    panelVisible = true;
                    Focused = false;
                }
            }

            // Limiters
            if (smoothSpeed < 0.01f)
                smoothSpeed = 0.01f;
            if (translationSpeed < 0.001f)
                translationSpeed = 0.001f;
            if (targetFov < 1f)
                targetFov = 1f;
            if (targetFov > 160f)
                targetFov = 160f;
            if (fov < 0.01f)
                fov = 0.01f;
            if (rollSpeed < 0.1f)
                rollSpeed = 0.1f;
            if (lookSensitivity < 0.1f)
                lookSensitivity = 0.1f;
            if (Time.timeScale < 0f)
                Time.timeScale = 0f;
        }

        public void UpdateInput()
        {
            // Update the target rotation based on mouse input
            var mouseInput = new Vector2(Input.GetAxis("Mouse X"), Input.GetAxis("Mouse Y") * -1);
            targetRotation.yaw += mouseInput.x * lookSensitivity;
            targetRotation.pitch += mouseInput.y * lookSensitivity;

            // Commit the rotation changes to the transform
            currentRotation.UpdateTransform(transform);

            // Roll camera
            if (Input.GetKey(keyRollLeft))
                targetRotation.roll += rollSpeed;
            if (Input.GetKey(keyRollRight))
                targetRotation.roll -= rollSpeed;
            if (Input.GetKey(keyRollReset))
                targetRotation.roll = 0;

            // Lateral Movement
            if (Input.GetKey(keyForward))
                targetPosition += transform.forward * translationSpeed;
            if (Input.GetKey(keyLeft))
                targetPosition -= transform.right * translationSpeed;
            if (Input.GetKey(keyBack))
                targetPosition -= transform.forward * translationSpeed;
            if (Input.GetKey(keyRight))
                targetPosition += transform.right * translationSpeed;

            // Vertical movement
            if (Input.GetKey(keyUp))
                targetPosition += transform.up * translationSpeed;
            if (Input.GetKey(keyDown))
                targetPosition -= transform.up * translationSpeed;

            // FOV
            if (Input.GetKey(keyFovDec))
                targetFov -= fov;
            if (Input.GetKey(keyFovInc))
                targetFov += fov;
            if (Input.GetKeyDown(keyFovReset))
                targetFov = 45f;

            if (Input.GetKeyDown(keyFast))
            {
                lasttranslationSpeed = translationSpeed;
                lastrollSpeed = rollSpeed;
                lastlookSensitivity = lookSensitivity;
                lastfov = fov;
                translationSpeed *= accSprintMultiplier;
                rollSpeed *= accSprintMultiplier;
                lookSensitivity *= accSprintMultiplier;
                fov *= accSprintMultiplier;
            }
            else if (Input.GetKeyUp(keyFast))
            {
                translationSpeed = lasttranslationSpeed;
                rollSpeed = lastrollSpeed;
                lookSensitivity = lastlookSensitivity;
                fov = lastfov;
            }
            if (Input.GetKeyDown(keySlow))
            {
                lasttranslationSpeed = translationSpeed;
                lastrollSpeed = rollSpeed;
                lastlookSensitivity = lookSensitivity;
                lastfov = fov;
                translationSpeed /= accSprintMultiplier;
                rollSpeed /= accSprintMultiplier;
                lookSensitivity /= accSprintMultiplier;
                fov /= accSprintMultiplier;
            }
            else if (Input.GetKeyUp(keySlow))
            {
                translationSpeed = lasttranslationSpeed;
                rollSpeed = lastrollSpeed;
                lookSensitivity = lastlookSensitivity;
                fov = lastfov;
            }
        }

        public void LateUpdate()
        {
            smoothPosition = Vector3.Lerp(transform.position, targetPosition, smoothSpeed);
            transform.position = smoothPosition;
            smoothFOV = Mathf.Lerp(cam.fieldOfView, targetFov, smoothSpeed);
            cam.fieldOfView = smoothFOV;
            currentRotation.LerpTowards(targetRotation, smoothSpeed);
        }
    }
}
