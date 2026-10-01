using MelonLoader;
using UnityEngine;
using static CameraTools.CameraTools;

namespace CameraTools
{
    // The ReShade tab's depth of field rows and the focus point on the free camera's HUD. It is the only class that names
    // the uniforms of iMMERSE's depth of field shader. Every value is read from the bridge each frame while the free
    // camera is on, so what the rows show is what ReShade has, also after ReShade loads its effects again.
    internal static class DepthOfField
    {
        private const string Effect = "MartysMods_DEPTHOFFIELD.fx";
        private const int ManualFocus = 0;
        private const int PointFocus = 1;
        // FOCUS_DEBUG's "Enabled", which paints the plane that is in focus over the picture.
        private const int Painted = 3;
        private const float PaintSeconds = 1f;
        private const float SaveDelay = 1f;
        private const float MinFocalLength = 10f;
        private const float MaxFocalLength = 350f;
        // Of the point's -1 to 1 per second at full deflection: edge to edge in two seconds.
        private const float SteerSpeed = 1f;
        // Presents until a set reads back: one applies it, and the next refreshes the bridge's copy if it had not yet.
        private const ulong SetPresents = 2;

        private enum Kind { Technique, Int, Float, Float2 }

        private enum Id { Technique, FocusMode, FocusDepth, FocusDebug, FocusPoint, FocusRange, FocalLength, Aperture }

        // In Id order.
        private static readonly (Id Id, string Name, Kind Kind)[] Table =
        {
            (Id.Technique, "MartysMods_DOF", Kind.Technique),
            (Id.FocusMode, "FOCUS_MODE", Kind.Int),
            // The shader squares it.
            (Id.FocusDepth, "RAW_FOCUS_PLANE_DEPTH", Kind.Float),
            (Id.FocusDebug, "FOCUS_DEBUG", Kind.Int),
            (Id.FocusPoint, "AUTOFOCUS_CENTER", Kind.Float2),
            (Id.FocusRange, "AUTOFOCUS_RANGE", Kind.Float),
            (Id.FocalLength, "FOCAL_LENGTH", Kind.Float),
            (Id.Aperture, "FSTOPS", Kind.Float),
        };

        // A value as the bridge last returned it or CameraTools last set it; a technique is 1 or 0. Known is false while
        // the bridge cannot read it. Until is the bridge's present count from which a read replaces a value that was set.
        private struct Cell
        {
            public bool Known;
            public float X;
            public float Y;
            public ulong Until;
        }

        private static readonly EffectName[] names = Table.Select(row => new EffectName(Effect, row.Name)).ToArray();
        private static readonly Cell[] cells = new Cell[Table.Length];
        // Focus mode's options, as FOCUS_MODE values. The shader's own click-to-focus mode is not offered: it keeps the
        // clicked point in a texture that a resize recreates, where the focus point is a uniform that the preset saves.
        private static readonly int[] Modes = { PointFocus, ManualFocus };
        private static readonly string[] ModeLabels = { "Focus point", "Manual distance" };
        private static readonly string[] StopLabels = Lens.Stops.Select(stop => $"f/{stop:0.##}").ToArray();

        private static MelonPreferences_Entry<bool> link;
        private static MelonPreferences_Entry<bool> marker;
        private static ulong presents;
        private static bool ready;
        // The present count from which the values are ReShade's again after it loaded its effects.
        private static ulong settled;
        // A value differs from the preset on disk. Changed is when it last did.
        private static bool dirty;
        private static float changed;
        private static bool panelWas;
        private static float paintUntil;
        // CameraTools turned the focus plane's paint on and has not seen it off since.
        private static bool painting;

