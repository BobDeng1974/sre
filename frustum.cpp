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

#include <stdlib.h>
#include <stdio.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <float.h>

#include "sre.h"
#include "sre_internal.h"
#include "sre_bounds.h"
#include "win32_compat.h"

#include <dstMatrixMath.h>
#ifdef USE_SIMD
#include <dstSIMDMatrix.h>
#endif

sreFrustum::sreFrustum() {
     frustum_world.AllocateStorage(8, 6);
     frustum_eye.AllocateStorage(8, 6);
     near_clip_volume.AllocateStorage(6);
     shadow_caster_volume.AllocateStorage(12);
     most_recent_frame_changed = 0;
     changing_every_frame = false;
}

// Set frustum projection parameters based on viewing angle, aspect ratio, and near
// and far plane distances.

void sreFrustum::SetParameters(float _angle, float _ratio, float _nearD, float _farD) {
    ratio = _ratio;
    angle = _angle;
    nearD = _nearD;
    farD = _farD;

    float half_angle_rad = angle * 0.5f * M_PI / 180.0f;
    cos_max_half_angular_size = cosf(half_angle_rad);
    sin_max_half_angular_size = sinf(half_angle_rad);
    // Compute width and height of the near and far plane sections.
    float tan_half_angular_size = sin_max_half_angular_size / cos_max_half_angular_size;
    e = 1.0f / tan_half_angular_size;
    nw = nearD * tan_half_angular_size;
    nh = (1.0f / ratio) * nw; 
    fw = farD  * tan_half_angular_size;
    fh = (1.0f / ratio) * ratio;
}

void sreFrustum::Calculate() {
    // Calculate eye-space frustum.
    frustum_eye.hull.vertex[0].Set(nearD / e, (1 / ratio) * nearD / e, - nearD);     // Near-top-right.
    frustum_eye.hull.vertex[1].Set(- nearD / e, (1 / ratio) * nearD / e, - nearD);   // Near-top-left.
    frustum_eye.hull.vertex[2].Set(- nearD / e, - (1 / ratio) * nearD / e, - nearD); // Near-bottom-left.
    frustum_eye.hull.vertex[3].Set(nearD / e, - (1 / ratio) * nearD / e, - nearD);   // Near-bottom-right.
    // farD is an arbitrary distance, we actually have an infinite view frustum.
    frustum_eye.hull.vertex[4].Set(farD / e, (1 / ratio) * farD / e, - farD);        // Far-top-right.
    frustum_eye.hull.vertex[5].Set(- farD / e, (1 / ratio) * farD / e, - farD);      // Far-top-left.
    frustum_eye.hull.vertex[6].Set(- farD / e, - (1 / ratio) * farD / e, - farD);    // Far-bottom-left
    frustum_eye.hull.vertex[7].Set(farD / e, - (1 / ratio) * farD / e, - farD);      // Far-bottom-right.
    Matrix4D inverse_view_matrix = Inverse(sre_internal_view_matrix);
    for (int i = 0; i < 8; i++)
        frustum_world.hull.vertex[i] = (inverse_view_matrix * frustum_eye.hull.vertex[i]).GetPoint3D();
    // Calculate the "centroid". Actually we have an infinite view frustum, but the "centroid" is
    // guaranteed to be in the frustum. Use the sphere bounding volume of the frustum to store the centroid.
    frustum_world.sphere.center.Set(0, 0, 0);
    for (int i = 0; i < 8 ; i++)
        frustum_world.sphere.center += frustum_world.hull.vertex[i];
    frustum_world.sphere.center *= (float)1 / 8;
    // Set the planes in eye space.
    float a = 1 / ratio;
    frustum_eye.plane[0].Set(0, 0, - 1.0, - nearD); // Near plane.
    frustum_eye.plane[1].Set(e / sqrtf(e * e  + 1), 0, - 1 / sqrtf(e * e + 1), 0); // Left plane.
    frustum_eye.plane[2].Set(- e / sqrtf(e * e  + 1), 0, - 1 / sqrtf(e * e + 1), 0); // Right plane.
    frustum_eye.plane[3].Set(0, e / sqrtf(e * e  + a * a), - a / sqrtf(e * e + a * a), 0); // Bottom plane.
    frustum_eye.plane[4].Set(0, - e / sqrtf(e * e  + a * a), - a / sqrtf(e * e + a * a), 0); // Top plane.
    frustum_eye.plane[5].Set(0, 0, 1.0, farD);
    // Convert the planes to world space.
    Matrix4D transpose_view_matrix = Transpose(sre_internal_view_matrix);
    dstMatrixMultiplyVectors1xN(6, transpose_view_matrix, &frustum_eye.plane[0],
        &frustum_world.plane[0]);
//    for (int i = 0; i < 5; i++)
//        printf("plane_world[%d] = (%f, %f, %f, %f)\n", i, plane_world[i].K.x, plane_world[i].K.y, plane_world[i].K.z,
//            plane_world[i].K.w);
    // Adjust the number of planes for intersection checks, if SRE_NU_FRUSTUM_PLANES is equal to 5 we don't use the far plane.
    frustum_world.nu_planes = SRE_NU_FRUSTUM_PLANES;
    frustum_eye.nu_planes = SRE_NU_FRUSTUM_PLANES;
#if SRE_NU_FRUSTUM_PLANES == 6
    // Calculate the bounding sphere if there is a far plane.
    frustum_world.sphere.radius = Magnitude(frustum_world.hull.vertex[4] - frustum_world.sphere.center);
    // When there is a far plane, define a special reduced frustum without a far plane.
    frustum_without_far_plane_world = frustum_world;  // Copy fields.
    frustum_without_far_plane_world.nu_planes = 5;
#endif
#ifndef NO_SHADOW_MAP
    // Set the shadow map region for directional lights.
    if (sre_internal_shadows == SRE_SHADOWS_SHADOW_MAPPING) {
        Point3DPadded vertex[8], vertex2[8];
        sreBoundingVolumeAABB AABB = sre_internal_shadow_map_AABB;
        vertex[0] = Point3DPadded(AABB.dim_min.x, AABB.dim_min.y, AABB.dim_min.z);
        vertex[1] = Point3DPadded(AABB.dim_max.x, AABB.dim_min.y, AABB.dim_min.z);
        vertex[2] = Point3DPadded(AABB.dim_min.x, AABB.dim_max.y, AABB.dim_min.z);
        vertex[3] = Point3DPadded(AABB.dim_max.x, AABB.dim_max.y, AABB.dim_min.z);
        vertex[4] = Point3DPadded(AABB.dim_min.x, AABB.dim_min.y, AABB.dim_max.z);
        vertex[5] = Point3DPadded(AABB.dim_max.x, AABB.dim_min.y, AABB.dim_max.z);
        vertex[6] = Point3DPadded(AABB.dim_min.x, AABB.dim_max.y, AABB.dim_max.z);
        vertex[7] = Point3DPadded(AABB.dim_max.x, AABB.dim_max.y, AABB.dim_max.z);
        dstMatrixMultiplyVectors1xN(8, inverse_view_matrix, vertex, vertex2);
        sreBoundingVolumeAABB empty_AABB;
        empty_AABB.dim_min =
            Point3D(FLT_MAX, FLT_MAX, FLT_MAX);
        empty_AABB.dim_max =
            Point3D(- FLT_MAX, - FLT_MAX, - FLT_MAX);
#ifdef USE_SIMD
        sreBoundingVolumeAABB_SIMD region_AABB;
        region_AABB.Set(empty_AABB);
#else
        sreBoundingVolumeAABB region_AABB = empty_AABB;
#endif
        for (int i = 0; i < 8; i++)
            UpdateAABB(region_AABB, vertex2[i]);
#ifdef USE_SIMD
        region_AABB.Get(shadow_map_region_AABB);
#else
        shadow_map_region_AABB = region_AABB;
#endif
#if 0
        sreMessage(SRE_MESSAGE_LOG, "Shadow map region AABB = (%f, %f, %f) - (%f, %f, %f)\n",
            shadow_map_region_AABB.dim_min.x, shadow_map_region_AABB.dim_min.y, shadow_map_region_AABB.dim_min.z,
            shadow_map_region_AABB.dim_max.x, shadow_map_region_AABB.dim_max.y, shadow_map_region_AABB.dim_max.z);
#endif
    }
#endif
   // Set information indicating when the frustum has changed.
    if (most_recent_frame_changed == sre_internal_current_frame - 1)
        changing_every_frame = true;
    // Before to setting changing_every_frame to false, have to check that
    // the frustum wasn't changed already this frame.
    else if (most_recent_frame_changed != sre_internal_current_frame)
        changing_every_frame = false;
    most_recent_frame_changed = sre_internal_current_frame;
}

