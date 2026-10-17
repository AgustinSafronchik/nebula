// Per-frame acceleration-structure capture. See dxr_capture.h for the design.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "galaxy/gx/dxr_capture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "dxr_rayquery_cs.h"
#include "dxr_probe_cs.h"

namespace galaxy::gx {

namespace {

constexpr std::uint64_t kAlignment = 256;

// Indexed by D3D12_HEAP_TYPE, used when creating the ray-query resources.
// The enum starts at 1, so slot 0 is unused: indexing this array with a heap
// type directly, which reads naturally at the call sites, only lands on the
// right entry with that offset.
constexpr D3D12_HEAP_PROPERTIES heap_properties[] = {
    {},  // unused: D3D12_HEAP_TYPE_DEFAULT is 1, not 0
    {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
     D3D12_MEMORY_POOL_UNKNOWN, 0, 0},
    {D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
     D3D12_MEMORY_POOL_UNKNOWN, 0, 0},
    {D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
     D3D12_MEMORY_POOL_UNKNOWN, 0, 0},
    {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
     D3D12_MEMORY_POOL_UNKNOWN, 0, 0},
};

std::uint64_t align_up(std::uint64_t value) {
    return (value + kAlignment - 1u) & ~(kAlignment - 1u);
}

D3D12_RESOURCE_DESC buffer_desc(std::uint64_t size) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

// Uploads a one-off buffer copy and waits for it. Used during initialisation
// for data that never changes, so it does not belong on the frame path.
bool copy_buffer_once(ID3D12Device* device, ID3D12Resource* source,
                      ID3D12Resource* destination, std::uint64_t bytes) {
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_desc.Priority = 0;
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) {
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list)))) {
        return false;
    }
    list->CopyBufferRegion(destination, 0, source, 0, bytes);
    if (FAILED(list->Close())) {
        return false;
    }
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    // The contents are needed before the first frame reads them, and nothing
    // else is queued yet, so a blocking wait is the simplest correct thing.
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&fence)))) {
        return false;
    }
    if (FAILED(queue->Signal(fence.Get(), 1))) {
        return false;
    }
    // SetEventOnCompletion on the null handle means "do not signal an event",
    // so the completion is polled instead.
    if (FAILED(fence->SetEventOnCompletion(1, nullptr))) {
        return false;
    }
    while (fence->GetCompletedValue() < 1) {
        SwitchToThread();
    }
    return true;
}

bool create_buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap_type,
                   std::uint64_t size, D3D12_RESOURCE_STATES state,
                   Microsoft::WRL::ComPtr<ID3D12Resource>* out) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heap_type;
    D3D12_RESOURCE_DESC desc = buffer_desc(align_up(size));
    return SUCCEEDED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
        IID_PPV_ARGS(out->GetAddressOf())));
}

}  // namespace

bool dxr_capture_requested() noexcept {
    static const bool requested = [] {
        char value[16]{};
        std::size_t length = 0;
        return getenv_s(&length, value, sizeof(value), "GALAXY_DXR_CAPTURE") == 0 &&
               length > 1 && value[0] == '1';
    }();
    return requested;
}

DxrCapture::~DxrCapture() {
    shutdown();
}

void DxrCapture::shutdown() {
    geometry_.clear();
    instance_data_.clear();
    tlas_.Reset();
    tlas_scratch_.Reset();
    instances_.Reset();
    blas_results_.Reset();
    blas_scratch_.Reset();
    sequential_indices_.Reset();
    device5_.Reset();
    slot_result_ = 0;
    slot_scratch_ = 0;
    slot_count_ = 0;
    stats_ = {};
    usable_ = false;
}

