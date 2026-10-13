using CameraToolsEnhancements;
using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // The Graphics tab's Enhancements category, and the only owner of the add-on's settings. Each switch and value is an
    // entry of the CameraToolsEnhancements preferences: a change goes to the add-on at once and is saved a moment later,
    // and the add-on gets the saved values whenever ReShade loads it, which is some time after the game starts.
    // EnhancementsModel holds the rows and the rules.
    internal static class EnhancementsControl
    {
        private const string Category = "CameraToolsEnhancements";
        // How long after the last change the preferences are written and the log names the values: a dragged slider
        // changes them every frame.
        private const float SaveDelay = 1f;

        // By field: writes a value to the field's entry.
        private static readonly Action<float>[] writers = new Action<float>[EnhancementsModel.Fields.Length];
        private static MelonPreferences_Entry<bool> folded;
        // The user's own values, which the preferences keep, and a screenshot's preset's while it runs them.
        private static EnhancementRun run = new(EnhancementValues.Defaults);
        private static int connections;
        private static float? saveAt;
        private static bool unsaved;
        // The heading's text for the values it was made from.
        private static (EnhancementValues Of, string Text) summary;

        // The user's own values.
        public static EnhancementValues Values => run.Own;

        private static EnhancementValues Running => run.Running;

        public static bool Folded => folded.Value;

        // The right of the heading: why the add-on cannot run the effects, or whether anything is switched on.
        private static string State
        {
            get
            {
                if (Enhancements.Missing is { } missing)
                    return missing;
                if (!ReferenceEquals(summary.Of, Values))
                    summary = (Values, EnhancementsModel.Summary(Values));
                return summary.Text;
            }
        }

        // The category's rows, as the mockup lists them: the heading that folds them, the master switch, each effect's
        // switch with the effect's own rows under it, and the reset.
        public static IEnumerable<Row> Rows()
        {
            yield return new FoldRow("Enhancements", () => !Folded, () => SetFolded(!Folded), () => State);
            foreach (var field in EnhancementsModel.Fields)
            {
                Func<bool> shown = () => EnhancementsModel.Shown(field, Values, Folded);
                Func<bool> usable = field.Id == Enhancement.Enabled ? null : () => EnhancementsModel.Usable(field, Values);
                if (field.Kind == EnhancementKind.Switch)
                {
                    // The line that says what the presets set a switch to comes with a saved preset and makes the row
                    // taller, so each switch has a row without the line and a row with it.
                    var plain = new ToggleRow(field.Label, () => Values.On(field.Id), on => Change(field.Id, on ? 1f : 0f), usable)
                    {
                        Shown = () => shown() && !Graphics.PresetsEnhance,
                    };
                    yield return plain;
                    yield return plain with { Shown = () => shown() && Graphics.PresetsEnhance, Note = () => Graphics.SwitchNote(field.Id) };
                    continue;
                }
                yield return field.Kind switch
                {
                    EnhancementKind.Choice => new ChoiceRow(field.Label, field.Options, () => (int)Values[field.Id], position => Change(field.Id, position), null, usable)
                    {
                        Shown = shown, Sub = true,
                    },
                    _ => new SliderRow(field.Label, field.Min, field.Max, field.Step, field.Format, () => Values[field.Id], value => Change(field.Id, value),
                        Enabled: usable)
                    {
                        Shown = shown, Sub = true,
                    },
                };
            }
            yield return new ActionRow("Reset enhancements", Reset, "Reset") { Shown = () => !Folded };
        }

        public static void Load()
        {
            var category = MelonPreferences.CreateCategory(Category);
            folded = category.CreateEntry("Folded", false, description: "Fold the Graphics tab's Enhancements category away, so only its heading shows.");
            var readers = new Func<float?>[writers.Length];
            foreach (var field in EnhancementsModel.Fields)
                (readers[(int)field.Id], writers[(int)field.Id]) = Entry(category, field);
            run = new EnhancementRun(EnhancementValues.From(field => readers[(int)field.Id]()));
            // An entry may hold what its row cannot, such as a number past the row's range, and then says what was read.
            foreach (var field in EnhancementsModel.Fields)
                writers[(int)field.Id](Values[field.Id]);
            MelonPreferences.Save();
            Log($"the preferences hold {Describe(Values)}.");
        }

        // Once a frame, after Enhancements.Update. An add-on that ReShade loaded has none of the settings yet.
        public static void Update()
        {
            if (connections != Enhancements.Connections)
            {
                connections = Enhancements.Connections;
                ulong generation = Enhancements.Apply(EnhancementsModel.Settings(Running));
                Log($"the add-on connected and runs {Describe(Running)}, as its settings {generation}.");
            }
            if (saveAt is not float at || Time.unscaledTime < at)
                return;
            saveAt = null;
            if (unsaved)
                MelonPreferences.Save();
            unsaved = false;
            Log($"now {Describe(Running)}{(Enhancements.Connected ? "" : ", which wait for the add-on")}: {Running}.");
        }

        // Sets the user's own values: has the add-on run them and writes the ones that changed to their entries, which
        // Update saves. Returns whether any changed.
        public static bool Set(EnhancementValues values)
        {
            if (values.Equals(Values))
                return false;
            foreach (var field in EnhancementsModel.Fields)
                if (values[field.Id] != Values[field.Id])
                    writers[(int)field.Id](values[field.Id]);
            run = run with { Own = values };
            unsaved = true;
            Push();
            return true;
        }

        private static void Change(Enhancement id, float value) => Set(Values.With(id, value));

        // Every switch and value back to its default: everything off.
        private static void Reset()
        {
            Set(EnhancementValues.Defaults);
            Log("every switch and value is back at its default.");
            CameraUi.Toast("Enhancements are back to their defaults");
        }

        private static void SetFolded(bool on)
        {
            folded.Value = on;
            MelonPreferences.Save();
        }

        // A screenshot's preset holds these values: the add-on runs them until Release, and the user's own stay as they
        // are. Returns whether the add-on runs other values for it; a preset that holds none changes nothing.
        public static bool Hold(EnhancementValues values) => Run(run.Hold(values));

        // Returns whether the add-on ran a screenshot's values, which the user's own now replace.
        public static bool Release() => Run(run.Release());

        private static bool Run(EnhancementRun next)
        {
            if (ReferenceEquals(next, run))
                return false;
            run = next;
            Push();
            return true;
        }

        private static void Push()
        {
            saveAt = Time.unscaledTime + SaveDelay;
            if (Enhancements.Connected)
                Enhancements.Apply(EnhancementsModel.Settings(Running));
        }

        // A field's entry: a switch is true or false, a choice its option's label, and a slider its number.
        private static (Func<float?> Read, Action<float> Write) Entry(MelonPreferences_Category category, EnhancementField field)
        {
            string effect = EnhancementsModel.Field(field.Effect).Label;
            switch (field.Kind)
            {
                case EnhancementKind.Switch:
                    var on = category.CreateEntry(field.Name, field.Default != 0f, description: field.Id == Enhancement.Enabled
                        ? "The Enhancements switch. While it is off, no effect runs."
                        : $"The {field.Label} effect, which runs while Enabled is on.");
                    return (() => on.Value ? 1f : 0f, value => on.Value = value != 0f);
                case EnhancementKind.Choice:
                    string fallback = field.Options[(int)field.Default];
                    var label = category.CreateEntry(field.Name, fallback, description: $"{effect}: {field.Label}: {string.Join(", ", field.Options)}.");
                    return (() =>
                    {
                        float? position = field.Option(label.Value);
                        if (position == null)
                            Melon<CameraTools>.Logger.Warning($"{Category} {field.Name} = \"{label.Value}\" is not one of the choices; using {fallback}.");
                        return position;
                    }, value => label.Value = field.Options[(int)value]);
                default:
                    var number = category.CreateEntry(field.Name, field.Default,
                        description: $"{effect}: {field.Label}, from {field.Min.ToString(field.Format)} to {field.Max.ToString(field.Format)}.");
                    return (() => number.Value, value => number.Value = value);
            }
        }

        private static string Describe(EnhancementValues values) => $"{EnhancementsModel.Summary(values)}, with {EnhancementsModel.Names(values)} switched on";

        private static void Log(string message) => Melon<CameraTools>.Logger.Msg($"Enhancements: {message}");
    }
}
