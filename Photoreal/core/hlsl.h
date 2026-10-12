// Just enough of HLSL in C++ to compile the trace kernel's shared files, shaders/bvh.hlsli, shaders/alpha.hlsli and
// shaders/trace.hlsli, so
// the tests and the Mac's reference tracer run the compute shader's own arithmetic in float. Only core/kernel.cpp
// includes it. The shared files keep to what this supports: vectors with x, y, z and w members but no swizzles, no out
// parameters, f-suffixed float literals, and LOOP and UNROLL for HLSL's [loop] and [unroll].
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace photoreal::shader
{
    using uint = uint32_t;

    struct float2
    {
        float x = 0, y = 0;
        float2() = default;
        float2(float a, float b) : x(a), y(b) {}
    };

    struct float3
    {
        float x = 0, y = 0, z = 0;
        float3() = default;
        float3(float a, float b, float c) : x(a), y(b), z(c) {}
    };

    struct float4
    {
        float x = 0, y = 0, z = 0, w = 0;
        float4() = default;
        float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
    };

    struct int2
    {
        int x = 0, y = 0;
        int2() = default;
        int2(int a, int b) : x(a), y(b) {}
    };

    struct uint2
    {
        uint x = 0, y = 0;
        uint2() = default;
        uint2(uint a, uint b) : x(a), y(b) {}
    };

    struct uint4
    {
        uint x = 0, y = 0, z = 0, w = 0;
        uint4() = default;
        uint4(uint a, uint b, uint c, uint d) : x(a), y(b), z(c), w(d) {}
    };

    inline float3 operator+(float3 a, float3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline float3 operator-(float3 a, float3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline float3 operator*(float3 a, float3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
    inline float3 operator*(float3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
    inline float3 operator*(float s, float3 a) { return { s * a.x, s * a.y, s * a.z }; }
    inline float3 operator/(float3 a, float s) { return { a.x / s, a.y / s, a.z / s }; }
    inline float3 operator-(float3 a) { return { -a.x, -a.y, -a.z }; }
    inline float2 operator*(float2 a, float s) { return { a.x * s, a.y * s }; }

    inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline float dot(float4 a, float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
    inline float3 cross(float3 a, float3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    inline float sqrt(float x) { return std::sqrt(x); }
    inline float rsqrt(float x) { return 1.0f / std::sqrt(x); }
    inline float length(float3 a) { return std::sqrt(dot(a, a)); }
    inline float3 normalize(float3 a) { return a * rsqrt(dot(a, a)); }
    inline float abs(float x) { return std::fabs(x); }
    // HLSL's min and max of floats return the other operand when one is NaN, as fmin and fmax do.
    inline float min(float a, float b) { return std::fmin(a, b); }
    inline float max(float a, float b) { return std::fmax(a, b); }
    inline uint min(uint a, uint b) { return a < b ? a : b; }
    inline uint max(uint a, uint b) { return a > b ? a : b; }
    inline int min(int a, int b) { return a < b ? a : b; }
    inline int max(int a, int b) { return a > b ? a : b; }
    inline float saturate(float x) { return std::fmin(std::fmax(x, 0.0f), 1.0f); }
    inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
    inline float sin(float x) { return std::sin(x); }
    inline float cos(float x) { return std::cos(x); }
    inline float floor(float x) { return std::floor(x); }
    inline float ceil(float x) { return std::ceil(x); }
    inline float trunc(float x) { return std::trunc(x); }
    // HLSL's round rounds halves to even, as round_ne does: the default rounding mode's nearbyint.
    inline float round(float x) { return std::nearbyint(x); }
    inline float exp2(float x) { return std::exp2(x); }
    inline float log2(float x) { return std::log2(x); }
    inline float asfloat(uint u)
    {
        float f;
        std::memcpy(&f, &u, sizeof f);
        return f;
    }
    inline uint asuint(float f)
    {
        uint u;
        std::memcpy(&u, &f, sizeof u);
        return u;
    }
    inline uint asuint(int i) { return static_cast<uint>(i); }
    inline int asint(uint u) { return static_cast<int>(u); }
}
