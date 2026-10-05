// The C interface of CameraToolsReShadeBridge.addon64, a ReShade add-on that lets CameraTools drive ReShade without
// ReShade's C++ API. ReShade loads the add-on from its base folder, or from the folder AddonPath names. CameraTools finds
// the loaded module by name and calls these exports.
//
// Every function may be called from any thread and returns at once. Changes are queued and applied on ReShade's
// render thread during the next present, so a mod watches BridgeStatus to see them take effect.
#pragma once

#include <stdint.h>

#define BRIDGE_VERSION 4

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BridgeStatus
{
    // 1 while ReShade has an effect runtime, which it destroys and creates again when the swap chain is resized.
    uint32_t runtime;
    // ReShade's global effects switch, as of the last present.
    uint32_t effects_enabled;
    // 1 while the runtime lists at least one technique, which it does not while it loads or compiles effects. It stays
    // 0 for a ReShade with no effect files. After a resize it can still read 1 until ReShade destroys the old runtime,
    // so a caller that changed the window size waits for width and height to match first.
    uint32_t effects_ready;
    // The runtime's back buffer size, which is the size of a screenshot.
    uint32_t width;
    uint32_t height;
    // Frames the runtime has presented since the add-on loaded. It never resets.
    uint64_t presents;
    // Screenshots ReShade has saved since the add-on loaded.
    uint64_t screenshots;
} BridgeStatus;

__declspec(dllexport) uint32_t BridgeVersion(void);

__declspec(dllexport) void BridgeGetStatus(BridgeStatus *status);

__declspec(dllexport) void BridgeSetEffects(uint32_t enabled);

// Saves a screenshot through ReShade's own screenshot function on the next present. On D3D11 with effects rendering
// it is taken after the effects and before ReShade's overlay; otherwise it is taken after the overlay.
// BridgeStatus.screenshots rises once the file is on disk. A failed save raises nothing, so a caller needs a timeout.
// ReShade's default file name has no counter for these shots, so two in the same second overwrite each other.
__declspec(dllexport) void BridgeSaveScreenshot(void);

// Copies the UTF-8 path of the last saved screenshot, with its terminator, and returns its length without the
// terminator. Returns 0 and writes an empty string while there is none or the buffer is too small.
__declspec(dllexport) uint32_t BridgeGetLastScreenshot(char *path, uint32_t size);

// Uniforms and techniques are named by their effect file, such as "MartysMods_DEPTHOFFIELD.fx", and their name in it.
//
// A set is kept until the runtime has the effect loaded and then applied once, so a set made while effects compile is
// not lost. The latest set of one uniform or technique replaces an earlier one that has not been applied.
// values holds count components, at most 4.
__declspec(dllexport) void BridgeSetFloat(const char *effect, const char *name, const float *values, uint32_t count);
__declspec(dllexport) void BridgeSetInt(const char *effect, const char *name, const int32_t *values, uint32_t count);
__declspec(dllexport) void BridgeSetTechnique(const char *effect, const char *technique, uint32_t enabled);

// A get reads a copy that the render thread refreshes every present for each uniform or technique that was asked for
// before. It returns 0 and writes nothing while there is no copy: on the first call, while effects load, and for a
// name the loaded effects do not have. A value set through the bridge reads back after the present that applied it.
// A uniform get returns the number of components it wrote. Enabling a technique of an effect that was not rendering
// makes ReShade create that effect, and gets return 0 for those frames too. While BridgeSetOverride holds a technique,
// its get reads the state the release will put back, not the override's.
__declspec(dllexport) uint32_t BridgeGetFloat(const char *effect, const char *name, float *values, uint32_t count);
__declspec(dllexport) uint32_t BridgeGetInt(const char *effect, const char *name, int32_t *values, uint32_t count);
__declspec(dllexport) uint32_t BridgeGetTechnique(const char *effect, const char *technique, uint32_t *enabled);

// Copies the value of a preprocessor definition as the effect file sees it, with its terminator, such as
// RESHADE_DEPTH_LINEARIZATION_FAR_PLANE for "MartysMods_RELIGHT.fx". It returns the bytes written with the terminator,
// so 1 for a name the effect has no definition of, and 0 while there is no copy, as a get does. A value that does not
// fit with its terminator is cut.
__declspec(dllexport) uint32_t BridgeGetDefinition(const char *effect, const char *name, char *value, uint32_t size);

// Keeps every technique of the effect files in forced_effects, separated by newlines, switched on, and with solo every
// other technique switched off, whatever the preset or a reload says. A later call replaces the override; an empty list
// with solo 0 releases it and puts each technique the override changed back as it was. A technique set through the
// bridge meanwhile is what the release puts back. A preset save writes the states from before the override.
__declspec(dllexport) void BridgeSetOverride(const char *forced_effects, uint32_t solo);

// Writes the technique states and uniform values to ReShade's current preset file, once effects are loaded and the
// pending sets have been applied. ReShade saves the uniforms only of effects with an enabled technique, and the
// file can land about a second later. Values set through the bridge are otherwise lost when effects reload, which
// a resize causes.
__declspec(dllexport) void BridgeSavePreset(void);

#ifdef __cplusplus
}
#endif
