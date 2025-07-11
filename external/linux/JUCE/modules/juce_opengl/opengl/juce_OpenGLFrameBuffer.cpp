/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

class OpenGLFrameBuffer::Pimpl
{
public:
    /*  Stores the currently-bound texture on construction, and re-binds it on destruction. */
    struct ScopedTextureBinding
    {
        ScopedTextureBinding()
        {
            glGetIntegerv (GL_TEXTURE_BINDING_2D, &prev);
            JUCE_CHECK_OPENGL_ERROR
        }

        ~ScopedTextureBinding()
        {
            glBindTexture (GL_TEXTURE_2D, (GLuint) prev);
            JUCE_CHECK_OPENGL_ERROR
        }

        GLint prev{};
    };

    Pimpl (OpenGLContext& c, const int w, const int h,
           const bool wantsDepthBuffer, const bool wantsStencilBuffer)
        : context (c), width (w), height (h),
          textureID (0), frameBufferID (0), depthOrStencilBuffer (0),
          hasDepthBuffer (false), hasStencilBuffer (false)
    {
        // Framebuffer objects can only be created when the current thread has an active OpenGL
        // context. You'll need to create this object in one of the OpenGLContext's callbacks.
        jassert (OpenGLHelpers::isContextActive());

       #if JUCE_WINDOWS || JUCE_LINUX || JUCE_BSD
        if (context.extensions.glGenFramebuffers == nullptr)
            return;
       #endif

        context.extensions.glGenFramebuffers (1, &frameBufferID);
        bind();

        {
            const ScopedTextureBinding scopedTextureBinding;

            glGenTextures (1, &textureID);
            glBindTexture (GL_TEXTURE_2D, textureID);
            JUCE_CHECK_OPENGL_ERROR

            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            JUCE_CHECK_OPENGL_ERROR

            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            JUCE_CHECK_OPENGL_ERROR
        }

        context.extensions.glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureID, 0);

        if (wantsDepthBuffer || wantsStencilBuffer)
        {
            context.extensions.glGenRenderbuffers (1, &depthOrStencilBuffer);
            context.extensions.glBindRenderbuffer (GL_RENDERBUFFER, depthOrStencilBuffer);
            jassert (context.extensions.glIsRenderbuffer (depthOrStencilBuffer));

            context.extensions.glRenderbufferStorage (GL_RENDERBUFFER,
                                      (wantsDepthBuffer && wantsStencilBuffer) ? (GLenum) GL_DEPTH24_STENCIL8
                                                                              #if JUCE_OPENGL_ES
                                                                               : (GLenum) GL_DEPTH_COMPONENT16,
                                                                              #else
                                                                               : (GLenum) GL_DEPTH_COMPONENT,
                                                                              #endif
                                      width, height);

            GLint params = 0;
            context.extensions.glGetRenderbufferParameteriv (GL_RENDERBUFFER, GL_RENDERBUFFER_DEPTH_SIZE, &params);
            context.extensions.glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthOrStencilBuffer);

            if (wantsStencilBuffer)
                context.extensions.glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthOrStencilBuffer);

            hasDepthBuffer = wantsDepthBuffer;
            hasStencilBuffer = wantsStencilBuffer;
        }

        unbind();
    }

    ~Pimpl()
    {
        if (OpenGLHelpers::isContextActive())
        {
            if (textureID != 0)
                glDeleteTextures (1, &textureID);

            if (depthOrStencilBuffer != 0)
                context.extensions.glDeleteRenderbuffers (1, &depthOrStencilBuffer);

            if (frameBufferID != 0)
                context.extensions.glDeleteFramebuffers (1, &frameBufferID);

            JUCE_CHECK_OPENGL_ERROR
        }
    }

    bool createdOk() const
    {
        return frameBufferID != 0 && textureID != 0;
    }

    void bind()
    {
        glGetIntegerv (GL_FRAMEBUFFER_BINDING, &prevFramebuffer);
        context.extensions.glBindFramebuffer (GL_FRAMEBUFFER, frameBufferID);
        JUCE_CHECK_OPENGL_ERROR
    }

    void unbind()
    {
        context.extensions.glBindFramebuffer (GL_FRAMEBUFFER, (GLuint) prevFramebuffer);
        JUCE_CHECK_OPENGL_ERROR
    }

    OpenGLContext& context;
    const int width, height;
    GLuint textureID, frameBufferID, depthOrStencilBuffer;
    bool hasDepthBuffer, hasStencilBuffer;

private:
    GLint prevFramebuffer{};

    bool checkStatus() noexcept
    {
        const GLenum status = context.extensions.glCheckFramebufferStatus (GL_FRAMEBUFFER);

        return status == GL_NO_ERROR
            || status == GL_FRAMEBUFFER_COMPLETE;
    }

    JUCE_DECLARE_NON_COPYABLE (Pimpl)
};

//==============================================================================
class OpenGLFrameBuffer::SavedState
{
public:
    SavedState (OpenGLFrameBuffer& buffer, const int w, const int h)
        : width (w), height (h),
          data ((size_t) (w * h))
    {
        buffer.readPixels (data, Rectangle<int> (w, h));
    }

    bool restore (OpenGLContext& context, OpenGLFrameBuffer& buffer)
    {
        if (buffer.initialise (context, width, height))
        {
            buffer.writePixels (data, Rectangle<int> (width, height));
            return true;
        }

        return false;
    }

private:
    const int width, height;
    HeapBlock<PixelARGB> data;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SavedState)
};

