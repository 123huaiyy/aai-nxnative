#include <OpenGLES/ES3/gl.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>
#include <map>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "shader_es.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/user32_window.h"

namespace recomp::hle {
uint32_t ActiveGlesWindowDc();

namespace {

constexpr char kGl[] = "opengl32.dll";
thread_local GLuint default_framebuffer = 0;
thread_local GLuint game_framebuffer = 0;
thread_local SDL_Window* game_drawable_window = nullptr;
thread_local SDL_Point game_drawable_size{};
thread_local std::map<GLuint, std::vector<darkest::UniformDefault>> shader_defaults;
thread_local std::map<uint64_t, gaddr> driver_strings;

struct BufferMapping {
    void* host;
    gaddr guest;
    size_t length;
    GLbitfield access;
};

thread_local std::map<GLuint, BufferMapping> mapped_buffers;
thread_local gaddr streaming_staging = 0;
thread_local size_t streaming_capacity = 0;
thread_local bool streaming_in_use = false;

SDL_Point WindowPixels(SDL_Window* window) {
    SDL_Point size{};
    SDL_GetWindowSizeInPixels(window, &size.x, &size.y);
    return size;
}

SDL_Point PreferredGameSize(SDL_Window* window, SDL_Point pixels) {
    if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) || pixels.x <= 0 ||
        pixels.y <= 0) return pixels;
    // UIKit may report the old orientation until it shows the fullscreen view.
    // Darkest launches in landscape, so size its guest framebuffer accordingly.
    if (pixels.x < pixels.y) std::swap(pixels.x, pixels.y);
    constexpr int max_render_width = 1960;
    // Use the ultrawide viewport on phone-shaped displays. Preserve the
    // display's aspect on iPads and round their resolution to whole pixels.
    if (pixels.x * 10 >= pixels.y * 19) {
        const int units = std::min({pixels.x / 7, pixels.y / 3, max_render_width / 7});
        if (units) return {units * 7, units * 3};
    }
    if (pixels.x <= max_render_width) return pixels;
    return {max_render_width, std::max(1, int(std::lround(
                                 double(pixels.y) * max_render_width / pixels.x)))};
}

SDL_Rect FitGameRect(SDL_Point game, SDL_Point window) {
    if (game.x <= 0 || game.y <= 0 || window.x <= 0 || window.y <= 0) return {};
    const auto scale = std::min(double(window.x) / game.x, double(window.y) / game.y);
    const int width = std::min(window.x, int(std::lround(game.x * scale)));
    const int height = std::min(window.y, int(std::lround(game.y * scale)));
    return {(window.x - width) / 2, (window.y - height) / 2, width, height};
}

template<class T>
T GlArg(const Ctx& c, int index) {
    if constexpr (std::is_pointer_v<T>) {
        const auto address = Arg(c, index);
        return address ? reinterpret_cast<T>(GPtr(address)) : nullptr;
    } else if constexpr (std::is_same_v<T, GLfloat>) {
        return ArgF(c, index);
    } else {
        return static_cast<T>(Arg(c, index));
    }
}

template<auto Target>
struct GlCall;

template<typename Result, typename... Args, Result (*Target)(Args...)>
struct GlCall<Target> {
    template<size_t... Index>
    static uint64_t Invoke(Ctx& c, std::index_sequence<Index...>) {
        if constexpr (std::is_void_v<Result>) {
            Target(GlArg<Args>(c, Index)...);
            return 0;
        } else {
            return static_cast<uint64_t>(Target(GlArg<Args>(c, Index)...));
        }
    }

    static uint64_t Run(Ctx& c) {
        return Invoke(c, std::index_sequence_for<Args...>{});
    }
};

#define BIND_GL(name) const Registrar bind_##name{kGl, #name, kCdecl, GlCall<::name>::Run, true}
#define BIND_ALIAS(alias, name) const Registrar bind_##alias{kGl, #alias, kCdecl, GlCall<::name>::Run, true}

