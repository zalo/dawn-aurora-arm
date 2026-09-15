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

#include "src/dawn/native/opengl/SwapChainEGL.h"

#include <utility>

#include "dawn/native/OpenGLBackend.h"
#include "src/dawn/native/ChainUtils.h"
#include "src/dawn/native/Surface.h"
#include "src/dawn/native/opengl/ContextEGL.h"
#include "src/dawn/native/opengl/DeviceGL.h"
#include "src/dawn/native/opengl/DisplayEGL.h"
#include "src/dawn/native/opengl/PhysicalDeviceGL.h"
#include "src/dawn/native/opengl/TextureGL.h"
#include "src/dawn/native/opengl/UtilsEGL.h"
#include "src/dawn/native/opengl/UtilsGL.h"

namespace dawn::native::opengl {

// static
ResultOrError<Ref<SwapChainEGL>> SwapChainEGL::Create(Device* device,
                                                      Surface* surface,
                                                      SwapChainBase* previousSwapChain,
                                                      const SurfaceConfiguration* config) {
    Ref<SwapChainEGL> swapchain = AcquireRef(new SwapChainEGL(device, surface, config));
    DAWN_TRY(swapchain->Initialize(previousSwapChain));
    return swapchain;
}

SwapChainEGL::SwapChainEGL(DeviceBase* dev, Surface* sur, const SurfaceConfiguration* config)
    : SwapChainBase(dev, sur, config) {}

SwapChainEGL::~SwapChainEGL() = default;

MaybeError SwapChainEGL::Initialize(SwapChainBase* previousSwapChain) {
    const Device* device = ToBackend(GetDevice());

    if (previousSwapChain != nullptr) {
        // TODO(crbug.com/dawn/269): figure out what should happen when surfaces are used by
        // multiple backends one after the other. It probably needs to block until the backend
        // and GPU are completely finished with the previous swapchain.
        DAWN_INVALID_IF(previousSwapChain->GetBackendType() != GetBackendType(),
                        "OpenGL SwapChain cannot switch backend types from %s to %s.",
                        previousSwapChain->GetBackendType(), GetBackendType());

        // TODO(crbug.com/dawn/269): figure out what should happen when surfaces are used by
        // a different EGL display. We probably need to block until the GPU is completely
        // finished with the previous work, and then a bit more.
        DAWN_INVALID_IF(
            previousSwapChain->GetDevice()->GetPhysicalDevice() != device->GetPhysicalDevice(),
            "OpenGL SwapChain cannot switch between contexts for %s and %s.",
            previousSwapChain->GetDevice(), device);

        // The EGLSurface created depends on the format; only reuse it if we have the same one.
        if (previousSwapChain->GetFormat() == GetFormat()) {
            SwapChainEGL* previousEGLSwapChain =
                reinterpret_cast<SwapChainEGL*>(ToBackend(previousSwapChain));
            std::swap(previousEGLSwapChain->mEGLSurface, mEGLSurface);

            // The GL textures behind the swapchain textures can be reused too when the textures
            // they will back are identical. They belong to the previous swapchain's GL context,
            // so the device must be the same. The one a current texture may be rendering into is
            // out of its slot and stays with that texture.
            if (previousSwapChain->GetDevice() == GetDevice() &&
                previousSwapChain->GetWidth() == GetWidth() &&
                previousSwapChain->GetHeight() == GetHeight() &&
                previousSwapChain->GetUsage() == GetUsage() &&
                previousSwapChain->GetViewFormats() == GetViewFormats()) {
                std::swap(previousEGLSwapChain->mBackingTextures, mBackingTextures);
                mNextBackingTexture = previousEGLSwapChain->mNextBackingTexture;
            }
        }

        previousSwapChain->DetachFromSurface();
    }

    const DisplayEGL* display = ToBackend(device->GetPhysicalDevice())->GetDisplay();

    // Create the EGLSurface if needed.
    if (mEGLSurface == EGL_NO_SURFACE) {
        DAWN_TRY(CreateEGLSurface(display));
    }

    EGLint swapInterval = GetPresentMode() == wgpu::PresentMode::Immediate ? 0 : 1;
    display->egl->SwapInterval(display->GetDisplay(), swapInterval);

    return {};
}

MaybeError SwapChainEGL::PresentImpl() {
    Device* device = ToBackend(GetDevice());
    EGLDisplay display = device->GetEGLDisplay();
    const EGLFunctions& egl = device->GetEGL(false);

    if (const auto* presenter = GetGLInteropPresentCallbacks()) {
        // The presenter fences this context and assumes ownership of the GL name. The WebGPU
        // surface texture is still destroyed at Present(), while the native name lives until
        // the presenter's context has finished with it (and may come back through acquire()).
        ContextEGL::ScopedMakeCurrent current;
        DAWN_TRY_ASSIGN(current, device->GetContext()->MakeCurrent());
        const bool submitted =
            presenter->submit(mTexture->GetTextureHandle(), mTexture->GetWidth(Aspect::Color),
                              mTexture->GetHeight(Aspect::Color), mEGLSurface);
        DAWN_TRY(current.End());
        if (submitted) {
            mTexture->DetachHandle();
            mTexture->APIDestroy();
            mTexture = nullptr;
            mTextureView = nullptr;
            return {};
        }
    }

    // Do the reverse-Y blit from the fake surface texture to the default framebuffer.
    {
        auto surfaceCurrent = device->GetContext()->SetCurrentSurfaceScope(mEGLSurface);
        ContextEGL::ScopedMakeCurrent scopedCurrentContext;
        DAWN_TRY_ASSIGN(scopedCurrentContext, device->GetContext()->MakeCurrent());

        EGLint surfaceWidth;
        EGLint surfaceHeight;
        DAWN_TRY(CheckEGL(egl, egl.QuerySurface(display, mEGLSurface, EGL_WIDTH, &surfaceWidth),
                          "getting surface width"));
        DAWN_TRY(CheckEGL(egl, egl.QuerySurface(display, mEGLSurface, EGL_HEIGHT, &surfaceHeight),
                          "getting surface height"));

        const OpenGLFunctions& gl = device->GetGL(/*makeCurrent=*/false);

        GLuint readFbo = 0;
        DAWN_GL_TRY(gl, GenFramebuffers(1, &readFbo));
        DAWN_GL_TRY(gl, BindFramebuffer(GL_READ_FRAMEBUFFER, readFbo));
        DAWN_TRY(mTextureView->BindToFramebuffer(gl, GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0));

        DAWN_GL_TRY(gl, BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0));
        DAWN_GL_TRY(gl, Scissor(0, 0, surfaceWidth, surfaceHeight));
        DAWN_GL_TRY(gl, BlitFramebuffer(0, 0, mTexture->GetWidth(Aspect::Color),
                                        mTexture->GetHeight(Aspect::Color), 0, surfaceHeight,
                                        surfaceWidth, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR));

        DAWN_GL_TRY(gl, DeleteFramebuffers(1, &readFbo));

        egl.SwapBuffers(display, mEGLSurface);
        DAWN_TRY(scopedCurrentContext.End());
    }

