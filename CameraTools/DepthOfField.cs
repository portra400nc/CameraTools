using MelonLoader;
using UnityEngine;
using static CameraTools.CameraTools;

namespace CameraTools
{
    // The ReShade tab's depth of field rows and the focus point on the free camera's HUD. It is the only class that names
    // the uniforms of the depth of field shaders: iMMERSE's, and Otis_Inf's free Cinematic DOF. With both installed the
    // Shader row picks the one the rows drive, and only that one renders. Every value is read from the bridge each frame
    // while the free camera is on, so what the rows show is what ReShade has, also after ReShade loads its effects again.
    internal static class DepthOfField
    {
        private const int ManualFocus = 0;
        private const int PointFocus = 1;
        private const float PaintSeconds = 1f;
        private const float SaveDelay = 1f;
        private const float FocusMargin = 0.25f;
        private const float MinFocalLength = 10f;
        private const float MaxFocalLength = 350f;
        // Of the point's -1 to 1 per second at full deflection: edge to edge in two seconds.
        private const float SteerSpeed = 1f;
        // Presents until a set reads back: one applies it, and the next refreshes the bridge's copy if it had not yet.
        private const ulong SetPresents = 2;
        // Cinematic DOF's autofocus samples the one pixel under the point, so the marker's square only frames it.
        private const float PixelRange = 0.03f;
        // Cinematic DOF eases its focus by a fraction per frame, so the wait for it assumes a slow screenshot's frame rate.
        private const float SlowFrameRate = 8f;

        private enum Kind { Technique, Int, Float, Float2 }

        private enum Id { Technique, FocusMode, FocusDepth, FocusDebug, FocusPoint, FocusRange, FocusSpeed, FocalLength, Aperture, Tangential, Sagittal, Anamorph, MouseFocus, PhysicalBlur, TiltAngle, TiltDirection }

        // How a shader keeps one of the rows' values. The rows use iMMERSE's units, or the shader's own for a value iMMERSE
        // does not have, and ToRow and ToShader convert each component of another shader's value. Forced is a value that
        // CameraTools sets back whenever the shader has another.
        private sealed record Uniform(string Name, Kind Kind, Func<float, float> ToRow = null, Func<float, float> ToShader = null, float? Forced = null);

        // A value as the bridge last returned it or CameraTools last set it, in the rows' units; a technique is 1 or 0.
        // Known is false while the bridge cannot read it. Until is the bridge's present count from which a read replaces a
        // value that was set.
        private struct Cell
        {
            public bool Known;
            public float X;
            public float Y;
            public ulong Until;
        }

        // Uniforms is indexed by Id and holds null for a value the shader does not have. Painted is the debug value that
        // paints the plane in focus over the picture. FocusSeconds is how long the autofocus takes at a speed, and
        // DefaultSpeed the speed to assume while it cannot be read.
        private sealed class Shader
        {
            public readonly string Label;
            public readonly string File;
            public readonly Uniform[] Uniforms;
            public readonly int Painted;
            public readonly Func<float, float> FocusSeconds;
            public readonly float DefaultSpeed;
            public readonly EffectName[] Names;
            public readonly Cell[] Cells;

            public Shader(string label, string file, int painted, Func<float, float> focusSeconds, float defaultSpeed, params (Id Id, Uniform Uniform)[] uniforms)
            {
                Label = label;
                File = file;
                Painted = painted;
                FocusSeconds = focusSeconds;
                DefaultSpeed = defaultSpeed;
                Uniforms = new Uniform[Enum.GetValues<Id>().Length];
                foreach (var (id, uniform) in uniforms)
                    Uniforms[(int)id] = uniform;
                Names = Uniforms.Select(uniform => uniform == null ? null : new EffectName(file, uniform.Name)).ToArray();
                Cells = new Cell[Uniforms.Length];
            }

            public bool Offers(Id id) => Uniforms[(int)id] != null;

            public bool Known(Id id) => Cells[(int)id].Known;

            public float Value(Id id) => Cells[(int)id].X;

            public bool On => Known(Id.Technique) && Value(Id.Technique) != 0f;

            public void Read(int index)
            {
                ref var cell = ref Cells[index];
                var uniform = Uniforms[index];
                if (uniform == null || presents < cell.Until)
                    return;
                var name = Names[index];
                float x, y = 0f;
                switch (uniform.Kind)
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
                cell.X = uniform.ToRow?.Invoke(x) ?? x;
                cell.Y = uniform.ToRow?.Invoke(y) ?? y;
            }

