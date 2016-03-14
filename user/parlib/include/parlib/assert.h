/* See COPYRIGHT for copyright information. */

#pragma once

#include <assert.h>
/* For __BEGIN_DECLS.  Most every header gets it already from features.h. */
#include <sys/cdefs.h>
/* So we can undefine warn */
#include <err.h>

__BEGIN_DECLS

#undef assert
#undef warn

void _warn(const char*, int, const char*, ...);
void _panic(const char*, int, const char*, ...) __attribute__((noreturn));
void _assert_failed(const char *file, int line, const char *msg)
     __attribute__((noreturn));

#define warn(...) _warn(__FILE__, __LINE__, __VA_ARGS__)
#define panic(...) _panic(__FILE__, __LINE__, __VA_ARGS__)

#define assert(x)	                                                           \
	do {                                                                       \
		if (!(x))                                                              \
			_assert_failed(__FILE__, __LINE__, #x);                            \
	} while (0)

// parlib_static_assert(x) will generate a compile-time error if 'x' is false.
#define parlib_static_assert(x)	switch (x) case 0: case (x):

__END_DECLS
