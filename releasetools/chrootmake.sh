#!/bin/sh
set -e
export SHELL=/bin/sh
cd /usr/src 

if [ $# -gt 0 ]
then	make $@
	exit $?
fi
make world
cp /usr/mdec/boot_monitor /
cp /boot/latest/* /boot/release/

if [ $MAKEMAP -ne 0 ]; then
	find . -type f -perm 755 | xargs nm -n 2> /dev/null > symbols.txt
fi
make clean cleandepend
