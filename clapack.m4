# Configure paths for lapack
# Allin Cottrell <cottrell@wfu.edu>, March 2003

dnl AM_PATH_LAPACK([MINIMUM-VERSION, [ACTION-IF-FOUND [, ACTION-IF-NOT-FOUND]]])
dnl Test for LAPACK, and define LAPACK_CFLAGS and LAPACK_LIBS.
dnl
AC_DEFUN(AM_PATH_LAPACK,
[dnl 
AC_ARG_WITH(lapack-prefix,[  --with-lapack-prefix=PFX   Prefix where LAPACK is installed (optional)],
            lapack_config_prefix="$withval", lapack_config_prefix="")

  if test x$lapack_config_prefix != x ; then
     lapack_config_args="$lapack_config_args --prefix=$lapack_config_prefix"
  fi

  AC_MSG_CHECKING(for LAPACK)
	
  AC_CHECK_LIB(f2c,dmax,FLIB="f2c",FLIB="none")
  if test $FLIB = "none" ; then
    AC_CHECK_LIB(g2c,dmax,FLIB="g2c",FLIB="none")
  fi
  if test $FLIB = "none" ; then
     echo "*** Couldn't find either libf2c or libg2c"
  fi

  LAPACK_CFLAGS="-I$lapack_config_prefix/include -I./plugin"
  LAPACK_LIBS="-L$lapack_config_prefix/lib -llapack -lblas -l$FLIB"

  ac_save_LIBS="$LIBS"
  CFLAGS="$CFLAGS $LAPACK_CFLAGS"
  LIBS="$LAPACK_LIBS $LIBS"

dnl
dnl Check the installed LAPACK.
dnl
  rm -f conf.lapacktest
  AC_TRY_RUN([
#include <stdlib.h>
#include <f2c.h>

int 
main ()
{
  integer ispec;
  real zero = 0.0;
  real one = 1.0;

  ieeeck_(&ispec, &zero, &one);
  system ("touch conf.lapacktest");
  return 0;
}
],, no_lapack=yes,[echo $ac_n "cross compiling; assumed OK... $ac_c"])
  CFLAGS="$ac_save_CFLAGS"
  LIBS="$ac_save_LIBS"

  if test "x$no_lapack" = x ; then
     AC_MSG_RESULT(yes)
     ifelse([$2], , :, [$2])     
  else
     AC_MSG_RESULT(no)
     if test -f conf.lapacktest ; then
       :
     else
       echo "*** Could not run GNU MP test program, checking why..."
       CFLAGS="$CFLAGS $LAPACK_CFLAGS"
       LIBS="$LIBS $LAPACK_LIBS"
       AC_TRY_LINK([
#include <f2c.h>
#include <stdio.h>
],     [ return (1); ],
       [ echo "*** The test program compiled, but did not run. This usually means"
         echo "*** that the run-time linker is not finding LAPACK. If it is not"
         echo "*** finding LAPACK, you'll need to set your LD_LIBRARY_PATH "
         echo "*** environment variable, or edit /etc/ld.so.conf to point"
         echo "*** to the installed location.  Also, make sure you have run"
         echo "*** ldconfig if that is required on your system."
         echo "***" ],
       [ echo "*** The test program failed to compile or link. See the file config.log for the"
         echo "*** exact error that occured. This usually means LAPACK was incorrectly installed"
         echo "*** or that you have moved LAPACK since it was installed." ])
         CFLAGS="$ac_save_CFLAGS"
         LIBS="$ac_save_LIBS"
     fi
     LAPACK_CFLAGS=""
     LAPACK_LIBS=""
     ifelse([$3], , :, [$3])
  fi
  AC_SUBST(LAPACK_CFLAGS)
  AC_SUBST(LAPACK_LIBS)
  rm -f conf.lapacktest
])
