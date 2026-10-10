#pragma once

#include <cstddef>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace n02::encoding {

#ifdef _WIN32
inline bool multiByteToWide(UINT codePage, DWORD flags, const char* input, std::wstring& output)
{
    output.clear();
    if (input == nullptr || input[0] == '\0')
    {
        return true;
    }

    const int inputLength = static_cast<int>(std::strlen(input));
    const int wideLength = MultiByteToWideChar(
        codePage,
        flags,
        input,
        inputLength,
        nullptr,
        0);
    if (wideLength <= 0)
    {
        return false;
    }

    output.resize(static_cast<std::size_t>(wideLength));
    return MultiByteToWideChar(
               codePage,
               flags,
               input,
               inputLength,
               output.data(),
               wideLength) == wideLength;
}

inline bool wideToMultiByte(
    UINT codePage,
    DWORD flags,
    const std::wstring& input,
    std::string& output)
{
    output.clear();
    if (input.empty())
    {
        return true;
    }

    const char replacement = '?';
    BOOL usedDefaultCharacter = FALSE;
    const char* replacementPtr = (codePage == CP_UTF8) ? nullptr : &replacement;
    BOOL* usedDefaultPtr = (codePage == CP_UTF8) ? nullptr : &usedDefaultCharacter;

    const int outputLength = WideCharToMultiByte(
        codePage,
        flags,
        input.data(),
        static_cast<int>(input.size()),
        nullptr,
        0,
        replacementPtr,
        usedDefaultPtr);
    if (outputLength <= 0)
    {
        return false;
    }

    output.resize(static_cast<std::size_t>(outputLength));
    return WideCharToMultiByte(
               codePage,
               flags,
               input.data(),
               static_cast<int>(input.size()),
               output.data(),
               outputLength,
               replacementPtr,
               usedDefaultPtr) == outputLength;
}
#endif

// Convert text received from a traditional Kaillera server to UTF-8.
// Valid UTF-8 is preserved so mixed environments and newer clients still work.
inline std::string decodeKailleraText(const char* text)
{
    if (text == nullptr || text[0] == '\0')
    {
        return {};
    }

#ifdef _WIN32
    std::wstring wide;
    if (multiByteToWide(CP_UTF8, MB_ERR_INVALID_CHARS, text, wide))
    {
        return std::string(text);
    }

    if (!multiByteToWide(932, 0, text, wide))
    {
        return std::string(text);
    }

    std::string utf8;
    if (!wideToMultiByte(CP_UTF8, 0, wide, utf8))
    {
        return std::string(text);
    }
    return utf8;
#else
    return std::string(text);
#endif
}

// Convert UTF-8 text to Windows-31J/CP932 for traditional Kaillera servers.
// Invalid UTF-8 is treated as already-encoded legacy text to avoid double conversion.
inline std::string encodeKailleraText(const char* text)
{
    if (text == nullptr || text[0] == '\0')
    {
        return {};
    }

#ifdef _WIN32
    std::wstring wide;
    if (!multiByteToWide(CP_UTF8, MB_ERR_INVALID_CHARS, text, wide))
    {
        // If the input already looks legacy-encoded, normalize it through CP932.
        if (multiByteToWide(932, 0, text, wide))
        {
            std::string cp932;
            if (wideToMultiByte(932, WC_NO_BEST_FIT_CHARS, wide, cp932))
            {
                return cp932;
            }
        }
        return "?";
    }

    std::string cp932;
    if (!wideToMultiByte(932, WC_NO_BEST_FIT_CHARS, wide, cp932))
    {
        return "?";
    }
    return cp932;
#else
    return std::string(text);
#endif
}

inline bool isCp932LeadByte(unsigned char byte)
{
    return (byte >= 0x81 && byte <= 0x9F) ||
           (byte >= 0xE0 && byte <= 0xFC);
}

// Return the largest prefix that fits in maxBytes without cutting a CP932 character.
inline std::size_t safeCp932PrefixLength(const std::string& text, std::size_t maxBytes)
{
    std::size_t position = 0;
    while (position < text.size())
    {
        const unsigned char current = static_cast<unsigned char>(text[position]);
        const std::size_t width = isCp932LeadByte(current) ? 2U : 1U;
        if (position + width > text.size() || position + width > maxBytes)
        {
            break;
        }
        position += width;
    }
    return position;
}

} // namespace n02::encoding
