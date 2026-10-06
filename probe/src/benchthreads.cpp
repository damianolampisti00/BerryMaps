#include "benchthreads.hpp"
#include "bbportlog.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QtGui/QImage>
#include <QVector>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <img/img.h>

#include <stdlib.h>
#include <string.h>

namespace {

QString eglErr()
{
    return QString("0x%1").arg(eglGetError(), 0, 16);
}

QString glStr(GLenum name)
{
    const GLubyte *s = glGetString(name);
    return s ? QString::fromLatin1(reinterpret_cast<const char *>(s)) : QString("(null)");
}

QString stats(const QVector<double> &ms)
{
    if (ms.isEmpty()) return "n/a";
    double sum = 0, mn = ms.at(0), mx = ms.at(0);
    for (int i = 0; i < ms.size(); ++i) {
        sum += ms.at(i);
        mn = qMin(mn, ms.at(i));
        mx = qMax(mx, ms.at(i));
    }
    return QString("media %1 ms (min %2, max %3, n=%4)")
        .arg(sum / ms.size(), 0, 'f', 2).arg(mn, 0, 'f', 2).arg(mx, 0, 'f', 2).arg(ms.size());
}

double elapsedMs(const QElapsedTimer &t)
{
    return t.nsecsElapsed() / 1000000.0;
}

} // namespace

// ---------------------------------------------------------------- GPU

void GlBenchThread::run()
{
    say("[gl] avvio test GPU (EGL pbuffer)");

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
        say("[gl] ERRORE eglInitialize " + eglErr());
        return;
    }
    say(QString("[gl] EGL %1.%2 vendor=%3 version=%4 apis=%5")
                     .arg(major).arg(minor)
                     .arg(eglQueryString(dpy, EGL_VENDOR))
                     .arg(eglQueryString(dpy, EGL_VERSION))
                     .arg(eglQueryString(dpy, EGL_CLIENT_APIS)));
    say(QString("[gl] EGL extensions: %1").arg(eglQueryString(dpy, EGL_EXTENSIONS)));

    EGLint total = 0;
    eglGetConfigs(dpy, 0, 0, &total);
    say(QString("[gl] configurazioni EGL totali: %1").arg(total));

    const EGLint cfgAttrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5,
        EGL_NONE
    };
    EGLConfig cfg = 0;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgAttrs, &cfg, 1, &n) || n < 1) {
        say("[gl] ERRORE nessuna config ES2+pbuffer " + eglErr());
        return;
    }
    const EGLint pbAttrs[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbAttrs);
    if (surf == EGL_NO_SURFACE) {
        say("[gl] ERRORE eglCreatePbufferSurface " + eglErr());
        return;
    }
    const EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) {
        say("[gl] ERRORE contesto ES2 " + eglErr());
        if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
        eglDestroySurface(dpy, surf);
        return;
    }

    say("[gl] GL_VENDOR=" + glStr(GL_VENDOR));
    say("[gl] GL_RENDERER=" + glStr(GL_RENDERER));
    say("[gl] GL_VERSION=" + glStr(GL_VERSION));
    say("[gl] GLSL=" + glStr(GL_SHADING_LANGUAGE_VERSION));
    QStringList exts = glStr(GL_EXTENSIONS).split(' ', QString::SkipEmptyParts);
    say(QString("[gl] %1 estensioni:").arg(exts.size()));
    for (int i = 0; i < exts.size(); i += 4)
        say("[gl]   " + QStringList(exts.mid(i, 4)).join("  "));
    const char *interesting[] = {
        "GL_OES_compressed_ETC1_RGB8_texture", "GL_AMD_compressed_ATC_texture",
        "GL_AMD_compressed_3DC_texture", "GL_OES_rgb8_rgba8", "GL_EXT_texture_format_BGRA8888",
        "GL_OES_texture_npot", "GL_OES_mapbuffer", "GL_OES_vertex_array_object",
        "GL_EXT_discard_framebuffer", "GL_OES_standard_derivatives"
    };
    for (unsigned i = 0; i < sizeof(interesting) / sizeof(interesting[0]); ++i)
        say(QString("[gl]   %1: %2").arg(interesting[i])
                         .arg(exts.contains(interesting[i]) ? "SI" : "no"));

    GLint v = 0, dims[2] = { 0, 0 };
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v);
    say(QString("[gl] GL_MAX_TEXTURE_SIZE=%1").arg(v));
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &v);
    say(QString("[gl] GL_MAX_TEXTURE_IMAGE_UNITS=%1").arg(v));
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &v);
    say(QString("[gl] GL_MAX_VERTEX_ATTRIBS=%1").arg(v));
    glGetIntegerv(GL_MAX_VARYING_VECTORS, &v);
    say(QString("[gl] GL_MAX_VARYING_VECTORS=%1").arg(v));
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &v);
    say(QString("[gl] GL_MAX_RENDERBUFFER_SIZE=%1").arg(v));
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, dims);
    say(QString("[gl] GL_MAX_VIEWPORT_DIMS=%1x%2").arg(dims[0]).arg(dims[1]));

    // Texture upload: 20 fresh textures per format (first upload of each one
    // includes the allocation, like a newly arrived map tile), then 10 updates
    // of an existing texture with glTexSubImage2D.
    const int W = 512, H = 512, N = 20;
    QByteArray pixels(W * H * 4, '\0');
    for (int i = 0; i < pixels.size(); ++i) pixels[i] = char(i * 31);
    GLuint tex[N];
    glGenTextures(N, tex);
    struct Fmt { GLenum format; GLenum type; const char *name; } fmts[] = {
        { GL_RGB,  GL_UNSIGNED_SHORT_5_6_5, "RGB565" },
        { GL_RGBA, GL_UNSIGNED_BYTE,        "RGBA8888" }
    };
    for (int f = 0; f < 2; ++f) {
        QVector<double> create, update;
        for (int i = 0; i < N; ++i) {
            glBindTexture(GL_TEXTURE_2D, tex[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            QElapsedTimer t; t.start();
            glTexImage2D(GL_TEXTURE_2D, 0, fmts[f].format, W, H, 0, fmts[f].format, fmts[f].type, pixels.constData());
            glFinish();
            create << elapsedMs(t);
        }
        glBindTexture(GL_TEXTURE_2D, tex[0]);
        for (int i = 0; i < 10; ++i) {
            QElapsedTimer t; t.start();
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, fmts[f].format, fmts[f].type, pixels.constData());
            glFinish();
            update << elapsedMs(t);
        }
        GLenum err = glGetError();
        say(QString("[gl] upload 512x512 %1 nuova texture: %2").arg(fmts[f].name).arg(stats(create)));
        say(QString("[gl] upload 512x512 %1 aggiornamento: %2  glError=0x%3")
                         .arg(fmts[f].name).arg(stats(update)).arg(err, 0, 16));
    }
    QVector<double> mip;
    for (int i = 0; i < 5; ++i) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        QElapsedTimer t; t.start();
        glGenerateMipmap(GL_TEXTURE_2D);
        glFinish();
        mip << elapsedMs(t);
    }
    say(QString("[gl] glGenerateMipmap 512x512: %1").arg(stats(mip)));
    glDeleteTextures(N, tex);

    // Only our own context/surface: the EGL display is shared with Cascades in
    // this process, and eglTerminate() on it made the whole app quit (exit code
    // 0, seen on the Q5 right after this test).
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, surf);
    eglReleaseThread();
    say("[gl] fine test GPU");
}

