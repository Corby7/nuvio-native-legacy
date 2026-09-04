// A bridge between the TV's GLES2 and the Mac's OpenGL.
//
// It exists so the UI can be seen on the computer instead of waiting through the
// package/send/install cycle on the TV for every layout tweak. What runs on the
// Mac is the SAME drawing code; only the function names and the shader dialect
// change. The TV remains the final word — colour, scale and performance only
// count measured there.
#ifndef NV_GL_COMPAT_H
#define NV_GL_COMPAT_H

#ifdef __APPLE__
  #define GL_SILENCE_DEPRECATION 1
  #include <OpenGL/gl.h>
  #include <OpenGL/glext.h>
  // Framebuffer de objeto e core no GLES2 e extensao no OpenGL 2.1 do Mac.
  #define glGenFramebuffers        glGenFramebuffersEXT
  #define glBindFramebuffer        glBindFramebufferEXT
  #define glFramebufferTexture2D   glFramebufferTexture2DEXT
  #define glCheckFramebufferStatus glCheckFramebufferStatusEXT
  #define glDeleteFramebuffers     glDeleteFramebuffersEXT
  #define glGenerateMipmap         glGenerateMipmapEXT
  #define GL_FRAMEBUFFER           GL_FRAMEBUFFER_EXT
  #define GL_COLOR_ATTACHMENT0     GL_COLOR_ATTACHMENT0_EXT
  #define GL_FRAMEBUFFER_COMPLETE  GL_FRAMEBUFFER_COMPLETE_EXT
  // GLSL 1.20 has no precision qualifiers; declaring them breaks compilation,
  // so they become nothing.
  #define NV_GLSL_PREFIX \
    "#version 120\n" \
    "#define lowp\n#define mediump\n#define highp\n"
#else
  #include <GLES2/gl2.h>
  #define NV_GLSL_PREFIX "precision mediump float;\n"
#endif

#endif
