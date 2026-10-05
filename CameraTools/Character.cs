using UnityEngine;

namespace CameraTools
{
    internal static class Character
    {
        private static Transform avatars;

        // The active character is the one active child of the avatar root.
        public static Transform Active()
        {
            if (!avatars)
            {
                var root = GameObject.Find("/EntityRoot/AvatarRoot");
                if (!root)
                    return null;
                avatars = root.transform;
            }
            for (int i = 0; i < avatars.childCount; i++)
            {
                var child = avatars.GetChild(i);
                if (child.gameObject.activeInHierarchy)
                    return child;
            }
            return null;
        }
    }
}
