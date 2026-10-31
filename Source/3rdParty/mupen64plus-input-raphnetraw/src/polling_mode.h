#ifndef RAPHNET_POLLING_MODE_H
#define RAPHNET_POLLING_MODE_H

typedef enum {
    RAPHNET_POLLING_AUTOMATIC = 0,
    RAPHNET_POLLING_CACHED = 1,
    RAPHNET_POLLING_DIRECT = 2
} raphnet_polling_mode;

static inline raphnet_polling_mode raphnet_polling_mode_from_int(int mode)
{
    return mode == RAPHNET_POLLING_CACHED || mode == RAPHNET_POLLING_DIRECT
        ? (raphnet_polling_mode)mode : RAPHNET_POLLING_AUTOMATIC;
}

static inline int raphnet_polling_uses_cache(raphnet_polling_mode mode, int automatic_cached)
{
    return mode == RAPHNET_POLLING_CACHED ||
        (mode == RAPHNET_POLLING_AUTOMATIC && automatic_cached);
}

#endif
