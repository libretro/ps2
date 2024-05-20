#ifndef __CORETYPES_H__
#define __CORETYPES_H__

#include <stdint.h>
#include <stdio.h>

#include <streams/file_stream.h>

#ifdef __cplusplus
extern "C" {
#endif

int rferror(RFILE* stream);
RFILE* rfopen(const char *path, const char *mode);
int rfclose(RFILE* stream);
int64_t rftell(RFILE* stream);
int64_t rfseek(RFILE* stream, int64_t offset, int origin);
int64_t rfwrite(void const* buffer,
   size_t elem_size, size_t elem_count, RFILE* stream);
int64_t rfread(void* buffer,
   size_t elem_size, size_t elem_count, RFILE* stream);
int rfgetc(RFILE* stream);
int rfeof(RFILE* stream);
char *rfgets(char *buffer, int maxCount, RFILE* stream);

#ifdef __cplusplus
}
#endif

#define ARRAY_LENGTH(x) (sizeof(x)/sizeof(x[0]))

#if defined(__PS3__) || defined(__PSL1GHT__)
#undef UINT32
#undef UINT16
#undef UINT8
#undef INT32
#undef INT16
#undef INT8
#endif

typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef uint8_t UINT8;

typedef int64_t INT64;
typedef int32_t INT32;
typedef int16_t INT16;
typedef int8_t INT8;

typedef struct chd_core_file {
	/*
	 * arbitrary pointer to data the implementation uses to implement the below functions
	 */
	void *argp;

	/*
	 * return the size of a given file as a 64-bit unsigned integer.
	 * the position of the file pointer after calling this function is
	 * undefined because many implementations will seek to the end of the
	 * file and call ftell.
	 *
	 * on error, (UINT64)-1 is returned.
	 */
	UINT64(*fsize)(struct chd_core_file*);

	/*
	 * should match the behavior of fread, except the FILE* argument at the end
	 * will be replaced with a struct chd_core_file*.
	 */
	size_t(*fread)(void*,size_t,size_t,struct chd_core_file*);

	// closes the given file.
	int (*fclose)(struct chd_core_file*);

	// fseek clone
	int (*fseek)(struct chd_core_file*, INT64, int);
} core_file;

static inline int core_fclose(core_file *fp) {
	return fp->fclose(fp);
}

static inline size_t core_fread(core_file *fp, void *ptr, size_t len) {
	return fp->fread(ptr, 1, len, fp);
}

static inline int core_fseek(core_file* fp, INT64 offset, int whence) {
	return fp->fseek(fp, offset, whence);
}

static inline UINT64 core_fsize(core_file *fp)
{
	return fp->fsize(fp);
}

#endif
