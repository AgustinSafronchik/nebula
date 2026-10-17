// Per-frame acceleration-structure capture for ray tracing.
//
// Nebula draws its scene into the EFB and discards nothing: every primitive's
// vertices live in the frame's upload rings for exactly one frame. Ray tracing
// needs the same geometry as bottom-level structures, built in world space.
//
// The design here avoids any of the usual cost: a bottom-level structure is
// built directly from the frame's existing vertex ring (positions are the first
// three floats of the 144-byte GxVertexOut, so the stride is just 144), and the
// draw's world transform becomes the top-level instance transform. That means
// no separate vertex copy, no per-vertex pre-transform, and no persistent
// geometry cache.
//
// Everything is rebuilt every frame and released at the end of it. Measured on
// an RX 7600 with ~580 primitives, that costs about 1.3 ms.
//
// Only triangle lists whose vertices all reference one position matrix are
// captured; the rest report as skipped rather than being approximated.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <vector>

namespace galaxy::gx {

// One primitive as the capture sees it: where its geometry already lives in the
// frame's rings, plus the world transform that maps it to world space.
struct DxrCaptureDraw {
    D3D12_GPU_VIRTUAL_ADDRESS vertex_base = 0;
    std::uint32_t vertex_stride = 0;
    std::uint32_t vertex_count = 0;
    // Zero for non-indexed triangle lists, whose indices are the implicit
    // sequence 0,1,2,... served from one shared buffer.
    D3D12_GPU_VIRTUAL_ADDRESS index_base = 0;
    std::uint32_t index_count = 0;
    // Indices readable at index_base, from the draw's own index view. The
    // capture uses this to refuse draws whose index count would read past the
    // ring; a bottom-level structure built from out-of-range indices produces a
    // malformed structure that hangs the GPU during traversal.
    std::uint32_t index_available = 0;
    std::array<float, 12> transform{};
};

struct DxrCaptureStats {
    std::uint32_t instances = 0;
    std::uint32_t skipped_no_transform = 0;
    std::uint32_t skipped_not_triangles = 0;
    std::uint32_t skipped_too_small = 0;
    // Draws refused because their geometry could not be addressed safely:
    // out-of-range indices, too many vertices for 16-bit indices, or a
    // transform containing a non-finite value. A non-zero value here means the
    // capture is silently dropping geometry, which matters when comparing the
    // traced image against the rendered scene.
    std::uint32_t skipped_out_of_range = 0;
    std::uint32_t skipped_non_finite_transform = 0;
    std::uint64_t triangles = 0;
    std::uint64_t bytes_reserved = 0;
    bool ready = false;
};

// Owns the acceleration structures for one frame. All resources are created in
// the COMMON state and used without transitions: an acceleration structure
// build reads its inputs and writes only its own destination and scratch, so no
// barrier is required, and the COMMON state is a superstate that permits the
// reads. That also keeps the path free of the state-transition mistakes that
// the standalone benchmark had to debug.
class DxrCapture {
public:
    DxrCapture() = default;
    ~DxrCapture();
    DxrCapture(const DxrCapture&) = delete;
    DxrCapture& operator=(const DxrCapture&) = delete;

    // Returns false when the adapter cannot ray trace, which leaves the caller
    // on the raster path with no error.
    bool initialize(ID3D12Device* device, std::uint32_t capacity_hint);
    void shutdown();

    void begin_frame();
    // Records one primitive. Returns false when it cannot be captured.
    bool add_draw(const DxrCaptureDraw& draw);

    // Builds every bottom-level structure and the top-level structure into the
    // caller's command list. Must run before the tracing pass that reads the
    // top-level structure.
    void record(ID3D12GraphicsCommandList4* list);

    // Inline ray-query pass. record_trace() dispatches the compute shader that
    // traces primary rays against the structure record() just built, so it must
    // run after record() on the same command list.
    //
    // inverse_view_projection is row-major with the translation row last; the
    // shader recovers the camera position from it, so no separate camera
    // matrix is needed. Returns false when the pass is unavailable.
    bool initialize_trace(unsigned width, unsigned height);
    void shutdown_trace();
    void record_trace(ID3D12GraphicsCommandList4* list,
                      const float inverse_view_projection[16],
                      float near_plane,
                      float far_plane,
                      float strength,
                      float sky_intensity);

