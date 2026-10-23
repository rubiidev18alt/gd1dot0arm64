// SPDX-License-Identifier: MIT
#include "thunks_gl.h"
#include "abi.h"
#include "loader.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <unordered_map>

namespace {

GLuint g_array_buf = 0;
GLuint g_element_buf = 0;

// 1.1.1 "slight runner optimizations": cocos2d-x 2.0 re-issues the same glBindTexture/glBindBuffer
// pair for every quad it submits. Both calls are pure state assignments, so re-binding the object
// that is already bound is a no-op that still costs an ARM32->ARM64 thunk transition and a GLES
// driver entry point (expensive on emulator GL and on low-end mobile GPUs). Every cache entry below
// mirrors the real GL state - initialised on context creation, adjusted when the object is deleted -
// so skipping is never stale.
constexpr int kMaxTexUnits = 8;
GLenum g_active_tex = GL_TEXTURE0;
GLuint g_bound_tex[kMaxTexUnits] = {};
GLenum g_bound_tex_target[kMaxTexUnits] = {};
bool g_bound_tex_valid[kMaxTexUnits] = {};

void t_glActiveTexture(GLenum unit) {
    g_active_tex = unit;
    glActiveTexture(unit);
}

void t_glBindTexture(GLenum target, GLuint texture) {
    const unsigned unit = (unsigned)(g_active_tex - GL_TEXTURE0);
    if (unit < (unsigned)kMaxTexUnits) {
        if (g_bound_tex_valid[unit] && g_bound_tex_target[unit] == target && g_bound_tex[unit] == texture)
            return;
        g_bound_tex_valid[unit] = true;
        g_bound_tex_target[unit] = target;
        g_bound_tex[unit] = texture;
    }
    glBindTexture(target, texture);
}

void t_glBindBuffer(GLenum target, GLuint buffer) {
    GLuint *slot = nullptr;
    if (target == GL_ARRAY_BUFFER) slot = &g_array_buf;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) slot = &g_element_buf;
    if (slot) {
        if (*slot == buffer) return;  // already bound; the resulting GL state is identical
        *slot = buffer;
    }
    glBindBuffer(target, buffer);
}

void t_glDeleteBuffers(GLsizei n, G<const GLuint> buffers) {
    const GLuint *b = buffers.get();
    if (b) {
        for (GLsizei i = 0; i < n; i++) {
            if (b[i] && b[i] == g_array_buf) g_array_buf = 0;
            if (b[i] && b[i] == g_element_buf) g_element_buf = 0;
        }
    }
    glDeleteBuffers(n, b);
}

void t_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, u32 ptr) {
    const void *p = g_array_buf ? (const void *)(uintptr_t)ptr : g2h(ptr);
    glVertexAttribPointer(index, size, type, normalized, stride, p);
}

void t_glDrawElements(GLenum mode, GLsizei count, GLenum type, u32 indices) {
    const void *p = g_element_buf ? (const void *)(uintptr_t)indices : g2h(indices);
    glDrawElements(mode, count, type, p);
}

void t_glShaderSource(GLuint shader, GLsizei count, G<const u32> strings_g, G<const GLint> lengths_g) {
    std::vector<const GLchar *> ptrs((size_t)count);
    for (GLsizei i = 0; i < count; i++) ptrs[(size_t)i] = (const GLchar *)g2h(strings_g[i]);
    glShaderSource(shader, count, ptrs.data(), lengths_g);
}

u32 t_glGetString(GLenum name) {
    static std::unordered_map<GLenum, u32> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    const GLubyte *s = glGetString(name);
    u32 g = s ? mem::hstrdup((const char *)s) : 0;
    cache[name] = g;
    return g;
}

void t_glBufferData(GLenum t, s32 size, const void *data, GLenum u) { glBufferData(t, size, data, u); }
void t_glBufferSubData(GLenum t, s32 off, s32 size, const void *data) { glBufferSubData(t, off, size, data); }