// The near-clip volume is the volume bounded by the lightsource and the near plane of the
// view frustum. This is used to determine the shadow volume rendering strategy required
// (depth pass vs depth fail).

void sreFrustum::CalculateNearClipVolume(const Vector4D& lightpos) {
    // Calculate the occlusion pyramid with the tip at the lightsource and the base
    // consisting of the viewport on the near clipping plane.
    // Note: for beam lights, this might be inaccurate.

    // Transform the light position to eye space.
    Vector4D lightpos_eye = sre_internal_view_matrix * lightpos;
    // Calculate the distance of the lightsource to the near plane.
    float d = Dot(lightpos_eye, Vector4D(0, 0, - 1, - nearD));
    if (d < - 0.001) {
        // Light source lies behind the near plane.
        light_position_type = SRE_LIGHT_POSITION_BEHIND_NEAR_PLANE;
    }
    else
    if (d > 0.001) {
        // Light source lies in front of the near plane.
        light_position_type = SRE_LIGHT_POSITION_IN_FRONT_OF_NEAR_PLANE;
    }
    else {
        // Light source lies "in" the near plane.
        light_position_type = SRE_LIGHT_POSITION_IN_NEAR_PLANE;
    }
    near_clip_volume.nu_planes = 0;
    if (light_position_type != SRE_LIGHT_POSITION_IN_NEAR_PLANE) {
        // The light source lies in front of or behind the near plane.
        // Calculate the four planes Ki.
        for (int i = 0; i < 4; i++) {
            // Use the vertices of the near plane in world space (frustum_world.hull.vertex[i]).
            // Calculate the normal.
            Vector3D N = Cross(frustum_world.hull.vertex[i] - frustum_world.hull.vertex[(i - 1) & 3], lightpos.GetVector3D()
                - lightpos.w * frustum_world.hull.vertex[i]);
            if (light_position_type == SRE_LIGHT_POSITION_BEHIND_NEAR_PLANE)
                N = - N;
            // Calculate the parametric plane.
            Vector4D K = 1 / Magnitude(N) * Vector4D(N.x, N.y, N.z, - Dot(N, frustum_world.hull.vertex[i]));
            near_clip_volume.plane[i] = K;
            near_clip_volume.nu_planes = 4;
        }
    }
    Matrix4D transpose_view_matrix = Transpose(sre_internal_view_matrix);
    if (light_position_type != SRE_LIGHT_POSITION_IN_NEAR_PLANE) {
        // Calculate the fifth plane that is coindicent with the near plane and has a normal pointing
        // towards the light source.
        Vector4D K4 = transpose_view_matrix * Vector4D(0, 0, - 1, - nearD);
        if (light_position_type == SRE_LIGHT_POSITION_BEHIND_NEAR_PLANE)
            K4 = -K4;
        near_clip_volume.plane[4] = K4;
        near_clip_volume.nu_planes = 5;
    }
    if (lightpos.w == 1.0f) {
        // For point lights only.
        // Calculate the sixth plane that contains the light position and has a normal direction that
        // points towards the center of the near rectangle.
//        Vector3D N5 = (transpose_view_matrix * Vector4D(0, 0, - nearD, 1)).GetVector3D() -
//            lightpos.GetVector3D();
        Matrix4D inverse_view_matrix = Inverse(sre_internal_view_matrix);
        Vector3D N5 = (inverse_view_matrix * Vector4D(0, 0, - nearD, 1)).GetVector3D() -
            lightpos.GetVector3D();
        Vector4D K5 = 1 / Magnitude(N5) * Vector4D(N5.x, N5.y, N5.z, - Dot(N5, lightpos));
        near_clip_volume.plane[near_clip_volume.nu_planes] = K5;
        near_clip_volume.nu_planes++;
        light_position_type |= SRE_LIGHT_POSITION_POINT_LIGHT;
    }
}

class AdjacentPlane {
public:
    int plane0;
    int plane1;
    int vertex0;
    int vertex1;
};

static const AdjacentPlane adjacent_plane[12] = {
    { 0, 1, 1, 2 },    // Near plane and left plane.
    { 0, 2, 3, 0 },    // Near plane and right plane.
    { 0, 3, 2, 3 },    // Near plane and bottom plane.
    { 0, 4, 0, 1 },    // Near plane and top plane.
    { 1, 3, 2, 6 },    // Left plane and bottom plane.
    { 1, 4, 1, 5 },    // Left plane and top plane.
    { 2, 3, 3, 7 },    // Right plane and bottom plane.
    { 2, 4, 0, 4 },    // Right plane and top plane.
    { 5, 1, 5, 6 },    // Far plane and left plane.
    { 5, 2, 4, 7 },    // Far plane and right plane.
    { 5, 3, 6, 7 },    // Far plane and bottom plane.
    { 5, 4, 4, 5 }     // Far plane and top plane.
};

// Calculate the extension of the view frustum in which objects can cast shadows into the frustum.
// This is used to preselect potential shadow casters for both stencil shadow volumes and
// shadow mapping.
//
// nu_frustum_planes is 6 for shadow mapping with directional lights, 5 otherwise.

