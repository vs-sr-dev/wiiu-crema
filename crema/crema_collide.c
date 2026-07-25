// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_collide.h"

#include <math.h>

float CremaBoundsRadius(const float aabbMin[3], const float aabbMax[3])
{
    float r = 0.0f;
    for (int i = 0; i < 3; i++) {
        float lo = fabsf(aabbMin[i]);
        float hi = fabsf(aabbMax[i]);
        float far = lo > hi ? lo : hi;
        r += far * far;
    }
    return sqrtf(r);
}

bool CremaSphereHitsSphere(Vec3 a, float radiusA, Vec3 b, float radiusB)
{
    Vec3 d = vec3_sub(b, a);
    float reach = radiusA + radiusB;
    return vec3_dot(d, d) <= reach * reach;
}

bool CremaRayHitsSphere(Vec3 origin, Vec3 dir, float maxDist,
                        Vec3 centre, float radius, float *outDist)
{
    // Project the sphere centre onto the ray, then compare the perpendicular
    // distance against the radius. No square roots until we know it hits.
    Vec3 toCentre = vec3_sub(centre, origin);
    float along = vec3_dot(toCentre, dir);
    if (along < -radius || along > maxDist + radius)
        return false;

    float distSq = vec3_dot(toCentre, toCentre) - along * along;
    float radiusSq = radius * radius;
    if (distSq > radiusSq)
        return false;

    float half = sqrtf(radiusSq - distSq);
    float hit = along - half;
    if (hit < 0.0f)
        hit = along + half;      // origin inside the sphere: take the far side
    if (hit < 0.0f || hit > maxDist)
        return false;

    if (outDist)
        *outDist = hit;
    return true;
}
