// Copyright 2019 The Dawn & Tint Authors
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// OpenGLBackend.cpp: contains the definition of symbols exported by OpenGLBackend.h so that they
// can be compiled twice: once export (shared library), once not exported (static library)

#include "dawn/native/OpenGLBackend.h"

#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>

#include "src/dawn/native/BindGroupLayoutInternal.h"
#include "src/dawn/native/opengl/BindGroupGL.h"
#include "src/dawn/native/opengl/BufferGL.h"
#include "src/dawn/native/opengl/DeviceGL.h"
#include "src/dawn/native/opengl/PipelineLayoutGL.h"
#include "src/dawn/native/opengl/RenderPipelineGL.h"
#include "src/dawn/native/opengl/SamplerGL.h"
#include "src/dawn/native/opengl/TextureGL.h"

namespace dawn::native::opengl {

// Defined in CommandBufferGL.cpp.
std::vector<GLuint> GLInteropDrainDestroyedTextures();
void GLInteropSetTrackDestroyedTextures(bool track);

namespace {
std::atomic<GLInteropRenderPassCallback> gInteropRenderPassCallback = nullptr;
std::atomic<void*> gInteropRenderPassUserdata = nullptr;
std::atomic<const GLInteropPresentCallbacks*> gInteropPresentCallbacks = nullptr;

bool ResolveBindingIndex(const BindGroupLayoutInternalBase* layout,
                         uint32_t binding,
                         BindingIndex* index) {
    const auto& bindings = layout->GetBindingMap();
    const auto it = bindings.find(BindingNumber(binding));
    if (it == bindings.end()) {
        return false;
    }
    *index = layout->AsBindingIndex(it->second);
    return true;
}

GLenum InteropComponentSwizzle(wgpu::ComponentSwizzle swizzle) {
    switch (swizzle) {
        case wgpu::ComponentSwizzle::Zero:
            return GL_ZERO;
        case wgpu::ComponentSwizzle::One:
            return GL_ONE;
        case wgpu::ComponentSwizzle::R:
            return GL_RED;
        case wgpu::ComponentSwizzle::G:
            return GL_GREEN;
        case wgpu::ComponentSwizzle::B:
            return GL_BLUE;
        case wgpu::ComponentSwizzle::A:
            return GL_ALPHA;
        case wgpu::ComponentSwizzle::Undefined:
            return GL_NONE;
    }
    return GL_NONE;
}

GLInteropTextureInfo InteropTextureInfo(TextureView* view) {
    const auto swizzle = view->GetSwizzle();
    return {.texture = view->GetTextureHandle(),
            .renderbuffer = view->GetRenderbufferHandle(),
            .target = view->GetGLTarget(),
            .baseMipLevel = view->GetBaseMipLevel(),
            .maxMipLevel = view->GetBaseMipLevel() + view->GetLevelCount() - 1,
            .swizzle = {InteropComponentSwizzle(swizzle.r), InteropComponentSwizzle(swizzle.g),
                        InteropComponentSwizzle(swizzle.b), InteropComponentSwizzle(swizzle.a)}};
}
}  // namespace

RequestAdapterOptionsGetGLProc::RequestAdapterOptionsGetGLProc() {
    sType = wgpu::SType::RequestAdapterOptionsGetGLProc;
}

RequestAdapterOptionsAngleVirtualizationGroup::RequestAdapterOptionsAngleVirtualizationGroup() {
    sType = wgpu::SType::RequestAdapterOptionsAngleVirtualizationGroup;
}

ExternalImageDescriptorEGLImage::ExternalImageDescriptorEGLImage()
    : ExternalImageDescriptor(ExternalImageType::EGLImage) {}

ExternalImageDescriptorGLTexture::ExternalImageDescriptorGLTexture()
    : ExternalImageDescriptor(ExternalImageType::GLTexture) {}

WGPUTexture WrapExternalEGLImage(WGPUDevice device,
                                 const ExternalImageDescriptorEGLImage* descriptor) {
    Device* backendDevice = ToBackend(FromAPI(device));
    Ref<TextureBase> texture =
        backendDevice->CreateTextureWrappingEGLImage(descriptor, descriptor->image);
    return ToAPI(ReturnToAPI(std::move(texture)));
}

WGPUTexture WrapExternalGLTexture(WGPUDevice device,
                                  const ExternalImageDescriptorGLTexture* descriptor) {
    Device* backendDevice = ToBackend(FromAPI(device));
    Ref<TextureBase> texture =
        backendDevice->CreateTextureWrappingGLTexture(descriptor, descriptor->texture);
    return ToAPI(ReturnToAPI(std::move(texture)));
}

void SetGLInteropPresentCallbacks(const GLInteropPresentCallbacks* callbacks) {
    if (callbacks != nullptr) {
        GLInteropSetTrackDestroyedTextures(true);
    }
    gInteropPresentCallbacks.store(callbacks, std::memory_order_release);
}

const GLInteropPresentCallbacks* GetGLInteropPresentCallbacks() {
    return gInteropPresentCallbacks.load(std::memory_order_acquire);
}

bool RunGLInterop(WGPUDevice device, GLInteropCallback callback, void* userdata) {
    Device* backendDevice = ToBackend(FromAPI(device));
    if (callback == nullptr) {
        return false;
    }
    return !backendDevice->ConsumedError(backendDevice->EnqueueAndFlushGL(
        [callback, userdata](const OpenGLFunctions&) -> MaybeError {
            callback(userdata);
            return {};
        }));
}

GLuint GetGLInteropBuffer(WGPUBuffer buffer) {
    return ToBackend(FromAPI(buffer))->GetHandle();
}

GLuint GetGLInteropSampler(WGPUSampler sampler) {
    return ToBackend(FromAPI(sampler))->GetHandle();
}

GLInteropTextureInfo GetGLInteropTextureView(WGPUTextureView view) {
    return InteropTextureInfo(ToBackend(FromAPI(view)));
}

GLInteropTextureInfo GetGLInteropBindGroupTexture(WGPUBindGroup group, uint32_t binding) {
    auto* bindGroup = ToBackend(FromAPI(group));
    BindingIndex index;
    if (!ResolveBindingIndex(bindGroup->GetLayout(), binding, &index)) {
        return {};
    }
    auto* textureView = bindGroup->GetBindingAsTextureView(index);
    return InteropTextureInfo(ToBackend(textureView));
}

GLuint GetGLInteropBindGroupSampler(WGPUBindGroup group, uint32_t binding) {
    auto* bindGroup = ToBackend(FromAPI(group));
    BindingIndex index;
    if (!ResolveBindingIndex(bindGroup->GetLayout(), binding, &index)) {
        return 0;
    }
    auto* sampler = bindGroup->GetBindingAsSampler(index);
    return ToBackend(sampler)->GetHandle();
}

GLInteropPipelineInfo GetGLInteropRenderPipeline(WGPURenderPipeline pipeline) {
    auto* renderPipeline = ToBackend(FromAPI(pipeline));
    return {.program = renderPipeline->GetProgramHandle(),
            .vertexArray = renderPipeline->GetVertexArrayObject(),
            .topology = renderPipeline->GetGLPrimitiveTopology()};
}

uint32_t GetGLInteropTextureUnits(WGPURenderPipeline pipeline,
                                  uint32_t group,
                                  uint32_t binding,
                                  bool sampler,
                                  uint32_t* units,
                                  uint32_t capacity) {
    auto* renderPipeline = ToBackend(FromAPI(pipeline));
    auto* layout = ToBackend(renderPipeline->GetLayout());
    const BindGroupIndex groupIndex(group);
    if (!layout->GetBindGroupLayoutsMask()[groupIndex]) {
        return 0;
    }
    BindingIndex index;
    if (!ResolveBindingIndex(layout->GetBindGroupLayout(groupIndex), binding, &index)) {
        return 0;
    }
    const auto flat = layout->GetBindingIndexInfo()[groupIndex][index];
    const auto& source = sampler ? renderPipeline->GetTextureUnitsForSampler(flat)
                                 : renderPipeline->GetTextureUnitsForTextureView(flat);
    const uint32_t count = std::min<uint32_t>(capacity, source.size());
    for (uint32_t i = 0; i < count; ++i) {
        units[i] = static_cast<uint32_t>(source[i]);
    }
    return static_cast<uint32_t>(source.size());
}

uint32_t GetGLInteropBufferBinding(WGPURenderPipeline pipeline, uint32_t group, uint32_t binding) {
    auto* renderPipeline = ToBackend(FromAPI(pipeline));
    auto* layout = ToBackend(renderPipeline->GetLayout());
    const BindGroupIndex groupIndex(group);
    if (!layout->GetBindGroupLayoutsMask()[groupIndex]) {
        return UINT32_MAX;
    }
    BindingIndex index;
    if (!ResolveBindingIndex(layout->GetBindGroupLayout(groupIndex), binding, &index)) {
        return UINT32_MAX;
    }
    return static_cast<uint32_t>(layout->GetBindingIndexInfo()[groupIndex][index]);
}

uint32_t GetGLInteropDestroyedTextures(WGPUDevice, GLuint* out, uint32_t maxCount) {
    // No GL calls here: this may be called outside Dawn's scoped-current regions.
    static thread_local std::vector<GLuint> pending;
    if (pending.empty()) {
        pending = GLInteropDrainDestroyedTextures();
    }
    uint32_t n = 0;
    while (n < maxCount && !pending.empty()) {
        out[n++] = pending.back();
        pending.pop_back();
    }
    return n;
}

void SetGLInteropRenderPassCallback(GLInteropRenderPassCallback callback, void* userdata) {
    if (callback == nullptr) {
        gInteropRenderPassCallback.store(nullptr, std::memory_order_release);
        gInteropRenderPassUserdata.store(nullptr, std::memory_order_release);
        return;
    }
    GLInteropSetTrackDestroyedTextures(true);
    gInteropRenderPassUserdata.store(userdata, std::memory_order_relaxed);
    gInteropRenderPassCallback.store(callback, std::memory_order_release);
}

bool TryGLInteropRenderPass(uint32_t passIndex, const char* label) {
    auto callback = gInteropRenderPassCallback.load(std::memory_order_acquire);
    return callback != nullptr &&
           callback(gInteropRenderPassUserdata.load(std::memory_order_relaxed), passIndex, label);
}

}  // namespace dawn::native::opengl
