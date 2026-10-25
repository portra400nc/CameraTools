// The depth of field's arithmetic, shaders/dof.hlsli compiled as C++: the circle of confusion at known lens values, each
// channel's under axial chromatic aberration, the aperture's shape at known points, the light across the disc, the
// highlights' growth, and which highlights become sprites and what their discs give.
#include "../core/dof.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace enhancements;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    bool near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

    // A 50 mm lens at f/2 on a full-frame sensor (24 mm high) at a render height of 800: 400 / 24 half-size pixels a mm.
    DofOptics lens50(float chromatic = 0)
    {
        DofOptics o;
        o.focal_mm = 50;
        o.f_number = 2;
        o.pixels_per_mm = 400.0f / 24.0f;
        o.chromatic = chromatic;
        return o;
    }

    // Unity's GL.GetGPUProjectionMatrix for a camera of vertical field of view 2 atan(1 / b) and aspect b / a with near 0.1
    // and far 1000, reversed Z, column-major.
    Camera reversed_camera(float a, float b)
    {
        Camera c {};
        const float n = 0.1f, f = 1000;
        c.view_to_clip[0] = a;
        c.view_to_clip[5] = b;
        c.view_to_clip[10] = n / (f - n);
        c.view_to_clip[11] = -1;
        c.view_to_clip[14] = n * f / (f - n);
        for (int i = 0; i < 4; i++)
            c.world_to_view[i * 5] = 1;
        return c;
    }

    // A 1280x800 frame at 16:10 through a projection of P11 p11 (50 / 12 by default: 50 mm on a 24 mm sensor), everything at
    // background meters, black, rows as stored.
    struct Scene
    {
        Camera camera;
        DofImage image;
        std::array<float, 4> k {};
        explicit Scene(float background, float p11 = 50.0f / 12)
            : camera(reversed_camera(p11 / 1.6f, p11)), k(*dof_depth_coefficients(camera))
        {
            image.size = { 1280, 800 };
            image.rgb.assign(size_t(1280) * 800 * 3, 0.0f);
            image.depth.assign(size_t(1280) * 800, reversed(background));
        }
        // 1 / z = -(k0 d + k1) / (k2 d + k3), solved for d.
        float reversed(float meters) const { return -(k[1] + k[3] / meters) / (k[0] + k[2] / meters); }
        void put(uint32_t x, uint32_t y, float value, float meters)
        {
            for (int c = 0; c < 3; c++)
                image.rgb[(size_t(y) * 1280 + x) * 3 + c] = value;
            image.depth[size_t(y) * 1280 + x] = reversed(meters);
        }
        float green(const DofOutput &out, uint32_t x, uint32_t y) const { return out.rgb[(size_t(y) * 1280 + x) * 3 + 1]; }
    };

    DofOutput render(const Scene &scene, const DepthOfFieldSettings &settings)
    {
        return render_dof(scene.image, *dof_constants(settings, scene.camera, scene.image.size, true, 0, 0), 0);
    }

    DepthOfFieldSettings lens_at(float f_number, float focus, float aberration)
    {
        DepthOfFieldSettings s;
        s.enabled = true;
        s.f_number = f_number;
        s.focus_distance = focus;
        s.spherical_aberration = aberration;
        s.highlight_gain = 0;
        s.scatter = true;
        s.bokeh_contrast = 0;
        return s;
    }

    DepthOfFieldSettings lens_at2(float f_number, float focus, float aberration, float gain)
    {
        DepthOfFieldSettings s = lens_at(f_number, focus, aberration);
        s.highlight_gain = gain;
        return s;
    }

    double sum_green(const DofOutput &out)
    {
        double sum = 0;
        for (size_t i = 1; i < out.rgb.size(); i += 3)
            sum += out.rgb[i];
        return sum;
    }

    // The mean of the light across the unit disc, by the midpoint rule over 20,000 rings.
    double disc_mean(float aberration)
    {
        double sum = 0;
        constexpr int kRings = 20000;
        for (int i = 0; i < kRings; i++)
        {
            const double rho = (i + 0.5) / kRings;
            sum += dof_bokeh_intensity(static_cast<float>(rho), aberration) * 2 * rho / kRings;
        }
        return sum;
    }
}

