// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// Bounding spheres, and the two tests a flight game actually needs.
//
// Why spheres when the baked mesh carries an AABB: an AABB stops being a
// bound the moment the object rotates, and everything in a flight game
// rotates constantly. Re-fitting a box every frame costs more than it saves
// at these object counts. So the box the baker computed becomes a radius,
// once, and the radius is right at every attitude.

#pragma once
#include <stdbool.h>

#include "crema_matrix.h"

#ifdef __cplusplus
extern "C" {
#endif

// Radius of the sphere enclosing a model-space AABB, measured from the model
// origin — not from the box centre, because that is where the object's
// transform is applied.
float CremaBoundsRadius(const float aabbMin[3], const float aabbMax[3]);

bool CremaSphereHitsSphere(Vec3 a, float radiusA, Vec3 b, float radiusB);

// Segment from `origin` along `dir` (unit length) for `maxDist`. On a hit,
// `outDist` receives the distance to the first intersection.
bool CremaRayHitsSphere(Vec3 origin, Vec3 dir, float maxDist,
                        Vec3 centre, float radius, float *outDist);

#ifdef __cplusplus
}
#endif
