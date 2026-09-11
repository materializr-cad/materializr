// Windows-only. gl_common.h routes everything past GL 1.1 through GLEW on
// this platform (opengl32.dll only exports GL 1.1) - the plain-function
// stubs in parallel_mesh_gl_sinks.cpp are invisible to those calls, which go
// through GLEW's __glew* function-pointer globals instead
// (#define glGenVertexArrays GLEW_GET_FUN(__glewGenVertexArrays), and
// GLEW_GET_FUN(x) is plain x in GLEW's default build - confirmed against the
// real GLEW 2.3.1 release header; this project floats on vcpkg's baseline
// GLEW version, which was not itself inspected, but this architecture has
// been stable for well over a decade). glewInit() would normally populate
// these pointers from a live GL context, which the headless test path never
// creates - so they start out null, and the first call through one of them
// crashes. This static initializer points them at the same no-op sinks the
// GL 1.1 functions get from parallel_mesh_gl_sinks.cpp, before any
// Viewport/ShapeRenderer construction can run.
#include "gl_common.h"

namespace {
void APIENTRY sinkGenVertexArrays(GLsizei n, GLuint* ids) { while (n--) *ids++ = 0; }
void APIENTRY sinkGenBuffers(GLsizei n, GLuint* ids) { while (n--) *ids++ = 0; }
void APIENTRY sinkBindVertexArray(GLuint) {}
void APIENTRY sinkBindBuffer(GLenum, GLuint) {}
void APIENTRY sinkBufferData(GLenum, GLsizeiptr, const void*, GLenum) {}
void APIENTRY sinkEnableVertexAttribArray(GLuint) {}
void APIENTRY sinkVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) {}
void APIENTRY sinkDeleteVertexArrays(GLsizei, const GLuint*) {}
void APIENTRY sinkDeleteBuffers(GLsizei, const GLuint*) {}
void APIENTRY sinkGenFramebuffers(GLsizei n, GLuint* ids) { while (n--) *ids++ = 0; }
void APIENTRY sinkBindFramebuffer(GLenum, GLuint) {}
void APIENTRY sinkFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint) {}
void APIENTRY sinkGenRenderbuffers(GLsizei n, GLuint* ids) { while (n--) *ids++ = 0; }
void APIENTRY sinkBindRenderbuffer(GLenum, GLuint) {}
void APIENTRY sinkRenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei) {}
void APIENTRY sinkRenderbufferStorageMultisample(GLenum, GLsizei, GLenum, GLsizei, GLsizei) {}
void APIENTRY sinkFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint) {}

struct InstallGlewSinks {
    InstallGlewSinks() {
        __glewGenVertexArrays = sinkGenVertexArrays;
        __glewGenBuffers = sinkGenBuffers;
        __glewBindVertexArray = sinkBindVertexArray;
        __glewBindBuffer = sinkBindBuffer;
        __glewBufferData = sinkBufferData;
        __glewEnableVertexAttribArray = sinkEnableVertexAttribArray;
        __glewVertexAttribPointer = sinkVertexAttribPointer;
        __glewDeleteVertexArrays = sinkDeleteVertexArrays;
        __glewDeleteBuffers = sinkDeleteBuffers;
        __glewGenFramebuffers = sinkGenFramebuffers;
        __glewBindFramebuffer = sinkBindFramebuffer;
        __glewFramebufferTexture2D = sinkFramebufferTexture2D;
        __glewGenRenderbuffers = sinkGenRenderbuffers;
        __glewBindRenderbuffer = sinkBindRenderbuffer;
        __glewRenderbufferStorage = sinkRenderbufferStorage;
        __glewRenderbufferStorageMultisample = sinkRenderbufferStorageMultisample;
        __glewFramebufferRenderbuffer = sinkFramebufferRenderbuffer;
    }
};
// Static-init, same pattern as TestSignalInit.cpp: runs before main(), so
// the pointers are live before Application's headless constructor (which
// builds a Viewport/ShapeRenderer) ever executes.
const InstallGlewSinks g_installGlewSinks;
} // namespace
