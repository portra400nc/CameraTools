using UnityEngine;
using static CameraTools.CameraTools;

namespace CameraTools
{
    public abstract record Row(string Label);

    public sealed record Section(string Label) : Row(Label);

    public sealed record ToggleRow(string Label, Func<bool> Get, Action<bool> Set) : Row(Label);

    // What the Max quality and Min for performance presets set a row to, shown on a second line under its label.
    public sealed record PresetNote(string Max, string Min, bool Restart);

    public sealed record SliderRow(string Label, float Min, float Max, float Step, string Format,
        Func<float> Get, Action<float> Set, PresetNote Note = null, Func<float, string> Display = null) : Row(Label)
    {
        public static SliderRow For(string label, Setting setting, float step, string format)
            => new(label, setting.Min, setting.Max, step, format, () => setting.Value, value => setting.Value = value);
    }

    // Get is the shown option's position in Options, or -1 when the current value is none of them.
    public sealed record ChoiceRow(string Label, string[] Options, Func<int> Get, Action<int> Set, PresetNote Note) : Row(Label);

    // Enabled, when set, dims the row and ignores A while it returns false, and DisabledNote then shows under the label.
    // Confirm, when set, makes the first A arm the row and a second A within a few seconds run it; it names what running
    // does, such as "delete path 2".
    public sealed record ActionRow(string Label, Action Run, string Hint, Func<bool> Enabled = null, string DisabledNote = null,
        Func<string> Confirm = null) : Row(Label);

    // Browses a list whose length changes, showing "2 / 3", or "None" while it is empty. Note, when set, is read every frame
    // for a line under the label.
    public sealed record StepperRow(string Label, Func<int> Count, Func<int> Get, Action<int> Set, Func<string> Note = null) : Row(Label);

    public sealed record ResolutionRow(string Label, int Slot) : Row(Label);

    public sealed record Tab(string Name, Row[] Rows);

    public sealed record Hint(CamAction Action, string Label);

    // Hidden: the free camera is off. Hud: legends and the field of view bar. Playing: a camera path's play bar. Panel:
    // settings.
    public enum View { Hidden, Hud, Playing, Panel }

    internal static class UiModel
    {
        private static readonly Tab Camera = new("Camera", new Row[]
        {
            new Section("Camera"),
            SliderRow.For("Movement speed", settings.MoveSpeed, 0.05f, "0.000"),
            SliderRow.For("Look sensitivity", settings.LookSensitivity, 0.1f, "0.0"),
            SliderRow.For("Roll speed", settings.RollSpeed, 0.1f, "0.0"),
            SliderRow.For("Zoom speed", settings.FovSpeed, 0.05f, "0.00"),
            SliderRow.For("Field of view", settings.Fov, 1f, "0.0"),
            SliderRow.For("Damping", settings.Damping, 0.05f, "0.00"),
            new ToggleRow("Remember last position", () => settings.RememberPosition, on => settings.RememberPosition = on),
        });

        private static readonly Tab World = new("World", new Row[]
        {
            new Section("World"),
            new SliderRow("Game speed", 0f, 10f, 0.1f, "0.00", () => Time.timeScale, SetGameSpeed),
            new ToggleRow("Paused", () => Time.timeScale == 0f, SetPaused),
            new ToggleRow("Max detail", () => Lod.MaxDetail, Lod.SetMaxDetail),
            new ToggleRow("Damage numbers", () => DamageNumbers, on => DamageNumbers = on),
            new Section("Time of day"),
            new ToggleRow("Lock time of day", () => TimeOfDay.Locked, TimeOfDay.SetLocked),
            new SliderRow("Time", 0f, 24f, 0.25f, "0.00", () => TimeOfDay.Hour, TimeOfDay.SetHour, Display: TimeOfDay.Clock),
            new SliderRow("Time-lapse speed", 0f, 120f, 1f, "0'×'", () => TimeOfDay.Speed, TimeOfDay.SetSpeed),
            new Section("Weather"),
            new ChoiceRow("Weather", Weather.Labels, () => Weather.Choice, Weather.SetChoice, null),
        });

