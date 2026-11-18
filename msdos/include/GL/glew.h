/* GL/glew.h for the MS-DOS build: there is no OpenGL on DOS. felixterminal
   draws through SDL's software renderer there (g_use_sdl_renderer is always
   true), so the OpenGL paths in sixel_graphics.cpp and kitty_graphics.cpp
   never run; these definitions only let them compile. */
#pragma once
typedef unsigned int GLuint; typedef int GLint; typedef unsigned int GLenum;
typedef float GLfloat; typedef int GLsizei; typedef unsigned char GLboolean;
#define glActiveTexture(...) ((void)0)
#define glAttachShader(...) ((void)0)
#define glBindBuffer(...) ((void)0)
#define glBindTexture(...) ((void)0)
#define glBindVertexArray(...) ((void)0)
#define glBufferData(...) ((void)0)
#define glBufferSubData(...) ((void)0)
#define glCompileShader(...) ((void)0)
#define glDeleteBuffers(...) ((void)0)
#define glDeleteProgram(...) ((void)0)
#define glDeleteShader(...) ((void)0)
#define glDeleteTextures(...) ((void)0)
#define glDeleteVertexArrays(...) ((void)0)
#define glDrawArrays(...) ((void)0)
#define glEnableVertexAttribArray(...) ((void)0)
#define glGenBuffers(...) ((void)0)
#define glGenTextures(...) ((void)0)
#define glGenVertexArrays(...) ((void)0)
#define glGetIntegerv(...) ((void)0)
#define glGetShaderInfoLog(...) ((void)0)
#define glGetShaderiv(...) ((void)0)
#define glGetTexImage(...) ((void)0)
#define glLinkProgram(...) ((void)0)
#define glPixelStorei(...) ((void)0)
#define glShaderSource(...) ((void)0)
#define glTexImage2D(...) ((void)0)
#define glTexParameteri(...) ((void)0)
#define glUniform1i(...) ((void)0)
#define glUniformMatrix4fv(...) ((void)0)
#define glUseProgram(...) ((void)0)
#define glVertexAttribPointer(...) ((void)0)
#define glViewport(...) ((void)0)
#define glCreateProgram(...) 0
#define glCreateShader(...) 0
#define glGetUniformLocation(...) 0
#define GL_ARRAY_BUFFER 1
#define GL_CLAMP_TO_EDGE 2
#define GL_COMPILE_STATUS 3
#define GL_DYNAMIC_DRAW 4
#define GL_FLOAT 5
#define GL_FRAGMENT_SHADER 6
#define GL_LINEAR 7
#define GL_PACK_ALIGNMENT 8
#define GL_RGB 9
#define GL_RGBA 10
#define GL_TEXTURE0 11
#define GL_TEXTURE_2D 12
#define GL_TEXTURE_MAG_FILTER 13
#define GL_TEXTURE_MIN_FILTER 14
#define GL_TEXTURE_WRAP_S 15
#define GL_TEXTURE_WRAP_T 16
#define GL_TRIANGLES 17
#define GL_UNPACK_ALIGNMENT 18
#define GL_UNSIGNED_BYTE 19
#define GL_VERTEX_SHADER 20
#define GL_TEXTURE_BINDING_2D 21
#define GL_FALSE 0
#define GL_TRUE 1