            // The bridge applies a set at ReShade's next present, so the value is kept here until it reads back.
            // Otherwise a row would show the old value for a frame, and a held D-pad would step from it.
            public void Write(Id id, float x, float y = 0f)
            {
                var uniform = Uniforms[(int)id];
                var name = Names[(int)id];
                float shaderX = uniform.ToShader?.Invoke(x) ?? x, shaderY = uniform.ToShader?.Invoke(y) ?? y;
                switch (uniform.Kind)
                {
                    case Kind.Technique:
                        ReShade.SetTechnique(name, x != 0f);
                        break;
                    case Kind.Int:
                        ReShade.SetInt(name, (int)shaderX);
                        break;
                    case Kind.Float2:
                        ReShade.SetFloat(name, shaderX, shaderY, 2);
                        break;
                    default:
                        ReShade.SetFloat(name, shaderX);
                        break;
                }
                Cells[(int)id] = new Cell { Known = true, X = x, Y = y, Until = presents + SetPresents };
            }
        }

        // The autofocus eases toward the distance under the focus point and covers 99% of the way in 2 - 2 *
        // AUTOFOCUS_SPEED seconds, starting from nothing each time ReShade loads its effects again. Half as long again
        // covers the rest. FOCUS_DEBUG's 3, "Enabled", paints the plane in focus. The shader squares the focus depth.
        private static readonly Shader Immerse = new("iMMERSE", "MartysMods_DEPTHOFFIELD.fx", 3,
            speed => (2f - 2f * Math.Clamp(speed, 0f, 1f)) * 1.5f, 0f,
            (Id.Technique, new("MartysMods_DOF", Kind.Technique)),
            (Id.FocusMode, new("FOCUS_MODE", Kind.Int)),
            (Id.FocusDepth, new("RAW_FOCUS_PLANE_DEPTH", Kind.Float)),
            (Id.FocusDebug, new("FOCUS_DEBUG", Kind.Int)),
            (Id.FocusPoint, new("AUTOFOCUS_CENTER", Kind.Float2)),
            (Id.FocusRange, new("AUTOFOCUS_RANGE", Kind.Float)),
            (Id.FocusSpeed, new("AUTOFOCUS_SPEED", Kind.Float)),
            (Id.FocalLength, new("FOCAL_LENGTH", Kind.Float)),
            (Id.Aperture, new("FSTOPS", Kind.Float)),
            (Id.Tangential, new("BOKEH_RATIO_TANGENTIAL", Kind.Float)),
            (Id.Sagittal, new("BOKEH_RATIO_SAGITTAL", Kind.Float)),
            (Id.Anamorph, new("BOKEH_ANAMORPH_RATIO", Kind.Float)));

        // The autofocus moves AutoFocusTransitionSpeed of the way each frame. Its manual focus plane is the linear depth
        // times 1000, its point runs from 0 to 1, and its anamorphic factor is 1 over iMMERSE's ratio. Its booleans read
        // and set as 0 and 1. ShowCoCValues paints the plane in focus blue, and the blur grey behind it and red in front. The shader's own
        // overlay needs the left mouse button held, which the bridge cannot set. Its mouse-driven autofocus would ignore
        // the focus point, so it is kept off, and its physical blur is kept on so the blur is the lens's. Its tilt-shift
        // leans the plane in focus by an angle in degrees, 0 for none, around the focus point, also in manual focus.
        private static readonly Shader Cinematic = new("Cinematic DOF", "CinematicDOF.fx", 1,
            speed => speed >= 1f ? 0f : MathF.Log(0.01f) / MathF.Log(1f - Math.Clamp(speed, 0.001f, 1f)) / SlowFrameRate * 1.5f, 0.2f,
            (Id.Technique, new("CinematicDOF", Kind.Technique)),
            (Id.FocusMode, new("UseAutoFocus", Kind.Int)),
            (Id.FocusDepth, new("ManualFocusPlane", Kind.Float, depth => MathF.Sqrt(Math.Max(depth, 0f) / 1000f), raw => raw * raw * 1000f)),
            (Id.FocusDebug, new("ShowCoCValues", Kind.Int)),
            (Id.FocusPoint, new("AutoFocusPoint", Kind.Float2, point => point * 2f - 1f, point => point * 0.5f + 0.5f)),
            (Id.FocusSpeed, new("AutoFocusTransitionSpeed", Kind.Float)),
            (Id.FocalLength, new("FocalLength", Kind.Float)),
            (Id.Aperture, new("FNumber", Kind.Float)),
            (Id.Anamorph, new("HighlightAnamorphicFactor", Kind.Float, factor => 1f / Math.Max(factor, 0.01f), ratio => 1f / ratio)),
            (Id.MouseFocus, new("UseMouseDrivenAutoFocus", Kind.Int, Forced: 0f)),
            (Id.PhysicalBlur, new("UsePhysicalBlur", Kind.Int, Forced: 1f)),
            (Id.TiltAngle, new("TiltShiftAngle", Kind.Float)),
            (Id.TiltDirection, new("TiltShiftDirection", Kind.Float)));

