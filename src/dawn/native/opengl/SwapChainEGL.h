// Copyright 2024 The Dawn & Tint Authors
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

#ifndef SRC_DAWN_NATIVE_OPENGL_SWAPCHAINEGL_H_
#define SRC_DAWN_NATIVE_OPENGL_SWAPCHAINEGL_H_

#include <array>
#include <cstddef>

#include "src/dawn/common/egl_platform.h"
#include "src/dawn/native/SwapChain.h"
#include "src/dawn/native/opengl/opengl_platform.h"

namespace dawn::native::opengl {

class Device;
class DisplayEGL;
class Texture;
class TextureView;

class SwapChainEGL final : public SwapChainBase {
  public:
    static ResultOrError<Ref<SwapChainEGL>> Create(Device* device,
                                                   Surface* surface,
                                                   SwapChainBase* previousSwapChain,
                                                   const SurfaceConfiguration* config);

    SwapChainEGL(DeviceBase* device, Surface* surface, const SurfaceConfiguration* config);
    ~SwapChainEGL() override;

  private:
    using SwapChainBase::SwapChainBase;
    MaybeError Initialize(SwapChainBase* previousSwapChain);

    MaybeError CreateEGLSurface(const DisplayEGL* display);

    EGLSurface mEGLSurface = EGL_NO_SURFACE;

    // WebGPU requires a new texture from every GetCurrentTexture, and Present destroys it, but the
    // GL texture behind it is detached first and kept here to be wrapped by a later frame's
    // texture. A ring of storages, not one: the blit that presents texture N may still be
    // executing on the GPU when the application renders into frame N+1's texture, and reusing a
    // single storage makes every frame wait for the previous present (measured as a 3x longer
    // wait for the display and a net slowdown on a Mali-G52 handheld). With three, a storage is
    // only reused two presents later. Storages are allocated on first use, taken out of their slot
    // (owned by the current Texture) while a frame renders into them, and deleted with their
    // cached framebuffers on detach.
    static constexpr size_t kBackingTextureCount = 3;
    std::array<GLuint, kBackingTextureCount> mBackingTextures = {};
    // The slot the next GetCurrentTexture takes from, and the one the current texture came from.
    size_t mNextBackingTexture = 0;
    size_t mCurrentBackingTexture = 0;
    Ref<Texture> mTexture;
    Ref<TextureView> mTextureView;

    MaybeError PresentImpl() override;
    ResultOrError<SwapChainTextureInfo> GetCurrentTextureImpl() override;
    void DetachFromSurfaceImpl() override;
};

}  // namespace dawn::native::opengl

#endif  // SRC_DAWN_NATIVE_OPENGL_SWAPCHAINEGL_H_