void sreFrustum::CalculateShadowCasterVolume(const Vector4D& lightpos, int nu_frustum_planes) {
    nu_frustum_planes = maxf(nu_frustum_planes, SRE_NU_FRUSTUM_PLANES);
    // Note: for beam lights, this might be inaccurate.
    if (lightpos.w == 1.0f && Intersects(lightpos.GetPoint3D(), frustum_world)) {
        // If the point light position is inside the view frustum, we only need to consider the
        // set of visible objects.
        shadow_caster_volume.nu_planes = nu_frustum_planes;
        for (int i = 0; i < nu_frustum_planes; i++)
            shadow_caster_volume.plane[i] = frustum_world.plane[i];
        goto end;
    }
    // Calculate the convex hull enclosing the view frustum and the light source.
    shadow_caster_volume.nu_planes = 0;
    // Calculate the dot products between the frustum planes and the light source.
    float dot[6] DST_ALIGNED(16);
    // The SIMD-accelerated version check for 16-bytes aligned arrays, which is hard to
    // guarantee in this case; otherwise, no SIMD will be used.
#if 1
    dstCalculateDotProductsNx1(nu_frustum_planes, &frustum_world.plane[0],
        lightpos, &dot[0]);
#else
    for (int i = 0; i < nu_frustum_planes; i++)
        dot[i] = Dot(frustum_world.plane[i], lightpos);
#endif
    // For each frustum plane, add it if it is part of the convex hull.
    for (int i = 0; i < nu_frustum_planes; i++)
        if (dot[i] > 0) {
             shadow_caster_volume.plane[shadow_caster_volume.nu_planes] = frustum_world.plane[i];
             shadow_caster_volume.nu_planes++;
        }
//    printf("Shadow caster volume planes: from frustum: %d\n", shadow_caster_volume.nu_planes);
    if (shadow_caster_volume.nu_planes == 0 && lightpos.w == 1.0f) {
        // Special case: the point light source is behind the camera and there is no far plane.
        // As a result, there is no plane from the frustum with a positive dot product. In
        // this case, construct a volume consisting of the lightsource and four planes parallel 
        // to the frustum side planes but starting at the lightsource.
        for (int i = 0; i < 4; i++) {
            // Copy the normal.
            shadow_caster_volume.plane[i] = frustum_world.plane[i];
            // Calculate the distance such that the lightsource is in the plane.
            shadow_caster_volume.plane[i].w = - Dot(frustum_world.plane[i].GetVector3D(),
                lightpos.GetPoint3D());
        }
        shadow_caster_volume.nu_planes = 4;
        goto end;
    }
    // For each pair of adjacent frustum planes, if one has a positive dot product and the other
    // does not, calculate a new plane defined by the edge between those two frustum planes and
    // the position of the light source, making sure the plane's normal direction faces inward.
    // For directional lights, the plane runs parallel to the direction of the light.
    nu_shadow_caster_edges = 0;
    int n;
    if (nu_frustum_planes == 5)
        n = 8;
    else
        n = 12;
    for (int i = 0; i < n; i++)
        if ((dot[adjacent_plane[i].plane0] > 0 && dot[adjacent_plane[i].plane1] <= 0) ||
        (dot[adjacent_plane[i].plane0] <= 0 && dot[adjacent_plane[i].plane1] > 0)) {
//            printf("Setting plane from adjacent planes %d and %d using frustum vertices %d and %d\n",
//                adjacent_plane[i].plane0, adjacent_plane[i].plane1, adjacent_plane[i].vertex0,
//                adjacent_plane[i].vertex1);
            if (lightpos.w == 1.0f)
                shadow_caster_volume.plane[shadow_caster_volume.nu_planes] = dstPlaneFromPoints(
                    frustum_world.hull.vertex[adjacent_plane[i].vertex0],
                    frustum_world.hull.vertex[adjacent_plane[i].vertex1],
                    lightpos.GetPoint3D()
                    );
            else {
                shadow_caster_volume.plane[shadow_caster_volume.nu_planes] = dstPlaneFromPoints(
                    frustum_world.hull.vertex[adjacent_plane[i].vertex0],
                    frustum_world.hull.vertex[adjacent_plane[i].vertex1],
                    frustum_world.hull.vertex[adjacent_plane[i].vertex0] + lightpos.GetVector3D()
                    );
                shadow_caster_edge[nu_shadow_caster_edges][0] = adjacent_plane[i].vertex0;
                shadow_caster_edge[nu_shadow_caster_edges][1] = adjacent_plane[i].vertex1;
                nu_shadow_caster_edges++;
            }
            // Make sure the normal is pointed inward, check by taking the dot product with the frustum
            // "centroid".
            shadow_caster_volume.plane[shadow_caster_volume.nu_planes].OrientPlaneTowardsPoint(
                frustum_world.sphere.center);
            shadow_caster_volume.nu_planes++;
        }
end: ;
#if 0
    printf("Light (%lf, %lf, %lf, %lf), %d planes in shadow caster volume.\n",
        lightpos.x, lightpos.y, lightpos.z, lightpos.w, shadow_caster_volume.nu_planes);
    for (int i = 0; i < shadow_caster_volume.nu_planes; i++)
        printf("Plane %d: (%lf, %lf, %lf, %lf) ", i,
            shadow_caster_volume.plane[i].x, shadow_caster_volume.plane[i].y,
            shadow_caster_volume.plane[i].y, shadow_caster_volume.plane[i].w);
    printf("\n");
#endif
}

void sreScissors::UpdateWithProjectedPoint(float x, float y, double z) {
    if (z >= - 1.001d) {
        // Beyond the near plane.
        z = maxd(- 1.0d, z);
        double depth = 0.5d * z + 0.5d;
        near = mind(depth, near);
        far = maxd(depth, far);
        left = minf(x, left);
        right = maxf(x, right);
        bottom = minf(y, bottom);
        top = maxf(y, top);
        return;
    }
    sreMessage(SRE_MESSAGE_WARNING,
        "Unexpected vertex in front of the near plane in UpdateWorldSpaceBoundingHull "
        "z = %lf", z);
    // In front of the near plane.
    near = 0;
    far = 1.0;
    // We know that the light volume intersects the frustum, so it must extend to
    // both sides of the near plane.
    // Assume it fills the whole viewport (not optimal).
    left = - 1.0f;
    right = 1.0f;
    bottom = - 1.0f;
    top = 1.0f;
}

// Project world space vertex positions onto the image plane and update the scissors region
// to include all vertices. Projected image plane locations are allowed to be outside of
// the visible screen; the scissors region is simply updated to include the image
// plane location even when it outside the visible screen.
//
// The input vertices are assumed to be beyond the near plane (although this is checked). 

