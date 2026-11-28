#include <OpenGLES/ES3/gl.h>
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <exception>
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
thread_local std::map<GLuint, std::vector<darkest::UniformDefault>> shader_defaults;
thread_local std::map<uint64_t, gaddr> driver_strings;

struct BufferMapping {
    void* host;
    gaddr guest;
    size_t length;
    GLbitfield access;
};

thread_local std::map<GLuint, BufferMapping> mapped_buffers;

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
    GLint buffer = 0;
    ::glGetIntegerv(binding, &buffer);
    if (buffer) return reinterpret_cast<const void*>(Arg(c, index));
    return GlArg<const void*>(c, index);
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

    const auto guest = HeapAllocProcess(static_cast<size_t>(length), !(native_access & GL_MAP_READ_BIT));
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
        HeapFreeProcess(view.guest);
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
                HeapFreeProcess(mapping->second.guest);
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
    ::glTexImage2D(GlArg<GLenum>(c, 0), GlArg<GLint>(c, 1), GlArg<GLint>(c, 2),
                   GlArg<GLsizei>(c, 3), GlArg<GLsizei>(c, 4), GlArg<GLint>(c, 5),
                   GlArg<GLenum>(c, 6), GlArg<GLenum>(c, 7),
                   ClientOrOffset(c, 8, GL_PIXEL_UNPACK_BUFFER_BINDING));
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
    const auto name = GlArg<GLenum>(c, 1);
    const GLint* values = GlArg<const GLint*>(c, 2);
    if (!values) return 0;
    if (name == GL_TEXTURE_WRAP_S || name == GL_TEXTURE_WRAP_T || name == GL_TEXTURE_WRAP_R)
        ::glTexParameteri(GlArg<GLenum>(c, 0), name, WrapMode(name, *values));
    else
        ::glTexParameteriv(GlArg<GLenum>(c, 0), name, values);
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

void CaptureGlesDefaultFramebuffer() {
    GLint bound = 0;
    ::glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
    default_framebuffer = static_cast<GLuint>(bound);
}

}  // namespace recomp::hle
