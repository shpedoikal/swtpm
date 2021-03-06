#
# configure.ac
#
#       The Initial Developer of the Original Code is International
#       Business Machines Corporation. Portions created by IBM
#       Corporation are Copyright (C) 2014 International Business
#       Machines Corporation. All Rights Reserved.
#
#       This program is free software; you can redistribute it and/or modify
#       it under the terms of the Common Public License as published by
#       IBM Corporation; either version 1 of the License, or (at your option)
#       any later version.
#
#       This program is distributed in the hope that it will be useful,
#       but WITHOUT ANY WARRANTY; without even the implied warranty of
#       MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#       Common Public License for more details.
#
#       You should have received a copy of the Common Public License
#       along with this program; if not, a copy can be viewed at
#       http://www.opensource.org/licenses/cpl1.0.php.
#
#       This file is derived from tpm-tool's configure.in.
#

AC_INIT(swtpm, 0.1.0)
AC_PREREQ(2.12)
AC_CONFIG_SRCDIR(Makefile.am)
AC_CONFIG_HEADER(config.h)

SWTPM_VER_MAJOR=`echo $PACKAGE_VERSION | cut -d "." -f1`
SWTPM_VER_MINOR=`echo $PACKAGE_VERSION | cut -d "." -f2`
SWTPM_VER_MICRO=`echo $PACKAGE_VERSION | cut -d "." -f3`

AC_SUBST([SWTPM_VER_MAJOR])
AC_SUBST([SWTPM_VER_MINOR])
AC_SUBST([SWTPM_VER_MICRO])

dnl Check for programs
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_LIBTOOL

AC_CONFIG_MACRO_DIR([m4])
AC_CANONICAL_TARGET
AM_INIT_AUTOMAKE([foreign 1.6])

DEBUG=""
AC_MSG_CHECKING([for debug-enabled build])
AC_ARG_ENABLE(debug, AC_HELP_STRING([--enable-debug], [create a debug build]),
  [if test "$enableval" = "yes"; then
     DEBUG="yes"
     AC_MSG_RESULT([yes])
   else
     DEBUG="no"
     AC_MSG_RESULT([no])
   fi],
  [DEBUG="no",
   AC_MSG_RESULT([no])])

# If the user has not set CFLAGS, do something appropriate
test_CFLAGS=${CFLAGS+set}
if test "$test_CFLAGS" != set; then
	if test "$DEBUG" == "yes"; then
		CFLAGS="-O0 -g -DDEBUG"
	else
		CFLAGS="-g -O2"
	fi
elif test "$DEBUG" == "yes"; then
	CFLAGS="$CFLAGS -O0 -g -DDEBUG"
fi

AC_HEADER_STDC
AC_C_CONST
AC_C_INLINE

AC_TYPE_SIZE_T
AC_TYPE_SIGNAL

AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_MKDIR_P

AC_ARG_WITH([selinux],
   AS_HELP_STRING([--with-selinux],
      [add SELinux policy extensions @<:@default=check@:>@]))
m4_divert_text([DEFAULTS], [with_selinux=check])

dnl Check for SELinux policy support

if test "$with_selinux" != "no"; then
    if test "$with_selinux" = "check" || test "$with_selinux" = "yes"; then
        if ! test -f /usr/share/selinux/devel/Makefile; then
            if test "$with_selinux" = "yes"; then
                AC_MSG_ERROR("Is selinux-policy-devel installed?")
            else
                with_selinux="no"
            fi
        fi
        AC_PATH_PROG([SEMODULE], semodule)
        if test "x$SEMODULE" == "x"; then
            if test "$with_selinux" = "yes"; then
	        AC_MSG_ERROR("Is selinux-policy-devel installed?")
	    else
	        with_selinux="no"
	    fi
        fi
        if test "$with_selinux" = "check"; then
            with_selinux="yes"
        fi
    fi
fi
AM_CONDITIONAL([WITH_SELINUX], [test "x$with_selinux" == "xyes"])

GLIB_CFLAGS=$(pkg-config --cflags glib-2.0)
if test $? -ne 0; then
	AC_MSG_ERROR("Is glib-2.0 installed? -- could not get cflags")
