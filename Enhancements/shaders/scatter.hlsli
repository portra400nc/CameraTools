// Light scattered in the air, shared by shaders/atmosphere_ps.hlsl, the light-shaft shaders and core/plan.cpp, which
// compiles it as C++ with float3 as one float, so the native tests check the shaders' own formulas one color channel at
// a time. Keep it to what both languages read alike.

static const float kPi = 3.14159265;

// Henyey-Greenstein, normalized over the sphere.
float henyey_greenstein(float cosine, float g)
{
    float denominator = 1 + g * g - 2 * g * cosine;
    return (1 - g * g) / (4 * kPi * denominator * sqrt(denominator));
}

// How much the mist may thicken below the camera, as a power of e: a valley far below fills with mist without turning
// into a wall a meter in front of the camera.
static const float kMaxThicken = 3;

// The mist's extinction per meter at a point rise meters above the camera, where the world's patch noise is noise, 0 to
// 1: density at the camera's height, thinning by e every height meters up and thickening as fast below, times a patch
// factor that patchiness mixes in. The factor stretches the noise toward 0 and 1 for banks and holes and is symmetric
// about the noise's middle, so it averages 1, and patchiness moves the mist around without changing how much there is.
float mist_density(float density, float rise, float height, float noise, float patchiness)
{
    return density * exp(clamp(-rise / height, -80, kMaxThicken)) * lerp(1, 2 * smoothstep(0.25, 0.75, noise), patchiness);
}

// Where sample i of n is taken along a ray ray_length meters long, jitter (0 to 1) of the way through its span. Spans
// grow quadratically, short near the camera, where shafts are large on screen, and long far away.
float march_at(float i, float n, float jitter, float ray_length)
{
    float u = (i + jitter) / n;
    return ray_length * u * u;
}

// How long sample i's span is: from ray_length (i / n)^2 to ray_length ((i + 1) / n)^2. The n spans cover the ray exactly.
float march_span(float i, float n, float ray_length) { return ray_length * (2 * i + 1) / (n * n); }

// The light a meter of mist scatters toward the camera per unit of extinction: the sun's, where the sun reaches it, and
// the sky's.
float3 mist_light(float3 sun, float visibility, float3 sky) { return sun * visibility + sky; }

// A march so far: the light the mist scattered toward the camera, and the share of what lies behind it that comes through.
struct Mist
{
    float3 inscatter;
    float transmittance;
};

// One step of dt meters through mist of constant extinction density, lit by light. The step is integrated exactly, so an
// even mist gives the same result at any step count, and for short steps it adds transmittance * density * dt * light.
Mist mist_step(Mist m, float density, float dt, float3 light)
{
    float through = exp(-density * dt);
    m.inscatter += m.transmittance * (1 - through) * light;
    m.transmittance *= through;
    return m;
}

// The march over the scene behind it.
float3 mist_over(float3 scene, Mist m) { return scene * m.transmittance + m.inscatter; }

// A march taken to a ray ratio times as long, as if its mist and light were even along the ray: exact for an even mist,
// and close for rays of similar length.
Mist mist_rescale(Mist m, float ratio)
{
    float optical_depth = -log(max(m.transmittance, 1e-6));
    Mist r;
    r.transmittance = exp(-optical_depth * ratio);
    r.inscatter = m.inscatter * (optical_depth > 1e-4 ? (1 - r.transmittance) / (1 - m.transmittance) : ratio);
    return r;
}