BIND_GL(glActiveTexture);
BIND_GL(glAttachShader);
BIND_GL(glBindAttribLocation);
BIND_GL(glBindBuffer);
BIND_GL(glBindRenderbuffer);
BIND_GL(glBindTexture);
BIND_GL(glBindVertexArray);
BIND_GL(glBlendColor);
BIND_GL(glBlendEquation);
BIND_GL(glBlendEquationSeparate);
BIND_GL(glBlendFunc);
BIND_GL(glBlendFuncSeparate);
BIND_GL(glBlitFramebuffer);
BIND_GL(glBufferData);
BIND_GL(glBufferSubData);
BIND_GL(glCheckFramebufferStatus);
BIND_GL(glClear);
BIND_GL(glClearColor);
BIND_GL(glClearDepthf);
BIND_GL(glClearStencil);
BIND_GL(glColorMask);
BIND_GL(glCompileShader);
BIND_GL(glCompressedTexImage2D);
BIND_GL(glCompressedTexSubImage2D);
BIND_GL(glCopyTexSubImage2D);
BIND_GL(glCreateProgram);
BIND_GL(glCullFace);
BIND_GL(glDeleteFramebuffers);
BIND_GL(glDeleteProgram);
BIND_GL(glDeleteRenderbuffers);
BIND_GL(glDeleteTextures);
BIND_GL(glDeleteVertexArrays);
BIND_GL(glDepthFunc);
BIND_GL(glDepthMask);
BIND_GL(glDetachShader);
BIND_GL(glDisable);
BIND_GL(glDisableVertexAttribArray);
BIND_GL(glDrawArrays);
BIND_GL(glDrawArraysInstanced);
BIND_GL(glDrawBuffers);
BIND_GL(glEnable);
BIND_GL(glEnableVertexAttribArray);
BIND_GL(glFinish);
BIND_GL(glFlush);
BIND_GL(glFramebufferRenderbuffer);
BIND_GL(glFramebufferTexture2D);
BIND_GL(glFramebufferTextureLayer);
BIND_GL(glFrontFace);
BIND_GL(glGenBuffers);
BIND_GL(glGenFramebuffers);
BIND_GL(glGenerateMipmap);
BIND_GL(glGenRenderbuffers);
BIND_GL(glGenTextures);
BIND_GL(glGenVertexArrays);
BIND_GL(glGetActiveAttrib);
BIND_GL(glGetActiveUniform);
BIND_GL(glGetAttribLocation);
BIND_GL(glGetBooleanv);
BIND_GL(glGetBufferParameteriv);
BIND_GL(glGetError);
BIND_GL(glGetFloatv);
BIND_GL(glGetFramebufferAttachmentParameteriv);
BIND_GL(glGetProgramInfoLog);
BIND_GL(glGetProgramiv);
BIND_GL(glGetRenderbufferParameteriv);
BIND_GL(glGetShaderInfoLog);
BIND_GL(glGetShaderiv);
BIND_GL(glGetUniformLocation);
BIND_GL(glGetUniformfv);
BIND_GL(glGetUniformiv);
BIND_GL(glIsBuffer);
BIND_GL(glIsEnabled);
BIND_GL(glIsProgram);
BIND_GL(glIsShader);
BIND_GL(glIsTexture);
BIND_GL(glLineWidth);
BIND_GL(glPixelStorei);
BIND_GL(glPolygonOffset);
BIND_GL(glRenderbufferStorage);
BIND_GL(glRenderbufferStorageMultisample);
BIND_GL(glScissor);
BIND_GL(glStencilFunc);
BIND_GL(glStencilMask);
BIND_GL(glStencilOp);
BIND_GL(glTexImage3D);
BIND_GL(glTexSubImage3D);
BIND_GL(glUniform1f);
BIND_GL(glUniform1fv);
BIND_GL(glUniform1i);
BIND_GL(glUniform1iv);
BIND_GL(glUniform2f);
BIND_GL(glUniform2fv);
BIND_GL(glUniform2i);
BIND_GL(glUniform2iv);
BIND_GL(glUniform3f);
BIND_GL(glUniform3fv);
BIND_GL(glUniform3i);
BIND_GL(glUniform3iv);
BIND_GL(glUniform4f);
BIND_GL(glUniform4fv);
BIND_GL(glUniform4i);
BIND_GL(glUniform4iv);
BIND_GL(glUniformMatrix2fv);
BIND_GL(glUniformMatrix2x3fv);
BIND_GL(glUniformMatrix3fv);
BIND_GL(glUniformMatrix4fv);
BIND_GL(glUseProgram);
BIND_GL(glValidateProgram);
BIND_GL(glVertexAttrib1f);
BIND_GL(glVertexAttrib2f);
BIND_GL(glVertexAttrib3f);
BIND_GL(glVertexAttrib4f);
BIND_GL(glVertexAttribDivisor);
BIND_GL(glViewport);