        private static readonly Tab Paths = new("Paths", new Row[]
        {
            new Section("Path"),
            new StepperRow("Path", () => CameraPaths.Paths.Count, () => CameraPaths.Active, CameraPaths.SelectPath, () => CameraPaths.PathNote),
            new ActionRow("New path", CameraPaths.NewPath, "Create"),
            new ActionRow("Delete path", CameraPaths.DeletePath, "Delete", () => CameraPaths.HasPath,
                Confirm: () => $"delete path {CameraPaths.Active + 1}"),
            new Section("Nodes"),
            new StepperRow("Node", () => CameraPaths.Current?.Nodes.Count ?? 0, () => CameraPaths.Node, CameraPaths.SelectNode),
            new ActionRow("Add node at end", CameraPaths.AddNode, "Add"),
            new ActionRow("Insert before this node", CameraPaths.InsertBefore, "Insert", () => CameraPaths.HasNode),
            new ActionRow("Insert after this node", CameraPaths.InsertAfter, "Insert", () => CameraPaths.HasNode),
            new ActionRow("Replace with current view", CameraPaths.ReplaceNode, "Replace", () => CameraPaths.HasNode),
            new ActionRow("Go to node", CameraPaths.GoToNode, "Go", () => CameraPaths.HasNode),
            new ActionRow("Delete node", CameraPaths.DeleteNode, "Delete", () => CameraPaths.HasNode),
            new Section("Playback"),
            new ActionRow("Play", PathPlayback.Play, "Play", () => CameraPaths.CanPlay, "Add at least 2 nodes"),
            new SliderRow("Duration", CameraPath.MinDuration, CameraPath.MaxDuration, 0.5f, "0.0' s'", () => CameraPaths.Duration, CameraPaths.SetDuration),
            PathToggle("Loop", options => options.Loop, (options, on) => options.Loop = on),
            PathToggle("Constant speed", options => options.ConstantSpeed, (options, on) => options.ConstantSpeed = on),
            PathToggle("Ease in", options => options.EaseIn, (options, on) => options.EaseIn = on),
            PathToggle("Ease out", options => options.EaseOut, (options, on) => options.EaseOut = on),
            PathToggle("Unpause game while playing", options => options.UnpauseGame, (options, on) => options.UnpauseGame = on),
            PathToggle("Hide UI while playing", options => options.HideUi, (options, on) => options.HideUi = on),
            PathToggle("3-second countdown", options => options.Countdown, (options, on) => options.Countdown = on),
            new Section("Shake"),
            Shake("Movement frequency", options => options.MoveShakeFrequency, (options, value) => options.MoveShakeFrequency = value),
            Shake("Rotation frequency", options => options.RotateShakeFrequency, (options, value) => options.RotateShakeFrequency = value),
            Shake("Movement strength", options => options.MoveShakeStrength, (options, value) => options.MoveShakeStrength = value),
            Shake("Rotation strength", options => options.RotateShakeStrength, (options, value) => options.RotateShakeStrength = value),
        });

        private static ToggleRow PathToggle(string label, Func<PathOptions, bool> get, Action<PathOptions, bool> set)
            => new(label, () => get(CameraPaths.Options), on => CameraPaths.ChangeOptions(options => set(options, on)));

        // A held mouse drag sets the slider every frame, and each change is saved.
        private static SliderRow Shake(string label, Func<PathOptions, float> get, Action<PathOptions, float> set)
            => new(label, 0f, PathOptions.MaxShake, 0.05f, "0.00", () => get(CameraPaths.Options), value =>
            {
                if (value != get(CameraPaths.Options))
                    CameraPaths.ChangeOptions(options => set(options, value));
            });

        // The Graphics tab's game settings rows depend on the option lists the game offers on this machine, so the tabs are
        // built with the UI.
        public static Tab[] Tabs()
        {
            var rows = new List<Row>
            {
                new Section("Presets"),
                new ActionRow("Max quality (screenshots)", () => Graphics.Apply(Graphics.Screenshot), "Apply"),
                new ActionRow("Min for performance", () => Graphics.Apply(Graphics.Performance), "Apply"),
                new ActionRow("Restore my settings", Graphics.Restore, "Restore"),
                new Section("Resolution"),
                new ResolutionRow("Slot 1", 1),
                new ResolutionRow("Slot 2", 2),
            };
            var gameSettings = Graphics.SettingRows().ToArray();
            if (gameSettings.Length > 0)
                rows.Add(new Section("Game settings"));
            rows.AddRange(gameSettings);
            rows.Add(new Section("Beyond the game's limits"));
            rows.AddRange(Graphics.BeyondRows());
            return new[] { Camera, World, Paths, new Tab("Graphics", rows.ToArray()) };
        }

        public static readonly Hint[] BottomHints =
        {
            new(CamAction.ToggleFreecam, "Leave"),
            new(CamAction.ToggleHUD, "Hide UI"),
            new(CamAction.TogglePause, "Pause"),
            new(CamAction.ResetSpeed, "Reset speed"),
            new(CamAction.ToggleGUI, "Settings"),
        };

        public static readonly Hint[] TopHints =
        {
            new(CamAction.Down, "Down"),
            new(CamAction.Up, "Up"),
            new(CamAction.RollLeft, "Roll left"),
            new(CamAction.RollRight, "Roll right"),
        };
    }
}
