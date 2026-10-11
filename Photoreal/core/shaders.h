// The game's vertex and domain shader bytecode, kept from the moment each shader is created, because D3D11 cannot give
// bytecode back from a bound shader, and its pixel shaders' hashes. Pure C++17 like recipe.h. The shell feeds it from
// ReShade's init_pipeline and destroy_pipeline events, as FrameCensus does.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace photoreal
{
    // A pixel shader keeps only its hash: the scene capture names it in draws.tsv and never replays it.
    enum class ShaderStage : uint8_t { vertex, domain, pixel };

    // FrameCensus's FNV-1a of the bytecode, so a scene's files name the shaders as a census capture's shaders/ folder
    // does, and the Mac tools can join the two.
    uint64_t shader_hash(const void *code, size_t size);

    struct ShaderCode
    {
        ShaderStage stage = ShaderStage::vertex;
        uint64_t hash = 0;
        std::shared_ptr<const std::string> code;   // null for a pixel shader
    };

    // Any thread: the game creates shaders on its loading threads while the render thread looks them up.
    class ShaderRegistry
    {
    public:
        // handle is the shader object. Bytecode two handles share is held once. A handle added again replaces its entry,
        // in case a destroyed shader's address came back without its destroy event.
        void add(uint64_t handle, ShaderStage stage, const void *code, size_t size);
        void remove(uint64_t handle);
        std::optional<ShaderCode> find(uint64_t handle) const;
        // Bytecode held, each distinct one counted once; pixel shaders hold none.
        size_t bytes() const;

    private:
        struct Shared
        {
            std::shared_ptr<const std::string> code;
            uint32_t handles = 0;
        };

        void forget(uint64_t handle);  // with mutex_ held

        mutable std::mutex mutex_;
        std::unordered_map<uint64_t, ShaderCode> by_handle_;
        std::unordered_map<uint64_t, Shared> by_hash_;
        size_t bytes_ = 0;
    };
}
