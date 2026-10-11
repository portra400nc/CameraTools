#include "shaders.h"

namespace photoreal
{
    uint64_t shader_hash(const void *code, size_t size)
    {
        uint64_t hash = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < size; i++)
            hash = (hash ^ static_cast<const uint8_t *>(code)[i]) * 0x100000001b3ull;
        return hash;
    }

    void ShaderRegistry::add(uint64_t handle, ShaderStage stage, const void *code, size_t size)
    {
        const uint64_t hash = shader_hash(code, size);
        std::lock_guard lock(mutex_);
        forget(handle);
        if (stage == ShaderStage::pixel)
        {
            by_handle_[handle] = ShaderCode { stage, hash, nullptr };
            return;
        }
        Shared &shared = by_hash_[hash];
        if (!shared.code)
        {
            shared.code = std::make_shared<const std::string>(static_cast<const char *>(code), size);
            bytes_ += size;
        }
        shared.handles++;
        by_handle_[handle] = ShaderCode { stage, hash, shared.code };
    }

    void ShaderRegistry::remove(uint64_t handle)
    {
        std::lock_guard lock(mutex_);
        forget(handle);
    }

    void ShaderRegistry::forget(uint64_t handle)
    {
        const auto it = by_handle_.find(handle);
        if (it == by_handle_.end())
            return;
        if (!it->second.code)
        {
            by_handle_.erase(it);
            return;
        }
        const auto shared = by_hash_.find(it->second.hash);
        if (--shared->second.handles == 0)
        {
            bytes_ -= shared->second.code->size();
            by_hash_.erase(shared);
        }
        by_handle_.erase(it);
    }

    std::optional<ShaderCode> ShaderRegistry::find(uint64_t handle) const
    {
        std::lock_guard lock(mutex_);
        const auto it = by_handle_.find(handle);
        if (it == by_handle_.end())
            return std::nullopt;
        return it->second;
    }

    size_t ShaderRegistry::bytes() const
    {
        std::lock_guard lock(mutex_);
        return bytes_;
    }
}