void sreScissors::UpdateWithWorldSpaceBoundingHull(Point3DPadded *P, int n) {
    int i = 0;
#ifdef USE_SIMD
    const float *f = (const float *)&sre_internal_view_projection_matrix;
    __simd128_float m_matrix_column0 = simd128_load_float(&f[0]);
    __simd128_float m_matrix_column1 = simd128_load_float(&f[4]);
    __simd128_float m_matrix_column2 = simd128_load_float(&f[8]);
    __simd128_float m_matrix_column3 = simd128_load_float(&f[12]);
    for (; i + 3 < n; i += 4) {
        // Handle four vertices at a time.
        __simd128_float m_Pproj0, m_Pproj1, m_Pproj2, m_Pproj3;
        dstInlineMatrixMultiplyVectors1x4M4x4CMP3P(m_matrix_column0, m_matrix_column1, m_matrix_column2,
            m_matrix_column3, (const float *)&P[i], m_Pproj0, m_Pproj1, m_Pproj2, m_Pproj3);
        // Divide x and y coordinates by w.
        __simd128_float m_Pproj_xy_0011 = simd128_merge_float(m_Pproj0, m_Pproj1, 0, 1, 0, 1);
        __simd128_float m_Pproj_w_0011 = simd128_merge_float(m_Pproj0, m_Pproj1, 3, 3, 3, 3);
        __simd128_float m_Pproj_xy_2233 = simd128_merge_float(m_Pproj2, m_Pproj3, 0, 1, 0, 1);
        __simd128_float m_Pproj_w_2233 = simd128_merge_float(m_Pproj2, m_Pproj3, 3, 3, 3, 3);
        __simd128_float m_xy_0011 = simd128_div_float(m_Pproj_xy_0011, m_Pproj_w_0011);
        __simd128_float m_xy_2233 = simd128_div_float(m_Pproj_xy_2233, m_Pproj_w_2233);
        // Divide z by w with double precision.
        __simd128_float m_Pproj_z_01xx = simd128_interleave_high_float(m_Pproj0, m_Pproj1);
        __simd128_float m_Pproj_z_23xx = simd128_interleave_high_float(m_Pproj2, m_Pproj3);
        __simd128_double md_Pproj_z_01 = simd128_convert_float_double(m_Pproj_z_01xx);
        __simd128_double md_Pproj_z_23 = simd128_convert_float_double(m_Pproj_z_23xx);
        __simd128_float m_Pproj_w_01xx = simd128_select_float(m_Pproj_w_0011, 0, 2, 0, 2);
        __simd128_float m_Pproj_w_23xx = simd128_select_float(m_Pproj_w_2233, 0, 2, 0, 2);
        __simd128_double md_Pproj_w_01 = simd128_convert_float_double(m_Pproj_w_01xx);
        __simd128_double md_Pproj_w_23 = simd128_convert_float_double(m_Pproj_w_23xx);
        __simd128_double md_z_01 = simd128_div_double(md_Pproj_z_01, md_Pproj_w_01);
        __simd128_double md_z_23 = simd128_div_double(md_Pproj_z_23, md_Pproj_w_23);
        double z[4];
        z[0] = simd128_get_double(md_z_01);
        z[1] = simd128_get_double(simd128_select_double(md_z_01, 1, 1));
        z[2] = simd128_get_double(md_z_23);
        z[3] = simd128_get_double(simd128_select_double(md_z_23, 1, 1));
        UpdateWithProjectedPoint(
            simd128_get_float(m_xy_0011),
            simd128_get_float(simd128_shift_right_float(m_xy_0011, 1)),
            z[0]);
        UpdateWithProjectedPoint(
            simd128_get_float(simd128_shift_right_float(m_xy_0011, 2)),
            simd128_get_float(simd128_shift_right_float(m_xy_0011, 3)),
            z[1]);
        UpdateWithProjectedPoint(
            simd128_get_float(m_xy_2233),
            simd128_get_float(simd128_shift_right_float(m_xy_2233, 1)),
            z[2]);
        UpdateWithProjectedPoint(
            simd128_get_float(simd128_shift_right_float(m_xy_2233, 2)),
            simd128_get_float(simd128_shift_right_float(m_xy_2233, 3)),
            z[3]);
    }
#endif
    for (; i < n; i++) {
       Vector4D Pproj = sre_internal_view_projection_matrix * P[i];
       float x = Pproj.x / Pproj.w;
       float y = Pproj.y / Pproj.w;
       double z = (double)Pproj.z / (double)Pproj.w;
       UpdateWithProjectedPoint(x, y, z);
    }
}

// Update the scissors region with a bounding box in world space, specified as vertices.
// The bounding box may be oriented, and may be beyond, in front of, or intersect
// the image plane. Any part that is in front of the image plane (i.e. not visible)
// is clipped to the image plane. When the box is wholly in front of the image plane,
// the function has no effect and returns false.
//
// The scissors region is not clipped to visible screen dimensions and may be larger.
// The scissors region is extended to include the projection of the box onto the image
// plane.
//
// This function requires an ordered bounding box or polyhedron where the first
// four vertices form a plane and the second four vertices form the second plane,
// and there is an edge between the corresponding vertices in the two planes and
// an edge between adjacent vertices within a plane. If n is four, there are only
// four vertices, and there is only one plane.
//
// A return value of false indicates the scissors region is empty (it may actually be
// set to an empty region), while true indicates a valid scissors region was calculated.

bool sreScissors::UpdateWithWorldSpaceBoundingBox(Point3DPadded *P, int n, const sreFrustum& frustum) {
    // Clip against image plane.
    float dist[8];
    int count;
    dstCalculateDotProductsAndCountNegativeNx1(
        n, &P[0], frustum.frustum_world.plane[0], &dist[0], count);
    if (count == 0) {
//        sreMessage(SRE_MESSAGE_INFO, "Bounding box of %d vertices is beyond near plane "
//            " (stCalculateDotProductsAndCountNegativeNx1).", n);
        UpdateWithWorldSpaceBoundingHull(P, n);
        return true;
    }
    if (count == n)
        return false;
    Point3DPadded Q[12];
    int n_clipped = 0;
    // First clip the edges within the two planes. 
    for (int i = 0; i < n; i++) {
        int j = (i & 4) | ((i + 1) & 3);
        if (dist[i] < 0) {
            // The vertex is in front of the near plane.
            // Check the edge going to next vertex.            
            if (dist[j] >= 0) {
                // The edge crosses the near plane to beyond the near plane.
                Vector3D V = P[j] - P[i];
                float t = - dist[i] / Dot(frustum.frustum_world.plane[0], V);
                Q[n_clipped] = P[i] + t * V;
                n_clipped++;
            }
        }
        else {
            // The vertex is beyond the near plane.
            // Check the edge going to the next vertex.
            Q[n_clipped] = P[i];
            n_clipped++;
            if (dist[j] < 0) {
                // The edge crosses the near plane to in front of the near plane.
                Vector3D V = P[j] - P[i];
                float t = - dist[i] / Dot(frustum.frustum_world.plane[0], V);
                Q[n_clipped] = P[i] + t * V;
                n_clipped++;
            }
        }
    }
    // Clip the edges going between planes. The starting vertices have already been
    // output if they were beyond the near plane.
    if (n == 8)
    for (int i = 0; i < 4; i++) {
        int j = i + 4;
        if (dist[i] < 0) {
            // The vertex is in front of the near plane.
            if (dist[j] >= 0) {
                // The edge crosses the near plane to beyond the near plane.
                Vector3D V = P[j] - P[i];
                float t = - dist[i] / Dot(frustum.frustum_world.plane[0], V);
                Q[n_clipped] = P[i] + t * V;
                n_clipped++;
            }
        }
        else {
            // The vertex is beyond the near plane.
            if (dist[j] < 0) {
                // The edge crosses the near plane to in front of the near plane.
                Vector3D V = P[j] - P[i];
                float t = - dist[i] / Dot(frustum.frustum_world.plane[0], V);
                Q[n_clipped] = P[i] + t * V;
                n_clipped++;
            }
        }
    }
#if 0
    sreMessage(SRE_MESSAGE_INFO, "Bounding box of %d vertices clipped to %d vertices.",
        n, n_clipped);
    for (int i = 0 ; i < n_clipped; i++)
        printf("Q[%d] = (%f, %f, %f)\n", i, Q[i].x, Q[i].y, Q[i].z);
#endif
    UpdateWithWorldSpaceBoundingHull(Q, n_clipped);
    if (IsEmptyOrOutside())
        return false;
    else 
        return true;
}

