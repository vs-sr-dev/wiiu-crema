// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_scene.h"

#include <coreinit/time.h>
#include <string.h>
#include <whb/log.h>

void CremaSceneStackInit(CremaSceneStack *stack, CremaScene **storage,
                         uint32_t capacity)
{
    memset(stack, 0, sizeof(*stack));
    stack->items    = storage;
    stack->capacity = capacity;
}

void CremaSceneRequest(CremaSceneStack *stack, CremaSceneOp op,
                       CremaScene *target)
{
    if (stack->op != CREMA_SCENE_NONE)
        return;                     // the first one of the frame wins
    stack->op     = op;
    stack->target = target;
}

bool CremaScenePending(const CremaSceneStack *stack)
{
    return stack->op != CREMA_SCENE_NONE;
}

CremaScene *CremaSceneTop(const CremaSceneStack *stack)
{
    return stack->depth > 0 ? stack->items[stack->depth - 1] : NULL;
}

// The deepest scene that has to be drawn: walk down until something opaque
// covers what is behind it.
static uint32_t visibleFloor(const CremaSceneStack *stack)
{
    if (stack->depth == 0)
        return 0;
    uint32_t i = stack->depth - 1;
    while (i > 0 && !stack->items[i]->opaque)
        i--;
    return i;
}

void CremaSceneUpdate(CremaSceneStack *stack, const CremaInput *in, float dt)
{
    if (CremaScenePending(stack))
        return;
    CremaScene *top = CremaSceneTop(stack);
    if (top && top->update)
        top->update(top, in, dt);
}

void CremaSceneBuild(CremaSceneStack *stack, uint32_t slot)
{
    for (uint32_t i = visibleFloor(stack); i < stack->depth; i++)
        if (stack->items[i]->build)
            stack->items[i]->build(stack->items[i], slot);
}

void CremaSceneDraw(void *user)
{
    CremaSceneStack *stack = (CremaSceneStack *)user;
    for (uint32_t i = visibleFloor(stack); i < stack->depth; i++)
        if (stack->items[i]->draw)
            stack->items[i]->draw(stack->items[i]);
}

const float *CremaSceneClearColor(const CremaSceneStack *stack)
{
    static const float BLACK[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (stack->depth == 0)
        return BLACK;
    return stack->items[visibleFloor(stack)]->clear;
}

void CremaSceneApply(CremaSceneStack *stack, CremaFrame *frame)
{
    if (stack->op == CREMA_SCENE_NONE)
        return;

    // Only the operations that hand memory back need the GPU drained first.
    bool frees = (stack->op == CREMA_SCENE_GOTO || stack->op == CREMA_SCENE_POP);
    stack->settleUs = 0;
    if (frees && frame) {
        uint64_t t = OSGetSystemTime();
        CremaFrameSettle(frame);
        stack->settleUs = (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - t);
    }

    uint64_t t0 = OSGetSystemTime();
    if (frees) {
        uint32_t keep = (stack->op == CREMA_SCENE_GOTO) ? 0 : stack->depth - 1;
        while (stack->depth > keep) {
            CremaScene *s = stack->items[--stack->depth];
            if (s->leave)
                s->leave(s);
        }
    }
    stack->leaveUs = (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - t0);

    uint64_t t1 = OSGetSystemTime();
    if (stack->target && stack->depth < stack->capacity) {
        CremaScene *s = stack->target;
        stack->items[stack->depth++] = s;
        if (s->enter)
            s->enter(s);
    }
    stack->enterUs = (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - t1);

    CremaScene *top = CremaSceneTop(stack);
    WHBLogPrintf("[scene] -> %s (depth %u) | settle %u us | leave %u us | "
                 "enter %u us", top ? top->name : "(empty)", stack->depth,
                 stack->settleUs, stack->leaveUs, stack->enterUs);
    stack->switches++;
    stack->op     = CREMA_SCENE_NONE;
    stack->target = NULL;
}