void t_glGenBuffers(GLsizei n, G<GLuint> b) { glGenBuffers(n, b); }
void t_glGenFramebuffers(GLsizei n, G<GLuint> b) { glGenFramebuffers(n, b); }
void t_glDeleteFramebuffers(GLsizei n, G<const GLuint> b) { glDeleteFramebuffers(n, b); }
void t_glGenRenderbuffers(GLsizei n, G<GLuint> b) { glGenRenderbuffers(n, b); }
void t_glDeleteRenderbuffers(GLsizei n, G<const GLuint> b) { glDeleteRenderbuffers(n, b); }
void t_glGenTextures(GLsizei n, G<GLuint> b) { glGenTextures(n, b); }
void t_glDeleteTextures(GLsizei n, G<const GLuint> b) {
    // Deleting a bound texture unbinds it (GL leaves the unit at 0), so mirror that in the cache.
    const GLuint *ids = b.get();
    if (ids) {
        for (GLsizei i = 0; i < n; i++) {
            if (!ids[i]) continue;
            for (int u = 0; u < kMaxTexUnits; u++) {
                if (g_bound_tex_valid[u] && g_bound_tex[u] == ids[i]) g_bound_tex[u] = 0;
            }
        }
    }
    glDeleteTextures(n, b);
}

void t_glGetIntegerv(GLenum p, G<GLint> d) { glGetIntegerv(p, d); }
void t_glGetFloatv(GLenum p, G<GLfloat> d) { glGetFloatv(p, d); }
void t_glGetBooleanv(GLenum p, G<GLboolean> d) { glGetBooleanv(p, d); }
void t_glGetShaderiv(GLuint s, GLenum p, G<GLint> d) { glGetShaderiv(s, p, d); }
void t_glGetProgramiv(GLuint s, GLenum p, G<GLint> d) { glGetProgramiv(s, p, d); }
void t_glGetShaderInfoLog(GLuint s, GLsizei m, G<GLsizei> l, char *b) { glGetShaderInfoLog(s, m, l, b); }
void t_glGetProgramInfoLog(GLuint s, GLsizei m, G<GLsizei> l, char *b) { glGetProgramInfoLog(s, m, l, b); }
void t_glGetActiveAttrib(GLuint p, GLuint i, GLsizei b, G<GLsizei> l, G<GLint> s, G<GLenum> t, char *n) {
    glGetActiveAttrib(p, i, b, l, s, t, n);
}
void t_glGetActiveUniform(GLuint p, GLuint i, GLsizei b, G<GLsizei> l, G<GLint> s, G<GLenum> t, char *n) {
    glGetActiveUniform(p, i, b, l, s, t, n);
}
void t_glGetAttachedShaders(GLuint p, GLsizei m, G<GLsizei> c, G<GLuint> s) { glGetAttachedShaders(p, m, c, s); }
void t_glGetShaderPrecisionFormat(GLenum st, GLenum pt, G<GLint> r, G<GLint> pr) {
    glGetShaderPrecisionFormat(st, pt, r, pr);
}
void t_glGetShaderSource(GLuint s, GLsizei b, G<GLsizei> l, char *src) { glGetShaderSource(s, b, l, src); }
void t_glGetTexParameterfv(GLenum t, GLenum p, G<GLfloat> d) { glGetTexParameterfv(t, p, d); }
void t_glGetTexParameteriv(GLenum t, GLenum p, G<GLint> d) { glGetTexParameteriv(t, p, d); }
void t_glGetUniformfv(GLuint p, GLint l, G<GLfloat> d) { glGetUniformfv(p, l, d); }
void t_glGetUniformiv(GLuint p, GLint l, G<GLint> d) { glGetUniformiv(p, l, d); }
void t_glGetVertexAttribfv(GLuint i, GLenum p, G<GLfloat> d) { glGetVertexAttribfv(i, p, d); }
void t_glGetVertexAttribiv(GLuint i, GLenum p, G<GLint> d) { glGetVertexAttribiv(i, p, d); }
void t_glGetBufferParameteriv(GLenum t, GLenum p, G<GLint> d) { glGetBufferParameteriv(t, p, d); }
void t_glGetFramebufferAttachmentParameteriv(GLenum t, GLenum a, GLenum p, G<GLint> d) {
    glGetFramebufferAttachmentParameteriv(t, a, p, d);
}
void t_glGetRenderbufferParameteriv(GLenum t, GLenum p, G<GLint> d) { glGetRenderbufferParameteriv(t, p, d); }