    [[nodiscard]] const DxrCaptureStats& stats() const noexcept { return stats_; }
    [[nodiscard]] bool usable() const noexcept { return usable_; }
    // True once the ray-query pipeline exists, and the image it writes.
    [[nodiscard]] bool trace_ready() const noexcept { return trace_ready_; }
    [[nodiscard]] ID3D12Resource* trace_texture() const noexcept {
        return trace_output_.Get();
    }
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS top_level() const noexcept {
        return tlas_ ? tlas_->GetGPUVirtualAddress() : 0;
    }
    // Instance descriptor buffer, readable by a compute shader alongside the
    // top-level structure.
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS instance_buffer() const noexcept {
        return instances_ ? instances_->GetGPUVirtualAddress() : 0;
    }

private:
    bool ensure_capacity(std::uint32_t instances, std::uint64_t result_bytes,
                         std::uint64_t scratch_bytes);
    bool grow(std::uint64_t result_bytes, std::uint64_t scratch_bytes);

    Microsoft::WRL::ComPtr<ID3D12Device5> device5_;
    bool usable_ = false;

    // Carved pools: instance i uses [i * slot_result, ...) and
    // [i * slot_scratch, ...). Slots are uniform so a frame with a different
    // draw mix does not need a reallocation of the layout.
    Microsoft::WRL::ComPtr<ID3D12Resource> blas_results_;
    Microsoft::WRL::ComPtr<ID3D12Resource> blas_scratch_;
    Microsoft::WRL::ComPtr<ID3D12Resource> tlas_;
    Microsoft::WRL::ComPtr<ID3D12Resource> tlas_scratch_;
    // Staging instance descriptors, filled on the CPU each frame.
    Microsoft::WRL::ComPtr<ID3D12Resource> instances_;
    // Same bytes on a DEFAULT heap in COMMON, because a top-level build may
    // only read its instance descriptors in COMMON and an upload buffer is
    // permanently GENERIC_READ.
    Microsoft::WRL::ComPtr<ID3D12Resource> instances_gpu_;
    // Shared sequential indices for non-indexed triangle lists.
    Microsoft::WRL::ComPtr<ID3D12Resource> sequential_indices_;

    std::uint64_t slot_result_ = 0;
    std::uint64_t slot_scratch_ = 0;
    std::uint32_t slot_count_ = 0;
    // Allocated sizes for the per-frame top-level resources, so they are
    // created once and only reallocated when a larger frame needs it.
    std::uint64_t instance_bytes_ = 0;
    std::uint64_t tlas_result_bytes_ = 0;
    std::uint64_t tlas_scratch_bytes_ = 0;

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometry_;
    std::vector<std::array<float, 12>> transforms_;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instance_data_;
    // Per-instance shade, parallel to instance_data_ and indexed the same way
    // the ray-query shader indexes it with CommittedInstanceIndex().
    std::vector<std::array<float, 4>> tints_;

    // Inline ray-query pass resources.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> trace_pso_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> trace_srv_heap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> trace_uav_heap_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> trace_root_signature_;
    Microsoft::WRL::ComPtr<ID3D12Resource> trace_output_;
    Microsoft::WRL::ComPtr<ID3D12Resource> trace_params_;
    Microsoft::WRL::ComPtr<ID3D12Resource> trace_tints_;
    D3D12_GPU_VIRTUAL_ADDRESS trace_params_address_ = 0;
    unsigned trace_width_ = 0;
    unsigned trace_height_ = 0;
    unsigned trace_slot_count_ = 0;
    bool trace_ready_ = false;

    DxrCaptureStats stats_{};
};

// True when GALAXY_DXR_CAPTURE=1. Kept as a function-local static so the
// measurement and the capture share one cheap test.
[[nodiscard]] bool dxr_capture_requested() noexcept;

}  // namespace galaxy::gx



