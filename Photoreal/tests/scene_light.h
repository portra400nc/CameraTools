// The Mac tools' reading of a scene folder's light: its faces' cameras, depth and raw copies as core/light.h's FaceImages,
// the add-on's own PNGs decoded, and the stand-ins trace_reference.cpp and predict_shot.py use for captures made before
// PHOTOREAL_SCENE_LIGHT: the HDR scene from a face's picture through the game's tone curve inverted, and albedos by class.
// For tests/ only.
#pragma once

#include "../core/light.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace photoreal::scene_light
{
    inline std::string slurp(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    template <class T>
    bool read_raw(const std::filesystem::path &path, size_t count, std::vector<T> &out)
    {
        const std::string bytes = slurp(path);
        if (bytes.size() != count * sizeof(T))
            return false;
        out.resize(count);
        std::memcpy(out.data(), bytes.data(), bytes.size());
        return true;
    }

    // A PNG the add-on wrote (core/png.cpp): 8-bit RGB, stored deflate blocks, no filters. Empty for anything else.
    inline std::vector<uint8_t> decode_png(const std::string &png, Size &size)
    {
        const auto be32 = [&png](size_t at) {
            return uint32_t(uint8_t(png[at])) << 24 | uint32_t(uint8_t(png[at + 1])) << 16 | uint32_t(uint8_t(png[at + 2])) << 8 | uint8_t(png[at + 3]);
        };
        std::string zlib;
        for (size_t at = 8; at + 12 <= png.size();)
        {
            const uint32_t length = be32(at);
            const std::string type = png.substr(at + 4, 4);
            if (at + 12 + length > png.size())
                return {};
            if (type == "IHDR")
                size = { be32(at + 8), be32(at + 12) };
            else if (type == "IDAT")
                zlib += png.substr(at + 8, length);
            at += 12 + length;
        }
        std::string raw;
        for (size_t at = 2; at + 5 <= zlib.size();)
        {
            const bool last = zlib[at] & 1;
            const size_t length = uint8_t(zlib[at + 1]) | size_t(uint8_t(zlib[at + 2])) << 8;
            raw += zlib.substr(at + 5, length);
            at += 5 + length;
            if (last)
                break;
        }
        const size_t row = size_t(size.width) * 3;
        if (raw.size() != (row + 1) * size.height)
            return {};
        std::vector<uint8_t> rgb(row * size.height);
        for (uint32_t y = 0; y < size.height; y++)
            std::memcpy(&rgb[y * row], raw.data() + y * (row + 1) + 1, row);
        return rgb;
    }

    // The game's tone curve at the census's exposure of 0.985, inverted: the HDR value an 8-bit pixel of the back buffer
    // shows, before bloom, which the inverse cannot take out.
    inline float picture_hdr(uint8_t stored)
    {
        // The encode max(x^(1/2.4) * 1.055 - 0.055, 0), inverted, then x(1.36x + 0.047) / (x(0.93x + 0.56) + 0.14) = v,
        // the quadratic (1.36 - 0.93 v) x^2 + (0.047 - 0.56 v) x - 0.14 v = 0.
        const double v = std::pow((stored / 255.0 + 0.055) / 1.055, 2.4);
        const double a = 1.36 - 0.93 * v, b = 0.047 - 0.56 * v, c = -0.14 * v;
        const double x = a > 1e-6 ? (-b + std::sqrt(b * b - 4 * a * c)) / (2 * a) : 16.0;
        return static_cast<float>(std::min(x, 16.0) / 0.985);
    }

    // The census's mean linear albedos, as stand-ins where no G-buffer albedo was copied: capture 111824's world pixels for
    // the world and the rest, capture 214859's, mostly vegetation, for vegetation and foliage.
    inline Vec3f class_albedo(TraceClass c)
    {
        switch (c)
        {
        case TraceClass::vegetation: case TraceClass::foliage: return { 0.0918f, 0.1641f, 0.0652f };
        case TraceClass::character: return { 0.3f, 0.3f, 0.3f };
        default: return { 0.1755f, 0.1841f, 0.1856f };
        }
    }

    // The rows of a scene folder's TSV with a header, as maps of the header's names.
    inline std::vector<std::map<std::string, std::string>> tsv_rows(const std::filesystem::path &path)
    {
        std::ifstream in(path);
        std::vector<std::string> head;
        std::vector<std::map<std::string, std::string>> rows;
        for (std::string line; std::getline(in, line);)
        {
            std::vector<std::string> cells;
            std::stringstream text(line);
            for (std::string cell; std::getline(text, cell, '\t');)
                cells.push_back(cell);
            if (head.empty())
            {
                head = cells;
                continue;
            }
            std::map<std::string, std::string> row;
            for (size_t i = 0; i < head.size() && i < cells.size(); i++)
                row[head[i]] = cells[i];
            rows.push_back(row);
        }
        return rows;
    }

    struct SceneFaces
    {
        std::vector<FaceImages> faces;
        bool exact_cameras = false;     // from trace-light.bin, else camera.tsv's 9 digits
        bool raw = false;               // every face had its raw albedo, stencil and HDR copies
        bool pictures = false;          // a face's HDR stood in from its picture
        // The view face's G-buffer copies, for the ambient fit: normals, irradiance and stencil as stored, at its size.
        std::vector<uint8_t> view_normals, view_irradiance, view_stencil;
        std::vector<float> view_depth;
        Size view_size;
    };

    // Every saved face of a scene folder at the size the add-on baked it: its camera, depth, and its raw copies
    // (face-N-albedo.rgba8, face-N-stencil.r8, face-N-hdr.r11g11b10) when the capture wrote them. Without them, with
    // pictures, a face's HDR stands in from its picture, the back buffer at half size, by the nearest pixel. Faces of more
    // than kFullFacePixels are halved, as the add-on does.
    inline SceneFaces read_faces(const std::filesystem::path &folder, const std::optional<LightFile> &light, bool pictures)
    {
        SceneFaces out;
        std::map<uint32_t, Mat4d> exact;
        if (light)
            for (const FaceImages &f : light->faces)
                exact[f.face] = f.clip_to_world;
        out.exact_cameras = !exact.empty();
        out.raw = true;
        const auto cameras = tsv_rows(folder / "camera.tsv");
        for (const auto &row : tsv_rows(folder / "scene.tsv"))
        {
            const uint32_t face = static_cast<uint32_t>(std::stoul(row.at("face")));
            const Size size = { static_cast<uint32_t>(std::stoul(row.at("width"))), static_cast<uint32_t>(std::stoul(row.at("height"))) };
            SceneCamera camera;
            bool found = false;
            if (exact.count(face))
            {
                camera.clip_to_world = exact[face];
                found = true;
            }
            else
                for (const auto &c : cameras)
                    if (std::stoul(c.at("face")) == face)
                    {
                        std::stringstream text(c.at("clip_to_world"));
                        std::string item;
                        for (int k = 0; k < 16 && std::getline(text, item, ','); k++)
                            camera.clip_to_world[k] = std::strtod(item.c_str(), nullptr);
                        found = true;
                    }
            std::optional<FaceImages> images = found ? face_images(face, camera, size) : std::nullopt;
            if (!images)
                continue;
            FaceImages &f = *images;
            const size_t pixels = size_t(size.width) * size.height;
            const std::string prefix = "face-" + std::to_string(face);
            read_raw(folder / (prefix + "-depth.r32"), pixels, f.depth);
            const bool albedo = read_raw(folder / (prefix + "-albedo.rgba8"), pixels * 4, f.albedo);
            const bool stencil = read_raw(folder / (prefix + "-stencil.r8"), pixels, f.stencil);
            const bool hdr = read_raw(folder / (prefix + "-hdr.r11g11b10"), pixels, f.hdr);
            out.raw &= albedo && stencil && hdr;
            if (face == kFaceView)
            {
                out.view_size = size;
                out.view_depth = f.depth;
                read_raw(folder / (prefix + "-normals.r10g10b10a2"), pixels * 4, out.view_normals);
                read_raw(folder / (prefix + "-irradiance.r11g11b10"), pixels * 4, out.view_irradiance);
                out.view_stencil = f.stencil;
            }
            if (!hdr && pictures)
            {
                // The picture is the back buffer at half size, whatever the render size: each pixel takes the nearest.
                Size picture;
                const std::vector<uint8_t> rgb = decode_png(slurp(folder / (prefix + ".png")), picture);
                if (!rgb.empty() && f.depth.size() == pixels)
                {
                    f.hdr.resize(pixels);
                    for (uint32_t y = 0; y < size.height; y++)
                        for (uint32_t x = 0; x < size.width; x++)
                        {
                            const size_t from = (size_t(uint64_t(y) * picture.height / size.height) * picture.width + uint64_t(x) * picture.width / size.width) * 3;
                            f.hdr[size_t(y) * size.width + x] = hdr_stored({ picture_hdr(rgb[from]), picture_hdr(rgb[from + 1]), picture_hdr(rgb[from + 2]) });
                        }
                    out.pictures = true;
                }
            }
            if (pixels > kFullFacePixels)
                halve_face(f);
            out.faces.push_back(std::move(f));
        }
        return out;
    }
}