// Update (extend) the scissors region with a box or polyhedron in world space, specified as
// vertices. The bounding box or polyhedron may be oriented, and may be partly or wholly in
// front of the near plane, in which case it is clipped so that only the part beyond the near
// plane remains.
//
// The scissors region is not clipped to visible screen dimensions and may be larger. The 
// scissors region is extended to include the projection of the clipped box or polyhedron
// onto the image plane.
//
// This function requires that the input hull is a box or polyhedron consisting of
// two planes of vertices where the first n / 2 vertices are in the first plane, and
// the second n / 2 vertices are in the second plane, and there is an edge between the
// corresponding vertices in the two planes.
//
// Currently, this function is not fully implemented and just calls the bounding box scissors
// update function when n is equal to 8.

bool sreScissors::UpdateWithWorldSpaceBoundingPolyhedron(Point3DPadded *P, int n,
const sreFrustum& frustum) {
    if (n == 8) {
        UpdateWithWorldSpaceBoundingBox(P, n, frustum);
        if (IsEmptyOrOutside())
            return false;
        else 
            return true;
    }
    sreMessage(SRE_MESSAGE_INFO, "UpdateWithWorldSpaceBoundingPolyhedron not implemented.");
    return false;
}

// Update the scissors region with a bounding pyramid world space, specified as vertex
// positions. The pyramid may be beyond, in front of or intersect the near plane. The
// pyramid is clipped so that only the (visible) part beyond the near plane remains.
//
// The resulting scissors region is not clipped to visible screen dimensions and may be larger.
// The scissors region is extended to include the projection of the clipped pyramid onto the
// image plane.
//
// The pyramid has a tip vertex (index 0) and four, six or seven base vertices. As a result,
// n must be 5, 7 or 8.
//
// The return value of type sreScissorsRegionType indicates whether the scissors region is
// empty, undefined (effectively the whole display), or defined.

sreScissorsRegionType sreScissors::UpdateWithWorldSpaceBoundingPyramid(Point3DPadded *P, int n,
const sreFrustum& frustum) {
    if (n != 5 && n != 7 && n != 8) {
        sreMessage(SRE_MESSAGE_WARNING, "Expected 5, 7 or 8 vertices in bounding pyramid (n = %d).", n);
        return SRE_SCISSORS_REGION_UNDEFINED;
    }
#if 0
    sreMessage(SRE_MESSAGE_INFO, "Pyramid vertices:");
    for (int i = 0 ; i < n; i++)
        sreMessage(SRE_MESSAGE_INFO, "P[%d] = (%f, %f, %f)", i, P[i].x, P[i].y, P[i].z);
#endif

    // Clip against image plane.
    float dist[8];
    // Count the number of pyramid vertices that is outside the near plane.
    int count;
    dstCalculateDotProductsAndCountNegativeNx1(n, &P[0], frustum.frustum_world.plane[0], &dist[0], count);
    if (count == 0) {
        // The pyramid is entirely inside the near plane; no clipping is necessary.
//        printf("Pyramid (n = %d).\n", n);
        UpdateWithWorldSpaceBoundingHull(P, n);
        if (IsEmptyOrOutside())
            return SRE_SCISSORS_REGION_EMPTY;
        else 
            return SRE_SCISSORS_REGION_DEFINED;
    }
    if (count == n)
        return SRE_SCISSORS_REGION_EMPTY;

    // The pyramid has to be clipped against the near plane.

    Point3DPadded Q[16];
    int n_clipped = 0;
    // First clip the edges starting at the tip of the pyramid if necessary.
    if (dist[0] < 0) {
        // The tip vertex is in front of the near plane. Clip all edges from the
        // tip to the base vertices that cross the image plane so that they start
        // at the image plane.
        for (int j = 1; j < n; j++) {
            if (dist[j] < 0)
                continue;
            Vector3D V = P[j] - P[0];
            float dot = Dot(frustum.frustum_world.plane[0], V);
            float t = - dist[0] / dot;
            Q[n_clipped] = P[0] + t * V;
            n_clipped++;
        }
    }
    else {
        // The tip vertex is beyond the near plane.
        // Clip all edges from the tip to the base vertices that cross the image plane
        // so that they end at the image plane.
        Q[0] = P[0];
        n_clipped = 1;
        for (int j = 1; j < n; j++) {
            if (dist[j] >= 0)
                continue;
            Vector3D V = P[j] - P[0];
            float dot = Dot(frustum.frustum_world.plane[0], V);
            float t = - dist[0] / dot;
            Q[n_clipped] = P[0] + t * V;
            n_clipped++;
        }
    }
    // Clip the edges of the base of the pyramid.
    for (int i = 1; i < n; i++) {
        int j;
        if (i == n - 1)
            j = 1;
        else
            j = i + 1;
        if (dist[i] < 0) {
            // The vertex is in front of the near plane. Project the edge if it is crossing the image plane to beyond
            // the near plane.
            if (dist[j] < 0)
                continue;
            Vector3D V = P[j] - P[i];
            float t = - dist[i] / Dot(frustum.frustum_world.plane[0], V);
            Q[n_clipped] = P[i] + t * V;
            n_clipped++;
        }
        else {
            // The vertex is beyond the near plane. Project the edge if it is crossing the image plane to in front of
            // the near plane.
            Q[n_clipped] = P[i];
            n_clipped++;
            if (dist[j] >= 0)
                continue;
            Vector3D V = P[j] - P[i];
            float t = - dist[i] / Dot(frustum.frustum_world.plane[0], V);
            Q[n_clipped] = P[i] + t * V;
            n_clipped++;
        }
    }
#if 0
    sreMessage(SRE_MESSAGE_INFO, "Bounding pyramid of %d vertices clipped to %d vertices.", n, n_clipped);
    for (int i = 0 ; i < n_clipped; i++)
        printf("Q[%d] = (%f, %f, %f)", i, Q[i].x, Q[i].y, Q[i].z);
#endif
    UpdateWithWorldSpaceBoundingHull(Q, n_clipped);
    if (IsEmptyOrOutside())
        return SRE_SCISSORS_REGION_EMPTY;
    else 
        return SRE_SCISSORS_REGION_DEFINED;
}

