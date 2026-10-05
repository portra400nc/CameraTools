using UnityEngine;
using static CameraTools.CameraTools;

namespace CameraTools
{
    // A toggle, slider, choice or action row may have Enabled. While it returns false the row is dimmed and ignores input.
    // Any row may have Shown; while it returns false the row is left out and the rows below move up. Detail, when set, is
    // read every frame for a line under a slider's or a choice's label.
    public abstract record Row(string Label)
    {
        public Func<bool> Shown { get; init; }

        public Func<string> Detail { get; init; }

        public bool Visible => Shown?.Invoke() ?? true;

        public bool Usable => (this switch
        {
            ToggleRow toggle => toggle.Enabled,
            SliderRow slider => slider.Enabled,
            ChoiceRow choice => choice.Enabled,
            ActionRow action => action.Enabled,
            _ => null,
        })?.Invoke() ?? true;
    }

    // Note, when set, is read every frame for a line at the right of the header, or null for none.
    public sealed record Section(string Label, Func<string> Note = null) : Row(Label);

    public sealed record ToggleRow(string Label, Func<bool> Get, Action<bool> Set, Func<bool> Enabled = null) : Row(Label);

    // What the Max quality and Min for performance presets set a row to, shown on a second line under its label.
    public sealed record PresetNote(string Max, string Min, bool Restart);

    // A slider's rail: the plain track with a cream fill, or the colours it picks from, with no fill.
    public enum RailLook { Plain, Hue, Temperature }

    public sealed record SliderRow(string Label, float Min, float Max, float Step, string Format,
        Func<float> Get, Action<float> Set, PresetNote Note = null, Func<float, string> Display = null, Func<bool> Enabled = null)
        : Row(Label)
    {
        public RailLook Rail { get; init; }

        public static SliderRow For(string label, Setting setting, float step, string format)
            => new(label, setting.Min, setting.Max, step, format, () => setting.Value, value => setting.Value = value);
    }

    // Get is the shown option's position in Options, or -1 when the current value is none of them.
    public sealed record ChoiceRow(string Label, string[] Options, Func<int> Get, Action<int> Set, PresetNote Note,
        Func<bool> Enabled = null) : Row(Label);

    // DisabledNote shows under the label while Enabled returns false.
    // Confirm, when set, makes the first A arm the row and a second A within a few seconds run it; it names what running
    // does, such as "delete path 2".
    public sealed record ActionRow(string Label, Action Run, string Hint, Func<bool> Enabled = null, string DisabledNote = null,
        Func<string> Confirm = null) : Row(Label);

    // Browses a list whose length changes, showing "2 / 3", or "None" while it is empty. Note, when set, is read every frame
    // for a line under the label.
    public sealed record StepperRow(string Label, Func<int> Count, Func<int> Get, Action<int> Set, Func<string> Note = null) : Row(Label);

    public sealed record ResolutionRow(string Label, int Slot) : Row(Label);

    // A value typed on the keyboard, or with Steam+X on the Deck. Allowed lists the characters it takes, in lower case;
    // Commit gets the typed text, and Tint colours the value while it is not being typed.
    public sealed record TextRow(string Label, Func<string> Get, Action<string> Commit, string Allowed, int MaxLength,
        Func<Color> Tint = null) : Row(Label);

    public sealed record Tab(string Name, Row[] Rows);

    public sealed record Hint(CamAction Action, string Label);

    // Hidden: the free camera is off. Hud: legends and the field of view bar. Playing: a camera path's play bar. Panel:
    // settings. Moving: the sticks move a light, and the legends say how.
    public enum View { Hidden, Hud, Playing, Panel, Moving }

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

        private static readonly string[] KindNotes =
        {
            "Shines in every direction",
            "Shines in a cone where it faces",
            "Soft light with a size and soft shadows, drawn by ReLight",
        };

        private static readonly string[] ReachNotes = { "Ground, scenery and characters", "Only characters; the ground stays as it is" };

        private static readonly string[] FollowNotes =
        {
            "Stays where it was placed",
            "Moves and turns with the camera",
            "Keeps its place beside the character",
        };

        private static Func<bool> LightIs(Func<LightSetup, bool> test) => () => Lights.Current is { } light && test(light);

        private static SliderRow LightSlider(string label, float min, float max, float step, string format, Func<LightSetup, float> get,
            Action<LightSetup, float> set, Func<LightSetup, bool> shown)
            => new(label, min, max, step, format, () => Lights.Current is { } light ? get(light) : min,
                value => Lights.Edit(light => set(light, value)))
            {
                Shown = LightIs(shown),
            };

        private static ChoiceRow LightChoice(string label, string[] options, Func<LightSetup, int> get, Action<int> set,
            Func<LightSetup, bool> shown, string[] notes = null)
            => new(label, options, () => Lights.Current is { } light ? get(light) : 0, set, null)
            {
                Shown = LightIs(shown),
                Detail = notes == null ? null : () => Lights.Current is { } light ? notes[get(light)] : "",
            };

