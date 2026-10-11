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
