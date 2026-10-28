using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    public enum InputOwner
    {
        Game,
        CameraTools,
    }

    public enum InputDevice
    {
        Keyboard,
        Pad,
    }

    public static class Controls
    {
        private static readonly KeyCode[] keys = new KeyCode[ControlsModel.Defaults.Length];
        private static readonly PadBinding[] pads = new PadBinding[ControlsModel.Defaults.Length];
        private static readonly MelonPreferences_Entry<KeyCode>[] keyEntries = new MelonPreferences_Entry<KeyCode>[ControlsModel.Defaults.Length];
        private static readonly MelonPreferences_Entry<string>[] padEntries = new MelonPreferences_Entry<string>[ControlsModel.Defaults.Length];

        private static InputDevice lastUsed;

        // The free camera takes the keyboard, mouse and pad from the game, except while a recording has you play. While the
        // game owns them, only the free camera's toggle is read.
        public static InputOwner Owner => CameraTools.freecamActive && ReplayRecorder.Idle ? InputOwner.CameraTools : InputOwner.Game;

        // Hints follow the device used last, like the game's own.
        public static InputDevice Layout => Owner == InputOwner.CameraTools && lastUsed == InputDevice.Pad ? InputDevice.Pad : InputDevice.Keyboard;

        // While a text field takes typing, the keyboard fires no actions, so typing 8 does not narrow the field of view.
        public static bool TextCapture { get; set; }

        // While the Keybinds tab waits for new buttons, the pad fires no actions, the free camera's toggle included.
        public static bool PadCapture { get; set; }

        // While the settings panel is open, or the sticks turn a joint or move the character, the pad is read there, so the
        // camera ignores it.
        private static bool PadActive => Owner == InputOwner.CameraTools && CameraUi.View is not (View.Panel or View.Joints or View.Placing);

        // Move light flies the camera as the light, so only the actions that fly it and change its view stay live, with the roll
        // buttons, which it reads as the intensity. A and B finish it, so B does not also hide the UI, and X and Y set the spot
        // angle.
        private static readonly HashSet<CamAction> MoveLightActions = new()
        {
            CamAction.Forward, CamAction.Back, CamAction.Left, CamAction.Right, CamAction.Up, CamAction.Down,
            CamAction.FastMovement, CamAction.SlowMovement, CamAction.IncreaseFOV, CamAction.DecreaseFOV, CamAction.ResetFOV,
            CamAction.RollLeft, CamAction.RollRight,
        };

        private static bool LiveInView(CamAction action) => CameraUi.View != View.Moving || MoveLightActions.Contains(action);

        // Pose joints reads the arrow keys, which also change the game speed, and the camera's movement keys, and Move
        // character reads those and the roll keys, so there the keyboard fires no actions.
        private static bool KeysActive => Owner == InputOwner.CameraTools && CameraUi.View is not (View.Joints or View.Placing);

        public static void Load()
        {
            var keyboard = MelonPreferences.CreateCategory("CameraTools");
            var controller = MelonPreferences.CreateCategory("CameraToolsController");
            var layout = keyboard.CreateEntry("ControlsLayout", 1,
                description: "The version of the default bindings this file was last brought up to. Leave it alone.");
            int saved = layout.Value;
            for (int i = 0; i < ControlsModel.Defaults.Length; i++)
            {
                var (action, key, pad) = ControlsModel.Defaults[i];
                if ((int)action != i || !PadBinding.TryParse(pad, out var fallback))
                    throw new InvalidOperationException($"ControlsModel.Defaults row {i} ({action}) is out of CamAction order or has an invalid pad binding.");
                string name = action.ToString();
                var keyEntry = keyboard.CreateEntry(name, key);
                var padEntry = controller.CreateEntry(name, pad);
                (keyEntry.Value, padEntry.Value) = ControlsModel.Migrate(action, keyEntry.Value, padEntry.Value, saved);
                keyEntries[i] = keyEntry;
                padEntries[i] = padEntry;
                keys[i] = keyEntry.Value;
                string text = padEntry.Value;
                if (!PadBinding.TryParse(text, out pads[i]))
                {
                    Melon<CameraTools>.Logger.Warning($"CameraToolsController {name} = \"{text}\" is not a controller binding; using \"{pad}\".");
                    pads[i] = fallback;
                }
            }
            GameCamera.Enabled = keyboard.CreateEntry("DetachGameCamera", true,
                description: "Point the game's camera system at a hidden camera while the free camera is on. Set to false if the game misbehaves.").Value;
            foreach (var (removedIn, name) in ControlsModel.Removed)
            {
                if (saved >= removedIn)
                    continue;
                keyboard.DeleteEntry(name);
                controller.DeleteEntry(name);
            }
            layout.Value = ControlsModel.Layout;
            if (ControlsModel.Defaults.Length != Enum.GetValues<CamAction>().Length)
                throw new InvalidOperationException("ControlsModel.Defaults is missing a CamAction.");
            MelonPreferences.Save();
        }

        // After Gamepad.Poll. Unity may also report a pad button as a key, so the pad wins a tie. On the Deck, holding LB or
        // RB switched the hints to the keyboard until release, so a held button or tilted stick counts as pad use every frame.
        public static void Update()
        {
            if (!TextCapture && Input.anyKeyDown || Input.GetAxis("Mouse X") != 0f || Input.GetAxis("Mouse Y") != 0f)
                lastUsed = InputDevice.Keyboard;
            if (Gamepad.Current != Gamepad.Previous || Gamepad.Current != default)
                lastUsed = InputDevice.Pad;
        }

        public static (KeyCode Key, PadBinding Pad) Binding(CamAction action) => (keys[(int)action], pads[(int)action]);

        // Rebinds as ControlsModel.Swap does, and saves to MelonPreferences.cfg.
        public static Rebind SetKey(CamAction action, KeyCode key)
        {
            var result = ControlsModel.Swap(keys, action, key, KeyCode.None);
            if (result.Done)
                Save();
            return result;
        }

        public static Rebind SetPad(CamAction action, PadBinding pad)
        {
            var result = ControlsModel.Swap(pads, action, pad, default);
            if (result.Done)
                Save();
            return result;
        }

        private static void Save()
        {
            for (int i = 0; i < keys.Length; i++)
            {
                keyEntries[i].Value = keys[i];
                padEntries[i].Value = pads[i].ToString();
            }
            MelonPreferences.Save();
        }

        public static void RestoreDefaults()
        {
            for (int i = 0; i < ControlsModel.Defaults.Length; i++)
            {
                var (_, key, pad) = ControlsModel.Defaults[i];
                keys[i] = key;
                PadBinding.TryParse(pad, out pads[i]);
            }
            Save();
        }

        // Esc stops playing a path or a replay. It is fixed, like the keys of Edit replay and Pose joints.
        public static bool EscapePressed => !TextCapture && Owner == InputOwner.CameraTools && Input.GetKeyDown(KeyCode.Escape);

        // The device used last, read even while the game owns input.
        public static InputDevice LastUsed => lastUsed;

        public static (float x, float y) Look
            => PadActive && !DepthOfField.Steering ? (Gamepad.Current.RightX, Gamepad.Current.RightY) : (0f, 0f);

        public static bool Pressed(CamAction action, bool keyboard = true)
            => keyboard && KeyOn(action) && Input.GetKeyDown(keys[(int)action])
                || PadOn(action) && pads[(int)action].Pressed(Gamepad.Current, Gamepad.Previous);

        public static bool Held(CamAction action, bool keyboard = true)
            => keyboard && KeyOn(action) && Input.GetKey(keys[(int)action])
                || PadOn(action) && pads[(int)action].Held(Gamepad.Current);

        public static float Value(CamAction action, bool keyboard = true)
            => keyboard && KeyOn(action) && Input.GetKey(keys[(int)action]) ? 1f
                : PadOn(action) ? pads[(int)action].Value(Gamepad.Current) : 0f;

        // Edit replay reads its keys and buttons itself, and the actions bound to the same ones wait until it closes. The
        // free camera's toggle is read everywhere, so input can always be taken from or given back to the game.
        private static bool KeyOn(CamAction action)
            => !TextCapture && (action == CamAction.ToggleFreecam || KeysActive && LiveInView(action)) && !ReplayEditor.Reads(keys[(int)action]);

        private static bool PadOn(CamAction action)
            => !PadCapture && (action == CamAction.ToggleFreecam || PadActive && LiveInView(action)) && !ReplayEditor.Reads(pads[(int)action]);
    }
}
