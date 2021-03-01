#!/bin/sh

#  install.sh
#  AudioIO24
#
#  Created by Pavel Tsybulin on 2/15/19.
#  Copyright © 2019 Pavel Tsybulin. All rights reserved.

ln -s "/Library/Application Support/IPAPro/LaunchDaemonsUS/com.es.fastio.ipapro.startup.plist" /Library/LaunchDaemons
launchctl load /Library/LaunchDaemons/com.es.fastio.ipapro.startup.plist
launchctl kickstart -k system/com.apple.audio.coreaudiod

echo ok
