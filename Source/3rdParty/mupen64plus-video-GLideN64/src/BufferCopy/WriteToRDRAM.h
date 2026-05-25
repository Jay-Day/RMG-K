#ifndef WriteToRDRAM_H
#define WriteToRDRAM_H


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "../Types.h"

static inline bool rollbackWatchRdramAddress(u32 _address, u32 _size)
{
	return _address + _size > 0x00394300u && _address < 0x00394380u;
}

static inline bool rollbackVerboseGlideInputLoggingEnabled()
{
	const char* value = getenv("RMGK_VERBOSE_GLIDE_INPUT_LOGGING");
	return value != nullptr && value[0] == '1';
}

static inline const char* rollbackLogPathSeparator(const char* _directory)
{
	if (_directory == nullptr)
		return "";

	const size_t length = strlen(_directory);
	if (length == 0)
		return "";

	const char last = _directory[length - 1];
	return last == '/' || last == '\\' ? "" : "/";
}

static inline void rollbackCreateLogDirectory(const char* _directory)
{
#ifdef _WIN32
	_mkdir(_directory);
#else
	mkdir(_directory, 0700);
#endif
}

static inline FILE* rollbackOpenLogFile(const char* _suffix)
{
	const char* directory = getenv("RMGK_ROLLBACK_LOG_DIR");
	const char* prefix = getenv("RMGK_ROLLBACK_LOG_PREFIX");
	char path[4096];

	if (directory != nullptr && directory[0] != '\0' && prefix != nullptr && prefix[0] != '\0') {
		rollbackCreateLogDirectory(directory);
		const int written = snprintf(path, sizeof(path), "%s%s%s_%s.log",
			directory,
			rollbackLogPathSeparator(directory),
			prefix,
			_suffix);
		if (written > 0 && static_cast<size_t>(written) < sizeof(path)) {
			FILE* file = fopen(path, "a");
			if (file != nullptr)
				return file;
		}
	}

	rollbackCreateLogDirectory("Logs");
	int written = snprintf(path, sizeof(path), "Logs%srollback_%s.log", rollbackLogPathSeparator("Logs"), _suffix);
	if (written > 0 && static_cast<size_t>(written) < sizeof(path)) {
		FILE* file = fopen(path, "a");
		if (file != nullptr)
			return file;
	}

	rollbackCreateLogDirectory("Bin/Release/Logs");
	written = snprintf(path, sizeof(path), "Bin/Release/Logs%srollback_%s.log",
		rollbackLogPathSeparator("Bin/Release/Logs"),
		_suffix);
	if (written > 0 && static_cast<size_t>(written) < sizeof(path))
		return fopen(path, "a");

	return nullptr;
}

template <typename TDst>
static inline void rollbackLogWriteToRdram(const char* _source, u32 _address, TDst _oldValue, TDst _newValue)
{
	FILE* file;

	if (!rollbackWatchRdramAddress(_address, sizeof(TDst)))
		return;

	file = rollbackOpenLogFile("rdram_watch");
	if (file == nullptr)
		return;

	fprintf(file, "source=%s address=0x%08x size=%u old=0x%08x new=0x%08x\n",
		_source,
		_address,
		static_cast<unsigned>(sizeof(TDst)),
		static_cast<unsigned>(_oldValue),
		static_cast<unsigned>(_newValue));
	fclose(file);
}

template <typename T, T testValue>
bool valueTester(T _c)
{
	return _c != testValue;
}

template <typename T>
bool dummyTester(T _c)
{
	return true;
}

template <typename TSrc, typename TDst>
void writeToRdram(TSrc* _src, TDst* _dst,
	TDst(*converter)(TSrc _c, u32 x, u32 y),
	bool(*tester)(TSrc _c),
	u32 _xor,
	u32 _width,
	u32 _height,
	u32 _numPixels,
	u32 _startAddress,
	u32 _bufferAddress,
	u32 _bufferSize,
	const char* _logSource = "glide_writeToRdram")
{
	u32 chunkStart = ((_startAddress - _bufferAddress) >> (_bufferSize - 1)) % _width;
	if (chunkStart % 2 != 0) {
		--chunkStart;
		--_dst;
		++_numPixels;
	}

	u32 numStored = 0;
	u32 y = 0;
	TSrc c;
	const bool logEnabled = rollbackVerboseGlideInputLoggingEnabled();
	if (chunkStart > 0) {
		for (u32 x = chunkStart; x < _width; ++x) {
			c = _src[x];
			if (tester(c)) {
				const u32 dstIndex = numStored ^ _xor;
				const TDst newValue = converter(c, x, y);
				if (logEnabled) {
					const u32 dstAddress = _bufferAddress + (dstIndex << (_bufferSize - 1));
					const TDst oldValue = _dst[dstIndex];
					_dst[dstIndex] = newValue;
					rollbackLogWriteToRdram(_logSource, dstAddress, oldValue, newValue);
				} else {
					_dst[dstIndex] = newValue;
				}
			}
			++numStored;
		}
		++y;
		_dst += numStored;
	}

	u32 dsty = 0;
	for (; y < _height; ++y) {
		for (u32 x = 0; x < _width && numStored < _numPixels; ++x) {
			c = _src[x + y *_width];
			if (tester(c)) {
				const u32 dstIndex = (x + dsty*_width) ^ _xor;
				const TDst newValue = converter(c, x, y);
				if (logEnabled) {
					const u32 dstAddress = _bufferAddress + (((_startAddress - _bufferAddress) >> (_bufferSize - 1)) + dstIndex) * sizeof(TDst);
					const TDst oldValue = _dst[dstIndex];
					_dst[dstIndex] = newValue;
					rollbackLogWriteToRdram(_logSource, dstAddress, oldValue, newValue);
				} else {
					_dst[dstIndex] = newValue;
				}
			}
			++numStored;
		}
		++dsty;
	}
}

#endif // WriteToRDRAM_H
