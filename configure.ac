#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.71])
# The version is kept in the top-level VERSION file and read here at autoreconf
# time (m4_esyscmd_s strips the trailing newline).
AC_INIT([knit-graph],[m4_esyscmd_s([cat VERSION])],[https://github.com/knit-sh/knit-graph])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([foreign subdir-objects -Wall])

# Programs.
AC_PROG_CC

# Grammar tooling. Not used until M1, but M0 verifies the toolchain is present.
AC_PROG_YACC
AC_PROG_LEX([noyywrap])

# SQLite3 C library (the only third-party runtime dependency).
AC_CHECK_HEADER([sqlite3.h],[],
  [AC_MSG_ERROR([sqlite3.h not found; install the sqlite3 development library (libsqlite3-dev)])])
AC_SEARCH_LIBS([sqlite3_open_v2],[sqlite3],[],
  [AC_MSG_ERROR([libsqlite3 not found; install the sqlite3 development library (libsqlite3-dev)])])

# Optional coverage instrumentation (--enable-coverage). Wired into CI in M11.
AC_ARG_ENABLE([coverage],
  [AS_HELP_STRING([--enable-coverage],[build with gcov coverage instrumentation])],
  [enable_coverage=$enableval],[enable_coverage=no])
AS_IF([test "x$enable_coverage" = "xyes"],
  [COVERAGE_CFLAGS="--coverage -O0 -g"
   COVERAGE_LDFLAGS="--coverage"])
AC_SUBST([COVERAGE_CFLAGS])
AC_SUBST([COVERAGE_LDFLAGS])

AC_CONFIG_FILES([Makefile src/Makefile tests/Makefile])
AC_OUTPUT
