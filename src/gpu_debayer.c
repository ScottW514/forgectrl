/*
 * gpu_debayer.c - Bayer demosaic on the i.MX6 GC880 GPU (etnaviv/Mesa)
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Three fragment-shader passes turn the raw BGGR frame into the planar
 * YUV420 layout the CODA encoders take, with the same superpixel
 * semantics as debayer_bggr_half_yuv420: every 2x2 Bayer quad becomes
 * one pixel (R and B taken, the two greens averaged), luma per pixel,
 * chroma per 2x2 pixel block, JFIF full-range ITU-R 601.
 *
 * Both ends are dmabufs: the V4L2 capture buffers are imported as
 * textures and the encoder's own OUTPUT buffer is imported as the render
 * target, so the frame never crosses the CPU. Buffers are byte streams
 * to the GPU, not pictures: both sides are bound as XRGB8888 with the
 * width in texels a quarter of the width in bytes, and the shaders do
 * the byte indexing (an XRGB8888 texel's memory bytes 0..3 read back as
 * .b .g .r .a in the shader, and write the same way through
 * gl_FragColor).
 *
 * GLES2 has no integer textures and no multiple render targets on this
 * class of GPU, which is why the passes are three (Y, U, V) and the
 * indexing is float math with floor(). A raw frame taller than the
 * GPU's maximum texture size is imported as row tiles of one dmabuf
 * (the EGL import offset selects the tile) and each pass draws once per
 * tile under a scissor; the conversion is quad-local, so tile seams at
 * multiple-of-4 raw rows are exact.
 *
 * Everything is dlopen'd (libEGL.so.1, libGLESv2.so.2) and probed, so a
 * build has no GL dependency and an image without Mesa (or a kernel
 * without etnaviv) just falls back to the NEON path at runtime.
 */
#define _GNU_SOURCE
#include "gpu_debayer.h"
#include "fflog.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------- minimal EGL/GLES2 ABI */

typedef void          *EGLDisplay;
typedef void          *EGLContext;
typedef void          *EGLConfig;
typedef void          *EGLSurface;
typedef void          *EGLImageKHR;
typedef int            EGLint;
typedef unsigned int   EGLBoolean;
typedef unsigned int   EGLenum;

typedef unsigned int   GLenum;
typedef unsigned int   GLuint;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLboolean;
typedef float          GLfloat;
typedef char           GLchar;
typedef unsigned int   GLbitfield;

#define EGL_NO_DISPLAY              ((EGLDisplay)0)
#define EGL_NO_CONTEXT              ((EGLContext)0)
#define EGL_NO_SURFACE              ((EGLSurface)0)
#define EGL_NO_IMAGE                ((EGLImageKHR)0)
#define EGL_FALSE                   0
#define EGL_TRUE                    1
#define EGL_NONE                    0x3038
#define EGL_EXTENSIONS              0x3055
#define EGL_HEIGHT                  0x3056
#define EGL_WIDTH                   0x3057
#define EGL_RENDERABLE_TYPE         0x3040
#define EGL_OPENGL_ES2_BIT          0x0004
#define EGL_CONTEXT_CLIENT_VERSION  0x3098
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_LINUX_DMA_BUF_EXT       0x3270
#define EGL_LINUX_DRM_FOURCC_EXT    0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT   0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT  0x3274

#define DRM_FORMAT_XRGB8888         0x34325258  /* 'XR24' */

#define GL_FALSE                    0
#define GL_TRIANGLES                0x0004
#define GL_UNSIGNED_BYTE            0x1401
#define GL_FLOAT                    0x1406
#define GL_RGBA                     0x1908
#define GL_FRAGMENT_SHADER          0x8B30
#define GL_VERTEX_SHADER            0x8B31
#define GL_COMPILE_STATUS           0x8B81
#define GL_LINK_STATUS              0x8B82
#define GL_TEXTURE_2D               0x0DE1
#define GL_TEXTURE0                 0x84C0
#define GL_TEXTURE_MIN_FILTER       0x2801
#define GL_TEXTURE_MAG_FILTER       0x2800
#define GL_TEXTURE_WRAP_S           0x2802
#define GL_TEXTURE_WRAP_T           0x2803
#define GL_NEAREST                  0x2600
#define GL_CLAMP_TO_EDGE            0x812F
#define GL_FRAMEBUFFER              0x8D40
#define GL_RENDERBUFFER             0x8D41
#define GL_COLOR_ATTACHMENT0        0x8CE0
#define GL_FRAMEBUFFER_COMPLETE     0x8CD5
#define GL_MAX_TEXTURE_SIZE         0x0D33
#define GL_MAX_RENDERBUFFER_SIZE    0x84E8
#define GL_SCISSOR_TEST             0x0C11
#define GL_EXTENSIONS               0x1F03
#define GL_NO_ERROR                 0