BIND_ALIAS(glActiveTextureARB, glActiveTexture);
BIND_ALIAS(glBindBufferARB, glBindBuffer);
BIND_ALIAS(glBufferDataARB, glBufferData);
BIND_ALIAS(glBufferSubDataARB, glBufferSubData);
BIND_ALIAS(glGenBuffersARB, glGenBuffers);
BIND_ALIAS(glGetBufferParameterivARB, glGetBufferParameteriv);
BIND_ALIAS(glBindRenderbufferEXT, glBindRenderbuffer);
BIND_ALIAS(glBlitFramebufferEXT, glBlitFramebuffer);
BIND_ALIAS(glCheckFramebufferStatusEXT, glCheckFramebufferStatus);
BIND_ALIAS(glDeleteFramebuffersEXT, glDeleteFramebuffers);
BIND_ALIAS(glDeleteRenderbuffersEXT, glDeleteRenderbuffers);
BIND_ALIAS(glFramebufferRenderbufferEXT, glFramebufferRenderbuffer);
BIND_ALIAS(glFramebufferTexture2DEXT, glFramebufferTexture2D);
BIND_ALIAS(glGenerateMipmapEXT, glGenerateMipmap);
BIND_ALIAS(glGenFramebuffersEXT, glGenFramebuffers);
BIND_ALIAS(glGenRenderbuffersEXT, glGenRenderbuffers);
BIND_ALIAS(glRenderbufferStorageEXT, glRenderbufferStorage);
BIND_ALIAS(glRenderbufferStorageMultisampleEXT, glRenderbufferStorageMultisample);
BIND_ALIAS(glBlendEquationEXT, glBlendEquation);
BIND_ALIAS(glBlendFuncSeparateEXT, glBlendFuncSeparate);

#undef BIND_GL
#undef BIND_ALIAS

gaddr CopyString(uint64_t key, const GLubyte* text) {
    if (!text) return 0;
    auto found = driver_strings.find(key);
    if (found != driver_strings.end()) return found->second;
    const std::string value(reinterpret_cast<const char*>(text));
    const gaddr address = HeapAllocProcess(value.size() + 1, false);
    PutStr(address, value);
    driver_strings.emplace(key, address);
    return address;
}

const void* ClientOrOffset(const Ctx& c, int index, GLenum binding) {
    const auto address = Arg(c, index);
    if (!address) return nullptr;
    GLint buffer = 0;
    ::glGetIntegerv(binding, &buffer);
    return buffer ? reinterpret_cast<const void*>(address) : GPtr(address);
}

GLint WrapMode(GLenum name, GLint value) {
    constexpr GLint clamp = 0x2900;  // GL_CLAMP on desktop OpenGL.
    if ((name == GL_TEXTURE_WRAP_S || name == GL_TEXTURE_WRAP_T || name == GL_TEXTURE_WRAP_R) &&
        value == clamp) return GL_CLAMP_TO_EDGE;
    return value;
}

void BindFramebuffer(GLenum target, GLuint framebuffer) {
    ::glBindFramebuffer(target, framebuffer ? framebuffer : default_framebuffer);
}

uint64_t HandleBindFramebuffer(Ctx& c) {
    BindFramebuffer(GlArg<GLenum>(c, 0), GlArg<GLuint>(c, 1));
    return 0;
}

const Registrar bind_framebuffer{kGl, "glBindFramebuffer", kCdecl, HandleBindFramebuffer, true};
const Registrar bind_framebuffer_ext{kGl, "glBindFramebufferEXT", kCdecl, HandleBindFramebuffer, true};

HLE64(kGl, wglGetProcAddress, kCdecl) {
    if (!SDL_GL_GetCurrentContext() || !Arg(c, 0)) return 0;
    const std::string name = GStr(Arg(c, 0));
    return HasHandler(kGl, name) ? ThunkFor(kGl, name) : 0;
}

HLE64(kGl, wglGetCurrentDC, kCdecl) {
    if (!SDL_GL_GetCurrentContext()) return 0;
    return ActiveGlesWindowDc();
}

HLE64(kGl, glGetString, kCdecl) {
    const auto name = GlArg<GLenum>(c, 0);
    const auto* value = ::glGetString(name);
    constexpr std::string_view prefix = "OpenGL ES ";
    if (value && name == GL_VERSION &&
        std::string_view(reinterpret_cast<const char*>(value)).starts_with(prefix)) {
        // The desktop caller expects its version string to begin with digits.
        value += prefix.size();
    }
    return CopyString(name, value);
}

HLE64(kGl, glGetStringi, kCdecl) {
    const auto name = GlArg<GLenum>(c, 0);
    const auto index = GlArg<GLuint>(c, 1);
    const auto* value = ::glGetStringi(name, index);
    return CopyString((uint64_t(name) << 32) | index, value);
}

