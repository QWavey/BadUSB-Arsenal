# Backup & restore

Every milestone commit on `main` has an annotated git tag of the form
`backup-NN-<desc>-YYYYMMDD`. Tags survive branch deletion, so even
after branch cleanup you can always roll back by name:

```bash
git tag -l 'backup-*'                       # see what's available
git log backup-10-ide-20261011 -1 --stat     # inspect a tag
git checkout -b restore-ide backup-10-ide-20261011   # a branch at that point
# ... fix things ...
git push -u origin restore-ide
```

To revert `main` to a specific milestone (destructive):

```bash
git checkout main
git reset --hard backup-10-ide-20261011
git push --force-with-lease origin main
```

Tags are pushed with `git push --follow-tags`.

## On-device backup

Device state is stored at:
- `/root/ghostlink/config.txt`         — editable config
- `/root/ghostlink/payloads/`          — user payloads
- `/root/ghostlink/extensions/`        — user extension library
- `/root/ghostlink/layouts/`           — vendored Hak5 layouts
- `/root/ghostlink/records/`           — captured pcap files (large: back up what you need)
- `/root/ghostlink/verdicts/`          — verdict stream (jsonl)
- `/root/ghostlink/identity.env`       — clone identity override, if set
- `/etc/init.d/S50usbdevice`           — stock init with our one-time
                                          sed patch (sources identity.env)
- `/etc/init.d/S50usbdevice.bak.ghostlink` — original stock pin for restore

To rescue a bricked device: flash the stock Luckfox update.img via
SocToolKit, then `adb push` the latest `onboard/` to `/tmp/onboard` and run
`cd /tmp/onboard && sh ./install.sh`.