        private static readonly Shader[] Shaders = { Immerse, Cinematic };
        private static readonly string[] ShaderLabels = Shaders.Select(shader => shader.Label).ToArray();
        // Focus mode's options, as FOCUS_MODE values. iMMERSE's own click-to-focus mode is not offered: it keeps the
        // clicked point in a texture that a resize recreates, where the focus point is a uniform that the preset saves.
        private static readonly int[] Modes = { PointFocus, ManualFocus };
        private static readonly string[] ModeLabels = { "Focus point", "Manual distance" };
        private static readonly string[] StopLabels = Lens.Stops.Select(stop => $"f/{stop:0.##}").ToArray();

        private static MelonPreferences_Entry<bool> link;
        private static MelonPreferences_Entry<bool> marker;
        private static MelonPreferences_Entry<string> choice;
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
            new ChoiceRow("Shader", ShaderLabels, () => Array.IndexOf(Shaders, Current), position => Choose(Shaders[position]), null)
            {
                Shown = () => Immerse.Known(Id.Technique) && Cinematic.Known(Id.Technique),
            },
            new ToggleRow("Depth of field", () => Value(Id.Technique) != 0f, on => Change(Id.Technique, on ? 1f : 0f), Has(Id.Technique)),
            new ChoiceRow("Focus mode", ModeLabels, () => Known(Id.FocusMode) ? Array.IndexOf(Modes, Mode) : -1,
                position => Change(Id.FocusMode, Modes[position]), null, Has(Id.FocusMode)),
            new SliderRow("Focus distance", 0.002f, 1f, 0.002f, "0.000", () => Value(Id.FocusDepth), depth => Repaint(Id.FocusDepth, depth),
                Enabled: () => Known(Id.FocusDepth) && Mode == ManualFocus),
            new ChoiceRow("Aperture", StopLabels, () => Known(Id.Aperture) ? Lens.NearestStop(Value(Id.Aperture)) : -1,
                position => Change(Id.Aperture, Lens.Stops[position]), null, Has(Id.Aperture)),
            new SliderRow("Focal length", MinFocalLength, MaxFocalLength, 1f, "0' mm'", () => FocalLength, SetFocalLength,
                Enabled: Has(Id.FocalLength)),
            new ToggleRow("Link focal length to field of view", () => Linked, on => Linked = on),
            new ToggleRow("Show focus point", () => ShowMarker, on => ShowMarker = on),
            new SliderRow("Focus point X", -1f, 1f, 0.02f, "0.00", () => Value(Id.FocusPoint),
                x => Change(Id.FocusPoint, x, Current.Cells[(int)Id.FocusPoint].Y), Enabled: () => Aiming),
            new SliderRow("Focus point Y", -1f, 1f, 0.02f, "0.00", () => Current.Cells[(int)Id.FocusPoint].Y,
                y => Change(Id.FocusPoint, Value(Id.FocusPoint), y), Enabled: () => Aiming),
            new SliderRow("Tilt", -15f, 15f, 0.5f, "0.0'°'", () => Value(Id.TiltAngle), tilt => Repaint(Id.TiltAngle, tilt),
                Display: tilt => tilt == 0f ? "Off" : tilt.ToString("0.0'°'"), Enabled: Has(Id.TiltAngle))
            {
                Shown = () => Current.Offers(Id.TiltAngle),
            },
            new SliderRow("Tilt direction", -90f, 90f, 5f, "0'°'", () => Value(Id.TiltDirection), direction => Repaint(Id.TiltDirection, direction),
                Enabled: () => Known(Id.TiltDirection) && Tilted)
            {
                Shown = () => Current.Offers(Id.TiltDirection),
            },
            new Section("Bokeh shape"),
            Shape("Tangential scale", Id.Tangential, -3f, 3f),
            Shape("Sagittal scale", Id.Sagittal, -3f, 3f),
            Shape("Anamorphic ratio", Id.Anamorph, 1f, 3f),
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