GLuint BoundBuffer(GLenum target) {
    GLenum binding = 0;
    switch (target) {
        case GL_ARRAY_BUFFER: binding = GL_ARRAY_BUFFER_BINDING; break;
        case GL_ELEMENT_ARRAY_BUFFER: binding = GL_ELEMENT_ARRAY_BUFFER_BINDING; break;
        case GL_COPY_READ_BUFFER: binding = GL_COPY_READ_BUFFER_BINDING; break;
        case GL_COPY_WRITE_BUFFER: binding = GL_COPY_WRITE_BUFFER_BINDING; break;
        case GL_PIXEL_PACK_BUFFER: binding = GL_PIXEL_PACK_BUFFER_BINDING; break;
        case GL_PIXEL_UNPACK_BUFFER: binding = GL_PIXEL_UNPACK_BUFFER_BINDING; break;
        case GL_TRANSFORM_FEEDBACK_BUFFER: binding = GL_TRANSFORM_FEEDBACK_BUFFER_BINDING; break;
        case GL_UNIFORM_BUFFER: binding = GL_UNIFORM_BUFFER_BINDING; break;
        default: return 0;
    }
    GLint id = 0;
    ::glGetIntegerv(binding, &id);
    return static_cast<GLuint>(id);
}

gaddr AcquireStaging(GLenum target, size_t length, GLbitfield access) {
    // The game repeatedly maps a 1 MiB vertex stream, writes a small range
    // and flushes it explicitly. Retain one guest allocation for that path.
    constexpr size_t max_streaming_length = 1024 * 1024;
    if (target != GL_ARRAY_BUFFER || !(access & GL_MAP_FLUSH_EXPLICIT_BIT) ||
        length > max_streaming_length || streaming_in_use)
        return HeapAllocProcess(length, false);
    if (streaming_capacity < length) {
        const auto replacement = HeapAllocProcess(length, false);
        if (!replacement) return 0;
        if (streaming_staging) HeapFreeProcess(streaming_staging);
        streaming_staging = replacement;
        streaming_capacity = length;
    }
    streaming_in_use = true;
    return streaming_staging;
}

void ReleaseStaging(gaddr address) {
    if (address == streaming_staging) streaming_in_use = false;
    else HeapFreeProcess(address);
}

gaddr MapBuffer(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    constexpr auto invalidates = GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT;
    auto native_access = access;
    // With an ordinary write mapping, the guest may leave parts of the
    // buffer untouched. Seed its copy from the existing bytes in that case.
    if ((access & GL_MAP_WRITE_BIT) &&
        !(access & (GL_MAP_READ_BIT | GL_MAP_FLUSH_EXPLICIT_BIT | invalidates))) {
        native_access = (access & ~GL_MAP_UNSYNCHRONIZED_BIT) | GL_MAP_READ_BIT;
    }
    const auto buffer = BoundBuffer(target);
    void* host = ::glMapBufferRange(target, offset, length, native_access);
    if (!host) return 0;
    if (!buffer || length <= 0) {
        ::glUnmapBuffer(target);
        return 0;
    }

    // Write-only mappings are populated by the guest. Explicit flushes copy
    // just the written ranges, so there is no full-buffer clear per frame.
    const auto guest = AcquireStaging(target, static_cast<size_t>(length), access);
    if (!guest) {
        ::glUnmapBuffer(target);
        return 0;
    }
    if (native_access & GL_MAP_READ_BIT) std::memcpy(GPtr(guest), host, static_cast<size_t>(length));
    mapped_buffers.emplace(buffer, BufferMapping{host, guest, static_cast<size_t>(length), access});
    return guest;
}

HLE64(kGl, glMapBufferRange, kCdecl) {
    return MapBuffer(GlArg<GLenum>(c, 0), GlArg<GLintptr>(c, 1), GlArg<GLsizeiptr>(c, 2),
                     GlArg<GLbitfield>(c, 3));
}

HLE64(kGl, glMapBuffer, kCdecl) {
    const auto target = GlArg<GLenum>(c, 0);
    GLbitfield access = 0;
    switch (GlArg<GLenum>(c, 1)) {
        case 0x88B8: access = GL_MAP_READ_BIT; break;   // GL_READ_ONLY
        case 0x88B9: access = GL_MAP_WRITE_BIT; break;  // GL_WRITE_ONLY
        case 0x88BA: access = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT; break;  // GL_READ_WRITE
    }
    GLint64 size = 0;
    ::glGetBufferParameteri64v(target, GL_BUFFER_SIZE, &size);
    return MapBuffer(target, 0, size, access);
}

