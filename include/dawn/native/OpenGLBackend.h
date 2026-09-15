// Copyright 2018 The Dawn & Tint Authors
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

#ifndef INCLUDE_DAWN_NATIVE_OPENGLBACKEND_H_
#define INCLUDE_DAWN_NATIVE_OPENGLBACKEND_H_

#include "dawn/native/DawnNative.h"
#include "webgpu/webgpu_cpp_chained_struct.h"

namespace dawn::native::opengl {

using EGLDisplay = void*;
using EGLImage = void*;
using GLuint = unsigned int;
using EGLint = int32_t;
using GLenum = unsigned int;

// Define a GetProc function pointer that mirrors the one in egl.h
#if defined(_WIN32)
#define DAWN_STDCALL __stdcall
#else  // defined(_WIN32)
#define DAWN_STDCALL
#endif  // defined(_WIN32)

using EGLFunctionPointerType = void (*)();
// NOLINTNEXTLINE(readability/casting): cpplint thinks this is a C-style cast but it isn't.
using EGLGetProcProc = EGLFunctionPointerType(DAWN_STDCALL*)(const char*);
#undef DAWN_STDCALL

// Can be chained in WGPURequestAdapterOptions
struct DAWN_NATIVE_EXPORT RequestAdapterOptionsGetGLProc : wgpu::ChainedStruct {
    RequestAdapterOptionsGetGLProc();

    EGLGetProcProc getProc;
    EGLDisplay display;
};

// Can be chained in WGPURequestAdapterOptions
struct DAWN_NATIVE_EXPORT RequestAdapterOptionsAngleVirtualizationGroup : wgpu::ChainedStruct {
    RequestAdapterOptionsAngleVirtualizationGroup();
    EGLint angleVirtualizationGroup = -1;  // EGL_DONT_CARE
};

struct DAWN_NATIVE_EXPORT ExternalImageDescriptorEGLImage : ExternalImageDescriptor {
  public:
    ExternalImageDescriptorEGLImage();

    EGLImage image;
};

DAWN_NATIVE_EXPORT WGPUTexture
WrapExternalEGLImage(WGPUDevice device, const ExternalImageDescriptorEGLImage* descriptor);

struct DAWN_NATIVE_EXPORT ExternalImageDescriptorGLTexture : ExternalImageDescriptor {
  public:
    ExternalImageDescriptorGLTexture();

    GLuint texture;
};

DAWN_NATIVE_EXPORT WGPUTexture
WrapExternalGLTexture(WGPUDevice device, const ExternalImageDescriptorGLTexture* descriptor);

// Native OpenGL interop. An application that keeps Dawn's resource ownership, upload
// scheduling and render pass setup can still issue the GL calls of a render pass itself,
// in the device's GL context, and can look up the GL names behind WebGPU objects to do so.
// Intended for draw-call-bound GLES devices (Mali, Adreno, VideoCore) where the per-draw
// cost of the WebGPU command executor dominates.

// Runs `callback` on the device's GL context with the context current, after every
// previously enqueued GL work of the device. Returns false when the device is lost.
using GLInteropCallback = void (*)(void* userdata);
DAWN_NATIVE_EXPORT bool RunGLInterop(WGPUDevice device, GLInteropCallback callback, void* userdata);

// Application-owned presenter running in a context that shares the device's share group.
// `submit` takes ownership of a GL_TEXTURE_2D name holding the frame (current context's share
// group) and returns true when it will present and later delete it; it must fence the
// device's context before returning. `acquire` (optional) hands back a previously submitted
// texture of this size whose presentation completed, or 0; the swapchain wraps it instead of
// allocating new storage. `shutdown` drains queued presentations before the EGL surface is
// destroyed.
struct GLInteropPresentCallbacks {
    bool (*submit)(GLuint texture, uint32_t width, uint32_t height, void* eglSurface);
    void (*shutdown)();
    GLuint (*acquire)(uint32_t width, uint32_t height);
};
DAWN_NATIVE_EXPORT void SetGLInteropPresentCallbacks(const GLInteropPresentCallbacks* callbacks);
DAWN_NATIVE_EXPORT const GLInteropPresentCallbacks* GetGLInteropPresentCallbacks();

struct DAWN_NATIVE_EXPORT GLInteropTextureInfo {
    GLuint texture;
    GLuint renderbuffer;
    GLenum target;
    uint32_t baseMipLevel;
    uint32_t maxMipLevel;
    GLenum swizzle[4];
};
struct DAWN_NATIVE_EXPORT GLInteropPipelineInfo {
    GLuint program;
    GLuint vertexArray;
    GLenum topology;
};
DAWN_NATIVE_EXPORT GLuint GetGLInteropBuffer(WGPUBuffer buffer);
DAWN_NATIVE_EXPORT GLuint GetGLInteropSampler(WGPUSampler sampler);
DAWN_NATIVE_EXPORT GLInteropTextureInfo GetGLInteropTextureView(WGPUTextureView view);
DAWN_NATIVE_EXPORT GLInteropTextureInfo GetGLInteropBindGroupTexture(WGPUBindGroup group,
                                                                     uint32_t binding);
DAWN_NATIVE_EXPORT GLuint GetGLInteropBindGroupSampler(WGPUBindGroup group, uint32_t binding);
DAWN_NATIVE_EXPORT GLInteropPipelineInfo GetGLInteropRenderPipeline(WGPURenderPipeline pipeline);
// Texture units the pipeline's program samples binding (group, binding) through; `sampler`
// selects the sampler half of a texture/sampler pair. Returns the total count.
DAWN_NATIVE_EXPORT uint32_t GetGLInteropTextureUnits(WGPURenderPipeline pipeline,
                                                     uint32_t group,
                                                     uint32_t binding,
                                                     bool sampler,
                                                     uint32_t* units,
                                                     uint32_t capacity);
// GL uniform/storage block binding index of buffer binding (group, binding), UINT32_MAX if none.
DAWN_NATIVE_EXPORT uint32_t GetGLInteropBufferBinding(WGPURenderPipeline pipeline,
                                                      uint32_t group,
                                                      uint32_t binding);
// Drains the GL texture/renderbuffer names destroyed since the last call (tracked once any
// interop callback has been installed; any thread may destroy, call from one thread). Lets the
// application drop caches keyed by GL name before the name is recycled. No GL calls are made.
DAWN_NATIVE_EXPORT uint32_t GetGLInteropDestroyedTextures(WGPUDevice device,
                                                          GLuint* out,
                                                          uint32_t maxCount);

// When installed, the callback runs after the command executor has bound and cleared a render
// pass's framebuffer and set the default dynamic state. Returning true means the callback
// rendered the pass; its recorded WebGPU commands are then skipped up to EndRenderPass.
using GLInteropRenderPassCallback = bool (*)(void* userdata, uint32_t passIndex, const char* label);
DAWN_NATIVE_EXPORT void SetGLInteropRenderPassCallback(GLInteropRenderPassCallback callback,
                                                       void* userdata);
DAWN_NATIVE_EXPORT bool TryGLInteropRenderPass(uint32_t passIndex, const char* label);

}  // namespace dawn::native::opengl

#endif  // INCLUDE_DAWN_NATIVE_OPENGLBACKEND_H_
