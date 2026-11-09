#define CORE_INTERNAL
#include "Compression.hpp"
#include "Library.hpp"

#include <limits>

#include <zlib.h>

//
// Exported Functions
//

CORE_EXPORT bool CoreZlibInflateExact(const void* src, size_t srcSize, void* dst, size_t dstSize)
{
    if (src == nullptr || dst == nullptr || srcSize == 0 || dstSize == 0 ||
        srcSize > std::numeric_limits<uInt>::max() ||
        dstSize > std::numeric_limits<uInt>::max())
    {
        return false;
    }

    z_stream stream = {};
    if (inflateInit(&stream) != Z_OK)
    {
        return false;
    }

    stream.next_in   = static_cast<Bytef*>(const_cast<void*>(src));
    stream.avail_in  = static_cast<uInt>(srcSize);
    stream.next_out  = static_cast<Bytef*>(dst);
    stream.avail_out = static_cast<uInt>(dstSize);

    // One call with the whole input: Z_STREAM_END only if the stream finished
    // inside the buffer. A stream that wants more room stops with Z_BUF_ERROR
    // once the buffer is full, so it can't write past it.
    const int ret = inflate(&stream, Z_FINISH);
    const bool ok = ret == Z_STREAM_END && stream.avail_out == 0 && stream.avail_in == 0;
    inflateEnd(&stream);
    return ok;
}
