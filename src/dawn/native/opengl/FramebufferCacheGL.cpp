// Copyright 2026 The Dawn & Tint Authors
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

#include "src/dawn/native/opengl/FramebufferCacheGL.h"

#include <iterator>

#include "src/dawn/native/opengl/OpenGLFunctions.h"
#include "src/dawn/native/opengl/UtilsGL.h"
#include "src/utils/assert.h"

namespace dawn::native::opengl {

namespace {

bool References(const FramebufferAttachment& attachment, GLuint handle, bool isRenderbuffer) {
    // Texture and renderbuffer names live in separate namespaces.
    return attachment.handle == handle && (attachment.target == GL_RENDERBUFFER) == isRenderbuffer;
}

bool References(const FramebufferKey& key, GLuint handle, bool isRenderbuffer) {
    for (const FramebufferAttachment& color : key.colors) {
        if (References(color, handle, isRenderbuffer)) {
            return true;
        }
    }
    return References(key.depthStencil, handle, isRenderbuffer);
}

}  // namespace

FramebufferCache::FramebufferCache() = default;

FramebufferCache::~FramebufferCache() = default;

GLuint FramebufferCache::Find(const FramebufferKey& key) const {
    auto it = mIndex.find(key);
    return it == mIndex.end() ? 0 : it->second->framebuffer;
}

void FramebufferCache::Insert(const OpenGLFunctions& gl,
                              const FramebufferKey& key,
                              GLuint framebuffer) {
    DAWN_ASSERT(framebuffer != 0);
    DAWN_ASSERT(!mIndex.contains(key));

    while (mEntries.size() >= kMaxEntries) {
        Erase(gl, mEntries.begin());
    }

    mEntries.push_back({key, framebuffer});
    mIndex.emplace(key, std::prev(mEntries.end()));
}

void FramebufferCache::RemoveTexture(const OpenGLFunctions& gl, GLuint texture) {
    RemoveIf(gl, texture, /*isRenderbuffer=*/false);
}

void FramebufferCache::RemoveRenderbuffer(const OpenGLFunctions& gl, GLuint renderbuffer) {
    RemoveIf(gl, renderbuffer, /*isRenderbuffer=*/true);
}

void FramebufferCache::Clear(const OpenGLFunctions& gl) {
    while (!mEntries.empty()) {
        Erase(gl, mEntries.begin());
    }
}

void FramebufferCache::RemoveIf(const OpenGLFunctions& gl, GLuint handle, bool isRenderbuffer) {
    if (handle == 0) {
        return;
    }
    for (auto it = mEntries.begin(); it != mEntries.end();) {
        auto next = std::next(it);
        if (References(it->key, handle, isRenderbuffer)) {
            Erase(gl, it);
        }
        it = next;
    }
}

void FramebufferCache::Erase(const OpenGLFunctions& gl, EntryList::iterator it) {
    // Deletion also unbinds the framebuffer from the current context if it is bound there.
    DAWN_GL_TRY_IGNORE_ERRORS(gl, DeleteFramebuffers(1, &it->framebuffer));
    mIndex.erase(it->key);
    mEntries.erase(it);
}

}  // namespace dawn::native::opengl
