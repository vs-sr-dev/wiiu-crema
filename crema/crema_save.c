// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan

#include "crema_save.h"

#include <coreinit/filesystem.h>
#include <whb/log.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SAVE_MAGIC   "CSAV"
#define SAVE_SD_ROOT "/vol/external01"

// A header for four bytes of high score is not ceremony. This is the only file
// in the project written by the console and read by a later run of a different
// build of the same program, so it is the only one where "what if the bytes are
// not what I think they are" is a real question rather than a build error.
typedef struct {
    char     magic[4];
    uint32_t version;    // the caller's, not this module's
    uint32_t size;       // payload bytes that follow
    uint32_t hash;       // FNV-1a over the payload
} SaveHeader;

static char sDir[192];
static bool sReady;

// Only true when *we* did the mounting, which is the only case where undoing it
// is ours to do.
static bool sWeMounted;
static FSClient   sClient __attribute__((aligned(64)));
static FSCmdBlock sCmd    __attribute__((aligned(64)));

static uint32_t fnv1a(const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool ensureDir(const char *path)
{
    if (exists(path))
        return true;
    if (mkdir(path, 0777) == 0)
        return true;
    // Two processes racing, or a filesystem that reports EEXIST as something
    // else: the question is whether the directory is there now, not whether we
    // were the one who made it.
    return exists(path);
}

// A fallback that has never been needed, and is kept because it was *tried*
// rather than assumed. Cemu's own source says the card is mounted only from
// FSMount and FSBindMount, which reads like "the emulator will not have it until
// we ask" — and then the probe above found /vol/external01 already there on both
// targets, so this never runs. Forcing it once was worth the build: FSGetMountSource
// and FSMount both return OK against an already-mounted card and change nothing,
// so the branch works if a launch path ever turns up without the card ready.
//
// The probe stays in front of it anyway. Under Aroma another module owns that
// mount and refcounts it, and mounting a volume somebody else is holding is not
// a thing to do speculatively.
static bool mountSdCard(void)
{
    FSMountSource source;
    char mountPath[128];

    FSInit();
    if (FSAddClient(&sClient, FS_ERROR_FLAG_NONE) != FS_STATUS_OK) {
        WHBLogPrintf("[save] FSAddClient failed");
        return false;
    }
    FSInitCmdBlock(&sCmd);

    memset(&source, 0, sizeof(source));
    // Zeroed and passed by pointer, because the two implementations read this
    // argument in opposite directions: the console fills in where it mounted,
    // Cemu writes the path it chose. An empty buffer is what both of them
    // expect to be handed.
    memset(mountPath, 0, sizeof(mountPath));

    FSStatus s = FSGetMountSource(&sClient, &sCmd, FS_MOUNT_SOURCE_SD, &source,
                                  FS_ERROR_FLAG_ALL);
    if (s != FS_STATUS_OK) {
        WHBLogPrintf("[save] no SD mount source (%d)", (int)s);
        FSDelClient(&sClient, FS_ERROR_FLAG_NONE);
        return false;
    }
    s = FSMount(&sClient, &sCmd, &source, mountPath, sizeof(mountPath),
                FS_ERROR_FLAG_ALL);
    if (s != FS_STATUS_OK) {
        WHBLogPrintf("[save] FSMount failed (%d)", (int)s);
        FSDelClient(&sClient, FS_ERROR_FLAG_NONE);
        return false;
    }
    // The client owns the mount: deleting it takes the card away with it, so it
    // lives as long as the module does.
    sWeMounted = true;
    WHBLogPrintf("[save] mounted the SD card at %s",
                 mountPath[0] ? mountPath : SAVE_SD_ROOT);
    return true;
}

bool CremaSaveInit(const char *appName)
{
    if (!appName || !*appName)
        return false;

    sReady = false;
    if (!exists(SAVE_SD_ROOT) && !mountSdCard())
        return false;

    if (!ensureDir(SAVE_SD_ROOT "/wiiu") ||
        !ensureDir(SAVE_SD_ROOT "/wiiu/apps")) {
        WHBLogPrintf("[save] cannot create " SAVE_SD_ROOT "/wiiu/apps");
        return false;
    }
    snprintf(sDir, sizeof(sDir), SAVE_SD_ROOT "/wiiu/apps/%s", appName);
    if (!ensureDir(sDir)) {
        WHBLogPrintf("[save] cannot create %s", sDir);
        return false;
    }

    sReady = true;
    WHBLogPrintf("[save] ready at %s", sDir);
    return true;
}

void CremaSaveShutdown(void)
{
    if (sWeMounted) {
        FSUnmount(&sClient, &sCmd, SAVE_SD_ROOT, FS_ERROR_FLAG_ALL);
        FSDelClient(&sClient, FS_ERROR_FLAG_NONE);
        sWeMounted = false;
    }
    sReady = false;
    sDir[0] = '\0';
}

const char *CremaSaveDir(void)
{
    return sDir[0] ? sDir : "(no save directory)";
}

static bool buildPath(char *out, size_t outSize, const char *name,
                      const char *suffix)
{
    if (!sReady || !name || strchr(name, '/'))
        return false;
    int n = snprintf(out, outSize, "%s/%s%s", sDir, name, suffix);
    return n > 0 && (size_t)n < outSize;
}

bool CremaSaveWrite(const char *name, uint32_t version,
                    const void *data, size_t size)
{
    char path[256], tmp[256];
    if (!data || size == 0 || size > 0x100000)
        return false;
    if (!buildPath(path, sizeof(path), name, "") ||
        !buildPath(tmp, sizeof(tmp), name, ".new"))
        return false;

    SaveHeader h;
    memcpy(h.magic, SAVE_MAGIC, 4);
    h.version = version;
    h.size    = (uint32_t)size;
    h.hash    = fnv1a(data, size);

    FILE *fh = fopen(tmp, "wb");
    if (!fh) {
        WHBLogPrintf("[save] cannot open %s for writing (%s)", tmp,
                     strerror(errno));
        return false;
    }
    bool ok = fwrite(&h, sizeof(h), 1, fh) == 1 &&
              fwrite(data, 1, size, fh) == size;
    // fclose is where a buffered write actually reaches the card, so its result
    // is part of "did this work" and not an afterthought.
    if (fclose(fh) != 0)
        ok = false;
    if (!ok) {
        WHBLogPrintf("[save] write to %s failed", tmp);
        remove(tmp);
        return false;
    }

    // The rename is the whole point of the temporary: for the whole of the write
    // above, the file under `path` is still the previous save, entire, so losing
    // power there costs the new score and nothing else.
    //
    // The remove is the one honest gap. FSA's rename onto an existing name is not
    // documented to replace it, so the old file goes first and there are a few
    // microseconds with neither — against the milliseconds of writing, which is
    // the window that actually matters. Truly atomic replacement would need a
    // rename that overwrites, and finding out whether this filesystem has one is
    // a job for the day something bigger than a high score is at stake.
    remove(path);
    if (rename(tmp, path) != 0) {
        WHBLogPrintf("[save] rename %s -> %s failed (%s)", tmp, path,
                     strerror(errno));
        remove(tmp);
        return false;
    }
    return true;
}

size_t CremaSaveRead(const char *name, uint32_t version,
                     void *data, size_t size)
{
    char path[256];
    if (!data || size == 0 || !buildPath(path, sizeof(path), name, ""))
        return 0;

    FILE *fh = fopen(path, "rb");
    if (!fh)
        return 0;                       // no save yet: the normal first run

    SaveHeader h;
    size_t got = 0;
    if (fread(&h, sizeof(h), 1, fh) == 1 &&
        memcmp(h.magic, SAVE_MAGIC, 4) == 0 && h.version == version &&
        h.size > 0 && h.size <= size) {
        got = fread(data, 1, h.size, fh);
        if (got != h.size) {
            WHBLogPrintf("[save] %s is short: %u of %u bytes",
                         name, (unsigned)got, h.size);
            got = 0;
        } else if (fnv1a(data, got) != h.hash) {
            // Not paranoia about bit rot: this is what a half-written file from
            // an earlier crash looks like, and reading it would put a plausible
            // wrong number on screen instead of no number at all.
            WHBLogPrintf("[save] %s fails its checksum - ignoring it", name);
            got = 0;
        }
    } else {
        WHBLogPrintf("[save] %s is not a v%u CSAV blob of <= %u bytes",
                     name, (unsigned)version, (unsigned)size);
    }
    fclose(fh);
    if (got)
        memset((uint8_t *)data + got, 0, size - got);
    return got;
}

bool CremaSaveErase(const char *name)
{
    char path[256];
    if (!buildPath(path, sizeof(path), name, ""))
        return false;
    remove(path);
    return !exists(path);
}