bool DxrCapture::initialize(ID3D12Device* device, std::uint32_t capacity_hint) {
    shutdown();
    if (device == nullptr) {
        std::printf("[dxr-capture] init failed: null device\n");
        return false;
    }
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5_)))) {
        std::printf("[dxr-capture] init failed: no ID3D12Device5\n");
        return false;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    if (FAILED(device5_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5,
                                              sizeof(options5)))) {
        std::printf("[dxr-capture] init failed: OPTIONS5 query failed\n");
        device5_.Reset();
        return false;
    }
    if (options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED) {
        std::printf("[dxr-capture] init failed: adapter reports no ray tracing\n");
        device5_.Reset();
        return false;
    }

    // Implicit triangle-list indices. Every non-indexed draw uses the same
    // sequence, relative to its own vertex base, so one buffer serves all of
    // them. R16 matches Nebula's own index ring, which caps a draw at 65535
    // vertices.
    //
    // The destination must live on a DEFAULT heap in COMMON: an acceleration
    // structure build may only read inputs in COMMON, and a resource on an
    // UPLOAD heap is permanently GENERIC_READ and can never be transitioned out
    // of it, so a BLAS cannot read indices straight out of one. The bytes are
    // written once through a short-lived upload buffer and a one-off copy.
    std::vector<std::uint16_t> sequential(65536u);
    for (std::size_t i = 0; i < sequential.size(); ++i) {
        sequential[i] = static_cast<std::uint16_t>(i);
    }
    const std::uint64_t sequential_bytes = sequential.size() * sizeof(std::uint16_t);
    Microsoft::WRL::ComPtr<ID3D12Resource> staging;
    if (!create_buffer(device, D3D12_HEAP_TYPE_UPLOAD, sequential_bytes,
                       D3D12_RESOURCE_STATE_GENERIC_READ, &staging) ||
        !create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, sequential_bytes,
                       D3D12_RESOURCE_STATE_COMMON, &sequential_indices_)) {
        std::printf("[dxr-capture] init failed: sequential index buffer\n");
        shutdown();
        return false;
    }
    void* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, &mapped))) {
        std::printf("[dxr-capture] init failed: map sequential indices\n");
        shutdown();
        return false;
    }
    std::memcpy(mapped, sequential.data(), sequential_bytes);
    staging->Unmap(0, nullptr);
    if (!copy_buffer_once(device, staging.Get(), sequential_indices_.Get(),
                          sequential_bytes)) {
        std::printf("[dxr-capture] init failed: sequential index copy\n");
        shutdown();
        return false;
    }
    staging.Reset();

    const std::uint32_t capacity = std::max<std::uint32_t>(capacity_hint, 256u);
    // A small primitive fits well inside 2 KiB; grow() raises this if a frame
    // ever needs more, so the first allocation is not tuned to one scene.
    slot_result_ = 2048;
    slot_scratch_ = 2048;
    slot_count_ = capacity;

    if (!create_buffer(device, D3D12_HEAP_TYPE_DEFAULT,
                       slot_result_ * slot_count_, D3D12_RESOURCE_STATE_COMMON,
                       &blas_results_) ||
        !create_buffer(device, D3D12_HEAP_TYPE_DEFAULT,
                       slot_scratch_ * slot_count_, D3D12_RESOURCE_STATE_COMMON,
                       &blas_scratch_)) {
        std::printf("[dxr-capture] init failed: BLAS pool\n");
        shutdown();
        return false;
    }

    geometry_.reserve(capacity);
    transforms_.reserve(capacity);
    instance_data_.reserve(capacity);
    usable_ = true;
    return true;
}

void DxrCapture::begin_frame() {
    // Only the per-frame statistics are reset here. The capture lists are
    // consumed and cleared by record() on the render thread, after the last
    // draw of the frame, which is the only place clearing it is safe: draws
    // arrive from the render thread while this runs on the main thread.
    stats_ = {};
}