        // The shader the rows drive. With both installed, it is the one that is on, and otherwise the one last chosen.
        private static Shader Current
        {
            get
            {
                if (!Cinematic.Known(Id.Technique))
                    return Immerse;
                if (!Immerse.Known(Id.Technique) || Cinematic.On && !Immerse.On)
                    return Cinematic;
                return Immerse.On && !Cinematic.On || choice.Value != Cinematic.File ? Immerse : Cinematic;
            }
        }

        // The focus point decides the focus, with its point readable: in the autofocus mode, or as the pivot of a tilted
        // plane in focus, also at a manual distance.
        private static bool Aiming => Known(Id.FocusPoint) && (!Current.Offers(Id.FocusRange) || Known(Id.FocusRange)) && (Mode == PointFocus || Tilted);

        private static bool Tilted => Known(Id.TiltAngle) && Value(Id.TiltAngle) != 0f;

        private static float Range => Current.Offers(Id.FocusRange) ? Value(Id.FocusRange) : PixelRange;

        // The focus point can be seen and moved while the depth of field is one of ReShade's effects, also with ReShade's
        // global switch off: effects cost frame rate, and the point is set for the screenshot, which turns them on.
        // With the marker switched off, the stick and the click do not move a point that cannot be seen.
        private static bool Marked => ShowMarker && Aiming && Value(Id.Technique) != 0f;

        // Back with the right stick moves the focus point, so the camera does not also turn. A binding is a chord of
        // buttons or one stick direction and never both, so the pad is read here, as the panel's navigation reads it.
        public static bool Steering => Marked && CameraUi.View == View.Hud && Controls.Owner == InputOwner.CameraTools
            && (Gamepad.Current.Buttons & PadButtons.Back) != 0;

        // The focus plane's paint is on, or may be: a set has not read back yet, or ReShade has only just loaded its
        // effects and their values have not arrived.
        public static bool Painting
        {
            get
            {
                var debug = Current.Cells[(int)Id.FocusDebug];
                return ready && presents < settled || debug.Known && (debug.X != 0f || presents < debug.Until);
            }
        }

        // How long the shader must render before its focus has arrived, for the screenshot. Manual distance and
        // click-to-focus do not ease.
        public static float FocusSeconds
        {
            get
            {
                var shader = Current;
                if (!shader.On || Known(Id.FocusMode) && Mode != PointFocus)
                    return 0f;
                return shader.FocusSeconds(Known(Id.FocusSpeed) ? Value(Id.FocusSpeed) : shader.DefaultSpeed) + FocusMargin;
            }
        }

        private static string Problem => !ReShade.Connected ? ReShade.Missing
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
            var category = MelonPreferences.CreateCategory("CameraToolsReShade");
            link = category.CreateEntry("LinkFocalLengthToFov", false,
                description: "Keep the depth of field's focal length and the free camera's field of view in step, as on a full-frame camera.");
            marker = category.CreateEntry("ShowFocusPoint", true,
                description: "Draw the depth of field's focus point on the picture in Focus point mode.");
            choice = category.CreateEntry("DepthOfFieldShader", Immerse.File,
                description: $"The depth of field shader the ReShade tab drives while both are installed: {Immerse.File} or {Cinematic.File}.");
            MelonPreferences.Save();
        }