//==============================================================================
OpenGLFrameBuffer::OpenGLFrameBuffer() {}
OpenGLFrameBuffer::~OpenGLFrameBuffer() {}

bool OpenGLFrameBuffer::initialise (OpenGLContext& context, int width, int height)
{
    jassert (context.isActive()); // The context must be active when creating a framebuffer!

    pimpl.reset();
    pimpl.reset (new Pimpl (context, width, height, false, false));

    if (! pimpl->createdOk())
        pimpl.reset();

    return pimpl != nullptr;
}

bool OpenGLFrameBuffer::initialise (OpenGLContext& context, const Image& image)
{
    if (! image.isARGB())
        return initialise (context, image.convertedToFormat (Image::ARGB));

    Image::BitmapData bitmap (image, Image::BitmapData::readOnly);

    return initialise (context, bitmap.width, bitmap.height)
            && writePixels ((const PixelARGB*) bitmap.data, image.getBounds());
}

bool OpenGLFrameBuffer::initialise (OpenGLFrameBuffer& other)
{
    auto* p = other.pimpl.get();

    if (p == nullptr)
    {
        pimpl.reset();
        return true;
    }

    const Rectangle<int> area (pimpl->width, pimpl->height);

    if (initialise (p->context, area.getWidth(), area.getHeight()))
    {
        pimpl->bind();

       #if ! JUCE_ANDROID
        if (! pimpl->context.isCoreProfile())
            glEnable (GL_TEXTURE_2D);

        clearGLError();
       #endif
        {
            const Pimpl::ScopedTextureBinding scopedTextureBinding;
            glBindTexture (GL_TEXTURE_2D, p->textureID);
            pimpl->context.copyTexture (area, area, area.getWidth(), area.getHeight(), false);
        }

        pimpl->unbind();
        return true;
    }

    return false;
}

void OpenGLFrameBuffer::release()
{
    pimpl.reset();
    savedState.reset();
}

void OpenGLFrameBuffer::saveAndRelease()
{
    if (pimpl != nullptr)
    {
        savedState.reset (new SavedState (*this, pimpl->width, pimpl->height));
        pimpl.reset();
    }
}

bool OpenGLFrameBuffer::reloadSavedCopy (OpenGLContext& context)
{
    if (savedState != nullptr)
    {
        std::unique_ptr<SavedState> state;
        std::swap (state, savedState);

        if (state->restore (context, *this))
            return true;

        std::swap (state, savedState);
    }

    return false;
}

int OpenGLFrameBuffer::getWidth() const noexcept            { return pimpl != nullptr ? pimpl->width : 0; }
int OpenGLFrameBuffer::getHeight() const noexcept           { return pimpl != nullptr ? pimpl->height : 0; }
GLuint OpenGLFrameBuffer::getTextureID() const noexcept     { return pimpl != nullptr ? pimpl->textureID : 0; }

bool OpenGLFrameBuffer::makeCurrentRenderingTarget()
{
    // trying to use a framebuffer after saving it with saveAndRelease()! Be sure to call
    // reloadSavedCopy() to put it back into GPU memory before using it..
    jassert (savedState == nullptr);

    if (pimpl == nullptr)
        return false;

    pimpl->bind();
    return true;
}

GLuint OpenGLFrameBuffer::getFrameBufferID() const noexcept
{
    return pimpl != nullptr ? pimpl->frameBufferID : 0;
}

GLuint OpenGLFrameBuffer::getCurrentFrameBufferTarget() noexcept
{
    GLint fb = {};
    glGetIntegerv (GL_FRAMEBUFFER_BINDING, &fb);
    return (GLuint) fb;
}

void OpenGLFrameBuffer::releaseAsRenderingTarget()
{
    if (pimpl != nullptr)
        pimpl->unbind();
}

void OpenGLFrameBuffer::clear (Colour colour)
{
    if (makeCurrentRenderingTarget())
    {
        OpenGLHelpers::clear (colour);
        releaseAsRenderingTarget();
    }
}

void OpenGLFrameBuffer::makeCurrentAndClear()
{
    if (makeCurrentRenderingTarget())
    {
        glClearColor (0, 0, 0, 0);
        glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
}

bool OpenGLFrameBuffer::readPixels (PixelARGB* target, const Rectangle<int>& area)
{
    if (! makeCurrentRenderingTarget())
        return false;

    glPixelStorei (GL_PACK_ALIGNMENT, 4);
    glReadPixels (area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                  JUCE_RGBA_FORMAT, GL_UNSIGNED_BYTE, target);

    pimpl->unbind();
    return true;
}

bool OpenGLFrameBuffer::writePixels (const PixelARGB* data, const Rectangle<int>& area)
{
    OpenGLTargetSaver ts (pimpl->context);

    if (! makeCurrentRenderingTarget())
        return false;

    glDisable (GL_DEPTH_TEST);
    glDisable (GL_BLEND);
    JUCE_CHECK_OPENGL_ERROR

    OpenGLTexture tex;
    tex.loadARGB (data, area.getWidth(), area.getHeight());

    glViewport (0, 0, pimpl->width, pimpl->height);
    pimpl->context.copyTexture (area, Rectangle<int> (area.getX(), area.getY(),
                                                      tex.getWidth(), tex.getHeight()),
                                pimpl->width, pimpl->height, true, false);

    JUCE_CHECK_OPENGL_ERROR
    return true;
}

} // namespace juce