        public static readonly Row[] Rows =
        {
            new Section("Depth of field", () => Problem),
            new ToggleRow("Depth of field", () => Value(Id.Technique) != 0f, on => Change(Id.Technique, on ? 1f : 0f), Has(Id.Technique)),
            new ChoiceRow("Focus mode", ModeLabels, () => Known(Id.FocusMode) ? Array.IndexOf(Modes, Mode) : -1,
                position => Change(Id.FocusMode, Modes[position]), null, Has(Id.FocusMode)),
            new SliderRow("Focus distance", 0.002f, 1f, 0.002f, "0.000", () => Value(Id.FocusDepth), SetFocusDepth,
                Enabled: () => Known(Id.FocusDepth) && Mode == ManualFocus),
            new ChoiceRow("Aperture", StopLabels, () => Known(Id.Aperture) ? Lens.NearestStop(Value(Id.Aperture)) : -1,
                position => Change(Id.Aperture, Lens.Stops[position]), null, Has(Id.Aperture)),
            new SliderRow("Focal length", MinFocalLength, MaxFocalLength, 1f, "0' mm'", () => FocalLength, SetFocalLength,
                Enabled: Has(Id.FocalLength)),
            new ToggleRow("Link focal length to field of view", () => Linked, on => Linked = on),
            new ToggleRow("Show focus point", () => ShowMarker, on => ShowMarker = on),
            new SliderRow("Focus point X", -1f, 1f, 0.02f, "0.00", () => Value(Id.FocusPoint),
                x => Change(Id.FocusPoint, x, cells[(int)Id.FocusPoint].Y), Enabled: () => Aiming),
            new SliderRow("Focus point Y", -1f, 1f, 0.02f, "0.00", () => cells[(int)Id.FocusPoint].Y,
                y => Change(Id.FocusPoint, Value(Id.FocusPoint), y), Enabled: () => Aiming),
        };

        public static bool Linked
        {
            get => link.Value;
            set
            {
                link.Value = value;
                MelonPreferences.Save();
            }
        }

        public static bool ShowMarker
        {
            get => marker.Value;
            set
            {
                marker.Value = value;
                MelonPreferences.Save();
            }
        }

        // The focus point decides the focus: the autofocus mode with its point readable.
        private static bool Aiming => Known(Id.FocusPoint) && Known(Id.FocusRange) && Mode == PointFocus;

        // The focus point can be seen and moved while the depth of field is one of ReShade's effects, also with ReShade's
        // global switch off: effects cost frame rate, and the point is set for the screenshot, which turns them on.
        // With the marker switched off, the stick and the click do not move a point that cannot be seen.
        private static bool Marked => ShowMarker && Aiming && Value(Id.Technique) != 0f;

        // Back with the right stick moves the focus point, so the camera does not also turn. A binding is a chord of
        // buttons or one stick direction and never both, so the pad is read here, as the panel's navigation reads it.
        public static bool Steering => Marked && CameraUi.View == View.Hud && Controls.Owner == PadOwner.CameraTools
            && (Gamepad.Current.Buttons & PadButtons.Back) != 0;

        // The focus plane's paint is on, or may be: a set has not read back yet, or ReShade has only just loaded its
        // effects and their values have not arrived.
        public static bool Painting
        {
            get
            {
                var debug = cells[(int)Id.FocusDebug];
                return ready && presents < settled || debug.Known && (debug.X != 0f || presents < debug.Until);
            }
        }

        private static string Problem => !ReShade.Connected ? "ReShade bridge not found"
            : !ready ? "ReShade is loading its effects"
            : !Known(Id.Technique) ? "Depth of field shader not loaded"
            : null;

        private static int Mode => Known(Id.FocusMode) ? (int)Value(Id.FocusMode) : -1;

        private static float Aspect => (float)Screen.width / Screen.height;

        // While linked, the row shows the field of view setting, and the shader follows the camera's smoothed one to it.
        private static float FocalLength => Linked
            ? Math.Min(Lens.FocalLength(settings.Fov.Value, Aspect), MaxFocalLength)
            : Value(Id.FocalLength);

