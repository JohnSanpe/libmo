/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright(c) 2022 Sanpe <sanpeqf@gmail.com>
 */

#include <libmo.h>
#include <stdio.h>
#include <string.h>

#include "bitmap.c"
#include "pjwhash.c"

#define swab32(x) ((uint32_t)( \
    (((uint32_t)(x) & (uint32_t)0x000000ffUL) << 24) | \
    (((uint32_t)(x) & (uint32_t)0x0000ff00UL) <<  8) | \
    (((uint32_t)(x) & (uint32_t)0x00ff0000UL) >>  8) | \
    (((uint32_t)(x) & (uint32_t)0xff000000UL) >> 24)))

#define LIBMO_SWAB(ctx, value) ( \
    (ctx)->swab ? swab32(value) : (value) \
)

static inline int load_swab(struct libmo_context *ctx)
{
    const struct libmo_header *head = ctx->data;

    switch (head->magic) {
        case LIBMO_MAGIC:
            ctx->swab = false;
            break;

        case LIBMO_MAGIC_SWAB:
            ctx->swab = true;
            break;

        default:
            return -EINVAL;
    }

    return 0;
}

static int load_context(struct libmo_context *ctx)
{
    const struct libmo_header *head = ctx->data;
    int retval;

    if (ctx->size < sizeof(*head))
        return -EFBIG;

    if ((retval = load_swab(ctx)))
        return retval;

    ctx->major = LIBMO_SWAB(ctx, head->revision) >> 16;
    ctx->minor = LIBMO_SWAB(ctx, head->revision) & 0xffff;

    if (ctx->major != 0 && ctx->major != 1)
        return -EPROTO;

    ctx->index_num = LIBMO_SWAB(ctx, head->index_num);
    ctx->hash_size = LIBMO_SWAB(ctx, head->hash_size);

    ctx->orig_offset = LIBMO_SWAB(ctx, head->orig_offset);
    ctx->tran_offset = LIBMO_SWAB(ctx, head->tran_offset);
    ctx->hash_offset = LIBMO_SWAB(ctx, head->hash_offset);

    if ((ctx->orig_offset | ctx->tran_offset) % sizeof(uint32_t))
        return -EADDRNOTAVAIL;

    if (ctx->orig_offset + ctx->index_num * sizeof(struct libmo_sdesc) >= ctx->size ||
        ctx->tran_offset + ctx->index_num * sizeof(struct libmo_sdesc) >= ctx->size ||
        ctx->hash_offset + ctx->index_num * sizeof(uint32_t) >= ctx->size)
        return -EFBIG;

    return 0;
}

static uint32_t get_uint32(const struct libmo_context *ctx, uintptr_t offset, uint32_t *valp)
{
    const uint32_t *desc = ctx->data + offset;

    if (offset + sizeof(uint32_t) > ctx->size)
        return -EOVERFLOW;

    *valp = LIBMO_SWAB(ctx, *desc);
    return 0;
}

static const char *get_string(const struct libmo_context *ctx, uintptr_t offset, size_t *lenp)
{
    uint32_t off, len;
    int retval;

    if ((retval = get_uint32(ctx, offset + offsetof(struct libmo_sdesc, offset), &off)) ||
        (retval = get_uint32(ctx, offset + offsetof(struct libmo_sdesc, length), &len)))
        return NULL;

    if (off + len > ctx->size)
        return NULL;

    if (((char *)ctx->data)[off + len])
        return NULL;

    if (lenp)
        *lenp = len;

    return ctx->data + off;
}

int libmo_load(struct libmo_context *ctx, const void *data, size_t size)
{
    *ctx = (struct libmo_context){data, size};
    return load_context(ctx);
}

int libmo_verify(const struct libmo_context *ctx)
{
    const char *msgid, *prev;
    unsigned int index;
    int retval;

    /* Verify that the array of messages is sorted. */
    for (index = 0; index < ctx->index_num; ++index) {
        msgid = get_string(ctx, LIBMO_ORIG_OFFSET(ctx, index), NULL);
        if (!msgid)
            return -EINVAL;

        if (index && strcmp(prev, msgid) > 0) {
            fprintf(stderr, "%p: messages array is not sorted\n", ctx);
            return -EINVAL;
        }

        prev = msgid;
    }

    /* Verify the hash table. */
    if (ctx->hash_size) {
        unsigned long *bitmap;

        /* Verify the hash table size. */
        if (ctx->hash_size < 2) {
            fprintf(stderr, "%p: hash table size is invalid\n", ctx);
            return -EINVAL;
        }

        /* Verify that the non-empty hash table entries contain the values. */
        bitmap = bitmap_zalloc(ctx->index_num);
        if (!bitmap)
            return -ENOMEM;

        for (index = 0; index < ctx->hash_size; ++index) {
            uint32_t entry;

            retval = get_uint32(ctx, LIBMO_HASH_OFFSET(ctx, index), &entry);
            if (retval)
                return retval;

            if (entry--) {
                if (entry < ctx->index_num && !bitmap_test_set(bitmap, entry))
                    continue;
                fprintf(stderr, "%p: hash table contains invalid entries\n", ctx);
                return -EINVAL;
            }
        }

        if (!bitmap_full(bitmap, ctx->index_num)) {
            fprintf(stderr, "%p: some messages are not present in hash table\n", ctx);
            return -EINVAL;
        }

        bitmap_free(bitmap);

        /* Verify that the hash table lookup algorithm finds the entry for each message. */
        for (index = 0; index < ctx->index_num; ++index) {
            uint32_t hashval, entry, incr;

            msgid = get_string(ctx, LIBMO_ORIG_OFFSET(ctx, index), NULL);
            hashval = pjwhash(msgid);
            entry = hashval % ctx->hash_size;
            incr = (hashval % (ctx->hash_size - 2)) + 1;

            for (;;) {
                uint32_t value;

                retval = get_uint32(ctx, LIBMO_HASH_OFFSET(ctx, entry), &value);
                if (retval)
                    return retval;

                if (!value) {
                    fprintf(stderr, "%p: some messages are at a wrong index in the hash table\n", ctx);
                    return -EINVAL;
                }

                if (value - 1 == index)
                    break;

                if (entry >= ctx->hash_size - incr)
                    entry -= ctx->hash_size - incr;
                else
                    entry += incr;
            }
        }
    }

    return 0;
}

const char *libmo_lookup(const struct libmo_context *ctx, const char *orig, size_t origlen, size_t *tranlenp)
{
    const char *msgid, *msgstr;
    uint32_t hashval, entry, incr, value;
    size_t msglen, tranlen;
    int retval;

    hashval = pjwhash(orig);
    entry = hashval % ctx->hash_size;
    incr = (hashval % (ctx->hash_size - 2)) + 1;

    for (;;) {
        retval = get_uint32(ctx, LIBMO_HASH_OFFSET(ctx, entry), &value);
        if (retval || !value)
            return NULL;

        msgid = get_string(ctx, LIBMO_ORIG_OFFSET(ctx, value - 1), &msglen);
        if (!msgid)
            return NULL;

        if ((!origlen || (origlen == msglen)) && !strcmp(orig, msgid)) {
            msgstr = get_string(ctx, LIBMO_TRAN_OFFSET(ctx, value - 1), &tranlen);
            if (!msgstr)
                return NULL;

            if (tranlenp)
                *tranlenp = tranlen;

            return msgstr;
        }

        if (entry >= ctx->hash_size - incr)
            entry -= ctx->hash_size - incr;
        else
            entry += incr;
    }
}
