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
        Func<float> Get, Action<float> Set, PresetNote Note = null) : Row(Label)
    {
        public static SliderRow For(string label, Setting setting, float step, string format)
            => new(label, setting.Min, setting.Max, step, format, () => setting.Value, value => setting.Value = value);
    }

    // Get is the shown option's position in Options, or -1 when the current value is none of them.
    public sealed record ChoiceRow(string Label, string[] Options, Func<int> Get, Action<int> Set, PresetNote Note) : Row(Label);

    public sealed record ActionRow(string Label, Action Run, string Hint) : Row(Label);

    public sealed record ResolutionRow(string Label, int Slot) : Row(Label);

    public sealed record Tab(string Name, Row[] Rows);

    public sealed record Hint(CamAction Action, string Label);

    // Hidden: the free camera is off. Hud: legends and the field of view bar. Panel: settings.
    public enum View { Hidden, Hud, Panel }

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
            return new[] { Camera, World, new Tab("Graphics", rows.ToArray()) };
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
