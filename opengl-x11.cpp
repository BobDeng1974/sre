/*

Copyright (c) 2014 Harm Hanemaaijer <fgenfb@yahoo.com>

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

*/

// X11 (low-level) OpenGL(GLX) interface.
// Uses X11 handling in x11-common.c.
// Currently requires freeglut to get around issues with
// initializing GLEW and destroying a temporary window at
// start-up.

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <sys/time.h>
#include <assert.h>
#include <GL/glew.h>
#include <GL/glxew.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
// We don´t really use glut, but only use it to create a temporary
// rendering context before initializing GLEW.
#include <GL/glut.h>
#include <GL/freeglut_ext.h>

#include "sre.h"
#include "sreBackend.h"
#include "x11-common.h"
#include "gui-common.h"

class sreBackendGLX11 : public sreBackend {
public :
    virtual void Initialize(int *argc, char ***argv, int requested_width, int requested_height,
         int& actual_width, int& actual_height, unsigned int backend_flags);
    virtual void Finalize();
    virtual void GLSwapBuffers();
    virtual void GLSync();
    virtual double GetCurrentTime();
    virtual void ProcessGUIEvents();
    virtual void ToggleFullScreenMode(int& width, int& height, bool pan_with_mouse);
    virtual void HideCursor();
    virtual void RestoreCursor();
    virtual void WarpCursor(int x, int y);
};

sreBackend *sreCreateBackendGLX11() {
    sreBackend *b = new sreBackendGLX11;
    b->name = "OpenGL 3.0+ X11 (low-level)";
    return b;
}

typedef struct
{
   uint32_t screen_width;
   uint32_t screen_height;
   Display *XDisplay;
   GLXWindow XWindow;
   GLXContext context;
} GL_STATE_T;

static GL_STATE_T *state;
static GLXFBConfig chosen_fb_config;

#define check() assert(glGetError() == 0)

// Default Open GL context settings: 8-bit truecolor with alpha.

static const GLint visual_attributes_base[] = {
    GLX_X_RENDERABLE, True,
    GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
    GLX_RENDER_TYPE, GLX_RGBA_BIT,
    GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
    GLX_RED_SIZE, 8,
    GLX_GREEN_SIZE, 8,
    GLX_BLUE_SIZE, 8,
    GLX_ALPHA_SIZE, 8,
    GLX_DOUBLEBUFFER, True,
    None,
};

static const GLint visual_attributes_stencil_buffer[] = {
    GLX_DEPTH_SIZE, 24,
    GLX_STENCIL_SIZE, 8,
    None
};

static const GLint visual_attributes_no_stencil_buffer[] = {
    GLX_DEPTH_SIZE, 24,
    None
};

static const GLint visual_attributes_multi_sample[] = {
    GLX_SAMPLE_BUFFERS, 1,	// Use MSAA
    GLX_SAMPLES, 4,
    None
};

#define MAX_VISUAL_ATTRIBUTES_SIZE ((sizeof(visual_attributes_base) + \
    sizeof(visual_attributes_stencil_buffer) + \
    sizeof(visual_attributes_multi_sample)) / sizeof(GLint))

static void AddAttributes(GLint *attributes, const GLint *extra_attributes) {
    int i = 0;
    while (attributes[i] != None)
        i++;
    int j = 0;
    while (extra_attributes[j] != None) {
        attributes[i] = extra_attributes[j];
        i++;
        j++;
    }
    attributes[i] = None;
}

void CloseGlutWindow() {
}

