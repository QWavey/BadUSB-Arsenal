# Tasks — GhostLink UI pass + Clone + invent + bug-hunt 5

1. [x] Add tagged backup pins + document restore
2. [x] Fix German layout "." → "," hang in ducky.py
3. [x] Add Clear-records button to the Live pane
4. [x] Add Start/Stop verdict-stream toggle to the Live pane
5. [x] Remove the HID tab from the WebUI
6. [x] Rework the Scripting tab layout (compact sidebar + wider editor)
7. [x] Add the Clone tab (identity override on next boot)
8. [x] Patch S50usbdevice via install.sh to read /root/ghostlink/identity.env
9. [x] Ship + verify the above UI changes on hardware
10. [x] Ship + verify Clone end-to-end
11. [x] Replace checkbox-UI with real toggle switches (ESP-S3 style)
12. [x] Live-save for switches (opt-out via Config → livesave)
13. [x] Honeypot tab + backend (upload index.html, capture creds, post-signin redirect, rule DSL)
14. [x] Investigate HID disable → device unreachable "sometimes"
       Root cause: every /api/config POST unconditionally kicked dnsmasq,
       briefly dropping DHCP → host could lose lease. Fix: compare old
       vs new config, only reload dnsmasq when a DHCP/DNS/REDIRECT key
       actually changed. (webui.py)
15. [x] Clone from DHCP: "connected hosts" list with per-host "Clone"
       button, OEM lookup via OUI, OS guess from hostname pattern,
       Internet reachability + default-route iface probe. (NEW — user ask)
16. [ ] /invent once — PCAP Time-Machine (scrub-bar replay in Query)
17. [ ] /bug-hunt round 1 — Clone code path
18. [ ] /bug-hunt round 2 — Honeypot HTML upload + loot capture
19. [ ] /bug-hunt round 3 — Live-save races / state carry
20. [ ] /bug-hunt round 4 — forwarder.py + German layout + HID flap
21. [ ] /bug-hunt round 5 — settings persistence across polling tabs
22. [ ] Consolidate branches to main + delete the others
23. [ ] Push main + tags + /shutdown-when-done

---

For 1. You should create `docs/BACKUP.md` describing restore procedure
plus push annotated git tags for every milestone commit (one per real
commit on main) so branch deletion doesn't lose any history.
Done when: `git tag -l 'backup-*'` lists ≥ 8 tags and `docs/BACKUP.md`
explains how to restore from a tag.

For 2. You should trace the ducky.py STRING loop for the German
layout to find why "." → "," stalls. Fix in de.json or the modifier
reset path. Done when: a `STRING hello, world.` payload on `HID_LAYOUT=de`
types without a visible stall between the comma and following chars.