HLE64(kGl, glFlushMappedBufferRange, kCdecl) {
    const auto target = GlArg<GLenum>(c, 0);
    const auto offset = GlArg<GLintptr>(c, 1);
    const auto length = GlArg<GLsizeiptr>(c, 2);
    const auto mapping = mapped_buffers.find(BoundBuffer(target));
    if (mapping != mapped_buffers.end() && offset >= 0 && length > 0) {
        const auto& view = mapping->second;
        if ((view.access & GL_MAP_FLUSH_EXPLICIT_BIT) &&
            size_t(offset) <= view.length && size_t(length) <= view.length - size_t(offset)) {
            std::memcpy(static_cast<uint8_t*>(view.host) + offset, GPtr(view.guest + offset), length);
        }
    }
    ::glFlushMappedBufferRange(target, offset, length);
    return 0;
}

HLE64(kGl, glUnmapBuffer, kCdecl) {
    const auto target = GlArg<GLenum>(c, 0);
    const auto mapping = mapped_buffers.find(BoundBuffer(target));
    if (mapping != mapped_buffers.end()) {
        const auto& view = mapping->second;
        if ((view.access & GL_MAP_WRITE_BIT) && !(view.access & GL_MAP_FLUSH_EXPLICIT_BIT))
            std::memcpy(view.host, GPtr(view.guest), view.length);
        ReleaseStaging(view.guest);
        mapped_buffers.erase(mapping);
    }
    return ::glUnmapBuffer(target);
}

HLE64(kGl, glDeleteBuffers, kCdecl) {
    const auto count = GlArg<GLsizei>(c, 0);
    const auto* buffers = GlArg<const GLuint*>(c, 1);
    if (buffers) {
        for (GLsizei i = 0; i < count; ++i) {
            if (auto mapping = mapped_buffers.find(buffers[i]); mapping != mapped_buffers.end()) {
                ReleaseStaging(mapping->second.guest);
                mapped_buffers.erase(mapping);
            }
        }
    }
    ::glDeleteBuffers(count, buffers);
    return 0;
}

HLE64(kGl, glDeleteBuffersARB, kCdecl) { return hle_glDeleteBuffers(c); }

HLE64(kGl, glGetIntegerv, kCdecl) {
    const auto name = GlArg<GLenum>(c, 0);
    GLint* value = GlArg<GLint*>(c, 1);
    if (!value) return 0;
    ::glGetIntegerv(name, value);
    if ((name == GL_DRAW_FRAMEBUFFER_BINDING || name == GL_READ_FRAMEBUFFER_BINDING) &&
        static_cast<GLuint>(*value) == default_framebuffer) *value = 0;
    return 0;
}

HLE64(kGl, glReadBuffer, kCdecl) {
    auto buffer = GlArg<GLenum>(c, 0);
    if (buffer == GL_BACK && default_framebuffer) {
        GLint bound = 0;
        ::glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &bound);
        if (static_cast<GLuint>(bound) == default_framebuffer) buffer = GL_COLOR_ATTACHMENT0;
    }
    ::glReadBuffer(buffer);
    return 0;
}

HLE64(kGl, glDrawElements, kCdecl) {
    ::glDrawElements(GlArg<GLenum>(c, 0), GlArg<GLsizei>(c, 1), GlArg<GLenum>(c, 2),
                     ClientOrOffset(c, 3, GL_ELEMENT_ARRAY_BUFFER_BINDING));
    return 0;
}

HLE64(kGl, glVertexAttribPointer, kCdecl) {
    ::glVertexAttribPointer(GlArg<GLuint>(c, 0), GlArg<GLint>(c, 1), GlArg<GLenum>(c, 2),
                            GlArg<GLboolean>(c, 3), GlArg<GLsizei>(c, 4),
                            ClientOrOffset(c, 5, GL_ARRAY_BUFFER_BINDING));
    return 0;
}

HLE64(kGl, glTexImage2D, kCdecl) {
    auto internal_format = GlArg<GLint>(c, 2);
    const auto format = GlArg<GLenum>(c, 6);
    const auto type = GlArg<GLenum>(c, 7);
    // The game's RGBA glyph fallback asks for desktop GL_SRGB_ALPHA.
    // GLES 3 uses a sized internal format with this RGBA upload.
    constexpr GLint desktop_srgb_alpha = 0x8C42;
    if (internal_format == desktop_srgb_alpha && format == GL_RGBA && type == GL_UNSIGNED_BYTE)
        internal_format = GL_SRGB8_ALPHA8;
    ::glTexImage2D(GlArg<GLenum>(c, 0), GlArg<GLint>(c, 1), internal_format,
                   GlArg<GLsizei>(c, 3), GlArg<GLsizei>(c, 4), GlArg<GLint>(c, 5),
                   format, type, ClientOrOffset(c, 8, GL_PIXEL_UNPACK_BUFFER_BINDING));
    return 0;
}

