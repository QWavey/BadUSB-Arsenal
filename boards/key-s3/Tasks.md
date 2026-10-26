# Tasks — v4.17

1. [x] DuckyScript preprocessor: EXTENSION/END_EXTENSION + REM_BLOCK/END_REM
2. [x] DuckyScript preprocessor: DEFINE + #NAME substitution + IF_DEFINED_TRUE/ELSE_DEFINED/END_IF_DEFINED
3. [x] DuckyScript parser: FUNCTION NAME(), ELSE IF, trailing THEN
4. [x] Special $_ variables
5. [x] CAPSLOCK/NUMLOCK/SCROLLLOCK keystroke verbs + track state
6. [x] SAVE_HOST_KEYBOARD_LOCK_STATE / RESTORE_HOST_KEYBOARD_LOCK_STATE
7. [ ] HID LED output-report hook to sync lock-key state (deferred - core exposure uncertain)
8. [x] LED blinks during blocking DELAY (rewrite delay loop)
9. [x] Settings toggle "Blink LED while executing payloads" + NVS blink_on_run
10. [x] Live typing Backspace on mobile virtual keyboard
11. [x] Editor error/warning overlay overflow on mobile (CSS)
12. [x] Extensions folder + endpoints
13. [x] Extensions pull-from-URL endpoint (WiFi required)
14. [x] Extensions web tab
15. [x] RUN_EXTENSION <name> DuckyScript command
16. [x] File Explorer: tap file -> preview panel; long-press -> context menu
17. [x] Bug-hunt agent 10 rounds, apply safe findings (8 of 10 fixed; #4 partial, #7 done)
18. [x] Bump firmware stamp (417), cache-bust (v=4.17b), compile, flash, push SD

---

For 1. You should add a preprocessor pass in DuckyInterpreter.cpp that strips `EXTENSION <name>` and `END_EXTENSION` framing lines, and drops everything between `REM_BLOCK` and `END_REM` (multi-line comments). Run this pass BEFORE the executor sees any lines. Done when: a script starting with `EXTENSION FOO\nREM_BLOCK\nany text\nEND_REM\nSTRING hi\nEND_EXTENSION` executes and types "hi" without errors.

For 2. You should collect `DEFINE #NAME value` into a std::map inside a preprocessor pass, then substitute every occurrence of `#NAME` in later lines with `value`. Also implement `IF_DEFINED_TRUE #NAME` / `ELSE_DEFINED` / `END_IF_DEFINED` block skipping. Truthy = non-empty and not equal to "FALSE"/"0". Done when: `DEFINE #X TRUE\nIF_DEFINED_TRUE #X\nSTRING hi\nEND_IF_DEFINED` types "hi".

For 3. You should accept `FUNCTION NAME()` (parens allowed after name) as identical to `FUNCTION NAME`, `ELSE IF <cond>` as identical to `ELIF <cond>`, and strip a trailing `THEN` from `IF ... THEN` / `ELSE IF ... THEN` lines. Done when: `FUNCTION FOO()\nSTRING hi\nEND_FUNCTION\nFOO()` types "hi" and `IF ($_OS == LINUX) THEN ... END_IF` parses.

For 4. You should register the six special variables in the interpreter's variable map at execution start. Default $_OS="", the lock booleans default to FALSE, HOST_CONFIGURATION_REQUEST_COUNT=0, RECEIVED_HOST_LOCK_LED_REPLY=FALSE. Read via `$_NAME` (existing $ prefix). Writable from script (for DETECT_OS assignment) but usually written by firmware hooks. Done when: a script `IF ($_OS == LINUX) ... END_IF` compiles and correctly branches based on the current $_OS value.

For 5. You should implement `CAPSLOCK`, `NUMLOCK`, `SCROLLLOCK` as DuckyScript verbs that send the corresponding HID keypress. Update the software-mirror lock flags (`$_CAPSLOCK_ON` etc.) locally on press so scripts have SOMETHING even if the host LED report hook isn't available. Done when: `CAPSLOCK\nCAPSLOCK` toggles the mirror twice.

For 6. You should implement `SAVE_HOST_KEYBOARD_LOCK_STATE` and `RESTORE_HOST_KEYBOARD_LOCK_STATE` as: SAVE snapshots the three lock booleans into hidden vars; RESTORE compares snapshots to current and presses each lock key that changed to bring them back in sync. Done when: `SAVE... CAPSLOCK ... RESTORE...` returns caps to its snapshot state.

For 7. You should hook TinyUSB's HID SET_REPORT OUT callback (`tud_hid_set_report_cb`) so when the host tells us caps/num/scroll LED changed, we update the three $_ variables. If the arduino-esp32 core doesn't expose this callback, note it as a limitation and leave the software mirror as the sole source of truth. Done when: pressing CapsLock on the target host is reflected in `$_CAPSLOCK_ON` on the next check (or documented as manual-only if the callback isn't accessible).

For 8. You should rewrite the `DELAY` command's inner wait so it calls `handleLED()`, `server.handleClient()`, `comShellLoop()` every ~20 ms instead of a straight `delay(N)`. That way an LED_BLINK started before the DELAY continues blinking through it. Done when: `LED_BLINK\nDELAY 2000\nLED_OFF` visibly blinks the LED throughout the 2 s window instead of only after.

For 9. You should add a "Blink LED while executing payloads" toggle to the Settings > Toggles group, persist as `blink_on_run` NVS, expose in /api/stats, and add /api/toggle-blink-on-run POST. In executeScript(), if the flag is on, call setLEDMode(1) at start and setLED(0,0,255) at end. Done when: enabling the toggle and running any script blinks the LED during the run.

For 10. You should add a `beforeinput` handler on the Live textarea in parallel with the `input` handler, and check the `inputType` for delete variants (`deleteContentBackward`, `deleteContentForward`, `deleteByCut`). Loosen the 30 ms debounce so it only skips when the last keydown was a printable char, not a delete. Done when: on Android GBoard, pressing Backspace in the Live text box actually sends a BACKSPACE HID event to the host.

For 11. You should audit style.css / inline styles for `.error-line` / `.inline-error` / `.warning-line` / `.inline-warning`. Add `max-width: 100%; overflow: hidden; text-overflow: ellipsis` where needed. On viewports < 600 px, shrink font-size to 11 px. Done when: on a 400-px-wide viewport, error labels no longer extend past the code line's right edge and no longer overlap the code itself.

For 12. You should add four HTTP endpoints in WebServerManager.cpp that operate over `/extensions/`: `/api/list-extensions` (returns JSON array of filenames), `/api/load-extension?name=X` (returns text), `/api/save-extension` (POST JSON with name+content), `/api/delete-extension?name=X` (DELETE, reject `..` traversal). Ensure `/extensions/` is created at boot if missing. Done when: all four endpoints work via curl and Chrome DevTools.

For 13. You should add a `/api/pull-extension` POST endpoint that takes `{url, saveAs}`, requires WiFi STA connected (else 503), calls the existing `downloadFileFromURL` helper into `/extensions/<saveAs>`, then returns the saved size. Done when: with the ESP joined to a real WiFi, POSTing a raw github.com/hak5 extension URL saves it to /extensions/.

For 14. You should add a new nav button "Extensions" to index.html between Boot and Settings, and a new section `<section id="tab-Extensions">` containing: a list of extensions from /api/list-extensions, click-to-load into a textarea editor, Save/Delete/Upload buttons, and a "Pull from URL" input + button. Wire up JS functions refreshExtensions/loadExtension/saveExtension/deleteExtension/pullExtension. Done when: the tab opens on a fresh boot, lists extensions on the SD, and Save/Load/Delete work end-to-end.

For 15. You should add a DuckyScript verb `RUN_EXTENSION <name>` that resolves to `loadScript("/extensions/<name>.txt")` and executes inline. Also accept `.dsx` suffix. Done when: with `/extensions/foo.txt` on the SD containing `STRING hi`, a script `RUN_EXTENSION foo` types "hi".

For 16. You should upgrade the File Explorer JS: single-tap a file opens a preview panel below the list with the first 64 KB of contents in a monospace pre. Long-press (500 ms touchstart) or right-click opens a context menu with Preview / Rename / Duplicate / Delete / Download / Copy path. Done when: on mobile, tapping a file shows a preview and long-pressing shows the menu; on desktop, right-clicking a file shows the menu.

For 17. You should spawn a fresh 10-round bug-hunt Explore agent focused on the NEW v4.17 code (preprocessor passes, HID LED report hook, host-lock-state save/restore, extension endpoints, RUN_EXTENSION, File Explorer preview + context menu, LED-in-DELAY loop). Return max 10 findings; apply CRITICAL and HIGH. Done when: agent output archived and its CRITICAL/HIGH items are fixed in the code.

For 18. You should bump FIRMWARE_STAMP to 417, bump index.html cache-bust to v=4.17, build+flash the .espkg via tools/build_espkg.py --force-compile --flash-after-done, then push updated web files to D:\. Done when: /api/stats returns fwVersion "4.17" and the SD listing shows the new files.