struct egl_api {
    void *lib;
    EGLint      (*GetError)(void);
    void       *(*GetProcAddress)(const char *);
    const char *(*QueryString)(EGLDisplay, EGLint);
    EGLBoolean  (*Initialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean  (*Terminate)(EGLDisplay);
    EGLBoolean  (*BindAPI)(EGLenum);
    EGLBoolean  (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *,
                                EGLint, EGLint *);
    EGLContext  (*CreateContext)(EGLDisplay, EGLConfig, EGLContext,
                                 const EGLint *);
    EGLBoolean  (*DestroyContext)(EGLDisplay, EGLContext);
    EGLBoolean  (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface,
                               EGLContext);
    /* extension procs */
    EGLDisplay  (*GetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
    EGLImageKHR (*CreateImageKHR)(EGLDisplay, EGLContext, EGLenum,
                                  void *, const EGLint *);
    EGLBoolean  (*DestroyImageKHR)(EGLDisplay, EGLImageKHR);
};

struct gles_api {
    void *lib;
    GLenum  (*GetError)(void);
    const unsigned char *(*GetString)(GLenum);
    void    (*GetIntegerv)(GLenum, GLint *);
    GLuint  (*CreateShader)(GLenum);
    void    (*ShaderSource)(GLuint, GLsizei, const GLchar *const *,
                            const GLint *);
    void    (*CompileShader)(GLuint);
    void    (*GetShaderiv)(GLuint, GLenum, GLint *);
    void    (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void    (*DeleteShader)(GLuint);
    GLuint  (*CreateProgram)(void);
    void    (*AttachShader)(GLuint, GLuint);
    void    (*LinkProgram)(GLuint);
    void    (*GetProgramiv)(GLuint, GLenum, GLint *);
    void    (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void    (*UseProgram)(GLuint);
    void    (*DeleteProgram)(GLuint);
    GLint   (*GetUniformLocation)(GLuint, const GLchar *);
    GLint   (*GetAttribLocation)(GLuint, const GLchar *);
    void    (*Uniform1f)(GLint, GLfloat);
    void    (*Uniform2f)(GLint, GLfloat, GLfloat);
    void    (*Uniform3f)(GLint, GLfloat, GLfloat, GLfloat);
    void    (*Uniform1i)(GLint, GLint);
    void    (*GenTextures)(GLsizei, GLuint *);
    void    (*DeleteTextures)(GLsizei, const GLuint *);
    void    (*BindTexture)(GLenum, GLuint);
    void    (*ActiveTexture)(GLenum);
    void    (*TexParameteri)(GLenum, GLenum, GLint);
    void    (*GenFramebuffers)(GLsizei, GLuint *);
    void    (*DeleteFramebuffers)(GLsizei, const GLuint *);
    void    (*BindFramebuffer)(GLenum, GLuint);
    void    (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    GLenum  (*CheckFramebufferStatus)(GLenum);
    void    (*GenRenderbuffers)(GLsizei, GLuint *);
    void    (*DeleteRenderbuffers)(GLsizei, const GLuint *);
    void    (*BindRenderbuffer)(GLenum, GLuint);
    void    (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void    (*Scissor)(GLint, GLint, GLsizei, GLsizei);
    void    (*Enable)(GLenum);
    void    (*Disable)(GLenum);
    void    (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean,
                                   GLsizei, const void *);
    void    (*EnableVertexAttribArray)(GLuint);
    void    (*DrawArrays)(GLenum, GLint, GLsizei);
    void    (*Finish)(void);
    /* extension procs */
    void    (*EGLImageTargetTexture2DOES)(GLenum, void *);
    void    (*EGLImageTargetRenderbufferStorageOES)(GLenum, void *);
};

#define MAX_RAW_SLOTS   8
#define MAX_DST_SLOTS   2
#define MAX_TILES       4

struct dst {
    int         attached;
    EGLImageKHR img[3];     /* Y, U, V plane views of the one dmabuf */
    GLuint      rb[3];
    GLuint      fbo[3];
};

struct gpu_debayer {
    struct egl_api  egl;
    struct gles_api gl;
    EGLDisplay      dpy;
    EGLContext      ctx;
    int             raw_w, raw_h;
    int             hflip;
    int             tiles;          /* row tiles the raw frame imports as */
    int             tile_h;         /* raw rows per tile (multiple of 4) */
    EGLImageKHR     raw_img[MAX_RAW_SLOTS][MAX_TILES];
    GLuint          raw_tex[MAX_RAW_SLOTS][MAX_TILES];
    int             raw_attached[MAX_RAW_SLOTS];
    struct dst      dst[MAX_DST_SLOTS];
    GLuint          prog_y, prog_c;
    /* uniform/attrib locations */
    GLint           y_a_pos, y_u_texsz, y_u_ow, y_u_flip, y_u_rowbase;
    GLint           c_a_pos, c_u_texsz, c_u_ow, c_u_flip, c_u_rowbase,
                    c_u_coef, c_u_off;
    int             max_tex;
    int             dead;
};

/* ------------------------------------------------------------- shaders */

static const char VS_SRC[] =
    "attribute vec2 a_pos;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

/* Byte j of an XRGB8888 texel reads back as .b (j=0), .g (1), .r (2),
 * .a (3); pair p of a texel is bytes (2p, 2p+1). An even raw row's pairs
 * are (B, G1), an odd row's are (G2, R). */
#define SP_FETCH \
    "vec3 sp(float c, float ry) {\n"                                     \
    "  float ix = floor(c * 0.5);\n"                                     \
    "  float p  = c - 2.0 * ix;\n"                                       \
    "  vec2 tc = vec2(ix + 0.5, ry - u_rowbase + 0.5) / u_texsz;\n"      \
    "  vec4 e = texture2D(u_raw, tc);\n"                                 \
    "  vec4 o = texture2D(u_raw, tc + vec2(0.0, 1.0 / u_texsz.y));\n"    \
    "  vec2 eb = mix(vec2(e.b, e.g), vec2(e.r, e.a), p);\n"              \
    "  vec2 ob = mix(vec2(o.b, o.g), vec2(o.r, o.a), p);\n"              \
    "  return vec3(ob.y, (eb.y + ob.x) * 0.5, eb.x);\n"  /* R G B */     \
    "}\n"

/* One output texel = 4 luma bytes = 4 superpixels. gl_FragCoord is in
 * output texels; raw rows for output row ty are 2ty and 2ty+1 (the sp()
 * helper adds the odd row itself). */
static const char FS_Y_SRC[] =
    "PRECISION\n"
    "uniform sampler2D u_raw;\n"
    "uniform vec2  u_texsz;\n"
    "uniform float u_ow;\n"
    "uniform float u_flip;\n"
    "uniform float u_rowbase;\n"
    SP_FETCH
    "float luma(vec3 rgb) {\n"
    "  return dot(rgb, vec3(0.299, 0.587, 0.114));\n"
    "}\n"
    "void main() {\n"
    "  float tx = floor(gl_FragCoord.x);\n"
    "  float ry = 2.0 * floor(gl_FragCoord.y);\n"
    "  vec4 y;\n"
    "  for (int k = 0; k < 4; k++) {\n"
    "    float s = 4.0 * tx + float(k);\n"
    "    float c = mix(s, u_ow - 1.0 - s, u_flip);\n"
    "    y[k] = luma(sp(c, ry));\n"
    "  }\n"
    "  gl_FragColor = vec4(y[2], y[1], y[0], y[3]);\n"
    "}\n";

/* One output texel = 4 chroma bytes = 4 chroma sites; each site averages
 * a 2x2 block of superpixels (raw 4x4). u_coef/u_off select U or V. */
static const char FS_C_SRC[] =
    "PRECISION\n"
    "uniform sampler2D u_raw;\n"
    "uniform vec2  u_texsz;\n"
    "uniform float u_ow;\n"
    "uniform float u_flip;\n"
    "uniform float u_rowbase;\n"
    "uniform vec3  u_coef;\n"
    "uniform float u_off;\n"
    SP_FETCH
    "void main() {\n"
    "  float tx = floor(gl_FragCoord.x);\n"
    "  float ty = floor(gl_FragCoord.y);\n"
    "  float uvw = u_ow * 0.25;\n"
    "  vec4 outv;\n"
    "  for (int k = 0; k < 4; k++) {\n"
    "    float cc = 4.0 * tx + float(k);\n"
    "    float sx = mix(cc, uvw - 1.0 - cc, u_flip);\n"
    "    vec3 acc = vec3(0.0);\n"
    "    for (int sy = 0; sy < 2; sy++) {\n"
    "      float ry = 2.0 * (2.0 * ty + float(sy));\n"
    "      acc += sp(2.0 * sx,       ry);\n"
    "      acc += sp(2.0 * sx + 1.0, ry);\n"
    "    }\n"
    "    outv[k] = dot(acc * 0.25, u_coef) + u_off;\n"
    "  }\n"
    "  gl_FragColor = vec4(outv[2], outv[1], outv[0], outv[3]);\n"
    "}\n";

/* ------------------------------------------------------------- loading */

static int load_egl(struct egl_api *e)
{
    e->lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!e->lib) {
        fflog(LOG_INFO, "gpu: no libEGL.so.1: %s", dlerror());
        return -1;
    }
#define E(name) \
    do { \
        *(void **)&e->name = dlsym(e->lib, "egl" #name); \
        if (!e->name) { \
            fflog(LOG_WARNING, "gpu: libEGL lacks egl" #name); \
            return -1; \
        } \
    } while (0)
    E(GetError); E(GetProcAddress); E(QueryString); E(Initialize);
    E(Terminate); E(BindAPI); E(ChooseConfig); E(CreateContext);
    E(DestroyContext); E(MakeCurrent);
#undef E
    *(void **)&e->GetPlatformDisplayEXT =
        e->GetProcAddress("eglGetPlatformDisplayEXT");
    *(void **)&e->CreateImageKHR = e->GetProcAddress("eglCreateImageKHR");
    *(void **)&e->DestroyImageKHR = e->GetProcAddress("eglDestroyImageKHR");
    return 0;
}

static int load_gles(struct gles_api *g,
                     void *(*proc)(const char *))
{
    g->lib = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!g->lib) {
        fflog(LOG_INFO, "gpu: no libGLESv2.so.2: %s", dlerror());
        return -1;
    }
#define G(name) \
    do { \
        *(void **)&g->name = dlsym(g->lib, "gl" #name); \
        if (!g->name) { \
            fflog(LOG_WARNING, "gpu: libGLESv2 lacks gl" #name); \
            return -1; \
        } \
    } while (0)
    G(GetError); G(GetString); G(GetIntegerv); G(CreateShader);
    G(ShaderSource); G(CompileShader); G(GetShaderiv);
    G(GetShaderInfoLog); G(DeleteShader); G(CreateProgram);
    G(AttachShader); G(LinkProgram); G(GetProgramiv);
    G(GetProgramInfoLog); G(UseProgram); G(DeleteProgram);
    G(GetUniformLocation); G(GetAttribLocation); G(Uniform1f);
    G(Uniform2f); G(Uniform3f); G(Uniform1i); G(GenTextures);
    G(DeleteTextures); G(BindTexture); G(ActiveTexture); G(TexParameteri);
    G(GenFramebuffers); G(DeleteFramebuffers); G(BindFramebuffer);
    G(FramebufferRenderbuffer); G(CheckFramebufferStatus);
    G(GenRenderbuffers); G(DeleteRenderbuffers); G(BindRenderbuffer);
    G(Viewport); G(Scissor); G(Enable); G(Disable);
    G(VertexAttribPointer); G(EnableVertexAttribArray); G(DrawArrays);
    G(Finish);
#undef G
    *(void **)&g->EGLImageTargetTexture2DOES =
        proc("glEGLImageTargetTexture2DOES");
    *(void **)&g->EGLImageTargetRenderbufferStorageOES =
        proc("glEGLImageTargetRenderbufferStorageOES");
    if (!g->EGLImageTargetTexture2DOES ||
        !g->EGLImageTargetRenderbufferStorageOES) {
        fflog(LOG_WARNING, "gpu: GL_OES_EGL_image procs missing");
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------- GL helpers */

static GLuint compile(gpu_debayer_t *g, GLenum kind, const char *src)
{
    GLuint sh = g->gl.CreateShader(kind);
    g->gl.ShaderSource(sh, 1, (const GLchar *const *)&src, NULL);
    g->gl.CompileShader(sh);
    GLint ok = 0;
    g->gl.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        g->gl.GetShaderInfoLog(sh, sizeof(log), NULL, log);
        fflog(LOG_WARNING, "gpu: shader compile failed: %.200s", log);
        g->gl.DeleteShader(sh);
        return 0;
    }
    return sh;
}

/* Substitute the PRECISION line and build a program; highp first, since
 * the row indexing needs more mantissa than fp16 offers on tall frames,
 * then mediump so a stack that lacks fragment highp still comes up (the
 * indexing error is checked by the one-shot compare, see cam.c). */
static GLuint build_prog(gpu_debayer_t *g, const char *fs_tmpl)
{
    static const char *prec[2] = { "precision highp float;",
                                   "precision mediump float;" };
    for (int i = 0; i < 2; i++) {
        char fs[4096];
        const char *at = strstr(fs_tmpl, "PRECISION");
        snprintf(fs, sizeof(fs), "%.*s%s%s", (int)(at - fs_tmpl), fs_tmpl,
                 prec[i], at + strlen("PRECISION"));
        GLuint v = compile(g, GL_VERTEX_SHADER, VS_SRC);
        GLuint f = compile(g, GL_FRAGMENT_SHADER, fs);
        if (!v || !f) {
            if (v)
                g->gl.DeleteShader(v);
            if (f)
                g->gl.DeleteShader(f);
            continue;
        }
        GLuint p = g->gl.CreateProgram();
        g->gl.AttachShader(p, v);
        g->gl.AttachShader(p, f);
        g->gl.LinkProgram(p);
        g->gl.DeleteShader(v);
        g->gl.DeleteShader(f);
        GLint ok = 0;
        g->gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
        if (ok) {
            if (i > 0)
                fflog(LOG_INFO, "gpu: fragment highp unavailable, "
                      "using mediump");
            return p;
        }
        char log[512] = "";
        g->gl.GetProgramInfoLog(p, sizeof(log), NULL, log);
        fflog(LOG_WARNING, "gpu: program link failed: %.200s", log);
        g->gl.DeleteProgram(p);
    }
    return 0;
}

static EGLImageKHR import_bytes(gpu_debayer_t *g, int fd, size_t offset,
                                int w_bytes, int h, int pitch)
{
    EGLint attrs[] = {
        EGL_WIDTH,                    w_bytes / 4,
        EGL_HEIGHT,                   h,
        EGL_LINUX_DRM_FOURCC_EXT,     DRM_FORMAT_XRGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT,    fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, pitch,
        EGL_NONE
    };
    EGLImageKHR img = g->egl.CreateImageKHR(g->dpy, EGL_NO_CONTEXT,
                                            EGL_LINUX_DMA_BUF_EXT, NULL,
                                            attrs);
    if (img == EGL_NO_IMAGE)
        fflog(LOG_WARNING, "gpu: dmabuf import failed (fd %d, %dx%d bytes, "
              "pitch %d, offset %zu): egl 0x%x", fd, w_bytes, h, pitch,
              offset, g->egl.GetError());
    return img;
}

static int gl_ok(gpu_debayer_t *g, const char *what)
{
    GLenum e = g->gl.GetError();
    if (e == GL_NO_ERROR)
        return 1;
    fflog(LOG_WARNING, "gpu: %s: gl 0x%x", what, e);
    return 0;
}

/* ------------------------------------------------------------ open/close */

gpu_debayer_t *gpu_debayer_open(int raw_w, int raw_h, int hflip)
{
    if (raw_w % 16 || raw_h % 4) {
        fflog(LOG_WARNING, "gpu: unsupported raw geometry %dx%d",
              raw_w, raw_h);
        return NULL;
    }

    gpu_debayer_t *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    g->raw_w = raw_w;
    g->raw_h = raw_h;
    g->hflip = !!hflip;

    if (load_egl(&g->egl))
        goto fail;

    const char *cext = g->egl.QueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!g->egl.GetPlatformDisplayEXT || !cext ||
        !strstr(cext, "platform_surfaceless")) {
        fflog(LOG_INFO, "gpu: no surfaceless EGL platform");
        goto fail;
    }
    g->dpy = g->egl.GetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
                                          NULL, NULL);
    if (g->dpy == EGL_NO_DISPLAY ||
        !g->egl.Initialize(g->dpy, NULL, NULL)) {
        fflog(LOG_INFO, "gpu: EGL display init failed (0x%x) - "
              "no usable GPU?", g->egl.GetError());
        g->dpy = EGL_NO_DISPLAY;
        goto fail;
    }

    const char *dext = g->egl.QueryString(g->dpy, EGL_EXTENSIONS);
    if (!dext || !strstr(dext, "EGL_EXT_image_dma_buf_import") ||
        !strstr(dext, "EGL_KHR_surfaceless_context") ||
        !g->egl.CreateImageKHR || !g->egl.DestroyImageKHR) {
        fflog(LOG_INFO, "gpu: required EGL extensions missing");
        goto fail;
    }

    static const EGLenum EGL_OPENGL_ES_API = 0x30A0;
    g->egl.BindAPI(EGL_OPENGL_ES_API);
    EGLConfig cfg;
    EGLint ncfg = 0;
    static const EGLint cfg_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE
    };
    if (!g->egl.ChooseConfig(g->dpy, cfg_attrs, &cfg, 1, &ncfg) ||
        ncfg < 1) {
        fflog(LOG_INFO, "gpu: no GLES2 EGL config");
        goto fail;
    }
    static const EGLint ctx_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
    };
    g->ctx = g->egl.CreateContext(g->dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (g->ctx == EGL_NO_CONTEXT ||
        !g->egl.MakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                            g->ctx)) {
        fflog(LOG_INFO, "gpu: GLES2 context failed (0x%x)",
              g->egl.GetError());
        goto fail;
    }

    if (load_gles(&g->gl, g->egl.GetProcAddress))
        goto fail;
    const char *glext = (const char *)g->gl.GetString(GL_EXTENSIONS);
    if (!glext || !strstr(glext, "GL_OES_EGL_image")) {
        fflog(LOG_INFO, "gpu: GL_OES_EGL_image missing");
        goto fail;
    }

    g->gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &g->max_tex);
    GLint max_rb = 0;
    g->gl.GetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb);
    /* The raw import is raw_w/4 texels wide (byte packing), so only the
     * height can exceed the cap; tile by rows. Output planes are half
     * and quarter height and never wider than the raw import. */
    if (raw_w / 4 > g->max_tex || raw_h / 2 > max_rb) {
        fflog(LOG_INFO, "gpu: frame exceeds GPU limits (tex %d, rb %d)",
              g->max_tex, max_rb);
        goto fail;
    }
    g->tiles = 1;
    g->tile_h = raw_h;
    while (g->tile_h > g->max_tex) {
        g->tiles *= 2;
        g->tile_h /= 2;
    }
    if (g->tiles > MAX_TILES || g->tile_h % 4) {
        fflog(LOG_INFO, "gpu: cannot tile %d rows under max texture %d",
              raw_h, g->max_tex);
        goto fail;
    }

    g->prog_y = build_prog(g, FS_Y_SRC);
    g->prog_c = build_prog(g, FS_C_SRC);
    if (!g->prog_y || !g->prog_c)
        goto fail;
    g->y_a_pos    = g->gl.GetAttribLocation(g->prog_y, "a_pos");
    g->y_u_texsz  = g->gl.GetUniformLocation(g->prog_y, "u_texsz");
    g->y_u_ow     = g->gl.GetUniformLocation(g->prog_y, "u_ow");
    g->y_u_flip   = g->gl.GetUniformLocation(g->prog_y, "u_flip");
    g->y_u_rowbase = g->gl.GetUniformLocation(g->prog_y, "u_rowbase");
    g->c_a_pos    = g->gl.GetAttribLocation(g->prog_c, "a_pos");
    g->c_u_texsz  = g->gl.GetUniformLocation(g->prog_c, "u_texsz");
    g->c_u_ow     = g->gl.GetUniformLocation(g->prog_c, "u_ow");
    g->c_u_flip   = g->gl.GetUniformLocation(g->prog_c, "u_flip");
    g->c_u_rowbase = g->gl.GetUniformLocation(g->prog_c, "u_rowbase");
    g->c_u_coef   = g->gl.GetUniformLocation(g->prog_c, "u_coef");
    g->c_u_off    = g->gl.GetUniformLocation(g->prog_c, "u_off");

    if (!gl_ok(g, "setup"))
        goto fail;

    fflog(LOG_INFO, "gpu: GLES2 debayer up for %dx%d (%d tile%s of %d "
          "rows, max texture %d)", raw_w, raw_h, g->tiles,
          g->tiles > 1 ? "s" : "", g->tile_h, g->max_tex);
    return g;

fail:
    gpu_debayer_close(g);
    return NULL;
}

void gpu_debayer_close(gpu_debayer_t *g)
{
    if (!g)
        return;
    if (g->dpy != EGL_NO_DISPLAY) {
        if (g->ctx != EGL_NO_CONTEXT) {
            g->egl.MakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                               g->ctx);
            for (int i = 0; i < MAX_RAW_SLOTS; i++)
                for (int t = 0; t < MAX_TILES; t++) {
                    if (g->raw_tex[i][t])
                        g->gl.DeleteTextures(1, &g->raw_tex[i][t]);
                    if (g->raw_img[i][t])
                        g->egl.DestroyImageKHR(g->dpy, g->raw_img[i][t]);
                }
            for (int i = 0; i < MAX_DST_SLOTS; i++)
                for (int pl = 0; pl < 3; pl++) {
                    if (g->dst[i].fbo[pl])
                        g->gl.DeleteFramebuffers(1, &g->dst[i].fbo[pl]);
                    if (g->dst[i].rb[pl])
                        g->gl.DeleteRenderbuffers(1, &g->dst[i].rb[pl]);
                    if (g->dst[i].img[pl])
                        g->egl.DestroyImageKHR(g->dpy, g->dst[i].img[pl]);
                }
            if (g->prog_y)
                g->gl.DeleteProgram(g->prog_y);
            if (g->prog_c)
                g->gl.DeleteProgram(g->prog_c);
            g->egl.MakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                               EGL_NO_CONTEXT);
            g->egl.DestroyContext(g->dpy, g->ctx);
        }
        g->egl.Terminate(g->dpy);
    }
    if (g->gl.lib)
        dlclose(g->gl.lib);
    if (g->egl.lib)
        dlclose(g->egl.lib);
    free(g);
}

/* ------------------------------------------------------------- attach */

int gpu_debayer_attach_raw(gpu_debayer_t *g, int idx, int fd)
{
    if (g->dead || idx < 0 || idx >= MAX_RAW_SLOTS)
        return -1;
    for (int t = 0; t < g->tiles; t++) {
        EGLImageKHR img = import_bytes(g, fd,
                                       (size_t)t * g->tile_h * g->raw_w,
                                       g->raw_w, g->tile_h, g->raw_w);
        if (img == EGL_NO_IMAGE)
            return -1;
        GLuint tex;
        g->gl.GenTextures(1, &tex);
        g->gl.BindTexture(GL_TEXTURE_2D, tex);
        g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            GL_NEAREST);
        g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                            GL_NEAREST);
        g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            GL_CLAMP_TO_EDGE);
        g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            GL_CLAMP_TO_EDGE);
        g->gl.EGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
        if (!gl_ok(g, "raw texture bind")) {
            g->gl.DeleteTextures(1, &tex);
            g->egl.DestroyImageKHR(g->dpy, img);
            return -1;
        }
        g->raw_img[idx][t] = img;
        g->raw_tex[idx][t] = tex;
    }
    g->raw_attached[idx] = 1;
    return 0;
}