void sreScissors::Print() {
    sreMessage(SRE_MESSAGE_INFO, "Scissors = (%f, %f), (%f, %f), near/far = (%f, %f).",
        left, right, bottom, top, near, far);
}

// Calculate the light scissors, which is the projection of the light volume
// onto the image plane. When rendering objects for a light, pixels outside of
// the light scissors region will never be lit, so the GPU scissors region can
// set to this region to reduce unnecessary processing and memory access.

void sreFrustum::CalculateLightScissors(sreLight *light) {
    if (light->type & (SRE_LIGHT_SPOT | SRE_LIGHT_BEAM)) {
        // Approximate the bounding volume of the light by the bounding box of the bounding cylinder.
        Vector3D up;
        if (fabsf(light->cylinder.axis.x) < 0.01f && fabsf(light->cylinder.axis.z) < 0.01f) {
            if (light->cylinder.axis.y > 0)
                up = Vector3D(0, 0, - 1.0f);
            else
                up = Vector3D(0, 0, 1.0f);
        }
        else
            up = Vector3D(0, 1.0f, 0);
        // Calculate tangent planes.
        Vector3D N2 = Cross(up, light->cylinder.axis);
        N2.Normalize();
        Vector3D N3 = Cross(light->cylinder.axis, N2);
        Point3DPadded B[8];
        Point3D E1 = light->cylinder.center - 0.5f * light->cylinder.length * light->cylinder.axis;
        Point3D E2 = E1 + light->cylinder.length * light->cylinder.axis;
        B[0] = E2 + light->cylinder.radius * N2 + light->cylinder.radius * N3;
        B[1] = E1 + light->cylinder.radius * N2 + light->cylinder.radius * N3;
        B[2] = E1 - light->cylinder.radius * N2 + light->cylinder.radius * N3;
        B[3] = E2 - light->cylinder.radius * N2 + light->cylinder.radius * N3;
        B[4] = E2 + light->cylinder.radius * N2 - light->cylinder.radius * N3;
        B[5] = E1 + light->cylinder.radius * N2 - light->cylinder.radius * N3;
        B[6] = E1 - light->cylinder.radius * N2 - light->cylinder.radius * N3;
        B[7] = E2 - light->cylinder.radius * N2 - light->cylinder.radius * N3;
        scissors.SetEmptyRegion();
        scissors.UpdateWithWorldSpaceBoundingBox(B, 8, *this);
        scissors.ClampEmptyRegion();
        scissors.ClampRegionAndDepthBounds();
#if 0
        printf("Light %d (spot): ");
        sreMessage(SRE_MESSAGE_INFO,
            "Scissors = (%f, %f, %f, %f)", scissors.left, scissors.right, scissors.bottom, scissors.top);
#endif
        return;
    }
    // Point source light.
    scissors.SetFullRegionAndDepthBounds();
    // Transform the light position from world space to eye space.
    Point3D L = (sre_internal_view_matrix * light->vector).GetPoint3D();
    // Calculate the depth range.
    // The near and far tangent planes parallel to the z-axis can be represented by 4D vectors
    // T= <0, 0, 1.0, Lz + r> and T = <0, 0, 1.0, L.z - r> respectively.
    // Multiply the z coordinates by the projection matrix to arrive at projected depths.
    // Divide by w coordinate. Note that we assume an infinite view frustum.
    float Lz_near = L.z + light->sphere.radius;
    if (Lz_near <= - nearD) {
        Vector4D V = sre_internal_projection_matrix * Vector4D(0, 0, Lz_near, 1.0);
        scissors.near = 0.5 * (double)V.z / (double)V.w + 0.5;
    }
    float Lz_far = L.z - light->sphere.radius;
    if (Lz_far <= - nearD) {
        Vector4D V = sre_internal_projection_matrix * Vector4D(0, 0, Lz_far, 1.0);
        scissors.far = 0.5 * (double)V.z / (double)V.w + 0.5;
    }
    else
        scissors.far = 0;
    // Calculate the determinant D
    float D = 4 * (sqrf(light->sphere.radius) * sqrf(L.x) - (sqrf(L.x) + sqrf(L.z)) *
        (sqrf(light->sphere.radius) - sqrf(L.z)));
    if (D <= 0) {
        // The light source's bounding sphere fills the entire viewport.
        return;
    }
    // Calculate the tangent planes <Nx, 0, Nz, 0> of the light volume.
    float Nx1 = (light->sphere.radius * L.x + sqrtf(D / 4)) /
        (sqrf(L.x) + sqrf(L.z));
    float Nx2 = (light->sphere.radius * L.x - sqrtf(D / 4)) /
        (sqrf(L.x) + sqrf(L.z));
    float Nz1 = (light->sphere.radius - Nx1 * L.x) / L.z;
    float Nz2 = (light->sphere.radius - Nx2 * L.x) / L.z;
    // The point P at which the plane T is tangent to the bounding sphere is
    // given by <Lx - rNx, 0, Lz - rNz, 1>
    float Pz1 = L.z - light->sphere.radius * Nz1;
    float Pz2 = L.z - light->sphere.radius * Nz2;
    if (Pz1 < 0) {
        // Plane 1 may shrink the scissors rectangle.
        float x = Nz1 * e / Nx1;
        float Px1 = L.x - light->sphere.radius * Nx1;
#if 0
        int xvp = 0 + (x + 1) * sre_internal_window_width / 2;
        if (Px1 < L.x) {
            // Left-side boundary
            scissors.left = max(xvp, 0);
        }
        else {
            // Right-side boundary
            scissors.right = min(xvp, sre_internal_window_width);
        }
#else
        if (Px1 < L.x) {
            // Left-side boundary
            scissors.left = maxf(x, - 1.0);
        }
        else {
            // Right-side boundary
            scissors.right = minf(x, 1.0);
        }
#endif
    }
    if (Pz2 < 0) {
        // Plane 2 may shrink the scissors rectangle.
        float x = Nz2 * e / Nx2;
        float Px2 = L.x - light->sphere.radius * Nx2;
#if 0
        int xvp = 0 + (x + 1) * sre_internal_window_width / 2;
        if (Px2 < L.x) {
            // Left-side boundary
            scissors.left = max(xvp, 0);
        }
        else {
            // Right-side boundary
            scissors.right = min(xvp, sre_internal_window_width);
        }
#else
        if (Px2 < L.x) {
            // Left-side boundary
            scissors.left = maxf(x, - 1.0);
        }
        else {
            // Right-side boundary
            scissors.right = minf(x, 1.0);
        }
#endif
    }
    // Calculate the tangent planes <0, Ny, Nz, 0> of the light volume.
    float term1 = sqrf(light->sphere.radius) * sqrf(L.y) -
        (sqrf(L.y) + sqrf(L.z)) *
        (sqrf(light->sphere.radius) - sqrf(L.z));
    float Ny1 = (light->sphere.radius * L.y + sqrtf(term1)) /
        (sqrf(L.y) + sqrf(L.z));
    float Ny2 = (light->sphere.radius * L.y - sqrtf(term1)) /
        (sqrf(L.y) + sqrf(L.z));
    Nz1 = (light->sphere.radius - Ny1 * L.y) / L.z;
    Nz2 = (light->sphere.radius - Ny2 * L.y) / L.z;
    Pz1 = L.z - light->sphere.radius * Nz1;
    Pz2 = L.z - light->sphere.radius * Nz2;
    if (Pz1 < 0) {
        // Plane 3 may shrink the scissors rectangle.
        float y = Nz1 * e * ratio / Ny1;
        float Py1 = L.y - light->sphere.radius * Ny1;
#if 0
        int yvp = 0 + (y + 1) * sre_internal_window_height / 2;
        if (Py1 < L.y) {
            // Bottom boundary.
            scissors.bottom = max(yvp, 0);
        }
        else {
            // Top boundary.
            scissors.top = min(yvp, sre_internal_window_height);
        }
#else
        if (Py1 < L.y) {
            // Bottom boundary.
            scissors.bottom = maxf(y, - 1.0);
        }
        else {
            // Top boundary.
            scissors.top = minf(y, 1.0);
        }
#endif
    }
    if (Pz2 < 0) {
        // Plane 4 may shrink the scissors rectangle.
        float y = Nz2 * e * ratio / Ny2;
        float Py2 = L.y - light->sphere.radius * Ny2;
#if 0
        int yvp = 0 + (y + 1) * sre_internal_window_height / 2;
        if (Py2 < L.y) {
            // Bottom boundary.
            scissors.bottom = max(yvp, 0);
        }
        else {
            // Top boundary.
            scissors.top = min(yvp, sre_internal_window_height);
        }
#else
        if (Py2 < L.y) {
            // Bottom boundary.
            scissors.bottom = maxf(y, - 1.0);
        }
        else {
            // Top boundary.
            scissors.top = minf(y, 1.0);
        }
#endif
    }
//    scissors.ClampRegionToScreen();
//    printf("Scissors = (%f, %f, %f, %f)\n", scissors.left, scissors.right, scissors.bottom, scissors.top);
}

