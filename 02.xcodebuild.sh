#!/bin/sh


for PRJ in 'AmpController' 'IPAProPlugin' 'IPAProUS' ; do
#    echo $PRJ
    cd $PRJ
    xcodebuild
    cd ..
done
