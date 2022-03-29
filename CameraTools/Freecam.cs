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
		public float slow = 5; // slow movement
		public float accSprintMultiplier = 10; // how much faster you go when "sprinting"
		public float lookSensitivity = 1; // mouse look sensitivity
		public float dampingCoefficient = 20; // how quickly you break to a halt after you stop your input
		public float fov = 0.5f;
		public float targetFov = 45f;
		public bool focusOnEnable = true; // whether or not to focus and lock cursor immediately on enable

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

		public void OnEnable()
		{
			if (focusOnEnable) Focused = true;
			cam.CopyFrom(maincam);
		}

		//public void OnDisable() => Focused = false;

		public void Update()
		{
			// Input
			if (Focused)
				UpdateInput();
			else if (Input.GetKeyDown(KeyCode.Quote))
            {
				if (Focused == false)
					Focused = true;
				else Focused = false;
			}

			// Physics
			velocity = Vector3.Lerp(velocity, Vector3.zero, dampingCoefficient * Time.unscaledDeltaTime);
			transform.position += velocity * Time.unscaledDeltaTime;

			// FOV
			if (Input.GetKey(KeyCode.Alpha8))
			{
				targetFov -= fov;
			}
			if (Input.GetKey(KeyCode.Alpha9))
			{
				targetFov += fov;
			}
			if (Input.GetKeyDown(KeyCode.Alpha0))
			{
				targetFov = 45f;
			}
			cam.fieldOfView = Mathf.Lerp(cam.fieldOfView, targetFov, Time.unscaledDeltaTime * dampingCoefficient);

			// Damping limiter
			if (dampingCoefficient < 2)
				dampingCoefficient = 2;
			if (dampingCoefficient > 30)
				dampingCoefficient = 30;
		}

		public void UpdateInput()
		{
			// Position
			velocity += GetAccelerationVector() * Time.unscaledDeltaTime;

            // Rotation
            Vector2 mouseDelta = lookSensitivity * new Vector2(Input.GetAxis("Mouse X"), -Input.GetAxis("Mouse Y"));
            Quaternion rotation = transform.rotation;
            Quaternion horiz = Quaternion.AngleAxis(mouseDelta.x, Vector3.up);
            Quaternion vert = Quaternion.AngleAxis(mouseDelta.y, Vector3.right);
            transform.rotation = horiz * rotation * vert;

            if (Input.GetKeyDown(KeyCode.F11))
				dampingCoefficient -= 2;
			if (Input.GetKeyDown(KeyCode.F12))
				dampingCoefficient += 2;
			if (Input.GetKeyDown(KeyCode.F10))
				dampingCoefficient = 20;

			// Leave cursor lock
			//if (Input.GetKeyDown(KeyCode.Escape))
			//	Focused = false;
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
				return direction * slow; // "slower movement"
			if (Input.GetKey(KeyCode.RightAlt))
				return direction * (acceleration * accSprintMultiplier); // "sprinting"
			return direction * acceleration; // "walking"
		}
	}
}