// ---------------------------------------------------------------- decoding

void DecodeBenchThread::run()
{
    say(QString("[decode] avvio, %1 file").arg(m_files.size()));

    img_lib_t ilib = 0;
    int rc = img_lib_attach(&ilib);
    if (rc != IMG_ERR_OK) say(QString("[decode] ERRORE img_lib_attach rc=%1 (salto libimg)").arg(rc));

    const int RUNS = 10;
    for (int f = 0; f < m_files.size(); ++f) {
        const QString path = m_files.at(f);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            say("[decode] non leggibile: " + path);
            continue;
        }
        const QByteArray data = file.readAll();
        file.close();
        say(QString("[decode] %1 (%2 KB)").arg(QFileInfo(path).fileName()).arg(data.size() / 1024.0, 0, 'f', 1));

        QVector<double> qt, qt565;
        QSize size;
        for (int i = 0; i < RUNS; ++i) {
            QElapsedTimer t; t.start();
            QImage im;
            im.loadFromData(data);
            qt << elapsedMs(t);
            size = im.size();
            QElapsedTimer t2; t2.start();
            QImage c = im.convertToFormat(QImage::Format_RGB16);
            qt565 << elapsedMs(t2);
            if (c.isNull()) break;
        }
        say(QString("[decode]   QImage %1x%2: %3").arg(size.width()).arg(size.height()).arg(stats(qt)));
        say(QString("[decode]   + conversione QImage->RGB16: %1").arg(stats(qt565)));

        if (rc != IMG_ERR_OK) continue;
        const QByteArray native = path.toLocal8Bit();
        struct Fmt { img_format_t fmt; const char *name; } fmts[] = {
            { IMG_FMT_PKLE_RGB565,   "RGB565" },
            { IMG_FMT_PKLE_XRGB8888, "XRGB8888" }
        };
        for (int k = 0; k < 2; ++k) {
            QVector<double> ms;
            int lastRc = IMG_ERR_OK;
            for (int i = 0; i < RUNS; ++i) {
                img_t img;
                memset(&img, 0, sizeof(img));
                img.format = fmts[k].fmt;
                img.flags |= IMG_FORMAT;
                QElapsedTimer t; t.start();
                lastRc = img_load_file(ilib, native.constData(), NULL, &img);
                ms << elapsedMs(t);
                if (lastRc != IMG_ERR_OK) break;
                free(img.access.direct.data);
            }
            if (lastRc != IMG_ERR_OK)
                say(QString("[decode]   libimg %1: ERRORE rc=%2").arg(fmts[k].name).arg(lastRc));
            else
                say(QString("[decode]   libimg %1: %2").arg(fmts[k].name).arg(stats(ms)));
        }
    }
    if (rc == IMG_ERR_OK) img_lib_detach(ilib);
    say("[decode] fine");
}

void GlBenchThread::say(const QString &line)
{
    bbportLog(line);
    emit message(line);
}

void DecodeBenchThread::say(const QString &line)
{
    bbportLog(line);
    emit message(line);
}
