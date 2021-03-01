#!/bin/sh

rm -rf /Library/Audio/Plug-Ins/HAL/IPAProPlugin.driver
launchctl kickstart -k system/com.apple.audio.coreaudiod
launchctl unload /Library/LaunchDaemons/com.es.fastio.ipapro.startup.plist
rm /Users/Shared/IPAudioPro/IPAProUS.log
echo ok
