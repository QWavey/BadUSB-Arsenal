#!/bin/sh
# Shark-Jack-style attack payload (PAYLOAD_SWITCH2, two button taps).
# Flips REDIRECT on and lets it run for 60s then flips it back.
#
# Only runs because the user explicitly chose it with their own button.

CFG=/mnt/sd/config.txt
sed -i 's/^REDIRECT=.*/REDIRECT=TRUE/' "$CFG"
# nudge ghostlink to reload
touch "$CFG"
sleep 60
sed -i 's/^REDIRECT=.*/REDIRECT=FALSE/' "$CFG"
touch "$CFG"
