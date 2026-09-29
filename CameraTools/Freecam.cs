using UnityEngine;
using static CameraTools.CameraTools;

namespace CameraTools
{
    // Genshin's MelonLoader cannot register custom MonoBehaviours yet, so the free camera is a plain
    // class that CameraTools drives from OnLateUpdate while the free camera is on.
    public class Freecam
    {
        private readonly Transform transform;

        public Freecam(Transform transform)
        {
            this.transform = transform;
        }

        private const float PadLookSpeed = 120f;
        private float speedModifier = 10;
        private bool focusOnEnable = true;

        private Vector3 targetPosition;
        private Vector3 smoothPosition;
        private Vector3 lastPosition;
        private Vector3 lastRotation;
        private bool hasLastPose;
        private float smoothFOV;
        private float gameFov;

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

        internal static bool Focused
        {
            get => Cursor.lockState == CursorLockMode.Locked;
            set
            {
                Cursor.lockState = value ? CursorLockMode.Locked : CursorLockMode.None;
                Cursor.visible = value == false;
            }
        }

        public void OnEnable()
        {
            if (focusOnEnable) Focused = true;
            if (settings.RememberPosition && hasLastPose)
            {
                transform.eulerAngles = lastRotation;
                transform.position = lastPosition;
            }
            targetRotation.InitializeFromTransform(transform);
            currentRotation.InitializeFromTransform(transform);
            smoothPosition = transform.position;
            targetPosition = smoothPosition;
            smoothFOV = cam.fieldOfView;
            gameFov = smoothFOV;
        }

        public void OnDisable()
        {
            lastPosition = smoothPosition;
            lastRotation = new Vector3(currentRotation.pitch, currentRotation.yaw, currentRotation.roll);
            hasLastPose = true;
            SetFieldOfView(gameFov);
        }

        // Genshin's own camera system rewrites the camera every frame after OnLateUpdate, so the pose is kept here
        // and written again right before the camera renders.
        public void Apply()
        {
            transform.position = smoothPosition;
            currentRotation.UpdateTransform(transform);
            SetFieldOfView(smoothFOV);
        }

        // Genshin's post-processing installs a jittered projection built from the game's field of view before this
        // runs, and Unity ignores fieldOfView while a custom projection is set.
        private void SetFieldOfView(float fov)
        {
            cam.fieldOfView = fov;
            cam.ResetProjectionMatrix();
            cam.nonJitteredProjectionMatrix = Matrix4x4.Perspective(fov, cam.aspect, cam.nearClipPlane, cam.farClipPlane);
        }

        public void Update()
        {
            UpdateInput(Focused);

            if (Controls.Pressed(CamAction.ToggleCursorFocus))
                Focused = Focused == false;
        }

        // Keyboard and mouse only count while the cursor is focused on the game; the controller always counts.
        public void UpdateInput(bool keyboard)
        {
            if (keyboard)
            {
                var mouseInput = new Vector2(Input.GetAxis("Mouse X"), Input.GetAxis("Mouse Y") * -1);
                targetRotation.yaw += mouseInput.x * GetSpeed(settings.LookSensitivity.Value);
                targetRotation.pitch += mouseInput.y * GetSpeed(settings.LookSensitivity.Value);
            }
            // Degrees per second at full deflection, on unscaled time so the stick still turns while the game is paused.
            var (lookX, lookY) = Controls.Look;
            float lookRate = GetSpeed(PadLookSpeed * settings.LookSensitivity.Value) * Time.unscaledDeltaTime;
            targetRotation.yaw += lookX * lookRate;
            targetRotation.pitch -= lookY * lookRate;

            var rotation = Quaternion.Euler(currentRotation.pitch, currentRotation.yaw, currentRotation.roll);
            var forward = rotation * Vector3.forward;
            var right = rotation * Vector3.right;
            var up = rotation * Vector3.up;

            if (Controls.Held(CamAction.RollLeft, keyboard))
                targetRotation.roll += GetSpeed(settings.RollSpeed.Value);
            if (Controls.Held(CamAction.RollRight, keyboard))
                targetRotation.roll -= GetSpeed(settings.RollSpeed.Value);
            if (Controls.Held(CamAction.ResetRoll, keyboard))
                targetRotation.roll = 0;

            float speed = GetSpeed(settings.MoveSpeed.Value);
            targetPosition += forward * ((Controls.Value(CamAction.Forward, keyboard) - Controls.Value(CamAction.Back, keyboard)) * speed);
            targetPosition += right * ((Controls.Value(CamAction.Right, keyboard) - Controls.Value(CamAction.Left, keyboard)) * speed);
            targetPosition += up * ((Controls.Value(CamAction.Up, keyboard) - Controls.Value(CamAction.Down, keyboard)) * speed);

            var fov = settings.Fov;
            if (Controls.Held(CamAction.DecreaseFOV, keyboard))
                fov.Value -= GetSpeed(settings.FovSpeed.Value);
            if (Controls.Held(CamAction.IncreaseFOV, keyboard))
                fov.Value += GetSpeed(settings.FovSpeed.Value);
            if (Controls.Pressed(CamAction.ResetFOV, keyboard))
                fov.Value = 45f;
        }

        public void LateUpdate()
        {
            float damping = settings.Damping.Value;
            smoothPosition = Vector3.Lerp(smoothPosition, targetPosition, damping);
            smoothFOV = Mathf.Lerp(smoothFOV, settings.Fov.Value, damping);
            currentRotation.LerpTowards(targetRotation, damping);
            Apply();
            Lod.Sync(smoothPosition, Quaternion.Euler(currentRotation.pitch, currentRotation.yaw, currentRotation.roll), smoothFOV);
        }

        private float GetSpeed(float movementSpeed)
        {
            if (Controls.Held(CamAction.FastMovement))
                return movementSpeed * speedModifier;
            if (Controls.Held(CamAction.SlowMovement))
                return movementSpeed / speedModifier;
            return movementSpeed;
        }
    }
}
