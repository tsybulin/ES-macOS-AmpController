#!/bin/sh

PRJDIR=`dirname $0`

cd ${PRJDIR}

./01.clean.sh
./02.xcodebuild.sh
./03.pkgbuild.sh
