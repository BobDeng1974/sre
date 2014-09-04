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

// Shader matrix calculations.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <malloc.h>
#include <float.h>
#ifdef OPENGL_ES2
#include <GLES2/gl2.h>
#endif
#ifdef OPENGL
#include <GL/glew.h>
#include <GL/gl.h>
#endif

#include "win32_compat.h"
#include "sre.h"
#include "sre_internal.h"
#include "shader.h"

Matrix4D sre_internal_projection_matrix;
MatrixTransform sre_internal_view_matrix;
Matrix4D sre_internal_view_projection_matrix;

MatrixTransform shadow_map_matrix;
Matrix4D projection_shadow_map_matrix;
Matrix4D cube_shadow_map_matrix;
MatrixTransform shadow_map_lighting_pass_matrix;
// Matrix4D sre_internal_geometry_matrix_scissors_projection_matrix;
Vector3D sre_internal_up_vector;
Vector3D sre_internal_camera_vector;
// The sre_internal_aspect_changed flag will be set at the time of
// the the first projection matrix set-up, and subsequently when the
// aspect ratio changes due to window resizes etc. The value of the
// sre_internal_aspect_ratio variable should be initialized to zero
// or the actual aspect ratio value by sreInitialize before any
// shaders are loaded.
float sre_internal_aspect_ratio;

void GL3Perspective(float fov, float aspect, float nearp, float farp) {
#if 0
    float f = 1 / tan((fov * M_PI/ 180) / 2);
    projection_matrix.Set(
        f / aspect, 0.0f, 0.0f, 0,
        0.0f, f, 0.0f, 0,
        0.0f, 0.0f, (farp + nearp) / (nearp - farp), 2 * farp * nearp / (nearp - farp),
        0.0f, 0.0f, -1.0f, 0.0f);
#endif
    if (aspect != sre_internal_aspect_ratio) {
        sre_internal_aspect_ratio = aspect;
        sre_internal_aspect_changed = true;
    }
    float e = 1 / tanf((fov * M_PI / 180) / 2);
    float n = nearp;
    float l = - n / e;
    float r = n / e;
    float b = - (1 / aspect) * n / e;
    float t = (1 / aspect) * n / e;
    // Set up a projection matrix with an infinite view frustum. We use depth clamping.
    sre_internal_projection_matrix.Set(
        2 * n / (r - l), 0.0f, (r + l) / (r - l), 0.0f,
        0.0f, 2 * n / (t - b), (t + b) / (t - b), 0.0f,
        0.0f, 0.0f, - 1.0f, - 2 * n,
        0.0f, 0.0f, - 1.0f, 0.0f);
}

void GL3PerspectiveTweaked(float fov, float aspect, float nearp, float farp) {
    if (aspect != sre_internal_aspect_ratio) {
        sre_internal_aspect_ratio = aspect;
        sre_internal_aspect_changed = true;
    }
    float e = 1 / tanf((fov * M_PI / 180) / 2);
    float n = nearp;
    float l = - n / e;
    float r = n / e;
    float b = - (1 / aspect) * n / e;
    float t = (1 / aspect) * n / e;
    // Set up a projection matrix with an infinite view frustum. Tweaked with small constant epsilon.
    const float epsilon = 0.001;
    sre_internal_projection_matrix.Set(
        2 * n / (r - l), 0.0f, (r + l) / (r - l), 0.0f,
        0.0f, 2 * n / (t - b), (t + b) / (t - b), 0.0f,
        0.0f, 0.0f, epsilon - 1.0f, n * (epsilon - 2),
        0.0f, 0.0f, - 1.0f, 0.0f);
}

void GL3LookAt(float viewpx, float viewpy, float viewpz, float lookx, float looky, float lookz,
float upx, float upy, float upz) {
    Vector3D F = Vector3D(lookx, looky, lookz) - Vector3D(viewpx, viewpy, viewpz);
    Vector3D Up = Vector3D(upx, upy, upz);
    Vector3D f = F.Normalize();
    sre_internal_camera_vector = f;
    Up.Normalize();
    sre_internal_up_vector = Up;
    Vector3D s = Cross(f, Up);
    Vector3D u = Cross(s, f);
    MatrixTransform M;
    M.Set(
        s.x, s.y, s.z, 0.0f,
        u.x, u.y, u.z, 0.0f,
        - f.x, - f.y, - f.z, 0.0f);
    MatrixTransform T;
    T.AssignTranslation(Vector3D(- viewpx, - viewpy, -viewpz));
    sre_internal_view_matrix = M * T;
    sre_internal_view_projection_matrix = sre_internal_projection_matrix * sre_internal_view_matrix;
//    printf("View-projection matrix:\n");
//    for (int row = 0; row < 4; row++)
//        for (int column = 0; column < 4; column++)
//            printf("%f, ", sre_internal_view_projection_matrix(row, column));
//    printf("\n");
}