bool DxrCapture::add_draw(const DxrCaptureDraw& draw) {
    if (!usable_) {
        return false;
    }
    if (draw.index_count < 3u || draw.index_count % 3u != 0u) {
        ++stats_.skipped_too_small;
        return false;
    }
    if (draw.vertex_base == 0 || draw.vertex_stride == 0 || draw.vertex_count == 0) {
        ++stats_.skipped_too_small;
        return false;
    }

    // Refuse geometry the structure cannot address safely. These are the
    // inputs that produce a malformed bottom-level structure, which does not
    // fail the build but hangs the GPU later during traversal, so they are
    // rejected here where the reason is still known.
    //
    // A transform with a non-finite entry collapses or explodes the instance,
    // and the traversal of such a structure never terminates.
    for (const float value : draw.transform) {
        if (!std::isfinite(value)) {
            ++stats_.skipped_non_finite_transform;
            return false;
        }
    }
    // The sequential index buffer is R16 and holds 65536 entries, and every
    // index it serves must also be a valid vertex of the same draw.
    if (draw.index_base == 0) {
        if (draw.index_count > 65536u || draw.index_count > draw.vertex_count) {
            ++stats_.skipped_out_of_range;
            return false;
        }
    } else {
        // Indexed draw: the index count has to fit in the draw's own view.
        if (draw.index_available != 0u && draw.index_count > draw.index_available) {
            ++stats_.skipped_out_of_range;
            return false;
        }
        // 16-bit indices cannot address vertices past 65535.
        if (draw.vertex_count > 65536u) {
            ++stats_.skipped_out_of_range;
            return false;
        }
    }
    // One instance per slot; a frame with more primitives than slots is
    // truncated rather than reallocating mid-record.
    if (geometry_.size() >= slot_count_) {
        return false;
    }

    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.VertexBuffer.StartAddress = draw.vertex_base;
    geometry.Triangles.VertexBuffer.StrideInBytes = draw.vertex_stride;
    geometry.Triangles.VertexCount = draw.vertex_count;
    geometry.Triangles.IndexCount = draw.index_count;
    if (draw.index_base != 0) {
        geometry.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
        geometry.Triangles.IndexBuffer = draw.index_base;
    } else {
        geometry.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
        geometry.Triangles.IndexBuffer = sequential_indices_->GetGPUVirtualAddress();
    }
    geometry_.push_back(geometry);
    transforms_.push_back(draw.transform);

    // Shade derived from the slot so adjacent primitives stay distinguishable
    // in the traced image. Index parity matches instance_data_, which is built
    // in the same order.
    const std::uint32_t index = static_cast<std::uint32_t>(tints_.size());
    // Hue advances half a sector per slot, giving six distinct colours before
    // it wraps, and the value cycles separately so neighbours differ in
    // brightness too.
    const int sector = static_cast<int>(index % 6u);
    const float value = 0.55f + 0.20f * static_cast<float>((index / 6u) % 2u);
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    switch (sector) {
        case 0: red = value; green = value * 0.30f; blue = value * 0.20f; break;
        case 1: red = value * 0.30f; green = value; blue = value * 0.20f; break;
        case 2: red = value * 0.20f; green = value; blue = value * 0.30f; break;
        case 3: red = value * 0.20f; green = value * 0.35f; blue = value; break;
        case 4: red = value * 0.30f; green = value * 0.20f; blue = value; break;
        default: red = value; green = value * 0.25f; blue = value * 0.35f; break;
    }
    tints_.push_back({red, green, blue, 1.0f});
    return true;
}

