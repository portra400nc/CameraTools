// A shader's output signature, read from its DXBC container, and the stream-output declaration that captures the
// position it writes. Pure C++17 like recipe.h. The scene capture builds a geometry shader with stream output from the
// game's own vertex or domain shader bytecode with it.
#pragma once

#include <cstddef>
#include <cstdint>
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
}
