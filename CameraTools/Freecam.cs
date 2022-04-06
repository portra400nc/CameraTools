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
		public bool focusOnEnable = true; // whether or not to focus and lock cursor immediately on enable
		public bool panelVisible = false;
		public bool rememberPos = false;
		public Rect MenuRect = new Rect(20, 100, 270, 640);

		private float translationSpeed = 0.5f;
		private float rollSpeed = 1f;
		private Vector3 targetPosition;
		private Vector3 smoothPosition;
		private Vector3 lastPosition;
		public Quaternion targetRotation;
		private Quaternion smoothRotation;
		private Quaternion lastRotation;
		public float smoothFOV;
		private float smoothSpeed = 1.0f;

		private float lasttranslationSpeed;
		private float lastlookSensitivity;
		private float lastrollSpeed;
		private float lastfov;

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
			if (focusOnEnable) Focused = true;
			cam.CopyFrom(maincam);
			if (rememberPos)
			{
				transform.rotation = lastRotation;
				transform.position = lastPosition;
			}
			targetRotation = transform.rotation;
			targetPosition = transform.position;
		}

		public void OnDisable()
        {
			lastPosition = transform.position;
			lastRotation = transform.rotation;
        }

		public void Update()
		{
			// Input
			if (Focused)
				UpdateInput();
			if (Input.GetKeyDown(KeyCode.Quote))
            {
				if (Focused == false)
					Focused = true;
				else Focused = false;
			}
			if (Input.GetKeyDown(KeyCode.F10))
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
			Vector2 delta = Vector2.zero;
			delta.y += Input.GetAxis("Mouse X");
			delta.x -= Input.GetAxis("Mouse Y");
			Quaternion deltaRotation = Quaternion.Euler(delta.x * lookSensitivity, delta.y * lookSensitivity, 0);

			Vector3 eulerRotation = transform.rotation.eulerAngles;

			// Roll camera
			if (Input.GetKey(KeyCode.Comma))
				deltaRotation *= Quaternion.Euler(0, 0, rollSpeed);
			if (Input.GetKey(KeyCode.Period))
				deltaRotation *= Quaternion.Euler(0, 0, -rollSpeed);
			if (Input.GetKey(KeyCode.RightShift))
				targetRotation = Quaternion.Euler(eulerRotation.x, eulerRotation.y, 0);

			// Rotation
			targetRotation *= deltaRotation;

			// Lateral Movement
			if (Input.GetKey(KeyCode.I))
				targetPosition += transform.forward * translationSpeed;
			if (Input.GetKey(KeyCode.J))
				targetPosition -= transform.right * translationSpeed;
			if (Input.GetKey(KeyCode.K))
				targetPosition -= transform.forward * translationSpeed;
			if (Input.GetKey(KeyCode.L))
				targetPosition += transform.right * translationSpeed;

			// Vertical movement
			if (Input.GetKey(KeyCode.O))
				targetPosition += transform.up * translationSpeed;
			if (Input.GetKey(KeyCode.U))
				targetPosition -= transform.up * translationSpeed;

			// FOV
			if (Input.GetKey(KeyCode.Alpha8))
				targetFov -= fov;
			if (Input.GetKey(KeyCode.Alpha9))
				targetFov += fov;
			if (Input.GetKeyDown(KeyCode.Alpha0))
				targetFov = 45f;

			if (Input.GetKeyDown(KeyCode.RightAlt))
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
			else if (Input.GetKeyUp(KeyCode.RightAlt))
			{
				translationSpeed = lasttranslationSpeed;
				rollSpeed = lastrollSpeed;
				lookSensitivity = lastlookSensitivity;
				fov = lastfov;
			}
			if (Input.GetKeyDown(KeyCode.Semicolon))
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
			else if (Input.GetKeyUp(KeyCode.Semicolon))
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
			smoothRotation = Quaternion.Lerp(transform.rotation, targetRotation, smoothSpeed);
			transform.rotation = smoothRotation;
			smoothFOV = Mathf.Lerp(cam.fieldOfView, targetFov, smoothSpeed);
			cam.fieldOfView = smoothFOV;
		}
	}
}