void DxrCapture::record(ID3D12GraphicsCommandList4* list) {
    if (!usable_ || list == nullptr || geometry_.empty()) {
        return;
    }

    // Stage limit for bisecting which recorded command invalidates the list.
    // 0 = everything, 1 = bottom-level builds only, 2 = plus the instance copy
    // and its transition, 3 = plus the top-level build.
    static const int build_stage = [] {
        char value[8]{};
        std::size_t length = 0;
        if (getenv_s(&length, value, sizeof(value), "GALAXY_DXR_BUILD_STAGE") != 0 ||
            length < 2) {
            return 0;
        }
        return value[0] - '0';
    }();
    if (build_stage == 1) {
        geometry_.clear();
        transforms_.clear();
        instance_data_.clear();
        tints_.clear();
        return;
    }

    const auto count = static_cast<std::uint32_t>(geometry_.size());
    std::uint64_t index_total = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        index_total += geometry_[i].Triangles.IndexCount;
    }

    // Every slot must be large enough for this frame's primitives.
    std::uint64_t result_bytes = 0;
    std::uint64_t scratch_bytes = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.NumDescs = 1;
        inputs.pGeometryDescs = &geometry_[i];
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device5_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
        result_bytes = std::max(result_bytes, info.ResultDataMaxSizeInBytes);
        scratch_bytes = std::max(scratch_bytes, info.ScratchDataSizeInBytes);
    }
    if (!ensure_capacity(count, align_up(result_bytes), align_up(scratch_bytes))) {
        return;
    }

    instance_data_.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.NumDescs = 1;
        inputs.pGeometryDescs = &geometry_[i];

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.DestAccelerationStructureData =
            blas_results_->GetGPUVirtualAddress() + i * slot_result_;
        build.ScratchAccelerationStructureData =
            blas_scratch_->GetGPUVirtualAddress() + i * slot_scratch_;
        build.Inputs = inputs;
            list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    }

    // Instance descriptors: one per bottom-level structure, carrying the draw's
    // world transform. This is where the object-space bottom level becomes
    // world space.
    instance_data_.clear();
    instance_data_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        D3D12_RAYTRACING_INSTANCE_DESC instance{};
        const auto& transform = transforms_[i];
        for (unsigned row = 0; row < 3u; ++row) {
            for (unsigned column = 0; column < 4u; ++column) {
                instance.Transform[row][column] = transform[row * 4u + column];
            }
        }
        instance.InstanceID = i;
        instance.InstanceMask = 0xFF;
        instance.AccelerationStructure =
            blas_results_->GetGPUVirtualAddress() + i * slot_result_;
        instance_data_.push_back(instance);
    }

    const std::uint64_t instance_bytes =
        instance_data_.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    // These persist across frames and are only (re)allocated when they need to
    // grow. Recreating them every frame destroys a resource the GPU may still be
    // reading from the previous frame, which is what made the top-level build
    // fail validation.
    //
    // Staging on an upload heap, plus a copy on a DEFAULT heap in COMMON: the
    // top-level build may only read its instance descriptors in COMMON, and an
    // upload buffer is permanently GENERIC_READ.
    if (instance_bytes > instance_bytes_ ||
        instances_ == nullptr || instances_gpu_ == nullptr) {
        if (!create_buffer(device5_.Get(), D3D12_HEAP_TYPE_UPLOAD, instance_bytes,
                           D3D12_RESOURCE_STATE_GENERIC_READ, &instances_) ||
            !create_buffer(device5_.Get(), D3D12_HEAP_TYPE_DEFAULT, instance_bytes,
                           D3D12_RESOURCE_STATE_COPY_DEST, &instances_gpu_)) {
            return;
        }
        instance_bytes_ = instance_bytes;
    }
    void* mapped = nullptr;
    if (FAILED(instances_->Map(0, nullptr, &mapped))) {
        instances_.Reset();
        return;
    }
    std::memcpy(mapped, instance_data_.data(), instance_bytes);
    instances_->Unmap(0, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top_inputs{};
    top_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top_inputs.NumDescs = count;
    top_inputs.InstanceDescs = instances_gpu_->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO top_info{};
    device5_->GetRaytracingAccelerationStructurePrebuildInfo(&top_inputs, &top_info);
    // Same persistence rule as the instance buffers: allocate once and only
    // when a larger frame arrives. Recreating the top-level structure every
    // frame frees memory the previous frame's traversal may still be using.
    if (top_info.ResultDataMaxSizeInBytes > tlas_result_bytes_ ||
        top_info.ScratchDataSizeInBytes > tlas_scratch_bytes_ ||
        tlas_ == nullptr || tlas_scratch_ == nullptr) {
        if (!create_buffer(device5_.Get(), D3D12_HEAP_TYPE_DEFAULT,
                           top_info.ResultDataMaxSizeInBytes,
                           D3D12_RESOURCE_STATE_COMMON, &tlas_) ||
            !create_buffer(device5_.Get(), D3D12_HEAP_TYPE_DEFAULT,
                           top_info.ScratchDataSizeInBytes,
                           D3D12_RESOURCE_STATE_COMMON, &tlas_scratch_)) {
            return;
        }
        tlas_result_bytes_ = top_info.ResultDataMaxSizeInBytes;
        tlas_scratch_bytes_ = top_info.ScratchDataSizeInBytes;
    }

    // Stage the descriptors, then move them into the COMMON copy the build
    // reads from. Both halves are required: the copy cannot happen while the
    // destination is still in COMMON, and the build cannot read COPY_DEST.
    list->CopyBufferRegion(instances_gpu_.Get(), 0, instances_.Get(), 0,
                           instance_bytes);
    D3D12_RESOURCE_BARRIER to_common{};
    to_common.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_common.Transition.pResource = instances_gpu_.Get();
    to_common.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_common.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_common.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(1, &to_common);
    if (build_stage == 2) {
        geometry_.clear();
        transforms_.clear();
        instance_data_.clear();
        tints_.clear();
        return;
    }

    // No barrier here on purpose. The build pools live in
    // D3D12_RESOURCE_STATE_COMMON, which is a superstate that permits the UAV
    // writes an acceleration-structure build performs, and an explicit
    // UAV barrier against a resource still in COMMON is invalid: it makes
    // ID3D12GraphicsCommandList::Close fail with E_INVALIDARG. Ordering between
    // the structures and the tracing dispatch is handled by the transition in
    // record_trace() instead.
    // The bottom-level builds above write the structures the top-level build
    // references by address, and that dependency needs ordering. A UAV barrier
    // is not usable here because the pools live in COMMON and a UAV barrier
    // against a COMMON resource is invalid. Moving them into the
    // ray-tracing-acceleration-structure state is the supported way to both
    // order the writes against the reads and declare the state the traversal
    // expects.
    D3D12_RESOURCE_BARRIER to_as{};
    to_as.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_as.Transition.pResource = blas_results_.Get();
    to_as.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_as.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    to_as.Transition.StateAfter = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    list->ResourceBarrier(1, &to_as);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC top_build{};
    top_build.DestAccelerationStructureData = tlas_->GetGPUVirtualAddress();
    top_build.ScratchAccelerationStructureData = tlas_scratch_->GetGPUVirtualAddress();
    top_build.Inputs = top_inputs;
    list->BuildRaytracingAccelerationStructure(&top_build, 0, nullptr);

    stats_.instances = count;
    stats_.triangles += index_total;
    stats_.ready = true;

    // Consumed: the next frame starts from empty, on this same thread.
    geometry_.clear();
    transforms_.clear();
    instance_data_.clear();
}

