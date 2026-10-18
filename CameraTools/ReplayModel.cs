using System.Text.RegularExpressions;

namespace CameraTools
{
    // What a track records, by the scene root it comes from.
    internal enum TrackKind { Character, Weapon, Effect, Gadget, Monster, MonsterWeapon }

    internal enum ReplayWorld { Running, Frozen }

    // Samples in the order they were taken, at rising times.
    internal sealed class Timeline<T>
    {
        // Samples further apart than this are not blended: the object was not recorded between them.
        public const float Gap = 0.25f;

        private readonly List<float> times = new();
        private readonly List<T> values = new();

        public int Count => times.Count;

        public float LastTime => times[^1];

        public IReadOnlyList<T> Values => values;

        public void Add(float time, T value)
        {
            times.Add(time);
            values.Add(value);
        }

        // The last sample at or before a time, or -1 when the first comes later.
        public int IndexAt(float time)
        {
            int lo = 0, hi = times.Count - 1;
            if (hi < 0 || times[0] > time)
                return -1;
            while (lo < hi)
            {
                int mid = (lo + hi + 1) / 2;
                if (times[mid] <= time)
                    lo = mid;
                else
                    hi = mid - 1;
            }
            return lo;
        }

        // When the last sample at or before a time was taken, or null when there is none.
        public float? LastAt(float time) => IndexAt(time) is var i && i >= 0 ? times[i] : null;

        // The sample at a time: the last one before it, blended toward the next when that follows within Gap.
        public bool Sample(float time, out T a, out T b, out float blend)
        {
            int i = IndexAt(time);
            if (i < 0)
            {
                a = b = default;
                blend = 0f;
                return false;
            }
            a = values[i];
            if (i + 1 < times.Count && times[i + 1] - times[i] <= Gap)
            {
                b = values[i + 1];
                blend = Math.Clamp((time - times[i]) / (times[i + 1] - times[i]), 0f, 1f);
            }
            else
            {
                b = a;
                blend = 0f;
            }
            return true;
        }

        // The item whose timeline was sampled most recently up to a time, or null when none was yet. The on-field
        // character is the one recorded last: a rule that hid a character across a gap in its recording hid characters at
        // switches whenever the recording hitched.
        public static TItem Latest<TItem>(IEnumerable<TItem> items, Func<TItem, Timeline<T>> of, float time) where TItem : class
        {
            TItem latest = null;
            float at = float.NegativeInfinity;
            foreach (var item in items)
                if (of(item).LastAt(time) is float last && last > at)
                {
                    latest = item;
                    at = last;
                }
            return latest;
        }
    }

    // The Replays tab's words for a replay, and the clock a linked replay keeps.
    internal static class ReplayText
    {
        private static readonly Regex Numbered = new(@"^Replay (\d+)$");

        // Avatar_Girl_Claymore_Noel(Clone) is Noel: the game's own name for the character, at the end of its root's name.
        public static string CharacterName(string root)
        {
            string name = root.EndsWith("(Clone)") ? root[..^"(Clone)".Length] : root;
            int underscore = name.LastIndexOf('_');
            return underscore >= 0 && underscore < name.Length - 1 ? name[(underscore + 1)..] : name;
        }

        public static string NextName(IEnumerable<string> names)
        {
            int last = names.Select(name => Numbered.Match(name)).Where(match => match.Success)
                .Select(match => int.Parse(match.Groups[1].Value)).DefaultIfEmpty(0).Max();
            return $"Replay {last + 1}";
        }

        public static string Note(float seconds, IReadOnlyList<string> party, int enemies)
        {
            var parts = new List<string> { $"{seconds:0.0} s" };
            if (party.Count > 0)
                parts.Add(string.Join(", ", party));
            if (enemies > 0)
                parts.Add(enemies == 1 ? "1 enemy" : $"{enemies} enemies");
            return string.Join(" · ", parts);
        }

        // path is the linked path's number, or null for none.
        public static string Recorded(string name, float seconds, int? path)
            => $"{name} recorded · {seconds:0.0} s{(path is int number ? $" · plays with path {number}" : "")}";

        public static string Recording(float seconds, int? path) => $"● REC {seconds:0.0} s{(path is int number ? $" · path {number}" : "")}";

        public static string Playing(string name, float seconds, float duration, ReplayWorld world)
            => $"{name} · {seconds:0.0} / {duration:0.0} s{(world == ReplayWorld.Frozen ? " · world frozen" : "")}";

        // A linked replay starts offset seconds into its path, holds its first frame until then and its last after it ends.
        public static float LinkedTime(float pathSeconds, float offset, float duration) => Math.Clamp(pathSeconds - offset, 0f, duration);
    }
}
