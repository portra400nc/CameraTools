using UnityEngine;
using static CameraTools.CameraTools;

namespace CameraTools
{
    // A toggle, slider, choice or action row may have Enabled. While it returns false the row is dimmed and ignores input.
    // Any row may have Shown; while it returns false the row is left out and the rows below move up. Detail, when set, is
    // read every frame for a line under the label of a slider, a choice, a toggle or an action. A Sub row belongs to the
    // switch above it and starts further in.
    public abstract record Row(string Label)
    {
        public Func<bool> Shown { get; init; }

        public bool Sub { get; init; }

        public Func<string> Detail { get; init; }

        public bool Visible => Shown?.Invoke() ?? true;

        public bool Usable => (this switch
        {
            ToggleRow toggle => toggle.Enabled,
            SliderRow slider => slider.Enabled,
            ChoiceRow choice => choice.Enabled,
            ActionRow action => action.Enabled,
            StepperRow stepper => stepper.Enabled,
            TextRow text => text.Enabled,
            _ => null,
        })?.Invoke() ?? true;
    }

    // Note, when set, is read every frame for a line at the right of the header, or null for none. Columns names the
    // Keybinds tab's keyboard and controller columns over their cells.
    public sealed record Section(string Label, Func<string> Note = null) : Row(Label)
    {
        public bool Columns { get; init; }
    }

    // A hotkey's keyboard key and controller binding, each rebound by clicking it or, for the controller, with A.
    public sealed record BindingRow(string Label, CamAction Action) : Row(Label);

    // A category's heading, which folds the category's rows away: A or a click runs Toggle, and the rows' own Shown hides
    // them. State is read every frame for the line at the heading's right, which shows folded or not.
    public sealed record FoldRow(string Label, Func<bool> Open, Action Toggle, Func<string> State) : Row(Label);

    // Note, when set, is read every frame for what the presets set the switch to, shown under its label in place of a
    // Detail.
    public sealed record ToggleRow(string Label, Func<bool> Get, Action<bool> Set, Func<bool> Enabled = null) : Row(Label)
    {
        public Func<PresetNote> Note { get; init; }
    }

    // A slider's rail: the plain track with a cream fill, or the colours it picks from, with no fill.
    public enum RailLook { Plain, Hue, Temperature }

    // Note, when set, is read every frame for what the presets set the row to, shown under its label.
    public sealed record SliderRow(string Label, float Min, float Max, float Step, string Format,
        Func<float> Get, Action<float> Set, Func<PresetNote> Note = null, Func<float, string> Display = null, Func<bool> Enabled = null)
        : Row(Label)
    {
        public RailLook Rail { get; init; }

        // Read every frame in place of Max, for a range that ends at a value read from the game, as the near clip's does.
        public Func<float> LiveMax { get; init; }

        public float Top => LiveMax != null ? Math.Max(LiveMax(), Min) : Max;

        public static SliderRow For(string label, Setting setting, float step, string format)
            => new(label, setting.Min, setting.Max, step, format, () => setting.Value, value => setting.Value = value);
    }

    // Get is the shown option's position in Options, or -1 when the current value is none of them. Note is as a slider's.
    public sealed record ChoiceRow(string Label, string[] Options, Func<int> Get, Action<int> Set, Func<PresetNote> Note,
        Func<bool> Enabled = null) : Row(Label)
    {
        // Read every frame in place of Options, for a list that changes, such as the active character's expressions.
        public Func<string[]> LiveOptions { get; init; }

        public string[] Choices => LiveOptions?.Invoke() ?? Options;
    }

    // DisabledNote shows under the label while Enabled returns false; a row has it or a Detail, not both.
    // Confirm, when set, makes the first A arm the row and a second A within a few seconds run it; it names what running
    // does, such as "delete path 2".
    public sealed record ActionRow(string Label, Action Run, string Hint, Func<bool> Enabled = null, string DisabledNote = null,
        Func<string> Confirm = null) : Row(Label);

    // Browses a list whose length changes, showing "2 / 3", or "None" while it is empty. Note, when set, is read every frame
    // for a line under the label.
    public sealed record StepperRow(string Label, Func<int> Count, Func<int> Get, Action<int> Set, Func<string> Note = null,
        Func<bool> Enabled = null) : Row(Label)
    {
        // Read every frame for a name to show in place of "2 / 3", or null for the count.
        public Func<string> Display { get; init; }
    }

    public sealed record ResolutionRow(string Label, int Slot) : Row(Label);

