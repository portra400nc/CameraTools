// Rain's drop layers and its shelter map's size, shared by the rain shaders and gpu.cpp, which draws one instance for each
// cell of every layer. Keep it to what both languages read alike.
//
// Layer i is a cube of kRainCells[i] cells a side, anchored to the world around the camera, whose drops are drawn out to
// kRainReach[i] meters from the camera along any axis. Its cells are kRainReach[i] / (kRainCells[i] / 2 - 1) meters across:
// 0.22, 0.45, 1.09 and 2.8. Farther layers hold fewer drops per cubic meter, where each one is a pixel or less across.
// 20^3 + 24^3 + 24^3 + 22^3 = 46296 instances.
static const int kRainLayers = 4;
static const float kRainReach[kRainLayers] = { 2, 5, 12, 28 };
static const int kRainCells[kRainLayers] = { 20, 24, 24, 22 };

// The cells a side of the shelter map, which shaders/rain_shelter_cs.hlsl writes: core/sky.h's kShelterCells, which gpu.cpp
// holds it to.
static const int kShelterSide = 256;
