#ifndef CORE_COMPRESSION_HPP
#define CORE_COMPRESSION_HPP

#include <cstddef>

// Inflates the zlib stream in src into exactly dstSize bytes at dst.
// Fails unless the stream ends at exactly dstSize bytes with no input left
// over, so the caller's buffer bounds what a stream can produce: use it for
// compressed data from another machine, where a small stream could otherwise
// inflate to gigabytes.
bool CoreZlibInflateExact(const void* src, size_t srcSize, void* dst, size_t dstSize);

#endif // CORE_COMPRESSION_HPP
