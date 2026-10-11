#include "scene_gpu.h"
#include "core/dxbc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace photoreal
{
    namespace
    {
        // The buffer a capture starts with: 2.8 M triangles at 48 bytes. B's direct draws alone needed about 67 MB.
        constexpr uint64_t kBufferStart = 128ull << 20;
        constexpr uint64_t kBufferStep = 16ull << 20;
        constexpr uint64_t kBufferMax = 512ull << 20;
        constexpr uint64_t kTriangleBytes = 48;     // three float4 clip positions

        template <class T>
        void release(T *&item)
        {
            if (item)
            {
                item->Release();
                item = nullptr;
            }
        }

        std::string hresult(const char *call, HRESULT hr)
        {
            char text[96];
            std::snprintf(text, sizeof text, "%s 0x%08X", call, static_cast<unsigned>(hr));
            return text;
        }

        // Triangle lists and strips, with or without adjacency, and patch lists, which only a domain shader turns into
        // triangles. Points and lines would not fill the files' triangles.
        bool triangles(D3D11_PRIMITIVE_TOPOLOGY topology, bool domain)
        {
            switch (topology)
            {
            case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST: case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
            case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ: case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
                return !domain;
            default:
                return domain && topology >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST
                    && topology <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST;
            }
        }

        // The pixel shader's inputs t0 to t7, as NativeDraw reads them.
        void read_inputs(ID3D11DeviceContext *context, std::array<Texture, 8> &out)
        {
            ID3D11ShaderResourceView *srvs[8] = {};
            context->PSGetShaderResources(0, 8, srvs);
            for (size_t slot = 0; slot < 8; slot++)
            {
                if (srvs[slot] == nullptr)
                    continue;
                ComPtr<ID3D11Resource> resource;
                srvs[slot]->GetResource(resource.GetAddressOf());
                out[slot].id = static_cast<ResourceId>(reinterpret_cast<uintptr_t>(resource.Get()));
                ComPtr<ID3D11Texture2D> texture;
                if (SUCCEEDED(resource.As(&texture)))
                {
                    D3D11_TEXTURE2D_DESC desc;
                    texture->GetDesc(&desc);
                    out[slot].format = static_cast<Format>(desc.Format);
                    out[slot].size = { desc.Width, desc.Height };
                }
                srvs[slot]->Release();
            }
        }

        template <class Shader>
        uint64_t handle_of(const ComPtr<Shader> &shader) { return reinterpret_cast<uintptr_t>(shader.Get()); }
    }

    ReplayGuard::ReplayGuard(ID3D11DeviceContext *context, const char *&moved) : context_(context), moved_(moved)
    {
        context_->OMGetRenderTargets(1, &rtv_, &dsv_);
        context_->PSGetShader(&ps_, nullptr, nullptr);
        context_->GSGetShader(&gs_, gs_instances_, &gs_count_);
        context_->OMGetBlendState(&blend_, blend_factor_, &sample_mask_);
        context_->OMGetDepthStencilState(&depth_stencil_, &stencil_ref_);
        context_->SOGetTargets(kSoSlots, so_);
    }

    ReplayGuard::~ReplayGuard()
    {
        context_->GSSetShader(gs_, gs_instances_, gs_count_);
        context_->OMSetBlendState(blend_, blend_factor_, sample_mask_);
        context_->OMSetDepthStencilState(depth_stencil_, stencil_ref_);
        if (!game_so())
        {
            ID3D11Buffer *const none[kSoSlots] = {};
            context_->SOSetTargets(kSoSlots, none, nullptr);
        }

        ID3D11RenderTargetView *rtv = nullptr;
        ID3D11DepthStencilView *dsv = nullptr;
        ID3D11PixelShader *ps = nullptr;
        ID3D11GeometryShader *gs = nullptr;
        ID3D11BlendState *blend = nullptr;
        FLOAT factor[4];
        UINT mask = 0, ref = 0;
        ID3D11DepthStencilState *depth_stencil = nullptr;
        ID3D11Buffer *so = nullptr;
        context_->OMGetRenderTargets(1, &rtv, &dsv);
        context_->PSGetShader(&ps, nullptr, nullptr);
        context_->GSGetShader(&gs, nullptr, nullptr);
        context_->OMGetBlendState(&blend, factor, &mask);
        context_->OMGetDepthStencilState(&depth_stencil, &ref);
        context_->SOGetTargets(1, &so);
        if (rtv != rtv_)
            moved_ = "render target 0";
        else if (dsv != dsv_)
            moved_ = "depth-stencil view";
        else if (ps != ps_)
            moved_ = "pixel shader";
        else if (gs != gs_)
            moved_ = "geometry shader";
        else if (blend != blend_ || mask != sample_mask_)
            moved_ = "blend state";
        else if (depth_stencil != depth_stencil_ || ref != stencil_ref_)
            moved_ = "depth-stencil state";
        else if (so != so_[0])
            moved_ = "stream-output target";
        for (IUnknown *item : std::initializer_list<IUnknown *> { rtv, dsv, ps, gs, blend, depth_stencil, so, rtv_, dsv_, ps_, gs_, blend_, depth_stencil_ })
            if (item)
                item->Release();
        for (ID3D11Buffer *&target : so_)
            release(target);
        for (ID3D11ClassInstance *&instance : gs_instances_)
            release(instance);
    }

    void SceneGpu::init(ID3D11Device *device, ID3D11DeviceContext1 *context, ID3D11BlendState *no_write, ID3D11DepthStencilState *no_depth)
    {
        *this = SceneGpu {};
        device_ = device;
        context_ = context;
        no_write_ = no_write;
        no_depth_ = no_depth;
    }

    bool SceneGpu::make_buffers(uint64_t bytes)
    {
        D3D11_BUFFER_DESC so = {};
        so.ByteWidth = static_cast<UINT>(bytes);
        so.Usage = D3D11_USAGE_DEFAULT;
        so.BindFlags = D3D11_BIND_STREAM_OUTPUT;
        D3D11_BUFFER_DESC staging = {};
        staging.ByteWidth = static_cast<UINT>(bytes);
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> buffer, readable;
        if (FAILED(device_->CreateBuffer(&so, nullptr, buffer.GetAddressOf())) || FAILED(device_->CreateBuffer(&staging, nullptr, readable.GetAddressOf())))
            return false;
        so_buffer_ = buffer;
        so_staging_ = readable;
        buffer_bytes_ = bytes;
        return true;
    }

    std::optional<std::string> SceneGpu::begin()
    {
        end();
        if (!make_buffers(kBufferStart))
            return "the 128 MB stream-output buffer could not be made";
        return std::nullopt;
    }

    void SceneGpu::end()
    {
        so_buffer_.Reset();
        so_staging_.Reset();
        buffer_bytes_ = 0;
        queries_.clear();
        records_.clear();
        replays_ = 0;
        copied_.clear();
        copied_replays_ = 0;
        copied_frame_ = false;
        depth_ = Mirror {};
        depth_staging_.Reset();
        depth_size_ = {};
        picture_ = Staging {};
        picture_copied_ = false;
    }

    void SceneGpu::begin_frame()
    {
        records_.clear();
        replays_ = 0;
    }

    const SceneGpu::CachedGs &SceneGpu::geometry_shader(const ShaderCode &code)
    {
        const auto [it, made] = gs_cache_.try_emplace(code.hash);
        CachedGs &cached = it->second;
        if (!made)
            return cached;
        const std::string &bytes = *code.code;
        const std::optional<OutputSignature> signature = parse_output_signature(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
        const std::optional<SoDeclaration> declaration = signature ? so_declaration(*signature) : std::nullopt;
        if (!declaration)
        {
            cached.skip = DrawSkip::no_position;
            return cached;
        }
        std::vector<D3D11_SO_DECLARATION_ENTRY> entries;
        for (const SoEntry &e : declaration->entries)
            entries.push_back({ 0, e.semantic.c_str(), e.index, e.start, e.count, e.slot });
        const UINT stride = declaration->stride;
        cached.result = device_->CreateGeometryShaderWithStreamOutput(bytes.data(), bytes.size(), entries.data(), static_cast<UINT>(entries.size()), &stride,
            1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, cached.shader.GetAddressOf());
        if (FAILED(cached.result))
        {
            cached.shader.Reset();
            cached.skip = DrawSkip::gs_create_failed;
        }
        return cached;
    }

    std::optional<std::string> SceneGpu::replay(const ReplayCall &call, const ShaderRegistry &shaders, const char *&moved)
    {
        if (!so_buffer_)
            return std::nullopt;
        ID3D11DeviceContext1 *c = context_.Get();
        SceneDrawRecord &r = records_.emplace_back();
        r.index = static_cast<uint32_t>(records_.size() - 1);
        r.kind = call.kind;
        r.count = call.count;
        r.instances = call.instances;
        r.first = call.first;
        r.base_vertex = call.base_vertex;
        r.first_instance = call.first_instance;
        r.args_offset = call.args_offset;

        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11DomainShader> ds;
        ComPtr<ID3D11GeometryShader> gs;
        ComPtr<ID3D11PixelShader> ps;
        c->VSGetShader(vs.GetAddressOf(), nullptr, nullptr);
        c->DSGetShader(ds.GetAddressOf(), nullptr, nullptr);
        c->GSGetShader(gs.GetAddressOf(), nullptr, nullptr);
        c->PSGetShader(ps.GetAddressOf(), nullptr, nullptr);
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        c->IAGetPrimitiveTopology(&topology);
        r.topology = static_cast<uint32_t>(topology);
        ComPtr<ID3D11DepthStencilState> depth_state;
        UINT stencil_ref = 0;
        c->OMGetDepthStencilState(depth_state.GetAddressOf(), &stencil_ref);
        // A null state is D3D11's default: depth test LESS with writes, stencil off.
        D3D11_DEPTH_STENCIL_DESC depth = {};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc = D3D11_COMPARISON_LESS;
        if (depth_state)
            depth_state->GetDesc(&depth);
        r.stencil = depth.StencilEnable != FALSE;
        r.stencil_ref = stencil_ref;
        r.depth_func = depth.DepthEnable ? static_cast<uint32_t>(depth.DepthFunc) : 0;
        r.depth_write = depth.DepthEnable && depth.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
        read_inputs(c, r.ps_inputs);

        const std::optional<ShaderCode> vs_code = vs ? shaders.find(handle_of(vs)) : std::nullopt;
        const std::optional<ShaderCode> ds_code = ds ? shaders.find(handle_of(ds)) : std::nullopt;
        const std::optional<ShaderCode> ps_code = ps ? shaders.find(handle_of(ps)) : std::nullopt;
        r.vs = vs_code ? vs_code->hash : 0;
        r.ds = ds_code ? ds_code->hash : 0;
        r.ps = ps_code ? ps_code->hash : 0;
        // The stage before the rasterizer writes the position: the domain shader when the draw is tessellated.
        const std::optional<ShaderCode> &source = ds ? ds_code : vs_code;
        if (gs)
            r.skip = DrawSkip::gs_bound;
        else if (!source || !source->code)
            r.skip = DrawSkip::no_bytecode;
        else if (!triangles(topology, ds != nullptr))
            r.skip = DrawSkip::topology;
        else if (replays_ >= kSceneQueries)
            r.skip = DrawSkip::query_pool;
        if (r.skip != DrawSkip::none)
            return std::nullopt;

        const CachedGs &so_gs = geometry_shader(*source);
        if (so_gs.result == DXGI_ERROR_INVALID_CALL)
        {
            // DXVK refuses every stream-output shader this way when the Vulkan driver lacks transform feedback.
            support_ = 2;
            r.skip = DrawSkip::gs_create_failed;
            return "stream output unsupported (" + hresult("CreateGeometryShaderWithStreamOutput", so_gs.result) + ")";
        }
        if (so_gs.skip != DrawSkip::none)
        {
            r.skip = so_gs.skip;
            return std::nullopt;
        }
        if (queries_.size() <= replays_)
        {
            const D3D11_QUERY_DESC desc = { D3D11_QUERY_SO_STATISTICS, 0 };
            ComPtr<ID3D11Query> query;
            const HRESULT hr = device_->CreateQuery(&desc, query.GetAddressOf());
            if (FAILED(hr))
            {
                r.skip = DrawSkip::query_pool;
                return "the stream-output statistics query could not be made (" + hresult("CreateQuery", hr) + ")";
            }
            queries_.push_back(query);
        }

        std::optional<std::string> stop;
        {
            ReplayGuard guard(c, moved);
            if (guard.game_so())
                r.skip = DrawSkip::game_so;
            else
            {
                ID3D11Buffer *const target = so_buffer_.Get();
                // The frame's first replay writes from the buffer's start, and each later one appends after it.
                const UINT offset = replays_ == 0 ? 0 : ~0u;
                c->GSSetShader(so_gs.shader.Get(), nullptr, 0);
                c->SOSetTargets(1, &target, &offset);
                c->OMSetBlendState(no_write_.Get(), nullptr, 0xFFFFFFFF);
                c->OMSetDepthStencilState(no_depth_.Get(), 0);
                // The set calls return nothing, so the bindings are read back before anything draws through them.
                ComPtr<ID3D11Buffer> bound;
                ComPtr<ID3D11GeometryShader> bound_gs;
                c->SOGetTargets(1, bound.GetAddressOf());
                c->GSGetShader(bound_gs.GetAddressOf(), nullptr, nullptr);
                if (bound.Get() != target || bound_gs.Get() != so_gs.shader.Get())
                {
                    r.skip = DrawSkip::gs_create_failed;
                    stop = "the stream-output target or geometry shader did not bind";
                }
                else
                {
                    ID3D11Query *const query = queries_[replays_].Get();
                    c->Begin(query);
                    switch (call.kind)
                    {
                    case ReplayKind::draw:
                        c->DrawInstanced(call.count, call.instances, call.first, call.first_instance);
                        break;
                    case ReplayKind::indexed:
                        c->DrawIndexedInstanced(call.count, call.instances, call.first, call.base_vertex, call.first_instance);
                        break;
                    case ReplayKind::indirect:
                        c->DrawInstancedIndirect(call.args, call.args_offset);
                        break;
                    case ReplayKind::indirect_indexed:
                        c->DrawIndexedInstancedIndirect(call.args, call.args_offset);
                        break;
                    case ReplayKind::count:
                        break;
                    }
                    c->End(query);
                    r.query = replays_++;
                }
            }
        }
        return stop;
    }

    bool SceneGpu::copy_at_present(const std::optional<Texture> &depth, ID3D11Resource *back_buffer)
    {
        if (!so_buffer_)
            return false;
        ID3D11DeviceContext1 *c = context_.Get();
        copied_ = std::move(records_);
        copied_replays_ = replays_;
        records_.clear();
        replays_ = 0;
        c->CopyResource(so_staging_.Get(), so_buffer_.Get());

        depth_size_ = {};
        if (depth)
        {
            ID3D11Resource *game = reinterpret_cast<ID3D11Resource *>(static_cast<uint64_t>(depth->id));
            if (!depth_.sync(device_.Get(), game, static_cast<DXGI_FORMAT>(depth->format), false))
                return false;
            D3D11_TEXTURE2D_DESC desc = {};
            if (depth_staging_)
                depth_staging_->GetDesc(&desc);
            if (!depth_staging_ || desc.Width != depth->size.width || desc.Height != depth->size.height)
            {
                desc = {};
                desc.Width = depth->size.width;
                desc.Height = depth->size.height;
                desc.MipLevels = desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_STAGING;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                depth_staging_.Reset();
                if (FAILED(device_->CreateTexture2D(&desc, nullptr, depth_staging_.GetAddressOf())))
                    return false;
            }
            // The game's depth is a depth-stencil resource, so it reaches the CPU-readable texture through our mirror,
            // as copy_depth_texel's texel does.
            depth_.copy_from(c, game);
            depth_.copy_to(c, depth_staging_.Get());
            depth_size_ = depth->size;
        }
        picture_copied_ = back_buffer != nullptr && picture_.copy_from(device_.Get(), c, back_buffer);
        copied_frame_ = true;
        return true;
    }

    SceneRead SceneGpu::read()
    {
        SceneRead out;
        if (!copied_frame_ || !so_staging_)
        {
            out.state = SceneRead::State::failed;
            out.why = "no frame was copied";
            return out;
        }
        ID3D11DeviceContext1 *c = context_.Get();
        std::vector<D3D11_QUERY_DATA_SO_STATISTICS> stats(copied_replays_);
        for (uint32_t i = 0; i < copied_replays_; i++)
        {
            const HRESULT hr = c->GetData(queries_[i].Get(), &stats[i], sizeof stats[i], D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE)
                return out;
            if (FAILED(hr))
            {
                out.state = SceneRead::State::failed;
                out.why = "the stream-output statistics could not be read (" + hresult("GetData", hr) + ")";
                return out;
            }
        }

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (depth_size_.width != 0)
        {
            const HRESULT hr = c->Map(depth_staging_.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
                return out;
            if (FAILED(hr))
            {
                out.state = SceneRead::State::failed;
                out.why = "the depth copy could not be read (" + hresult("Map", hr) + ")";
                return out;
            }
            // 8 bytes a texel: the float depth, then the stencil and padding.
            out.depth.resize(size_t(depth_size_.width) * depth_size_.height);
            for (uint32_t y = 0; y < depth_size_.height; y++)
                for (uint32_t x = 0; x < depth_size_.width; x++)
                    std::memcpy(&out.depth[size_t(y) * depth_size_.width + x], static_cast<const uint8_t *>(mapped.pData) + size_t(y) * mapped.RowPitch + x * 8,
                        sizeof(float));
            c->Unmap(depth_staging_.Get(), 0);
            out.render = depth_size_;
        }
        if (picture_copied_)
        {
            out.picture = picture_.read(c, D3D11_MAP_FLAG_DO_NOT_WAIT);
            if (!out.picture)
                return SceneRead {};
        }

        out.records = copied_;
        uint64_t at = 0;
        for (SceneDrawRecord &r : out.records)
        {
            if (r.skip != DrawSkip::none)
                continue;
            const D3D11_QUERY_DATA_SO_STATISTICS &s = stats[r.query];
            r.written = s.NumPrimitivesWritten;
            r.needed = s.PrimitivesStorageNeeded;
            r.first_triangle = at;
            at += r.written;
            out.needed += r.needed;
            out.overflowed |= r.needed > r.written;
        }
        // Written triangles never pass the buffer's end; a statistic that says so is not trusted past it.
        if (at * kTriangleBytes > buffer_bytes_)
        {
            at = buffer_bytes_ / kTriangleBytes;
            out.overflowed = true;
        }
        const HRESULT hr = c->Map(so_staging_.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
            return SceneRead {};
        if (FAILED(hr))
        {
            out = {};
            out.state = SceneRead::State::failed;
            out.why = "the stream-output copy could not be read (" + hresult("Map", hr) + ")";
            return out;
        }
        out.clip.resize(size_t(at) * (kTriangleBytes / sizeof(float)));
        std::memcpy(out.clip.data(), mapped.pData, out.clip.size() * sizeof(float));
        c->Unmap(so_staging_.Get(), 0);
        if (at > 0)
            support_ = 1;
        out.state = SceneRead::State::ok;
        return out;
    }

    bool SceneGpu::grow(uint64_t needed_triangles)
    {
        const uint64_t wanted = std::min(kBufferMax, (needed_triangles * kTriangleBytes * 5 / 4 + kBufferStep - 1) / kBufferStep * kBufferStep);
        if (wanted <= buffer_bytes_)
            return false;
        // make_buffers keeps the old buffers when the new ones fail.
        return make_buffers(wanted);
    }
}