int main(int argc, char **argv)
{
    // f^2 / (N (S - f)) (z - S) / z / 2, in half-size pixels: 2500 / (2 x 1950) / 2 x 400 / 24 = 5.3419 at infinity.
    check("a point 30 m away with a 50 mm f/2 lens focused at 2 m has a CoC of 4.9858 half-size pixels",
        near(dof_green_coc(30, 2, lens50()), 4.98575f));
    check("a point 1 m away has -5.3419, in front of focus", near(dof_green_coc(1, 2, lens50()), -5.34188f));
    check("a point on the focus plane has 0", near(dof_green_coc(2, 2, lens50()), 0));
    check("at f/4 the CoC halves", near(dof_green_coc(30, 2, [] { DofOptics o = lens50(); o.f_number = 4; return o; }()), 4.98575f / 2));
    const std::array<float, 3> plain = dof_channel_cocs(30, 2, lens50());
    check("without chromatic aberration every channel has green's CoC", plain[0] == plain[1] && plain[2] == plain[1] && near(plain[1], 4.98575f));

    // At chromatic 1 red's focal length is 0.05% and blue's 0.12% longer than green's, so their CoCs are green's less
    // 5.3419 x 2000 / 50 x 0.0005 / 1.0005 = 0.1068 and x 0.0012 / 1.0012 = 0.2561 at every depth.
    const std::array<float, 3> behind = dof_channel_cocs(30, 2, lens50(1));
    check("behind focus with chromatic 1, red's CoC is 4.8790, green's 4.9858 and blue's 4.7297",
        near(behind[0], 4.87897f) && near(behind[1], 4.98575f) && near(behind[2], 4.72965f));
    check("behind focus green's disc is the largest and blue's the smallest, so bokeh fringes green",
        behind[1] > behind[0] && behind[0] > behind[2]);
    const std::array<float, 3> front = dof_channel_cocs(1, 2, lens50(1));
    check("in front of focus with chromatic 1, red's CoC is -5.4487, green's -5.3419 and blue's -5.5980",
        near(front[0], -5.44866f) && near(front[1], -5.34188f) && near(front[2], -5.59798f));
    check("in front of focus red's and blue's discs are larger than green's, so bokeh fringes purple",
        std::fabs(front[2]) > std::fabs(front[0]) && std::fabs(front[0]) > std::fabs(front[1]));
    const std::array<float, 3> focused = dof_channel_cocs(2, 2, lens50(1));
    check("on green's focus plane red's CoC is -0.1068 and blue's -0.2561, in front of their own focus",
        focused[1] == 0 && near(focused[0], -0.10678f) && near(focused[2], -0.25610f));
    DofOptics tele = lens50(1);
    tele.focal_mm = 200;
    const std::array<float, 3> long_lens = dof_channel_cocs(30, 2, tele);
    check("through a 200 mm lens the shift grows with the focal length, over 4 times the 50 mm lens's: green's CoC 86.4198 at 30 m, "
          "red's 0.4627 less and blue's 1.1098",
        near(long_lens[1], 86.4198f, 2e-3f) && near(long_lens[1] - long_lens[0], 0.46273f, 2e-4f) && near(long_lens[1] - long_lens[2], 1.10978f, 2e-4f));
    tele.max_radius = 20;
    const std::array<float, 3> capped = dof_capped_cocs(30, 2, tele);
    check("capped at 20 half-size pixels, the 200 mm lens's discs keep their proportions: green's 20, red's 19.8929 and blue's 19.7432, "
          "not three discs of 20",
        near(capped[1], 20) && near(capped[0], 19.8929f, 2e-4f) && near(capped[2], 19.7432f, 2e-4f));
    DofOptics capped50 = lens50(1);
    capped50.max_radius = 5;
    const std::array<float, 3> capped_front = dof_capped_cocs(1, 2, capped50);
    check("in front of focus the widest channel, blue's, meets the cap of 5: red's -4.8666, green's -4.7713 and blue's -5",
        near(capped_front[0], -4.86663f) && near(capped_front[1], -4.77126f) && near(capped_front[2], -5));
    const std::array<float, 3> under = dof_capped_cocs(30, 2, capped50);
    check("discs under the cap keep their CoCs", under[0] == behind[0] && under[1] == behind[1] && under[2] == behind[2]);
    // A sensor 10 times taller on the same projection is a 500 mm lens with a tenth of the pixels a mm, the whole lens scaled
    // by 10, which images as the 50 mm lens does with every distance divided by 10: a 1:10 miniature.
    DofOptics model = lens50(1);
    model.focal_mm = 500;
    model.pixels_per_mm = 400.0f / 240.0f;
    const std::array<float, 3> miniature = dof_channel_cocs(30, 2, model), scaled = dof_channel_cocs(3, 0.2f, lens50(1));
    check("a 10 times taller sensor focused at 2 m gives a point 30 m away the CoCs of the 50 mm lens at 3 m focused at 0.2 m: 64.6760, "
          "64.8148 and 64.4819",
        near(miniature[0], 64.6760f, 2e-3f) && near(miniature[1], 64.8148f, 2e-3f) && near(miniature[2], 64.4819f, 2e-3f)
            && near(scaled[0], 64.6760f, 2e-3f) && near(scaled[1], 64.8148f, 2e-3f) && near(scaled[2], 64.4819f, 2e-3f));

    check("the light across the disc averages 1 at spherical aberration 1 behind focus (soap bubble)", std::fabs(disc_mean(1) - 1) < 1e-4);
    check("and at 1 in front of focus, where the clamp at 0 cuts the rim", std::fabs(disc_mean(-1) - 1) < 1e-4);
    check("and at 0.6, -0.6, -0.3 and 0", std::fabs(disc_mean(0.6f) - 1) < 1e-4 && std::fabs(disc_mean(-0.6f) - 1) < 1e-4
        && std::fabs(disc_mean(-0.3f) - 1) < 1e-4 && std::fabs(disc_mean(0) - 1) < 1e-4);
    check("at 1 behind focus the disc's center is dark and its rim 3 times the mean",
        dof_bokeh_intensity(0, 1) == 0 && near(dof_bokeh_intensity(1, 1), 3) && near(dof_bokeh_intensity(0.5f, 1), 0.1875f));
    check("at 1 in front of focus the center is 1.837 and the light is 0 past rho 0.904",
        near(dof_bokeh_intensity(0, -1), 2 * 0.918559f) && near(dof_bokeh_intensity(0.5f, -1), 1.66489f) && dof_bokeh_intensity(0.91f, -1) == 0);
    check("a source of radius 0 or 1e-4 weighs as one at the floor of 0.5, 1 / (pi 0.25) = 1.27324 at its center, at any aberration",
        near(dof_source_weight(0, 0, 0), 1.273240f) && near(dof_source_weight(1e-4f, 0, 0), 1.273240f) && dof_source_weight(0, 0, 1) == dof_source_weight(0.5f, 0, 1)
            && near(dof_source_weight(0, 0, 1), 1.273240f) && near(dof_source_weight(0, 0.75f, 0), 0.318310f) && dof_source_weight(0, 1, 0) == 0);
    check("from a pixel past the floor a disc has the lens's profile: 0 at the center of radius 1.5 at aberration 1, and half the flat light at radius 1, 0.5 / pi",
        dof_source_weight(1.5f, 0, 1) == 0 && near(dof_source_weight(1, 0, 1), 0.5f / (3.14159265f * 1.0f)));
    check("the clamp's normalization is 0.91856 at -1 and 1 from -0.5 up", near(dof_bokeh_norm(-1), 0.918559f) && dof_bokeh_norm(-0.5f) == 1
        && dof_bokeh_norm(1) == 1);

    check("a highlight of luminance 1.5 and contrast 2.25 grows at gain 2 over a threshold of 1 by 0.08 x 16 pi (1 - e^-2) to 4.97702 for a "
          "disc of radius 4",
        near(dof_grow_highlight(1.5f, 2.25f, 1, 2, 4), 4.977024f));
    check("however high its contrast, it adds at most 0.08 thresholds to each pixel of its disc: 5.52124 at radius 4, 17.58496 at 8",
        near(dof_grow_highlight(1.5f, 100, 1, 2, 4), 5.521239f) && near(dof_grow_highlight(1.5f, 100, 1, 2, 8), 17.584955f, 1e-3f));
    check("below a contrast of 1.25 and at gain 0 it keeps its own light, to the bit",
        dof_grow_highlight(1.5f, 1.2f, 1, 2, 4) == 1.5f && dof_grow_highlight(1.5f, 3, 1, 0, 4) == 1.5f);

    DofShapeParams round;
    check("a round disc of radius 10 covers its center and 9.5 pixels out fully, its edge by half, and 11 pixels out not",
        dof_aperture(0, 0, 10, 0, 0, round) == 1 && dof_aperture(9.5f, 0, 10, 0, 0, round) == 1 && dof_aperture(0, -10, 10, 0, 0, round) == 0.5f
        && dof_aperture(11, 0, 10, 0, 0, round) == 0);
    check("seven straight blades cover 0.87103 of a disc, so their shape grows by 1.0715 to keep the area",
        near(dof_area_scale(7, 0, 1), 0.933288f) && near(dof_shape_reach(7, 0, 1), 1.07148f));
    DofShapeParams seven;
    seven.blades = 7;
    check("seven blades at rotation 0 put a corner straight up: 10.5 pixels up is covered by 0.7005",
        near(dof_aperture(0, 10.5f, 10, 0, 0, seven), 0.700476f));
    const float side = 3.14159265f / 2 + 3.14159265f / 7;
    check("and a side's middle a seventh of a turn further: 10 pixels out is covered by 0.1413",
        near(dof_aperture(10 * std::cos(side), 10 * std::sin(side), 10, 0, 0, seven), 0.141285f));
    seven.rotation_degrees = 90;
    check("turned 90 degrees, the corner points left and 10.5 pixels up lies outside",
        near(dof_aperture(-10.5f, 0, 10, 0, 0, seven), 0.700476f) && dof_aperture(0, 10.5f, 10, 0, 0, seven) == 0);
    seven.roundness = 1;
    check("fully rounded blades make a disc", dof_area_scale(7, 1, 1) == 1 && dof_aperture(0, 10, 10, 0, 0, seven) == 0.5f);

    DofShapeParams squeezed;
    squeezed.anamorphic = 2;
    check("an anamorphic squeeze of 2 halves the width and grows the shape by 1.4142 to keep the area: 14 pixels up and 7 across are "
          "covered by 0.6005",
        near(dof_area_scale(0, 0, 2), 0.707107f) && near(dof_aperture(0, 14, 10, 0, 0, squeezed), 0.600505f)
            && near(dof_aperture(7, 0, 10, 0, 0, squeezed), 0.600505f) && dof_aperture(10, 0, 10, 0, 0, squeezed) == 0);

    DofShapeParams cat;
    cat.cat_eye = 0.6f;
    check("cat's eye 0.6 leaves a disc at the picture's center whole inside, and its edge pixel a quarter",
        dof_aperture(-9, -3, 10, 0, 0, cat) == 1 && near(dof_aperture(0, -10, 10, 0, 0, cat), 0.25f));
    check("and at the top right corner cuts its lower left: (-9, -3) is out, (9, 3) in, (-4.5, 1) 0.3284 and (-4.4, 1.2) 0.5383",
        dof_aperture(-9, -3, 10, 1, 1, cat) == 0 && dof_aperture(9, 3, 10, 1, 1, cat) == 1 && near(dof_aperture(-4.5f, 1, 10, 1, 1, cat), 0.328416f)
            && near(dof_aperture(-4.4f, 1.2f, 10, 1, 1, cat), 0.538307f));

    DofShapeParams swirl;
    swirl.swirl = 1;
    const float ex = 1.6f / std::sqrt(3.56f), ey = 1 / std::sqrt(3.56f);
    check("swirl 1 at the top right corner squeezes the disc 2.5 times along the line from the center: 6 pixels out along it lies "
          "outside, 6 pixels across it inside",
        dof_aperture(6 * ex, 6 * ey, 10, 1, 1, swirl) == 0 && dof_aperture(-6 * ey, 6 * ex, 10, 1, 1, swirl) == 1);
    check("and leaves a disc at the picture's center round", dof_aperture(9.5f, 0, 10, 0, 0, swirl) == 1 && dof_aperture(0, 9.5f, 10, 0, 0, swirl) == 1);

    const Camera camera = reversed_camera(2.0f, 3.2f);
    const std::optional<std::array<float, 4>> k = dof_depth_coefficients(camera);
    check("the depth coefficients take reversed depth 1 to the near plane, 0 to the far one, and 0.0001 to 500.025 m",
        k && near(dof_view_depth(*k, 1), 0.1f, 1e-6f) && near(dof_view_depth(*k, 0), 1000, 0.05f) && near(dof_view_depth(*k, 0.0001f), 500.025f, 0.01f));
    check("a projection that does not invert has none", !dof_depth_coefficients(Camera {}));

    DepthOfFieldSettings settings;
    settings.focus_point[1] = 0.25f;
    settings.focus_seconds = 2;
    settings.blades = 7;
    settings.blade_roundness = 0;
    settings.blade_rotation = 90;
    settings.spherical_aberration = -1;
    settings.chromatic = 1;
    const Camera lens = reversed_camera(50.0f / 12 / 1.6f, 50.0f / 12);
    check("highlight sprites are off by default", !settings.scatter && dof_constants(settings, lens, { 1280, 800 }, true, 0.5f, 1)->highlight[2] == 0);
    settings.scatter = true;
    const DofConstants c = *dof_constants(settings, lens, { 1280, 800 }, true, 0.5f, 1);
    check("the constants: a 50 mm focal length from the projection and the 24 mm sensor, f/2.8, 16.667 half-size pixels a mm, a largest "
          "radius of 12",
        near(c.optics[0], 50) && near(c.optics[1], 2.8f) && near(c.optics[2], 16.6667f) && near(c.optics[3], 12));
    check("the focus point stored upside down, and the autofocus at 2 seconds 1 - 0.01^0.25 of the way after half a second",
        c.focus[0] == 0 && c.focus[1] == 0.5f && c.focus[2] == 0.75f && near(c.focus[3], 0.683772f));
    check("seven straight blades turned 90 degrees, grown by 1.0715, which the gather's radius grows by too, chromatic aberration or not",
        c.shape[0] == 7 && c.shape[1] == 0 && near(c.shape[2], 1.570796f) && near(c.shape[3], 0.933288f) && near(c.bokeh[3], 1.07148f));
    check("spherical aberration -1 normalized in front of focus, where it is +1, and behind", c.bokeh[0] == -1 && near(c.bokeh[1], 0.918559f) && c.bokeh[2] == 1);
    check("49 samples are 3 rings, frame 1 turns them 0.618, flip makes stored rows run up, and the aspect is 1.6",
        c.sampling[0] == 3 && near(c.sampling[1], 0.618034f) && c.sampling[2] == 1 && c.sampling[3] == 1.6f);
    check("the half size 640x400 in 80x50 tiles, a reach of 2 tiles, and depth samples 8 pixels apart", c.sizes[0] == 640 && c.sizes[1] == 400
        && c.sizes[2] == 80 && c.sizes[3] == 50 && c.counts[0] == 2 && c.counts[1] == 1280 && c.counts[2] == 800 && c.counts[3] == 8);
    check("the highlights from twice the frame's mean luminance at gain 2 by default, with scatter as many sprites a field as 8 half-size pictures of "
          "the largest quad, 12 x 1.0715 + 1 pixels from its center, cover: 2666, and the near field's from slot 4096",
        c.highlight[0] == 2 && c.highlight[1] == 2 && c.highlight[2] == 2666 && c.highlight[3] == 4096);
    check("rings: 9 samples are 2, 64 are 4, 289 are 8 and 1089 are 16", dof_rings(9) == 2 && dof_rings(64) == 4 && dof_rings(289) == 8 && dof_rings(1089) == 16);
    settings.focus_seconds = 0.1f;
    check("at 0.1 seconds the autofocus goes 1 - 0.01^(1/6) of the way in a frame at 60 fps",
        near(dof_constants(settings, lens, { 1280, 800 }, false, 1.0f / 60, 0)->focus[3], 0.535841f));
    settings.focus_seconds = 0;
    check("0 seconds of focus jumps", dof_constants(settings, lens, { 1280, 800 }, false, 0.5f, 0)->focus[3] == 1
        && dof_constants(settings, lens, { 1280, 800 }, false, 0.5f, 0)->focus[2] == 0.25f);
    check("a projection without a focal length has no constants", !dof_constants(settings, Camera {}, { 1280, 800 }, true, 0, 0));
    {
        const Scene far(30);
        DepthOfFieldSettings set = lens_at(2, 3, 0);
        set.focus_seconds = 1;
        const float eased = render_dof(far.image, *dof_constants(set, far.camera, far.image.size, true, 0.5f, 0), 1).focus;
        set.focus_seconds = 0;
        const float jumped = render_dof(far.image, *dof_constants(set, far.camera, far.image.size, true, 0.5f, 0), 1).focus;
        check("a set distance eases from the last frame's as the autofocus does: from 1 m toward 3 m at 1 second, 2.8 m after half a "
              "second, and at 0 seconds it jumps", near(eased, 2.8f) && jumped == 3);
    }

    {
        Scene point(30);
        point.put(640, 400, 100, 30);
        double before = 0, after = 0;
        const DofOutput blurred = render(point, lens_at(2, 2, 0));
        for (size_t i = 1; i < blurred.rgb.size(); i += 3)
        {
            before += point.image.rgb[i];
            after += blurred.rgb[i];
        }
        check("a point 30 m away with the lens focused at 2 m keeps its light within 1% as it spreads into a disc", std::fabs(after / before - 1) < 0.01
            && point.green(blurred, 640, 400) < 1 && point.green(blurred, 646, 400) > 0.1f && point.green(blurred, 640, 406) > 0.1f
            && point.green(blurred, 664, 400) == 0);
        // At spherical aberration 1 the rim, 8 to 9 pixels out, outshines the center 3 pixels around.
        const DofOutput bubble = render(point, lens_at(2, 2, 1));
        float center = 0, rim = 0;
        for (int a = 0; a < 8; a++)
        {
            const float angle = a * 0.785398f;
            center += point.green(bubble, 640 + uint32_t(std::lround(3 * std::cos(angle))), 400 + uint32_t(std::lround(3 * std::sin(angle))));
            rim += point.green(bubble, 640 + uint32_t(std::lround(8.5f * std::cos(angle))), 400 + uint32_t(std::lround(8.5f * std::sin(angle))));
        }
        check("at spherical aberration 1 behind focus the disc's rim is over twice as bright as its center (soap bubble)", rim > 2 * center && center > 0);
        const DofOutput sharp = render(point, lens_at(2, 30, 0));
        check("focused at the point it stays as it was, to the bit", sharp.rgb == point.image.rgb);
    }
    {
        // Bokeh contrast. A gray field 30 m away with lens focused at 2 m (discs of 5 half-size pixels), and in it a grid of
        // tips 1.67 times as bright, one half-size texel each, 24 pixels apart, like a lawn's bright blade tips.
        Scene flat(30);
        for (float &v : flat.image.rgb)
            v = 0.3f;
        Scene lawn = flat;
        for (uint32_t y = 200; y < 600; y += 24)
            for (uint32_t x = 300; x < 980; x += 24)
                for (uint32_t j = 0; j < 2; j++)
                    for (uint32_t i = 0; i < 2; i++)
                        lawn.put(x + i, y + j, 0.5f, 30);
        DepthOfFieldSettings plain = lens_at(2, 2, 0), lifted = plain;
        lifted.bokeh_contrast = 1;
        const DofOutput flat_out = render(flat, lifted);
        float least = 1, most = 0;
        for (const float v : flat_out.rgb)
        {
            least = std::min(least, v);
            most = std::max(most, v);
        }
        check("a flat gray field keeps its light at every pixel at bokeh contrast 1", near(least, 0.3f, 2e-4f) && near(most, 0.3f, 2e-4f));
        const DofOutput before = render(lawn, plain), after = render(lawn, lifted);
        double sum_before = 0, sum_after = 0;
        for (uint32_t y = 300; y < 500; y++)
            for (uint32_t x = 400; x < 880; x++)
            {
                sum_before += lawn.green(before, x, y);
                sum_after += lawn.green(after, x, y);
            }
        check("a lawn of tips 1.67 times its gray: at bokeh contrast 1 a tip's disc rises from 0.3032 to 0.3470 and the gaps darken from "
              "0.3000 to 0.2629, while the lawn keeps its light within 0.1%",
            near(lawn.green(before, 637, 393), 0.303177f) && near(lawn.green(after, 637, 393), 0.346985f) && near(lawn.green(before, 649, 405), 0.300049f)
                && near(lawn.green(after, 649, 405), 0.262939f) && std::fabs(sum_after / sum_before - 1) < 0.001);
    }
    {
        // A white frame: a plane 2.16 m away, where with chromatic 1 at f/1 and 2 m green's disc is 0.79 half-size pixels,
        // red's 0.58 and blue's 0.28, next to a background 30 m away whose discs of 10 widen the gather's spiral past all
        // three. Whatever each channel's disc, white spread over white stays white.
        Scene white(30);
        for (float &v : white.image.rgb)
            v = 1;
        for (uint32_t y = 0; y < 800; y++)
            for (uint32_t x = 0; x < 640; x++)
                white.put(x, y, 1, 2.16f);
        float least = 1, most = 1;
        for (const float aberration : { 0.6f, 1.0f, -1.0f })
        {
            DepthOfFieldSettings settings = lens_at(1, 2, aberration);
            settings.chromatic = 1;
            for (const float v : render(white, settings).rgb)
            {
                least = std::min(least, v);
                most = std::max(most, v);
            }
        }
        check("a white frame stays white at every pixel and channel with chromatic 1 at spherical aberration 0.6, 1 and -1, each channel's disc under a pixel beside a far blur", least > 0.999f && most < 1.001f);
    }
    {
        // A white frame through a 202 mm lens (P11 17.61 on a 22.95 mm sensor) at f/2.8 focused at 2.66 m, with cat's eye 0.6,
        // spherical aberration 0.6 and chromatic 1: its top half 2.63 m away, where green's disc is 0.59 half-size pixels in
        // front of focus and blue's 1.41, its bottom half 2.70 m away, behind focus, and nothing farther, so no tile reaches
        // past these small discs. Each channel's disc must be found whole and composited by its own coverage.
        Scene white(30, 17.6105576f);
        for (uint32_t y = 0; y < 800; y++)
            for (uint32_t x = 0; x < 1280; x++)
                white.put(x, y, 1, y < 400 ? 2.70f : 2.63f);
        DepthOfFieldSettings settings = lens_at(2.8f, 2.66f, 0.6f);
        settings.sensor_height_mm = 22.95f;
        settings.cat_eye = 0.6f;
        settings.chromatic = 1;
        const DofOutput out = render(white, settings);
        float least = 1, most = 1;
        for (const float v : out.rgb)
        {
            least = std::min(least, v);
            most = std::max(most, v);
        }
        check("a white frame stays white at every pixel and channel through a 202 mm lens at chromatic 1, just in front of focus and just behind",
            least > 0.999f && most < 1.001f);
    }
    {
        Scene edge(30);
        for (uint32_t y = 200; y < 600; y++)
            for (uint32_t x = 400; x < 800; x++)
                edge.put(x, y, 1, 2);
        const DofOutput front = render(edge, lens_at(2, 30, 0));
        check("a white square 2 m away in front of focus at 30 m spreads over its edge evenly: half at the edge, 0 and 1 ten pixels either side",
            near(edge.green(front, 399, 400) + edge.green(front, 400, 400), 1, 0.05f) && edge.green(front, 388, 400) < 0.02f
                && edge.green(front, 412, 400) > 0.98f);
        const DofOutput behind = render(edge, lens_at(2, 1, 0));
        check("behind focus at 1 m, with the background black at 30 m behind it, it spreads as evenly",
            near(edge.green(behind, 399, 400) + edge.green(behind, 400, 400), 1, 0.05f) && edge.green(behind, 412, 400) > 0.97f);
        DepthOfFieldSettings autofocus = lens_at(2, 0, 0);
        const DofOutput focused = render(edge, autofocus);
        check("the autofocus at the picture's center finds the square 2 m away, and keeps it sharp", near(focused.focus, 2, 0.001f)
            && edge.green(focused, 640, 400) == 1 && edge.green(focused, 401, 400) == 1);
        autofocus.focus_point[0] = 0.1f;
        check("and at the left finds the background 30 m away", near(render(edge, autofocus).focus, 30, 0.01f));
    }
    {
        // An eye whose eyeball's center is 0.5 m away at the picture's center, through the 50 mm lens's 1666.7 pixels per unit of tangent, 3333.3 pixels
        // a meter there: its eyeball of radius 12 mm around the center, 40 pixels, behind a thin iris and lid 13 mm in front of
        // the center, 26.7 pixels in radius, a face 4 mm in front of the center around the eyeball, a hair strand 22 mm in front
        // over columns 640 and 641, where the nearest of 3x3 depths around the point lies, and bangs 50 mm in front over
        // columns 655 to 669. The window is 50 pixels wide, its 9x9 samples
        // 6.25 pixels apart: columns 615, 621, 627, 633, 640, 646, 652, 658 and 665.
        Scene eye(30);
        for (uint32_t y = 300; y < 500; y++)
            for (uint32_t x = 540; x < 740; x++)
            {
                const float dx = x + 0.5f - 640, dy = y + 0.5f - 400, rho = std::sqrt(dx * dx + dy * dy) / 3333.33f;
                float meters = rho < 0.012f ? 0.5f - std::sqrt(0.012f * 0.012f - rho * rho) : 0.496f;
                if (rho < 0.008f)
                    meters = 0.487f;
                if (x == 640 || x == 641)
                    meters = 0.478f;
                if (x >= 655 && x < 670)
                    meters = 0.45f;
                eye.put(x, y, 0.5f, meters);
            }
        DepthOfFieldSettings on = lens_at(2, 0, 0);
        on.eye_distance = 0.5f;
        const DofConstants c = *dof_constants(on, eye.camera, eye.image.size, true, 0, 0);
        check("an eye reckoned 0.5 m away through the 50 mm lens: the distance, and 15 mm across in 9 samples 6.25 pixels apart", c.eye[0] == 0.5f && near(c.eye[1], 6.25f));
        on.eye_distance = 0.495f;
        const float behind = render(eye, on).focus;
        on.eye_distance = 0.48f;
        check("reckoned 8 mm behind the iris or 7 mm in front, the autofocus takes the iris 13 mm in front of the center, past the strand that "
              "crosses a ninth of the window and the bangs beyond the band", near(behind, 0.487f) && near(render(eye, on).focus, 0.487f));
        on.eye_distance = 0.6f;
        check("with nothing from 2.5 cm in front of 0.6 m to 1.5 cm behind it, it takes 0.6 m", near(render(eye, on).focus, 0.6f));
        on.focus_distance = 2;
        check("a set distance still decides", render(eye, on).focus == 2);
        on.focus_distance = 0;
        on.eye_distance = 30;
        check("an eye 30 m away gets the least window, 4 pixels, and one 0.05 m away the largest, an eighth of the height: 100 pixels",
            near(dof_constants(on, eye.camera, eye.image.size, true, 0, 0)->eye[1], 0.5f)
                && (on.eye_distance = 0.05f, near(dof_constants(on, eye.camera, eye.image.size, true, 0, 0)->eye[1], 12.5f)));
        on.eye_distance = 0;
        check("without an eye the window is off", dof_constants(on, eye.camera, eye.image.size, true, 0, 0)->eye[0] == 0);
    }

    {
        // A glint of 50 over 2x2 pixels, one half-size texel, on black 30 m away, which the threshold takes as 2 x 0.25.
        const auto glint = [](float meters) {
            Scene s(30);
            for (uint32_t y = 400; y < 402; y++)
                for (uint32_t x = 640; x < 642; x++)
                    s.put(x, y, 50, meters);
            return s;
        };
        const Scene far = glint(30);
        DepthOfFieldSettings settings = lens_at(2, 2, 0);
        const DofOutput sprite = render(far, settings);
        settings.scatter = false;
        const DofOutput gathered = render(far, settings);
        check("a glint behind focus is one far sprite, and in front of focus one near sprite", sprite.sprites[0] == 1 && sprite.sprites[1] == 0
            && render(glint(1), lens_at(2, 2, 0)).sprites[1] == 1 && render(glint(1), lens_at(2, 2, 0)).sprites[0] == 0);
        check("a glint on the focus plane is none", render(glint(2), lens_at(2, 2, 0)).sprites[0] == 0 && render(glint(2), lens_at(2, 2, 0)).sprites[1] == 0);
        check("at gain 0 the sprite keeps the glint's light within 1%, as the gather does: 201.64 and 201.69 of 200",
            near(float(sum_green(sprite)), 201.639f, 0.01f) && near(float(sum_green(gathered)), 201.693f, 0.01f));
        // Radius 4.9858 half-size pixels, 9.97 full-size: 50 / (pi 4.9858^2) a pixel inside as R16F stores it, its edge smoothed
        // over two pixels.
        check("its disc is 0.6406 a pixel inside, 0.3810 and 0.2208 at 9 and 10 pixels out, 0.0736 at 11 and 0 at 13",
            near(far.green(sprite, 641, 401), 0.640625f) && near(far.green(sprite, 648, 401), 0.640625f) && near(far.green(sprite, 650, 401), 0.380981f)
                && near(far.green(sprite, 651, 401), 0.220825f) && near(far.green(sprite, 652, 401), 0.073608f) && far.green(sprite, 654, 401) == 0);
        check("at gain 2 the lone glint grows: 0.6807 a pixel inside its disc", near(far.green(render(far, lens_at2(2, 2, 0, 2)), 641, 401), 0.680664f));
    }
    {
        Scene square(30);
        for (uint32_t y = 300; y < 500; y++)
            for (uint32_t x = 540; x < 740; x++)
                square.put(x, y, 5, 30);
        const DofOutput out = render(square, lens_at2(2, 2, 0, 2));
        check("a flat bright square sends only the 16 texels of its corners, and keeps its light to 0.002%", out.sprites[0] == 16
            && std::fabs(sum_green(out) / sum_green(render(square, lens_at2(2, 2, 0, 0))) - 1) < 2e-5);
        Scene field(30);
        for (uint32_t y = 200; y < 600; y += 4)
            for (uint32_t x = 300; x < 900; x += 4)
                field.put(x, y, 5, 30);
        const DofOutput dense = render(field, lens_at2(2, 2, 0, 2));
        check("a dense field of glints two texels apart fills the frame's 3029 sprites and does not grow at gain 2", dense.sprites[0] == 3029
            && sum_green(dense) == sum_green(render(field, lens_at2(2, 2, 0, 0))));
        Scene lamp(30);
        for (uint32_t y = 398; y < 404; y++)
            for (uint32_t x = 638; x < 644; x++)
                lamp.put(x, y, 2, 30);
        const DofOutput grown = render(lamp, lens_at2(2, 2, 0, 2));
        check("a small lamp alone in the dark is 9 sprites, which at gain 2 grow its disc from 0.2306 to 0.5908 a pixel", grown.sprites[0] == 9
            && near(lamp.green(grown, 641, 401), 0.590820f) && near(lamp.green(render(lamp, lens_at2(2, 2, 0, 0)), 641, 401), 0.230591f));
    }

    // The display place.
    check("at the bloom step the scene place blurs, and the display place runs the autofocus, keeping the depth for the display "
          "unless the accumulator adds",
        dof_at_bloom(DofPlace::scene, AccumulateMode::add) == DofAtBloom::blur && dof_at_bloom(DofPlace::scene, AccumulateMode::off) == DofAtBloom::blur
            && dof_at_bloom(DofPlace::display, AccumulateMode::off) == DofAtBloom::defer
            && dof_at_bloom(DofPlace::display, AccumulateMode::present) == DofAtBloom::defer
            && dof_at_bloom(DofPlace::display, AccumulateMode::add) == DofAtBloom::focus);
    const DofEffectsPoint after_rtgi = dof_effects_point({ false, true, false, true, false });
    check("among five techniques where the second and fourth read the depth buffer, it runs after the fourth, or after the fifth when "
          "the fourth was skipped, and not after the first three",
        after_rtgi.after == std::optional<size_t>(3) && !dof_runs_after(after_rtgi, 0) && !dof_runs_after(after_rtgi, 2)
            && dof_runs_after(after_rtgi, 3) && dof_runs_after(after_rtgi, 4));
    const DofEffectsPoint none_read = dof_effects_point({ false, false });
    check("with no technique that reads the depth buffer, or none at all, it runs before the first", !none_read.after && !dof_effects_point({}).after
        && !dof_runs_after(none_read, 0) && !dof_runs_after(none_read, 1));
    check("8-bit and 10-bit back buffers are sRGB in either view, 16-bit float ones linear, and 32-bit float ones are not taken",
        dof_display_transfer(28) == DofTransfer::srgb && dof_display_transfer(29) == DofTransfer::srgb && dof_display_transfer(87) == DofTransfer::srgb
            && dof_display_transfer(91) == DofTransfer::srgb && dof_display_transfer(24) == DofTransfer::srgb
            && dof_display_transfer(10) == DofTransfer::linear && !dof_display_transfer(2));
    check("D32S8, D24S8, D32 and D16 and their views are depth formats; R32 and R16 typeless, R32F and RGBA8 are not", dof_depth_format(19)
        && dof_depth_format(20) && dof_depth_format(21) && dof_depth_format(45) && dof_depth_format(47) && dof_depth_format(40) && dof_depth_format(55)
        && !dof_depth_format(39) && !dof_depth_format(53) && !dof_depth_format(41) && !dof_depth_format(28));
    check("the sRGB curve takes 0.5 to 0.214041 and back, and 0.02 to 0.001548", near(dof_srgb_decode(0.5f), 0.214041f, 1e-6f)
        && near(dof_srgb_encode(0.214041f), 0.5f, 1e-6f) && near(dof_srgb_decode(0.02f), 0.00154799f, 1e-7f));
    {
        const Scene focused(2);
        const DepthOfFieldSettings shallow = lens_at(2, 2, 0);
        const std::optional<DofConstants> c = dof_display_constants(shallow, focused.camera, { 1280, 800 }, { 640, 400 }, true, DofTransfer::srgb, 3);
        check("the display's constants: the chain at the back buffer's 1280x800, upright, the autofocus's step 0, and the main depth's "
              "640x400 stored upside down with sRGB", c && c->counts[1] == 1280 && c->counts[2] == 800 && c->sampling[2] == -1 && c->focus[3] == 0
            && c->focus[2] == 0.5f && c->display[0] == 640 && c->display[1] == 400 && c->display[2] == 1 && c->display[3] == 1);
        // Every 8-bit level across the picture, all of it on the focus plane: the depth of field changes no pixel.
        std::vector<float> back(size_t(1280) * 800 * 3);
        for (size_t i = 0; i < back.size() / 3; i++)
            for (int k = 0; k < 3; k++)
                back[i * 3 + k] = float((i % 1280 + k * 85) % 256) / 255;
        const std::vector<float> on_plane(size_t(640) * 400, focused.reversed(2));
        const DofOutput sharp = render_dof_display(back, on_plane, *c, 0);
        uint32_t moved = 0;
        for (size_t i = 0; i < back.size(); i++)
            moved += std::lround(sharp.rgb[i] * 255) != std::lround(back[i] * 255) ? 1 : 0;
        check("a back buffer on the focus plane comes back level for level through the sRGB curve and R16F", moved == 0 && sharp.nonfinite == 0);
        // Stripes 4 pixels wide, black and white; the main depth's stored rows 200 to 399, the picture's top half, 30 m away.
        std::vector<float> depth = on_plane;
        for (size_t i = size_t(640) * 200; i < depth.size(); i++)
            depth[i] = focused.reversed(30);
        for (size_t i = 0; i < back.size() / 3; i++)
            for (int k = 0; k < 3; k++)
                back[i * 3 + k] = (i % 1280) / 4 % 2 == 0 ? 0.0f : 1.0f;
        const DofOutput split = render_dof_display(back, depth, *c, 0);
        const auto green = [&split](uint32_t x, uint32_t y) { return split.rgb[(size_t(y) * 1280 + x) * 3 + 1]; };
        check("the main depth comes upright: the far top's stripes blur to gray, 0.7205 and 0.7481 on a black and a white stripe "
              "where sRGB's middle gray is 0.7354, while the bottom's on the focus plane stay black and white",
            near(green(641, 100), 0.7205f, 0.002f) && near(green(645, 100), 0.7481f, 0.002f) && green(641, 700) == 0 && near(green(645, 700), 1, 1e-6f));
    }

    FrameSave frame;
    frame.size = { 3, 2 };
    frame.flip = true;
    frame.frame = 4242;
    frame.camera = camera;
    frame.hdr = { 1, 2, 3, 0xFFFFFFFFu, 5, 6 };
    frame.depth = { 0, 0.5f, 1, 0.25f, 0.125f, 0.0001f };
    const std::string tsv = frame_tsv(frame);
    check("frame.tsv holds the size, flip, frame, matrices and the near and far planes",
        tsv.find("width\t3\nheight\t2\nflip\t1\nframe\t4242\nworld_to_view\t1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1\nview_to_clip\t2,0,0,0,0,3.20000005,")
                == 0
            && tsv.find("\nnear\t0.099999994\nfar\t999.999939\n") != std::string::npos);
    if (argc > 1)
    {
        const std::filesystem::path folder = std::filesystem::path(argv[1]) / "frame-roundtrip";
        const std::vector<std::string> failed = write_frame_folder(folder, frame);
        std::string why;
        const std::optional<FrameSave> back = read_frame_folder(folder, why);
        check("a frame folder reads back as written", failed.empty() && back && back->size == frame.size && back->flip && back->frame == 4242
            && back->hdr == frame.hdr && back->depth == frame.depth && std::equal(camera.view_to_clip, camera.view_to_clip + 16, back->camera.view_to_clip));
        std::filesystem::remove(folder / "depth.r32");
        check("a frame folder without its depth reads as nothing, and says why", !read_frame_folder(folder, why)
            && why == "hdr.r11g11b10 or depth.r32 is missing or not 6 pixels");
    }

    std::printf("dof_test: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