int gpu_debayer_attach_dst(gpu_debayer_t *g, int slot, int fd,
                           int y_stride, size_t buf_len)
{
    if (g->dead || slot < 0 || slot >= MAX_DST_SLOTS)
        return -1;
    const int ow = g->raw_w / 2, oh = g->raw_h / 2;
    if (y_stride < ow || y_stride % 8) {
        fflog(LOG_WARNING, "gpu: unusable encoder stride %d", y_stride);
        return -1;
    }
    size_t y_sz = (size_t)y_stride * oh;
    size_t c_sz = (size_t)(y_stride / 2) * (oh / 2);
    if (buf_len < y_sz + 2 * c_sz) {
        fflog(LOG_WARNING, "gpu: encoder buffer too small (%zu)", buf_len);
        return -1;
    }
    struct {
        size_t off;
        int    w_bytes, h, pitch;
    } plane[3] = {
        { 0,            ow,     oh,     y_stride     },
        { y_sz,         ow / 2, oh / 2, y_stride / 2 },
        { y_sz + c_sz,  ow / 2, oh / 2, y_stride / 2 },
    };
    struct dst *d = &g->dst[slot];
    for (int pl = 0; pl < 3; pl++) {
        d->img[pl] = import_bytes(g, fd, plane[pl].off, plane[pl].w_bytes,
                                  plane[pl].h, plane[pl].pitch);
        if (d->img[pl] == EGL_NO_IMAGE)
            return -1;
        g->gl.GenRenderbuffers(1, &d->rb[pl]);
        g->gl.BindRenderbuffer(GL_RENDERBUFFER, d->rb[pl]);
        g->gl.EGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER,
                                                   d->img[pl]);
        g->gl.GenFramebuffers(1, &d->fbo[pl]);
        g->gl.BindFramebuffer(GL_FRAMEBUFFER, d->fbo[pl]);
        g->gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                      GL_RENDERBUFFER, d->rb[pl]);
        GLenum st = g->gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE || !gl_ok(g, "dst attach")) {
            fflog(LOG_WARNING, "gpu: plane %d framebuffer incomplete "
                  "(0x%x) - the GPU cannot render into this buffer",
                  pl, st);
            return -1;
        }
    }
    d->attached = 1;
    return 0;
}

