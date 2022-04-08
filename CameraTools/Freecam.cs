using System;
using UnityEngine;
using UnhollowerRuntimeLib;

namespace CameraTools
{
	internal class Freecam : MonoBehaviour
	{
		public Freecam(IntPtr ptr) : base(ptr) { }
		public Freecam() : base(ClassInjector.DerivedConstructorPointer<Freecam>())
		{
			ClassInjector.DerivedConstructorBody(this);
		}
		Camera cam = GameObject.Find("MainCamera(Clone)(Clone)").GetComponent<Camera>();
		Camera maincam = GameObject.Find("/EntityRoot/MainCamera(Clone)").GetComponent<Camera>();
		public float accSprintMultiplier = 10;
		public float lookSensitivity = 1;
		public float fov = 0.5f;
		public float targetFov = 45f;
		public bool focusOnEnable = true;
		public bool panelVisible = false;
		public bool rememberPos = false;
		public Rect MenuRect = new Rect(20, 100, 270, 640);

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
				MenuRect = GUI.Window(0, MenuRect, (GUI.WindowFunction)CameraWindow, "Camera Tools by portra");
            }
        }
		public void CameraWindow(int id)
		{
			if (id == 0)
			{
				// Movement Speed
				GUI.Label(new Rect(20, 40, 200, 30), "Movement speed");
				translationSpeed = GUI.HorizontalSlider(new Rect(10, 60, 200, 10), translationSpeed, 0.01f, 10.0f);
				GUI.Label(new Rect(215, 60, 80, 30), translationSpeed.ToString());
				if (GUI.Button(new Rect(20, 80, 80, 20), "Reset"))
					translationSpeed = 1.0f;
				if (GUI.Button(new Rect(110, 80, 50, 20), "-"))
					translationSpeed -= 0.1f;
				if (GUI.Button(new Rect(170, 80, 50, 20), "+"))
					translationSpeed += 0.1f;

				// FOV speed
				GUI.Label(new Rect(20, 120, 200, 30), "FOV speed");
				fov = GUI.HorizontalSlider(new Rect(10, 140, 200, 10), fov, 0.01f, 10.0f);
				GUI.Label(new Rect(215, 140, 80, 30), fov.ToString());
				if (GUI.Button(new Rect(20, 160, 80, 20), "Reset"))
					fov = 1.0f;
				if (GUI.Button(new Rect(110, 160, 50, 20), "-"))
					fov -= 0.1f;
				if (GUI.Button(new Rect(170, 160, 50, 20), "+"))
					fov += 0.1f;

				// Roll speed
				GUI.Label(new Rect(20, 200, 200, 30), "Roll speed");
				rollSpeed = GUI.HorizontalSlider(new Rect(10, 220, 200, 10), rollSpeed, 0.1f, 10.0f);
				GUI.Label(new Rect(215, 220, 80, 30), rollSpeed.ToString());
				if (GUI.Button(new Rect(20, 240, 80, 20), "Reset"))
					rollSpeed = 1;
				if (GUI.Button(new Rect(110, 240, 50, 20), "-"))
					rollSpeed -= 0.1f;
				if (GUI.Button(new Rect(170, 240, 50, 20), "+"))
					rollSpeed += 0.1f;

				// Mouse sensitivity
				GUI.Label(new Rect(20, 280, 200, 30), "Mouse sensitivity");
				lookSensitivity = GUI.HorizontalSlider(new Rect(10, 300, 200, 10), lookSensitivity, 0.1f, 10.0f);
				GUI.Label(new Rect(215, 300, 80, 30), lookSensitivity.ToString());
				if (GUI.Button(new Rect(20, 320, 80, 20), "Reset"))
					lookSensitivity = 1;
				if (GUI.Button(new Rect(110, 320, 50, 20), "-"))
					lookSensitivity -= 0.1f;
				if (GUI.Button(new Rect(170, 320, 50, 20), "+"))
					lookSensitivity += 0.1f;

				// Game speed
				GUI.Label(new Rect(20, 360, 200, 30), "Game speed");
				Time.timeScale = GUI.HorizontalSlider(new Rect(10, 380, 200, 10), Time.timeScale, 0.0f, 10.0f);
				GUI.Label(new Rect(215, 380, 80, 30), Time.timeScale.ToString());
				if (GUI.Button(new Rect(20, 400, 80, 20), "Reset"))
					Time.timeScale = 1f;
				if (GUI.Button(new Rect(110, 400, 50, 20), "-"))
					Time.timeScale -= 1f;
				if (GUI.Button(new Rect(170, 400, 50, 20), "+"))
					Time.timeScale += 1f;

				// Movement smoothness
				GUI.Label(new Rect(20, 440, 200, 30), "Camera damping");
				smoothSpeed = GUI.HorizontalSlider(new Rect(10, 460, 200, 10), smoothSpeed, 0.01f, 1f);
				GUI.Label(new Rect(215, 460, 80, 30), smoothSpeed.ToString());
				if (GUI.Button(new Rect(20, 480, 80, 20), "Reset"))
					smoothSpeed = 1.0f;
				if (GUI.Button(new Rect(110, 480, 50, 20), "-"))
					smoothSpeed -= 0.05f;
				if (GUI.Button(new Rect(170, 480, 50, 20), "+"))
					smoothSpeed += 0.05f;

				// FOV
				GUI.Label(new Rect(20, 520, 200, 30), "Field of view");
				targetFov = GUI.HorizontalSlider(new Rect(10, 540, 200, 10), targetFov, 1.0f, 160.0f);
				GUI.Label(new Rect(215, 540, 80, 30), targetFov.ToString());
				if (GUI.Button(new Rect(20, 560, 80, 20), "Reset"))
					targetFov = 45.0f;
				if (GUI.Button(new Rect(110, 560, 50, 20), "-"))
					targetFov -= 1f;
				if (GUI.Button(new Rect(170, 560, 50, 20), "+"))
					targetFov += 1f;

				rememberPos = GUI.Toggle(new Rect(20, 600, 200, 30), rememberPos, "Remember last position");
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
			if (Input.GetKeyDown(CameraTools.keyFocus))
            {
				if (Focused == false)
					Focused = true;
				else Focused = false;
			}
			if (Input.GetKeyDown(CameraTools.keyGUI))
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
			if (translationSpeed < 0.01f)
				translationSpeed = 0.01f;
			if (targetFov < 1f)
				targetFov = 1f;
			if (targetFov > 160f)
				targetFov = 160f;
			if (fov < 0.1f)
				fov = 0.1f;
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
            if (Input.GetKey(CameraTools.keyRollLeft))
				targetRotation.roll += rollSpeed;
			if (Input.GetKey(CameraTools.keyRollRight))
				targetRotation.roll -= rollSpeed;
			if (Input.GetKey(CameraTools.keyRollReset))
				targetRotation.roll = 0;

            // Lateral Movement
            if (Input.GetKey(CameraTools.keyForward))
				targetPosition += transform.forward * translationSpeed;
			if (Input.GetKey(CameraTools.keyLeft))
				targetPosition -= transform.right * translationSpeed;
			if (Input.GetKey(CameraTools.keyBack))
				targetPosition -= transform.forward * translationSpeed;
			if (Input.GetKey(CameraTools.keyRight))
				targetPosition += transform.right * translationSpeed;

			// Vertical movement
			if (Input.GetKey(CameraTools.keyUp))
				targetPosition += transform.up * translationSpeed;
			if (Input.GetKey(CameraTools.keyDown))
				targetPosition -= transform.up * translationSpeed;

			// FOV
			if (Input.GetKey(CameraTools.keyFovDec))
				targetFov -= fov;
			if (Input.GetKey(CameraTools.keyFovInc))
				targetFov += fov;
			if (Input.GetKeyDown(CameraTools.keyFovReset))
				targetFov = 45f;

			if (Input.GetKeyDown(CameraTools.keyFast))
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
			else if (Input.GetKeyUp(CameraTools.keyFast))
			{
				translationSpeed = lasttranslationSpeed;
				rollSpeed = lastrollSpeed;
				lookSensitivity = lastlookSensitivity;
				fov = lastfov;
			}
			if (Input.GetKeyDown(CameraTools.keySlow))
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
			else if (Input.GetKeyUp(CameraTools.keySlow))
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