    // WebGPU requires the texture to be destroyed by Present, but its GL storage goes back into
    // its slot of the ring for a later frame's texture (see GetCurrentTextureImpl).
    DAWN_ASSERT(mBackingTextures[mCurrentBackingTexture] == 0);
    mBackingTextures[mCurrentBackingTexture] = mTexture->DetachHandle();
    mTexture->APIDestroy();
    mTexture = nullptr;
    mTextureView = nullptr;

    return {};
}

ResultOrError<SwapChainTextureInfo> SwapChainEGL::GetCurrentTextureImpl() {
    // Create the fake surface texture that we'll blit from. A new Texture is required every frame
    // (it starts uninitialized and is destroyed by Present, as WebGPU requires), but the GL
    // texture a previous frame's texture was presented from is wrapped again instead of
    // allocating new storage: that saves the allocation and keeps the framebuffers cached against
    // it valid. The storages rotate through a small ring (see mBackingTextures) so that the
    // texture rendered into now is not the one whose present blit may still be in flight.
    TextureDescriptor desc = GetSwapChainBaseTextureDescriptor(this);
    Ref<TextureBase> texture;
    Ref<TextureViewBase> view;
    mCurrentBackingTexture = mNextBackingTexture;
    mNextBackingTexture = (mNextBackingTexture + 1) % kBackingTextureCount;
    GLuint& backingTexture = mBackingTextures[mCurrentBackingTexture];
    // With an interop presenter the presented names live in its pool instead of the ring: a
    // texture whose presentation completed is wrapped again rather than allocated anew.
    GLuint recycled = 0;
    if (const auto* presenter = GetGLInteropPresentCallbacks();
        presenter != nullptr && presenter->acquire != nullptr) {
        recycled = presenter->acquire(desc.size.width, desc.size.height);
    }
    if (recycled != 0) {
        texture = AcquireRef(
            new Texture(ToBackend(GetDevice()), Unpack(&desc), recycled, OwnsHandle::Yes));
    } else if (backingTexture != 0) {
        texture = AcquireRef(
            new Texture(ToBackend(GetDevice()), Unpack(&desc), backingTexture, OwnsHandle::Yes));
        backingTexture = 0;
    } else {
        DAWN_TRY_ASSIGN(texture, GetDevice()->CreateTexture(&desc));
    }
    DAWN_TRY_ASSIGN(view, GetDevice()->CreateTextureView(texture.Get()));

    mTexture = std::move(ToBackend(texture));
    mTextureView = std::move(ToBackend(view));

    SwapChainTextureInfo info;
    info.texture = mTexture;
    // TODO(dawn:2320): Check for optimality
    info.status = wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal;
    return info;
}

