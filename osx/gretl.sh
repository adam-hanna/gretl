#!/bin/sh

# if no fonts are available, do
#   sudo fc-cache
# before running this program

# record prior dir
STARTDIR=`pwd`
export "GRETL_STARTDIR=$STARTDIR"

CWD="`(cd \"\`dirname \\\"$0\\\"\`\"; echo $PWD)`"
TOP="`dirname \"$CWD\"`"

# echo "TOP=$TOP" > ~/where

if [ -f ~/.profile ] ; then
  . ~/.profile
fi  

export "GRETL_HOME=$TOP/share/gretl/"
export "GTKSOURCEVIEW_LANGUAGE_DIR=$TOP/share/gretl/gtksourceview"

# location of gnuplot help file
export "GNUHELP=$TOP/share/gnuplot/4.5/gnuplot.gih"
# location of gnuplot X11 driver
export "GNUPLOT_PS_DIR=$TOP/share/gnuplot/4.5/PostScript"
# location of PostScript resources
export "GNUPLOT_DRIVER_DIR=$TOP/libexec/gnuplot/4.5"
# we do not support the 'aqua' terminal type
if [ "$GNUTERM" = "aqua" ] ; then
   export GNUTERM=x11
fi
# R shared library: may require platform-specific symlink
if [ "x$R_HOME" = "x" ] ; then
   export R_HOME=/Library/Frameworks/R.framework/Resources
fi

export "PATH=$CWD:$PATH"

# echo "pwd is `pwd`" >>~/where

cd $CWD
if [ "x$DISPLAY" = "x" ] ; then
  exec "$CWD/gretlcli" "$@" 
else
  exec "$CWD/gretl_x11" "$@"
fi