// Reports which stage of the ray-query pass setup failed. On stderr, because a
// failing setup is exactly when a buffered log would be lost.
void dxr_trace_fail(const char* stage, HRESULT hr) {
    std::fprintf(stderr, "[dxr-trace] init failed at %s hr=0x%08lx\n", stage,
                 static_cast<unsigned long>(hr));
}

bool DxrCapture::initialize_trace(unsigned width, unsigned height) {
    if (!usable_ || device5_ == nullptr || width == 0u || height == 0u) {
        return false;
    }
    shutdown_trace();

    trace_width_ = width;
    trace_height_ = height;

    // The shader binds the instance tints and the top-level structure as SRVs
    // (t1, t2) and writes the image through a UAV (u0). A UAV binding needs a
    // UAV descriptor range; a table of SRV-type ranges alone does not satisfy
    // it, and CreateComputePipelineState rejects the mismatch.
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].NumDescriptors = 2;  // t1 instance tints, t2 top level structure
    ranges[0].BaseShaderRegister = 1;
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;  // u0 traced image
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 2;
    parameter.DescriptorTable.pDescriptorRanges = ranges;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_DESCRIPTOR cbv{};
    cbv.ShaderRegister = 0;  // shader register b0
    D3D12_ROOT_PARAMETER constants{};
    constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    constants.Descriptor = cbv;
    constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    const D3D12_ROOT_PARAMETER parameters[2] = {parameter, constants};
    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = 2;
    signature.pParameters = parameters;
    signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    // This SDK only exposes the serialised form of CreateRootSignature, so the
    // description is serialised first.
    Microsoft::WRL::ComPtr<ID3DBlob> signature_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> signature_error;
    if (FAILED(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1,
                                          &signature_blob, &signature_error))) {
        dxr_trace_fail("root-signature-serialise", S_OK);
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    if (FAILED(device5_->CreateRootSignature(
            0, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(),
            IID_PPV_ARGS(&root_signature)))) {
        dxr_trace_fail("root-signature-create", S_OK);
        return false;
    }
    trace_root_signature_ = root_signature;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root_signature.Get();
    // GALAXY_DXR_PROBE=1 swaps in a shader with no RayQuery. The driver used
    // here crashes its own shader compiler on any RayQuery shader, so this is
    // the only way to exercise the dispatch, the output texture and the blend
    // on this machine.
    static const bool probe = [] {
        char value[8]{};
        std::size_t length = 0;
        return getenv_s(&length, value, sizeof(value), "GALAXY_DXR_PROBE") == 0 &&
               length > 1 && value[0] == '1';
    }();
    pso.CS.pShaderBytecode = probe ? g_dxr_probe_cs_dxil : g_dxr_rayquery_cs_dxil;
    pso.CS.BytecodeLength = probe ? g_dxr_probe_cs_dxil_size
                                  : g_dxr_rayquery_cs_dxil_size;
    if (FAILED(device5_->CreateComputePipelineState(&pso,
                                                    IID_PPV_ARGS(&trace_pso_)))) {
        dxr_trace_fail("compute-pso", S_OK);
        shutdown_trace();
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
    srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = 3;
    srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device5_->CreateDescriptorHeap(&srv_desc,
                                              IID_PPV_ARGS(&trace_srv_heap_)))) {
        dxr_trace_fail("srv-heap", S_OK);
        shutdown_trace();
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC uav_desc{};
    uav_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uav_desc.NumDescriptors = 1;
    if (FAILED(device5_->CreateDescriptorHeap(&uav_desc,
                                              IID_PPV_ARGS(&trace_uav_heap_)))) {
        dxr_trace_fail("uav-heap", S_OK);
        shutdown_trace();
        return false;
    }

    D3D12_RESOURCE_DESC output_desc{};
    output_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    output_desc.Width = width;
    output_desc.Height = height;
    output_desc.DepthOrArraySize = 1;
    output_desc.MipLevels = 1;
    // Float16 rather than UNORM: the blend samples this texture with a linear
    // filter, and a filtered UNORM UAV is the combination older validation
    // paths tend to reject.
    output_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    output_desc.SampleDesc.Count = 1;
    // SampleDesc.Quality must be 1; leaving it zeroed makes the whole
    // description invalid and CreateCommittedResource returns E_INVALIDARG.
    output_desc.SampleDesc.Quality = 1;
    output_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const HRESULT hr = device5_->CreateCommittedResource(
        &heap_properties[D3D12_HEAP_TYPE_DEFAULT], D3D12_HEAP_FLAG_NONE,
        &output_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&trace_output_));
    if (FAILED(hr)) {
        // Every field, because this description is the difference between a
        // texture the engine creates successfully and one the runtime rejects.
        // Comparing the two dumps is faster than guessing which field is wrong.
        std::fprintf(stderr,
                     "[dxr-trace] output desc: dim=%u layout=%u w=%llu h=%u "
                     "array=%u mips=%u fmt=%u sample={count=%u quality=%u} "
                     "flags=0x%x align=%u\n",
                     static_cast<unsigned>(output_desc.Dimension),
                     static_cast<unsigned>(output_desc.Layout),
                     static_cast<unsigned long long>(output_desc.Width),
                     output_desc.Height, static_cast<unsigned>(output_desc.DepthOrArraySize),
                     output_desc.MipLevels, static_cast<unsigned>(output_desc.Format),
                     output_desc.SampleDesc.Count, output_desc.SampleDesc.Quality,
                     static_cast<unsigned>(output_desc.Flags),
                     static_cast<unsigned>(output_desc.Alignment));
        std::fprintf(stderr, "[dxr-trace] heap: type=%u page=%u pool=%u\n",
                     static_cast<unsigned>(heap_properties[D3D12_HEAP_TYPE_DEFAULT].Type),
                     static_cast<unsigned>(
                         heap_properties[D3D12_HEAP_TYPE_DEFAULT].CPUPageProperty),
                     static_cast<unsigned>(
                         heap_properties[D3D12_HEAP_TYPE_DEFAULT].MemoryPoolPreference));
        dxr_trace_fail("output-texture", hr);
        shutdown_trace();
        return false;
    }

    // RayParams: four inverse-view-projection rows plus three packed vectors.
    if (!create_buffer(device5_.Get(), D3D12_HEAP_TYPE_UPLOAD, 256u,
                       D3D12_RESOURCE_STATE_GENERIC_READ, &trace_params_)) {
        shutdown_trace();
        return false;
    }
    trace_params_address_ = trace_params_->GetGPUVirtualAddress();

    trace_ready_ = true;
    return true;
}

