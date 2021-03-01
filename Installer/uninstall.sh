#!/bin/sh

if [ 0 != $EUID ] ; then
    echo Please use sudo to run
    exit
fi

launchctl unload /Library/LaunchDaemons/com.es.fastio.ipapro.startup.plist
rm -f /Library/LaunchDaemons/com.es.fastio.ipapro.startup.plist

rm -rf /Library/Audio/Plug-Ins/HAL/IPAProPlugin.driver
launchctl kickstart -k system/com.apple.audio.coreaudiod

rm -rf /Applications/AmpController.app
rm -rf /Users/Shared/IPAudioPro
rm -rf /Library/Application\ Support/IPAPro
