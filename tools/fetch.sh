#!/bin/sh

if [ `which wget` ]
then
	wget $1
elif [ `which curl` ]
then
	curl $1 > `basename $1`
elif [ `which fetch` ]
then
	fetch $1
else
	echo "ERROR: $0: No download utility found."
	exit
fi