// sreFrustum-related intersection tests.

bool sreFrustum::ObjectIntersectsNearClipVolume(const sreObject& so) const {
    if (light_position_type & SRE_LIGHT_POSITION_IN_NEAR_PLANE) {
        // First check the only plane defined.
        if (Dot(near_clip_volume.plane[0], so.sphere.center) <= - so.sphere.radius)
            return false;
        // The object is outside the near clip volume if the object does not intersect the near plane.
        // Calculate distance from center of the bounding sphere to the near plane in world space.
        if (fabsf(Dot(frustum_world.plane[0], so.sphere.center)) < so.sphere.radius)
            return true; // Intersect.
        return false;
    }
    // When the light position is in front of or behind the near plane, check the five planes of the near-clip
    // volume, six plane for point lights.
    return Intersects(so, near_clip_volume);
}

#if 0
bool sreFrustum::ShadowCasterVolumeAndCasterBoundsIntersect(const sreObject& so) const {
    // Check whether the object is inside the shadow caster volume.
    if (so.object->bounds_flags & SRE_BOUNDS_PREFER_SPHERE) {
        for (int i = 0; i < nu_shadow_caster_planes; i++) {
            if (Dot(shadow_caster_plane[i].K, so.sphere_center) <= - so.sphere_radius)
                return false;
        }

        // Inside the shadow caster volume.
        return true;
    }
    // Bounding box check.
    if (so.object->bounds_flags & SRE_BOUNDS_PREFER_BOX_LINE_SEGMENT) {
        // Line segment box bounds check.
        Point3D Q1 = so.box_center + so.box_R * 0.5;
        Point3D Q2 = so.box_center - so.box_R * 0.5;
        for (int i = 0; i < nu_shadow_caster_planes; i++) {
            float r_eff = (fabsf(Dot(so.box_S, shadow_caster_plane[i].K.GetVector3D())) +
                fabsf(Dot(so.box_T, shadow_caster_plane[i].K.GetVector3D()))) * 0.5;
            float dot1 = Dot(plane_world[i].K, Q1);
            float dot2 = Dot(plane_world[i].K, Q2);
            if (dot1 <= - r_eff && dot2 <= - r_eff)
                return false;
            if (i == nu_shadow_caster_planes - 1)
                return true;
            if (dot1 >= - r_eff && dot2 >= - r_eff)
                continue;
            // We now have that one of the dot products is less than - r_eff and the other is greater than
            // - r_eff.
            // Calculate the point Q3 such that Dot(K, Q3)  = - r_eff and replace the exterior endpoint
            // with it.
            float t = (r_eff + dot1) / Dot(shadow_caster_plane[i].K, Q1 - Q2);
            Point3D Q3 = Q1 + t * (Q2 - Q1);
            if (dot1 <= - r_eff)
                Q1 = Q3;
            else
                Q2 = Q3;
        }
        return true;
    }
    // Standard box check.
    for (int i = 0; i < nu_shadow_caster_planes; i++) {
       float r_eff = (fabsf(Dot(so.box_R, shadow_caster_plane[i].K.GetVector3D())) +
           fabsf(Dot(so.box_S, shadow_caster_plane[i].K.GetVector3D())) + 
           fabsf(Dot(so.box_T, shadow_caster_plane[i].K.GetVector3D()))) * 0.5;
       if (Dot(shadow_caster_plane[i].K, so.box_center) <= - r_eff)
           return false;
    }
    return true;
}
#endif


#if 0

bool sreFrustum::OctreeIsOutsideFrustum(const Octree& octree) const {
    // Standard box check.
    Vector3D R, S, T;
    R.Set(octree.dim_max.x - octree.dim_min.x, 0, 0);
    S.Set(0, octree.dim_max.y - octree.dim_min.y, 0);
    T.Set(0, 0, octree.dim_max.z - octree.dim_min.z);
    Point3D center;
    center.Set((octree.dim_min.x + octree.dim_max.x) * 0.5, (octree.dim_min.y + octree.dim_max.y) * 0.5,
        (octree.dim_min.z + octree.dim_max.z) * 0.5);
    for (int i = 0; i < 5; i++) {
       float r_eff = (fabsf(Dot(R, frustum_world.plane[i].GetVector3D())) +
           fabsf(Dot(S, frustum_world.plane[i].GetVector3D())) + 
           fabsf(Dot(T, frustum_world.plane[i].GetVector3D()))) * 0.5;
       if (Dot(frustum_world.plane[i], center) <= - r_eff)
           return true;
    }
    return false;
}

