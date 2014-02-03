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
// initializing GLEW and destroying atemporary window at
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
#include "demo.h"
#include "x11-common.h"
#include "gui-common.h"

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

// Default Open GL context settings: 8-bit truecolor with alpha,
// 24-bit depth buffer, 8-bit stencil, 4 sample MSAA.

static GLint visual_attributes[] = {
    GLX_X_RENDERABLE, True,
    GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
    GLX_RENDER_TYPE, GLX_RGBA_BIT,
    GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
    GLX_RED_SIZE, 8,
    GLX_GREEN_SIZE, 8,
    GLX_BLUE_SIZE, 8,
    GLX_ALPHA_SIZE, 8,
    GLX_DEPTH_SIZE, 24,
    GLX_STENCIL_SIZE, 8,
    GLX_DOUBLEBUFFER, True,
#ifndef NO_MULTI_SAMPLE
    GLX_SAMPLE_BUFFERS, 1,	// Use MSAA
    GLX_SAMPLES, 4,
#endif
    None
};

void CloseGlutWindow() {
}

void InitializeGUI(int *argc, char ***argv) {
    // To call GLX functions with glew, we need to call glewInit()
    // first, but it needs an active OpenGL context to be present. So we have to
    // create a temporary GL context.
    glutInit(argc, *argv);
    int glut_window = glutCreateWindow("GLEW Test");
    GLenum err = glewInit();
    if (GLEW_OK != err) {
        /* Problem: glewInit failed, something is seriously wrong. */
        fprintf(stderr, "Error: %s\n", glewGetErrorString(err));
        exit(1);
    }
    fprintf(stdout, "Status: Using GLEW %s.\n", glewGetString(GLEW_VERSION));
//    glutCloseFunc(CloseGlutWindow);
    glutHideWindow();
    glutDestroyWindow(glut_window);

    // Problem: How to completely hide the glut window? It doesn't disappear. Requires
    // freeglut to fix.
    glutMainLoopEvent();

    state = (GL_STATE_T *)malloc(sizeof(GL_STATE_T));
    // Clear application state
    memset(state, 0, sizeof(*state));

    X11OpenDisplay();
    state->XDisplay = (Display *)X11GetDisplay();
//    XDestroyWindow(state->XDisplay, glut_window);

    // Require GLX >= 1.3
    GLint glx_major, glx_minor;
    GLint result = glXQueryVersion(state->XDisplay, &glx_major, &glx_minor);
    if (glx_major < 1 || (glx_major == 1 && glx_minor < 3)) {
        printf("Error: GLX version major reported is %d.%d, need at least 1.3.\n",
            glx_major, glx_minor);
        exit(1);
    }
    printf("GLX version: %d.%d\n", glx_major, glx_minor);

    // Obtain appropriate GLX framebuffer configurations.
    GLint num_config;
    GLXFBConfig *fb_config = glXChooseFBConfig(state->XDisplay, X11GetScreenIndex(),
        visual_attributes, &num_config);
    assert(result != GL_FALSE);
    if (num_config == 0) {
        printf("GLX returned no suitable framebuffer configurations.\n");
        exit(1);
    }
    printf("OpenGL (GLX): %d framebuffer configurations returned.\n", num_config);
    for (int i = 0; i < num_config; i++ ) {
        XVisualInfo *vi = glXGetVisualFromFBConfig(state->XDisplay, fb_config[i]);
        if (vi) {
            int samp_buf, samples;
            glXGetFBConfigAttrib(state->XDisplay, fb_config[i], GLX_SAMPLE_BUFFERS, &samp_buf);
            glXGetFBConfigAttrib(state->XDisplay, fb_config[i], GLX_SAMPLES, &samples);
            printf( "  Matching framebuffer config %d, visual ID 0x%2x: SAMPLE_BUFFERS = %d,"
                " SAMPLES = %d\n", i, vi->visualid, samp_buf, samples);
            XFree(vi);
        }
    }
    chosen_fb_config = fb_config[0];
    XFree(fb_config);

    XVisualInfo *vi = glXGetVisualFromFBConfig(state->XDisplay, chosen_fb_config);
    printf("Chosen visual ID = 0x%x\n", vi->visualid );
 
    // Create an X window with that visual.
    X11CreateWindow(window_width, window_height, vi, "render OpenGL 3.0+ X11 demo");
    XFree(vi);
    state->XWindow = (GLXWindow)X11GetWindow();

    if (!GLXEW_ARB_create_context) {
        // Create GLX rendering context.
        printf("Creating old-style (GLX 1.3) context.\n");
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
        printf( "Creating OpenGL 3.0 context.\n" );
        state->context = glXCreateContextAttribsARB(state->XDisplay,
            chosen_fb_config, 0, True, context_attribs);
    }
    assert(state->context != NULL);
    check();

    int stencil_bits = 8;
    int depth_bits = 24;

    printf("Opened OpenGL context of size %d x %d with 32-bit pixels, %d-bit depthbuffer and %d-bit stencil.\n", window_width,
        window_height, depth_bits, stencil_bits);
    // Verifying that context is a direct context
    if (!glXIsDirect(state->XDisplay, state->context)) {
        printf("Indirect GLX rendering context obtained.\n");
    }
    else {
        printf("Direct GLX rendering context obtained.\n");
    }

    glXMakeCurrent(state->XDisplay, X11GetWindow(), state->context);
    check();

    sreInitialize(window_width, window_height, GUIGLSwapBuffers);
}

void DeinitializeGUI() {
   // Clear screen.
   glClear(GL_COLOR_BUFFER_BIT);
   GUIGLSync();

   glXMakeCurrent(state->XDisplay, 0, NULL);
   glXDestroyContext(state->XDisplay, state->context);
   X11DestroyWindow();
   X11CloseDisplay();
}

void GUIGLSwapBuffers() {
    glXSwapBuffers(state->XDisplay, state->XWindow);
}

void GUIGLSync() {
    glXSwapBuffers(state->XDisplay, state->XWindow);
    glXWaitGL();
}

double GetCurrentTime() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000;
}

const char *GUIGetBackendName() {
   return "OpenGL X11 (low-level)";
}
