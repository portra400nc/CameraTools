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