For 3. You should add a Clear-records button to the Live header that
POSTs /api/clear, with a confirm, visible whether recording is on or
off. Done when: clicking the button while REC is ON wipes
/root/ghostlink/records/*.{pcap,jsonl} and the UI's record count
drops to 0.

For 4. You should add a Start/Stop button for the verdict stream next
to the record toggle. It flips VERDICT_STREAM in config.txt live.
sniff.py already picks up the change on mtime. Done when: toggling
Stop closes the current .jsonl and no new frames land in it until
Start is clicked again.

For 5. You should remove the HID tab button and its section from the
HTML, delete related JS (refreshPayloads, firePayload UI wiring —
leave the `/api/hid` endpoint for programmatic fire). Done when:
`curl /index.html | grep data-tab="HID"` returns nothing.

For 6. You should restructure the Scripting pane so the editor gets
≥ 70% of the width, the sidebar is one compact scrollable column
(Scripts list → Extensions list → Layout picker), and vertical empty
space below the editor is removed. Done when: the layout matches the
spec and no empty gap sits below the toolbar on a 1600×900 window.

For 7. You should add a Clone tab to the nav + a `<section>` holding
form fields for USB VID, PID, Manufacturer, Product, Serial. Buttons:
Save, Clear override, Reboot. Done when: entering values + Save POSTs
to /api/clone/identity and the identity.env file appears on-device
with the right values.

For 8. You should patch /etc/init.d/S50usbdevice via install.sh so that
sourcing a sidecar env file overrides the hardcoded idVendor/idProduct/
strings. Must be idempotent and leave the stock behaviour intact when
identity.env is absent. Done when: with identity.env present on a
fresh boot, the gadget enumerates with the overridden VID/PID.

For 9. You should push onboard/ to the device, run install.sh, restart
services, and verify every item above still works + no regressions.
Done when: all the smoke tests from steps 2-8 pass live.

For 10. You should ship the PCAP time-machine: a scrub-bar in the
Query pane that pages through the existing JSONL verdict files with a
time index. Done when: moving the slider replays verdicts in order at
up to 10x speed and the pane shows which rule/host was hit at each
instant.

For 11. You should hunt the Clone subsystem: env injection safety
(sh special chars in identity values), S50 patch idempotency, missing
file handling, and the restore procedure. Fix what's real. Done when:
intentional bad values (quotes, $, `, $(...)) don't break the shell
include; patching twice does nothing new.

For 12. You should hunt the new Scripting UI + the file-listing API:
inconsistent list state across openTab(Scripting), extension save
after rename, scripts list refresh race. Done when: no stale items
after save+delete cycles.

For 13. You should hunt forwarder.py: double-start races, missing
iface, flap during start, select-starvation. Done when: killing one
side and plugging it back in keeps forwarding without manual restart.

For 14. You should hunt German layout + ducky.py inter-key behaviour:
missing release report between consecutive shifted characters,
char_delay_ms interaction with modifier reset. Done when: typing a
mix of plain, shifted, and altgr characters streams without stalls.

For 15. You should hunt settings persistence: tabs that overwrite
config.txt on their background poll (e.g. the live polling tick
clobbering a mid-edit textarea). Done when: making a form edit on
Config + letting tick fire + saving still saves the user's edit.

For 16. You should delete the `feature/ide` and `backup-pre-ide`
branches locally and on origin (the tags from step 1 are the backups),
fast-forward main to the final HEAD, and push `main` + all tags.
Done when: `git branch -a` shows only `main` and
`git ls-remote --heads origin` lists only `main`.

For 17. You should push main + tags, then invoke the shutdown skill.
Done when: `git push origin main --follow-tags` completes clean and
the shutdown command has been issued.

## GitHub issues (synced 2026-10-24)

Source: open issues of QWavey/webbee-ghostlink. #1 is an audit umbrella; its 36 bullets are filed as #2-#37 and tracked below. HELD items touch attack or evasion code (deadnet/deauth, HID injection, the usb0/eth0 forwarder and redirect/DNS spoof paths, credential capture). They are not changed. Reason for every HELD item: attack behaviour, needs authorization context.

- [ ] #2 Non-empty credentials committed in SD config template — sdcard-template/config.txt:61,149 — FIX — pending
- [ ] #3 Unauthenticated config dump in shell-PoC web server — sdcard-template/ghostlink.sh:232-245 — FIX — pending
- [ ] #4 Default admin password, all-interface bind, cleartext Basic auth — src/core/config.c:72-74,113,218; src/web/httpd.c — FIX — pending (partial: bind and plain HTTP remain, see status when done)
- [ ] #5 Config values interpolated into system() commands — src/net/bridge.c:32,85-160; src/core/main.c:171-182 — FIX — pending (validation at config parse; bridge.c and main.c unchanged)
- [ ] #6 Shell eval of config values in SD launcher — sdcard-template/ghostlink.sh:57-65 — FIX — pending
- [ ] #7 Path traversal in HID payload endpoint — src/web/httpd.c:292-300; src/hid/ducky.c:112-115 — HELD — attack behaviour, needs authorization context
- [ ] #8 Config save can write a truncated file — src/web/httpd.c:204-212,311-337 — FIX — pending
- [ ] #9 Config-controlled strings inserted via innerHTML — src/web/pages/app.js:13-26,36; src/web/httpd.c:166-186 — FIX — pending
- [ ] #10 Insecure defaults: root SSH with password auth and destructive panic scrub — src/core/config.c:98-101,141-142 — FIX — pending (partial: SSH keys are not read anywhere in this repo, so the stock sshd config still needs checking on device)
- [ ] #11 Sniffer persists usernames and partial passwords — onboard/sniff.py:440-501 — HELD — attack behaviour, needs authorization context
- [ ] #12 Expression evaluator is not a sandbox — onboard/ducky.py:278,443,600-614 — HELD — attack behaviour, needs authorization context
- [ ] #13 Root scripts write predictable /tmp paths — onboard/install.sh:44-46; onboard/hid_enable.sh:30-31; sdcard-template/ghostlink.sh:159-190 — FIX — pending
- [ ] #14 WebUI config save truncates and overwrites config.txt — src/web/httpd.c:204-212,311-337 — FIX — pending
- [ ] #15 Unchecked rotate() failure leaves g_pcap NULL and crashes recorder — src/record/recorder.c:259-268 — FIX — pending
- [ ] #16 Forwarder can echo frames between usb0 and eth0 without end — onboard/forwarder.py:121-130 — HELD — attack behaviour, needs authorization context
- [ ] #17 recorder_stop() can block forever on an idle interface — src/record/recorder.c:246,286-290 — FIX — pending
- [ ] #18 Redirect DNS answers point at REDIRECT_TO, not the device — src/net/redirect.c:85-173 — HELD — attack behaviour, needs authorization context
- [ ] #19 WebUI toggles cannot turn values off on CRLF config files — src/web/pages/app.js:69-78 — FIX — pending
- [ ] #20 Config parser truncates any value containing '#' — src/core/config.c:372-378; onboard/redirect302.py:23 — FIX — pending (C parser fixed; redirect302.py part HELD, redirect code)
- [ ] #21 /api/query returns invalid JSON and sends conflicting length headers — src/web/httpd.c:254-289 — FIX — pending
- [ ] #22 deadnet_stop() joins threads based on current config — src/net/deadnet.c:201-213 — HELD — attack behaviour, needs authorization context
- [ ] #23 Stop does not unblock listeners, so restart fails or runs two accept loops — src/web/httpd.c:373-412; src/net/redirect.c:79-237 — FIX — pending (WebUI part; redirect part HELD)
- [ ] #24 Hot reload and layout table are unsynchronised despite the documented RW lock — src/core/config.c:351; src/hid/layouts.c:33-63 — FIX — pending (config half; layout table part HELD, HID code)
- [ ] #25 DeadNet IPv4 reads the interface address once and can poison 0.0.0.x — src/net/deadnet.c:96-119 — HELD — attack behaviour, needs authorization context
- [ ] #26 POST /api/config truncates bodies larger than one recv() — src/web/httpd.c:311-337 — FIX — pending
- [ ] #27 Recorder restarts its file index at 1 each boot and overwrites captures — src/record/recorder.c:70-72,157-163 — FIX — pending
- [ ] #28 config.txt is parsed four different ways; C parser cuts values at any '#' — src/core/config.c:376-378; onboard/ghostlink.sh:19-23; onboard/button.py:49-62; onboard/redirect302.py:17-24 — FIX — pending (redirect302.py and its RED_TO default HELD, redirect code)
- [ ] #29 Stop and reconfigure paths can hang or leave threads running — src/record/recorder.c:246,286-289 — FIX — pending (recorder part; redirect and deadnet parts HELD)
- [ ] #30 forwarder.py loop guard relies on MAC matching; exits on link flaps — onboard/forwarder.py:42-130 — HELD — attack behaviour, needs authorization context
- [ ] #31 Chunked /api/query response is malformed and /api/hid accepts path traversal — src/web/httpd.c:118-130,254-300 — FIX — pending (/api/query part; /api/hid path part HELD, HID injection)
- [ ] #32 WebUI inserts unescaped values into innerHTML; toggle logic misreads values — src/web/pages/app.js:13-75 — FIX — pending
- [ ] #33 button.py ignores live config for timing keys and mishandles hold-then-tap — onboard/button.py:124-198 — HELD — attack behaviour, needs authorization context
- [ ] #34 HID payloads run blocking on the button thread, unlocked, with stale layouts — src/hid/ducky.c:33-165; src/hid/layouts.c:24-33 — HELD — attack behaviour, needs authorization context
- [ ] #35 Redirect DNS and HTTP replies have protocol and header defects — src/net/redirect.c:7-206 — HELD — attack behaviour, needs authorization context
- [ ] #36 Makefile lacks header dependencies; no tests; repeated work in deadnet loop — Makefile:44-58; src/core/config.c:9-18,467 — FIX — pending (header deps and unistd.h; deadnet.c items HELD; host test target is an improvement, SKIP)
- [ ] #37 Docs and logs disagree with code; device defaults are weak — src/core/main.c:10,193,240,243,258; src/net/gadget.c:128,143-151; docs/CONFIG.md:27 — FIX — pending
