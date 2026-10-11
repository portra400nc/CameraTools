// The scene capture's files: each face's mesh as OBJ or binary PLY, its picture, depth and raw stream, the materials,
// and the TSV rows, written into one scene folder. Pure C++17 like compare.h. The add-on's writer thread and the tests'
// synthetic scene call the same functions.
#pragma once

#include "scene.h"

#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

namespace photoreal
{
    // What a mesh file got.
    struct MeshStats
    {
        uint64_t triangles = 0;     // written
        uint64_t vertices = 0;      // written, after merging the corners of one draw that have the same clip bits
        uint64_t dropped = 0;       // triangles with a corner at infinity
        uint64_t past_max = 0;      // triangles of the whole draws left out once max_triangles was reached
    };

    // The mesh convention of both writers: positions relative to origin, so the camera sits near the origin and viewers
    // keep their precision, and right-handed, Unity's (x, y, z) written as (x, y, -z) with the second and third corner of
    // each triangle swapped so its winding survives the mirror. Blender's default import (forward -Z, up Y) then shows the
    // scene upright and unmirrored. Corners of one draw with the same clip bits came from the same vertex shader
    // invocation, so they merge into one vertex.
    //
    // OBJ: "mtllib scene.mtl", then per replayed draw a "g d<draw>_<kind>[_s<stencil>]_vs<hash>[_ds<hash>]" and a "usemtl <class>"
    // line, its "v" lines with 3 decimals (millimeters) and its "f" lines with 1-based indices.
    MeshStats write_obj(std::ostream &out, const SceneCamera &camera, const Vec3d &origin, const std::vector<SceneDrawRecord> &records,
        const std::vector<float> &clip, uint64_t max_triangles);

    // Binary little-endian PLY: float x, y, z per vertex, and per triangle a uchar 3 and three int indices from 0. It holds
    // no draws or classes.
    MeshStats write_ply(std::ostream &out, const SceneCamera &camera, const Vec3d &origin, const std::vector<SceneDrawRecord> &records,
        const std::vector<float> &clip, uint64_t max_triangles);

    // One Kd per draw class, the stencil debug view's colors: the prepass red, 128 gray, 129 grass green, 133 magenta, 136
    // dark green, 137 light green, and 144, 165 and any other brown.
    std::string scene_mtl();

    // Half the size by a 2x2 box filter, rounding odd sizes down, at least 1x1. rgb holds size's pixels, 3 bytes each.
    std::vector<uint8_t> half_size(const std::vector<uint8_t> &rgb, Size size, Size *half);

    // Everything the writer gets for one face.
    struct SceneFaceData
    {
        uint32_t face = 0;                  // PHOTOREAL_FACE_*
        uint64_t frame = 0;                 // the add-on's frame count of the captured frame
        uint32_t attempts = 0;              // captures of this face it took
        SceneCamera camera;                 // what placed its geometry
        std::string camera_note;            // why the pushed camera placed it, empty for the game's
        std::optional<SunCbuffers> sun;     // the shadow-mask draw's b0 to b3 of that frame, when they were read
        std::optional<Camera> pushed;       // the camera CameraTools pushed for that frame, when there was one
        std::string status;                 // describe() of the captured frame
        Size render;                        // the main depth's size
        std::vector<SceneDrawRecord> records;
        std::vector<float> clip;            // 12 floats a triangle, in draw order
        std::vector<float> depth;           // render's reversed depth, top row first
        std::vector<uint8_t> rgb;           // the back buffer before ReShade's effects, 3 bytes a pixel, top row first
        Size picture;                       // its size
        bool overflowed = false;            // saved partial at the largest stream-output buffer
        Vec3d origin {};                    // the first saved face's camera position, which every mesh is relative to
    };

    // A scene folder being written, touched only by the writer.
    struct SceneFolder
    {
        std::filesystem::path path;
        std::vector<std::string> faces;     // scene.tsv's rows so far
        std::string draws;                  // draws.tsv's rows so far
        std::vector<std::string> cameras;   // camera.tsv's rows so far
    };

    // "face-0-pos-x.obj", "face-6-view.ply".
    std::string mesh_name(uint32_t face, bool ply);

    // The rows one face adds to scene.tsv, draws.tsv and camera.tsv, each with its line end.
    std::string scene_row(const SceneFaceData &data, const MeshStats &mesh, const DepthCheck &depth);
    std::string draw_rows(uint32_t face, const std::vector<SceneDrawRecord> &records);
    std::string camera_row(const SceneFaceData &data);
    extern const char *const kSceneHeader;
    extern const char *const kDrawsHeader;
    extern const char *const kCameraHeader;

    // Creates the folder and writes scene.mtl. Returns the names of the files that could not be written.
    std::vector<std::string> begin_scene_folder(const SceneFolder &folder);

    // Writes the face's mesh (PLY when request.ply), half-size picture, depth, raw stream when request.raw, and the
    // shadow-mask draw's b0 to b3 as they were read back when there were any (face-N-sun.bin, 2224 bytes), then
    // rewrites scene.tsv, draws.tsv and camera.tsv with its rows added. Returns the names of the files that could not be
    // written.
    std::vector<std::string> write_scene_face(SceneFolder &folder, const SceneFaceData &data, const SceneRequest &request);

    // stopped.txt: how many faces were saved, and why the capture stopped.
    std::vector<std::string> write_scene_stopped(const SceneFolder &folder, const std::string &note);
}
