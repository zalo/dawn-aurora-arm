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

#ifndef SRC_DAWN_NATIVE_OPENGL_FRAMEBUFFERCACHEGL_H_
#define SRC_DAWN_NATIVE_OPENGL_FRAMEBUFFERCACHEGL_H_

#include <cstddef>
#include <list>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "src/dawn/common/ityp_array.h"
#include "src/dawn/native/IntegerTypes.h"
#include "src/dawn/native/opengl/opengl_platform.h"

namespace dawn::native::opengl {

struct OpenGLFunctions;

// Identifies what a framebuffer attachment point is bound to: one level and layer of a GL
// texture, or a renderbuffer (target == GL_RENDERBUFFER, level and layer unused).
struct FramebufferAttachment {
    GLuint handle = 0;
    GLenum target = 0;
    GLuint level = 0;
    GLuint layer = 0;

    bool operator==(const FramebufferAttachment&) const = default;
};

// Everything that determines the state of the framebuffer object a render pass draws into.
struct FramebufferKey {
    // Unused color attachments have a zero handle. The draw buffers follow from which are used.
    PerColorAttachment<FramebufferAttachment> colors = {};
    FramebufferAttachment depthStencil = {};
    // GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT or GL_DEPTH_STENCIL_ATTACHMENT (0 if unused).
    GLenum depthStencilAttachmentPoint = 0;
    // The render pass's sample count. It is part of the attachment state when it differs from the
    // texture's (glFramebufferTexture2DMultisampleEXT).
    uint32_t sampleCount = 1;

    bool operator==(const FramebufferKey&) const = default;

    template <typename H>
    friend H AbslHashValue(H h, const FramebufferKey& key) {
        for (const FramebufferAttachment& color : key.colors) {
            h = H::combine(std::move(h), color.handle, color.target, color.level, color.layer);
        }
        return H::combine(std::move(h), key.depthStencil.handle, key.depthStencil.target,
                          key.depthStencil.level, key.depthStencil.layer,
                          key.depthStencilAttachmentPoint, key.sampleCount);
    }
};

// Caches complete framebuffer objects by attachment set so that render passes drawing into the
// same attachments reuse one framebuffer instead of building and validating a new one every time.
// Framebuffer completeness validation is expensive on tile-based mobile drivers, and most
// applications render into the same handful of attachment sets every frame.
//
// The cache belongs to the GL Device and is only touched while the device's GL context is
// current (render pass execution, deferred GL object deletion, device destruction), so it needs no
// locking of its own. Framebuffer objects are not shared between contexts, so they are always
// created and deleted in the same one.
//
// An entry must not outlive the GL objects it references: a framebuffer whose attachment was
// deleted keeps rendering into the orphaned storage, and a new object under the same name would
// silently produce a stale hit. Every place that deletes or stops tracking a texture or
// renderbuffer removes the entries referencing it (see Texture::DestroyImpl).
class FramebufferCache {
  public:
    // Insertion order is kept and the oldest entry is evicted beyond this many. Real workloads use
    // far fewer distinct attachment sets per frame; the cap only bounds unusual ones.
    static constexpr size_t kMaxEntries = 256;

    FramebufferCache();
    ~FramebufferCache();

    FramebufferCache(const FramebufferCache&) = delete;
    FramebufferCache& operator=(const FramebufferCache&) = delete;

    // Returns the cached framebuffer for the key, or 0 if there is none.
    GLuint Find(const FramebufferKey& key) const;

    // Takes ownership of a complete framebuffer built for the key. Evicts the oldest entry when
    // the cache is full.
    void Insert(const OpenGLFunctions& gl, const FramebufferKey& key, GLuint framebuffer);

    // Deletes every cached framebuffer that references the texture or renderbuffer.
    void RemoveTexture(const OpenGLFunctions& gl, GLuint texture);
    void RemoveRenderbuffer(const OpenGLFunctions& gl, GLuint renderbuffer);

    // Deletes every cached framebuffer.
    void Clear(const OpenGLFunctions& gl);

  private:
    struct Entry {
        FramebufferKey key;
        GLuint framebuffer;
    };
    using EntryList = std::list<Entry>;

    void RemoveIf(const OpenGLFunctions& gl, GLuint handle, bool isRenderbuffer);
    void Erase(const OpenGLFunctions& gl, EntryList::iterator it);

    // Oldest entry first.
    EntryList mEntries;
    absl::flat_hash_map<FramebufferKey, EntryList::iterator> mIndex;
};

}  // namespace dawn::native::opengl

#endif  // SRC_DAWN_NATIVE_OPENGL_FRAMEBUFFERCACHEGL_H_
