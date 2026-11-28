// The DNG CameraTools saves beside a screenshot: the add-on's linear light before the tone curve, as a raw photo editors
// open with Genshin's own curve as its profile, and its way from EnhancementsDevelopDng to the file. Pure C++17 like plan.h.
// The GPU side is in gpu.cpp.
#pragma once

#include "plan.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace enhancements
{
    // Linear RGB floats, channels a pixel (3, or 4 with a fourth the file leaves out), width by height, rows as stored.
    struct DngImage
    {
        const float *pixels = nullptr;
        uint32_t width = 0, height = 0;
        uint32_t channels = 3;
        bool rows_up = false;   // stored rows run up the screen, as the game's targets do; the file's always run down
    };

    // The file's 16 bits hold 0 to H, the smallest power of two at or above the image's largest channel, from 1 to 16: dark
    // shots keep their precision and a lamp up to 16 keeps its color. Values above 16 clip. Not finite reads as 1.
    float dng_headroom(float largest);

    // The profile's tone curve: the game's filmic curve x(1.36x + 0.047) / (x(0.93x + 0.56) + 0.14) of headroom times the
    // file's value, up to 1 at the game's white, about 1.422, 257 points, linear in and out, at x = (i / 256)^2 so the
    // shadows get most of them. The file's 1 is the headroom, not the game's white, so with no BaselineExposure no light
    // reaches Adobe's raw processing above 1, where its exposure ramp clips each ProPhoto channel and paled the sky.
    std::vector<std::pair<float, float>> dng_tone_curve(float headroom);

    // The profile's look table, which Adobe's raw processing applies in ProPhoto linear HSV before the tone curve, which it
    // applies to the largest and smallest channel and the middle one at its share (the DNG SDK's RefBaselineRGBTone): each
    // node holds the hue shift in degrees, the saturation scale and the value scale that take its color to the one that
    // curve turns into the game's, which applies the curve to each Rec. 709 channel; without it the sky went lavender and
    // pale. The value scale applies to the sRGB-encoded value, as Adobe applies it. kDngLookHues x kDngLookSats x
    // kDngLookVals nodes, value outermost, then hue, saturation innermost, as ProfileLookTableData stores them, the value
    // divisions sRGB-encoded.
    constexpr uint32_t kDngLookHues = 36, kDngLookSats = 16, kDngLookVals = 32;
    std::vector<float> dng_look_table(float headroom);

    // The camera the shot was taken with, for the file's EXIF. A value of 0, or an empty date, leaves its tag out.
    struct DngExif
    {
        float focal_length_mm = 0;      // full-frame focal length, also written as FocalLengthIn35mmFilm
        float f_number = 0;
        float subject_distance_m = 0;   // the focus distance
        std::string date_time;          // DateTimeOriginal, "YYYY:MM:DD HH:MM:SS"
        float exposure_time_s = 0;      // ExposureTime: 1/round(1/t) below 1 s, round(t)/1 from 1 s on
        uint32_t iso = 0;               // ISOSpeedRatings, at most 65535
    };

    // The side of the square tiles the pixels are compressed in.
    constexpr uint32_t kDngTile = 256;

    // The preview's JPEG quality, and the most pixels across or down of the thumbnail.
    constexpr int kDngPreviewQuality = 90;
    constexpr uint32_t kDngThumbnail = 256;

    // The pictures a DNG carries for viewers that do not develop raw images, as a camera's JPEG: samples, three a pixel
    // top row first, each scene_per_sample times the light the game's filmic curve takes, shown as the game shows them
    // through that curve and the sRGB curve. The preview is a baseline JPEG at kDngPreviewQuality, the thumbnail 8-bit RGB
    // averaged into at most kDngThumbnail pixels across and down.
    struct DngThumbnail
    {
        std::vector<uint8_t> pixels;
        uint32_t width = 0, height = 0;
    };
    struct DngPreview
    {
        std::vector<uint8_t> jpeg;
        DngThumbnail thumbnail;
    };
    DngPreview dng_preview(const uint16_t *samples, uint32_t width, uint32_t height, double scene_per_sample);

    // The whole file, a little-endian TIFF laid out as Adobe's DNG Converter lays one out. IFD0 is the thumbnail, 8-bit RGB
    // at most kDngThumbnail pixels across, and holds BaselineExposure 0, Rec. 709 primaries with D65 white as shot, the
    // embedded profile "CameraTools Game" with dng_tone_curve and dng_look_table, and the EXIF IFD of exif. Its SubIFDs are
    // the raw image, LinearRaw with three 16-bit samples a pixel and white at 65535 in kDngTile tiles, each a lossless JPEG
    // (ITU T.81 process 14, predictor 1, its own optimal Huffman table), edge tiles padded with their last row and column;
    // then the preview, dng_preview's JPEG at full size, which Finder, Photos and Google Drive show where they would
    // otherwise develop the raw image with their own contrast.
    // Non-finite and negative channels store 0. Lightroom opened the raw image's layout on 2026-10-29 with the profile
    // selected and the pixels of the uncompressed file of 2026-10-28.
    std::vector<uint8_t> encode_dng(const DngImage &image, const DngExif &exif = {});

    // encode_dng into a file. false when it could not be written.
    bool write_dng(const std::filesystem::path &path, const DngImage &image, const DngExif &exif = {});

    // One tile of kDngTile x kDngTile pixels, three samples each, as its lossless JPEG stream; exposed for the tests'
    // decoder.
    std::vector<uint8_t> dng_lossless_jpeg(const uint16_t *samples, uint32_t width, uint32_t height);

    // The tone map's weights the DNG takes: bloom_strength, 2^exposure_ev and the auto exposure's metered stops while our
    // tonemap pass draws the frame, and 1, 1 and none while the game's own tone map does.
    struct DngGrade
    {
        float bloom_strength = 1;
        float exposure_scale = 1;
        bool auto_exposure = false;     // the auto exposure's 1x1 target multiplies in by 2 to its power
    };
    DngGrade dng_grade(const TonemapSettings &settings, bool ours);

    // ENHANCEMENTS_DNG_* and ENHANCEMENTS_DNG_FAILED_*.
    enum class DngState : uint32_t { none, developing, ready, writing, written, failed };
    enum class DngFailure : uint32_t { none, no_tonemap, develop, readback, write, dropped };

    // The DNG asked for last, which the exports, the render thread and the worker thread share under one lock. since is the
    // present it entered its state at.
    struct DngJob
    {
        uint32_t id = 0;
        DngState state = DngState::none;
        DngFailure failure = DngFailure::none;
        uint64_t since = 0;
    };

    // Presents a DNG may wait for its tone map draw and read back, and that a developed image is held for its write.
    constexpr uint64_t kDngDevelopWait = 300;
    constexpr uint64_t kDngHold = 1800;

    // EnhancementsDevelopDng: a new id begins developing, unless id is 0 or a DNG is developing or being written.
    bool dng_develop(DngJob &job, uint32_t id, uint64_t presents);

    // The render thread's read back of id's image: failure none makes it ready, else it fails. A job that moved on since
    // the image was copied keeps its state. true when the image is to be held.
    bool dng_developed(DngJob &job, uint32_t id, DngFailure failure, uint64_t presents);

    // EnhancementsWriteDng: id's held image goes to the worker, when it is the one ready.
    bool dng_write(DngJob &job, uint32_t id);

    // The worker's result for id.
    void dng_written(DngJob &job, uint32_t id, bool ok);

    // At each present: a DNG developing for longer than kDngDevelopWait fails with no_tonemap, and one ready for longer than
    // kDngHold is dropped. true when it gave up on one now, whose held image is then freed.
    bool dng_expire(DngJob &job, uint64_t presents);
}