        public static void Load()
        {
            for (int index = 0; index < Table.Length; index++)
                if ((int)Table[index].Id != index)
                    throw new InvalidOperationException($"DepthOfField.Table row {index} ({Table[index].Id}) is out of Id order.");
            var category = MelonPreferences.CreateCategory("CameraToolsReShade");
            link = category.CreateEntry("LinkFocalLengthToFov", false,
                description: "Keep the depth of field's focal length and the free camera's field of view in step, as on a full-frame camera.");
            marker = category.CreateEntry("ShowFocusPoint", true,
                description: "Draw the depth of field's focus point on the picture in Focus point mode.");
            MelonPreferences.Save();
        }

        // The window the autofocus samples, while the focus point shows; aspect is the screen's width over its height.
        public static ScreenBox? FocusWindow(float aspect)
        {
            if (!Marked)
                return null;
            var point = cells[(int)Id.FocusPoint];
            return Lens.FocusWindow(point.X, point.Y, Value(Id.FocusRange), aspect);
        }

        // Before the screenshot changes the window's size: a resize makes ReShade load its effects again from the preset,
        // which has to hold the current values and no paint.
        public static void Flush()
        {
            if (dirty)
                Save();
            else
                StopPainting();
        }

        // Before the UI's update, which draws the focus point and runs the rows.
        public static void Update()
        {
            var status = ReShade.Status;
            presents = status.Presents;
            bool loaded = status.Runtime == 1 && status.EffectsReady == 1;
            if (loaded && !ready)
            {
                Array.Clear(cells);
                settled = presents + SetPresents;
            }
            ready = loaded;
            bool active = freecamActive && ReShade.Connected;
            bool panel = CameraUi.PanelOpen;
            float now = Time.unscaledTime;
            if (active)
            {
                for (int index = 0; index < cells.Length; index++)
                    Read(index);
                Steer();
                Click();
                Follow();
                Paint(now);
            }
            else
            {
                // Leaving the free camera within a second of a focus distance change must not leave the paint on.
                if (painting)
                    StopPainting();
                Array.Clear(cells);
            }
            // The panel's changes are saved when it closes, and changes made outside it once they stop. A screenshot in
            // progress is left alone: it saved before it started.
            if (dirty && !panel && Screenshot.State is ShotState.Idle && (panelWas || !active || now - changed >= SaveDelay))
                Save();
            panelWas = panel;
        }

        private static void Read(int index)
        {
            ref var cell = ref cells[index];
            if (presents < cell.Until)
                return;
            var name = names[index];
            float x, y = 0f;
            switch (Table[index].Kind)
            {
                case Kind.Technique:
                    cell.Known = ReShade.TryGetTechnique(name, out bool enabled);
                    x = enabled ? 1f : 0f;
                    break;
                case Kind.Int:
                    cell.Known = ReShade.TryGetInt(name, out int value);
                    x = value;
                    break;
                case Kind.Float2:
                    cell.Known = ReShade.TryGetFloat(name, out x, out y, 2);
                    break;
                default:
                    cell.Known = ReShade.TryGetFloat(name, out x, out y);
                    break;
            }
            if (!cell.Known)
                return;
            cell.X = x;
            cell.Y = y;
        }

        // The bridge applies a set at ReShade's next present, so the value is kept here until it reads back. Otherwise a
        // row would show the old value for a frame, and a held D-pad would step from it.
        private static void Write(Id id, float x, float y = 0f)
        {
            var name = names[(int)id];
            switch (Table[(int)id].Kind)
            {
                case Kind.Technique:
                    ReShade.SetTechnique(name, x != 0f);
                    break;
                case Kind.Int:
                    ReShade.SetInt(name, (int)x);
                    break;
                case Kind.Float2:
                    ReShade.SetFloat(name, x, y, 2);
                    break;
                default:
                    ReShade.SetFloat(name, x);
                    break;
            }
            cells[(int)id] = new Cell { Known = true, X = x, Y = y, Until = presents + SetPresents };
        }

