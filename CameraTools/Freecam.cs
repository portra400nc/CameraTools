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

        private float speedModifier = 10;
        private float lookSensitivity = 1;
        private float fovSpeed = 0.5f;
        private float targetFov = 45f;
        private bool focusOnEnable = true;
        private bool panelVisible;
        private bool rememberPos;
        private Rect menuRect = new Rect(20, (Screen.height - 600) / 2, 340, 20);

        private float moveSpeed = 0.5f;
        private float rollSpeed = 1f;
        private Vector3 targetPosition;
        private Vector3 smoothPosition;
        private Vector3 lastPosition;
        private Vector3 lastRotation;
        private float smoothFOV;
        private float smoothSpeed = 1.0f;

        private class CameraRotation
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

        private static bool Focused
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
            if (panelVisible)
            {
                menuRect = GUILayout.Window(0, menuRect, (GUI.WindowFunction)CameraWindow, "Camera Tools by portra", new GUILayoutOption[0]);
            }
        }
        public void CameraWindow(int id)
        {
            if (id == 0)
            {
                var buttonStyle = new[]
                {
                    GUILayout.Width(80)
                };
                var buttonStyle2 = new[]
                {
                    GUILayout.Width(50)
                };

                GUILayout.Space(20);

                // Movement Speed
                GUILayout.Label($"Movement Speed: {moveSpeed:F3}", new GUILayoutOption[0]);
                moveSpeed = GUILayout.HorizontalSlider(moveSpeed, 0.001f, 10.0f, new GUILayoutOption[0]);
                GUILayout.BeginHorizontal(new GUILayoutOption[0]);
                if (GUILayout.Button("Reset", buttonStyle))
                    moveSpeed = 1.0f;
                if (GUILayout.Button("-", buttonStyle2))
                    moveSpeed -= 0.1f;
                if (GUILayout.Button("+", buttonStyle2))
                    moveSpeed += 0.1f;
                GUILayout.EndHorizontal();

                GUILayout.Space(20);

                // FOV speed
                GUILayout.Label($"FOV Speed: {fovSpeed:F2}", new GUILayoutOption[0]);
                fovSpeed = GUILayout.HorizontalSlider(fovSpeed, 0.01f, 10.0f, new GUILayoutOption[0]);
                GUILayout.BeginHorizontal(new GUILayoutOption[0]);
                if (GUILayout.Button("Reset", buttonStyle))
                    fovSpeed = 1.0f;
                if (GUILayout.Button("-", buttonStyle2))
                    fovSpeed -= 0.1f;
                if (GUILayout.Button("+", buttonStyle2))
                    fovSpeed += 0.1f;
                GUILayout.EndHorizontal();

                GUILayout.Space(20);

                // Roll speed
                GUILayout.Label($"Roll Speed: {rollSpeed:F1}", new GUILayoutOption[0]);
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
                GUILayout.Label($"Mouse Sensitivity: {lookSensitivity:F1}", new GUILayoutOption[0]);
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
                GUILayout.Label($"Game Speed: {Time.timeScale:F2}", new GUILayoutOption[0]);
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
                GUILayout.Label($"Camera Damping: {smoothSpeed:F2}", new GUILayoutOption[0]);
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
                GUILayout.Label($"Field of View: {targetFov:F3}", new GUILayoutOption[0]);
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
            if (Focused)
                UpdateInput();
            
            if (Input.GetKeyDown(keyFocus))
                Focused = Focused == false;
            
            if (Input.GetKeyDown(keyGUI))
                TogglePanel();

            // Limiters
            if (smoothSpeed < 0.01f)
                smoothSpeed = 0.01f;
            if (smoothSpeed > 1)
                smoothSpeed = 1;
            if (moveSpeed < 0.001f)
                moveSpeed = 0.001f;
            if (targetFov < 1f)
                targetFov = 1f;
            if (targetFov > 160f)
                targetFov = 160f;
            if (fovSpeed < 0.01f)
                fovSpeed = 0.01f;
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
            targetRotation.yaw += mouseInput.x * GetSpeed(lookSensitivity);
            targetRotation.pitch += mouseInput.y * GetSpeed(lookSensitivity);

            // Commit the rotation changes to the transform
            currentRotation.UpdateTransform(transform);

            // Roll camera
            if (Input.GetKey(keyRollLeft))
                targetRotation.roll += GetSpeed(rollSpeed);
            if (Input.GetKey(keyRollRight))
                targetRotation.roll -= GetSpeed(rollSpeed);
            if (Input.GetKey(keyRollReset))
                targetRotation.roll = 0;

            // Lateral Movement
            if (Input.GetKey(keyForward))
                targetPosition += transform.forward * GetSpeed(moveSpeed);
            if (Input.GetKey(keyLeft))
                targetPosition -= transform.right * GetSpeed(moveSpeed);
            if (Input.GetKey(keyBack))
                targetPosition -= transform.forward * GetSpeed(moveSpeed);
            if (Input.GetKey(keyRight))
                targetPosition += transform.right * GetSpeed(moveSpeed);

            // Vertical movement
            if (Input.GetKey(keyUp))
                targetPosition += transform.up * GetSpeed(moveSpeed);
            if (Input.GetKey(keyDown))
                targetPosition -= transform.up * GetSpeed(moveSpeed);

            // FOV
            if (Input.GetKey(keyFovDec))
                targetFov -= GetSpeed(fovSpeed);
            if (Input.GetKey(keyFovInc))
                targetFov += GetSpeed(fovSpeed);
            if (Input.GetKeyDown(keyFovReset))
                targetFov = 45f;
        }
        
        public void LateUpdate()
        {
            smoothPosition = Vector3.Lerp(transform.position, targetPosition, smoothSpeed);
            transform.position = smoothPosition;
            smoothFOV = Mathf.Lerp(cam.fieldOfView, targetFov, smoothSpeed);
            cam.fieldOfView = smoothFOV;
            currentRotation.LerpTowards(targetRotation, smoothSpeed);
        }

        private float GetSpeed(float movementSpeed)
        {
            if (Input.GetKey(keyFast))
                return movementSpeed * speedModifier;
            if (Input.GetKey(keySlow))
                return movementSpeed / speedModifier;
            return movementSpeed;
        }

        private void TogglePanel()
        {
            if (panelVisible)
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
    }
}
