// A shader's output signature, read from its DXBC container, and the stream-output declaration that captures the
// position it writes. Pure C++17 like recipe.h. The scene capture builds a geometry shader with stream output from the
// game's own vertex or domain shader bytecode with it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace enhancements
{
    // The program type in the SHEX or SHDR chunk's version token, in its order.
    enum class ShaderStageKind : uint8_t { ps, vs, gs, hs, ds, cs };

    struct SignatureElement
    {
        std::string semantic;       // as the compiler wrote it, such as "SV_POSITION"
        uint32_t index = 0;         // the semantic index
        uint32_t system_value = 0;  // D3D_NAME: 1 for SV_Position, 0 for none
        uint32_t component_type = 0;    // D3D_REGISTER_COMPONENT_TYPE: 1 uint32, 2 sint32, 3 float32
        uint32_t reg = 0;           // the register; elements may share one, each in its own components
        uint8_t mask = 0;           // the components the element takes, x = 1 to w = 8
        uint8_t stream = 0;         // the geometry shader's stream, 0 for every other stage
    };

    struct OutputSignature
    {
        ShaderStageKind stage = ShaderStageKind::vs;
        std::vector<SignatureElement> elements;
    };

    // The container's chunk table, the SHEX or SHDR chunk's stage, and its OSGN (24-byte elements), OSG1 (32-byte, with
    // minimum precision) or OSG5 (28-byte, a geometry shader's with streams) chunk. Every offset is checked against
    // size, and any that falls outside it, a bad magic, or a missing chunk gives empty.
    std::optional<OutputSignature> parse_output_signature(const uint8_t *code, size_t size);

    // The same for the container's ISGN (24-byte elements) or ISG1 (32-byte) chunk: the inputs a pixel shader reads.
    std::optional<std::vector<SignatureElement>> parse_input_elements(const uint8_t *code, size_t size);

    // The tessellator domain a hull or domain shader declares (dcl_tessellator_domain in its SHEX or SHDR chunk). Empty
    // when the container is broken or declares none, as for every other stage.
    enum class TessellatorDomain : uint8_t { isoline = 1, tri = 2, quad = 3 };
    std::optional<TessellatorDomain> tessellator_domain(const uint8_t *code, size_t size);

    // Surface detail's pixel shaders read a draw's world position from the vertex shader's TEXCOORD3 to TEXCOORD5 .w, the
    // rows of Unity's tangent-to-world matrix with the position after them. A pixel shader links to a vertex shader only
    // when its inputs sit in the same registers, so each layout has its own: 1 for SV_POSITION, TEXCOORD0, COLOR0 and
    // TEXCOORD1 to TEXCOORD5 in registers 0 to 7, 2 for the same without TEXCOORD2, in 0 to 6. 3 for the near terrain's:
    // SV_POSITION, COLOR0 and TEXCOORD0 to TEXCOORD3 in registers 0 to 5, TEXCOORD3.xyz the vector from the point to the
    // camera, whose position the vertex shader's cb1 row 5 holds. 4 for the near terrain drawn by the game's own terrain
    // system: SV_POSITION and TEXCOORD0 to TEXCOORD3 in registers 0 to 4, the world position in TEXCOORD1 to TEXCOORD3 .w.
    // 0 for any other.
    int world_position_layout(const OutputSignature &signature);
    // The same for a pixel shader's inputs, which name the layout of whatever stage feeds it, such as a domain shader
    // after tessellation.
    int world_position_layout(const std::vector<SignatureElement> &inputs);

    // The game's sky dome vertex shader writes the direction from the camera to each vertex, normalized, in TEXCOORD8.xyz:
    // true when the signature is SV_POSITION and TEXCOORD0 to TEXCOORD8 in registers 0 to 9, as sky_dome_view_ps.hlsl
    // reads them.
    bool sky_direction_layout(const OutputSignature &signature);

    // One D3D11_SO_DECLARATION_ENTRY, without the stream, which is always 0.
    struct SoEntry
    {
        std::string semantic;
        uint32_t index = 0;
        uint8_t start = 0, count = 0, slot = 0;
    };

    struct SoDeclaration
    {
        std::vector<SoEntry> entries;
        uint32_t stride = 0;        // bytes per vertex in slot 0
    };

    // The element whose system value is SV_Position, by its own semantic name and index, components 0 to 3 in slot 0, 16
    // bytes a vertex. The match is by system value, never by register. Empty when there is none, as for a vertex shader
    // that feeds a hull shader.
    std::optional<SoDeclaration> so_declaration(const OutputSignature &signature);

    // The position's declaration followed by extra, each after the one before in slot 0, the stride their sum: what the
    // scene capture streams for a draw whose pixel shader has an alpha program (core/alpha.h). Each extra entry must name an
    // element of the signature, by semantic (case-insensitive, as D3D11 links stages) and index, whose components cover
    // start to start + count - 1 counted from the element's first, as DXVK reads an entry. Otherwise empty, with missing
    // naming the entry, such as "TEXCOORD4.w": DXVK skips an entry it cannot find, and its stride would hold garbage.
    std::optional<SoDeclaration> so_declaration(const OutputSignature &signature, const std::vector<SoEntry> &extra, std::string &missing);

    // The container's checksum, the 16 bytes at 4: MD5 over everything after them, with DXBC's own last block (the length
    // in bits first, and the length over 4 with its low bit set last). DXVK refuses a shader whose checksum is wrong.
    std::array<uint8_t, 16> dxbc_checksum(const uint8_t *code, size_t size);

    // A copy of the container with one more chunk, "CTSL" holding salt, and its checksum made again: the same shader in
    // other bytes. Empty when the bytes are not a container.
    std::string salted_container(const std::string &code, uint64_t salt);

    // extra, entries of a domain shader's signature, as shaders/stream_attributes_gs.hlsl's outputs: each element's register
    // as ATTR<register>, its components counted from x. Empty when an entry names no element, or one outside registers 1 to 7.
    std::optional<std::vector<SoEntry>> attribute_entries(const OutputSignature &domain, const std::vector<SoEntry> &extra);
}