HLE64(kGl, glTexSubImage2D, kCdecl) {
    ::glTexSubImage2D(GlArg<GLenum>(c, 0), GlArg<GLint>(c, 1), GlArg<GLint>(c, 2),
                      GlArg<GLint>(c, 3), GlArg<GLsizei>(c, 4), GlArg<GLsizei>(c, 5),
                      GlArg<GLenum>(c, 6), GlArg<GLenum>(c, 7),
                      ClientOrOffset(c, 8, GL_PIXEL_UNPACK_BUFFER_BINDING));
    return 0;
}

HLE64(kGl, glReadPixels, kCdecl) {
    ::glReadPixels(GlArg<GLint>(c, 0), GlArg<GLint>(c, 1), GlArg<GLsizei>(c, 2),
                   GlArg<GLsizei>(c, 3), GlArg<GLenum>(c, 4), GlArg<GLenum>(c, 5),
                   const_cast<void*>(ClientOrOffset(c, 6, GL_PIXEL_PACK_BUFFER_BINDING)));
    return 0;
}

HLE64(kGl, glTexParameteri, kCdecl) {
    const auto name = GlArg<GLenum>(c, 1);
    ::glTexParameteri(GlArg<GLenum>(c, 0), name, WrapMode(name, GlArg<GLint>(c, 2)));
    return 0;
}

HLE64(kGl, glTexParameteriv, kCdecl) {
    const auto target = GlArg<GLenum>(c, 0);
    const auto name = GlArg<GLenum>(c, 1);
    const GLint* values = GlArg<const GLint*>(c, 2);
    if (!values) return 0;
    // Desktop GL allows setting the four swizzles together. GLES exposes
    // them separately. The bitmap font loader uses (1, 1, 1, red) to turn
    // an 8-bit grayscale atlas into a white glyph with variable opacity.
    constexpr GLenum texture_swizzle_rgba = 0x8E46;
    if (name == texture_swizzle_rgba) {
        constexpr GLenum components[]{GL_TEXTURE_SWIZZLE_R, GL_TEXTURE_SWIZZLE_G,
                                      GL_TEXTURE_SWIZZLE_B, GL_TEXTURE_SWIZZLE_A};
        for (size_t i = 0; i < std::size(components); ++i)
            ::glTexParameteri(target, components[i], values[i]);
        return 0;
    }
    if (name == GL_TEXTURE_WRAP_S || name == GL_TEXTURE_WRAP_T || name == GL_TEXTURE_WRAP_R)
        ::glTexParameteri(target, name, WrapMode(name, *values));
    else
        ::glTexParameteriv(target, name, values);
    return 0;
}

HLE64(kGl, glTexParameterf, kCdecl) {
    const auto name = GlArg<GLenum>(c, 1);
    const auto value = GlArg<GLfloat>(c, 2);
    constexpr GLfloat clamp = 10496.0f;
    const bool wrap = name == GL_TEXTURE_WRAP_S || name == GL_TEXTURE_WRAP_T || name == GL_TEXTURE_WRAP_R;
    ::glTexParameterf(GlArg<GLenum>(c, 0), name, wrap && value == clamp ? GL_CLAMP_TO_EDGE : value);
    return 0;
}

HLE64(kGl, glCreateShader, kCdecl) {
    const GLuint shader = ::glCreateShader(GlArg<GLenum>(c, 0));
    shader_defaults.erase(shader);
    return shader;
}

HLE64(kGl, glDeleteShader, kCdecl) {
    const GLuint shader = GlArg<GLuint>(c, 0);
    ::glDeleteShader(shader);
    if (!::glIsShader(shader)) shader_defaults.erase(shader);
    return 0;
}

HLE64(kGl, glShaderSource, kCdecl) {
    const auto shader = GlArg<GLuint>(c, 0);
    const auto count = GlArg<GLsizei>(c, 1);
    const auto fragments = Arg(c, 2);
    const auto* lengths = GlArg<const GLint*>(c, 3);
    if (count <= 0 || !fragments) return 0;
    std::string source;
    for (int i = 0; i < count; ++i) {
        const auto address = RdPtr(fragments + i * sizeof(uint64_t));
        const char* fragment = reinterpret_cast<const char*>(GPtr(address));
        if (lengths && lengths[i] >= 0) source.append(fragment, lengths[i]);
        else source.append(fragment);
    }
    GLint stage = 0;
    ::glGetShaderiv(shader, GL_SHADER_TYPE, &stage);
    shader_defaults.erase(shader);
    if (source.find("#version 120") != std::string::npos) {
        try {
            auto converted = darkest::MakeMobileShader(source, stage == GL_VERTEX_SHADER
                ? darkest::ShaderStage::Vertex : darkest::ShaderStage::Fragment);
            source = std::move(converted.text);
            if (!converted.defaults.empty()) shader_defaults.emplace(shader, std::move(converted.defaults));
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Darkest Dungeon: GLSL conversion failed: %s\n", error.what());
        }
    }
    const char* text = source.c_str();
    const GLint length = static_cast<GLint>(source.size());
    ::glShaderSource(shader, 1, &text, &length);
    return 0;
}

