#!/bin/bash -e

if [ "x"$1 = "x" ]; then
	echo "Usage: build.sh <prefix> <bootstrap> <make> <install>"
	exit
fi

PREFIX=$1

if [ ! -d "$PREFIX" ]; then
	"$PREFIX"
	exit
fi

echo -e "\e[96mdoppioDB will be installed in $PREFIX\e[0m"

MONET_SOURCE=$PWD
echo "MONET_SOURCE: $MONET_SOURCE"

if [ ! -f "$MONET_SOURCE/bootstrap" ]; then
	echo -e "\e[31mYou are not calling this from doppioDB root\e[0m"
	exit
fi

MONET_BUILD="$PWD/BUILD"
echo -e "\e[96mWe are building in doppioDB in $MONET_BUILD\e[0m"

if [ ! -d "$MONET_BUILD" ]; then
	mkdir BUILD
else
	cd $MONET_BUILD
	if [ -f "$MONET_BUILD/Makefile" ]; then
		make clean
	fi
	cd $MONET_SOURCE
fi

CF="-O3"

echo "CFLAGS: $CF"

if [ "$2" = "bootstrap" ]; then
	./bootstrap
fi

cd $MONET_BUILD
$MONET_SOURCE/configure --prefix=$PREFIX CFLAGS="$CF" $DEBUG

if [ "$3" = "make" ]; then
	cd $MONET_BUILD
	touch ../gdk/gdk_join.c && make -j 16
fi
if [ "$4" = "install" ]; then
	make install
fi