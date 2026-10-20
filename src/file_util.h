#ifndef FILE_UTIL_H
#define FILE_UTIL_H

#include <stddef.h>

/*
 * Replace path with data without ever leaving it half-written: write
 * path.tmp, fsync it, then rename it over path. A power-off or sleep part
 * way through leaves either the old file or the new one.
 * Returns 0 on success, -1 on failure (path is then unchanged).
 */
int write_file_atomic(const char *path, const void *data, size_t len);

#endif /* FILE_UTIL_H */