fi
AC_SUBST([GLIB_CFLAGS])

GLIB_LIBS=$(pkg-config --libs glib-2.0)
if test $? -ne 0; then
	AC_MSG_ERROR("Is glib-2.0 installed? -- could not get libs")
fi
AC_SUBST([GLIB_LIBS])

GTHREAD_LIBS=$(pkg-config --libs gthread-2.0)
if test $? -ne 0; then
	AC_MSG_ERROR("Is glib-2.0 installed? -- could not get libs for gthread-2.0")
fi
AC_SUBST([GTHREAD_LIBS])

cryptolib=freebl

AC_ARG_WITH([openssl],
            AC_HELP_STRING([--with-openssl],
                           [build with openssl library]),
              [AC_CHECK_LIB(crypto,
                            [AES_set_encrypt_key],
                            [],
                            AC_MSG_ERROR(Faulty openssl crypto library))
               AC_CHECK_HEADERS([openssl/aes.h],[],
                            AC_MSG_ERROR(Is openssl-devel/libssl-dev installed?))
               AC_MSG_RESULT([Building with openssl crypto library])
               cryptolib=openssl
              ]
)

case "$cryptolib" in
freebl)
    AM_CONDITIONAL(SWTPM_USE_FREEBL, true)
    AM_CONDITIONAL(SWTPM_USE_OPENSSL, false)
    AC_DEFINE([USE_FREEBL_CRYPTO_LIBRARY],
              [1],
              [use freebl crypto library])

    NSPR_CFLAGS=$(nspr-config --cflags)
    if test $? -ne 0; then
        AC_MSG_ERROR("Could not find nspr-config. Is nspr-devel/libnspr4-dev installed?")
    fi
    AC_SUBST([NSPR_CFLAGS])

    NSS_CFLAGS=$(nss-config --cflags)
    if test $? -ne 0; then
        AC_MSG_ERROR("Could not find nss-config. Is nss-devel/libnss3-dev installed?")
    fi
    AC_SUBST([NSS_CFLAGS])

    NSS_LIBS=$(nss-config --libs)
    if test $? -ne 0; then
        AC_MSG_ERROR("Is nss-devel/libnss3-dev installed? -- could not get libs for nss")
    fi
    dnl On RHEL 7 ppc64 we need an explicit -lfreebl
    NSS_LIBS="$NSS_LIBS -lfreebl"
    AC_SUBST([NSS_LIBS])

    CPPFLAGS="$NSS_CFLAGS $NSPR_CFLAGS"
    AC_CHECK_HEADERS([sslerr.h],[],
                     AC_MSG_ERROR(nss-devel/libnss3-dev is bad))

    # Check for missing headers
    CFLAGS_save="$CFLAGS"
    CFLAGS="$NSS_CFLAGS $NSPR_CFLAGS"
    AC_CHECK_HEADERS([blapi.h],[],
                     AC_MSG_ERROR(nss-softokn-freebl-devel/libnss3-dev is missing blapi.h))
    # Check for missing freebl library or missing library functions
    LIBS_save="$LIBS"
    LIBS="$(nss-config --libs) $(nspr-config --libs)"
    AC_SEARCH_LIBS([AES_CreateContext], [freebl],[],
                   AC_MSG_ERROR("Could not find AES_CreateContext(). Is nss-softokn-freebl-devel/libnss3-dev installed?"),
                   [])
    LIBS="$LIBS_save"
    CPPFLAGS=""
    CFLAGS="$CFLAGS_save"

    ;;
openssl)
    AM_CONDITIONAL(SWTPM_USE_FREEBL, false)
    AM_CONDITIONAL(SWTPM_USE_OPENSSL, true)
    AC_DEFINE([USE_OPENSSL_CRYPTO_LIBRARY],
              [1],
              [use openssl crypto library])
    ;;
esac

LIBTASN1_LIBS=$(pkg-config --libs libtasn1)
if test $? -ne 0; then
	AC_MSG_ERROR("Is libtasn1-devel installed? -- could not get libs for libtasn1")
fi
AC_SUBST([LIBTASN1_LIBS])

