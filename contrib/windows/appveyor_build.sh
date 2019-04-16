#!/bin/sh
# This file is a part of Julia. License is MIT: https://julialang.org/license

# Script to compile Windows Julia, using binary dependencies from nightlies.
# Should work in MSYS assuming 7zip is installed and on the path,
# or Cygwin or Linux assuming make, curl, p7zip, and mingw64-$ARCH-gcc-g++
# are installed

# Run in top-level Julia directory
cd `dirname "$0"`/../..
# Stop on error
set -e
# Make sure stdin exists (not true on appveyor msys2)
exec < /dev/null

curlflags="curl --retry 10 -k -L -y 5"
checksum_download() {
  # checksum_download filename url
  f=$1
  url=$2
  if [ -e "$f" ]; then
    deps/tools/jlchecksum "$f" 2> /dev/null && return
    echo "Checksum for '$f' changed, download again." >&2
  fi
  echo "Downloading '$f'"
  $curlflags -O "$url"
  deps/tools/jlchecksum "$f"
}

# If ARCH environment variable not set, choose based on uname -m
if [ -z "$ARCH" -a -z "$XC_HOST" ]; then
  export ARCH=`uname -m`
elif [ -z "$ARCH" ]; then
  ARCH=`echo $XC_HOST | sed 's/-w64-mingw32//'`
fi

echo "" > Make.user
echo "" > get-deps.log
# set MARCH for consistency with how binaries get built
if [ "$ARCH" = x86_64 ]; then
  bits=64
  archsuffix=64
  exc=seh
  echo "override MARCH = x86-64" >> Make.user
  echo 'USE_BLAS64 = 1' >> Make.user
  echo 'LIBBLAS = -L$(JULIAHOME)/usr/bin -lopenblas64_' >> Make.user
  echo 'LIBBLASNAME = libopenblas64_' >> Make.user
else
  bits=32
  archsuffix=86
  exc=sjlj
  echo "override MARCH = pentium4" >> Make.user
  echo 'LIBBLAS = -L$(JULIAHOME)/usr/bin -lopenblas' >> Make.user
  echo 'LIBBLASNAME = libopenblas' >> Make.user
fi
echo "override JULIA_CPU_TARGET=generic;native" >> Make.user

# Set XC_HOST if in Cygwin or Linux
case $(uname) in
  CYGWIN*)
    if [ -z "$XC_HOST" ]; then
      XC_HOST="$ARCH-w64-mingw32"
      echo "XC_HOST = $XC_HOST" >> Make.user
    fi
    CROSS_COMPILE="$XC_HOST-"
    # Set BUILD_MACHINE and HOSTCC in case we don't have Cygwin gcc installed
    echo "override BUILD_MACHINE = $ARCH-pc-cygwin" >> Make.user
    if [ -z "`which gcc 2>/dev/null`" ]; then
      echo 'override HOSTCC = $(CROSS_COMPILE)gcc' >> Make.user
    fi
    SEVENZIP="7z"
    ;;
  Linux)
    if [ -z "$XC_HOST" ]; then
      XC_HOST="$ARCH-w64-mingw32"
      echo "XC_HOST = $XC_HOST" >> Make.user
    fi
    CROSS_COMPILE="$XC_HOST-"
    make win-extras >> get-deps.log
    SEVENZIP="wine dist-extras/7z.exe"
    ;;
  *)
    CROSS_COMPILE=""
    SEVENZIP="7z"
    ;;
esac

if [ -z "$USEMSVC" ]; then
  if [ -z "`which ${CROSS_COMPILE}gcc 2>/dev/null`" ]; then
    f=$ARCH-4.9.2-release-win32-$exc-rt_v4-rev3.7z
    checksum_download \
        "$f" "https://bintray.com/artifact/download/tkelman/generic/$f"
    echo "Extracting $f"
    $SEVENZIP x -y $f >> get-deps.log
    export PATH=$PWD/mingw$bits/bin:$PATH
    # If there is a version of make.exe here, it is mingw32-make which won't work
    rm -f mingw$bits/bin/make.exe
  fi
  export AR=${CROSS_COMPILE}ar
  mkdir -p usr/tools
else
  echo "override USEMSVC = 1" >> Make.user
  echo "override ARCH = $ARCH" >> Make.user
  echo "override XC_HOST = " >> Make.user
  export CC="$PWD/deps/srccache/libuv/compile cl -nologo -MD -Z7"
  export AR="$PWD/deps/srccache/libuv/ar-lib lib"
  export LD="$PWD/linkld link"
  echo "override CC = $CC" >> Make.user
  echo 'override CXX = $(CC) -EHsc' >> Make.user
  echo "override AR = $AR" >> Make.user
  echo "override LD = $LD -DEBUG" >> Make.user

  f=llvm-3.3-$ARCH-msvc12-juliadeps.7z
  checksum_download \
      "$f" "https://bintray.com/artifact/download/tkelman/generic/$f"
  echo "Extracting $f"
  $SEVENZIP x -y $f >> get-deps.log
fi

if [ -z "`which make 2>/dev/null`" ]; then
  if [ -n "`uname | grep CYGWIN`" ]; then
    echo "Install the Cygwin package for 'make' and try again."
    exit 1
  fi
  f="/make/make-3.81-2/make-3.81-2-msys-1.0.11-bin.tar"
  if ! [ -e `basename $f.lzma` ]; then
    echo "Downloading `basename $f`"
    $curlflags -O http://sourceforge.net/projects/mingw/files/MSYS/Base$f.lzma
  fi
  $SEVENZIP x -y `basename $f.lzma` >> get-deps.log
  tar -xf `basename $f`
  export PATH=$PWD/bin:$PATH
fi

if [ -n "$USEMSVC" ]; then
  # Openlibm doesn't build well with MSVC right now
  echo 'USE_SYSTEM_OPENLIBM = 1' >> Make.user
  # Since we don't have a static library for openlibm
  echo 'override UNTRUSTED_SYSTEM_LIBM = 0' >> Make.user

  # Compile libuv and utf8proc without -TP first, then add -TP
  make -C deps install-libuv install-utf8proc
  cp usr/lib/uv.lib usr/lib/libuv.a
  echo 'override CC += -TP' >> Make.user
  echo 'override DEP_LIBS += dsfmt' >> Make.user

  # Create a modified version of compile for wrapping link
  sed -e 's/-link//' -e 's/cl/link/g' -e 's/ -Fe/ -OUT:/' \
    -e 's|$dir/$lib|$dir/lib$lib|g' deps/srccache/libuv/compile > linkld
  chmod +x linkld
else
  # Use BinaryBuilder
  echo 'USE_BINARYBUILDER_LLVM = 1' >> Make.user
  echo 'USE_BINARYBUILDER_OPENBLAS = 1' >> Make.user
  echo 'USE_BINARYBUILDER_SUITESPARSE = 1' >> Make.user
  echo 'BINARYBUILDER_LLVM_ASSERTS = 1' >> Make.user
  echo 'override DEP_LIBS += llvm openlibm openblas suitesparse' >> Make.user
  export CCACHE_DIR=/cygdrive/c/ccache
  echo 'USECCACHE=1' >> Make.user
  make check-whitespace
  make VERBOSE=1 -C base version_git.jl.phony
  echo 'NO_GIT = 1' >> Make.user
fi
echo 'FORCE_ASSERTIONS = 1' >> Make.user

cat Make.user
make -j3 VERBOSE=1 release
make -j3 VERBOSE=1 install
make VERBOSE=1 JULIA=../../usr/bin/julia.exe BIN=. "$(make print-CC)" -C test/embedding release
make build-stats
ccache -s