        private static readonly Tab LightsTab = new("Lights", new Row[]
        {
            new Section("Lights"),
            new StepperRow("Light", () => Lights.All.Count, () => Lights.Active, Lights.Select, () => Lights.LightNote),
            new ActionRow("New light", Lights.Add, "Create"),
            new ActionRow("Delete light", Lights.Delete, "Delete", () => Lights.HasLight, Confirm: () => $"delete light {Lights.Active + 1}"),
            new ToggleRow("Show light markers", () => Lights.Markers, Lights.SetMarkers),
            new Section("Placement") { Shown = () => Lights.HasLight },
            LightChoice("Follow", Lights.FollowNames, light => (int)light.Follow, Lights.SetFollow, _ => true, FollowNotes),
            new ActionRow("Move light", Lights.StartMove, "Move") { Shown = () => Lights.HasLight },
            new ActionRow("Move in front of camera", Lights.MoveToCamera, "Move") { Shown = () => Lights.HasLight },
            new Section("Light") { Shown = () => Lights.HasLight },
            LightChoice("Type", Lights.KindNames, light => (int)light.Kind, choice => Lights.Edit(light => light.Kind = (LightKind)choice),
                _ => true, KindNotes),
            LightChoice("Lights up", Lights.ReachNames, light => (int)light.Reach, choice => Lights.Edit(light => light.Reach = (LightReach)choice),
                light => light.Kind != LightKind.Sphere, ReachNotes),
            LightSlider("Intensity", 0f, LightSetup.MaxIntensity, 0.1f, "0.00", light => light.Intensity, (light, value) => light.Intensity = value,
                light => !light.CharactersOnly) with
            {
                Detail = () => Lights.Current?.Kind == LightKind.Sphere ? "2 is a soft fill, 5 is strong" : "The game's lamps use 1 to 4",
            },
            LightSlider("Intensity", 0f, LightSetup.MaxCharacterIntensity, 0.05f, "0.00", light => light.Intensity,
                (light, value) => light.Intensity = value, light => light.CharactersOnly) with
            {
                Detail = () => "1 is a soft fill, 3 is very bright",
            },
            new ToggleRow("Shadows", () => Lights.Current?.Shadows ?? false, on => Lights.Edit(light => light.Shadows = on))
            {
                Shown = LightIs(light => light.Kind != LightKind.Sphere),
            },
            LightSlider("Range", 0.5f, 40f, 0.5f, "0.0' m'", light => light.Range, (light, value) => light.Range = value,
                light => light.Kind != LightKind.Sphere),
            LightSlider("Radius", 0.05f, 2f, 0.05f, "0.00' m'", light => light.Radius, (light, value) => light.Radius = value,
                light => light.Kind == LightKind.Sphere) with
            {
                Detail = () => "Bigger is softer, not brighter",
            },
            LightSlider("Spot angle", 1f, 160f, 1f, "0'°'", light => light.SpotAngle, (light, value) => light.SpotAngle = value,
                light => light.Kind == LightKind.Spot),
            LightSlider("Inner angle", 0f, 160f, 1f, "0'°'", light => light.InnerAngle, (light, value) => light.InnerAngle = value,
                light => light.Kind == LightKind.Spot) with
            {
                Detail = () => "Closer to the spot angle gives a harder edge",
            },
            new Section("Colour") { Shown = () => Lights.HasLight },
            LightChoice("Colour from", Lights.SourceNames, light => (int)light.Source,
                choice => Lights.Edit(light => light.Source = (ColorSource)choice), _ => true),
            LightSlider("Temperature", 1000f, 12000f, 100f, "0' K'", light => light.Kelvin, (light, value) => light.Kelvin = value,
                light => light.Source == ColorSource.Temperature) with
            {
                Detail = () => Lights.Current is { } light ? Colors.KelvinName(light.Kelvin) : "",
                Rail = RailLook.Temperature,
            },
            LightSlider("Hue", 0f, 360f, 5f, "0'°'", light => light.Hue, (light, value) => light.Hue = value,
                light => light.Source == ColorSource.Hue) with
            {
                Rail = RailLook.Hue,
            },
            LightSlider("Saturation", 0f, 100f, 5f, "0'%'", light => light.Saturation, (light, value) => light.Saturation = value,
                light => light.Source == ColorSource.Hue),
            new TextRow("Hex", () => Lights.Current is { } light ? Colors.Hex(light.Color) : "", Lights.SetHex, "0123456789abcdef", 6,
                () => Lights.Current is { } light ? new Color(light.Color.X, light.Color.Y, light.Color.Z, 1f) : Style.Dim)
            {
                Shown = () => Lights.HasLight,
            },
            new Section("Spheres", () => ReShade.Missing ?? (ReLight.Loaded ? null : "Needs iMMERSE ReLight"))
            {
                Shown = () => Lights.HasSpheres,
            },
            new SliderRow("Ambient light", 0f, 1f, 0.05f, "0.00", () => Lights.Ambient, Lights.SetAmbient)
            {
                Shown = () => Lights.HasSpheres,
                Detail = () => "Below 1 dims the game's own light, so the spheres take over",
            },
        });

        // The effects stay on while the user composes a shot; the screenshot turns them off afterwards.
        private static readonly Tab ReShadeTab = new("ReShade", new Row[]
        {
            new Section("Effects", () => ReShade.Missing),
            new ToggleRow("ReShade effects", () => ReShade.Status.EffectsEnabled == 1, ReShade.SetEffects, () => ReShade.Status.Runtime == 1),
        }.Concat(DepthOfField.Rows).Concat(new Row[]
        {
            new Section("Screenshot"),
            new ActionRow("Take screenshot", Screenshot.Take, "Shoot", () => ReShade.Connected, "Needs the ReShade bridge; see Effects"),
            new ToggleRow("3-second countdown", () => Screenshot.Countdown, on => Screenshot.Countdown = on),
        }).ToArray());

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
            return new[] { Camera, World, Paths, LightsTab, new Tab("Graphics", rows.ToArray()), ReShadeTab };
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
