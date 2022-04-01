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
		public float acceleration = 50; // how fast you accelerate
		public float slow = 10; // slow movement
		public float accSprintMultiplier = 10; // how much faster you go when "sprinting"
		public float lookSensitivity = 1; // mouse look sensitivity
		public float dampingCoefficient = 30; // how quickly you break to a halt after you stop your input
		public float fov = 0.5f;
		public float targetFov = 45f;
		public bool focusOnEnable = true; // whether or not to focus and lock cursor immediately on enable
		public bool panelVisible = false;

		// Roll Speed
		private float rollSpeed = 1f;
		// Current Target rotation
		public Quaternion targetRotation;
		// The current intermediary Lerp rotation
		private Quaternion smoothRotation;
		// Lerping Smooth Speed
		private float smoothSpeed = 1.0f;


		Vector3 velocity; // current velocity

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
				// Movement Speed
				GUI.Label(new Rect(20, 100, 200, 30), "Movement speed");
				acceleration = GUI.HorizontalSlider(new Rect(10, 120, 200, 10), acceleration, 1.0f, 10000.0f);
				GUI.Label(new Rect(215, 120, 80, 30), acceleration.ToString());
				if (GUI.Button(new Rect(20, 140, 80, 20), "Reset"))
					acceleration = 50;
				if (GUI.Button(new Rect(110, 140, 50, 20), "+"))
					acceleration += 5;
				if (GUI.Button(new Rect(170, 140, 50, 20), "-"))
					acceleration -= 5;

				// FOV speed
				GUI.Label(new Rect(20, 180, 200, 30), "FOV speed");
				fov = GUI.HorizontalSlider(new Rect(10, 200, 200, 10), fov, 0.01f, 10.0f);
				GUI.Label(new Rect(215, 200, 80, 30), fov.ToString());
				if (GUI.Button(new Rect(20, 220, 80, 20), "Reset"))
					fov = 0.5f;
				if (GUI.Button(new Rect(110, 220, 50, 20), "+"))
					fov += 0.1f;
				if (GUI.Button(new Rect(170, 220, 50, 20), "-"))
					fov -= 0.1f;

				// Roll speed
				GUI.Label(new Rect(20, 260, 200, 30), "Roll speed");
				rollSpeed = GUI.HorizontalSlider(new Rect(10, 280, 200, 10), rollSpeed, 0.1f, 10.0f);
				GUI.Label(new Rect(215, 280, 80, 30), rollSpeed.ToString());
				if (GUI.Button(new Rect(20, 300, 80, 20), "Reset"))
					rollSpeed = 1;
				if (GUI.Button(new Rect(110, 300, 50, 20), "+"))
					rollSpeed += 0.1f;
				if (GUI.Button(new Rect(170, 300, 50, 20), "-"))
					rollSpeed -= 0.1f;

				// Mouse sensitivity
				GUI.Label(new Rect(20, 340, 200, 30), "Mouse sensitivity");
				lookSensitivity = GUI.HorizontalSlider(new Rect(10, 360, 200, 10), lookSensitivity, 0.1f, 10.0f);
				GUI.Label(new Rect(215, 360, 80, 30), lookSensitivity.ToString());
				if (GUI.Button(new Rect(20, 380, 80, 20), "Reset"))
					lookSensitivity = 1;
				if (GUI.Button(new Rect(110, 380, 50, 20), "+"))
					lookSensitivity += 0.1f;
				if (GUI.Button(new Rect(170, 380, 50, 20), "-"))
					lookSensitivity -= 0.1f;

				// Game speed
				GUI.Label(new Rect(20, 420, 200, 30), "Game speed");
				Time.timeScale = GUI.HorizontalSlider(new Rect(10, 440, 200, 10), Time.timeScale, 0.0f, 10.0f);
				GUI.Label(new Rect(215, 440, 80, 30), Time.timeScale.ToString());
				if (GUI.Button(new Rect(20, 460, 80, 20), "Reset"))
					Time.timeScale = 1f;
				if (GUI.Button(new Rect(110, 460, 50, 20), "+"))
					Time.timeScale += 1f;
				if (GUI.Button(new Rect(170, 460, 50, 20), "-"))
					Time.timeScale -= 1f;

				// Movement smoothness
				GUI.Label(new Rect(20, 500, 200, 30), "Movement & FOV damping");
				dampingCoefficient = GUI.HorizontalSlider(new Rect(10, 520, 200, 10), dampingCoefficient, 1.0f, 30.0f);
				GUI.Label(new Rect(215, 520, 80, 30), dampingCoefficient.ToString());
				if (GUI.Button(new Rect(20, 540, 80, 20), "Reset"))
					dampingCoefficient = 30f;
				if (GUI.Button(new Rect(110, 540, 50, 20), "+"))
					dampingCoefficient += 1f;
				if (GUI.Button(new Rect(170, 540, 50, 20), "-"))
					dampingCoefficient -= 1f;

				// Damping
				GUI.Label(new Rect(20, 580, 200, 30), "Rotation & roll damping");
				smoothSpeed = GUI.HorizontalSlider(new Rect(10, 600, 200, 10), smoothSpeed, 0.01f, 1f);
				GUI.Label(new Rect(215, 600, 80, 30), smoothSpeed.ToString());
				if (GUI.Button(new Rect(20, 620, 80, 20), "Reset"))
					smoothSpeed = 1.0f;
				if (GUI.Button(new Rect(110, 620, 50, 20), "+"))
					smoothSpeed += 0.05f;
				if (GUI.Button(new Rect(170, 620, 50, 20), "-"))
					smoothSpeed -= 0.05f;

				// FOV
				GUI.Label(new Rect(20, 660, 200, 30), "Field of view");
				targetFov = GUI.HorizontalSlider(new Rect(10, 680, 200, 10), targetFov, 1.0f, 160.0f);
				GUI.Label(new Rect(215, 680, 80, 30), targetFov.ToString());
				if (GUI.Button(new Rect(20, 700, 80, 20), "Reset"))
					targetFov = 45.0f;
				if (GUI.Button(new Rect(110, 700, 50, 20), "+"))
					targetFov += 1f;
				if (GUI.Button(new Rect(170, 700, 50, 20), "-"))
					targetFov -= 1f;
			}
        }
		public void OnEnable()
		{
			if (focusOnEnable) Focused = true;
			cam.CopyFrom(maincam);
			targetRotation = transform.rotation;
		}

		//public void OnDisable() => Focused = false;

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
					panelVisible = false;
				else
					panelVisible = true;
            }

			// Physics
			velocity = Vector3.Lerp(velocity, Vector3.zero, dampingCoefficient * Time.unscaledDeltaTime);
			transform.position += velocity * Time.unscaledDeltaTime;
			
			cam.fieldOfView = Mathf.Lerp(cam.fieldOfView, targetFov, Time.unscaledDeltaTime * dampingCoefficient);

			// Limiters
			if (dampingCoefficient < 1)
				dampingCoefficient = 1;
			if (smoothSpeed < 0.01f)
				smoothSpeed = 0.01f;
			if (acceleration < 1f)
				acceleration = 1f;
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
			// Position
			velocity += GetAccelerationVector() * Time.unscaledDeltaTime;

			Vector2 delta = Vector2.zero;
			delta.y += Input.GetAxis("Mouse X");
			delta.x -= Input.GetAxis("Mouse Y");
			Quaternion deltaRotation = Quaternion.Euler(delta.x * lookSensitivity, delta.y * lookSensitivity, 0);

			// Roll camera
			if (Input.GetKey(KeyCode.Comma))
				deltaRotation *= Quaternion.Euler(0, 0, rollSpeed);
			if (Input.GetKey(KeyCode.Period))
				deltaRotation *= Quaternion.Euler(0, 0, -rollSpeed);
			if (Input.GetKey(KeyCode.RightShift))
				targetRotation = deltaRotation;

			// Rotation
			targetRotation *= deltaRotation;

			// FOV
			if (Input.GetKey(KeyCode.Alpha8))
				targetFov -= fov;
			if (Input.GetKey(KeyCode.Alpha9))
				targetFov += fov;
			if (Input.GetKeyDown(KeyCode.Alpha0))
				targetFov = 45f;

			// Leave cursor lock
			//if (Input.GetKeyDown(KeyCode.Escape))
			//	Focused = false;
		}

		public void LateUpdate()
        {
			// Lerp toward target rotation at all times.
			smoothRotation = Quaternion.Lerp(transform.rotation, targetRotation, smoothSpeed);
			transform.rotation = smoothRotation;
		}

		Vector3 GetAccelerationVector()
		{
			Vector3 moveInput = default;

			void AddMovement(KeyCode key, Vector3 dir)
			{
				if (Input.GetKey(key))
					moveInput += dir;
			}

			AddMovement(KeyCode.I, Vector3.forward);
			AddMovement(KeyCode.K, Vector3.back);
			AddMovement(KeyCode.L, Vector3.right);
			AddMovement(KeyCode.J, Vector3.left);
			AddMovement(KeyCode.O, Vector3.up);
			AddMovement(KeyCode.U, Vector3.down);
			Vector3 direction = transform.TransformVector(moveInput.normalized);

			if (Input.GetKey(KeyCode.Semicolon))
				return direction * (acceleration / slow); // "slower movement"
			if (Input.GetKey(KeyCode.RightAlt))
				return direction * (acceleration * accSprintMultiplier); // "sprinting"
			return direction * acceleration; // "walking"
		}
	}
}
