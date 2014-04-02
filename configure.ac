#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.68])
AC_INIT([ml-stress], [0.1], [safchain@gmail.com])
AC_CONFIG_SRCDIR([src/drone.c])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CC

# Checks for libraries.
# FIXME: Replace `main' with a function in `-levent':
AC_CHECK_LIB([event], [main])

# Checks for header files.
AC_CHECK_HEADERS([arpa/inet.h inttypes.h stdlib.h string.h sys/time.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_INT32_T
AC_TYPE_INT64_T
AC_TYPE_SIZE_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_UINT8_T

# Checks for library functions.
AC_FUNC_MALLOC
AC_CHECK_FUNCS([memset])

AC_ARG_WITH([protobuf-libdir],
    [AS_HELP_STRING([--with-protobuf-libdir=LIB_DIR],
        [location of the protocol buffers libraries, defaults to /usr/lib])],
    [PROTOBUF_LIBDIR="$withval"],
    [PROTOBUF_LIBDIR='/usr/lib'])
AC_SUBST([PROTOBUF_LIBDIR])

LDFLAGS="$LDFLAGS -L$PROTOBUF_LIBDIR"

AC_CHECK_LIB([protobuf-c], [main], [], [AC_MSG_ERROR([cannot find protobuf-c library])])
AC_CHECK_LIB([protobuf], [main], [], [AC_MSG_ERROR([cannot find protobuf library])])

AC_ARG_WITH([protoc-c],
    [AS_HELP_STRING([--with-protoc-c=PATH],
        [location of the protoc-c protocol buffer compiler binary, defaults to protoc-c])],
    [PROTOC_C="$withval"],
    [PROTOC_C='protoc-c'])
AC_SUBST([PROTOC_C])

AC_ARG_WITH([protoc],
    [AS_HELP_STRING([--with-protoc=PATH],
        [location of the protoc protocol buffer compiler binary, defaults to protoc])],
    [PROTOC="$withval"],
    [PROTOC='protoc'])
AC_SUBST([PROTOC])

AC_CONFIG_FILES([Makefile
                 src/Makefile])

AM_INIT_AUTOMAKE([ml-stress], [0.1])

AC_OUTPUT