void GL3CalculateShadowMapMatrix(Vector3D viewp, Vector3D light_direction, Vector3D x_direction,
Vector3D y_direction, Vector3D dim_min, Vector3D dim_max) {
    MatrixTransform M;
    // Note that the y direction has to be negated in order to preserve the handedness of
    // triangles when rendering the shadow map.
    M.Set(
        x_direction.x, x_direction.y, x_direction.z, 0.0f,
        - y_direction.x, - y_direction.y, - y_direction.z, 0.0f,
        - light_direction.x, - light_direction.y, - light_direction.z, 0.0f);
    MatrixTransform T;
    T.AssignTranslation(- viewp);
    // Set orthographic projection matrix.
    MatrixTransform orthographic_shadow_map_projection_matrix;
    orthographic_shadow_map_projection_matrix.Set(
        2.0f / (dim_max.x - dim_min.x), 0.0f, 0.0f, - (dim_max.x + dim_min.x) / (dim_max.x - dim_min.x),
        0.0f, 2.0f / (dim_max.y - dim_min.y), 0.0f, - (dim_max.y + dim_min.y) / (dim_max.y - dim_min.y),
        0.0f, 0.0f, - 2.0f / dim_max.z, - 1.0f);
    shadow_map_matrix = orthographic_shadow_map_projection_matrix * (M * T);
    // Calculate viewport matrix for lighting pass with shadow map.
    MatrixTransform shadow_map_viewport_matrix;
    shadow_map_viewport_matrix.Set(
        0.5f, 0.0f, 0.0f, 0.5f,
        0.0f, 0.5f, 0.0f, 0.5f,
        0.0f, 0.0f, 0.5f, 0.5f);
    shadow_map_lighting_pass_matrix = shadow_map_viewport_matrix * shadow_map_matrix;
}

void GL3CalculateCubeShadowMapMatrix(Vector3D light_position, Vector3D zdir,
Vector3D up_vector, float zmax) {
    Vector3D fvec = zdir;
    Vector3D s = Cross(fvec, up_vector);
    Vector3D u = Cross(s, fvec);
    MatrixTransform M;
    M.Set(
        s.x, s.y, s.z, 0.0f,
        u.x, u.y, u.z, 0.0f,
        - fvec.x, - fvec.y, - fvec.z, 0.0f);
    MatrixTransform T;
    T.AssignTranslation(- light_position);
    // Calculate the projection matrix with a field of view of 90 degrees.
    float aspect = 1.0;
    float e = 1 / tanf((90.0f * M_PI / 180) / 2);
    float n = zmax * 0.001f;
    float f = zmax;
    float l = - n / e;
    float r = n / e;
    float b = - (1.0f / aspect) * n / e;
    float t = (1.0f / aspect) * n / e;
    Matrix4D shadow_map_projection_matrix;
    shadow_map_projection_matrix.Set(
        2 * n / (r - l), 0.0f, (r + l) / (r - l), 0.0f,
        0.0f, 2 * n / (t - b), (t + b) / (t - b), 0.0f,
        0.0f, 0.0f, - (f + n) / (f - n), - 2 * n * f / (f - n),
        0.0f, 0.0f, - 1.0f, 0.0f);
    cube_shadow_map_matrix = shadow_map_projection_matrix * (M * T);
}

// Calculate projection shadow map matrix, used for generating spotlight shadow
// maps.