    // A value typed on the keyboard, or with Steam+X on the Deck. Allowed lists the characters it takes, in lower case;
    // Commit gets the typed text, and Tint colours the value while it is not being typed. Prefix shows before the text
    // being typed, as # before a colour's hex.
    public sealed record TextRow(string Label, Func<string> Get, Action<string> Commit, string Allowed, int MaxLength,
        Func<Color> Tint = null, Func<bool> Enabled = null) : Row(Label)
    {
        public string Prefix { get; init; } = "";

        // The text typing starts from, to edit a value rather than replace it; null starts empty.
        public Func<string> StartsWithValue { get; init; }

        // Whether Enter on an empty field commits the empty text, as clearing a name does, rather than keeping the value.
        public bool CommitsEmpty { get; init; }
    }

    public sealed record Tab(string Name, Row[] Rows);

    public sealed record Hint(CamAction Action, string Label);

    // Hidden: the free camera is off. Hud: legends and the field of view bar. Playing: the play bar of a camera path, a
    // recording or a replay playing on its own. Panel: settings. Moving: the sticks move a light, and the legends say how.
    // Joints: the sticks turn the posed character's joints, with a marker on each. Editing: a replay held on the timeline.
    public enum View { Hidden, Hud, Playing, Panel, Moving, Joints, Editing }

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
            new SliderRow("Near clip", 1f, 1f, 1f, "0' cm'", () => settings.NearClip * 100f, value => settings.NearClip = value / 100f)
            {
                LiveMax = () => settings.GameNear * 100f,
            },
            SliderRow.For("Damping", settings.Damping, 0.05f, "0.00"),
            new ToggleRow("Remember last position", () => settings.RememberPosition, on => settings.RememberPosition = on),
            new Section("Frame guide"),
            new ToggleRow("Show frame guide", () => FrameGuide.Shown, FrameGuide.SetShown),
            new ChoiceRow("Aspect ratio", FrameGuide.Labels, () => FrameGuide.Choice, FrameGuide.SetChoice, null, () => FrameGuide.Shown),
            new TextRow("Custom ratio", () => FrameGuide.CustomRatio, FrameGuide.SetCustom, "0123456789:.", 9,
                Enabled: () => FrameGuide.CustomChosen),
            new ToggleRow("Portrait", () => FrameGuide.Portrait, FrameGuide.SetPortrait, () => FrameGuide.Turnable)
            {
                Detail = () => "Turns the frame on its side",
            },
            new SliderRow("Outside the frame", 0f, 100f, 5f, "0'%'", () => FrameGuide.Shade * 100f, value => FrameGuide.SetShade(value / 100f),
                Enabled: () => FrameGuide.Shown)
            {
                Detail = () => FrameGuide.ShadeNote,
            },
            new ToggleRow("Rule of thirds", () => FrameGuide.Thirds, FrameGuide.SetThirds, () => FrameGuide.Shown),
        });

        private static readonly Tab World = new("World", new Row[]
        {
            new Section("World"),
            new SliderRow("Game speed", 0f, 10f, 0.1f, "0.00", () => Time.timeScale, SetGameSpeed),
            new ToggleRow("Paused", () => Time.timeScale == 0f, SetPaused),
            new ToggleRow("Pause in free camera", () => PauseInFreecam, SetPauseInFreecam),
            new ToggleRow("Damage numbers", () => DamageNumbers, on => DamageNumbers = on),
            new Section("Time of day"),
            new ToggleRow("Lock time of day", () => TimeOfDay.Locked, TimeOfDay.SetLocked),
            new SliderRow("Time", 0f, 24f, 0.25f, "0.00", () => TimeOfDay.Hour, TimeOfDay.SetHour, Display: TimeOfDay.Clock),
            new SliderRow("Time-lapse speed", 0f, 120f, 1f, "0'×'", () => TimeOfDay.Speed, TimeOfDay.SetSpeed),
            new Section("Weather"),
            new ChoiceRow("Weather", Weather.Labels, () => Weather.Choice, Weather.SetChoice, null),
        });

        // A path or pose name: letters (capitals with Shift), digits, spaces and a little punctuation.
        private const string NameCharacters = "abcdefghijklmnopqrstuvwxyz0123456789 -'";

        private static readonly Tab Paths = new("Paths", new Row[]
        {
            new Section("Path"),
            new StepperRow("Path", () => CameraPaths.Paths.Count, () => CameraPaths.Active, CameraPaths.SelectPath, () => CameraPaths.PathNote)
            {
                Display = () => CameraPaths.HasPath ? CameraPaths.CurrentLabel : null,
            },
            new TextRow("Name", () => CameraPaths.CurrentLabel, CameraPaths.SetName, NameCharacters, 24, Enabled: () => CameraPaths.HasPath)
            {
                Detail = () => "Shown instead of the path number",
                StartsWithValue = () => CameraPaths.Current?.Name ?? "",
                CommitsEmpty = true,
            },
            new ActionRow("New path", CameraPaths.NewPath, "Create"),
            new ActionRow("Delete path", CameraPaths.DeletePath, "Delete", () => CameraPaths.HasPath,
                Confirm: () => $"delete {CameraPaths.CurrentLabel}"),
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

        private static readonly string[] WorldNames = { "Running", "Frozen" };
        private static readonly string[] WorldNotes = { "Wind, water and people keep moving", "Everything else stops: a time-stop shot" };

        // A replay records the party, their effects and the enemies near them, and plays them back on copies, alone or
        // with a camera path, so the camera can film the moment again from other angles.
        private static readonly Tab ReplaysTab = new("Replays", new Row[]
        {
            new Section("Replay"),
            new StepperRow("Replay", () => Replays.All.Count, () => Replays.Active, Replays.Select, () => Replays.Note) { Display = () => Replays.Current?.Name },
            new ActionRow("Record new replay", Replays.Record, "Record") { Detail = () => Replays.RecordNote },
            new ActionRow("Delete replay", Replays.Delete, "Delete", () => Replays.HasReplay, Confirm: () => $"delete {Replays.Current?.Name}"),
            new Section("Recording"),
            new ToggleRow("Play camera path while recording", () => Replays.PlayPath, on => Replays.PlayPath = on)
            {
                Detail = () => CameraPaths.HasPath ? $"{CameraPaths.CurrentLabel}, picked in the Paths tab" : "Pick a path in the Paths tab first",
            },
            new ToggleRow("Record enemies", () => Replays.RecordEnemies, on => Replays.RecordEnemies = on) { Detail = () => "Enemies near you, with their spawns and deaths" },
            new ToggleRow("Record effects", () => Replays.RecordEffects, on => Replays.RecordEffects = on) { Detail = () => "Skills, bursts, hits and energy orbs" },
            new ToggleRow("3-second countdown", () => Replays.Countdown, on => Replays.Countdown = on),
            new Section("Playback"),
            new ActionRow("Edit replay", ReplayEditor.Start, "Edit", () => Replays.HasReplay) { Detail = () => "Scrub the replay and place camera nodes at its moments" },
            new ActionRow("Play replay", Replays.Play, "Play", () => Replays.HasReplay) { Detail = () => Replays.PlayNote },
            new ChoiceRow("Camera path", new[] { "None" }, () => Replays.PathChoice, Replays.SetPathChoice, null, () => Replays.HasReplay)
            {
                LiveOptions = Replays.PathChoices,
                Detail = () => "Playing that path plays this replay with it",
            },
            new SliderRow("Start", 0f, Replays.MaxOffset, 0.1f, "0.0' s after the path'", () => Replays.Current?.Offset ?? 0f, Replays.SetOffset,
                Enabled: () => Replays.PathNumber(Replays.Current) != null),
            new ChoiceRow("World while playing", WorldNames, () => (int)(Replays.Current?.World ?? ReplayWorld.Running),
                choice => Replays.SetWorld((ReplayWorld)choice), null, () => Replays.HasReplay)
            {
                Detail = () => WorldNotes[(int)(Replays.Current?.World ?? ReplayWorld.Running)],
            },
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
                Shown = LightIs(light => light.Kind == LightKind.Spot),
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
            new TextRow("Hex", () => Lights.Current is { } light ? Colors.Hex(light.Color) : "", Lights.SetHex, "0123456789ABCDEF", 6,
                () => Lights.Current is { } light ? new Color(light.Color.X, light.Color.Y, light.Color.Z, 1f) : Style.Dim)
            {
                Prefix = "#",
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

        private static readonly Func<bool> Posed = () => Posing.On;

        private static readonly string[] LookNames = { "Ahead", "At the camera", "By hand" };
        private static readonly string[] LookNotes = { "Straight ahead from the head", "Follow the camera as it moves", "Set with the two rows below" };
        private static readonly string[] HeadNames = { "As posed", "At the camera" };

        private static string Count(int count, string noun) => $"{count} {noun}{(count == 1 ? "" : "s")}";

        private static string SavedNote()
        {
            if (Poses.Current is not { } saved)
                return "Save a pose to keep it";
            return $"{Poses.Active + 1} of {Poses.All.Count} · {Count(saved.Setup.PosedJoints, "joint")} posed"
                + (Posing.Changed ? " · the current pose has changes" : "");
        }

        private static string JointNote()
        {
            var info = Posing.Selected;
            string state = !Posing.Has(info.Target) ? "not on this character"
                : Posing.Current.Turn(info.Target).IsZero ? "as the game posed it"
                : "posed";
            return $"{info.Bone}{(info.Target.IsStrand ? " and the bones below it" : "")} · {state}";
        }

        private static readonly string[] AxesNames = { "The joint itself", "The character" };

        // notes holds what the row turns about in each JointAxes.
        private static SliderRow JointSlider(string label, float max, Func<JointTurn, float> get, Func<JointTurn, float, JointTurn> set, string[] notes)
            => new(label, -max, max, 5f, "0'°'", () => get(Posing.Current.Turn(Posing.Selected.Target)),
                value => Posing.Edit(pose => pose.SetTurn(Posing.Selected.Target, set(pose.Turn(Posing.Selected.Target), MathF.Round(value)))),
                Enabled: () => Posing.CanTurnJoint)
            {
                Detail = () => notes[(int)Posing.Current.Axes],
            };

        private static string HairNote()
        {
            float follow = Posing.Current.HairFollow;
            return !Posing.HasHairStrands ? "This character has no hair strands"
                : follow >= 100f ? "Moves with the head, as the game holds it"
                : follow <= 0f ? "Keeps the direction it had before posing"
                : "Lower keeps the hair off the body when the head bows or turns";
        }

        private static ChoiceRow HandChoice(string label, Side side)
            => new(label, HandShapes.Names, () => (int)Posing.Current.Hand(side).Shape,
                choice => Posing.Edit(pose => pose.SetHand(side, pose.Hand(side).WithShape((HandShape)choice))), null,
                () => Posing.On && Posing.HasHand(side))
            {
                Detail = () => !Posing.HasHand(side) ? "This character's hand has no finger bones"
                    : Posing.Current.Hand(side).Shape == HandShape.Game ? "As the game posed it"
                    : $"Sets Bip001 {side.Letter()} Finger0 to Finger42",
            };

        private static bool OnThumb() => Fingers.At(Posing.Finger).Finger == Fingers.Thumb;

        // The thumb's joints stop at other limits than the fingers', so each of these rows comes in two: one shown while a
        // finger is picked and one while the thumb is.
        private static SliderRow LimitedSlider(string label, bool thumb, Func<FingerLimits, (float Min, float Max)> range, float step, string format,
            Func<FingerPose, float> get, Func<FingerPose, float, FingerPose> set, Func<string> note)
        {
            var (min, max) = range(Fingers.Limits(thumb ? Fingers.Thumb : Fingers.Thumb + 1));
            return FingerSlider(label, min, max, step, format, get, set, note) with { Shown = () => OnThumb() == thumb };
        }

        // A thumb-only row is dimmed while another finger is selected.
        private static SliderRow FingerSlider(string label, float min, float max, float step, string format, Func<FingerPose, float> get,
            Func<FingerPose, float, FingerPose> set, Func<string> note, bool thumbOnly = false)
        {
            return new SliderRow(label, min, max, step, format, () =>
                {
                    var (side, index) = Fingers.At(Posing.Finger);
                    return get(Posing.Current.Hand(side).Finger(index));
                },
                value => Posing.Edit(pose =>
                {
                    var (side, index) = Fingers.At(Posing.Finger);
                    var hand = pose.Hand(side);
                    pose.SetHand(side, hand.WithFinger(index, set(hand.Finger(index), value)));
                }),
                Enabled: () => Posing.On && Posing.HasHand(Fingers.At(Posing.Finger).Side) && (!thumbOnly || OnThumb()))
            {
                Detail = () => thumbOnly && !OnThumb() ? "For the thumb" : note(),
            };
        }

        private static string FingerName()
        {
            var (side, finger) = Fingers.At(Posing.Finger);
            return $"{(side == Side.Left ? "Left" : "Right")} {Fingers.Names[finger]}";
        }

        // The selected finger's bone at a joint, the knuckle's with its full name.
        private static string FingerBone(int joint)
        {
            var (side, finger) = Fingers.At(Posing.Finger);
            return joint == 0 ? Fingers.Bone(side, finger, 0) : $"Finger{finger}{joint}";
        }

        private static string ShapeNote(string shape, Func<string, string> sets, Func<string, bool> has)
            => shape == null ? "From the expression" : has(shape) ? sets(shape) : $"This character has no {shape}";

        private static ChoiceRow FaceRow(string label, FaceChoice[] choices, Func<FacePose, string> get, Func<FacePose, string, FacePose> set,
            Func<string, string> sets, Func<string, bool> has)
            => new(label, FaceChoices.Labels(choices), () => FaceChoices.IndexOf(choices, get(Posing.Current.Face)),
                choice => Posing.Edit(pose => pose.Face = set(pose.Face, choices[choice].Shape)), null, Posed)
            {
                Detail = () => ShapeNote(get(Posing.Current.Face), sets, has),
            };

        private static SliderRow ClosedSlider(string label, Side side)
        {
            string wink = FaceChoices.Wink('A', side);
            return new SliderRow(label, 0f, 100f, 5f, "0'%'", () => Posing.Current.Face.Closed(side),
                value => Posing.Edit(pose => pose.Face = side == Side.Left ? pose.Face with { LeftClosed = value } : pose.Face with { RightClosed = value }),
                Enabled: () => Posing.On && Posing.HasShape(wink))
            {
                Detail = () => Posing.HasShape(wink) ? $"Sets {wink}" : $"This character has no {wink}",
            };
        }

        private static string ExpressionNote()
        {
            string expression = Posing.Current.Face.Expression;
            return expression == null ? "Whatever the game shows" : $"{expression}, one of this character's {Posing.Emotions.Length}";
        }

        private static SliderRow GazeSlider(string label, float max, float step, string format, Func<GazePose, float> get,
            Func<GazePose, float, GazePose> set, string note)
            => new(label, -max, max, step, format, () => get(Posing.Current.Gaze), value => Posing.Edit(pose => pose.Gaze = set(pose.Gaze, value)),
                Enabled: () => Posing.On && Posing.HasEyes && Posing.Current.Gaze.Eyes == EyesLook.ByHand)
            {
                Detail = () => note,
            };

        // The face rows set the game's blend shapes by name on top of the expression; a row's note says when this character
        // lacks its shape.
        private static readonly Tab PoseTab = new("Pose", new Row[]
        {
            new Section("Pose"),
            new ToggleRow("Posing", () => Posing.On, Posing.SetOn),
            new ActionRow("Back to the game's pose", Posing.ResetPose, "Reset", Posed),
            new ActionRow("Bring back last pose", Posing.RestoreLast, "Restore", () => Poses.Last != null)
            {
                Detail = () => Poses.Last != null ? "The pose from before posing ended" : "Keeps the pose when posing ends by accident",
            },
            new Section("Saved poses"),
            new StepperRow("Saved pose", () => Poses.All.Count, () => Poses.Active, Poses.Select, SavedNote) { Display = () => Poses.Current?.Name },
            new TextRow("Name", () => Poses.Current?.Name ?? "None", Poses.Rename, NameCharacters, 24, Enabled: () => Poses.Current != null)
            {
                Detail = () => "Renames the saved pose",
                StartsWithValue = () => Poses.Current?.Name ?? "",
                CommitsEmpty = true,
            },
            new ActionRow("Load pose", Posing.LoadSaved, "Load", () => Poses.Current != null),
            new ActionRow("Save as new pose", Posing.SaveNew, "Save", Posed),
            new ActionRow("Save over this pose", Posing.SaveOver, "Save", () => Posing.On && Poses.Current != null),
            new ActionRow("Delete pose", Posing.DeleteSaved, "Delete", () => Poses.Current != null, Confirm: () => $"delete {Poses.Current?.Name}"),
            new Section("Body"),
            new ActionRow("Pose joints", Posing.StartEditing, "Pose", Posed) { Detail = () => "Pick joints on the character and turn them with the sticks" },
            new StepperRow("Joint", () => Posing.Targets.Count, () => Posing.Joint, Posing.SelectJoint, JointNote, Posed)
            {
                Display = () => Posing.Selected.Name,
            },
            new ChoiceRow("Turn around", AxesNames, () => (int)Posing.Current.Axes, choice => Posing.SetAxes((JointAxes)choice), null, Posed)
            {
                Detail = () => Posing.Current.Axes == JointAxes.Character ? "Bend, Turn and Twist use the character's own axes"
                    : "Bend, Turn and Twist follow the joint, whichever way it points",
            },
            JointSlider("Bend", JointTurn.MaxBend, turn => turn.Bend, (turn, value) => turn with { Bend = value },
                new[] { "Folds the joint, as an elbow or a knee folds", "About the character's side-to-side axis" }),
            JointSlider("Turn", JointTurn.MaxTurn, turn => turn.Turn, (turn, value) => turn with { Turn = value },
                new[] { "Swings the joint to the side", "About the character's up axis" }),
            JointSlider("Twist", JointTurn.MaxTwist, turn => turn.Twist, (turn, value) => turn with { Twist = value },
                new[] { "Spins the joint along its own length", "About the character's front-to-back axis" }),
            new ActionRow("Copy to the other side", Posing.MirrorJoint, "Copy", () => Posing.On && Posing.MirrorIndex != null),
            new ActionRow("Reset joint", Posing.ResetJoint, "Reset", Posed),
            new Section("Hands"),
            HandChoice("Left hand", Side.Left),
            HandChoice("Right hand", Side.Right),
            new StepperRow("Finger", () => Fingers.Selectable, () => Posing.Finger, Posing.SelectFinger, () => "Pick a finger, then set its joints below", Posed)
            {
                Display = FingerName,
            },
            LimitedSlider("Curl", false, limits => (0f, limits.MaxBend), 5f, "0'%'", finger => finger.Curl, (finger, value) => finger.WithCurl(value),
                () => "Bends all three joints together; 0 is straight"),
            LimitedSlider("Curl", true, limits => (0f, limits.MaxBend), 5f, "0'%'", finger => finger.Curl, (finger, value) => finger.WithCurl(value),
                () => "Bends all three joints together; 0 is straight"),
            LimitedSlider("Knuckle", false, limits => (limits.MinBend, limits.MaxBend), 5f, "0'%'", finger => finger.Base, (finger, value) => finger with { Base = value },
                () => $"Bends {FingerBone(0)}; below 0 bends it back"),
            LimitedSlider("Knuckle", true, limits => (limits.MinBend, limits.MaxBend), 5f, "0'%'", finger => finger.Base, (finger, value) => finger with { Base = value },
                () => $"Bends {FingerBone(0)}; below 0 bends it back"),
            LimitedSlider("Middle joint", false, limits => (limits.MinBend, limits.MaxBend), 5f, "0'%'", finger => finger.Middle, (finger, value) => finger with { Middle = value },
                () => $"Bends {FingerBone(1)}"),
            LimitedSlider("Middle joint", true, limits => (limits.MinBend, limits.MaxBend), 5f, "0'%'", finger => finger.Middle, (finger, value) => finger with { Middle = value },
                () => $"Bends {FingerBone(1)}"),
            LimitedSlider("Tip", false, limits => (limits.MinBend, limits.MaxBend), 5f, "0'%'", finger => finger.Tip, (finger, value) => finger with { Tip = value },
                () => $"Bends {FingerBone(2)}"),
            LimitedSlider("Tip", true, limits => (limits.MinBend, limits.MaxBend), 5f, "0'%'", finger => finger.Tip, (finger, value) => finger with { Tip = value },
                () => $"Bends {FingerBone(2)}"),
            LimitedSlider("Spread", false, limits => (limits.MinSpread, limits.MaxSpread), 1f, "0'°'", finger => finger.Spread, (finger, value) => finger with { Spread = value },
                () => "Moves the finger toward the thumb or away from it"),
            LimitedSlider("Spread", true, limits => (limits.MinSpread, limits.MaxSpread), 1f, "0'°'", finger => finger.Spread, (finger, value) => finger with { Spread = value },
                () => "Moves the finger toward the thumb or away from it"),
            FingerSlider("Across the palm", 0f, 100f, 5f, "0'%'", finger => finger.Across, (finger, value) => finger with { Across = value },
                () => "Swings the thumb in front of the palm, toward the fingertips", thumbOnly: true),
            FingerSlider("Thumb twist", -Fingers.Limits(Fingers.Thumb).MaxTwist, Fingers.Limits(Fingers.Thumb).MaxTwist, 5f, "0'°'", finger => finger.Twist,
                (finger, value) => finger with { Twist = value }, () => "Turns the thumb's pad toward the fingers or away", thumbOnly: true),
            new Section("Face"),
            new ChoiceRow("Expression", new[] { "Game's" }, () => Array.IndexOf(Posing.Emotions, Posing.Current.Face.Expression) + 1,
                choice => Posing.Edit(pose => pose.Face = pose.Face with { Expression = choice == 0 ? null : Posing.Emotions[choice - 1] }), null, Posed)
            {
                LiveOptions = () => Posing.ExpressionNames,
                Detail = ExpressionNote,
            },
            FaceRow("Mouth", FaceChoices.Mouths, face => face.Mouth, (face, shape) => face with { Mouth = shape }, shape => $"Sets {shape}", Posing.HasShape),
            ClosedSlider("Left eye closed", Side.Left),
            ClosedSlider("Right eye closed", Side.Right),
            FaceRow("Eyes", FaceChoices.Eyes, face => face.Eyes, (face, shape) => face with { Eyes = shape }, shape => $"Sets {shape}", Posing.HasShape),
            FaceRow("Brows", FaceChoices.Brows, face => face.Brows, (face, shape) => face with { Brows = shape }, pair => $"Sets {pair}_L and _R",
                Posing.HasBrows),
            new ToggleRow("Blinking", () => Posing.Current.Face.Blink, on => Posing.Edit(pose => pose.Face = pose.Face with { Blink = on }), Posed)
            {
                Detail = () => Posing.Current.Face.Blink ? "Blinks now and then, so a shot can catch it" : "Eyes stay as set",
            },
            new Section("Gaze"),
            new ChoiceRow("Eyes look", LookNames, () => (int)Posing.Current.Gaze.Eyes,
                choice => Posing.Edit(pose => pose.Gaze = pose.Gaze with { Eyes = (EyesLook)choice }), null, Posed)
            {
                Detail = () => LookNotes[(int)Posing.Current.Gaze.Eyes],
            },
            GazeSlider("Eyes left and right", GazePose.MaxX, 1f, "0'°'", gaze => gaze.X, (gaze, value) => gaze with { X = value },
                "Up to 17°, the game's own eye range"),
            GazeSlider("Eyes up and down", GazePose.MaxY, 0.5f, "0.0'°'", gaze => gaze.Y, (gaze, value) => gaze with { Y = value },
                "Up to 6.5°, the game's own eye range"),
            new ChoiceRow("Head", HeadNames, () => Posing.Current.Gaze.HeadAtCamera ? 1 : 0,
                choice => Posing.Edit(pose => pose.Gaze = pose.Gaze with { HeadAtCamera = choice == 1 }), null, () => Posing.On && Posing.Has(PoseJoint.Head))
            {
                Detail = () => Posing.Current.Gaze.HeadAtCamera ? "Turns toward the camera; the Head joint waits" : "Set with the Head joint",
            },
            new Section("Hair"),
            new SliderRow("Hair follows head", 0f, 100f, 5f, "0'%'", () => Posing.Current.HairFollow, value => Posing.Edit(pose => pose.HairFollow = value),
                Enabled: () => Posing.On && Posing.HasHairStrands)
            {
                Detail = HairNote,
            },
        });

        // The effects stay on while the user composes a shot; the screenshot turns them off afterwards.
        private static readonly Tab ReShadeTab = new("ReShade", new Row[]
        {
            new Section("Effects", () => ReShade.Missing),
            new ToggleRow("ReShade effects", () => ReShade.Status.EffectsEnabled == 1, ReShade.SetEffects, () => ReShade.Status.Runtime == 1),
        }.Concat(DepthOfField.Rows).Concat(LongExposure.Rows).Concat(new Row[]
        {
            new Section("Screenshot"),
            new ActionRow("Take screenshot", Screenshot.Take, "Shoot", () => ReShade.Connected)
            {
                Detail = () => !ReShade.Connected ? "Needs the ReShade bridge; see Effects"
                    : LongExposure.Active ? $"Records {LongExposure.Current.ShutterLabel}, then saves" : "",
            },
            new ToggleRow("3-second countdown", () => Screenshot.Countdown, on => Screenshot.Countdown = on),
            new ToggleRow("Supersampling", () => Supersampling.Enabled, Supersampling.SetEnabled, () => CameraToolsEnhancements.Enhancements.Connected),
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
                new ActionRow("Highest", () => Graphics.Apply(PresetSlot.Highest), "Apply"),
                new ActionRow("Lowest", () => Graphics.Apply(PresetSlot.Lowest), "Apply"),
                new ActionRow("Restore my settings", Graphics.Restore, "Restore"),
                new ActionRow("Save current settings as Highest", () => Graphics.Save(PresetSlot.Highest), "Save"),
                new ActionRow("Save current settings as Lowest", () => Graphics.Save(PresetSlot.Lowest), "Save"),
                new ActionRow("Reset presets", Graphics.ResetPresets, "Reset"),
                new Section("Resolution"),
                new ResolutionRow("Slot 1", 1),
                new ResolutionRow("Slot 2", 2),
            };
            rows.AddRange(EnhancementsControl.Rows());
            var gameSettings = Graphics.SettingRows().ToArray();
            if (gameSettings.Length > 0)
                rows.Add(new Section("Game settings"));
            rows.AddRange(gameSettings);
            rows.Add(new Section("Beyond the game's limits"));
            rows.AddRange(Graphics.BeyondRows());
            return new[] { Camera, World, Paths, ReplaysTab, LightsTab, PoseTab, new Tab("Graphics", rows.ToArray()), ReShadeTab, KeybindsTab };
        }

        private static bool pathKeysOpen;

        // The README's groups. Select path 1 to 9 folds away, so the list stays short.
        private static readonly Tab KeybindsTab = new("Keybinds", new Row[]
        {
            new ActionRow("Restore defaults", CameraUi.RestoreKeybinds, "Restore", Confirm: () => "restore every keybind"),
            new Section("Free camera") { Columns = true },
            new BindingRow("Toggle free camera", CamAction.ToggleFreecam),
            new BindingRow("Move forward", CamAction.Forward),
            new BindingRow("Move back", CamAction.Back),
            new BindingRow("Move left", CamAction.Left),
            new BindingRow("Move right", CamAction.Right),
            new BindingRow("Move up", CamAction.Up),
            new BindingRow("Move down", CamAction.Down),
            new BindingRow("Roll left", CamAction.RollLeft),
            new BindingRow("Roll right", CamAction.RollRight),
            new BindingRow("Reset roll", CamAction.ResetRoll),
            new BindingRow("Fast movement (hold)", CamAction.FastMovement),
            new BindingRow("Slow movement (hold)", CamAction.SlowMovement),
            new BindingRow("Field of view down", CamAction.DecreaseFOV),
            new BindingRow("Field of view up", CamAction.IncreaseFOV),
            new BindingRow("Reset field of view", CamAction.ResetFOV),
            new Section("Game speed") { Columns = true },
            new BindingRow("Toggle pause", CamAction.TogglePause),
            new BindingRow("Reset game speed", CamAction.ResetSpeed),
            new BindingRow("Speed +0.1", CamAction.SpeedInc1),
            new BindingRow("Speed -0.1", CamAction.SpeedDec1),
            new BindingRow("Speed +0.5", CamAction.SpeedInc5),
            new BindingRow("Speed -0.5", CamAction.SpeedDec5),
            new BindingRow("Toggle speed 5.0", CamAction.ToggleSpeedTo5),
            new Section("Interface") { Columns = true },
            new BindingRow("Settings panel", CamAction.ToggleGUI),
            new BindingRow("Free the cursor", CamAction.ToggleCursorFocus),
            new BindingRow("Hide UI", CamAction.ToggleHUD),
            new BindingRow("Apply resolution slot 1", CamAction.SetResolutionTo1080p),
            new BindingRow("Apply resolution slot 2", CamAction.SetResolutionTo4K),
            new Section("Camera paths and replays") { Columns = true },
            new BindingRow("Add path node", CamAction.AddPathNode),
            new BindingRow("Play or stop path", CamAction.PlayPath),
            new FoldRow("Select path 1 to 9", () => pathKeysOpen, () => pathKeysOpen = !pathKeysOpen, () => pathKeysOpen ? "" : "9 keybinds"),
        }
            .Concat(Enumerable.Range(0, 9).Select(index =>
                new BindingRow($"Select path {index + 1}", CamAction.SelectPath1 + index) { Sub = true, Shown = () => pathKeysOpen }))
            .Concat(new Row[]
            {
                new BindingRow("Record replay", CamAction.RecordReplay),
                new BindingRow("Play or stop replay", CamAction.PlayReplay),
                new Section("Depth of field") { Columns = true },
                new BindingRow("Center focus point", CamAction.CenterFocusPoint),
            })
            .ToArray());

        // What the Keybinds tab calls an action, for the toast that names the action a swap changed.
        public static string ActionLabel(CamAction action)
            => KeybindsTab.Rows.OfType<BindingRow>().First(row => row.Action == action).Label;

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