void sreBackendGLX11::Initialize(int *argc, char ***argv, int requested_width, int requested_height,
int& actual_width, int& actual_height, unsigned int backend_flags) {
    // To call GLX functions with glew, we need to call glewInit()
    // first, but it needs an active OpenGL context to be present. So we have to
    // create a temporary GL context.
    glutInit(argc, *argv);
    int glut_window = glutCreateWindow("GLEW Test");
    GLenum err = glewInit();
    if (GLEW_OK != err) {
        /* Problem: glewInit failed, something is seriously wrong. */
        sreFatalError("Error: %s", glewGetErrorString(err));
    }
    sreMessage(SRE_MESSAGE_INFO, "Status: Using GLEW %s.", glewGetString(GLEW_VERSION));
//    glutCloseFunc(CloseGlutWindow);
    glutHideWindow();
    glutDestroyWindow(glut_window);

    // Problem: How to completely hide the glut window? It doesn't disappear. Requires
    // freeglut to fix.
    glutMainLoopEvent();

    state = (GL_STATE_T *)malloc(sizeof(GL_STATE_T));
    // Clear application state
    memset(state, 0, sizeof(*state));

    if (!X11OpenDisplay())
        sreFatalError("Failed to open X display.\n");
    state->XDisplay = (Display *)X11GetDisplay();
//    XDestroyWindow(state->XDisplay, glut_window);

    // Require GLX >= 1.3
    GLint glx_major, glx_minor;
    GLint result = glXQueryVersion(state->XDisplay, &glx_major, &glx_minor);
    if (glx_major < 1 || (glx_major == 1 && glx_minor < 3)) {
        sreFatalError("Error: GLX version major reported is %d.%d, need at least 1.3",
            glx_major, glx_minor);
    }
    sreMessage(SRE_MESSAGE_LOG, "GLX version: %d.%d", glx_major, glx_minor);

    // Create visual attributes structure.
    GLint visual_attributes[MAX_VISUAL_ATTRIBUTES_SIZE];
    visual_attributes[0] = None;
    AddAttributes(visual_attributes, visual_attributes_base);
    if (backend_flags & SRE_BACKEND_INIT_FLAG_STENCIL_BUFFER)
        AddAttributes(visual_attributes, visual_attributes_stencil_buffer);
    else
        AddAttributes(visual_attributes, visual_attributes_no_stencil_buffer);
    if (backend_flags & SRE_BACKEND_INIT_FLAG_MULTI_SAMPLE)
        AddAttributes(visual_attributes, visual_attributes_multi_sample);

    // Obtain appropriate GLX framebuffer configurations.
    GLint num_config;
    GLXFBConfig *fb_config = glXChooseFBConfig(state->XDisplay, X11GetScreenIndex(),
        visual_attributes, &num_config);
    assert(result != GL_FALSE);
    if (num_config == 0) {
        sreFatalError("GLX returned no suitable framebuffer configurations.");
    }
    sreMessage(SRE_MESSAGE_LOG, "OpenGL (GLX): %d framebuffer configurations returned.\n", num_config);
    for (int i = 0; i < num_config; i++ ) {
        XVisualInfo *vi = glXGetVisualFromFBConfig(state->XDisplay, fb_config[i]);
        if (vi) {
            int samp_buf, samples;
            glXGetFBConfigAttrib(state->XDisplay, fb_config[i], GLX_SAMPLE_BUFFERS, &samp_buf);
            glXGetFBConfigAttrib(state->XDisplay, fb_config[i], GLX_SAMPLES, &samples);
            sreMessage(SRE_MESSAGE_LOG, "  Matching framebuffer config %d, visual ID 0x%2x: SAMPLE_BUFFERS = %d,"
                " SAMPLES = %d", i, vi->visualid, samp_buf, samples);
            XFree(vi);
        }
    }
    chosen_fb_config = fb_config[0];
    XFree(fb_config);

    XVisualInfo *vi = glXGetVisualFromFBConfig(state->XDisplay, chosen_fb_config);
    sreMessage(SRE_MESSAGE_LOG, "Chosen visual ID = 0x%x\n", vi->visualid );

    // Create an X window with that visual.
    X11CreateWindow(requested_width, requested_height, vi, "SRE OpenGL 3.0+ X11 demo");
    XFree(vi);
    state->XWindow = (GLXWindow)X11GetWindow();
    // Would be better to use the actual size of the returned window.
    actual_width = requested_width;
    actual_height = requested_height;

    if (!GLXEW_ARB_create_context) {
        // Create GLX rendering context.
        sreMessage(SRE_MESSAGE_INFO, "Creating old-style (GLX 1.3) context.");
        state->context = glXCreateNewContext(state->XDisplay, chosen_fb_config,
            GLX_RGBA_TYPE, 0, True);
     }
    else {
        int context_attribs[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
            GLX_CONTEXT_MINOR_VERSION_ARB, 0,
            //GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
            None
        };
        sreMessage(SRE_MESSAGE_INFO, "Creating OpenGL 3.0 context.");
        state->context = glXCreateContextAttribsARB(state->XDisplay,
            chosen_fb_config, 0, True, context_attribs);
    }
    assert(state->context != NULL);
    check();

    int stencil_bits = 8;
    int depth_bits = 24;

    sreMessage(SRE_MESSAGE_INFO, "Opened OpenGL context of size %d x %d with 32-bit pixels, %d-bit depthbuffer and %d-bit stencil.", actual_width,
        actual_height, depth_bits, stencil_bits);
    // Verifying that context is a direct context
    if (!glXIsDirect(state->XDisplay, state->context)) {
        sreMessage(SRE_MESSAGE_INFO, "Indirect GLX rendering context obtained.");
    }
    else {
        sreMessage(SRE_MESSAGE_INFO, "Direct GLX rendering context obtained.");
    }

    glXMakeCurrent(state->XDisplay, X11GetWindow(), state->context);
    check();
    glXWaitX();
    glXWaitGL();
}

void sreBackendGLX11::Finalize() {
    // Clear screen.
    glClear(GL_COLOR_BUFFER_BIT);
    sre_internal_backend->GLSync();

    glXMakeCurrent(state->XDisplay, 0, NULL);
    glXDestroyContext(state->XDisplay, state->context);
    X11DestroyWindow();
    X11CloseDisplay();
}

void sreBackendGLX11::GLSwapBuffers() {
    glXSwapBuffers(state->XDisplay, state->XWindow);
}

void sreBackendGLX11::GLSync() {
    glXSwapBuffers(state->XDisplay, state->XWindow);
    glXWaitGL();
}

double sreBackendGLX11::GetCurrentTime() {
    return X11GetCurrentTime();
}

void sreBackendGLX11::ProcessGUIEvents() {
    X11ProcessGUIEvents();
}

void sreBackendGLX11::ToggleFullScreenMode(int& width, int& height, bool pan_with_mouse) {
    X11ToggleFullScreenMode(width, height, pan_with_mouse);
}

void sreBackendGLX11::HideCursor() {
    X11HideCursor();
}

void sreBackendGLX11::RestoreCursor() {
    X11RestoreCursor();
}

void sreBackendGLX11::WarpCursor(int x, int y) {
    X11WarpCursor(x, y);
}