void GL3CalculateProjectionShadowMapMatrix(Vector3D viewp, Vector3D light_direction,
Vector3D x_direction, Vector3D y_direction, float zmax) {
    Vector3D fvec = light_direction;
//    Vector3D up_vector = y_direction;
//    Vector3D s = Cross(fvec, up_vector);
//    Vector3D u = Cross(s, fvec);
    Vector3D s = x_direction;
    Vector3D u = y_direction;
    Matrix4D M;
    M.Set(
        s.x, s.y, s.z, 0,
        u.x, u.y, u.z, 0,
        - fvec.x, - fvec.y, - fvec.z, 0,
        0.0f, 0.0f, 0.0f, 1.0f);
    Matrix4D T;
    T.AssignTranslation(- viewp);
    // Calculate the projection matrix with a field of view of 90 degrees.
    float aspect = 1.0;
    float e = 1 / tanf((90.0 * M_PI / 180) / 2);
    float n = zmax * 0.001;
    float f = zmax;
    float l = - n / e;
    float r = n / e;
    float b = - (1 / aspect) * n / e;
    float t = (1 / aspect) * n / e;
    Matrix4D projection_matrix;
    projection_matrix.Set(
        2 * n / (r - l), 0.0f, (r + l) / (r - l), 0.0f,
        0.0f, 2 * n / (t - b), (t + b) / (t - b), 0.0f,
        0.0f, 0.0f, - (f + n) / (f - n), - 2 * n * f / (f - n),
        0.0f, 0.0f, - 1.0f, 0.0f);
    projection_shadow_map_matrix = projection_matrix * (M * T);
    // For spot lights, the shadow map viewport transformation is done in the
    // object lighting pass shaders.
}

void GL3CalculateShadowMapMatrixAlwaysLight() {
    // Set a matrix that produces shadow map coordinates that are out of bounds in x and y, with w coordinate 1
    // and a z-coordinate of 0.5. In the pixel shader, this produces no shadow.
    shadow_map_lighting_pass_matrix.Set(
        0.0f, 0.0f, 0.0f, -2.0f,
        0.0f, 0.0f, 0.0f, -2.0,
        0.0f, 0.0f, 0.0f, 0.5f);
}

#if 0

void GL3CalculateGeometryScissorsMatrixAndSetViewport(const sreScissors& scissors) {
    Matrix4D clip_matrix;
    if (scissors.left == - 1.0 && scissors.right == 1.0 && scissors.bottom == -1.0 && scissors.top == 1.0) {
        sre_internal_geometry_matrix_scissors_projection_matrix = sre_internal_projection_matrix;
        return;
    }
    float left_pixel = round((scissors.left + 1.0) * 0.5 * sre_internal_window_width);
    float width_pixels = round((scissors.right + 1.0) * 0.5 * sre_internal_window_width) - left_pixel;
    float bottom_pixel = round((scissors.bottom + 1.0) * 0.5 * sre_internal_window_height);
    float height_pixels = round((scissors.top + 1.0) * 0.5 * sre_internal_window_height) - bottom_pixel;
// printf("Scissors at (%f, %f) of size (%f, %f)\n", left_pixel, bottom_pixel, width_pixels, height_pixels);
    float factor_x = sre_internal_window_width / width_pixels;
    float factor_y = sre_internal_window_height / height_pixels;
    float left = 2.0 * left_pixel / sre_internal_window_width - 1.0;
    float width = 2.0 * width_pixels / sre_internal_window_width;
    float bottom = 2.0 * bottom_pixel / sre_internal_window_height - 1.0;
    float height = 2.0 * height_pixels / sre_internal_window_height;
    // Set the matrix that maps clip space coordinates to where the scissors rectangle ranges from [-1, 1].
    clip_matrix.Set(
        factor_x, 0, 0, 1.0 - factor_x * (left + width),
        0, factor_y, 0, 1.0 - factor_y * (bottom + height), 
        0, 0, 1.0, 0,
        0, 0, 0, 1.0
        );
    // x = left -> x_clip = factor_x * left + 1.0 - factor_x * (left + width);
    //          = 1.0 + factor_x * (left - left - width)
    //          = 1.0 - factor_x * width;
    //          = 1.0 - factor_x * (2.0 / factor_x) = - 1.0 
    glViewport(left_pixel, bottom_pixel, width_pixels, height_pixels);
    glDepthFunc(GL_LEQUAL);
    sre_internal_geometry_matrix_scissors_projection_matrix = clip_matrix * sre_internal_projection_matrix;
}

#endif
