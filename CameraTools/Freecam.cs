using MelonLoader;
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
        // Absolute, so the remembered position survives a world shift while the free camera is off.
        private Vector3 lastPosition;
        private Vector3 lastRotation;
        private bool hasLastPose;
        private float smoothFOV;
        private float gameFov;
        // Where the world's absolute origin was last frame, in scene coordinates.
        private Vector3 worldOrigin;

        private class CameraRotation
        {
            public float yaw, pitch, roll;

            public void InitializeFromTransform(Transform t)
            {
                pitch = t.eulerAngles.x;
                yaw = t.eulerAngles.y;
                roll = t.eulerAngles.z;
            }

            // Within -180 to 180, so resetting the roll afterwards turns the short way back to 0.
            public void Set(Vector3 euler)
            {
                pitch = Mathf.DeltaAngle(0f, euler.x);
                yaw = Mathf.DeltaAngle(0f, euler.y);
                roll = Mathf.DeltaAngle(0f, euler.z);
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
                transform.position = WorldShift.Relative(lastPosition);
            }
            targetRotation.InitializeFromTransform(transform);
            currentRotation.InitializeFromTransform(transform);
            smoothPosition = transform.position;
            targetPosition = smoothPosition;
            smoothFOV = cam.fieldOfView;
            gameFov = smoothFOV;
            settings.GameNear = cam.nearClipPlane;
            CameraTools.LogOnce($"Free camera: the game's near clip is {settings.GameNear:0.000} m and its far clip {cam.farClipPlane:0} m.");
            worldOrigin = WorldShift.Relative(default);
        }

        public void OnDisable()
        {
            lastPosition = WorldShift.Absolute(smoothPosition);
            lastRotation = new Vector3(currentRotation.pitch, currentRotation.yaw, currentRotation.roll);
            hasLastPose = true;
            cam.nearClipPlane = settings.GameNear;
            SetFieldOfView(gameFov);
        }

        // What is on screen, which a camera path node records.
        public (Vector3 Position, Quaternion Rotation, float Fov) Pose
            => (smoothPosition, Quaternion.Euler(currentRotation.pitch, currentRotation.yaw, currentRotation.roll), smoothFOV);

        // Moves the camera there at once: the target and the smoothed pose both change, so damping neither lags behind nor
        // pulls back afterwards.
        public void Snap(Vector3 position, Quaternion rotation, float fov)
        {
            targetPosition = position;
            smoothPosition = position;
            var euler = rotation.eulerAngles;
            targetRotation.Set(euler);
            currentRotation.Set(euler);
            settings.Fov.Value = fov;
            smoothFOV = settings.Fov.Value;
        }

        // When Genshin shifts its world origin, everything in the scene jumps by the same offset; moving the camera with it
        // keeps the shot where it was.
        public void FollowWorldShift()
        {
            var origin = WorldShift.Relative(default);
            if (origin.x == worldOrigin.x && origin.y == worldOrigin.y && origin.z == worldOrigin.z)
                return;
            var moved = new Vector3 { x = origin.x - worldOrigin.x, y = origin.y - worldOrigin.y, z = origin.z - worldOrigin.z };
            worldOrigin = origin;
            targetPosition += moved;
            smoothPosition += moved;
            Melon<CameraTools>.Logger.Msg($"World shift: the origin moved by ({moved.x:0.0}, {moved.y:0.0}, {moved.z:0.0}); the free camera moved with it.");
        }

        // Genshin's own camera system rewrites the camera every frame after OnLateUpdate, so the pose is kept here
        // and written again right before the camera renders.
        public void Apply()
        {
            transform.position = smoothPosition;
            currentRotation.UpdateTransform(transform);
            cam.nearClipPlane = settings.NearClip;
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