LIBTPMS_LIBS=$(pkg-config --libs libtpms)
if test $? -ne 0; then
	AC_MSG_ERROR("Is libtpms-devel installed? -- could not get libs for libtpms")
fi
AC_SUBST([LIBTPMS_LIBS])

AC_PATH_PROG([TPM_NVDEFINE], tpm_nvdefine)
if test "x$TPM_NVDEFINE" == "x"; then
	AC_MSG_ERROR([NVRAM area tools are need: tpm-tools package])
fi

LIBFUSE_CFLAGS=$(pkg-config fuse --cflags)
if test $? -ne 0; then
	AC_MSG_ERROR("Is fuse-devel installed? -- could not get cflags for libfuse")
fi
AC_SUBST([LIBFUSE_CFLAGS])

LIBFUSE_LIBS=$(pkg-config fuse --libs)
if test $? -ne 0; then
	AC_MSG_ERROR("Is fuse-devel installed? -- could not get libs for libfuse")
fi
AC_SUBST([LIBFUSE_LIBS])

if test "x$with_gnutls" != "xno"; then
    GNUTLS_LDFLAGS=$(pkg-config --libs gnutls)
    if test $? -ne 0; then
        if "x$with_gnutls" == "xyes"; then
            AC_MSG_ERROR("Is gnutls installed? -- could not get libs for gnutls")
        else
            with_gnutls=no
        fi
    fi
fi

if test "x$with_gnutls" != "xno"; then
    GNUTLS_CFLAGS=$(pkg-config gnutls --cflags)
    AC_CHECK_LIB([gnutls], [gnutls_x509_crt_set_key], [
                 GNUTLS_LIBS=-lgnutls
             ],
             [if test "x$with_gnutls" == "xyes"; then
                 AC_MSG_ERROR([GNUTLS >= 3.1.0 library not found: libgnutls.so])
              else
                 with_gnutls="no"
              fi])
fi

if test "x$with_gnutls" != "xno"; then
    AC_CHECK_HEADER(gnutls/abstract.h, [], \
             [if test "x$with_gnutls" == "xyes"; then
                 AC_MSG_ERROR([GNUTLS >= 3.1.0 library header not found: gnutls/abstract.h])
              else
                 with_gnutls="no"
              fi])
fi

if test "x$with_gnutls" != "xno"; then
    with_gnutls="yes"
fi
AM_CONDITIONAL([WITH_GNUTLS], [test "x$with_gnutls" == "xyes"])
AC_SUBST([GNUTLS_LIBS])

AC_PATH_PROG([EXPECT], expect)
if test "x$EXPECT" == "x"; then
	AC_MSG_ERROR([expect is required: expect package])
fi

HARDENING_CFLAGS="-fstack-protector-all -Wstack-protector -pie -fPIE "
HARDENING_CFLAGS+="-D_FORTIFY_SOURCE=2 -Wl,-z,relro,-z,now"
AC_SUBST([HARDENING_CFLAGS])

CFLAGS="$CFLAGS -Wreturn-type -Wsign-compare -Wswitch-enum"
CFLAGS="$CFLAGS -Wmissing-prototypes -Wall -Werror"
CFLAGS="$CFLAGS -Wformat -Wformat-security"

AC_CONFIG_FILES(Makefile                    \
		dist/swtpm.spec             \
		etc/Makefile                \
		samples/Makefile            \
		include/Makefile            \
		include/swtpm/Makefile      \
		src/Makefile                \
		src/selinux/Makefile        \
		src/swtpm/Makefile          \
		src/swtpm/swtpm.h           \
		src/swtpm_bios/Makefile     \
		src/swtpm_cert/Makefile     \
		src/swtpm_ioctl/Makefile    \
		src/swtpm_setup/Makefile    \
		man/Makefile                \
		man/man8/Makefile           \
		tests/Makefile              \
		)
AC_OUTPUT

echo
printf "with_gnutls : %5s  (no = swtpm_cert will NOT be built)\n" $with_gnutls
printf "with_selinux: %5s  (no = SELinux policy extenions will NOT be build)\n" $with_selinux
echo
echo "CFLAGS=$CFLAGS"
echo "LDFLAGS=$LDFLAGS"
