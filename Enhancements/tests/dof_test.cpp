// The depth of field's arithmetic, shaders/dof.hlsli compiled as C++: the circle of confusion at known lens values, each
// channel's under axial chromatic aberration, the aperture's shape at known points, and the light across the disc.
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

    // A 1280x800 frame through a 50 mm lens (P11 = 50 / 12) at 16:10, everything at background meters, black, rows as stored.
    struct Scene
    {
        Camera camera = reversed_camera(50.0f / 12 / 1.6f, 50.0f / 12);
        DofImage image;
        std::array<float, 4> k {};
        explicit Scene(float background) : k(*dof_depth_coefficients(camera))
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
        return s;
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

    // Red focuses 4% and blue 2% farther than green at chromatic 1.
    const std::array<float, 3> behind = dof_channel_cocs(30, 2, lens50(1));
    check("behind focus with chromatic 1, red's CoC is 4.7756, green's 4.9858 and blue's 4.8786",
        near(behind[0], 4.77559f) && near(behind[1], 4.98575f) && near(behind[2], 4.87856f));
    check("behind focus green's disc is the largest and red's the smallest, so bokeh fringes green",
        behind[1] > behind[2] && behind[2] > behind[0]);
    const std::array<float, 3> front = dof_channel_cocs(1, 2, lens50(1));
    check("in front of focus with chromatic 1, red's CoC is -5.5419, green's -5.3419 and blue's -5.4439",
        near(front[0], -5.54187f) && near(front[1], -5.34188f) && near(front[2], -5.44389f));
    check("in front of focus red's and blue's discs are larger than green's, so bokeh fringes magenta",
        std::fabs(front[0]) > std::fabs(front[2]) && std::fabs(front[2]) > std::fabs(front[1]));

    check("the light across the disc averages 1 at spherical aberration 1 behind focus (soap bubble)", std::fabs(disc_mean(1) - 1) < 1e-4);
    check("and at 1 in front of focus, where the clamp at 0 cuts the rim", std::fabs(disc_mean(-1) - 1) < 1e-4);
    check("and at 0.6, -0.6, -0.3 and 0", std::fabs(disc_mean(0.6f) - 1) < 1e-4 && std::fabs(disc_mean(-0.6f) - 1) < 1e-4
        && std::fabs(disc_mean(-0.3f) - 1) < 1e-4 && std::fabs(disc_mean(0) - 1) < 1e-4);
    check("at 1 behind focus the disc's center is dark and its rim 3 times the mean",
        dof_bokeh_intensity(0, 1) == 0 && near(dof_bokeh_intensity(1, 1), 3) && near(dof_bokeh_intensity(0.5f, 1), 0.1875f));
    check("at 1 in front of focus the center is 1.837 and the light is 0 past rho 0.904",
        near(dof_bokeh_intensity(0, -1), 2 * 0.918559f) && near(dof_bokeh_intensity(0.5f, -1), 1.66489f) && dof_bokeh_intensity(0.91f, -1) == 0);
    check("the clamp's normalization is 0.91856 at -1 and 1 from -0.5 up", near(dof_bokeh_norm(-1), 0.918559f) && dof_bokeh_norm(-0.5f) == 1
        && dof_bokeh_norm(1) == 1);

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
    settings.focus_speed = 0.9f;
    settings.blades = 7;
    settings.blade_roundness = 0;
    settings.blade_rotation = 90;
    settings.spherical_aberration = -1;
    settings.chromatic = 1;
    const Camera lens = reversed_camera(50.0f / 12 / 1.6f, 50.0f / 12);
    const DofConstants c = *dof_constants(settings, lens, { 1280, 800 }, true, 0.5f, 1);
    check("the constants: a 50 mm focal length from the projection and the 24 mm sensor, f/2.8, 16.667 half-size pixels a mm, a largest "
          "radius of 12",
        near(c.optics[0], 50) && near(c.optics[1], 2.8f) && near(c.optics[2], 16.6667f) && near(c.optics[3], 12));
    check("the focus point stored upside down, and the autofocus 1 - 0.1^0.5 of the way after half a second",
        c.focus[0] == 0 && c.focus[1] == 0.5f && c.focus[2] == 0.75f && near(c.focus[3], 0.683772f));
    check("seven straight blades turned 90 degrees, grown by 1.0715 and the margin with chromatic 1 by 5% more",
        c.shape[0] == 7 && c.shape[1] == 0 && near(c.shape[2], 1.570796f) && near(c.shape[3], 0.933288f) && near(c.bokeh[3], 1.07148f * 1.05f));
    check("spherical aberration -1 normalized in front of focus, where it is +1, and behind", c.bokeh[0] == -1 && near(c.bokeh[1], 0.918559f) && c.bokeh[2] == 1);
    check("49 samples are 3 rings, frame 1 turns them 0.618, flip makes stored rows run up, and the aspect is 1.6",
        c.sampling[0] == 3 && near(c.sampling[1], 0.618034f) && c.sampling[2] == 1 && c.sampling[3] == 1.6f);
    check("the half size 640x400 in 80x50 tiles, a reach of 2 tiles, and depth samples 8 pixels apart", c.sizes[0] == 640 && c.sizes[1] == 400
        && c.sizes[2] == 80 && c.sizes[3] == 50 && c.counts[0] == 2 && c.counts[1] == 1280 && c.counts[2] == 800 && c.counts[3] == 8);
    check("rings: 9 samples are 2, 64 are 4, 289 are 8 and 1089 are 16", dof_rings(9) == 2 && dof_rings(64) == 4 && dof_rings(289) == 8 && dof_rings(1089) == 16);
    settings.focus_speed = 0;
    check("a focus speed of 0 jumps", dof_constants(settings, lens, { 1280, 800 }, false, 0.5f, 0)->focus[3] == 1
        && dof_constants(settings, lens, { 1280, 800 }, false, 0.5f, 0)->focus[2] == 0.25f);
    check("a projection without a focal length has no constants", !dof_constants(settings, Camera {}, { 1280, 800 }, true, 0, 0));

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
        check("a point 30 m away with the lens focused at 2 m keeps its light within 3% as it spreads into a disc", std::fabs(after / before - 1) < 0.03
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
