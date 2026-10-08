namespace CameraTools
{
    // Every writer goes through Value, so the settings panel, hotkeys, and the controller share one clamp.
    public sealed class Setting
    {
        private float value;

        public Setting(float min, float max, float value)
        {
            Min = min;
            Max = max;
            Value = value;
        }

        public float Min { get; }
        public float Max { get; }

        public float Value
        {
            get => value;
            set => this.value = Math.Clamp(value, Min, Max);
        }
    }

    public sealed class CameraSettings
    {
        public Setting MoveSpeed { get; } = new(0.001f, 10f, 0.5f);
        public Setting LookSensitivity { get; } = new(0.1f, 10f, 1f);
        public Setting RollSpeed { get; } = new(0.1f, 10f, 1f);
        public Setting FovSpeed { get; } = new(0.01f, 10f, 0.5f);
        public Setting Fov { get; } = new(1f, 160f, 45f);
        public Setting Damping { get; } = new(0.01f, 1f, 1f);
        public bool RememberPosition { get; set; }

        // The game's own near clip, read when the free camera starts.
        public float GameNear { get; set; }

        // Null while the free camera keeps the game's own near clip.
        private float? nearClip;

        // The camera cuts away anything closer than this, so a close-up needs it below the game's own.
        public float NearClip
        {
            get => Math.Min(nearClip ?? GameNear, GameNear);
            set => nearClip = value < GameNear ? value : null;
        }
    }
}