void SwapChainEGL::DetachFromSurfaceImpl() {
    if (const auto* presenter = GetGLInteropPresentCallbacks()) {
        // Queued presentations reference the EGL surface destroyed below.
        presenter->shutdown();
    }
    if (mEGLSurface != EGL_NO_SURFACE) {
        Device* device = ToBackend(GetDevice());
        device->GetEGL(false).DestroySurface(device->GetEGLDisplay(), mEGLSurface);
        mEGLSurface = EGL_NO_SURFACE;
    }

    if (mTexture != nullptr) {
        // The current texture owns its GL texture and deletes it.
        DAWN_ASSERT(mBackingTextures[mCurrentBackingTexture] == 0);
        mTexture->APIDestroy();
        mTexture = nullptr;
        mTextureView = nullptr;
    }

    Device* device = ToBackend(GetDevice());
    for (GLuint& backingTexture : mBackingTextures) {
        if (backingTexture == 0) {
            continue;
        }
        IgnoreErrors(device->EnqueueGL(
            [texture = backingTexture](const OpenGLFunctions& gl) -> MaybeError {
                DAWN_GL_TRY_IGNORE_ERRORS(gl, DeleteTextures(1, &texture));
                return {};
            }));
        backingTexture = 0;
    }
}

MaybeError SwapChainEGL::CreateEGLSurface(const DisplayEGL* display) {
    DAWN_ASSERT(mEGLSurface == EGL_NO_SURFACE);

    EGLConfig config = display->ChooseConfig(EGL_WINDOW_BIT, GetFormat());
    if (config == kNoConfig) {
        return DAWN_FORMAT_INTERNAL_ERROR("Couldn't find an EGLConfig for %s on %s.", GetFormat(),
                                          GetSurface());
    }

    // [[maybe_unused]] to prevent unused variable warnings when platform code is disabled.
    [[maybe_unused]] const EGLFunctions& egl = display->egl.get();
    [[maybe_unused]] EGLDisplay eglDisplay = display->GetDisplay();
    Surface* surface = GetSurface();

    absl::InlinedVector<EGLint, 3> attribs;
    auto AddAttrib = [&](EGLint attrib, EGLint value) {
        attribs.push_back(attrib);
        attribs.push_back(value);
    };

    if (GetFormat() == wgpu::TextureFormat::RGBA8UnormSrgb) {
        DAWN_ASSERT(egl.HasExt(EGLExt::GLColorspace));
        AddAttrib(EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_SRGB_KHR);
    }

    attribs.push_back(EGL_NONE);

    // Note that we cannot use ResultOrError<EGLSurface> as it might not have the required alignment
    // constraints.
    auto TryCreateSurface = [&]() -> MaybeError {
        switch (surface->GetType()) {
            case Surface::Type::EGLNativeWindow:
                // The application hands over an EGLNativeWindowType for the display's platform
                // (a gbm_surface on DRM/GBM, for instance) with no window system in between.
                mEGLSurface = egl.CreateWindowSurface(
                    eglDisplay, config,
                    reinterpret_cast<EGLNativeWindowType>(surface->GetEGLNativeWindow()),
                    attribs.data());
                return {};
#if DAWN_PLATFORM_IS(ANDROID)
            case Surface::Type::AndroidWindow:
                mEGLSurface = egl.CreateWindowSurface(
                    eglDisplay, config,
                    static_cast<ANativeWindow*>(surface->GetAndroidNativeWindow()), attribs.data());
                return {};
#endif  // DAWN_PLATFORM_IS(ANDROID)
#if defined(DAWN_ENABLE_BACKEND_METAL)
            case Surface::Type::MetalLayer:
                mEGLSurface = egl.CreateWindowSurface(eglDisplay, config, surface->GetMetalLayer(),
                                                      attribs.data());
                return {};
#endif  // defined(DAWN_ENABLE_BACKEND_METAL)
#if DAWN_PLATFORM_IS(WIN32)
            case Surface::Type::WindowsHWND:
                mEGLSurface = egl.CreateWindowSurface(
                    eglDisplay, config, static_cast<HWND>(surface->GetHWND()), attribs.data());
                return {};
#endif  // DAWN_PLATFORM_IS(WIN32)
#if defined(DAWN_USE_X11)
            case Surface::Type::XlibWindow:
                mEGLSurface = egl.CreateWindowSurface(
                    eglDisplay, config, uint32_t(surface->GetXWindow()), attribs.data());
                return {};
#endif  // defined(DAWN_USE_X11)

            // TODO(344814083): Add support for creating surfaces using EGL_KHR_platform_base and
            // friends.
            case Surface::Type::WaylandSurface:

            default:
                return DAWN_FORMAT_INTERNAL_ERROR("%s cannot be supported on EGL.", surface);
        }
    };

    DAWN_TRY(TryCreateSurface());
    if (mEGLSurface == EGL_NO_SURFACE) {
        return DAWN_FORMAT_INTERNAL_ERROR("Couldn't create an EGLSurface for %s.", surface);
    }
    return {};
}

}  // namespace dawn::native::opengl
