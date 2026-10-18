using System.Text.RegularExpressions;

namespace CameraTools
{
    // What a track records, by the scene root it comes from.
    internal enum TrackKind { Character, Weapon, Effect, Gadget, Monster, MonsterWeapon }

    internal enum ReplayWorld { Running, Frozen }

    // The game's setter calls a replay repeats: on one material slot, or, for CustomFloat, RendererSet, EnableProperty and
    // MeshSwap, on one renderer.
    internal enum CallKind { Float, Int, Color, Vector, Shader, KeywordOn, KeywordOff, KeywordIdOn, KeywordIdOff, CustomFloat, Swap, RendererSet, EnableProperty, Texture, MeshSwap }

    // What a call sets. A later call on the same target leaves an earlier one nothing to do.
    internal readonly record struct CallTarget(CallKind Kind, int Slot, int Id, string Text)
    {
        public static CallTarget Of(CallKind kind, int slot, int id, string text) => kind switch
        {
            CallKind.KeywordOn or CallKind.KeywordOff => new(CallKind.KeywordOn, slot, 0, text),
            CallKind.KeywordIdOn or CallKind.KeywordIdOff => new(CallKind.KeywordIdOn, slot, id, null),
            CallKind.Shader or CallKind.Swap or CallKind.MeshSwap or CallKind.EnableProperty => new(kind, slot, 0, null),
            _ => new(kind, slot, id, null),
        };

        public bool OnRenderer => Kind is CallKind.CustomFloat or CallKind.RendererSet or CallKind.EnableProperty or CallKind.MeshSwap;
    }

    internal static class LookCalls
    {
        // The first count calls that still change something once all of them have run, in their order: the last call on
        // each target and, where swaps apply, none from before a slot's last swap, whose snapshot already holds them.
        // Seeking repeats only these on fresh materials, which ends where repeating every call does.
        public static List<int> Live(int count, Func<int, CallTarget> target, bool swaps)
        {
            var seen = new HashSet<CallTarget>();
            var swapped = new HashSet<int>();
            var live = new List<int>();
            for (int index = count - 1; index >= 0; index--)
            {
                var call = target(index);
                if (swaps && !call.OnRenderer && swapped.Contains(call.Slot) || !seen.Add(call))
                    continue;
                live.Add(index);
                if (swaps && call.Kind == CallKind.Swap)
                    swapped.Add(call.Slot);
            }
            live.Reverse();
            return live;
        }
    }

    // Samples in the order they were taken, at rising times.
    internal sealed class Timeline<T>
    {
        // Samples further apart than this are not blended: the object was not recorded between them.
        public const float Gap = 0.25f;

        private readonly List<float> times = new();
        private readonly List<T> values = new();

        public int Count => times.Count;

        public float FirstTime => times[0];

        public float LastTime => times[^1];

        public IReadOnlyList<T> Values => values;

        public IReadOnlyList<float> Times => times;

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

        // The first sample after a time, or null when there is none.
        public float? After(float time) => IndexAt(time) + 1 is var i && i < times.Count ? times[i] : null;

        // The last sample before a time, or null when there is none.
        public float? Before(float time)
        {
            int i = IndexAt(time);
            while (i >= 0 && times[i] >= time)
                i--;
            return i >= 0 ? times[i] : null;
        }