/* ------------------------------------------------------------- convert */

static const GLfloat FS_TRI[6] = { -1.f, -1.f, 3.f, -1.f, -1.f, 3.f };

/* Draw one pass over every tile: `pass` selects the plane (0 Y, 1 U,
 * 2 V), which fixes the program, the output geometry and the number of
 * raw rows one output row consumes. */
static void draw_pass(gpu_debayer_t *g, int idx, struct dst *d, int pass)
{
    const int ow = g->raw_w / 2, oh = g->raw_h / 2;
    const int out_w = (pass == 0 ? ow : ow / 2) / 4;
    const int out_h = pass == 0 ? oh : oh / 2;
    const int rows_per = pass == 0 ? 2 : 4;   /* raw rows per output row */
    GLuint prog = pass == 0 ? g->prog_y : g->prog_c;
    GLint a_pos = pass == 0 ? g->y_a_pos : g->c_a_pos;

    g->gl.UseProgram(prog);
    g->gl.BindFramebuffer(GL_FRAMEBUFFER, d->fbo[pass]);
    g->gl.Viewport(0, 0, out_w, out_h);
    g->gl.VertexAttribPointer((GLuint)a_pos, 2, GL_FLOAT, GL_FALSE, 0,
                              FS_TRI);
    g->gl.EnableVertexAttribArray((GLuint)a_pos);
    if (pass == 0) {
        g->gl.Uniform2f(g->y_u_texsz, (GLfloat)(g->raw_w / 4),
                        (GLfloat)g->tile_h);
        g->gl.Uniform1f(g->y_u_ow, (GLfloat)ow);
        g->gl.Uniform1f(g->y_u_flip, (GLfloat)g->hflip);
    } else {
        g->gl.Uniform2f(g->c_u_texsz, (GLfloat)(g->raw_w / 4),
                        (GLfloat)g->tile_h);
        g->gl.Uniform1f(g->c_u_ow, (GLfloat)ow);
        g->gl.Uniform1f(g->c_u_flip, (GLfloat)g->hflip);
        static const GLfloat coef[2][3] = {
            { -0.169f, -0.331f,  0.500f },      /* Cb */
            {  0.500f, -0.419f, -0.081f },      /* Cr */
        };
        const GLfloat *c = coef[pass - 1];
        g->gl.Uniform3f(g->c_u_coef, c[0], c[1], c[2]);
        g->gl.Uniform1f(g->c_u_off, 128.f / 255.f);
    }

    if (g->tiles == 1) {
        g->gl.Uniform1f(pass == 0 ? g->y_u_rowbase : g->c_u_rowbase, 0.f);
        g->gl.BindTexture(GL_TEXTURE_2D, g->raw_tex[idx][0]);
        g->gl.DrawArrays(GL_TRIANGLES, 0, 3);
        return;
    }
    g->gl.Enable(GL_SCISSOR_TEST);
    for (int t = 0; t < g->tiles; t++) {
        int y0 = t * g->tile_h / rows_per;
        g->gl.Scissor(0, y0, out_w, g->tile_h / rows_per);
        g->gl.Uniform1f(pass == 0 ? g->y_u_rowbase : g->c_u_rowbase,
                        (GLfloat)(t * g->tile_h));
        g->gl.BindTexture(GL_TEXTURE_2D, g->raw_tex[idx][t]);
        g->gl.DrawArrays(GL_TRIANGLES, 0, 3);
    }
    g->gl.Disable(GL_SCISSOR_TEST);
}

int gpu_debayer_convert(gpu_debayer_t *g, int idx, int slot)
{
    if (g->dead || idx < 0 || idx >= MAX_RAW_SLOTS ||
        !g->raw_attached[idx] || slot < 0 || slot >= MAX_DST_SLOTS ||
        !g->dst[slot].attached)
        return -1;

    g->gl.ActiveTexture(GL_TEXTURE0);
    for (int pass = 0; pass < 3; pass++)
        draw_pass(g, idx, &g->dst[slot], pass);
    g->gl.Finish();
    if (!gl_ok(g, "convert")) {
        g->dead = 1;
        return -1;
    }
    return 0;
}
