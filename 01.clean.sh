#!/bin/sh


for PRJ in 'AmpController' 'IPAProPlugin' 'IPAProUS' ; do
#    echo $PRJ
    cd $PRJ
    xcodebuild clean
    cd ..
done

rm -f Installer/build/IPAProAmp.pkg
