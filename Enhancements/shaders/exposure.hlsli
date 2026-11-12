// Auto exposure, shared by exposure_meter_ps.hlsl, exposure_adapt_ps.hlsl and core/exposure.cpp: the scene is metered by
// the mean of its pixels' log2 luminance, the sky left out, and the exposure moves toward the one that brings that mean to
// middle gray, by a share of the way set by the strength.

static const float kExposureMiddleGray = 0.18;
static const float kExposureLeast = -2.0;  // stops the metering may take off the game's exposure
static const float kExposureMost = 3.0;    // and add to it, so a night scene stays dark

// A pixel's log2 luminance, floored so a black pixel does not sink the mean.
float exposure_log_luminance(float3 hdr)
{
    return log2(max(dot(hdr, float3(0.2126, 0.7152, 0.0722)), 1e-4));
}

// The stops the metering adds to the game's exposure: strength 0 none, 1 all the way to middle gray.
float exposure_target_ev(float mean_log2, float game_exposure, float strength)
{
    float full = log2(kExposureMiddleGray / game_exposure) - mean_log2;
    return clamp(full * strength, kExposureLeast, kExposureMost);
}

// This frame's stops: eased from the last frame's by ease, the share of the way one frame moves, and at once when the last
// frame's is no value.
float exposure_next_ev(float previous, float target, float ease)
{
    if (!(previous >= kExposureLeast && previous <= kExposureMost))
        return target;
    return lerp(previous, target, ease);
}