void DxrCapture::shutdown_trace() {
    trace_params_.Reset();
    trace_tints_.Reset();
    trace_output_.Reset();
    trace_srv_heap_.Reset();
    trace_uav_heap_.Reset();
    trace_root_signature_.Reset();
    trace_pso_.Reset();
    trace_width_ = 0;
    trace_height_ = 0;
    trace_slot_count_ = 0;
    trace_ready_ = false;
}

void DxrCapture::record_trace(ID3D12GraphicsCommandList4* list,
                              const float inverse_view_projection[16],
                              float near_plane, float far_plane, float strength,
                              float sky_intensity) {
    // record() runs first and has already consumed geometry_, so the instance
    // count is read from stats_ rather than from the capture lists.
    if (!trace_ready_ || !usable_ || list == nullptr ||
        inverse_view_projection == nullptr || stats_.instances == 0u ||
        trace_width_ == 0u || trace_height_ == 0u) {
        return;
    }

    const auto count = static_cast<std::uint32_t>(tints_.size());
    if (count == 0u) {
        return;
    }

    // Instance tints, uploaded each frame because the instance set changes.
    const std::uint64_t tint_bytes = align_up(count * sizeof(float) * 4u);
    if (trace_tints_ == nullptr || trace_slot_count_ < count) {
        trace_tints_.Reset();
        if (!create_buffer(device5_.Get(), D3D12_HEAP_TYPE_UPLOAD, tint_bytes,
                           D3D12_RESOURCE_STATE_GENERIC_READ, &trace_tints_)) {
            return;
        }
        trace_slot_count_ = count;
    }
    void* mapped = nullptr;
    if (trace_tints_->Map(0, nullptr, &mapped) == S_OK && mapped != nullptr) {
        std::memcpy(mapped, tints_.data(), count * sizeof(float) * 4u);
        trace_tints_->Unmap(0, nullptr);
    }

    struct RayParams {
        float row0[4];
        float row1[4];
        float row2[4];
        float row3[4];
        float screen[4];
        float light[4];
        float tune[4];
    } params{};
    std::memcpy(params.row0, inverse_view_projection + 0, sizeof(float) * 4u);
    std::memcpy(params.row1, inverse_view_projection + 4, sizeof(float) * 4u);
    std::memcpy(params.row2, inverse_view_projection + 8, sizeof(float) * 4u);
    std::memcpy(params.row3, inverse_view_projection + 12, sizeof(float) * 4u);
    params.screen[0] = static_cast<float>(trace_width_);
    params.screen[1] = static_cast<float>(trace_height_);
    params.screen[2] = near_plane;
    params.screen[3] = far_plane;
    params.light[0] = 0.35f;
    params.light[1] = 0.62f;
    params.light[2] = 0.70f;
    params.light[3] = 0.12f;
    params.tune[0] = strength;
    params.tune[1] = sky_intensity;
    if (trace_params_->Map(0, nullptr, &mapped) == S_OK && mapped != nullptr) {
        std::memcpy(mapped, &params, sizeof(params));
        trace_params_->Unmap(0, nullptr);
    }

    // The dispatch below reads the top-level structure the build above produced.
    // The build pools are in COMMON, so an explicit UAV barrier against them
    // would be invalid; transitioning the structure into the ray-tracing
    // acceleration-structure state is the supported way to both order the write
    // against the read and declare the state the traversal expects.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = tlas_.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    list->ResourceBarrier(1, &barrier);

    // This SDK's d3d12.h predates GetDescriptorTableFromHeap, so the handles are
    // derived from the heap starts directly.
    const UINT increment = device5_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE tlas_cpu =
        trace_srv_heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE tlas_gpu =
        trace_srv_heap_->GetGPUDescriptorHandleForHeapStart();
    tlas_cpu.ptr += increment;  // the shader reads tints at t1, tlas at t2
    tlas_gpu.ptr += increment;

    D3D12_SHADER_RESOURCE_VIEW_DESC tints_view{};
    tints_view.Format = DXGI_FORMAT_UNKNOWN;
    tints_view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    tints_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    tints_view.Buffer.StructureByteStride = sizeof(float) * 4u;
    device5_->CreateShaderResourceView(trace_tints_.Get(), &tints_view,
                                       tlas_cpu);
    // D3D12_RAYTRACING_ACCELERATION_STRUCTURE_SRV holds the address, not a
    // resource pointer, in this SDK.
    D3D12_SHADER_RESOURCE_VIEW_DESC tlas_view{};
    tlas_view.Format = DXGI_FORMAT_UNKNOWN;
    tlas_view.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    tlas_view.RaytracingAccelerationStructure.Location =
        tlas_->GetGPUVirtualAddress();
    device5_->CreateShaderResourceView(nullptr, &tlas_view, tlas_cpu);

    D3D12_CPU_DESCRIPTOR_HANDLE output_cpu =
        trace_uav_heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device5_->CreateUnorderedAccessView(trace_output_.Get(), nullptr, &uav,
                                        output_cpu);

    // The output texture is created in COMMON; the dispatch writes it as a UAV,
    // and the blend pass later transitions it out of UNORDERED_ACCESS to read
    // it. Without this transition the states disagree and the command list
    // fails to close.
    D3D12_RESOURCE_BARRIER output_barrier{};
    output_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    output_barrier.Transition.pResource = trace_output_.Get();
    output_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    output_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    output_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list->ResourceBarrier(1, &output_barrier);

    // Stage limit for bisecting which recorded command makes the list invalid.
    // 0 = everything, 1 = nothing recorded here, 2 = barriers only,
    // 3 = barriers and descriptors, 4 = full dispatch.
    enum : int { kNothing = 1, kBarriers = 2, kDescriptors = 3 };
    static const int early_stage = [] {
        char value[8]{};
        std::size_t length = 0;
        if (getenv_s(&length, value, sizeof(value), "GALAXY_DXR_TRACE_STAGE") != 0 ||
            length < 2) {
            return 0;
        }
        return value[0] - '0';
    }();
    if (early_stage == kNothing) {
        return;
    }
    // 0 = everything, 1 = transitions only, 2 = transitions plus descriptors,
    // 3 = full dispatch.
    static const int stage_limit = [] {
        char value[8]{};
        std::size_t length = 0;
        if (getenv_s(&length, value, sizeof(value), "GALAXY_DXR_TRACE_STAGE") != 0 ||
            length < 2) {
            return 0;
        }
        return value[0] - '0';
    }();
    // The ray-query pass has its own descriptor heaps, and D3D12 requires them
    // bound on the command list before the root tables that reference them are
    // set. Without this the tables resolve to descriptors the list cannot see
    // and Close() fails with E_INVALIDARG.
    ID3D12DescriptorHeap* heaps[2] = {trace_srv_heap_.Get(), trace_uav_heap_.Get()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetComputeRootSignature(trace_root_signature_.Get());
    list->SetComputeRootDescriptorTable(0, tlas_gpu);
    list->SetComputeRootConstantBufferView(0, trace_params_address_);
    if (early_stage == kBarriers) {
        return;
    }
    list->SetPipelineState(trace_pso_.Get());
    const auto groups_x = (trace_width_ + 7u) / 8u;
    const auto groups_y = (trace_height_ + 7u) / 8u;
    list->Dispatch(groups_x, groups_y, 1);
}

bool DxrCapture::ensure_capacity(std::uint32_t instances, std::uint64_t result_bytes,
                                std::uint64_t scratch_bytes) {
    if (instances <= slot_count_ && result_bytes <= slot_result_ &&
        scratch_bytes <= slot_scratch_) {
        return true;
    }
    return grow(std::max(result_bytes, slot_result_),
                std::max(scratch_bytes, slot_scratch_));
}

bool DxrCapture::grow(std::uint64_t result_bytes, std::uint64_t scratch_bytes) {
    slot_result_ = std::max(slot_result_, align_up(result_bytes));
    slot_scratch_ = std::max(slot_scratch_, align_up(scratch_bytes));
    if (!create_buffer(device5_.Get(), D3D12_HEAP_TYPE_DEFAULT,
                       slot_result_ * slot_count_, D3D12_RESOURCE_STATE_COMMON,
                       &blas_results_) ||
        !create_buffer(device5_.Get(), D3D12_HEAP_TYPE_DEFAULT,
                       slot_scratch_ * slot_count_, D3D12_RESOURCE_STATE_COMMON,
                       &blas_scratch_)) {
        usable_ = false;
        return false;
    }
    return true;
}

}  // namespace galaxy::gx














