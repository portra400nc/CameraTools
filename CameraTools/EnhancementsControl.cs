using CameraToolsEnhancements;
using MelonLoader;
using UnityEngine;

namespace CameraTools
{
    // The Graphics tab's Enhancements category, and the only owner of the add-on's settings. Each switch and value is an
    // entry of the CameraToolsEnhancements preferences: a change is saved and goes to the add-on at once, and the add-on
    // gets the saved values whenever ReShade loads it, which is some time after the game starts. EnhancementsModel holds
    // the rows and the rules.
    internal static class EnhancementsControl
    {
        private const string Category = "CameraToolsEnhancements";
        // How long after the last change the log names the values: a dragged slider changes them every frame.
        private const float LogDelay = 1f;

        // By field: writes a value to the field's entry.
        private static readonly Action<float>[] writers = new Action<float>[EnhancementsModel.Fields.Length];
        private static EnhancementValues own = EnhancementValues.Defaults;
        // A screenshot's preset's values, which the add-on runs in place of the user's own until the screenshot puts its
        // settings back. The preferences keep the user's own.
        private static EnhancementValues shot;
        private static int connections;
        private static float? logAt;

        // The user's own values.
        public static EnhancementValues Values => own;

        private static EnhancementValues Running => shot ?? own;

        public static void Load()
        {
            var category = MelonPreferences.CreateCategory(Category);
            var readers = new Func<float?>[writers.Length];
            foreach (var field in EnhancementsModel.Fields)
                (readers[(int)field.Id], writers[(int)field.Id]) = Entry(category, field);
            own = EnhancementValues.From(field => readers[(int)field.Id]());
            // An entry may hold what its row cannot, such as a number past the row's range, and then says what was read.
            foreach (var field in EnhancementsModel.Fields)
                writers[(int)field.Id](own[field.Id]);
            MelonPreferences.Save();
            Log($"the preferences hold {Describe(own)}.");
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
            if (logAt is not float at || Time.unscaledTime < at)
                return;
            logAt = null;
            Log($"now {Describe(Running)}{(Enhancements.Connected ? "" : ", which wait for the add-on")}: {Running}.");
        }

        // Sets the user's own values: saves the ones that changed and has the add-on run them. Returns whether any did.
        public static bool Set(EnhancementValues values)
        {
            if (values.Equals(own))
                return false;
            foreach (var field in EnhancementsModel.Fields)
                if (values[field.Id] != own[field.Id])
                    writers[(int)field.Id](values[field.Id]);
            own = values;
            MelonPreferences.Save();
            Push();
            return true;
        }

        // A screenshot's preset holds these values: the add-on runs them until Release, and the user's own stay as they
        // are. Returns whether the add-on runs other values for it; a preset that holds none changes nothing.
        public static bool Hold(EnhancementValues values)
        {
            if (values == null || values.Equals(Running))
                return false;
            shot = values;
            Push();
            return true;
        }

        // Returns whether the add-on ran a screenshot's values, which the user's own now replace.
        public static bool Release()
        {
            if (shot == null)
                return false;
            shot = null;
            Push();
            return true;
        }

        private static void Push()
        {
            logAt = Time.unscaledTime + LogDelay;
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