#endif

#if 0

bool sreFrustum::LineGoesOutsideFrustum(const Point3D& E1, const Vector3D& V) const {
    Point3D Q1 = E1;
    for (int j = 0; j < 5; j++) {
        // Calculate the distance between the endpoint and the plane.
        float dot1 = Dot(frustum_world.plane[j], Q1);
        // Take the dot product between the line direction and the plane normal.
        float dot2 = Dot(frustum_world.plane[j].GetVector3D(), V);
        if (dot1 <= 0 && dot2 <= 0)
            // The line's endpoint is outside the plane and extends away from the plane.
            return true;
        if (dot1 >= 0 && dot2 >= 0)
            // The line's endpoint is inside the plane and extends inside. No conclusions can be
            // drawn.
            continue;
        // The line overlaps the plane.
        if (dot2 < 0)
            // The line goes away from the plane. That means it goes out of the frustum.
            return true;
        // The line's finite end will be clipped.
        // Use the normalized axis direction as a hypothetical second endpoint to calculate the intersection
        // with the frustum plane.
        float t = dot1 / Dot(frustum_world.plane[j], V);
        Q1 = Q1 - t * V;
    }
    return false;
}

#endif

// Determine whether a geometrical shadow volume intersects the frustum. The shadow volume
// may be of the type SRE_BOUNDING_VOLUME_EMPTY, SRE_BOUNDING_VOLUME_EVERYWHERE,
// SRE_BOUNDING_VOLUME_HALF_CYLINDER (used for directional lights),
// or SRE_BOUNDING_VOLUME_PYRAMID (used for point and spot lights).
// Handling of SRE_BOUNDING_VOLUME_CYLINDER has been added for beam lights.

// #define SHADOW_VOLUME_INTERSECTION_LOG

bool sreFrustum::ShadowVolumeIsOutsideFrustum(sreShadowVolume& sv) const {
    if (sv.type == SRE_BOUNDING_VOLUME_EMPTY)
        return true;
    if (sv.type == SRE_BOUNDING_VOLUME_EVERYWHERE)
        return false;
    if (sv.type == SRE_BOUNDING_VOLUME_HALF_CYLINDER) {
        bool r = !Intersects(*sv.half_cylinder, frustum_world);
#ifdef SHADOW_VOLUME_INTERSECTION_LOG
        if (r) {
            sreMessage(SRE_MESSAGE_LOG, "Half-cylinder shadow volume is outside frustum: "
                "endpoint = (%f, %f, %f), radius = %f, axis = (%f, %f, %f)\n",
                sv.half_cylinder->endpoint.x,  sv.half_cylinder->endpoint.y, sv.half_cylinder->endpoint.z,
                sv.half_cylinder->radius,  sv.half_cylinder->axis.x,  sv.half_cylinder->axis.y,
                sv.half_cylinder->axis.z);
        }
#endif
        return r;
    }
    else if (sv.type == SRE_BOUNDING_VOLUME_PYRAMID) {
        // Pyramid.
        // First check whether the apex is inside the frustum for performance reasons.
        if (Intersects(sv.pyramid->vertex[0], frustum_world))
            return false;
#if 0
        // Calculate pyramid planes when not yet calculated.
        // This is no longer necessary; a pyramid is defined only by its vertices (hull).
        if (!sv.is_complete)
            sv.CompleteParameters();
#endif
        bool r = !Intersects(*sv.pyramid, frustum_world);
#ifdef SHADOW_VOLUME_INTERSECTION_LOG
        if (r)
            sreMessage(SRE_MESSAGE_LOG, "Pyramid shadow volume is outside frustum.\n");
#endif
        return r;
    }
    else if (sv.type == SRE_BOUNDING_VOLUME_PYRAMID) {
        bool r = !Intersects(*sv.spherical_sector, frustum_world);
        return r;
    }
    else if (sv.type == SRE_BOUNDING_VOLUME_PYRAMID_CONE) {
        // Pyramid cone.
        // First check whether the apex is inside the frustum for performance reasons.
        if (Intersects(sv.pyramid_cone->vertex[0], frustum_world))
            return false;
        bool r = !Intersects(*sv.pyramid_cone, frustum_world);
#ifdef SHADOW_VOLUME_INTERSECTION_LOG
        if (r)
            sreMessage(SRE_MESSAGE_LOG, "Pyramid cone shadow volume is outside frustum.\n");
#endif
        return r;
    }
    else if (sv.type == SRE_BOUNDING_VOLUME_CYLINDER) {
        // Cylinder (beam light).
        bool r = !Intersects(*sv.cylinder, frustum_world);
#ifdef SHADOW_VOLUME_INTERSECTION_LOG
        if (r)
            sreMessage(SRE_MESSAGE_LOG, "Cylinder shadow volume is outside frustum.\n");
#endif
        return r;
    }
    else if (sv.type == SRE_BOUNDING_VOLUME_SPHERICAL_SECTOR) {
        bool r = !Intersects(*sv.spherical_sector, frustum_world);
        return r;
    }
    // Unknown bounding volume type.
    sreMessage(SRE_MESSAGE_WARNING, "ShadowVolumeIsOutsideFrustum: Unknown bounding volume type %d",
        sv.type);
    return false;
}

// Rather the geometrical shadow volume, this test refers to the dark cap extruded to infinity
// as used on the GPU. Since a dark cap is not defined for directional or beam light shadow
// volumes, only the infinite pyramid base of point and spot light shadow volumes needs to
// be handled.

bool sreFrustum::DarkCapIsOutsideFrustum(sreShadowVolume& sv) const {
    if (sv.type == SRE_BOUNDING_VOLUME_EMPTY)
        return true;
    if (sv.type == SRE_BOUNDING_VOLUME_EVERYWHERE)
        return false;
    if (sv.type == SRE_BOUNDING_VOLUME_PYRAMID_CONE) {
        bool r = !Intersects(*(sreBoundingVolumeInfinitePyramidBase *)sv.pyramid_cone, frustum_world,
            cos_max_half_angular_size, sin_max_half_angular_size);
        return r;
    }
    else if (sv.type == SRE_BOUNDING_VOLUME_SPHERICAL_SECTOR) {
        bool r = !Intersects(*(sreBoundingVolumeInfiniteSphericalSector *)sv.spherical_sector, frustum_world,
            cos_max_half_angular_size, sin_max_half_angular_size);
        return r;
    }
    else
        return false;
}