HLE64(kGl, glLinkProgram, kCdecl) {
    const auto program = GlArg<GLuint>(c, 0);
    ::glLinkProgram(program);
    GLint linked = 0, count = 0;
    ::glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) return 0;
    ::glGetProgramiv(program, GL_ATTACHED_SHADERS, &count);
    if (count <= 0 || shader_defaults.empty()) return 0;
    std::vector<GLuint> shaders(count);
    GLsizei written = 0;
    ::glGetAttachedShaders(program, count, &written, shaders.data());
    GLint previous = 0;
    ::glGetIntegerv(GL_CURRENT_PROGRAM, &previous);
    ::glUseProgram(program);
    for (GLsizei i = 0; i < written; ++i) {
        const auto defaults = shader_defaults.find(shaders[i]);
        if (defaults == shader_defaults.end()) continue;
        for (const auto& uniform : defaults->second) {
            const auto location = ::glGetUniformLocation(program, uniform.name.c_str());
            if (location >= 0) ::glUniform1fv(location, static_cast<GLsizei>(uniform.values.size()),
                                              uniform.values.data());
        }
    }
    ::glUseProgram(static_cast<GLuint>(previous));
    return 0;
}

}  // namespace

void CaptureGlesDefaultFramebuffer(SDL_Window* window, SDL_Point requested) {
    const auto properties = SDL_GetWindowProperties(window);
    default_framebuffer = static_cast<GLuint>(SDL_GetNumberProperty(
        properties, SDL_PROP_WINDOW_UIKIT_OPENGL_FRAMEBUFFER_NUMBER, 0));
    game_framebuffer = 0;
    game_drawable_window = window;
    const auto pixels = WindowPixels(window);
    game_drawable_size = pixels;
    if (!default_framebuffer || SDL_GetNumberProperty(
            properties, SDL_PROP_WINDOW_UIKIT_OPENGL_RESOLVE_FRAMEBUFFER_NUMBER, 0)) return;
    const auto size = requested.x > 0 && requested.y > 0
        ? requested : PreferredGameSize(window, pixels);
    if (size.x <= 0 || size.y <= 0 || (size.x == pixels.x && size.y == pixels.y)) return;

    GLint read = 0, draw = 0, renderbuffer = 0;
    ::glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    ::glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    ::glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    ::glGenFramebuffers(1, &game_framebuffer);
    ::glBindFramebuffer(GL_FRAMEBUFFER, game_framebuffer);
    GLuint attachments[2]{};
    ::glGenRenderbuffers(2, attachments);
    for (int i = 0; i < 2; ++i) {
        ::glBindRenderbuffer(GL_RENDERBUFFER, attachments[i]);
        ::glRenderbufferStorage(GL_RENDERBUFFER, i ? GL_DEPTH24_STENCIL8 : GL_RGBA8, size.x, size.y);
        ::glFramebufferRenderbuffer(GL_FRAMEBUFFER, i ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0,
                                    GL_RENDERBUFFER, attachments[i]);
    }
    const bool complete = ::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    ::glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(renderbuffer));
    if (complete) {
        game_drawable_size = size;
        if (static_cast<GLuint>(read) == default_framebuffer) read = static_cast<GLint>(game_framebuffer);
        if (static_cast<GLuint>(draw) == default_framebuffer) draw = static_cast<GLint>(game_framebuffer);
        default_framebuffer = game_framebuffer;
        std::fprintf(stderr, "Darkest Dungeon: game drawable %dx%d on %dx%d screen.\n",
                     size.x, size.y, pixels.x, pixels.y);
    } else {
        ::glDeleteFramebuffers(1, &game_framebuffer);
        ::glDeleteRenderbuffers(2, attachments);
        game_framebuffer = 0;
        std::fputs("Darkest Dungeon: using the native drawable size.\n", stderr);
    }
    ::glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read));
    ::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw));
}

