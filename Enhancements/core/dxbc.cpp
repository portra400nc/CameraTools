#include "dxbc.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace enhancements
{
    namespace
    {
        // The container: "DXBC", a 16-byte checksum, a version, the total size at 24, the chunk count at 28, then each
        // chunk's offset. A chunk is its four-character code, its size, and its body.
        constexpr size_t kHeader = 32;
        // A signature chunk's body: the element count, the elements' offset (always 8), then the elements, whose names
        // are offsets into the body.
        constexpr size_t kSignatureHeader = 8;

        bool read32(const uint8_t *code, size_t size, size_t at, uint32_t &out)
        {
            if (at > size || size - at < 4)
                return false;
            std::memcpy(&out, code + at, 4);
            return true;
        }

        struct Chunk
        {
            size_t body = 0, size = 0;
        };

        // Where an OSGN-family chunk's fields sit in one element: 24-byte OSGN elements have no stream, and OSG1 and
        // OSG5 elements start with one.
        struct Layout
        {
            size_t stride;
            bool stream;
        };
    }

    namespace
    {
        // The stage and the chosen signature chunk's elements: the output's OSGN, OSG1 or OSG5, or the input's ISGN or ISG1.
        std::optional<OutputSignature> parse_signature(const uint8_t *code, size_t size, bool input)
        {
            uint32_t total = 0, count = 0;
            if (code == nullptr || size < kHeader || std::memcmp(code, "DXBC", 4) != 0 || !read32(code, size, 24, total) || total > size
                || total < kHeader || !read32(code, size, 28, count) || count > (total - kHeader) / 4)
                return std::nullopt;
            size = total;

            std::optional<ShaderStageKind> stage;
            std::optional<Chunk> signature;
            Layout layout { 24, false };
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t offset = 0, chunk_size = 0;
                if (!read32(code, size, kHeader + 4 * i, offset) || !read32(code, size, size_t(offset) + 4, chunk_size)
                    || size_t(offset) + 8 + chunk_size > size)
                    return std::nullopt;
                const char *fourcc = reinterpret_cast<const char *>(code + offset);
                const Chunk chunk { size_t(offset) + 8, chunk_size };
                if (std::memcmp(fourcc, "SHEX", 4) == 0 || std::memcmp(fourcc, "SHDR", 4) == 0)
                {
                    uint32_t version = 0;
                    if (!read32(code, chunk.body + chunk.size, chunk.body, version) || version >> 16 > static_cast<uint32_t>(ShaderStageKind::cs))
                        return std::nullopt;
                    stage = static_cast<ShaderStageKind>(version >> 16);
                }
                else if (std::memcmp(fourcc, input ? "ISGN" : "OSGN", 4) == 0)
                    signature = chunk, layout = { 24, false };
                else if (std::memcmp(fourcc, input ? "ISG1" : "OSG1", 4) == 0)
                    signature = chunk, layout = { 32, true };
                else if (!input && std::memcmp(fourcc, "OSG5", 4) == 0)
                    signature = chunk, layout = { 28, true };
            }
            if (!stage || !signature)
                return std::nullopt;

            // From here on, offsets are within the signature chunk's body.
            const uint8_t *body = code + signature->body;
            const size_t body_size = signature->size;
            uint32_t elements = 0;
            if (body_size < kSignatureHeader || !read32(body, body_size, 0, elements) || elements > (body_size - kSignatureHeader) / layout.stride)
                return std::nullopt;
            OutputSignature out;
            out.stage = *stage;
            for (uint32_t i = 0; i < elements; i++)
            {
                const size_t at = kSignatureHeader + i * layout.stride, fields = at + (layout.stream ? 4 : 0);
                uint32_t stream = 0, name = 0, index = 0, system_value = 0, component_type = 0, reg = 0;
                if ((layout.stream && !read32(body, body_size, at, stream)) || !read32(body, body_size, fields, name)
                    || !read32(body, body_size, fields + 4, index) || !read32(body, body_size, fields + 8, system_value)
                    || !read32(body, body_size, fields + 12, component_type) || !read32(body, body_size, fields + 16, reg) || fields + 21 > body_size
                    || name >= body_size)
                    return std::nullopt;
                const void *end = std::memchr(body + name, '\0', body_size - name);
                if (end == nullptr)
                    return std::nullopt;
                SignatureElement e;
                e.semantic.assign(reinterpret_cast<const char *>(body + name), static_cast<const uint8_t *>(end) - (body + name));
                e.index = index;
                e.system_value = system_value;
                e.component_type = component_type;
                e.reg = reg;
                e.mask = body[fields + 20];
                e.stream = static_cast<uint8_t>(stream);
                out.elements.push_back(std::move(e));
            }
            return out;
        }
    }

    std::optional<OutputSignature> parse_output_signature(const uint8_t *code, size_t size)
    {
        return parse_signature(code, size, false);
    }

    std::optional<std::vector<SignatureElement>> parse_input_elements(const uint8_t *code, size_t size)
    {
        std::optional<OutputSignature> signature = parse_signature(code, size, true);
        if (!signature)
            return std::nullopt;
        return std::move(signature->elements);
    }

    std::optional<TessellatorDomain> tessellator_domain(const uint8_t *code, size_t size)
    {
        // An instruction's opcode is bits 0 to 10 of its first token and its length in tokens bits 24 to 30, except
        // customdata's (0x35), whose length is the next token. dcl_tessellator_domain is 0x95, its domain in bits 11 and 12.
        constexpr uint32_t kCustomData = 0x35, kTessDomain = 0x95;
        uint32_t total = 0, count = 0;
        if (code == nullptr || size < kHeader || std::memcmp(code, "DXBC", 4) != 0 || !read32(code, size, 24, total) || total > size
            || total < kHeader || !read32(code, size, 28, count) || count > (total - kHeader) / 4)
            return std::nullopt;
        size = total;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t offset = 0, chunk_size = 0;
            if (!read32(code, size, kHeader + 4 * i, offset) || !read32(code, size, size_t(offset) + 4, chunk_size)
                || size_t(offset) + 8 + chunk_size > size)
                return std::nullopt;
            if (std::memcmp(code + offset, "SHEX", 4) != 0 && std::memcmp(code + offset, "SHDR", 4) != 0)
                continue;
            const size_t body = size_t(offset) + 8, end = body + chunk_size;
            uint32_t tokens = 0;
            if (!read32(code, end, body + 4, tokens) || tokens < 2 || tokens > chunk_size / 4)
                return std::nullopt;
            for (size_t at = body + 8; at < body + size_t(tokens) * 4;)
            {
                uint32_t token = 0, length = 0;
                if (!read32(code, end, at, token))
                    return std::nullopt;
                const uint32_t opcode = token & 0x7FFu;
                if (opcode == kCustomData ? !read32(code, end, at + 4, length) : (length = token >> 24 & 0x7Fu, false))
                    return std::nullopt;
                if (opcode == kTessDomain)
                {
                    const uint32_t domain = token >> 11 & 3u;
                    return domain == 0 ? std::nullopt : std::optional(static_cast<TessellatorDomain>(domain));
                }
                if (length == 0)
                    return std::nullopt;
                at += size_t(length) * 4;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    std::optional<SoDeclaration> so_declaration(const OutputSignature &signature, const std::vector<SoEntry> &extra, std::string &missing)
    {
        std::optional<SoDeclaration> out = so_declaration(signature);
        if (!out)
        {
            missing = "SV_Position";
            return std::nullopt;
        }
        const auto same = [](const std::string &a, const std::string &b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(x) == std::tolower(y); });
        };
        for (const SoEntry &entry : extra)
        {
            const auto element = std::find_if(signature.elements.begin(), signature.elements.end(), [&](const SignatureElement &e) {
                if (!same(e.semantic, entry.semantic) || e.index != entry.index || entry.count == 0 || e.mask == 0)
                    return false;
                uint32_t first = 0;
                while ((e.mask >> first & 1u) == 0)
                    first++;
                for (uint32_t c = first + entry.start; c < first + entry.start + entry.count; c++)
                    if (c > 3 || (e.mask >> c & 1u) == 0)
                        return false;
                return true;
            });
            if (element == signature.elements.end())
            {
                missing = entry.semantic + std::to_string(entry.index) + '.' + std::string("xyzw").substr(std::min<size_t>(entry.start, 3), entry.count);
                return std::nullopt;
            }
            out->entries.push_back({ entry.semantic, entry.index, entry.start, entry.count, 0 });
            out->stride += 4u * entry.count;
        }
        return out;
    }

    std::optional<SoDeclaration> so_declaration(const OutputSignature &signature)
    {
        constexpr uint32_t kPosition = 1;  // D3D_NAME_POSITION
        for (const SignatureElement &e : signature.elements)
            if (e.system_value == kPosition)
                return SoDeclaration { { SoEntry { e.semantic, e.index, 0, 4, 0 } }, 16 };
        return std::nullopt;
    }
}