        // A change the user made, which is saved to the preset later. A held mouse drag sets its slider every frame, so
        // a set to the same value is not a change.
        private static bool Change(Id id, float x, float y = 0f)
        {
            var cell = cells[(int)id];
            if (cell.Known && cell.X == x && cell.Y == y)
                return false;
            Write(id, x, y);
            dirty = true;
            changed = Time.unscaledTime;
            return true;
        }

        private static bool Known(Id id) => cells[(int)id].Known;

        private static float Value(Id id) => cells[(int)id].X;

        private static Func<bool> Has(Id id) => () => Known(id);

        private static void SetFocusDepth(float depth)
        {
            if (Change(Id.FocusDepth, depth))
                paintUntil = Time.unscaledTime + PaintSeconds;
        }

        // While linked, the row sets the camera's field of view, and Follow brings the shader's focal length after it.
        private static void SetFocalLength(float focalLength)
        {
            if (Linked)
                settings.Fov.Value = Lens.FieldOfView(focalLength, Aspect);
            else
                Change(Id.FocalLength, focalLength);
        }

        private static void Steer()
        {
            var point = cells[(int)Id.FocusPoint];
            if (Marked && CameraUi.View == View.Hud && Controls.Pressed(CamAction.CenterFocusPoint))
                Change(Id.FocusPoint, 0f, 0f);
            else if (Steering)
            {
                var pad = Gamepad.Current;
                float step = SteerSpeed * Time.unscaledDeltaTime;
                // The stick's up is positive, and the point's up is negative.
                Change(Id.FocusPoint, Math.Clamp(point.X + pad.RightX * step, -1f, 1f), Math.Clamp(point.Y - pad.RightY * step, -1f, 1f));
            }
        }

        // With the cursor free and the panel closed, a click on the picture puts the focus point there. The point is kept
        // as the shader takes it, as a fraction of the screen, so it means the same place at every resolution.
        private static void Click()
        {
            if (!Marked || CameraUi.View != View.Hud || Freecam.Focused || !Input.GetMouseButtonDown(0))
                return;
            var mouse = Input.mousePosition;
            Change(Id.FocusPoint, Math.Clamp(mouse.x / Screen.width * 2f - 1f, -1f, 1f), Math.Clamp(1f - mouse.y / Screen.height * 2f, -1f, 1f));
        }

        // Rounded to a tenth of a millimetre, so the tail of the camera's smoothing does not set and save it forever.
        private static void Follow()
        {
            if (!Linked || !Known(Id.FocalLength))
                return;
            float focalLength = Math.Min(Lens.FocalLength(freecam.Pose.Fov, Aspect), MaxFocalLength);
            Change(Id.FocalLength, MathF.Round(focalLength * 10f) / 10f);
        }

        // The focus plane is painted for a second after the focus distance changed, and never while the UI is hidden or a
        // screenshot is in progress, whoever turned the paint on.
        private static void Paint(float now)
        {
            var debug = cells[(int)Id.FocusDebug];
            if (!debug.Known)
                return;
            bool hidden = CameraUi.View == View.Hidden || Screenshot.State is not ShotState.Idle;
            if (now < paintUntil && !hidden)
            {
                painting = true;
                if (debug.X != Painted)
                    Write(Id.FocusDebug, Painted);
            }
            else if (debug.X == 0f)
                painting = false;
            else if (painting || hidden)
                Write(Id.FocusDebug, 0f);
        }

        private static void StopPainting()
        {
            paintUntil = 0f;
            var debug = cells[(int)Id.FocusDebug];
            if (painting || debug.Known && debug.X != 0f)
                Write(Id.FocusDebug, 0f);
            painting = false;
        }

        // The bridge saves after the sets that are still queued, so the preset never holds the paint.
        private static void Save()
        {
            StopPainting();
            ReShade.SavePreset();
            dirty = false;
        }
    }
}