void GameDrawableSize(SDL_Window* window, int* width, int* height) {
    const auto pixels = WindowPixels(window);
    const auto size = game_drawable_window == window
        ? (game_framebuffer ? game_drawable_size : pixels) : PreferredGameSize(window, pixels);
    *width = size.x;
    *height = size.y;
}

void GameDrawablePointer(SDL_Window* window, float& x, float& y, float& dx, float& dy) {
    const float density = SDL_GetWindowPixelDensity(window);
    if (density <= 0) return;
    const auto pixels = WindowPixels(window);
    auto area = SDL_Rect{0, 0, pixels.x, pixels.y};
    SDL_Point size{pixels};
    if (game_drawable_window == window && game_framebuffer) {
        size = game_drawable_size;
        area = FitGameRect(size, pixels);
    }
    if (area.w <= 0 || area.h <= 0) return;
    const auto sx = float(size.x) / area.w;
    const auto sy = float(size.y) / area.h;
    x = (x * density - area.x) * sx;
    y = (y * density - (pixels.y - area.y - area.h)) * sy;
    dx *= density * sx;
    dy *= density * sy;
}

void ForgetGameDrawable(SDL_Window* window) {
    if (game_drawable_window == window) {
        for (const auto& [_, mapping] : mapped_buffers) ReleaseStaging(mapping.guest);
        mapped_buffers.clear();
        if (streaming_staging) HeapFreeProcess(streaming_staging);
        streaming_staging = 0;
        streaming_capacity = 0;
        streaming_in_use = false;
        default_framebuffer = game_framebuffer = 0;
        game_drawable_window = nullptr;
        game_drawable_size = {};
    }
}

bool PresentGlesWindow(SDL_Window* window) {
    const auto properties = SDL_GetWindowProperties(window);
    const auto drawable = static_cast<GLuint>(SDL_GetNumberProperty(
        properties, SDL_PROP_WINDOW_UIKIT_OPENGL_RENDERBUFFER_NUMBER, 0));
    if (!drawable) return SDL_GL_SwapWindow(window);

    GLint previous_renderbuffer = 0;
    ::glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous_renderbuffer);
    const bool rebind = static_cast<GLuint>(previous_renderbuffer) != drawable;
    if (rebind) {
        ::glBindRenderbuffer(GL_RENDERBUFFER, drawable);
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "Darkest Dungeon: presenting UIKit color buffer %u (guest bound %d).\n",
                         drawable, previous_renderbuffer);
        }
    }

    // SDL resolves the drawable for MSAA windows. It uses the current read
    // framebuffer as the source and changes the draw binding during the blit.
    GLint previous_read = 0, previous_draw = 0;
    const bool multisampled = SDL_GetNumberProperty(
        properties, SDL_PROP_WINDOW_UIKIT_OPENGL_RESOLVE_FRAMEBUFFER_NUMBER, 0) != 0;
    const bool composite = game_drawable_window == window && game_framebuffer;
    if (composite || multisampled) {
        ::glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
        ::glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
        ::glBindFramebuffer(GL_READ_FRAMEBUFFER, default_framebuffer);
    }
    if (composite) {
        const auto target = static_cast<GLuint>(SDL_GetNumberProperty(
            properties, SDL_PROP_WINDOW_UIKIT_OPENGL_FRAMEBUFFER_NUMBER, 0));
        const auto pixels = WindowPixels(window);
        const auto area = FitGameRect(game_drawable_size, pixels);
        if (target && area.w > 0 && area.h > 0) {
            ::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
            const bool scissor = ::glIsEnabled(GL_SCISSOR_TEST);
            if (scissor) ::glDisable(GL_SCISSOR_TEST);
            if (area.w != pixels.x || area.h != pixels.y) {
                constexpr GLfloat black[]{0, 0, 0, 1};
                GLboolean mask[4]{};
                ::glGetBooleanv(GL_COLOR_WRITEMASK, mask);
                ::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                ::glClearBufferfv(GL_COLOR, 0, black);
                ::glColorMask(mask[0], mask[1], mask[2], mask[3]);
            }
            ::glBlitFramebuffer(0, 0, game_drawable_size.x, game_drawable_size.y,
                                area.x, area.y, area.x + area.w, area.y + area.h,
                                GL_COLOR_BUFFER_BIT, GL_LINEAR);
            if (scissor) ::glEnable(GL_SCISSOR_TEST);
        }
    }
    const bool presented = SDL_GL_SwapWindow(window);
    if (composite || multisampled) {
        ::glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_read));
        ::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previous_draw));
    }
    if (rebind) ::glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previous_renderbuffer));
    return presented;
}

}  // namespace recomp::hle
