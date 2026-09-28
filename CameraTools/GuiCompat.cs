using UnityEngine;

namespace CameraTools
{
    // Genshin's IL2CPP build strips GUILayout.HorizontalSlider and GUI.DragWindow because the game never
    // calls them. These rebuild both from the IMGUI calls that survive.
    internal static class GuiCompat
    {
        public static float HorizontalSlider(float value, float left, float right)
        {
            var skin = GUI.skin;
            var rect = GUILayoutUtility.GetRect(40f, 12f, skin.horizontalSlider, new[] { GUILayout.ExpandWidth(true) });
            return GUI.Slider(rect, value, 0f, left, right, skin.horizontalSlider, skin.horizontalSliderThumb, true, 0);
        }

        // Call inside the window function. Returns how far the title bar was dragged this event, to be added
        // to the window rect after GUILayout.Window returns (it overwrites changes made inside the callback).
        public static Vector2 DragWindow(float width, float titleHeight)
        {
            int id = GUIUtility.GetControlID("CameraToolsDrag".GetHashCode(), FocusType.Passive);
            var e = Event.current;
            switch (e.GetTypeForControl(id))
            {
                case EventType.MouseDown:
                    var p = e.mousePosition;
                    if (p.x >= 0 && p.x <= width && p.y >= 0 && p.y <= titleHeight)
                    {
                        GUIUtility.hotControl = id;
                        e.Use();
                    }
                    break;
                case EventType.MouseDrag:
                    if (GUIUtility.hotControl == id)
                    {
                        var delta = e.delta;
                        e.Use();
                        return delta;
                    }
                    break;
                case EventType.MouseUp:
                    if (GUIUtility.hotControl == id)
                    {
                        GUIUtility.hotControl = 0;
                        e.Use();
                    }
                    break;
            }
            return Vector2.zero;
        }
    }
}