void t_glUniform1fv(GLint l, GLsizei c, G<const GLfloat> v) { glUniform1fv(l, c, v); }
void t_glUniform2fv(GLint l, GLsizei c, G<const GLfloat> v) { glUniform2fv(l, c, v); }
void t_glUniform3fv(GLint l, GLsizei c, G<const GLfloat> v) { glUniform3fv(l, c, v); }
void t_glUniform4fv(GLint l, GLsizei c, G<const GLfloat> v) { glUniform4fv(l, c, v); }
void t_glUniform1iv(GLint l, GLsizei c, G<const GLint> v) { glUniform1iv(l, c, v); }
void t_glUniform2iv(GLint l, GLsizei c, G<const GLint> v) { glUniform2iv(l, c, v); }
void t_glUniform3iv(GLint l, GLsizei c, G<const GLint> v) { glUniform3iv(l, c, v); }
void t_glUniform4iv(GLint l, GLsizei c, G<const GLint> v) { glUniform4iv(l, c, v); }
void t_glUniformMatrix2fv(GLint l, GLsizei c, GLboolean tr, G<const GLfloat> v) { glUniformMatrix2fv(l, c, tr, v); }
void t_glUniformMatrix3fv(GLint l, GLsizei c, GLboolean tr, G<const GLfloat> v) { glUniformMatrix3fv(l, c, tr, v); }
void t_glUniformMatrix4fv(GLint l, GLsizei c, GLboolean tr, G<const GLfloat> v) { glUniformMatrix4fv(l, c, tr, v); }
void t_glVertexAttrib1fv(GLuint i, G<const GLfloat> v) { glVertexAttrib1fv(i, v); }
void t_glVertexAttrib2fv(GLuint i, G<const GLfloat> v) { glVertexAttrib2fv(i, v); }
void t_glVertexAttrib3fv(GLuint i, G<const GLfloat> v) { glVertexAttrib3fv(i, v); }
void t_glVertexAttrib4fv(GLuint i, G<const GLfloat> v) { glVertexAttrib4fv(i, v); }

void t_glTexParameterfv(GLenum t, GLenum p, G<const GLfloat> d) { glTexParameterfv(t, p, d); }
void t_glTexParameteriv(GLenum t, GLenum p, G<const GLint> d) { glTexParameteriv(t, p, d); }

}  // namespace

namespace gl {
void reset_context_state() {
    g_array_buf = 0;
    g_element_buf = 0;
    g_active_tex = GL_TEXTURE0;
    for (int u = 0; u < kMaxTexUnits; u++) {
        g_bound_tex_valid[u] = false;
        g_bound_tex[u] = 0;
        g_bound_tex_target[u] = 0;
    }
}
}  // namespace gl