        // The window the autofocus samples, while the focus point shows; aspect is the screen's width over its height.
        public static ScreenBox? FocusWindow(float aspect)
        {
            if (!Marked)
                return null;
            var point = Current.Cells[(int)Id.FocusPoint];
            return Lens.FocusWindow(point.X, point.Y, Range, aspect);
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
                Clear();
                settled = presents + SetPresents;
            }
            ready = loaded;
            bool active = freecamActive && ReShade.Connected;
            bool panel = CameraUi.PanelOpen;
            float now = Time.unscaledTime;
            if (active)
            {
                foreach (var shader in Shaders)
                    for (int index = 0; index < shader.Cells.Length; index++)
                        shader.Read(index);
                Exclude();
                Steer();
                Click();
                Follow();
                Paint(now);
            }
            else
            {
                // Leaving the free camera within a second of a change that painted must not leave the paint on.
                if (painting)
                    StopPainting();
                Clear();
            }
            // The panel's changes are saved when it closes, and changes made outside it once they stop. A screenshot in
            // progress is left alone: it saved before it started.
            if (dirty && !panel && Screenshot.State is ShotState.Idle && (panelWas || !active || now - changed >= SaveDelay))
                Save();
            panelWas = panel;
        }

        // A change the user made, which is saved to the preset later. A held mouse drag sets its slider every frame, so
        // a set to the same value is not a change.
        private static bool Change(Id id, float x, float y = 0f) => Change(Current, id, x, y);

        private static bool Change(Shader shader, Id id, float x, float y = 0f)
        {
            var cell = shader.Cells[(int)id];
            if (cell.Known && cell.X == x && cell.Y == y)
                return false;
            shader.Write(id, x, y);
            dirty = true;
            changed = Time.unscaledTime;
            return true;
        }

        private static SliderRow Shape(string label, Id id, float min, float max)
            => new(label, min, max, 0.05f, "0.00", () => Value(id), value => Change(id, value), Enabled: Has(id))
            {
                Shown = () => Current.Offers(id),
            };

        private static bool Known(Id id) => Current.Known(id);

        private static float Value(Id id) => Current.Value(id);

        private static void Clear()
        {
            foreach (var shader in Shaders)
                Array.Clear(shader.Cells);
        }

        // Choosing the other shader while the depth of field is on switches the chosen one on and the other off.
        private static void Choose(Shader next)
        {
            var previous = Current;
            choice.Value = next.File;
            MelonPreferences.Save();
            if (next == previous || !previous.On)
                return;
            StopPainting();
            Change(previous, Id.Technique, 0f);
            Change(next, Id.Technique, 1f);
        }

        // Only one depth of field renders: one switched on in ReShade's overlay beside the other turns the other off.
        // A forced value changed in ReShade's overlay is set back.
        private static void Exclude()
        {
            var shader = Current;
            foreach (var other in Shaders)
            {
                if (other != shader && shader.On && other.On)
                    Change(other, Id.Technique, 0f);
                for (int index = 0; index < other.Uniforms.Length; index++)
                    if (other.Uniforms[index]?.Forced is float forced && other.Known((Id)index) && other.Value((Id)index) != forced)
                        Change(other, (Id)index, forced);
            }
            if (shader.On && Immerse.Known(Id.Technique) && Cinematic.Known(Id.Technique) && choice.Value != shader.File)
            {
                choice.Value = shader.File;
                MelonPreferences.Save();
            }
        }

        private static Func<bool> Has(Id id) => () => Known(id);

        // A change that moves the plane in focus paints it for a second.
        private static void Repaint(Id id, float value)
        {
            if (Change(id, value))
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
            var point = Current.Cells[(int)Id.FocusPoint];
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

        // With the cursor free, a click on the picture puts the focus point there, also beside the open panel unless the
        // click ends typing in a field. The point is kept as the shader takes it, as a fraction of the screen, so it means
        // the same place at every resolution.
        private static void Click()
        {
            if (!Marked || Freecam.Focused || !Input.GetMouseButtonDown(0))
                return;
            var mouse = Input.mousePosition;
            if (CameraUi.View != View.Hud
                && (CameraUi.View != View.Panel || Controls.TextCapture || CameraUi.OverPanel(new Vector2 { x = mouse.x, y = mouse.y })))
                return;
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

        // The focus plane is painted for a second after the focus distance or the tilt changed, and never while the UI is
        // hidden or a screenshot is in progress, whoever turned the paint on.
        private static void Paint(float now)
        {
            var shader = Current;
            var debug = shader.Cells[(int)Id.FocusDebug];
            if (!debug.Known)
                return;
            bool hidden = CameraUi.View == View.Hidden || Screenshot.State is not ShotState.Idle;
            if (now < paintUntil && !hidden)
            {
                painting = true;
                if (debug.X != shader.Painted)
                    shader.Write(Id.FocusDebug, shader.Painted);
            }
            else if (debug.X == 0f)
                painting = false;
            else if (painting || hidden)
                shader.Write(Id.FocusDebug, 0f);
        }

        private static void StopPainting()
        {
            paintUntil = 0f;
            foreach (var shader in Shaders)
            {
                var debug = shader.Cells[(int)Id.FocusDebug];
                if (shader == Current && painting || debug.Known && debug.X != 0f)
                    shader.Write(Id.FocusDebug, 0f);
            }
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
