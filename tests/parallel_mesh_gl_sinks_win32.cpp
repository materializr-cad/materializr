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
// __stdcall directly, not the APIENTRY macro: on a real Windows CI run this
// macro failed to expand as expected in this translation unit (each sink
// below was misparsed as an implicit-int redeclaration of a variable named
// APIENTRY), even though GLEW's own headers use it internally without
// issue. __stdcall is a native MSVC keyword, not dependent on any macro
// resolving correctly, and matches what PFNGL*PROC's typedefs expect on x86;
// on x64 (this project's vcpkg triplet) __stdcall is a no-op, so this is
// safe either way.
void __stdcall sinkGenVertexArrays(GLsizei n, GLuint* ids) { while (n--) *ids++ = 0; }
void __stdcall sinkGenBuffers(GLsizei n, GLuint* ids) { while (n--) *ids++ = 0; }
void __stdcall sinkBindVertexArray(GLuint) {}
void __stdcall sinkBindBuffer(GLenum, GLuint) {}
void __stdcall sinkBufferData(GLenum, GLsizeiptr, const void*, GLenum) {}
void __stdcall sinkEnableVertexAttribArray(GLuint) {}
void __stdcall sinkVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) {}
void __stdcall sinkDeleteVertexArrays(GLsizei, const GLuint*) {}
void __stdcall sinkDeleteBuffers(GLsizei, const GLuint*) {}
void __stdcall sinkGenFramebuffers(GLsizei n, GLuint* ids) { while (n--) *ids++ = 0; }
void __stdcall sinkBindFramebuffer(GLenum, GLuint) {}
void __stdcall sinkFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint) {}
void __stdcall sinkGenRenderbuffers(GLsizei n, GLuint* ids) { while (n--) *ids++ = 0; }
void __stdcall sinkBindRenderbuffer(GLenum, GLuint) {}
void __stdcall sinkRenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei) {}
void __stdcall sinkRenderbufferStorageMultisample(GLenum, GLsizei, GLenum, GLsizei, GLsizei) {}
void __stdcall sinkFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint) {}

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