void register_gl_thunks() {
    THUNK("glActiveTexture", t_glActiveTexture);
    THUNK("glAttachShader", glAttachShader);
    THUNK("glBindAttribLocation", glBindAttribLocation);
    THUNK("glBindBuffer", t_glBindBuffer);
    THUNK("glBindFramebuffer", glBindFramebuffer);
    THUNK("glBindRenderbuffer", glBindRenderbuffer);
    THUNK("glBindTexture", t_glBindTexture);
    THUNK("glBlendColor", glBlendColor);
    THUNK("glBlendEquation", glBlendEquation);
    THUNK("glBlendEquationSeparate", glBlendEquationSeparate);
    THUNK("glBlendFunc", glBlendFunc);
    THUNK("glBlendFuncSeparate", glBlendFuncSeparate);
    THUNK("glBufferData", t_glBufferData);
    THUNK("glBufferSubData", t_glBufferSubData);
    THUNK("glCheckFramebufferStatus", glCheckFramebufferStatus);
    THUNK("glClear", glClear);
    THUNK("glClearColor", glClearColor);
    THUNK("glClearDepthf", glClearDepthf);
    THUNK("glClearStencil", glClearStencil);
    THUNK("glColorMask", glColorMask);
    THUNK("glCompileShader", glCompileShader);
    THUNK("glCompressedTexImage2D", glCompressedTexImage2D);
    THUNK("glCompressedTexSubImage2D", glCompressedTexSubImage2D);
    THUNK("glCopyTexImage2D", glCopyTexImage2D);
    THUNK("glCopyTexSubImage2D", glCopyTexSubImage2D);
    THUNK("glCreateProgram", glCreateProgram);
    THUNK("glCreateShader", glCreateShader);
    THUNK("glCullFace", glCullFace);
    THUNK("glDeleteBuffers", t_glDeleteBuffers);
    THUNK("glDeleteFramebuffers", t_glDeleteFramebuffers);
    THUNK("glDeleteProgram", glDeleteProgram);
    THUNK("glDeleteRenderbuffers", t_glDeleteRenderbuffers);
    THUNK("glDeleteShader", glDeleteShader);
    THUNK("glDeleteTextures", t_glDeleteTextures);
    THUNK("glDepthFunc", glDepthFunc);
    THUNK("glDepthMask", glDepthMask);
    THUNK("glDepthRangef", glDepthRangef);
    THUNK("glDetachShader", glDetachShader);
    THUNK("glDisable", glDisable);
    THUNK("glDisableVertexAttribArray", glDisableVertexAttribArray);
    THUNK("glDrawArrays", glDrawArrays);
    THUNK("glDrawElements", t_glDrawElements);
    THUNK("glEnable", glEnable);
    THUNK("glEnableVertexAttribArray", glEnableVertexAttribArray);
    THUNK("glFinish", glFinish);
    THUNK("glFlush", glFlush);
    THUNK("glFramebufferRenderbuffer", glFramebufferRenderbuffer);
    THUNK("glFramebufferTexture2D", glFramebufferTexture2D);
    THUNK("glFrontFace", glFrontFace);
    THUNK("glGenBuffers", t_glGenBuffers);
    THUNK("glGenFramebuffers", t_glGenFramebuffers);
    THUNK("glGenRenderbuffers", t_glGenRenderbuffers);
    THUNK("glGenTextures", t_glGenTextures);
    THUNK("glGenerateMipmap", glGenerateMipmap);
    THUNK("glGetActiveAttrib", t_glGetActiveAttrib);
    THUNK("glGetActiveUniform", t_glGetActiveUniform);
    THUNK("glGetAttachedShaders", t_glGetAttachedShaders);
    THUNK("glGetAttribLocation", glGetAttribLocation);
    THUNK("glGetBooleanv", t_glGetBooleanv);
    THUNK("glGetBufferParameteriv", t_glGetBufferParameteriv);
    THUNK("glGetError", glGetError);
    THUNK("glGetFloatv", t_glGetFloatv);
    THUNK("glGetFramebufferAttachmentParameteriv", t_glGetFramebufferAttachmentParameteriv);
    THUNK("glGetIntegerv", t_glGetIntegerv);
    THUNK("glGetProgramInfoLog", t_glGetProgramInfoLog);
    THUNK("glGetProgramiv", t_glGetProgramiv);
    THUNK("glGetRenderbufferParameteriv", t_glGetRenderbufferParameteriv);
    THUNK("glGetShaderInfoLog", t_glGetShaderInfoLog);
    THUNK("glGetShaderPrecisionFormat", t_glGetShaderPrecisionFormat);
    THUNK("glGetShaderSource", t_glGetShaderSource);
    THUNK("glGetShaderiv", t_glGetShaderiv);
    THUNK("glGetString", t_glGetString);
    THUNK("glGetTexParameterfv", t_glGetTexParameterfv);
    THUNK("glGetTexParameteriv", t_glGetTexParameteriv);
    THUNK("glGetUniformLocation", glGetUniformLocation);
    THUNK("glGetUniformfv", t_glGetUniformfv);
    THUNK("glGetUniformiv", t_glGetUniformiv);
    THUNK("glGetVertexAttribfv", t_glGetVertexAttribfv);
    THUNK("glGetVertexAttribiv", t_glGetVertexAttribiv);
    THUNK("glHint", glHint);
    THUNK("glIsBuffer", glIsBuffer);
    THUNK("glIsEnabled", glIsEnabled);
    THUNK("glIsFramebuffer", glIsFramebuffer);
    THUNK("glIsProgram", glIsProgram);
    THUNK("glIsRenderbuffer", glIsRenderbuffer);
    THUNK("glIsShader", glIsShader);
    THUNK("glIsTexture", glIsTexture);
    THUNK("glLineWidth", glLineWidth);
    THUNK("glLinkProgram", glLinkProgram);
    THUNK("glPixelStorei", glPixelStorei);
    THUNK("glPolygonOffset", glPolygonOffset);
    THUNK("glReadPixels", glReadPixels);
    THUNK("glReleaseShaderCompiler", glReleaseShaderCompiler);
    THUNK("glRenderbufferStorage", glRenderbufferStorage);
    THUNK("glSampleCoverage", glSampleCoverage);
    THUNK("glScissor", glScissor);
    THUNK("glShaderSource", t_glShaderSource);
    THUNK("glStencilFunc", glStencilFunc);
    THUNK("glStencilFuncSeparate", glStencilFuncSeparate);
    THUNK("glStencilMask", glStencilMask);
    THUNK("glStencilMaskSeparate", glStencilMaskSeparate);
    THUNK("glStencilOp", glStencilOp);
    THUNK("glStencilOpSeparate", glStencilOpSeparate);
    THUNK("glTexImage2D", glTexImage2D);
    THUNK("glTexParameterf", glTexParameterf);
    THUNK("glTexParameterfv", t_glTexParameterfv);
    THUNK("glTexParameteri", glTexParameteri);
    THUNK("glTexParameteriv", t_glTexParameteriv);
    THUNK("glTexSubImage2D", glTexSubImage2D);
    THUNK("glUniform1f", glUniform1f);
    THUNK("glUniform1fv", t_glUniform1fv);
    THUNK("glUniform1i", glUniform1i);
    THUNK("glUniform1iv", t_glUniform1iv);
    THUNK("glUniform2f", glUniform2f);
    THUNK("glUniform2fv", t_glUniform2fv);
    THUNK("glUniform2i", glUniform2i);
    THUNK("glUniform2iv", t_glUniform2iv);
    THUNK("glUniform3f", glUniform3f);
    THUNK("glUniform3fv", t_glUniform3fv);
    THUNK("glUniform3i", glUniform3i);
    THUNK("glUniform3iv", t_glUniform3iv);
    THUNK("glUniform4f", glUniform4f);
    THUNK("glUniform4fv", t_glUniform4fv);
    THUNK("glUniform4i", glUniform4i);
    THUNK("glUniform4iv", t_glUniform4iv);
    THUNK("glUniformMatrix2fv", t_glUniformMatrix2fv);
    THUNK("glUniformMatrix3fv", t_glUniformMatrix3fv);
    THUNK("glUniformMatrix4fv", t_glUniformMatrix4fv);
    THUNK("glUseProgram", glUseProgram);
    THUNK("glValidateProgram", glValidateProgram);
    THUNK("glVertexAttrib1f", glVertexAttrib1f);
    THUNK("glVertexAttrib1fv", t_glVertexAttrib1fv);
    THUNK("glVertexAttrib2f", glVertexAttrib2f);
    THUNK("glVertexAttrib2fv", t_glVertexAttrib2fv);
    THUNK("glVertexAttrib3f", glVertexAttrib3f);
    THUNK("glVertexAttrib3fv", t_glVertexAttrib3fv);
    THUNK("glVertexAttrib4f", glVertexAttrib4f);
    THUNK("glVertexAttrib4fv", t_glVertexAttrib4fv);
    THUNK("glVertexAttribPointer", t_glVertexAttribPointer);
    THUNK("glViewport", glViewport);
}
