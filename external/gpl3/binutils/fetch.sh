#!/bin/sh

# Make sure we're in our directory (i.e., where this shell script is)
echo $0
cd `dirname $0`

# Use an available download utility
FETCH=../../../tools/fetch.sh

# Fetch sources if not available
if [ ! -d dist ];
then
	if [ ! -f binutils-2.17a.tar.bz2 ];
	then
		sh $FETCH http://ftp.gnu.org/gnu/binutils/binutils-2.17a.tar.bz2
	fi

	tar -xf binutils-2.17a.tar.bz2 && \
	mv binutils-2.17 dist && \
	cd dist && \
	cat ../../../../tools/binutils/patches/* | patch -p0
fi