        // The sample at a time: the last one before it, blended toward the next when that follows within Gap. Before the
        // first sample it is the first; with no samples there is none.
        public bool Sample(float time, out T a, out T b, out float blend)
        {
            int i = IndexAt(time);
            if (i < 0)
            {
                a = b = times.Count > 0 ? values[0] : default;
                blend = 0f;
                return times.Count > 0;
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

        // The item whose timeline was sampled most recently up to a time; before any was, the one sampled first; null when
        // none has samples. The on-field character is the one recorded last: a rule that hid a character across a gap in
        // its recording hid characters at switches whenever the recording hitched.
        public static TItem Latest<TItem>(IEnumerable<TItem> items, Func<TItem, Timeline<T>> of, float time) where TItem : class
        {
            TItem latest = null, first = null;
            float at = float.NegativeInfinity, earliest = float.PositiveInfinity;
            foreach (var item in items)
            {
                var timeline = of(item);
                if (timeline.Count == 0)
                    continue;
                if (timeline.LastAt(time) is float last && last > at)
                {
                    latest = item;
                    at = last;
                }
                if (timeline.FirstTime < earliest)
                {
                    first = item;
                    earliest = timeline.FirstTime;
                }
            }
            return latest ?? first;
        }
    }

    // Edit replay's timeline: the frames it steps between, how fast it scrubs, the nodes near the playhead, the party
    // switches, and its words.
    internal static class EditTimeline
    {
        // A node this close to the playhead is the one there: Place node updates it and Delete node removes it.
        public const float Reach = 0.1f;
        // Jumping to the next node goes past the one the playhead is on.
        private const float Past = 0.01f;

        // The recorded frame before or after t, direction -1 or 1, across the tracks; t itself when there is none that way.
        public static float Step<T>(IEnumerable<Timeline<T>> tracks, float t, int direction)
        {
            float? best = null;
            foreach (var track in tracks)
                if ((direction < 0 ? track.Before(t) : track.After(t)) is float time && (best is not float now || (direction < 0 ? time > now : time < now)))
                    best = time;
            return best ?? t;
        }

        // Replay seconds per real second while a frame step is held for held seconds: from half speed, doubling every
        // 0.75 s, up to 16 times.
        public static float ScrubRate(float held) => Math.Min(0.5f * MathF.Pow(2f, held / 0.75f), 16f);

        // The times another track becomes the one recorded most recently, which for characters is the one on the field.
        public static float[] Switches<T>(IReadOnlyList<Timeline<T>> tracks)
        {
            var samples = tracks.SelectMany((track, index) => track.Times.Select(time => (Time: time, Track: index))).OrderBy(sample => sample.Time);
            var switches = new List<float>();
            int current = -1;
            foreach (var (time, track) in samples)
            {
                if (current >= 0 && track != current)
                    switches.Add(time);
                current = track;
            }
            return switches.ToArray();
        }

        // The node whose moment is nearest t, within Reach, or -1.
        public static int NodeAt(IReadOnlyList<float> moments, float t)
        {
            int found = -1;
            for (int index = 0; index < moments.Count; index++)
                if (Math.Abs(moments[index] - t) <= Reach && (found < 0 || Math.Abs(moments[index] - t) < Math.Abs(moments[found] - t)))
                    found = index;
            return found;
        }

        // The first node after t, or the last before it, direction 1 or -1; -1 when there is none.
        public static int Neighbour(IReadOnlyList<float> moments, float t, int direction)
        {
            int found = -1;
            for (int index = 0; index < moments.Count; index++)
                if (direction > 0 ? moments[index] > t + Past && found < 0 : moments[index] < t - Past)
                    found = index;
            return found;
        }

        public static string Meta(string replay, float t, float duration, bool playing)
            => $"{replay} · {t:0.00} / {duration:0.0} s · {(playing ? "playing" : "paused")}";

        public static string Mode(string path, int nodes, bool look, ReplayWorld world)
            => $"{path} · {Nodes(nodes)} · {(look ? "looking through the path" : "free camera")}{(world == ReplayWorld.Frozen ? " · world frozen" : "")}";

        public static string Nodes(int count) => count == 1 ? "1 node" : $"{count} nodes";
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

        // path is the linked path's label, its name or "Path 2", or null for none.
        public static string Recorded(string name, float seconds, string path)
            => $"{name} recorded · {seconds:0.0} s{(path != null ? $" · plays with {path}" : "")}";

        public static string Recording(float seconds, string path) => $"● REC {seconds:0.0} s{(path != null ? $" · {path}" : "")}";

        public static string Playing(string name, float seconds, float duration, ReplayWorld world)
            => $"{name} · {seconds:0.0} / {duration:0.0} s{(world == ReplayWorld.Frozen ? " · world frozen" : "")}";

        // A linked replay starts offset seconds into its path, holds its first frame until then and its last after it ends.
        public static float LinkedTime(float pathSeconds, float offset, float duration) => Math.Clamp(pathSeconds - offset, 0f, duration);
    }
}
