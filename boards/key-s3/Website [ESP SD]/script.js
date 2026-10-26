// Global state
let currentBrowserPath = '/', selectedFile = null, currentStats = null;
let selectedDesign = null, progressInterval;
let bootScriptsSelected = [];
let scriptChanged = false;
let lastErrorState = ""; // Track state to avoid redundant Issue tab re-renders

// Trigger browser warning for unsaved changes
window.addEventListener('beforeunload', (e) => {
    if (scriptChanged) {
        e.preventDefault();
        e.returnValue = ''; // Required for Chrome/Firefox
        return "Unsaved changes will be lost!";
    }
});

// UI Initialization
document.addEventListener('DOMContentLoaded', () => {
    const scriptArea = document.getElementById('scriptArea');
    const highlights = document.getElementById('editorHighlights');
    const gutter = document.getElementById('editorGutter');

    if (scriptArea) {
        const syncScroll = () => {
            highlights.scrollTop = scriptArea.scrollTop;
            highlights.scrollLeft = scriptArea.scrollLeft;
            gutter.scrollTop = scriptArea.scrollTop;
        };

        let debounceTimer;
        scriptArea.addEventListener('input', () => {
            updateGutter();
            scriptChanged = true;
            syncScroll();
            
            fastUpdateHighlights();
            
            clearTimeout(debounceTimer);
            debounceTimer = setTimeout(() => {
                updateErrorLens();
            }, 500); 
        });
        
        scriptArea.addEventListener('scroll', syncScroll);
        
        scriptArea.addEventListener('paste', () => {
            setTimeout(() => {
                updateGutter();
                updateErrorLens();
                syncScroll();
            }, 0);
        });

        scriptArea.addEventListener('keydown', (e) => {
            if (e.key === 'Backspace') {
                const start = scriptArea.selectionStart;
                const end = scriptArea.selectionEnd;
                if (start === end && start > 0) {
                    const text = scriptArea.value;
                    const before = text.substring(0, start);
                    const lineStart = before.lastIndexOf('\n') + 1;
                    const lineText = before.substring(lineStart);
                    if (lineText.length > 0 && !/[^\s\t]/.test(lineText)) {
                        e.preventDefault();
                        scriptArea.value = text.substring(0, lineStart) + text.substring(end);
                        scriptArea.selectionStart = scriptArea.selectionEnd = lineStart;
                        updateGutter();
                        updateErrorLens();
                        return;
                    }
                }
            }
            if (e.key === 'Tab') {
                if (typeof autocompletePopup !== 'undefined' && autocompletePopup && autocompletePopup.style.display === 'block') return;
                e.preventDefault();
                const start = scriptArea.selectionStart;
                const end = scriptArea.selectionEnd;
                scriptArea.value = scriptArea.value.substring(0, start) + "\t" + scriptArea.value.substring(end);
                scriptArea.selectionStart = scriptArea.selectionEnd = start + 1;
                updateErrorLens();
                syncScroll();
            } else if (e.key === 'Enter') {
                if (typeof autocompletePopup !== 'undefined' && autocompletePopup && autocompletePopup.style.display === 'block') return;
                e.preventDefault();
                const start = scriptArea.selectionStart;
                const value = scriptArea.value;
                const beforeCursor = value.substring(0, start);
                const lineStart = beforeCursor.lastIndexOf('\n') + 1;
                const currentLine = beforeCursor.substring(lineStart);
                const indentMatch = currentLine.match(/^(\s*)/);
                let indent = indentMatch ? indentMatch[1] : '';
                const trimmedLine = currentLine.trim().toUpperCase();

                // Auto-indent next line if current line starts a block
                const isFunctionBlockStart = trimmedLine.startsWith('FUNCTION') || trimmedLine.startsWith('DEF_');
                const isControlBlockStart = trimmedLine.startsWith('IF ') || trimmedLine.startsWith('IF:') || 
                                           trimmedLine.startsWith('FOR ') || trimmedLine.startsWith('FOR:') || 
                                           trimmedLine.startsWith('WHILE ') || trimmedLine.startsWith('WHILE:') || 
                                           trimmedLine.startsWith('REPEAT ') || trimmedLine.startsWith('REPEAT:') ||
                                           trimmedLine === 'ELSE' || trimmedLine === 'ELSE:' ||
                                           trimmedLine.startsWith('ELIF ') || trimmedLine.startsWith('ELIF:');
                const isFunctionLabelStart = trimmedLine.includes('FUNCTION_') && trimmedLine.endsWith('():');
                const isColonFunctionStart = trimmedLine.endsWith(':') && (trimmedLine.startsWith('FUNCTION') || trimmedLine.includes('():'));

                if (isFunctionBlockStart || isControlBlockStart || isFunctionLabelStart || isColonFunctionStart) {
                    indent += '\t';
                }

                // Automatic de-indent for closing blocks ON ENTER
                if (trimmedLine.startsWith('ENDIF') || trimmedLine.startsWith('END_IF') || trimmedLine.startsWith('ENDFOR') || trimmedLine.startsWith('END_FOR') || 
                    trimmedLine.startsWith('END_FUNCTION') || trimmedLine.startsWith('END_DEF') || 
                    trimmedLine.startsWith('END_WHILE') || trimmedLine.startsWith('END_REPEAT') ||
                    trimmedLine.startsWith('END_RUN_ON_REBOOT')) {
                    const currentIndent = (currentLine.match(/^\s*/) || [""])[0];
                    if (currentIndent.length > 0) {
                        const newIndent = currentIndent.substring(0, currentIndent.length - 1);
                        const newLineText = newIndent + currentLine.trim();
                        scriptArea.value = value.substring(0, lineStart) + newLineText + value.substring(start);
                        const newPos = lineStart + newLineText.length;
                        scriptArea.setSelectionRange(newPos, newPos);
                        indent = newIndent; // New line will follow this de-indent
                    }
                }

                // v4.31: auto-insert the matching closer when Enter is
                // pressed at the end of a block-OPENER line and the closer
                // isn't already present downstream at the same/lesser
                // indent. Applies to:
                //   EXTENSION NAME    -> END_EXTENSION
                //   FUNCTION NAME()   -> END_FUNCTION      (also DEF_)
                //   IF ...            -> END_IF
                //   FOR ...           -> END_FOR
                //   WHILE ...         -> END_WHILE
                //   REPEAT ...        -> END_REPEAT
                //   REM_BLOCK         -> END_REM
                //   STRING_BASH       -> END_STRING
                //   STRING_POWERSHELL -> END_STRING
                //   BUTTON_DEF ...    -> END_BUTTON
                //   RUN_ON_REBOOT     -> END_RUN_ON_REBOOT
                //   IF_DEFINED_TRUE   -> END_IF_DEFINED
                //   IF_NOT_DEFINED_TRUE -> END_IF_DEFINED
                // The closer is placed on a NEW line after the caret's new
                // indented position, and the caret is left ON the indented
                // body line (between opener and closer) so the user can
                // start typing the body immediately.
                let autoCloseSuffix = '';
                // v4.35: EXTENSION is normally EMPTY-body ("pull in FUNCTION
                // defs from SD"), not a scaffold you type code into. So its
                // auto-close should collapse the block to two lines
                //   EXTENSION name
                //   END_EXTENSION
                // and leave the caret on line 3 (after the closer), not on
                // an indented body line between opener and closer.
                // Every OTHER opener keeps the "caret in the body" behavior
                // (FUNCTION/IF/FOR/WHILE/REPEAT/BUTTON_DEF/... all expect a
                // body).
                let closerLayout = 'body';   // 'body' = caret between; 'trailing' = caret AFTER closer
                {
                    const opener = trimmedLine;
                    const baseIndent = (currentLine.match(/^\s*/) || [""])[0];
                    let closer = null;
                    if (opener === 'EXTENSION' || opener.startsWith('EXTENSION ')) {
                        // Only auto-close when there's a NAME after EXTENSION,
                        // and NOT when the line ends with the collapsed ˅
                        // marker (a collapsed ref doesn't need a closer).
                        const rest = opener.substring('EXTENSION'.length).trim();
                        if (rest && !rest.endsWith('˅')) { closer = 'END_EXTENSION'; closerLayout = 'trailing'; }
                    }
                    else if (opener.startsWith('FUNCTION ') || opener.startsWith('DEF_')) closer = 'END_FUNCTION';
                    else if (opener === 'IF' || opener.startsWith('IF ') || opener.startsWith('IF(')) closer = 'END_IF';
                    else if (opener === 'FOR' || opener.startsWith('FOR ') || opener.startsWith('FOR(')) closer = 'END_FOR';
                    else if (opener === 'WHILE' || opener.startsWith('WHILE ') || opener.startsWith('WHILE(')) closer = 'END_WHILE';
                    else if (opener === 'REPEAT' || opener.startsWith('REPEAT ')) closer = 'END_REPEAT';
                    else if (opener === 'REM_BLOCK' || opener.startsWith('REM_BLOCK ')) closer = 'END_REM';
                    else if (opener === 'STRING_BASH' || opener.startsWith('STRING_BASH ')) closer = 'END_STRING';
                    else if (opener === 'STRING_POWERSHELL' || opener.startsWith('STRING_POWERSHELL ')) closer = 'END_STRING';
                    else if (opener === 'BUTTON_DEF' || opener.startsWith('BUTTON_DEF ')) closer = 'END_BUTTON';
                    else if (opener === 'RUN_ON_REBOOT' || opener.startsWith('RUN_ON_REBOOT ')) closer = 'END_RUN_ON_REBOOT';
                    else if (opener.startsWith('IF_DEFINED_TRUE') || opener.startsWith('IF_NOT_DEFINED_TRUE')) closer = 'END_IF_DEFINED';

                    if (closer) {
                        // Look ahead: is there already a matching closer at
                        // baseIndent or shallower on a following line? If so,
                        // don't double-insert.
                        const after = value.substring(start);
                        const closerRE = new RegExp('^' + baseIndent.replace(/\t/g, '\\t') + '\\s*' + closer + '\\b', 'm');
                        if (!closerRE.test(after)) {
                            // 'trailing': opener\nCLOSER\n<caret>
                            // 'body':     opener\n<indent><caret>\n<baseIndent>CLOSER
                            if (closerLayout === 'trailing') {
                                autoCloseSuffix = baseIndent + closer + '\n';
                            } else {
                                autoCloseSuffix = '\n' + baseIndent + closer;
                            }
                        } else {
                            closerLayout = 'body';   // don't reposition caret if we didn't insert one
                        }
                    } else {
                        closerLayout = 'body';
                    }
                }

                let insertion, caretAt;
                const newStart = scriptArea.selectionStart;
                if (closerLayout === 'trailing') {
                    // Layout: `<opener>\n<CLOSER>\n<caret>`
                    insertion = "\n" + autoCloseSuffix;
                    caretAt   = newStart + insertion.length;
                } else if (autoCloseSuffix) {
                    // Layout: `<opener>\n<indent><caret>\n<baseIndent><CLOSER>`
                    insertion = "\n" + indent + autoCloseSuffix;
                    caretAt   = newStart + 1 + indent.length;
                } else {
                    // No closer to insert - regular Enter.
                    insertion = "\n" + indent;
                    caretAt   = newStart + insertion.length;
                }
                scriptArea.value = scriptArea.value.substring(0, newStart) + insertion + scriptArea.value.substring(scriptArea.selectionEnd);
                scriptArea.selectionStart = scriptArea.selectionEnd = caretAt;
                updateGutter();
                updateErrorLens();
                syncScroll();
            }
        });

        const editorMain = document.querySelector('.editor-main');
        const syncHover = (e) => {
            const rect = scriptArea.getBoundingClientRect();
            const y = e.clientY - rect.top + scriptArea.scrollTop;
            const lineIdx = Math.floor((y - 15) / 22);
            
            const highlightLines = highlights.children;
            for (let i = 0; i < highlightLines.length; i++) {
                if (i === lineIdx) {
                    highlightLines[i].classList.add('is-hovered');
                } else {
                    highlightLines[i].classList.remove('is-hovered');
                }
            }
        };

        if (editorMain) {
            editorMain.addEventListener('mousemove', syncHover);
            editorMain.addEventListener('mouseleave', () => {
                Array.from(highlights.children).forEach(el => el.classList.remove('is-hovered'));
            });
        }

        updateGutter();
        updateErrorLens();
        syncScroll();
    }
    
    updateStats();
    initFileManager();
    initMacosDock(document.querySelector('.tab'));
    
    // Pre-populate state
    updateStats();
    initMacosDock(document.querySelector('.control-panel'));
    
    document.getElementById('autoRetryToggle').checked = localStorage.getItem('autoRetryConn') !== 'false';
    
    setInterval(updateStats, 5000);
    setInterval(pollSystemStatus, 3000);
    setInterval(refreshTasks, 10000);

    // Custom Scrollbar Init
    setTimeout(initAllCustomScrollbars, 600);
    setTimeout(initAutocomplete, 500);
    
    // Initialize Resizable Editor
    initResizableEditor();
});

function initResizableEditor() {
    const editor = document.querySelector('.editor-wrapper');
    if (!editor) return;

    const handle = document.createElement('div');
    handle.className = 'editor-resizer';
    editor.appendChild(handle);

    let isResizing = false;
    let startX, startY, startWidth, startHeight;

    handle.addEventListener('mousedown', (e) => {
        isResizing = true;
        startX = e.clientX;
        startY = e.clientY;
        startWidth = editor.offsetWidth;
        startHeight = editor.offsetHeight;
        document.body.style.userSelect = 'none';
        document.body.style.cursor = 'all-scroll';
        handle.classList.add('active');
    });

    document.addEventListener('mousemove', (e) => {
        if (!isResizing) return;
        const deltaX = e.clientX - startX;
        const deltaY = e.clientY - startY;
        
        const newWidth = startWidth + deltaX;
        const newHeight = startHeight + deltaY;
        
        if (newHeight >= 150 && newHeight <= 1200) {
            editor.style.height = `${newHeight}px`;
        }
        if (newWidth >= 400 && newWidth <= 1600) {
            editor.style.width = `${newWidth}px`;
        }
        
        // Trigger scroll sync and updates
        if (typeof updateGutter === 'function') updateGutter();
        if (typeof syncScroll === 'function') {
            const scriptArea = document.getElementById('scriptArea');
            if (scriptArea) syncScroll.call(scriptArea);
        }
    });

    document.addEventListener('mouseup', () => {
        if (isResizing) {
            isResizing = false;
            document.body.style.userSelect = '';
            document.body.style.cursor = '';
            handle.classList.remove('active');
        }
    });
}

// =============================================
// System Status Polling
// =============================================
function pollSystemStatus() {
    fetch('/api/stats')
        .then(r => r.json())
        .then(data => {
            const statusEl = document.getElementById('scriptStatus');
            if (statusEl) statusEl.textContent = data.scriptRunning ? 'Running...' : 'Idle';
            const progBar = document.getElementById('progressBar');
            const progFill = document.getElementById('progressFill');
            if (progBar && progFill) {
                if (data.delayProgress > 0) {
                    progBar.style.display = 'block';
                    const secs = (data.delayTotal / 1000).toFixed(1);
                    progFill.style.width = data.delayProgress + '%';
                    progFill.title = `Delay: ${secs}s`;
                } else {
                    progBar.style.display = 'none';
                    progFill.style.width = '0%';
                }
            }
        }).catch(() => {});
}

function toggleIssuesList() {
    const panel = document.getElementById('issuesPanel');
    if (panel) {
        panel.classList.toggle('expanded');
    }
}

function toggleAdvancedErrorOptions() {
    const el = document.getElementById('advancedErrorOptions');
    const arrow = document.getElementById('advErrorArrow');
    if (!el) return;
    const isVisible = el.style.display === 'block';
    el.style.display = isVisible ? 'none' : 'block';
    if (arrow) arrow.style.transform = isVisible ? 'rotate(0deg)' : 'rotate(180deg)';
}

function toggleAutoRetry() {
    const toggle = document.getElementById('autoRetryToggle');
    localStorage.setItem('autoRetryConn', toggle.checked);
}

let statsController = null;
let statusController = null;
let cachedLanguageList = [];
let languagesDiscovered = false;
let globalDeclaredVars = new Set();
let globalDeclaredFunctions = new Set();
let ignoredWarnings = new Set();

function toggleExample(el) {
    el.classList.toggle('active');
}

function ignoreWarning(line, varName, btn) {
    const lineEl = btn ? btn.closest('.warning-line') : null;
    if (lineEl) {
        // Only fade warning elements, NOT the code
        lineEl.style.transition = 'background 0.6s cubic-bezier(0.4, 0, 0.2, 1)';
        lineEl.style.background = 'transparent';
        
        const message = lineEl.querySelector('.inline-warning');
        const button = lineEl.querySelector('.ignore-btn-inline');
        const code = lineEl.querySelector('.warning-text') || lineEl.querySelector('.line-code');
        
        if (message) {
            message.style.transition = 'all 0.6s ease';
            message.style.opacity = '0';
            message.style.filter = 'blur(10px)';
        }
        if (button) {
            button.style.transition = 'all 0.6s ease';
            button.style.opacity = '0';
            button.style.transform = 'translateY(-50%) scale(0.8)';
        }
        if (code) {
            code.style.transition = 'all 0.6s ease';
            code.style.filter = 'none';
            code.style.opacity = '1';
        }
        
        setTimeout(() => {
            ignoredWarnings.add(`${line}-${varName}`);
            updateErrorLens();
        }, 600);
    } else {
        ignoredWarnings.add(`${line}-${varName}`);
        updateErrorLens();
    }
}

function initMacosDock(container) {
    if (!container) return;
    // Disable JS-based animation on mobile/touch to save resources and avoid "apple bar effect"
    if (window.matchMedia("(max-width: 767px)").matches || ('ontouchstart' in window)) return;
    
    const items = Array.from(container.querySelectorAll('.tablinks'));
    const state = items.map(item => ({ el: item, currentScale: 1, targetScale: 1 }));
    
    let lastMouseX = 0, lastMouseY = 0, lastTime = 0;
    let velocity = 0, isMouseIn = false, mouseX = 0;

    container.addEventListener('mousemove', (e) => {
        const now = performance.now();
        const dt = now - lastTime;
        if (dt > 0) {
            const dx = e.clientX - lastMouseX;
            const dy = e.clientY - lastMouseY;
            velocity = velocity * 0.8 + (Math.sqrt(dx*dx + dy*dy) / dt) * 0.2;
        }
        lastMouseX = e.clientX; lastMouseY = e.clientY; lastTime = now;
        mouseX = e.clientX;
        isMouseIn = true;
    });

    container.addEventListener('mouseleave', () => {
        isMouseIn = false;
        velocity = 0;
    });

    function animate() {
        const vFactor = Math.max(0, Math.min(1, 1 - velocity / 3.0));
        
        state.forEach(s => {
            if (isMouseIn) {
                const rect = s.el.getBoundingClientRect();
                const centerX = rect.left + rect.width / 2;
                const dist = Math.abs(mouseX - centerX);
                const maxDist = 130;
                
                if (dist < maxDist) {
                    const intensity = (1 - dist / maxDist) * vFactor;
                    s.targetScale = 1 + (0.3 * intensity);
                } else {
                    s.targetScale = 1;
                }
            } else {
                s.targetScale = 1;
            }

            // Lerp for ultra-smoothness
            s.currentScale += (s.targetScale - s.currentScale) * 0.15;
            s.el.style.transform = `scale(${s.currentScale})`;
            s.el.style.zIndex = s.currentScale > 1.05 ? "10" : "1";
        });
        
        requestAnimationFrame(animate);
    }
    animate();
}

function updateGutter() {
    const scriptArea = document.getElementById('scriptArea');
    const gutter = document.getElementById('editorGutter');
    if (!scriptArea || !gutter) return;
    // v4.32: render a click-to-fold arrow (▾ or ▸) next to any EXTENSION
    // line so users don't have to move the caret + hit the toolbar button.
    //   `EXTENSION NAME`             (empty body / auto-inline) -> ▾  collapses to ˅
    //   `EXTENSION NAME ^`           (inline-expanded opener)   -> ▾  collapses to ˅
    //   `EXTENSION NAME ˅`           (collapsed reference)      -> ▸  expands inline
    const src = scriptArea.value.split('\n');
    let html = '';
    for (let i = 0; i < src.length; i++) {
        const t = src[i].trim();
        let arrow = '';
        if (/^EXTENSION\s+[A-Za-z0-9_.\-]+/i.test(t)) {
            const isCollapsed = /˅\s*$/.test(t);
            arrow = `<span class="gutter-fold" onclick="_gutterToggleFold(${i})" title="${isCollapsed ? 'Expand this extension' : 'Collapse this extension block'}">${isCollapsed ? '▸' : '▾'}</span>`;
        }
        html += `<div>${i + 1}${arrow}</div>`;
    }
    gutter.innerHTML = html;
}

// v4.32: click-to-fold from a gutter arrow. Puts the caret on the given
// line index then reuses the existing toggleExtensionAtCursor.
function _gutterToggleFold(lineIdx) {
    const sa = document.getElementById('scriptArea');
    if (!sa) return;
    const lines = sa.value.split('\n');
    let pos = 0;
    for (let k = 0; k < lineIdx && k < lines.length; k++) pos += lines[k].length + 1;
    sa.focus();
    sa.selectionStart = sa.selectionEnd = pos;
    if (typeof toggleExtensionAtCursor === 'function') toggleExtensionAtCursor();
}

function pollSystemStatus() {
    if (statusController) statusController.abort();
    statusController = new AbortController();

    fetch('/status', { signal: statusController.signal }).then(r => r.text()).then(data => {
        const statusEl = document.getElementById('scriptStatus');
        if (statusEl && !statusEl.classList.contains('status-error')) {
            statusEl.textContent = data.replace(/[\uD800-\uDBFF][\uDC00-\uDFFF]|\u200D|\uFE0F/g, '');
        }
    }).catch(err => {
        if (err.name !== 'AbortError') console.error("Status Poll Error:", err);
    });
}

// =============================================
// Tab Navigation (single source of truth)
// =============================================
function openTab(evt, tabName) {
    const tablinks = document.getElementsByClassName('tablinks');
    for (let i = 0; i < tablinks.length; i++) tablinks[i].classList.remove('active');

    if (evt && evt.currentTarget) evt.currentTarget.classList.add('active');
    else Array.from(tablinks).forEach(btn => { if (btn.textContent.trim() === displayNameForTab(tabName)) btn.classList.add('active'); });

    const tabcontents = document.getElementsByClassName('tabcontent');
    for (let i = 0; i < tabcontents.length; i++) tabcontents[i].style.display = 'none';

    const target = document.getElementById('tab-' + tabName);
    if (target) target.style.display = 'block';

    // v4.15: persist the current tab so a reload comes back to it (instead
    // of always jumping to Coding).
    try { localStorage.setItem('espLastTab', tabName); } catch (e) {}

    // Lazy-load per-tab data. Wrapped so a failing refresh can never
    // leave the UI wedged on the current tab (the switch already happened above).
    try {
        if (tabName === 'Scripts') refreshFiles();
        else if (tabName === 'Boot') refreshBootScripts();
        else if (tabName === 'Statistics') updateStats();
        else if (tabName === 'File_Manager') refreshFileBrowser();
        else if (tabName === 'Design') refreshDesigns();
        else if (tabName === 'Settings') initSettingsTab();
        else if (tabName === 'Live') { const li = document.getElementById('liveInput'); if (li) setTimeout(() => li.focus(), 50); }
    } catch (e) {
        console.error('Tab data load failed for', tabName, e);
    }
}

// v4.15: restore last-open tab on page load. Runs after DOMContentLoaded
// so all tab handlers are wired. Falls back silently if the stored value
// no longer maps to a real tab (e.g. Design tab was renamed).
document.addEventListener('DOMContentLoaded', () => {
    try {
        const last = localStorage.getItem('espLastTab');
        if (!last || last === 'Script') return;   // 'Script' is the default already
        if (document.getElementById('tab-' + last)) {
            setTimeout(() => openTab(null, last), 50);
        }
    } catch (e) {}
});

// ---------------------------------------------------------------
// v4.15: "Keep unsaved draft in browser"
// ---------------------------------------------------------------
// When the toggle is ON (default), the Coding tab's scriptArea contents are
// persisted to localStorage every second and restored on next page open.
// Unlike server-side SAVE, this survives without touching the SD card - so
// you can close the tab, come back, and your work is still there.
// Toggle OFF and the stored draft is dropped immediately.
const DRAFT_KEY = 'espEditorDraft';
const DRAFT_SETTING_KEY = 'espKeepDraft';
let __draftSaveTimer = null;

function __draftEnabled() {
    try {
        const stored = localStorage.getItem(DRAFT_SETTING_KEY);
        // Default ON if never set.
        return stored === null ? true : (stored === '1');
    } catch (e) { return true; }
}

function __persistDraftSettingChanged() {
    const el = document.getElementById('settingKeepDraft');
    if (!el) return;
    try {
        localStorage.setItem(DRAFT_SETTING_KEY, el.checked ? '1' : '0');
        if (!el.checked) localStorage.removeItem(DRAFT_KEY);
    } catch (e) {}
}

function __scheduleDraftSave() {
    if (!__draftEnabled()) return;
    if (__draftSaveTimer) return;
    __draftSaveTimer = setTimeout(() => {
        __draftSaveTimer = null;
        const sa = document.getElementById('scriptArea');
        if (!sa) return;
        try {
            localStorage.setItem(DRAFT_KEY, sa.value || '');
        } catch (e) {
            // v4.16 FIX: don't silently swallow QuotaExceededError. Disable
            // the toggle and tell the user - previously users could think
            // their draft was safe when the ~5 MB quota had been blown by a
            // huge paste and nothing was actually being saved.
            const t = document.getElementById('settingKeepDraft');
            if (t) t.checked = false;
            try { localStorage.setItem(DRAFT_SETTING_KEY, '0'); } catch (_) {}
            if (!window.__draftQuotaShown) {
                window.__draftQuotaShown = true;
                alert('Editor auto-save DISABLED: browser storage quota exceeded (~5 MB). ' +
                      'Your last save may be truncated. Save to SD via the Save button to keep this work.');
            }
        }
    }, 1000);
}

document.addEventListener('DOMContentLoaded', () => {
    // Restore draft on load, if any.
    const sa = document.getElementById('scriptArea');
    const toggle = document.getElementById('settingKeepDraft');
    if (toggle) toggle.checked = __draftEnabled();
    if (sa && __draftEnabled()) {
        try {
            const d = localStorage.getItem(DRAFT_KEY);
            if (d && !sa.value) {
                sa.value = d;
                sa.dispatchEvent(new Event('input', { bubbles: true }));
            }
        } catch (e) {}
        // Debounced save on every input.
        sa.addEventListener('input', __scheduleDraftSave);
    }
    // Clear button should also nuke the persisted draft.
    const clearBtn = document.querySelector('.control-panel button[onclick*="clearScript"]');
    if (clearBtn) {
        const origHandler = clearBtn.onclick;
        clearBtn.addEventListener('click', () => {
            try { localStorage.removeItem(DRAFT_KEY); } catch (e) {}
        });
    }
});

// Maps an internal tab id to its visible button label (they differ for a few tabs).
function displayNameForTab(tabName) {
    const map = { 'Script': 'Coding', 'File_Manager': 'Explorer', 'Statistics': 'Stats' };
    return map[tabName] || tabName;
}

function applyHighlighting(line) {
    if (!line.trim()) return '&nbsp;';
    
    // 1. Handle comments immediately
    if (line.trim().startsWith('REM') || line.trim().startsWith('//')) {
        const escapedComment = line.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
        return `<span class="syntax-comment">${escapedComment}</span>`;
    }

    const cat = {
        text: ['STRING', 'STRINGLN'],
        logic: ['IF', 'ELSE', 'ELIF', 'THEN', 'ENDIF', 'END_IF', 'REPEAT', 'END_REPEAT', 'WHILE', 'END_WHILE', 'FOR', 'FOR:', 'ENDFOR', 'END_FOR', 'WAIT_FOR_EVENT', 'RUN_AT_TIME', 'RUN_AT_DAY', 'RUN_WHEN_WIFI', 'IS_ONLINE', 'IS_OFFLINE', 'WIFI_OFF_WHEN_WIFI', 'WIFI_ON_WHEN_WIFI', 'BLUETOOTH_OFF_WHEN_WIFI', 'BLUETOOTH_ON_WHEN_WIFI', 'IF_CLIENT_CONNECTED_BLUETOOTH', 'IF_CLIENT_CONNECTED_WIFI', 'IF_CLIENT_DISCONNECTED_WIFI', 'IF_CLIENT_DISCONNECTED_BLUETOOTH', 'IF_CLIENT_CONNECTED', 'IF_CLIENT_DISCONNECTED', 'IF_CLIENT_CONNECTED_DISCONNECTED', 'IF_CLIENT_CONNECTED_DISCONNECTED_BLUETOOTH', 'IF_CLIENT_CONNECTED_DISCONNECTED_WIFI', 'RUN_ON_REBOOT', 'END_RUN_ON_REBOOT', 'BLUETOOTH_DISCOVERY', 'RUN_WHEN_BLUETOOTH_FOUND', 'RUN_WHEN_BT_FOUND', 'BT_FOUND', 'FROM', 'TO', 'STEP', 'FUNCTION', 'DEF_', 'END_FUNCTION', 'END_DEF', 'BEGIN_ROWER', 'END_ROWER', 'IF_NOT_PRESENT', 'LOCALE', 'LOCALE_DE', 'LOCALE_EN', 'LOCALE_FR', 'LOCALE_ES', 'LOCALE_IT', 'LOCALE_UK',
               // v4.31: EXTENSION framing + Hak5 3.0 preprocessor/runtime
               // block markers now colour-coded (were rendered as plain
               // white text before).
               'EXTENSION', 'END_EXTENSION', 'RUN_EXTENSION', 'IMPORT',
               'BUTTON_DEF', 'END_BUTTON', 'DISABLE_BUTTON', 'WAIT_FOR_BUTTON_PRESS',
               'REM_BLOCK', 'END_REM',
               'DEFINE', 'IF_DEFINED_TRUE', 'IF_NOT_DEFINED_TRUE',
               'ELSE_DEFINED', 'END_IF_DEFINED',
               'STRING_BASH', 'STRING_POWERSHELL', 'END_STRING', 'END_STRINGLN',
               'HIDE_PAYLOAD', 'RESTORE_PAYLOAD', 'STOP_PAYLOAD',
               'SOFT_BRICK', 'REVERT_TO_THUMBDRIVE', 'PERSIST',
               'WAIT_FOR_CAPS_ON', 'WAIT_FOR_CAPS_OFF',
               'WAIT_FOR_NUM_ON', 'WAIT_FOR_NUM_OFF',
               'WAIT_FOR_SCROLL_ON', 'WAIT_FOR_SCROLL_OFF',
               'WAIT_FOR_CAPS_CHANGE', 'WAIT_FOR_NUM_CHANGE', 'WAIT_FOR_SCROLL_CHANGE',
               'WAIT_FOR_EOF', 'INJECT_MOD',
               'SAVE_HOST_KEYBOARD_LOCK_STATE', 'RESTORE_HOST_KEYBOARD_LOCK_STATE'],
        delay: ['DELAY', 'DEFAULTDELAY', 'DEFAULT_DELAY'],
        mod: ['GUI', 'CTRL', 'ALT', 'SHIFT', 'CAPSLOCK', 'WINDOWS', 'CONTROL'],
        special: ['ENTER', 'TAB', 'ESC', 'ESCAPE', 'BACKSPACE', 'DELETE', 'DEL', 'HOME', 'END', 'PAGEUP', 'PAGEDOWN', 'F1','F2','F3','F4','F5','F6','F7','F8','F9','F10','F11','F12', 'SPACE', 'PAUSE', 'BREAK', 'INSERT', 'PRINTSCREEN', 'SCROLLLOCK', 'MENU', 'APP', 'UP', 'UPARROW', 'DOWN', 'DOWNARROW', 'LEFT', 'LEFTARROW', 'RIGHT', 'RIGHTARROW', 'NUMLOCK'],
        sys: ['REBOOT', 'SHUTDOWN', 'PING', 'HTTP_REQUEST', 'HTTPS_REQUEST', 'GET_TIME', 'GET_DAY', 'DOWNLOAD_FILE', 'UPLOAD_FILE', 'LED_R', 'LED_G', 'LED_B', 'LED_Y', 'LED_W', 'LED_O', 'LED_P', 'LED_C', 'LED_M', 'LED_A', 'LED_V', 'LED_IR', 'LED_UV', 'LED_ON', 'LED_OFF', 'LED_STOP', 'SELFDESTRUCT', 'WIFI_ON', 'WIFI_OFF', 'BLUETOOTH_ON', 'BLUETOOTH_OFF', 'JOIN_INTERNET', 'LEAVE_INTERNET', 'RANDOM_CHAR', 'RANDOM_NUMBER', 'RANDOM_SPECIAL', 'DETECT_OS', 'HOLD_TILL_STRING', 'LED_BLINK', 'BLINK_STOP', 'BLINK_LED_R', 'BLINK_LED_G', 'BLINK_LED_B', 'BLINK_LED_V', 'BLINK_LED_A', 'LED', 'RGB', 'VID_', 'PID_', 'MAN_', 'PRODUCT_', 'ATTACKMODE', 'STORAGE', 'MSC', 'STORE', 'HID', 'HID_ATTACH', 'HID_DETACH', 'SIZE_']
    };

    let html = line.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
    let tokens = [];
    const addToken = (cls, content) => {
        const id = `@@TOKEN${tokens.length}@@`;
        tokens.push(`<span class="syntax-${cls}">${content}</span>`);
        return id;
    };

    // 1. Interpolations (Shielded early)
    html = html.replace(/\${([^}]*)}/g, (m, p1) => {
        return addToken('cmd-logic', '${') + p1 + addToken('cmd-logic', '}');
    });

    // v4.33: EXTENSION / RUN_EXTENSION / IMPORT followed by a name -> colour
    // the name distinctively (was rendered plain white in the user's image).
    html = html.replace(/^(\s*)(EXTENSION|RUN_EXTENSION|IMPORT)(\s+)([A-Za-z0-9_.\-]+)/i, (m, sp, kw, sep, name) => {
        return sp + addToken('cmd-logic', kw) + sep + addToken('cmd-ext-name', name);
    });

    // v4.35: bare `NAME` or `NAME()` line at top level that resolves to an
    // in-scope extension function (via __extFunctionsByStem) OR a
    // globally-declared FUNCTION - colour distinctively so the user sees
    // that HELLO_OS after `EXTENSION hello_os.txt` is a known callable,
    // not an unknown token. Uses the same source-of-truth the linter uses.
    {
        const bareMatch = html.match(/^(\s*)([A-Za-z_][A-Za-z0-9_]*)(\(\))?\s*$/);
        if (bareMatch) {
            const name = bareMatch[2].toUpperCase();
            let known = (typeof globalDeclaredFunctions !== 'undefined' && globalDeclaredFunctions.has && globalDeclaredFunctions.has(name));
            if (!known && typeof _autocompleteFunctionsFromReferencedExtensions === 'function') {
                try {
                    const sa = document.getElementById('scriptArea');
                    const scope = _autocompleteFunctionsFromReferencedExtensions(sa ? sa.value : '');
                    if (scope && scope.has && scope.has(name)) known = true;
                } catch (_) {}
            }
            if (known) {
                html = bareMatch[1] + addToken('cmd-ext-name', bareMatch[2]) + (bareMatch[3] || '');
            }
        }
    }

    // 2. STRING / STRINGLN (Everything after is yellow, except shielded interpolations)
    const stringMatch = html.match(/^(\s*)(STRING|STRINGLN)(\s|$)(.*)/i);
    if (stringMatch) {
        const space = stringMatch[1];
        const cmd = stringMatch[2].toUpperCase();
        const sep = stringMatch[3] || "";
        let content = stringMatch[4] || "";
        if (content) {
            content = content.replace(/(\b(VAR_[a-zA-Z0-9_]*|VARIABLE_[a-zA-Z0-9_]*)\b|\$[a-zA-Z0-9_]+)/gi, (m) => {
                let v = m.toUpperCase();
                if (v.startsWith('$')) v = v.substring(1);
                
                if (globalDeclaredVars.has(v)) {
                    return addToken('var-in-string', m);
                }
                return m;
            });
        }
        html = space + addToken('cmd-text', cmd) + sep + (content ? addToken('string', content) : "");
    } else {
        // 3. Normal Strings in quotes
        html = html.replace(/"([^"]*)"/g, (m) => addToken('string', m));

        // 4. Keyword Prefixes (Integrated names like VAR_target_os)
        html = html.replace(/\b(VAR_|VARIABLE_|LOCALE_|VID_|PID_|MAN_|PRODUCT_)([a-zA-Z0-9_]*)/gi, (m, p1, p2) => {
            let catName = 'cmd-logic';
            const upperP1 = p1.toUpperCase();
            if (['VID_', 'PID_', 'MAN_', 'PRODUCT_'].includes(upperP1)) catName = 'cmd-sys';
            // Only highlight the prefix as green, keep the name (p2) white
            if (['VAR_', 'VARIABLE_'].includes(upperP1)) return addToken('cmd-logic', p1) + p2;
            return addToken(catName, p1) + p2;
        });

        // 5. Category Keywords
        Object.keys(cat).forEach(category => {
            cat[category].forEach(word => {
                let regex;
                if (word.endsWith('_')) {
                    // Prefix commands like VID_ or LOCALE_ don't need a trailing word boundary
                    regex = new RegExp(`\\b(${word})`, 'gi');
                } else if (word.endsWith(':')) {
                    // Match commands with a colon like FOR:
                    regex = new RegExp(`\\b(${word})`, 'gi');
                } else {
                    regex = new RegExp(`\\b(${word})\\b`, 'gi');
                }
                html = html.replace(regex, (m) => addToken(`cmd-${category}`, m));
            });
        });

        // 6. Assignments (Preserve exact spacing to prevent cursor drift)
        html = html.replace(/^(\s*)([a-zA-Z_][a-zA-Z0-9_]*)(\s*)=/g, (m, p1, p2, p3) => {
            const upper = p2.toUpperCase();
            if (Object.values(cat).flat().includes(upper)) return m;
            return p1 + p2 + p3 + '=';
        });

        // 7. Functions & Labels
        html = html.replace(/\b([a-zA-Z_][a-zA-Z0-9_]*)(\(\):?|\s*\()/g, (m, p1, p2) => {
            return addToken('cmd-sys', p1) + p2;
        });

        // 8. Numbers
        html = html.replace(/\b(\d+)\b/g, (m) => addToken('num', m));

        // 9. Operators
        html = html.replace(/([+\-*/%^])/g, (m) => addToken('op', m));

        // 10. Variables ($)
        html = html.replace(/(\$[a-zA-Z0-9_]+)/g, '$1');
    }

    // Final: Global Token Replacement (Regex to prevent partial matches like TOKEN1 vs TOKEN11)
    for (let i = tokens.length - 1; i >= 0; i--) {
        const tokenID = `@@TOKEN${i}@@`;
        html = html.split(tokenID).join(tokens[i]);
    }

    return html;
}


const STRUCTURAL_KEYWORDS = new Set([
    'IF', 'ELSE', 'ELIF', 'ENDIF', 'END_IF', 'FOR', 'ENDFOR', 'END_FOR', 
    'WHILE', 'END_WHILE', 'REPEAT', 'END_REPEAT', 'FUNCTION', 'DEF_', 
    'END_FUNCTION', 'END_DEF', 'RUN_ON_REBOOT', 'END_RUN_ON_REBOOT'
]);

const VALID_KEYWORDS = new Set([
    'STRING', 'STRINGLN', 'DELAY', 'GUI', 'CTRL', 'ALT', 'SHIFT', 'ENTER', 'TAB', 'ESC', 'VAR', 'VARIABLE', 'IF', 'ELSE', 'ELIF', 'ENDIF', 'END_IF', 'REPEAT', 
    'HTTP_REQUEST', 'HTTPS_REQUEST', 'GET_TIME', 'GET_DAY', 'RUN_AT_TIME', 'RUN_AT_DAY', 'RUN_WHEN_WIFI', 'WAIT_FOR_EVENT', 'REM', 'DEFAULTDELAY', 'DEFAULT_DELAY',
    'FOR', 'ENDFOR', 'END_FOR', 'MENU', 'APP', 'CAPSLOCK', 'DELETE', 'BACKSPACE', 'HOME', 'END', 'PAGEUP', 'PAGEDOWN', 'PRINTSCREEN', 'SCROLLLOCK', 
    'PAUSE', 'BREAK', 'INSERT', 'F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10', 'F11', 'F12', 'SPACE', 'REBOOT', 'SHUTDOWN', 'PING', 
    'DOWNLOAD_FILE', 'UPLOAD_FILE', 'LED_R', 'LED_G', 'LED_B', 'LED_Y', 'LED_W', 'LED_O', 'LED_P', 'LED_C', 'LED_M', 'LED_A', 'LED_V', 'LED_OFF',
    'BLINK_LED_R', 'BLINK_LED_G', 'BLINK_LED_B', 'BLINK_LED_V', 'LED_STOP', 'SELFDESTRUCT', 'WIFI_ON', 'WIFI_OFF', 'BLUETOOTH_ON', 'BLUETOOTH_OFF', 
    'JOIN_INTERNET', 'LEAVE_INTERNET', 'WIFI_OFF_WHEN_WIFI', 'WIFI_ON_WHEN_WIFI', 'BLUETOOTH_OFF_WHEN_WIFI', 'BLUETOOTH_ON_WHEN_WIFI',
    'IF_PRESENT', 'IF_NOTPRESENT', 'IF_BT_PRESENT', 'IF_ONLINE', 'IF_OFFLINE', 'IF_OS', 'IF_DETECT_OS_INCLUDES', 'IF_NOT_PRESENT',
    'IF_CLIENT_CONNECTED_BLUETOOTH', 'IF_CLIENT_CONNECTED_WIFI', 'IF_CLIENT_DISCONNECTED_WIFI', 'IF_CLIENT_DISCONNECTED_BLUETOOTH', 
    'IF_CLIENT_CONNECTED', 'IF_CLIENT_DISCONNECTED', 'IF_CLIENT_CONNECTED_DISCONNECTED', 'IF_CLIENT_CONNECTED_DISCONNECTED_BLUETOOTH', 
    // v4.31: DETECT_OS removed from VALID_KEYWORDS - it's an extension-defined
    // function (OS_DETECT extension), not a firmware built-in. Autocomplete and
    // the linter now surface it ONLY when the current script references the
    // extension that defines it (via __extFunctionsByStem). Same reasoning
    // applies to HELLO_OS, TRANSLATE_*, RUN_LINUX/WINDOWS_EXFIL, etc. - none
    // are firmware built-ins, all live in extensions.
    'IF_CLIENT_CONNECTED_DISCONNECTED_WIFI', 'WAIT_FOR_SD', 'HOLD', 'KEYCODE', 'HOLD_TILL_STRING', 'CD', 'SET_BUTTON_PIN', 'RUN_PAYLOAD',
    'FUNCTION', 'DEF_', 'RANDOM_CHAR', 'RANDOM_NUMBER', 'RANDOM_SPECIAL', 'END_FUNCTION', 'END_DEF', 'BEGIN_ROWER', 'END_ROWER',
    'UPARROW', 'DOWNARROW', 'LEFTARROW', 'RIGHTARROW', 'UP', 'DOWN', 'LEFT', 'RIGHT', 'ESCAPE', 'DEL', 'WINDOWS', 'CONTROL', 'NUMLOCK', 'FROM', 'TO', 'STEP', 'LOCALE',
    'VID_', 'PID_', 'MAN_', 'PRODUCT_', 'LED_BLINK', 'BLINK_STOP', 'RUN_ON_REBOOT', 'END_RUN_ON_REBOOT', 'BLUETOOTH_DISCOVERY',
    'RUN_WHEN_BLUETOOTH_FOUND', 'RUN_WHEN_BT_FOUND', 'BT_FOUND',
    'RANDOM_VID', 'RANDOM_PID', 'RANDOM_MAN', 'RANDOM_PRODUCT', 'SET_BOOT_SCRIPT',
    // ---- v3.0 additions: Hak5-compatible ATTACKMODE, custom stick size, stealth HID, LED_ON ----
    'ATTACKMODE', 'HID_ATTACH', 'HID_DETACH', 'LED_ON', 'STORAGE_ONLY', 'MSC_ONLY', 'NO_HID', 'NOHID', 'HID_OFF',
    // v4.4: FACTORY_RESET / BEHAVE_BROKEN / SELF_DESTRUCT command aliases
    'FACTORY_RESET', 'FACTORYRESET', 'BEHAVE_BROKEN', 'BEHAVEBROKEN', 'SELF_DESTRUCT', 'BLANK', 'NONE', 'UNMOUNT',
    // ---- previously-missing commands actually implemented in firmware ----
    'LED_IR', 'LED_UV', 'RGB', 'RETURN', 'STOPHOLD', 'SAVE_CREDENTIALS',
    'COPY_FILE', 'CUT_FILE', 'USE_FILE', 'PASTE_FILE', 'IF_CONNECTED_TO_WIFI',
    'LOCALE_EN', 'LOCALE_DE', 'LOCALE_FR', 'LOCALE_ES', 'LOCALE_IT', 'LOCALE_UK',
    'RANDOM_CHAR', 'RANDOM_NUMBER', 'RANDOM_SPECIAL',
    // ---- v4.17-v4.26 Hak5 DuckyScript 3.0 corpus support (bug-fix v4.27) ----
    // Frontend linter was flagging every one of these as "Unknown command"
    // and drawing the red overlay ON TOP of the code (see error-line CSS).
    // Adding them here silences the false positives so the code stays legible.
    'EXTENSION', 'END_EXTENSION', 'RUN_EXTENSION', 'IMPORT', 'INJECT_MOD',
    'BUTTON_DEF', 'END_BUTTON', 'DISABLE_BUTTON',
    'WAIT_FOR_BUTTON_PRESS',
    'WAIT_FOR_CAPS_ON', 'WAIT_FOR_CAPS_OFF',
    'WAIT_FOR_NUM_ON',  'WAIT_FOR_NUM_OFF',
    'WAIT_FOR_SCROLL_ON', 'WAIT_FOR_SCROLL_OFF',
    'WAIT_FOR_CAPS_CHANGE', 'WAIT_FOR_NUM_CHANGE', 'WAIT_FOR_SCROLL_CHANGE',
    'WAIT_FOR_EOF',
    'HIDE_PAYLOAD', 'RESTORE_PAYLOAD', 'STOP_PAYLOAD',
    'SOFT_BRICK', 'REVERT_TO_THUMBDRIVE', 'PERSIST',
    'REM_BLOCK', 'END_REM',
    'DEFINE', 'IF_DEFINED_TRUE', 'IF_NOT_DEFINED_TRUE',
    'ELSE_DEFINED', 'END_IF_DEFINED',
    'STRING_BASH', 'STRING_POWERSHELL', 'END_STRING', 'END_STRINGLN',
    'RELEASE', 'MASS_STORAGE_ON', 'MASS_STORAGE_OFF',
    'RANDOM_LETTER', 'RANDOM_LOWERCASE_LETTER', 'RANDOM_UPPERCASE_LETTER',
    'THEN', 'CONTAINS', 'IN',
    // v4.28: additional Hak5 3.0 commands used by the OS_DETECTION extension
    'SAVE_HOST_KEYBOARD_LOCK_STATE', 'RESTORE_HOST_KEYBOARD_LOCK_STATE'
]);

// v4.28: Hak5 built-in $_ variables. These are seeded by the firmware at
// every script start (see executeScript's variables[_OS] etc. seeds), so
// the linter must treat them as always-declared - both as assignment
// targets ($_OS = LINUX) and as usages ($_CAPSLOCK_ON inside an IF).
// Without this the OS_DETECTION extension lights up 20+ false errors.
const BUILTIN_VARIABLES = new Set([
    '_OS',
    '_CAPSLOCK_ON', '_NUMLOCK_ON', '_SCROLLLOCK_ON',
    '_HOST_CONFIGURATION_REQUEST_COUNT',
    '_RECEIVED_HOST_LOCK_LED_REPLY',
    '_JITTER_MIN', '_JITTER_MAX', '_JITTER_ENABLED',
    '_RANDOM_MIN', '_RANDOM_MAX',
    // v4.29 bug-hunt HIGH #4: additional built-ins referenced (read-only)
    // by the Hak5 corpus exfil helpers - the firmware seeds/reads these,
    // scripts may inspect them without ever writing them, so a bare read
    // no longer warns "trying to use variable ... doesn't exist".
    '_EXFIL_MODE_ENABLED', '_EXFIL_LEDS_ENABLED',
    '_RANDOM_INT', '_RANDOM_LETTER', '_RANDOM_LOWERCASE_LETTER',
    '_RANDOM_UPPERCASE_LETTER', '_RANDOM_SPECIAL', '_RANDOM_CHAR',
    '_RANDOM_NUMBER'
]);



function getLevenshteinDistance(a, b) {
    if (!a || !b) return (a || b).length;
    const matrix = [];
    for (let i = 0; i <= b.length; i++) matrix[i] = [i];
    for (let j = 0; j <= a.length; j++) matrix[0][j] = j;
    for (let i = 1; i <= b.length; i++) {
        for (let j = 1; j <= a.length; j++) {
            if (b.charAt(i - 1) === a.charAt(j - 1)) matrix[i][j] = matrix[i - 1][j - 1];
            else matrix[i][j] = Math.min(matrix[i - 1][j - 1] + 1, Math.min(matrix[i][j - 1] + 1, matrix[i - 1][j] + 1));
        }
    }
    return matrix[b.length][a.length];
}

function getDidYouMean(cmd) {
    if (!cmd || cmd.length < 2) return null;
    let bestMatch = null;
    let bestDist = Infinity;
    for (const v of VALID_KEYWORDS) {
        const dist = getLevenshteinDistance(cmd, v);
        if (dist < bestDist && dist <= 3) {
            bestDist = dist;
            bestMatch = v;
        }
    }
    return bestMatch;
}

function fastUpdateHighlights() {
    const scriptArea = document.getElementById('scriptArea');
    const highlights = document.getElementById('editorHighlights');
    if (!scriptArea || !highlights) return;

    const lines = scriptArea.value.split('\n');
    
    globalDeclaredVars.clear();
    globalDeclaredFunctions.clear();

    lines.forEach((line) => {
        const trimmed = line.trim();
        const upper = trimmed.toUpperCase();
        
        const varDefMatch = trimmed.match(/^(VAR|VARIABLE)\s+([a-zA-Z0-9_]+)/i);
        if (varDefMatch) globalDeclaredVars.add(varDefMatch[2].toUpperCase());
        
        const varPrefixMatch = upper.match(/^(VAR_|VARIABLE_)([A-Z0-9_]*)\s*=/);
        if (varPrefixMatch) {
            const name = (varPrefixMatch[1] + varPrefixMatch[2]).toUpperCase();
            globalDeclaredVars.add(name);
        }

        const assignMatch = trimmed.match(/^([a-zA-Z_][a-zA-Z0-9_]*)\s*=/);
        if (assignMatch) {
            const name = assignMatch[1].toUpperCase();
            if (!VALID_KEYWORDS.has(name)) globalDeclaredVars.add(name);
        }

        const funcDefMatch = trimmed.match(/^(FUNCTION|DEF_)\s*([a-zA-Z0-9_]+)/i);
        if (funcDefMatch) globalDeclaredFunctions.add(funcDefMatch[2].toUpperCase());

        const forLoopMatch = trimmed.match(/^FOR\s+\$([a-zA-Z0-9_]+)/i);
        if (forLoopMatch) globalDeclaredVars.add(forLoopMatch[1].toUpperCase());
    });

    let html = '';
    for (let line of lines) {
        html += `<div>${applyHighlighting(line)}</div>`;
    }
    highlights.innerHTML = html;
}

function updateErrorLens() {
    try {
        const scriptArea = document.getElementById('scriptArea');
        const highlights = document.getElementById('editorHighlights');
        const container = document.getElementById('issuesList');
        const statusEl = document.getElementById('issueStatus');
        const panel = document.getElementById('issuesPanel');
        if (!scriptArea || !highlights) return;

        const lines = scriptArea.value.split('\n');
        
        globalDeclaredVars.clear();
        globalDeclaredFunctions.clear();

        const cursorIdx = scriptArea.selectionStart;
        const textBeforeCursor = scriptArea.value.substring(0, cursorIdx);
        const cursorLine = textBeforeCursor.split('\n').length - 1;

        const errors = [];
        let ifCount = 0, forCount = 0, whileCount = 0, repeatCount = 0;
        let inFunction = false;
        let linesInBlock = 0; // Track if function has content

        // v4.27 bug-hunt HIGH #4: pre-scan for opaque-body blocks whose
        // contents are NOT DuckyScript statements and must be excluded from
        // per-line validation. Without this, every `sudo apt-get update`
        // inside a STRING_BASH ... END_STRING block lit up as "Unknown
        // command 'sudo'", drowning the real errors in false positives.
        //
        // Blocks handled:
        //   STRING_BASH / STRING_POWERSHELL   -> body until END_STRING(LN)
        //   REM_BLOCK                          -> body until END_REM
        //   EXTENSION NAME ^                   -> collapsed-inline extension
        //                                        body until END_EXTENSION
        // The `^` marker distinguishes an INLINED-EXPANDED extension body
        // (which is real DuckyScript we DO want to lint) from a COLLAPSED
        // extension body (˅ marker, one line only, no body to skip).
        const opaqueLines = new Set();
        // v4.30: track opaque-block OPENER indices too so the per-line
        // validator can skip the `needsInput` check for `STRING` / `STRINGLN`
        // when they open a block (Hak5 corpus SAVE_FILES uses `STRINGLN`
        // alone on a line to open a multi-line body).
        const opaqueOpeners = new Set();
        {
            let inStr = false, inRem = false, inExtInline = false;
            for (let i = 0; i < lines.length; i++) {
                const t = lines[i].trim();
                const u = t.toUpperCase();
                if (inStr) {
                    if (u === 'END_STRING' || u === 'END_STRINGLN') { inStr = false; continue; }
                    opaqueLines.add(i);
                    continue;
                }
                if (inRem) {
                    if (u === 'END_REM') { inRem = false; continue; }
                    opaqueLines.add(i);
                    continue;
                }
                if (inExtInline) {
                    if (u === 'END_EXTENSION') { inExtInline = false; continue; }
                    // Inline-expanded extension body IS ducky - do NOT mark
                    // opaque. We only track inExtInline so that a nested
                    // STRING_BASH block starter inside the extension works.
                    if (u === 'STRING_BASH' || u === 'STRING_POWERSHELL' || u.startsWith('STRING_BASH ') || u.startsWith('STRING_POWERSHELL ')) { inStr = true; opaqueOpeners.add(i); continue; }
                    if (u === 'STRING' || u === 'STRINGLN') { inStr = true; opaqueOpeners.add(i); continue; }   // v4.30 bare-form
                    if (u === 'REM_BLOCK' || u.startsWith('REM_BLOCK ')) { inRem = true; opaqueOpeners.add(i); continue; }
                    continue;
                }
                if (u === 'STRING_BASH' || u === 'STRING_POWERSHELL' || u.startsWith('STRING_BASH ') || u.startsWith('STRING_POWERSHELL ')) { inStr = true; opaqueOpeners.add(i); continue; }
                // v4.30: bare `STRING` / `STRINGLN` with NO args opens a
                // multi-line block terminated by END_STRING(LN) - Hak5
                // corpus SAVE_FILES_IN_RUBBER_DUCKY_STORAGE_WINDOWS uses this
                // form to embed foreach/`mv` PowerShell bodies. Treat the
                // body as opaque so the linter doesn't flag `foreach` /
                // `mv` / `}` as unknown commands.
                if (u === 'STRING' || u === 'STRINGLN') { inStr = true; opaqueOpeners.add(i); continue; }
                if (u === 'REM_BLOCK' || u.startsWith('REM_BLOCK ')) { inRem = true; opaqueOpeners.add(i); continue; }
                if (u.startsWith('EXTENSION ') && t.endsWith('^')) { inExtInline = true; continue; }
            }
        }

        // Pre-scan for unclosed functions
        let lastUnclosedFunctionStart = -1;
        let funcLevel = 0;
        lines.forEach((line, idx) => {
            const trimmed = line.trim().toUpperCase();
            const isDefStart = trimmed.startsWith('FUNCTION') || trimmed.startsWith('DEF_') || (trimmed.endsWith('():') && !trimmed.startsWith('END_'));
            
            if (isDefStart) {
                lastUnclosedFunctionStart = idx;
                funcLevel++;
            } else if (trimmed === 'END_FUNCTION' || trimmed === 'END_DEF') {
                funcLevel--;
                if (funcLevel <= 0) {
                    lastUnclosedFunctionStart = -1;
                    funcLevel = 0;
                }
            }
        });

        const makeError = (txt) => ({ type: 'error', text: txt });
        const makeWarning = (txt, v = 'WIFI', idx) => ({ type: 'warning', text: txt, var: v, line: idx });

        const needsInput = [
            'STRING', 'STRINGLN', 'DELAY', 'DEFAULTDELAY', 'DEFAULT_DELAY', 'REPEAT', 
            'IF', 'ELIF', 'FOR', 'FUNCTION', 'DEF_', 'HOLD', 'KEYCODE',
            'DOWNLOAD_FILE', 'UPLOAD_FILE', 'JOIN_INTERNET', 'IF_PRESENT', 'IF_NOTPRESENT',
            'IF_BT_PRESENT', 'IF_OS', 'IF_DETECT_OS_INCLUDES', 'RUN_AT_TIME', 'RUN_AT_DAY',
            'WAIT_FOR_EVENT', 'RUN_WHEN_WIFI', 'LOCALE'
        ];

        // Pass 1: Collect definitions.
        // v4.29 bug-hunt HIGH #6: every registration below MUST skip lines
        // that are inside a STRING_BASH / STRING_POWERSHELL / REM_BLOCK
        // opaque body (sample code inside a REM_BLOCK was polluting
        // globalDeclaredVars and masking real "used before declared"
        // warnings elsewhere in the script). Iterate with the index so we
        // can consult opaqueLines.
        lines.forEach((line, idx) => {
            if (opaqueLines.has(idx)) return;
            const trimmed = line.trim();
            const upper = trimmed.toUpperCase();

            // Standard VAR/FUNCTION
            const varDefMatch = trimmed.match(/^(VAR|VARIABLE)\s+([a-zA-Z0-9_]+)/i);
            if (varDefMatch) globalDeclaredVars.add(varDefMatch[2].toUpperCase());

            const funcDefMatch = trimmed.match(/^(FUNCTION|DEF_)\s*([a-zA-Z0-9_]+)/i);
            if (funcDefMatch) globalDeclaredFunctions.add(funcDefMatch[2].toUpperCase());

            // Prefixed FUNCTION_ (Image 2 support)
            const funcPrefixMatch = upper.match(/^FUNCTION_([A-Z0-9_]+)/);
            if (funcPrefixMatch) globalDeclaredFunctions.add(funcPrefixMatch[1]);

            // Label-style FunctionName(): (Image 3 support)
            const funcLabelMatch = upper.match(/^([A-Z0-9_]+)\(\):/);
            if (funcLabelMatch) globalDeclaredFunctions.add(funcLabelMatch[1]);

            const varPrefixMatch = upper.match(/^(VAR_|VARIABLE_)([A-Z0-9_]*)\s*=/);
            if (varPrefixMatch) {
                const name = (varPrefixMatch[1] + varPrefixMatch[2]).toUpperCase();
                globalDeclaredVars.add(name);
            }

            const assignMatch = trimmed.match(/^([a-zA-Z_][a-zA-Z0-9_]*)\s*=/);
            if (assignMatch) {
                const name = assignMatch[1].toUpperCase();
                if (!VALID_KEYWORDS.has(name) && (name.startsWith('VAR_') || name.startsWith('VARIABLE_'))) {
                    globalDeclaredVars.add(name);
                }
            }

            // v4.28: Hak5 3.0 `$name = value` assignments auto-declare the
            // variable. v4.29 CRITICAL #2: also `VAR $name = value`.
            const dollarAssignMatch = trimmed.match(/^\$([a-zA-Z_][a-zA-Z0-9_]*)\s*=/);
            if (dollarAssignMatch) globalDeclaredVars.add(dollarAssignMatch[1].toUpperCase());
            const varDollarMatch = trimmed.match(/^(?:VAR|VARIABLE)\s+\$([a-zA-Z_][a-zA-Z0-9_]*)\s*=/i);
            if (varDollarMatch) globalDeclaredVars.add(varDollarMatch[1].toUpperCase());

            const forLoopMatch = trimmed.match(/^FOR\s+\$([a-zA-Z0-9_]+)/i);
            if (forLoopMatch) globalDeclaredVars.add(forLoopMatch[1].toUpperCase());
        });

        // Pass 2: Validation
        const processedVars = new Set();
        const processedFunctions = new Set();
        let highlightsHTML = '';
        let blockBroken = false;
        // v4.31: compute this once for the whole pass - it walks every
        // EXTENSION ref line and unions the cached function-name lists.
        const inScopeExtFuncs = _autocompleteFunctionsFromReferencedExtensions(scriptArea.value);

        lines.forEach((line, i) => {
            // v4.27 bug-hunt HIGH #4: lines inside STRING_BASH / STRING_POWERSHELL
            // / REM_BLOCK bodies are NOT DuckyScript - render them plainly and
            // skip every validation branch below.
            if (opaqueLines.has(i)) {
                highlightsHTML += `<div class="opaque-body">${escapeHtml(line)}</div>`;
                return;
            }
            const trimmed = line.trim();
            const upper = trimmed.toUpperCase();
            const words = trimmed.split(/\s+/);
            let cmd = words[0].toUpperCase();
            if (cmd.endsWith(':')) cmd = cmd.slice(0, -1);
            const argStr = trimmed.substring(words[0].length).trim();
            let errorMsg = null;

            const isIndented = line.startsWith(' ') || line.startsWith('\t');
            const isDefStartLine = upper.startsWith('FUNCTION') || upper.startsWith('DEF_') || (upper.endsWith('():') && !upper.startsWith('END_'));

            // Check if block is broken - must run before internal state changes
            if (inFunction && !isIndented && !isDefStartLine && upper !== 'END_FUNCTION') {
                if (!trimmed || (!trimmed.startsWith('REM') && !trimmed.startsWith('//'))) {
                    blockBroken = true;
                }
            }

            if (trimmed && !trimmed.startsWith('REM') && !trimmed.startsWith('//')) {
                const varPrefixMatch = upper.match(/^(VAR_|VARIABLE_)([A-Z0-9_]+)/);
                if (upper.startsWith('VAR_') || upper.startsWith('VARIABLE_')) {
                    if (!varPrefixMatch) {
                        errorMsg = makeError("Variable name required after '_' (e.g., VAR_MYVAR)");
                    } else {
                        const fullVarName = (varPrefixMatch[1] + varPrefixMatch[2]).toUpperCase();
                        if (trimmed.includes('=')) {
                            const afterEquals = trimmed.split('=')[1].trim();
                            if (!afterEquals && !ignoredWarnings.has(`${i}-${fullVarName}`)) {
                                errorMsg = makeWarning(`Variable assignment is empty`, fullVarName, i);
                            } else if (STRUCTURAL_KEYWORDS.has(afterEquals.toUpperCase())) {
                                errorMsg = makeError(`Variable assignment cannot be a structural keyword: '${afterEquals}'`);
                            } else if (processedVars.has(fullVarName)) {
                                errorMsg = makeError(`Variable '${fullVarName}' is already declared.`);
                            }
                            processedVars.add(fullVarName);
                        } else {
                            errorMsg = makeError(`Variable declaration must use '=' (e.g., VAR_NAME = VALUE)`);
                        }
                    }
                } else if (upper.startsWith('VAR ') || upper.startsWith('VARIABLE ')) {
                    // v4.29 bug-hunt CRITICAL #2: `VAR $name = value` IS the
                    // canonical Hak5 3.0 dynamic-variable declaration and is
                    // used across the corpus (translate.txt, self_destruct.txt,
                    // linux_hid_exfil.txt). Only flag the truly-legacy form
                    // (`VAR foo = ...` without the `$` prefix). The dollar form
                    // is already registered in globalDeclaredVars by Pass 1's
                    // dollarAssignMatch, so we just accept the line here.
                    const rest = trimmed.substring(trimmed.indexOf(' ') + 1).trim();
                    if (rest.startsWith('$')) {
                        // Hak5 3.0 form: nothing to flag, Pass 1 handled the declaration.
                    } else {
                        errorMsg = makeError("Legacy 'VAR' command is disabled. Use 'VAR_name = value' (or the Hak5 3.0 'VAR $name = value' form) instead.");
                    }
                } else {
                    const assignMatch = trimmed.match(/^([a-zA-Z0-9_]+)\s*=/);
                    if (assignMatch) {
                        const name = assignMatch[1].toUpperCase();
                        if (!name.startsWith('VAR_') && !VALID_KEYWORDS.has(name)) {
                            errorMsg = makeError("Variables must start with the 'VAR_' prefix.");
                        }
                    } else if (trimmed && !trimmed.startsWith('REM') && !trimmed.startsWith('//') && !trimmed.endsWith('():') && !trimmed.startsWith('FUNCTION') && !trimmed.startsWith('DEF_')) {
                        if (inFunction) linesInBlock++;
                    }
                }

                if (!errorMsg && needsInput.includes(cmd) && !argStr && !opaqueOpeners.has(i)) {
                    // v4.30: skip when this line is a block-opener (bare
                    // STRING/STRINGLN/STRING_BASH/... that starts an opaque
                    // body terminated by END_STRING(LN) / END_REM later).
                    errorMsg = makeError(`${cmd} requires parameters or input`);
                }

                if (!errorMsg && !varPrefixMatch) {
                    const varDefMatch = trimmed.match(/^(VAR|VARIABLE)\s+([a-zA-Z0-9_]+)/i);
                    const funcDefMatch = trimmed.match(/^(FUNCTION|DEF_)\s*([a-zA-Z0-9_]+)/i);
                    const funcPrefixMatch = upper.match(/^FUNCTION_([A-Z0-9_]+)/);
                    const funcLabelMatch = upper.match(/^([A-Z0-9_]+)\(\):/);

                    if (varDefMatch) {
                        errorMsg = makeError("Legacy 'VAR' command is disabled. Use the 'VAR_name = value' format.");
                    } else if (funcDefMatch || funcPrefixMatch || funcLabelMatch) {
                        const name = (funcDefMatch ? funcDefMatch[2] : (funcPrefixMatch ? funcPrefixMatch[1] : funcLabelMatch[1])).toUpperCase();
                        if (processedFunctions.has(name)) errorMsg = makeError(`Function '${name}' is already defined.`);
                        processedFunctions.add(name);
                        inFunction = true;
                        linesInBlock = 0;
                    } else if (upper.startsWith('END_FUNCTION') || upper.startsWith('END_DEF')) {
                        if (!inFunction) {
                            errorMsg = makeError("Found 'END_FUNCTION' without a matching 'FUNCTION' block.");
                        } else {
                            if (upper !== 'END_FUNCTION' && upper !== 'END_DEF') {
                                const extra = trimmed.substring(cmd.length).trim();
                                errorMsg = makeError(`${cmd} needs to be in a newline. You still have text: '${extra}'`);
                            } else if (linesInBlock === 0 && !ignoredWarnings.has(`${i}-EMPTY_FUNC`)) {
                                errorMsg = makeWarning("Function has nothing inside of it", "EMPTY_FUNC", i);
                            }
                        }
                        inFunction = false;
                    } else if (upper === 'IF' || upper === 'IF:' || upper.startsWith('IF ') || upper.startsWith('IF(') || upper.startsWith('IF_') || upper === 'RUN_ON_REBOOT' || upper === 'RUN_ON_REBOOT:' || upper.startsWith('RUN_ON_REBOOT ')) {
                        // v4.29 bug-hunt CRITICAL #1: also match `IF(` with no
                        // space - Hak5 3.0 extensions write `IF($X != $Y) THEN`
                        // (exfil_auto_eof_detect.txt, translate.txt).
                        // v4.27 bug-hunt HIGH #3: IF_DEFINED_TRUE / IF_NOT_DEFINED_TRUE
                        // are Hak5 preprocessor directives, NOT runtime IF blocks
                        // (they resolve at define-time via END_IF_DEFINED, not
                        // ENDIF), so they must NOT bump ifCount. Skip them here.
                        if (!upper.startsWith('IF_DEFINED_TRUE') && !upper.startsWith('IF_NOT_DEFINED_TRUE')) {
                            ifCount++;
                        }
                    } else if (upper.startsWith('ENDIF') || upper.startsWith('END_IF') || upper.startsWith('END_RUN_ON_REBOOT')) {
                        // v4.27 bug-hunt HIGH #3: whitelist the preprocessor pair
                        // END_IF_DEFINED / END_IF_NOT_DEFINED / END_IF_NOT_DEFINED_TRUE
                        // - they DON'T decrement ifCount (no matching IF pushed one)
                        // and they aren't "leftover text" errors.
                        const preprocEnd =
                            upper === 'END_IF_DEFINED' || upper === 'END_IF_NOT_DEFINED' ||
                            upper === 'END_IF_NOT_DEFINED_TRUE';
                        if (preprocEnd) {
                            // no-op: preprocessor terminator
                        } else {
                            ifCount--;
                            if (ifCount < 0) { errorMsg = makeError(`Found '${cmd}' without a matching block.`); ifCount = 0; }
                            // v4.30: also accept the `END_IF()` / `ENDIF()` form
                            // (translate.txt). Only complain about leftover text
                            // when the whole line isn't just the terminator or the
                            // terminator with a bare `()`.
                            else if (upper !== 'ENDIF' && upper !== 'END_IF' && upper !== 'END_RUN_ON_REBOOT'
                                     && upper !== 'ENDIF()' && upper !== 'END_IF()' && upper !== 'END_RUN_ON_REBOOT()') {
                                const extra = trimmed.substring(cmd.length).trim();
                                errorMsg = makeError(`${cmd} needs to be in a newline. You still have text: '${extra}'`);
                            }
                        }
                    } else if (upper.startsWith('ELSE')) {
                        // v4.27 bug-hunt HIGH #3: ELSE_DEFINED is the preprocessor
                        // sibling of IF_DEFINED_TRUE - not the runtime ELSE clause.
                        if (upper === 'ELSE_DEFINED') {
                            // no-op: preprocessor branch marker
                        } else if (ifCount <= 0) {
                            errorMsg = makeError("Found 'ELSE' without a matching 'IF' block.");
                        } else if (upper !== 'ELSE' && !upper.startsWith('ELSE IF') && !upper.startsWith('ELIF')
                                   // v4.30: `ELSE (cond) THEN` is the shorthand
                                   // ELIF the Hak5 corpus uses (ROLLING_POWERSHELL).
                                   && !upper.startsWith('ELSE (') && !upper.startsWith('ELSE(')) {
                            const extra = trimmed.substring(cmd.length).trim();
                            errorMsg = makeError(`ELSE needs to be in a newline. You still have text: '${extra}'`);
                        }
                    } else if (upper === 'FOR' || upper === 'FOR:' || upper.startsWith('FOR ') || upper.startsWith('FOR(')) {
                        forCount++;
                    } else if (upper.startsWith('ENDFOR') || upper.startsWith('END_FOR')) {
                        forCount--;
                        if (forCount < 0) { errorMsg = makeError(`Found '${cmd}' without a matching 'FOR' block.`); forCount = 0; }
                        else if (upper !== 'ENDFOR' && upper !== 'END_FOR') {
                            const extra = trimmed.substring(cmd.length).trim();
                            errorMsg = makeError(`${cmd} needs to be in a newline. You still have text: '${extra}'`);
                        }
                    } else if (upper === 'WHILE' || upper === 'WHILE:' || upper.startsWith('WHILE ') || upper.startsWith('WHILE(')) {
                        // v4.29 bug-hunt CRITICAL #1: `WHILE(($_FOO == FALSE))` -
                        // Hak5 corpus writes this with no space (passive_detect_ready
                        // .txt, passive_windows_detect.txt, linux_hid_exfil.txt).
                        whileCount++;
                    } else if (upper.startsWith('END_WHILE')) {
                        whileCount--;
                        if (whileCount < 0) { errorMsg = makeError(`Found 'END_WHILE' without a matching 'WHILE' block.`); whileCount = 0; }
                        else if (upper !== 'END_WHILE') {
                            const extra = trimmed.substring(cmd.length).trim();
                            errorMsg = makeError(`END_WHILE needs to be in a newline. You still have text: '${extra}'`);
                        }
                    } else if (upper === 'REPEAT' || upper === 'REPEAT:' || upper.startsWith('REPEAT ')) {
                        repeatCount++;
                        if (i === 0) errorMsg = makeError("REPEAT cannot be on the first line.");
                        const val = parseInt(argStr);
                        if (argStr && isNaN(val)) errorMsg = makeError("REPEAT requires a number");
                        else if (val > 100 && !ignoredWarnings.has(`${i}-REPEAT_HIGH`)) errorMsg = makeWarning("High repeat count. Payload will take time.", 'REPEAT_HIGH', i);
                    } else if (upper.startsWith('END_REPEAT')) {
                        repeatCount--;
                        if (repeatCount < 0) { errorMsg = makeError(`Found 'END_REPEAT' without a matching 'REPEAT' block.`); repeatCount = 0; }
                        else if (upper !== 'END_REPEAT') {
                            const extra = trimmed.substring(cmd.length).trim();
                            errorMsg = makeError(`END_REPEAT needs to be in a newline. You still have text: '${extra}'`);
                        }
                    } else if (cmd === 'DELAY' || cmd === 'DEFAULTDELAY' || cmd === 'DEFAULT_DELAY') {
                        // v4.28: accept `DELAY $var`, `DELAY #DEFINE`,
                        // `DELAY VAR_x` and bare identifiers as valid too -
                        // the firmware resolves them via processVariables /
                        // #DEFINE substitution before parsing the number.
                        // Only flag when the argument is a LITERAL non-number.
                        // v4.29 bug-hunt HIGH #8: also accept bare identifiers
                        // (`DELAY DEFAULT_DELAY_VAR`) and `$` followed by any
                        // subsequent ident char (Hak5 tolerates `$2ND_TRY`).
                        const isVarRef = /^(\$[A-Za-z0-9_]+|#[A-Za-z_][A-Za-z0-9_]*|[A-Za-z_][A-Za-z0-9_]*)$/i.test(argStr);
                        const val = parseInt(argStr);
                        if (!isVarRef) {
                            if (isNaN(val)) errorMsg = makeError(`${cmd} requires a number`);
                            else if (val < 0) errorMsg = makeError(`${cmd} cannot be negative`);
                            else if (val > 30000 && !ignoredWarnings.has(`${i}-DELAY_HIGH`)) errorMsg = makeWarning(`Delay is very long (${val}ms).`, 'DELAY_HIGH', i);
                            else if (val < 20 && val > 0 && !ignoredWarnings.has(`${i}-DELAY_FAST`)) errorMsg = makeWarning(`Delay might be too fast for some HID interfaces.`, 'DELAY_FAST', i);
                        }
                    } else if (cmd === 'LED') {
                        const parts = argStr.split(/\s+/).filter(x => x.length > 0);
                        if (parts.length === 3) {
                            parts.forEach(p => {
                                const v = parseInt(p);
                                if (isNaN(v) || v < 0 || v > 255) errorMsg = makeError("LED RGB values must be 0-255");
                            });
                        }
                    } else if (cmd === 'SELFDESTRUCT') {
                        if (!ignoredWarnings.has(`${i}-DANGER`)) errorMsg = makeWarning(`Dangerous command: This will trigger a device event immediately.`, 'DANGER', i);
                    } else if (trimmed.includes('=') && !upper.startsWith('IF') && !upper.startsWith('ELIF') && !upper.startsWith('FOR') && !upper.startsWith('WHILE')
                                                                                    // v4.30: don't parse STRING / STRINGLN / DEFINE args as
                                                                                    // assignments - their bodies legitimately contain `=`
                                                                                    // (`STRING cmd /c "FOR ... tokens=4"`, `DEFINE #X val`).
                                                                                    && !upper.startsWith('STRING ') && !upper.startsWith('STRINGLN ')
                                                                                    && !upper.startsWith('STRING_') && !upper.startsWith('DEFINE ')
                                                                                    // v4.30: `VAR $name = value` is handled up in the VAR
                                                                                    // branch (chain 1); don't double-fire on it here.
                                                                                    && !upper.startsWith('VAR ') && !upper.startsWith('VARIABLE ')) {
                        const name = trimmed.split('=')[0].trim().toUpperCase();
                        const afterEquals = trimmed.split('=')[1].trim();
                        // v4.28: $-prefixed names are Hak5 3.0 dynamic variables.
                        // They can be assigned freely without a prior declaration
                        // AND can be reassigned any number of times (`$_OS =
                        // WINDOWS` then later `$_OS = LINUX` inside an ELSE arm
                        // is normal). Skip both "used before declared" and
                        // "already declared" checks for them, and register the
                        // name (stripped $) as declared so later usages don't
                        // warn.
                        const isDollarVar = name.startsWith('$');
                        // v4.29 bug-hunt HIGH #7: check the FIRST TOKEN of the
                        // RHS, not the whole string. Previously `$foo =
                        // END_FUNCTION suffix` slipped through because the
                        // full RHS didn't match any STRUCTURAL_KEYWORDS entry,
                        // and the dollar-branch then auto-registered FOO as
                        // declared. Split on whitespace so the first token
                        // gets checked in isolation.
                        const afterHead = (afterEquals.split(/\s+/)[0] || '').toUpperCase()
                                            .replace(/[;,)]+$/, '');   // strip trailing punctuation
                        if (!afterEquals && !ignoredWarnings.has(`${i}-${name}`)) {
                            // v4.30: Hak5 built-in `$_JITTER_MIN =` (empty) is a
                            // deliberate reset (protected_storage_mode.txt).
                            // Don't nag on those - they're intentional.
                            const bareBuiltin = name.startsWith('$') && BUILTIN_VARIABLES.has(name.substring(1));
                            if (!bareBuiltin) errorMsg = makeWarning(`Variable assignment is empty`, name, i);
                        } else if (STRUCTURAL_KEYWORDS.has(afterHead)) {
                            errorMsg = makeError(`Variable assignment cannot be a structural keyword: '${afterHead}'`);
                        } else if (isDollarVar) {
                            // Auto-declare and allow reassignment.
                            globalDeclaredVars.add(name.substring(1));
                        } else if (processedVars.has(name)) {
                             errorMsg = makeError(`Variable '${name}' is already declared.`);
                        } else if (!globalDeclaredVars.has(name) && !VALID_KEYWORDS.has(name)) {
                            errorMsg = makeError(`Variable '${name}' is used before being declared.`);
                        }
                        if (!isDollarVar) processedVars.add(name);
                    } else if (trimmed.endsWith('()')) {
                        // v4.30: DEFINE #NAME REVERT_TO_THUMBDRIVE() etc.
                        // is a DEFINE whose value happens to end in `()`, not
                        // a call. Also, several Hak5 3.0 built-ins are invoked
                        // with the trailing `()` form (WAIT_FOR_EOF(),
                        // SOFT_BRICK(), REVERT_TO_THUMBDRIVE(), END_IF()...);
                        // treat them as valid built-in commands.
                        const fName = trimmed.replace('()', '').trim().toUpperCase();
                        const isDefineHead = upper.startsWith('DEFINE ');
                        const builtinCall = new Set([
                            'WAIT_FOR_EOF', 'SOFT_BRICK', 'REVERT_TO_THUMBDRIVE',
                            'END_IF', 'ENDIF', 'END_FUNCTION', 'END_DEF',
                            'STOP_PAYLOAD', 'HIDE_PAYLOAD', 'RESTORE_PAYLOAD',
                            'SAVE_HOST_KEYBOARD_LOCK_STATE',
                            'RESTORE_HOST_KEYBOARD_LOCK_STATE',
                            'PERSIST', 'CONSUME', 'DISABLE_BUTTON',
                            // v4.31: DETECT_OS removed - extension-defined,
                            // gated by inScopeExtFuncs below. GET_TIME/GET_DAY
                            // stay - they're firmware built-ins (see
                            // DuckyInterpreter.cpp:1903).
                            'GET_TIME', 'GET_DAY'
                        ]);
                        if (isDefineHead) {
                            // no-op: DEFINE value; the DEFINE handler is elsewhere
                        } else if (builtinCall.has(fName)) {
                            // Recognised built-in - accept the `()` invocation form.
                        } else if (!globalDeclaredFunctions.has(fName)
                                   // v4.31: also accept functions the script
                                   // pulls in via a collapsed `EXTENSION NAME ˅`
                                   // reference - the linter can't see the body
                                   // but the extension-body cache built by
                                   // refreshExtensionAutocompletePool knows the
                                   // names, hoisted into inScopeExtFuncs above.
                                   && !inScopeExtFuncs.has(fName)) {
                            errorMsg = makeError(`Call to undefined function: '${fName}()'`);
                        }
                    } else {
                        // FALLBACK: Unknown command or variable check
                        // NOTE: SIZE_XX_UNIT (e.g. SIZE_22_GB) is a Hak5-compatible token
                        // that may appear standalone OR inside an ATTACKMODE line - always accept.
                        // HID_ prefix covers HID_ATTACH / HID_DETACH.
                        // v4.27: WAIT_FOR_ and INJECT_ prefixes cover the v3.0 Hak5
                        // families (WAIT_FOR_CAPS_*, INJECT_MOD, etc.) so future
                        // additions in that namespace don't spurious-error.
                        const isPrefixCmd = /^(VID_|PID_|MAN_|PRODUCT_|HOLD_|HOLD_TILL_|SIZE_|HID_|LED_|BLINK_LED_|LOCALE_|RANDOM_|IF_|WAIT_FOR_|INJECT_)/.test(cmd);
                        const isKeyword = VALID_KEYWORDS.has(cmd);
                        const isDeclaredVar = globalDeclaredVars.has(cmd);
                        // v4.29 bug-hunt HIGH #3: `#DEFINE`d macros can be
                        // invoked as bare statements - the preprocessor
                        // substitutes them before execution
                        // (self_destruct.txt: `#DESTRUCT_METHOD`,
                        // `#BOOT_ATTACKMODE`). Accept a `#IDENT` first word.
                        const isDefineRef = /^#[A-Za-z_][A-Za-z0-9_]*$/.test(cmd);
                        // v4.30: dash-separated modifier combos like
                        // `CTRL-SHIFT`, `CTRL-ALT-DELETE`, `GUI-SHIFT-S` are
                        // valid Hak5 3.0 combos (community_WINDOWS_ELEVATED_
                        // EXECUTION.txt). Accept any token where every dash-
                        // separated segment resolves to a known key/modifier.
                        const isComboKey = cmd.includes('-') && cmd.split('-').every(seg => {
                            if (!seg) return false;
                            if (VALID_KEYWORDS.has(seg)) return true;
                            return /^(CTRL|CONTROL|SHIFT|ALT|GUI|WINDOWS|META|COMMAND|CMD|OPTION|ALTGR|F([1-9]|1[0-2])|[A-Z0-9])$/.test(seg);
                        });
                        // v4.31 (bare-call): the Hak5 dialect accepts
                        // `DETECT_OS` / `HELLO_OS` at top level as a bare
                        // call to a user function (no parens). If the
                        // in-scope extension-function set (or the local
                        // FUNCTION scanner) knows about this name, accept
                        // it as a call - don't complain that it's an
                        // "Unknown command". Same for a user's own local
                        // FUNCTION FOO() { ... } END_FUNCTION invoked as
                        // just `FOO` (Ducky firmware matches functionTable
                        // for bare identifiers too, see DuckyInterpreter
                        // .cpp:797-802 - `potentialFunc = line` strips a
                        // trailing `()` but works without one either way).
                        const isBareFuncCall = globalDeclaredFunctions.has(cmd) || inScopeExtFuncs.has(cmd);

                        if (!isKeyword && !isDeclaredVar && !isPrefixCmd && !isDefineRef && !isComboKey && !isBareFuncCall) {
                            if (cmd.endsWith(':')) {
                                const name = cmd.slice(0, -1);
                                errorMsg = makeError(`Unknown command '${cmd}'. Did you mean 'FUNCTION ${name}'?`);
                            } else {
                                // v4.31 (typo help): first check in-scope
                                // extension functions and locally-defined
                                // FUNCTIONs for a near match - a user who
                                // typed `OS_DETECT` while referencing the
                                // OS_DETECT extension probably meant
                                // `DETECT_OS`. Falls back to VALID_KEYWORDS
                                // via getDidYouMean if no close match.
                                let suggestion = null;
                                const scope = new Set([...inScopeExtFuncs, ...globalDeclaredFunctions]);
                                const cmdSegsArr = cmd.split('_');
                                const cmdSegsSorted = cmdSegsArr.slice().sort().join('|');
                                // (a) exact segment-swap match: OS_DETECT vs
                                // DETECT_OS have the same underscore-separated
                                // segments in a different order.
                                for (const fn of scope) {
                                    if (fn.split('_').sort().join('|') === cmdSegsSorted) { suggestion = fn; break; }
                                }
                                // (b) fuzzy segment-swap: same segment COUNT,
                                // each segment in the typo has a close match
                                // in the target (Levenshtein <= 1 for short
                                // words, <= 2 for longer) - catches plurals
                                // (FILES vs FILE), single-letter typos in one
                                // segment, and common shortening.
                                if (!suggestion) {
                                    for (const fn of scope) {
                                        const fs = fn.split('_');
                                        if (fs.length !== cmdSegsArr.length) continue;
                                        const target = fs.slice();
                                        let ok = true;
                                        for (const seg of cmdSegsArr) {
                                            let hit = -1;
                                            for (let k = 0; k < target.length; k++) {
                                                const t = target[k]; if (t == null) continue;
                                                const tol = Math.max(t.length, seg.length) >= 5 ? 2 : 1;
                                                if (getLevenshteinDistance(seg, t) <= tol) { hit = k; break; }
                                            }
                                            if (hit === -1) { ok = false; break; }
                                            target[hit] = null;
                                        }
                                        if (ok) { suggestion = fn; break; }
                                    }
                                }
                                // (c) fall back to whole-string Levenshtein
                                // against the in-scope function names.
                                if (!suggestion) {
                                    let bestDist = 3;
                                    for (const fn of scope) {
                                        const d = getLevenshteinDistance(cmd, fn);
                                        if (d < bestDist) { bestDist = d; suggestion = fn; }
                                    }
                                }
                                // (d) finally, VALID_KEYWORDS via getDidYouMean.
                                if (!suggestion) suggestion = getDidYouMean(cmd);
                                if (suggestion) {
                                    errorMsg = makeError(`Unknown command '${cmd}'. Did you mean '${suggestion}'?`);
                                } else {
                                    errorMsg = makeError(`Unknown command or variable: '${words[0]}'`);
                                }
                            }
                        }
                    }
                    
                    if (inFunction && !isDefStartLine && upper !== 'END_FUNCTION' && trimmed && !trimmed.startsWith('REM') && !trimmed.startsWith('//')) {
                        linesInBlock++;
                    }
                }
            }

                // v4.29 bug-hunt HIGH #5: WHILE and REPEAT bodies were not
                // scanned for "trying to use variable ... doesn't exist", so a
                // typo like `$_CAPS_LOCK_ON` inside a WHILE conditional slipped
                // through unnoticed. Include both here. Also normalize the
                // no-space `IF(`/`WHILE(`/`FOR(` forms so their conditionals
                // still get scanned.
                const cmdBase = cmd.replace(/[(:].*/, '');
                // v4.30: if the current line is a STRING/STRINGLN whose body
                // clearly contains shell/PS content (semicolons, assignments,
                // `foreach`, `Set-Variable`, `-band`), the `$VAR` refs in it
                // are PowerShell/bash vars typed at the host, NOT DuckyScript
                // vars - suppress the "trying to use variable" warning so we
                // stop nagging on exfil payloads (windows/linux_hid_exfil,
                // SAVE_FILES_IN_RUBBER_DUCKY_STORAGE_WINDOWS).
                const looksLikeShell = (cmdBase === 'STRING' || cmdBase === 'STRINGLN') &&
                    /(;|=|foreach\b|Set-Variable|ToCharArray|-band|-bor|\$\{|>>|\|\||&&)/i.test(argStr);
                if (!errorMsg && !looksLikeShell && ['STRING', 'STRINGLN', 'IF', 'ELIF', 'FOR', 'WHILE', 'REPEAT'].includes(cmdBase)) {
                    const varMatches = trimmed.match(/(VAR_[a-zA-Z0-9_]*|VARIABLE_[a-zA-Z0-9_]*|\$[a-zA-Z0-9_]+)/gi);
                    if (varMatches) {
                        for (const m of varMatches) {
                            let v = m.toUpperCase();
                            // If it starts with $, strip it before checking if it was declared
                            if (v.startsWith('$')) v = v.substring(1);

                            // v4.28: accept Hak5 built-in $_ vars ($_OS,
                            // $_CAPSLOCK_ON etc.) as always-declared - the
                            // firmware seeds them at every script start.
                            if (BUILTIN_VARIABLES.has(v)) continue;

                            if (!globalDeclaredVars.has(v) && !ignoredWarnings.has(`${i}-${v}`)) {
                                errorMsg = makeWarning(`Trying to use variable '${v}'? It doesn't exist.`, v, i);
                                break;
                            }
                        }
                    }
                }

            const hlLine = applyHighlighting(line);
            
            // Check for block-level error (Unclosed Function)
            let isBlockErrorLine = (lastUnclosedFunctionStart !== -1 && i >= lastUnclosedFunctionStart && cursorLine > lastUnclosedFunctionStart && !blockBroken);
            let blockError = (isBlockErrorLine && i === lastUnclosedFunctionStart) ? "Block requires an END_FUNCTION" : null;

            if (errorMsg || isBlockErrorLine) {
                const isWarning = errorMsg ? errorMsg.type === 'warning' : false;
                const msgText = errorMsg ? (errorMsg.text || (typeof errorMsg === 'string' ? errorMsg : 'Unknown Issue')) : (blockError || "");
                // v4.27 bug-hunt HIGH #5: msgText may contain the user's own text
                // (e.g. `Unknown command '<img src=x onerror=...>'`), and it goes
                // into innerHTML below. Escape it once here so both the tooltip
                // attribute AND the visible span get safe HTML.
                const msgHtml = escapeHtml(msgText);
                const msgTitle = msgHtml.replace(/"/g, '&quot;');
                const className = isWarning ? 'warning-line' : 'error-line';
                const lensClass = isWarning ? 'inline-warning' : 'inline-error';

                const isFixable = !isWarning && (msgText && (msgText.includes("Did you mean") || msgText.includes("END_FUNCTION")));
                // Only push to error list if it's the first line of block error or a normal error
                if (errorMsg || i === lastUnclosedFunctionStart) {
                    errors.push({ type: isWarning ? 'warning' : 'error', line: i, text: `Line ${i+1}: ${msgText}`, var: errorMsg ? errorMsg.var : null, fixable: isFixable });
                }
                
                const isStructuralStart = upper.startsWith('FUNCTION') || upper.startsWith('DEF_') || (upper.endsWith('():') && !upper.startsWith('END_')) || upper.startsWith('IF') || upper.startsWith('FOR') || upper.startsWith('WHILE') || upper.startsWith('REPEAT');
                const isStructuralEnd = upper === 'END_FUNCTION' || upper === 'END_DEF' || upper === 'ENDIF' || upper === 'END_IF' || upper === 'ENDFOR' || upper === 'END_FOR' || upper === 'END_WHILE' || upper === 'END_REPEAT' || upper === 'ELSE';
                
                const isScoped = (inFunction || ifCount > 0 || forCount > 0 || whileCount > 0 || repeatCount > 0) && !isStructuralStart && !isStructuralEnd && (line.startsWith(' ') || line.startsWith('\t'));
                const scopeClass = isScoped ? `scope-guide ${isBlockErrorLine ? 'scope-error' : ''}` : '';
                let lineContent = `<div class="${className} ${scopeClass}" style="position: relative; white-space: pre;">`;
                if (isWarning) {
                    lineContent += `<span class="warning-text">${hlLine}</span><span class="${lensClass}" title="${msgTitle}">${msgHtml}</span>`;
                    lineContent += `<button class="ignore-btn-inline" style="pointer-events: auto;" onclick="event.stopPropagation(); ignoreWarning(${i}, '${errorMsg ? (errorMsg.var || '') : ''}', this)">Ignore</button>`;
                } else {
                    lineContent += `<span>${hlLine}</span><span class="${lensClass}" title="${msgTitle}">${msgHtml}</span>`;
                }
                lineContent += `</div>`;
                highlightsHTML += lineContent;
            } else {
                const isStructuralStart = upper.startsWith('FUNCTION') || upper.startsWith('DEF_') || (upper.endsWith('():') && !upper.startsWith('END_')) || upper.startsWith('IF') || upper.startsWith('FOR') || upper.startsWith('WHILE') || upper.startsWith('REPEAT');
                const isStructuralEnd = upper === 'END_FUNCTION' || upper === 'END_DEF' || upper === 'ENDIF' || upper === 'END_IF' || upper === 'ENDFOR' || upper === 'END_FOR' || upper === 'END_WHILE' || upper === 'END_REPEAT';
                
                const isScoped = (inFunction || ifCount > 0 || forCount > 0) && !isStructuralStart && !isStructuralEnd && (line.startsWith(' ') || line.startsWith('\t'));
                const scopeClass = isScoped ? `scope-guide ${isBlockErrorLine ? 'scope-error' : ''}` : '';
                highlightsHTML += `<div class="${scopeClass}">${hlLine}</div>`;
            }
        });

        if (ifCount > 0) errors.push({ type: 'error', line: -1, text: "Missing 'ENDIF' for one or more 'IF' blocks." });
        if (forCount > 0) errors.push({ type: 'error', line: -1, text: "Missing 'ENDFOR' for one or more 'FOR' loops." });
        if (inFunction) errors.push({ type: 'error', line: -1, text: "Missing 'END_FUNCTION' for function definition.", fixable: true });

        highlights.innerHTML = highlightsHTML;
        
        if (container && panel) {
            const newState = JSON.stringify(errors);
            if (newState !== lastErrorState) {
                container.innerHTML = errors.map(e => {
                    const isError = e.type === 'error';
                    const itemClass = isError ? 'error-item' : 'warning-item';
                    const icon = isError ? '✕' : '▲';
                    // v4.27 bug-hunt HIGH #5: escape user-derived text before
                    // splicing it into the panel's innerHTML.
                    return `
                        <div class="issue-item ${itemClass}" onclick="jumpToLine(${e.line})">
                            <div class="issue-icon">${icon}</div>
                            <div class="issue-text">${escapeHtml(e.text)}</div>
                        </div>`;
                }).join('');
                lastErrorState = newState;
            }

            const hasErrors = errors.some(e => e.type === 'error');
            const hasWarnings = errors.some(e => e.type === 'warning');

            if (errors.length > 0) {
                panel.style.display = 'block';
                panel.classList.toggle('has-errors', hasErrors);
                panel.classList.toggle('has-warnings', hasWarnings && !hasErrors);
                
                const countText = `${errors.length} Issue${errors.length > 1 ? 's' : ''} Found`;
                statusEl.innerHTML = `<span>${hasErrors ? '✕' : '▲'}</span> ${countText}`;
            } else {
                panel.style.display = 'none';
                panel.classList.remove('has-errors', 'has-warnings', 'expanded');
            }
        }

        const fixAllBtn = document.getElementById('fixAllBtn');
        if (fixAllBtn) {
            const hasFixable = errors.some(e => e.fixable);
            fixAllBtn.style.display = hasFixable ? 'block' : 'none';
        }
    } catch (e) { console.error("Validator Crash:", e); }
}

function jumpToLine(lineIdx) {
    if (lineIdx < 0) return;
    const scriptArea = document.getElementById('scriptArea');
    const lines = scriptArea.value.split('\n');
    let charPos = 0;
    for (let i = 0; i < lineIdx; i++) {
        charPos += lines[i].length + 1;
    }
    scriptArea.focus();
    scriptArea.setSelectionRange(charPos, charPos + lines[lineIdx].length);
    const lineHeight = 22;
    scriptArea.scrollTop = (lineIdx * lineHeight) - (scriptArea.offsetHeight / 3);
    syncHighlightsScroll();
}



function fixAllErrors() {
    const scriptArea = document.getElementById('scriptArea');
    let lines = scriptArea.value.split('\n');
    let changed = false;
    let newLines = [];
    let inFunc = false;

    for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        const trimmed = line.trim();
        const upper = trimmed.toUpperCase();
        
        // Block detection
        const isDefStart = upper.startsWith('FUNCTION') || upper.startsWith('DEF_') || (upper.endsWith('():') && !upper.startsWith('END_'));
        const isEnd = upper === 'END_FUNCTION' || upper === 'END_DEF';

        // If we are in a function and find a non-indented line that isn't a comment/empty, close it
        if (inFunc && !isEnd && trimmed && !line.startsWith(' ') && !line.startsWith('\t') && !isDefStart && !trimmed.startsWith('REM') && !trimmed.startsWith('//')) {
            newLines.push('END_FUNCTION');
            inFunc = false;
            changed = true;
        }

        if (isDefStart) inFunc = true;
        if (isEnd) inFunc = false;

        // Perform typo correction
        let fixedLine = line;
        if (trimmed && !trimmed.startsWith('REM') && !trimmed.startsWith('//') && !isDefStart && !isEnd) {
            const firstWord = trimmed.split(/\s+/)[0].toUpperCase().replace('()', '');
            if (!VALID_KEYWORDS.has(firstWord) && !globalDeclaredVars.has(firstWord) && !globalDeclaredFunctions.has(firstWord)) {
                let bestMatch = null;
                let minDistance = 3; 
                VALID_KEYWORDS.forEach(cmd => {
                    const dist = levenshtein(firstWord, cmd);
                    if (dist < minDistance) { minDistance = dist; bestMatch = cmd; }
                });
                if (bestMatch) {
                    fixedLine = line.replace(new RegExp(firstWord.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), 'i'), bestMatch);
                    changed = true;
                }
            }
        }
        newLines.push(fixedLine);
    }

    if (inFunc) {
        newLines.push('END_FUNCTION');
        changed = true;
    }

    if (changed) {
        scriptArea.value = newLines.join('\n');
        updateGutter();
        updateErrorLens();
    }
}

function levenshtein(a, b) {
    const matrix = [];
    for (let i = 0; i <= b.length; i++) matrix[i] = [i];
    for (let j = 0; j <= a.length; j++) matrix[0][j] = j;
    for (let i = 1; i <= b.length; i++) {
        for (let j = 1; j <= a.length; j++) {
            if (b.charAt(i - 1) === a.charAt(j - 1)) matrix[i][j] = matrix[i - 1][j - 1];
            else matrix[i][j] = Math.min(matrix[i - 1][j - 1] + 1, matrix[i][j - 1] + 1, matrix[i - 1][j] + 1);
        }
    }
    return matrix[b.length][a.length];
}



function refreshFiles() {
    fetch('/api/scripts').then(r => r.json()).then(files => {
        const list = document.getElementById('fileList');
        list.innerHTML = '';
        files.forEach(f => {
            // v4.27 bug-hunt CRITICAL #1: previous version interpolated the
            // filename directly into an onclick= attribute AND into a JS-string
            // argument, so a name like `foo'); fetch('/api/self-destruct',
            // {method:'POST'}); //` broke out into admin-origin JS. Build via
            // DOM + textContent + addEventListener so the filename never
            // touches HTML or a JS string.
            const item = document.createElement('div');
            item.className = 'file-item';
            const name = document.createElement('span');
            name.textContent = f;
            const row = document.createElement('div');
            row.className = 'flex-row';
            const loadBtn = document.createElement('button');
            loadBtn.textContent = 'Load';
            loadBtn.addEventListener('click', () => loadFile(f));
            const delBtn = document.createElement('button');
            delBtn.className = 'danger';
            delBtn.textContent = 'Del';
            delBtn.addEventListener('click', () => deleteFile(f));
            row.appendChild(loadBtn);
            row.appendChild(delBtn);
            item.appendChild(name);
            item.appendChild(row);
            list.appendChild(item);
        });
    });
}

function refreshBootScripts() {
    fetch('/api/scripts').then(r => r.json()).then(files => {
        fetch('/api/stats').then(r => r.json()).then(stats => {
            const list = document.getElementById('bootScriptsList');
            list.innerHTML = '';
            files.forEach(f => {
                const checked = stats.bootScripts && stats.bootScripts.includes(f);
                const item = document.createElement('div');
                item.className = 'file-item';
                item.innerHTML = `<label class="custom-checkbox"><input type="checkbox" name="bootScript" value="${f}" ${checked ? 'checked' : ''}> ${f}</label>`;
                list.appendChild(item);
            });
        }).catch(e => console.error('boot stats load failed', e));
    }).catch(e => console.error('boot scripts load failed', e));
}

function saveBootScripts() {
    const checkboxes = document.querySelectorAll('input[name="bootScript"]:checked');
    const filenames = Array.from(checkboxes).map(cb => cb.value);
    fetch('/api/set-boot-script', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ filenames: filenames })
    }).then(r => r.text()).then(msg => alert(msg));
}

function refreshFileBrowser() {
    const browser = document.getElementById('fileBrowser');
    browser.innerHTML = '<div class="file-browser-item">Loading...</div>';
    fetch('/api/list-files?path=' + encodeURIComponent(currentBrowserPath)).then(r => r.json()).then(files => {
        browser.innerHTML = '';
        document.getElementById('currentDirDisplay').textContent = currentBrowserPath;
        // v4.27 bug-hunt CRITICAL #1: rebuild via DOM API so file.name /
        // file.path never touch an inline onclick or interpolated JS-string.
        // The old innerHTML template splat both fields into onclick= and into
        // navigateToDirectory('${path}') - a crafted filename could inject
        // admin-origin JS.
        files.forEach(file => {
            const item = document.createElement('div');
            item.className = 'file-browser-item';
            item.dataset.filePath = file.path;
            item.dataset.fileName = file.name;
            item.dataset.isDir    = file.isDirectory ? '1' : '0';
            const label = document.createElement('span');
            label.style.cssText = 'cursor:pointer;color:' + (file.isDirectory ? 'var(--primary)' : 'white');
            label.textContent = file.name + (file.isDirectory ? '/' : '');
            if (file.isDirectory) {
                label.addEventListener('click', () => navigateToDirectory(file.path));
            } else {
                // v4.33: previous version called selectFileInBrowser which
                // was never defined (regression from the v4.27 XSS refactor,
                // console showed "selectFileInBrowser is not defined"). Now
                // opens the context menu on plain click for touch parity.
                label.addEventListener('click', (ev) => { ev.preventDefault(); ev.stopPropagation(); openFileContextMenu(file, ev.clientX, ev.clientY); });
            }
            const delBtn = document.createElement('button');
            delBtn.className = 'danger';
            delBtn.style.cssText = 'padding:2px 6px;font-size:10px;';
            delBtn.textContent = 'Del';
            delBtn.addEventListener('click', (e) => { e.stopPropagation(); deleteBrowserFile(file.path); });
            item.appendChild(label);
            item.appendChild(delBtn);

            // v4.33: desktop right-click + mobile long-press both open a
            // context menu for the file (Load / Copy path / Download /
            // Delete / Rename).
            item.addEventListener('contextmenu', (ev) => {
                ev.preventDefault();
                openFileContextMenu(file, ev.clientX, ev.clientY);
            });
            let __holdT = null, __holdFired = false;
            item.addEventListener('touchstart', (ev) => {
                __holdFired = false;
                const t = ev.touches[0];
                __holdT = setTimeout(() => {
                    __holdFired = true;
                    openFileContextMenu(file, t.clientX, t.clientY);
                }, 550);
            }, { passive: true });
            const cancelHold = () => { if (__holdT) { clearTimeout(__holdT); __holdT = null; } };
            item.addEventListener('touchmove', cancelHold, { passive: true });
            item.addEventListener('touchend', (ev) => { cancelHold(); if (__holdFired) { ev.preventDefault(); } });
            item.addEventListener('touchcancel', cancelHold, { passive: true });

            browser.appendChild(item);
        });
    });
}

// v4.33: file context menu (right-click on desktop, long-press on mobile).
// Actions: Load into editor, Copy path, Download, Rename, Delete.
// Old inline `selectFileInBrowser` call was undefined - this replaces it
// with a real menu the user can act on.
function openFileContextMenu(file, x, y) {
    // Close any existing menu.
    const existing = document.getElementById('fileCtxMenu');
    if (existing) existing.remove();
    const menu = document.createElement('div');
    menu.id = 'fileCtxMenu';
    menu.style.cssText =
        'position:fixed;z-index:99999;background:#1e1e28;color:#fff;border-radius:10px;' +
        'box-shadow:0 12px 40px rgba(0,0,0,0.5);border:1px solid rgba(255,255,255,0.1);' +
        'min-width:200px;padding:6px 0;font-size:13px;';
    // Anchor within viewport.
    const vw = window.innerWidth, vh = window.innerHeight;
    menu.style.left = Math.min(x, vw - 220) + 'px';
    menu.style.top  = Math.min(y, vh - 260) + 'px';

    const header = document.createElement('div');
    header.style.cssText = 'padding:8px 12px;font-family:monospace;font-size:11px;opacity:0.7;border-bottom:1px solid rgba(255,255,255,0.1);word-break:break-all;';
    header.textContent = file.path;
    menu.appendChild(header);

    const mkItem = (label, handler, danger) => {
        const it = document.createElement('div');
        it.textContent = label;
        it.style.cssText = 'padding:8px 14px;cursor:pointer;' + (danger ? 'color:#f55;' : '');
        it.addEventListener('mouseover', () => it.style.background = 'rgba(76,175,80,0.15)');
        it.addEventListener('mouseout',  () => it.style.background = '');
        it.addEventListener('click', () => { menu.remove(); document.removeEventListener('click', closeOnce, true); handler(); });
        menu.appendChild(it);
    };

    if (!file.isDirectory) {
        mkItem('Open in editor',   () => {
            fetch('/api/download?path=' + encodeURIComponent(file.path))
                .then(r => r.ok ? r.text() : Promise.reject('HTTP ' + r.status))
                .then(txt => {
                    const sa = document.getElementById('scriptArea');
                    if (sa) { sa.value = txt; if (typeof openTab === 'function') openTab(null, 'Script'); updateGutter(); updateErrorLens(); }
                })
                .catch(err => alert('Open failed: ' + err));
        });
        mkItem('Copy path',        () => { navigator.clipboard.writeText(file.path).catch(() => {}); });
        mkItem('Download',         () => { window.open('/api/download?path=' + encodeURIComponent(file.path), '_blank'); });
        mkItem('Rename…',          () => {
            const n = prompt('New name for ' + file.name + ':', file.name);
            if (!n || n === file.name) return;
            const newPath = file.path.substring(0, file.path.lastIndexOf('/') + 1) + n;
            fetch('/api/rename', { method: 'POST', headers: {'Content-Type':'application/json'},
                                   body: JSON.stringify({ from: file.path, to: newPath }) })
                .then(r => r.ok ? refreshFileBrowser() : r.text().then(t => alert('Rename failed: ' + t)))
                .catch(err => alert('Rename failed: ' + err));
        });
    } else {
        mkItem('Open folder',      () => navigateToDirectory(file.path));
    }
    mkItem('Delete', () => {
        if (confirm('Delete "' + file.name + '"?')) deleteBrowserFile(file.path);
    }, true);

    const closeOnce = (e) => {
        if (menu.contains(e.target)) return;
        menu.remove();
        document.removeEventListener('click', closeOnce, true);
    };
    setTimeout(() => document.addEventListener('click', closeOnce, true), 0);
    document.body.appendChild(menu);
}

function executeScript() {
    const script = document.getElementById('scriptArea').value.trim();
    if (!script) return;
    const statusEl = document.getElementById('scriptStatus');
    statusEl.textContent = 'Executing...';
    fetch('/execute', { method: 'POST', body: script }).then(r => { statusEl.textContent = r.ok ? 'Script Finished' : 'Execution Failed'; });
}

function stopScript() { fetch('/stop', { method: 'POST' }); }
function clearScript() { document.getElementById('scriptArea').value = ''; updateGutter(); updateErrorLens(); scriptChanged = true; }
function loadFile(f) { fetch('/api/load?file='+encodeURIComponent(f)).then(r=>r.text()).then(c => { document.getElementById('scriptArea').value = c; openTab(null, 'Script'); updateGutter(); updateErrorLens(); scriptChanged = false; }); }
function deleteFile(f) { if(confirm('Delete?')) fetch('/api/delete?file='+encodeURIComponent(f), {method:'DELETE'}).then(()=>refreshFiles()); }
function saveScriptPrompt() { const n = prompt('Name:'); if(n) saveScriptAs(n); }
function saveScriptAs(n) { fetch('/api/save', {method:'POST', body:JSON.stringify({filename:n.endsWith('.txt')?n:n+'.txt', content:document.getElementById('scriptArea').value})}).then(()=>{scriptChanged = false; refreshFiles();}); }
function saveScript() { const n = document.getElementById('newFilename').value; if(n) saveScriptAs(n); }
function navigateToDirectory(path) { currentBrowserPath = path; refreshFileBrowser(); }
function goToParent() { if (currentBrowserPath === '/') return; const p = currentBrowserPath.split('/'); p.pop(); currentBrowserPath = p.join('/') || '/'; refreshFileBrowser(); }
function initFileManager() { document.getElementById('uploadArea').onclick = () => document.getElementById('fileInput').click(); document.getElementById('fileInput').onchange = (e) => Array.from(e.target.files).forEach(uploadFile); }
function uploadFile(file) {
    const form = new FormData();
    form.append('file', file, currentBrowserPath + (currentBrowserPath.endsWith('/') ? '' : '/') + file.name);
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/upload');
    xhr.onload = () => refreshFileBrowser();
    xhr.send(form);
}
function updateStats() {
    const autoRetry = localStorage.getItem('autoRetryConn') !== 'false';
    const retryCheckbox = document.getElementById('retryConnToggle');
    if (retryCheckbox && !retryCheckbox.checked) return; // Respect popup checkbox

    if (statsController) statsController.abort();
    statsController = new AbortController();
    
    fetch('/api/stats', { signal: statsController.signal })
        .then(r => r.json())
        .then(data => {
        const set = (id, val) => { const el = document.getElementById(id); if (el) el.textContent = val; };
        // Firmware-driven UI hiding: /api/stats may include a
        // `hidden_settings` array (["led"], ["led","com"], ...). Any
        // element with a `data-setting` attribute matching one of the
        // entries is hidden. Lets a single dashboard serve every
        // firmware variant (Key, devkitc, Watch) — each firmware just
        // returns its own list.
        if (Array.isArray(data.hidden_settings)) {
            const hide = new Set(data.hidden_settings);
            document.querySelectorAll('[data-setting]').forEach(el => {
                const key = el.getAttribute('data-setting');
                el.style.display = hide.has(key) ? 'none' : '';
            });
        }
        set('errorCount', data.errorCount);
        set('totalScripts', data.totalScripts);
        set('totalCommands', data.totalCommands);
        set('clientCount', data.clientCount);
        set('detectedOS', data.detectedOS);
        set('uptime', data.uptime + 's');
        set('freeMemory', Math.round(data.freeMemory / 1024) + ' KB');
        set('lastError', data.lastError || 'None');
        // Live tab warning if HID has been disabled by ATTACKMODE
        const ls = document.getElementById('liveStatus');
        if (ls && data.attackMode && !data.attackMode.hid) {
            ls.textContent = '⚠ HID disabled by ATTACKMODE - set ATTACKMODE HID first';
            ls.style.color = 'var(--danger)';
        }
        // ATTACKMODE chip: e.g. "HID+MSC 046D:C31C  22G"
        const amEl = document.getElementById('attackModeChip');
        if (amEl && data.attackMode) {
            const am = data.attackMode;
            let bits = [];
            if (am.hid)     bits.push('HID');
            if (am.storage) bits.push('MSC');
            const parts = [bits.join('+') || 'OFF'];
            if (am.vid && am.pid) parts.push(am.vid.replace('0x','').toUpperCase() + ':' + am.pid.replace('0x','').toUpperCase());
            if (am.sizeBytes && am.sizeBytes > 0) {
                const gb = am.sizeBytes / (1024*1024*1024);
                parts.push(gb >= 1 ? gb.toFixed(1) + 'G' : Math.round(am.sizeBytes / (1024*1024)) + 'M');
            }
            amEl.textContent = parts.join('  ');
        }

        // Update WiFi Status
        const wifiStatusEl = document.getElementById('wifiStatus');
        window.isWifiConnected = data.wifiConnected;
        if (wifiStatusEl) {
            if (data.wifiConnected) {
                const uiSSID = document.getElementById('wifiSSIDJoin').value;
                const isSynced = (uiSSID === data.staSSID);
                wifiStatusEl.textContent = `Connected: ${data.staSSID} ${isSynced ? '(Synced)' : '(Modified)'}`;
                wifiStatusEl.style.color = isSynced ? 'var(--primary)' : '#ff9800'; // Orange for modified
            } else {
                wifiStatusEl.textContent = 'Status: Idle';
                wifiStatusEl.style.color = 'var(--text-muted)';
            }
        }

        // Update toggles even if hidden
        // Synchronize Settings (Toggles)
        const toggles = {
            'ledToggle': data.ledEnabled,
            'loggingToggle': data.loggingEnabled,
            'btToggle': data.btToggleEnabled,
            'btDiscoveryToggle': data.btDiscoveryEnabled
        };
        for (const [id, val] of Object.entries(toggles)) {
            const el = document.getElementById(id);
            if (el) el.checked = (val === true);
        }

        // Sync new WiFi/connect toggles
        const autoConn = document.getElementById('autoConnectToggle');
        if (autoConn) autoConn.checked = (data.autoConnectEnabled === true);
        const saveCred = document.getElementById('saveCredToggle');
        if (saveCred) saveCred.checked = (data.saveOnConnectEnabled === true);

        // Auto-discover languages if not done yet
        if (!languagesDiscovered) {
            discoverLanguages(data.currentLanguage);
        } else {
            const select = document.getElementById('languageSelect');
            if (select && document.activeElement !== select) {
                const targetVal = (data.currentLanguage || 'us').toLowerCase();
                if (select.value !== targetVal) select.value = targetVal;
            }
        }
    }).catch(err => {
        if (err.name !== 'AbortError') {
            console.error("Stats Fetch Error:", err);
            showConnectionError();
        }
    });
}

function showConnectionError() {
    if (sessionStorage.getItem('hideConnectionError') === 'true') return;
    const el = document.getElementById('connectionError');
    if (el) el.style.display = 'flex';
}

function hideConnectionError() {
    const el = document.getElementById('connectionError');
    const checkbox = document.getElementById('dontShowConnError');
    if (checkbox && checkbox.checked) {
        sessionStorage.setItem('hideConnectionError', 'true');
    }
    if (el) el.style.display = 'none';
}

function retryConnection() {
    console.log("Retrying connection...");
    hideConnectionError();
    updateStats();
    // Also try to refresh files if we were in that tab
    if (typeof refreshFiles === 'function') refreshFiles();
}

let discoveryRetryCount = 0;
function discoverLanguages(currentLang) {
    console.log("Discovering languages...");
    fetch('/api/languages')
        .then(r => {
            if (!r.ok) throw new Error("HTTP " + r.status);
            return r.json();
        })
        .then(langs => {
            const select = document.getElementById('languageSelect');
            if (!select) return;
            
            if (langs.length > 0) {
                languagesDiscovered = true;
                select.innerHTML = '';
                langs.forEach(l => {
                    const opt = document.createElement('option');
                    opt.value = l.toLowerCase();
                    opt.textContent = l.toUpperCase();
                    select.appendChild(opt);
                });
                
                const target = (currentLang || 'us').toLowerCase();
                select.value = target;
            } else if (discoveryRetryCount < 3) {
                discoveryRetryCount++;
                setTimeout(() => discoverLanguages(currentLang), 2000);
            }
        }).catch(err => {
            console.error("Language discovery failed:", err);
            if (discoveryRetryCount < 3) {
                discoveryRetryCount++;
                setTimeout(() => discoverLanguages(currentLang), 2000);
            }
        });
}

function initSettingsTab() {
    fetch('/api/stats').then(r => r.json()).then(data => {
        // USB Settings
        document.getElementById('usbVID').value = data.usbVID || '';
        document.getElementById('usbPID').value = data.usbPID || '';
        document.getElementById('usbMfr').value = data.usbMfr || '';
        document.getElementById('usbProd').value = data.usbProd || '';
        document.getElementById('usbRndVID').checked = data.usbRndVID;
        document.getElementById('usbRndPID').checked = data.usbRndPID;
        
        // System Toggles
        document.getElementById('ledToggle').checked = (data.ledEnabled === true);
        document.getElementById('loggingToggle').checked = (data.loggingEnabled === true);
        document.getElementById('btToggle').checked = (data.btToggleEnabled === true);
        const btDisc = document.getElementById('btDiscoveryToggle');
        if (btDisc) btDisc.checked = (data.btDiscoveryEnabled === true);
        const silent = document.getElementById('silentToggle');
        if (silent) silent.checked = (data.silentStartup === true);
        updateSilentIndicator(data.silentStartup === true);

        // AP Settings
        document.getElementById('wifiSSID').value = data.wifiSSID || '';
        document.getElementById('wifiPassword').value = data.wifiPassword || '';
        document.getElementById('wifiScanTime').value = data.wifiScanTime || 5000;
        
        handleRandomToggle();
    }).catch(err => {
        console.error("Settings Tab Init Failed:", err);
        alert("Failed to load settings from ESP32. Please check connection.\nError: " + err.message);
    });
}
function toggleLED() { 
    const enabled = document.getElementById('ledToggle').checked;
    fetch('/api/toggle-led', {
        method:'POST', 
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({enabled: enabled})
    }); 
}
function toggleLogging() { fetch('/api/toggle-logging', {method:'POST', body:JSON.stringify({enabled:document.getElementById('loggingToggle').checked})}); }
function toggleWiFi() { fetch('/api/toggle-wifi', {method:'POST', body:JSON.stringify({enabled:document.getElementById('wifiToggle').checked})}); }
function toggleBluetooth() { fetch('/api/toggle-bluetooth', {method:'POST', body:JSON.stringify({enabled:document.getElementById('btToggle').checked})}); }
function toggleBluetoothDiscovery() { fetch('/api/toggle-bt-discovery', {method:'POST', body:JSON.stringify({enabled:document.getElementById('btDiscoveryToggle').checked})}); }
function toggleSilentStartup() {
    const on = document.getElementById('silentToggle').checked;
    fetch('/api/set-silent', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({enabled:on})})
        .then(r=>r.text()).then(msg=>{ if (on) alert('Silent Startup enabled.\nThe HID keyboard will no longer appear at boot - it attaches only while typing, then detaches.'); });
}
function toggleAutoConnect() { fetch('/api/toggle-autoconnect', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({enabled:document.getElementById('autoConnectToggle').checked})}); }
function toggleSaveOnConnect() { fetch('/api/toggle-save-on-connect', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({enabled:document.getElementById('saveCredToggle').checked})}); }

function toggleSavedNetworksPanel() {
    const panel = document.getElementById('savedNetworksPanel');
    const isHidden = panel.style.display === 'none' || !panel.style.display;
    panel.style.display = isHidden ? 'block' : 'none';
    if (isHidden) loadSavedNetworks();
}

function loadSavedNetworks() {
    const list = document.getElementById('savedNetworksList');
    if (list) list.innerHTML = 'Loading...';
    fetch('/api/saved-wifi').then(r => r.json()).then(networks => {
        if (!list) return;
        if (!networks || networks.length === 0) {
            list.innerHTML = '<div style="color:var(--text-muted); padding:8px;">No saved networks</div>';
            return;
        }
        list.innerHTML = '';
        networks.forEach(net => {
            const item = document.createElement('div');
            item.className = 'file-item';
            item.innerHTML = `<span style="flex:1">${net.ssid}</span><button onclick="connectToSaved('${net.ssid}','${net.pass}')" style="font-size:11px;padding:3px 8px;">Connect</button><button class="danger" onclick="deleteSavedNetwork('${net.ssid}')" style="font-size:11px;padding:3px 8px;">✕</button>`;
            list.appendChild(item);
        });
    }).catch(() => { if (list) list.innerHTML = 'Failed to load'; });
}

function connectToSaved(ssid, password) {
    const statusEl = document.getElementById('wifiStatus');
    if (statusEl) statusEl.textContent = 'Connecting to ' + ssid + '...';
    fetch('/api/join-internet', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ssid, password})
    }).then(r => r.json()).then(res => { if (res.status === 'connecting') pollWiFiJoinStatus(); });
}

function deleteSavedNetwork(ssid) {
    if (!confirm('Delete saved network: ' + ssid + '?')) return;
    fetch('/api/delete-saved-wifi', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ssid})
    }).then(() => loadSavedNetworks());
}

function quickSetBootScript() {
    const name = document.getElementById('quickBootScript').value.trim();
    if (!name) return alert('Enter a filename');
    const filename = name.endsWith('.txt') ? name : name + '.txt';
    fetch('/api/set-boot-script', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({filenames: [filename]})
    }).then(r => r.text()).then(msg => alert(msg));
}
function handleRandomToggle() {
    const rndVID = document.getElementById('usbRndVID').checked;
    const rndPID = document.getElementById('usbRndPID').checked;
    const vidInput = document.getElementById('usbVID');
    const pidInput = document.getElementById('usbPID');
    if (vidInput) { vidInput.disabled = rndVID; vidInput.style.opacity = rndVID ? '0.5' : '1'; }
    if (pidInput) { pidInput.disabled = rndPID; pidInput.style.opacity = rndPID ? '0.5' : '1'; }
}

function saveUSBSettings() {
    const data = { vid: document.getElementById('usbVID').value, pid: document.getElementById('usbPID').value, rndVid: document.getElementById('usbRndVID').checked, rndPid: document.getElementById('usbRndPID').checked, mfr: document.getElementById('usbMfr').value, prod: document.getElementById('usbProd').value };
    fetch('/api/save-usb', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(data)}).then(()=>alert('Rebooting...'));
}
function saveLanguageSettings() {
    const lang = document.getElementById('languageSelect').value;
    fetch('/api/save-settings', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ type: 'language', language: lang })
    }).then(r => r.text()).then(msg => alert(msg));
}

function saveWiFiSettings() {
    const data = {
        ssid: document.getElementById('wifiSSID').value,
        password: document.getElementById('wifiPassword').value,
        scanTime: parseInt(document.getElementById('wifiScanTime').value)
    };
    fetch('/api/save-wifi', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(data)
    }).then(r => r.text()).then(msg => alert(msg));
}
function refreshTasks() {
    fetch('/api/tasks').then(r => r.json()).then(tasks => {
        const list = document.getElementById('taskList');
        if (!list) return;
        list.innerHTML = tasks.length ? '' : '<div class="file-item">No active background tasks</div>';
        tasks.forEach(t => {
            list.innerHTML += `<div class="file-item"><span>${t.description}</span><button class="danger" onclick="cancelTask(${t.id})">Cancel</button></div>`;
        });
        // Check for active delay via stats
        fetch('/api/stats').then(r => r.json()).then(data => {
            if (data.delayProgress > 0) {
                const secs = Math.round(data.delayTotal / 1000);
                list.innerHTML = `<div class="file-item" style="flex-direction:column; gap:6px;"><span>⏳ DELAY - ${secs}s total</span><div style="width:100%;height:6px;background:var(--glass-border);border-radius:3px;"><div style="height:100%;width:${data.delayProgress}%;background:var(--primary);border-radius:3px;transition:width 0.5s"></div></div></div>` + list.innerHTML;
            }
        }).catch(() => {});
    });
}
function cancelTask(id) { fetch('/api/cancel-task', {method:'POST', body:JSON.stringify({id})}).then(() => refreshTasks()); }
function toggleHelp(id) { const el = document.getElementById(id); el.style.display = el.style.display === 'none' ? 'block' : 'none'; }
function joinInternet() {
    const ssid = document.getElementById('wifiSSIDJoin').value;
    const password = document.getElementById('wifiPasswordJoin').value;
    if (!ssid) return alert('SSID required');
    
    const statusEl = document.getElementById('wifiStatus');
    if (statusEl) statusEl.textContent = 'Connecting...';
    
    fetch('/api/join-internet', { 
        method: 'POST', 
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ssid, password, save: document.getElementById('saveCredToggle')?.checked || false }) 
    }).then(r => r.json()).then(res => {
        if (res.status === 'connecting') {
            pollWiFiJoinStatus();
        } else {
            if (statusEl) statusEl.textContent = 'Failed to start connection';
        }
    });
}

function pollWiFiJoinStatus() {
    const statusEl = document.getElementById('wifiStatus');
    fetch('/api/wifi-join-status?t=' + Date.now()).then(r => r.json()).then(res => {
        if (res.status === 'connected') {
            statusEl.textContent = `Connected! IP: ${res.ip}`;
            statusEl.style.color = 'var(--primary)';
            updateStats(); // Refresh everything
        } else if (res.status === 'connecting') {
            statusEl.textContent = 'Still connecting...';
            setTimeout(pollWiFiJoinStatus, 2000);
        } else {
            statusEl.textContent = 'Connection Failed / Idle';
            statusEl.style.color = 'var(--danger)';
        }
    });
}

function leaveInternet() {
    fetch('/api/leave-internet', { method: 'POST' }).then(() => {
        const statusEl = document.getElementById('wifiStatus');
        if (statusEl) {
            statusEl.textContent = 'Disconnected';
            statusEl.style.color = 'var(--text-muted)';
        }
        updateStats();
    });
}

function scanNetworksForUI(btn) {
    btn.disabled = true;
    btn.originalText = btn.textContent;
    btn.textContent = 'Starting Scan...';
    
    fetch('/api/scan-wifi').then(r => r.json()).then(res => {
        if (res.status === 'scanning') {
            btn.textContent = 'Scanning...';
            const container = document.getElementById('wifiScanResults');
            if (container) {
                container.style.display = 'block';
                container.innerHTML = '<div style="padding: 10px; text-align: center;">Scanning for networks...</div>';
            }
            pollScanResults(btn);
        } else {
            btn.textContent = 'Scan Failed';
            setTimeout(() => {
                btn.textContent = btn.originalText;
                btn.disabled = false;
            }, 2000);
        }
    }).catch(() => {
        btn.textContent = 'Connection Error';
        setTimeout(() => {
            btn.textContent = btn.originalText;
            btn.disabled = false;
        }, 2000);
    });
}

function pollScanResults(btn) {
    fetch('/api/scan-results').then(r => r.json()).then(res => {
        if (res.done) {
            btn.disabled = false;
            btn.textContent = btn.originalText;
            displayScanResults(res.networks);
        } else {
            setTimeout(() => pollScanResults(btn), 1000);
        }
    }).catch(() => {
        btn.disabled = false;
        btn.textContent = btn.originalText;
    });
}

function displayScanResults(networks) {
    const container = document.getElementById('wifiScanResults');
    if (!container) return;
    
    if (!networks || networks.length === 0) {
        container.innerHTML = '<div style="padding: 10px; text-align: center;">No networks found</div>';
        return;
    }

    // Sort by RSSI (strongest first)
    networks.sort((a, b) => b.rssi - a.rssi);

    // Group by SSID to detect Router/Repeater setups
    const ssidGroups = {};
    networks.forEach(n => {
        const id = n.ssid || '[Hidden]';
        if (!ssidGroups[id]) ssidGroups[id] = [];
        ssidGroups[id].push(n);
    });

    let html = '';
    networks.forEach(net => {
        const ssid = net.ssid || '[Hidden]';
        // If it's a saved network, show unlocked even if it has encryption, because we have the key
        const lock = (net.saved || net.encryption === 7) ? '🔓' : '🔒'; 
        
        // Router/Repeater logic
        let typeBadge = '';
        if (ssidGroups[ssid].length > 1) {
            // If multiple APs have same SSID, the strongest is usually the repeater/closest node
            const strongestBssid = ssidGroups[ssid][0].bssid; // Sorted strongest first above
            if (net.bssid === strongestBssid) {
                typeBadge = '<span style="font-size: 9px; background: var(--primary); color: black; padding: 1px 4px; border-radius: 3px; margin-left: 5px; vertical-align: middle;">REPEATER/CLOSEST</span>';
            } else {
                typeBadge = '<span style="font-size: 9px; background: rgba(255,255,255,0.1); color: var(--text-muted); padding: 1px 4px; border-radius: 3px; margin-left: 5px; vertical-align: middle;">ROUTER/NODE</span>';
            }
        }

        // Saved network badge
        const savedBadge = net.saved ? '<span style="font-size: 9px; background: #4CAF50; color: white; padding: 1px 4px; border-radius: 3px; margin-left: 5px; vertical-align: middle;">SAVED</span>' : '';

        // v4.33: real signal-strength bars (5-bar ladder mapped from RSSI):
        //   >=-55 dBm  -> 5 bars   (excellent)
        //   -55..-65   -> 4 bars   (good)
        //   -65..-75   -> 3 bars   (fair)
        //   -75..-85   -> 2 bars   (weak)
        //   <-85       -> 1 bar    (very weak)
        const bars = getSignalBarsHtml(net.rssi);

        html += `
            <div class="file-item" onclick="selectWiFiNetwork('${ssid.replace(/'/g, "\\'")}')">
                <div class="flex-row" style="justify-content: space-between; width: 100%;">
                    <div class="flex-row" style="gap: 5px; overflow: hidden;">
                        <span style="font-weight: 500; white-space: nowrap; overflow: hidden; text-overflow: ellipsis;">${ssid}</span>
                        ${typeBadge}
                        ${savedBadge}
                    </div>
                    <div class="flex-row" style="gap: 8px; font-size: 11px; color: var(--text-muted); flex-shrink: 0;">
                        ${bars}
                        <span>${lock}</span>
                        <span>${net.rssi} dBm</span>
                        <span style="font-size: 9px; opacity: 0.5;">CH ${net.channel}</span>
                    </div>
                </div>
            </div>`;
    });
    container.innerHTML = html;
}

function getSignalBarsHtml(rssi) {
    let count;
    if      (rssi >= -55) count = 5;
    else if (rssi >= -65) count = 4;
    else if (rssi >= -75) count = 3;
    else if (rssi >= -85) count = 2;
    else                  count = 1;
    let s = '<span class="wifi-bars" aria-label="' + count + '/5 bars">';
    for (let i = 1; i <= 5; i++) s += '<span class="bar' + (i <= count ? ' on' : '') + '"></span>';
    s += '</span>';
    return s;
}

function getSignalIcon(rssi) {
    return ''; // Emoji removed as requested
}

function selectWiFiNetwork(ssid) {
    const ssidInput = document.getElementById('wifiSSIDJoin');
    const passInput = document.getElementById('wifiPasswordJoin');
    if (ssidInput) {
        ssidInput.value = ssid;
        passInput.focus();
        // Optional: scroll to inputs
        ssidInput.scrollIntoView({ behavior: 'smooth', block: 'center' });
    }
}

// =========================================================
// Autocomplete System
// =========================================================
let autocompletePopup = null;
let autocompleteSuggestions = [];
let autocompleteIndex = -1;

function initAutocomplete() {
    autocompletePopup = document.createElement('div');
    autocompletePopup.id = 'autocompletePopup';
    autocompletePopup.className = 'autocomplete-popup';
    document.body.appendChild(autocompletePopup);

    const scriptArea = document.getElementById('scriptArea');
    if (!scriptArea) return;

    scriptArea.addEventListener('input', handleAutocompleteInput);
    scriptArea.addEventListener('keydown', handleAutocompleteKeydown);
    
    document.addEventListener('click', (e) => {
        if (e.target !== scriptArea && !autocompletePopup.contains(e.target)) {
            hideAutocomplete();
        }
    });
}

// Argument-suggestion maps for multi-token commands. The autocomplete popup
// pops up after "<CMD> " and every subsequent partial token, so users get
// hints for e.g. `ATTACKMODE B|` → BLANK, or `LOCALE d|` → de.
//
// v4.27: EXTENSION / RUN_EXTENSION / IMPORT get populated at runtime from
// /api/list-extensions so a user typing `EXTENSION os|` sees the actual
// extension filenames on the SD (was previously "no autocomplete for
// extensions"). Pool is refreshed whenever refreshExtensions() runs and on
// first-use so the Script tab picks up new SDs / uploads without reload.
const ARG_SUGGESTIONS = {
    'ATTACKMODE': [
        'HID', 'STORAGE', 'HID STORAGE', 'STORAGE_ONLY', 'MSC_ONLY',
        'NO_HID', 'HID_OFF', 'OFF',
        'BLANK', 'NONE', 'UNMOUNT',
        'VID_046D', 'VID_1D6B', 'VID_303A',
        'PID_C31C', 'PID_0002',
        'SIZE_8_GB', 'SIZE_16_GB', 'SIZE_32_GB', 'SIZE_64_GB',
        'SIZE_100_MB', 'SIZE_500_MB',
    ],
    'LOCALE':          ['us', 'de', 'fr', 'es', 'it', 'uk'],
    'IF_OS':           ['WINDOWS', 'LINUX', 'MAC', 'ANDROID', 'IOS'],
    'LED':             ['ON', 'OFF', 'BLINK', 'STOP', '255 0 0', '0 255 0', '0 0 255'],
    'INJECT_MOD':      ['CTRL', 'SHIFT', 'ALT', 'GUI', 'WINDOWS', 'COMMAND',
                        'RIGHT_CTRL', 'RIGHT_SHIFT', 'RIGHT_ALT', 'RIGHT_GUI', 'ALTGR'],
    'EXTENSION':       [],   // populated by refreshExtensionAutocompletePool()
    'RUN_EXTENSION':   [],
    'IMPORT':          [],
    'END_EXTENSION':   [],
};

// v4.27: cache of extension filenames for the autocomplete pool. Refreshed
// whenever the Extensions tab lists (via refreshExtensions) and lazily on
// first Script-tab keystroke that hits an EXTENSION-family token.
// v4.31: also caches each extension's FUNCTION names in __extFunctionsByStem
// (stem -> ['DETECT_OS', 'HELLO_OS', ...]) so the Script-tab autocomplete
// can surface those callables IFF the current script references the ext.
let __extNamePool = [];
let __extNamePoolLoading = false;
let __extNamePoolRefetch = false;   // v4.27 bug-hunt HIGH #6: coalesce
let __extFunctionsByStem = {};      // v4.31: stem -> [functionName, ...]
function refreshExtensionAutocompletePool() {
    if (__extNamePoolLoading) {
        // A refresh is already in flight - mark for a follow-up so a Pull /
        // Save / Delete that arrives mid-fetch still ends up in the pool.
        __extNamePoolRefetch = true;
        return;
    }
    __extNamePoolLoading = true;
    fetch('/api/list-extensions').then(r => r.json()).then(async data => {
        const all = []
            .concat(Array.isArray(data.hak5)   ? data.hak5.map(f => ({...f, folder:'hak5'}))     : [])
            .concat(Array.isArray(data.custom) ? data.custom.map(f => ({...f, folder:'custom'})) : [])
            .concat(Array.isArray(data.legacy) ? data.legacy.map(f => ({...f, folder:''}))       : []);
        // Offer both the full filename ("os_detect.txt") and the bare stem
        // ("os_detect") so `EXTENSION os_d|` completes to whatever the user
        // is typing toward.
        const names = new Set();
        for (const f of all) {
            if (!f || !f.name) continue;
            names.add(f.name);
            const stem = f.name.replace(/\.(txt|ext|dsx|dd)$/i, '');
            if (stem && stem !== f.name) names.add(stem);
        }
        __extNamePool = Array.from(names).sort();
        // Rebind the ARG_SUGGESTIONS slots so the handler picks them up.
        ARG_SUGGESTIONS['EXTENSION']     = __extNamePool;
        ARG_SUGGESTIONS['RUN_EXTENSION'] = __extNamePool;
        ARG_SUGGESTIONS['IMPORT']        = __extNamePool;
        ARG_SUGGESTIONS['END_EXTENSION'] = __extNamePool;

        // v4.31: fetch each ext body once, extract FUNCTION names, cache under
        // the bare stem AND the full filename. Small (~110KB total across the
        // Hak5 corpus), done in parallel, best-effort - if any fetch fails we
        // just skip that one. The cache is used by handleAutocompleteInput to
        // surface `DETECT_OS`/`HELLO_OS`/etc. ONLY when the current script
        // references the extension that defines them.
        // v4.34 bug-hunt HIGH #7: throttle to 3 concurrent /api/load-extension
        // requests instead of one Promise.all() burst. On a large corpus the
        // ESP32 WebServer's tiny handle table would exhaust and starve other
        // UI polling (/status, refreshFiles). Simple chunked walk.
        const nextMap = {};
        const CONCURRENCY = 3;
        const scanBody = (f, txt) => {
            const funcs = new Set();
            for (const raw of txt.split('\n')) {
                const line = raw.trim();
                if (!line || line.startsWith('REM') || line.startsWith('//')) continue;
                const m = line.match(/^FUNCTION\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(?\)?\s*$/i);
                if (m) funcs.add(m[1].toUpperCase());
            }
            const arr = Array.from(funcs);
            const stem = f.name.replace(/\.(txt|ext|dsx|dd)$/i, '');
            if (arr.length) {
                nextMap[f.name.toUpperCase()] = arr;
                if (stem && stem !== f.name) nextMap[stem.toUpperCase()] = arr;
            }
        };
        for (let i = 0; i < all.length; i += CONCURRENCY) {
            const chunk = all.slice(i, i + CONCURRENCY);
            await Promise.all(chunk.map(f => {
                let url = '/api/load-extension?name=' + encodeURIComponent(f.name);
                if (f.folder) url += '&folder=' + encodeURIComponent(f.folder);
                return fetch(url).then(r => r.ok ? r.text() : '').then(txt => { if (txt) scanBody(f, txt); }).catch(() => {});
            }));
        }
        __extFunctionsByStem = nextMap;
    }).catch(() => { /* silent: keep whatever pool we had */ })
      .finally(() => {
          __extNamePoolLoading = false;
          if (__extNamePoolRefetch) {
              __extNamePoolRefetch = false;
              refreshExtensionAutocompletePool();
          }
      });
}

// v4.31: scan the current editor value for every `EXTENSION <name>`
// reference (any of ˅ / ^ / bare) and return the union of FUNCTION names
// the referenced extensions define, using the cached __extFunctionsByStem.
// Used by handleAutocompleteInput to include only in-use extension callables.
function _autocompleteFunctionsFromReferencedExtensions(fullText) {
    const out = new Set();
    if (!fullText) return out;
    const re = /^\s*EXTENSION\s+([A-Za-z0-9_.\-]+)(?:\s+(?:\^|˅))?\s*$/gm;
    let m;
    while ((m = re.exec(fullText)) !== null) {
        const key = m[1].toUpperCase();
        const stem = key.replace(/\.(TXT|EXT|DSX|DD)$/i, '');
        const arr = __extFunctionsByStem[key] || __extFunctionsByStem[stem];
        if (arr) for (const fn of arr) out.add(fn);
    }
    return out;
}
// Prime the pool at first paint so the Script tab has it before the user
// even opens the Extensions tab.
if (typeof window !== 'undefined') {
    window.addEventListener('load', refreshExtensionAutocompletePool);
}

function handleAutocompleteInput(e) {
    const scriptArea = e.target;
    const text = scriptArea.value;
    const cursor = scriptArea.selectionStart;

    const lastNewline = text.lastIndexOf('\n', cursor - 1);
    const lineStart = lastNewline === -1 ? 0 : lastNewline + 1;
    const currentLine = text.substring(lineStart, cursor);

    const trimmed = currentLine.trimStart();
    const words = trimmed.split(/\s+/);
    let firstWord = (words[0] || '').toUpperCase();
    // v4.27 bug-hunt MEDIUM #10: DuckyScript accepts `IF:`, `FOR:`,
    // `ATTACKMODE:` and other trailing-colon forms; strip it before the
    // ARG_SUGGESTIONS lookup so `ATTACKMODE: HID |` still suggests args.
    if (firstWord.endsWith(':')) firstWord = firstWord.slice(0, -1);

    // ---- Argument-position autocomplete for known multi-token commands ----
    // Triggers when there IS a space in the current line and the first word
    // is one of ARG_SUGGESTIONS. The current partial (possibly empty) is the
    // last whitespace-separated token.
    if (trimmed.includes(' ') && ARG_SUGGESTIONS[firstWord]) {
        const partial = (words[words.length - 1] || '').toUpperCase();
        const pool = ARG_SUGGESTIONS[firstWord];
        autocompleteSuggestions = pool
            .filter(s => partial === '' || s.toUpperCase().startsWith(partial))
            .filter(s => s.toUpperCase() !== partial);
        if (autocompleteSuggestions.length > 0) showAutocomplete(scriptArea);
        else hideAutocomplete();
        return;
    }

    if (trimmed.includes(' ')) {
        hideAutocomplete();
        return;
    }

    const word = trimmed.toUpperCase();
    if (word.length < 1) {
        hideAutocomplete();
        return;
    }

    // Dynamic scan for fresh suggestions
    const currentVars = new Set();
    const currentFuncs = new Set();
    text.split('\n').forEach(l => {
        const t = l.trim();
        const u = t.toUpperCase();
        const vM = t.match(/^(VAR|VARIABLE)\s+([a-zA-Z0-9_]+)/i);
        if (vM) currentVars.add(vM[2].toUpperCase());
        const vPM = u.match(/^(VAR_|VARIABLE_)([A-Z0-9_]+)/);
        if (vPM) currentVars.add((vPM[1] + vPM[2]).toUpperCase());
        const fM = t.match(/^(FUNCTION|DEF_)\s*([a-zA-Z0-9_]+)/i);
        if (fM) currentFuncs.add(fM[2].toUpperCase());
        const fPM = u.match(/^FUNCTION_([A-Z0-9_]+)/);
        if (fPM) currentFuncs.add(fPM[1]);
        const fLM = u.match(/^([A-Z0-9_]+)\(\):/);
        if (fLM) currentFuncs.add(fLM[1]);
    });

    // v4.31: also surface FUNCTION names from every EXTENSION the current
    // script references (collapsed `˅` or bare form). Inline-expanded
    // `^ ... END_EXTENSION` bodies are already picked up by currentFuncs
    // above - adding them here is safe (duplicates are filtered downstream).
    const extFuncs = _autocompleteFunctionsFromReferencedExtensions(text);

    const dynamicCommands = [
        ...Array.from(VALID_KEYWORDS),
        ...Array.from(currentVars),
        ...Array.from(currentFuncs),
        ...Array.from(extFuncs)
    ];

    autocompleteSuggestions = dynamicCommands.filter(c => c.startsWith(word) && c !== word);

    if (autocompleteSuggestions.length > 0) {
        showAutocomplete(scriptArea);
    } else {
        hideAutocomplete();
    }
}

function showAutocomplete(scriptArea) {
    const setting = document.getElementById('settingAutocomplete');
    if (setting && !setting.checked) return;

    // Elite Header
    autocompletePopup.innerHTML = '<div class="autocomplete-header">Suggestions</div>';
    const listContainer = document.createElement('div');
    listContainer.className = 'autocomplete-list';
    
    autocompleteSuggestions.forEach((sug, i) => {
        const item = document.createElement('div');
        item.className = 'autocomplete-item';
        item.textContent = sug;
        item.onmousedown = (e) => {
            e.preventDefault();
            const cursor = scriptArea.selectionStart;
            const lastNewline = scriptArea.value.lastIndexOf('\n', cursor - 1);
            const lineStart = lastNewline === -1 ? 0 : lastNewline + 1;
            applyAutocomplete(sug, scriptArea, lineStart, cursor);
        };
        listContainer.appendChild(item);
    });
    autocompletePopup.appendChild(listContainer);

    autocompleteIndex = 0;
    highlightAutocompleteItem();

    // Position FLOATING after the cursor
    const editorWrapper = scriptArea.closest('.editor-wrapper');
    if (editorWrapper) {
        const wrapperRect = editorWrapper.getBoundingClientRect();
        const cursorIdx = scriptArea.selectionStart;
        const textBeforeCursor = scriptArea.value.substr(0, cursorIdx);
        const lines = textBeforeCursor.split('\n');
        const currentLineIdx = lines.length - 1;
        const currentLineText = lines[currentLineIdx];
        
        // Calculate X offset based on text width + margin (3 chars approx 24px)
        const charWidth = 8.4; // Average for Consolas 14px
        const textWidth = getTextWidth(currentLineText, "14px Consolas");
        const xOffset = 15 + 40 + textWidth + (charWidth * 3); // Padding + Gutter + Width + 3 chars
        
        // Vertical position based on current line (22px height)
        const topOffset = wrapperRect.top + (currentLineIdx * 22) + 15;
        
        // Final bounds check to stay inside editor
        const leftPos = Math.min(wrapperRect.right - 200, wrapperRect.left + xOffset);
        
        autocompletePopup.style.top = `${topOffset}px`;
        autocompletePopup.style.left = `${leftPos}px`;
        autocompletePopup.style.right = 'auto';
        autocompletePopup.style.display = 'block';
    }
}

// Helper to calculate text width for precision positioning
function getTextWidth(text, font) {
    const canvas = getTextWidth.canvas || (getTextWidth.canvas = document.createElement("canvas"));
    const context = canvas.getContext("2d");
    context.font = font;
    const metrics = context.measureText(text);
    return metrics.width;
}

function hideAutocomplete() {
    if (autocompletePopup) {
        autocompletePopup.style.display = 'none';
        autocompleteSuggestions = [];
        autocompleteIndex = -1;
    }
}

function highlightAutocompleteItem() {
    const items = autocompletePopup.getElementsByClassName('autocomplete-item');
    for (let i = 0; i < items.length; i++) {
        if (i === autocompleteIndex) {
            items[i].classList.add('selected');
            items[i].scrollIntoView({ block: 'nearest' });
        } else {
            items[i].classList.remove('selected');
        }
    }
}

function handleAutocompleteKeydown(e) {
    if (autocompletePopup && autocompletePopup.style.display === 'block') {
        if (e.key === 'ArrowDown') {
            e.preventDefault();
            autocompleteIndex = (autocompleteIndex + 1) % autocompleteSuggestions.length;
            highlightAutocompleteItem();
        } else if (e.key === 'ArrowUp') {
            e.preventDefault();
            autocompleteIndex = (autocompleteIndex - 1 + autocompleteSuggestions.length) % autocompleteSuggestions.length;
            highlightAutocompleteItem();
        } else if (e.key === 'Enter' || e.key === 'Tab') {
            e.preventDefault();
            if (autocompleteIndex >= 0 && autocompleteIndex < autocompleteSuggestions.length) {
                const scriptArea = e.target;
                const cursor = scriptArea.selectionStart;
                const lastNewline = scriptArea.value.lastIndexOf('\n', cursor - 1);
                const lineStart = lastNewline === -1 ? 0 : lastNewline + 1;
                applyAutocomplete(autocompleteSuggestions[autocompleteIndex], scriptArea, lineStart, cursor);
            }
        } else if (e.key === 'Escape') {
            hideAutocomplete();
        }
    }
}

function applyAutocomplete(suggestion, scriptArea, lineStart, cursor) {
    const text = scriptArea.value;
    const before = text.substring(0, lineStart);
    const currentLine = text.substring(lineStart, cursor);
    const after = text.substring(cursor);

    // Argument position: the line already has words, replace only the LAST
    // partial token so `ATTACKMODE HID S|` + STORAGE → `ATTACKMODE HID STORAGE `.
    let newLine;
    if (currentLine.includes(' ')) {
        // Chop off the trailing partial token (whitespace-separated).
        const m = currentLine.match(/^(.*?)(\S*)$/);
        const head = m ? m[1] : currentLine;   // preserves trailing space if the user was between args
        newLine = head + suggestion + ' ';
    } else {
        // First-word position: original behaviour - replace the whole partial cmd.
        newLine = suggestion + ' ';
    }

    scriptArea.value = before + newLine + after;
    scriptArea.selectionStart = scriptArea.selectionEnd = lineStart + newLine.length;
    hideAutocomplete();
    // v4.27 bug-hunt HIGH #2: previously we followed updateErrorLens() with a
    // second overwrite that joined lines with a raw '\n' (which HTML collapses
    // to a space) - every editor row rendered as one long inline strip, cursor
    // sync broke, and errors painted by updateErrorLens were thrown away.
    // updateErrorLens already refreshes the highlights correctly, so drop the
    // overwrite.
    updateErrorLens();
    updateGutter();
}




function syncHighlightsScroll() {
    const scriptArea = document.getElementById('scriptArea');
    const highlights = document.getElementById('editorHighlights');
    const gutter = document.getElementById('editorGutter');
    if (scriptArea && highlights) {
        highlights.scrollTop = scriptArea.scrollTop;
        highlights.scrollLeft = scriptArea.scrollLeft;
    }
    if (scriptArea && gutter) {
        gutter.scrollTop = scriptArea.scrollTop;
    }
}

function setupCustomScrollbar(contentId, trackId, thumbId) {
    const content = document.getElementById(contentId);
    const track = document.getElementById(trackId);
    const thumb = document.getElementById(thumbId);
    
    if (!content || !track || !thumb) return;

    let isDragging = false;
    let startY, startScrollTop;

    function updateThumb() {
        const height = content.clientHeight;
        const scrollHeight = content.scrollHeight;
        const scrollTop = content.scrollTop;
        const trackHeight = track.clientHeight;
        
        // Ensure we have a valid height before calculating
        if (height <= 0 || trackHeight <= 0) {
            setTimeout(updateThumb, 50);
            return;
        }

        track.style.display = 'block';

        const contentIndicator = document.getElementById(contentId === 'scriptArea' ? 'customScrollbarContent' : '');
        if (contentIndicator) {
            if (contentId === 'scriptArea' && content.value === "") {
                contentIndicator.style.height = `0%`;
            } else if (contentId === 'scriptArea') {
                const lineCount = content.value.split('\n').length;
                // 22px per line + 30px padding (15 top, 15 bottom)
                const actualTextHeight = (lineCount * 22) + 30;
                const coverage = Math.min(1, actualTextHeight / height);
                contentIndicator.style.height = `${coverage * 100}%`;
            } else {
                // For other lists like file browser
                const coverage = Math.min(1, scrollHeight / height);
                contentIndicator.style.height = `${coverage * 100}%`;
            }
        }

        const isTiny = (contentId === 'scriptArea');
        const thumbHeight = isTiny ? 45 : Math.max(45, (height / Math.max(scrollHeight, 1)) * height);
        
        const scrollRange = Math.max(0, scrollHeight - height);
        const thumbRange = trackHeight - thumbHeight;
        
        let thumbTop = 0;
        if (scrollRange > 0) {
            thumbTop = (scrollTop / scrollRange) * thumbRange;
        }
        
        thumb.style.height = `${thumbHeight}px`;
        thumb.style.transform = `translateY(${thumbTop}px)`;
        
        if (contentId === 'scriptArea') {
            syncHighlightsScroll();
            const lines = content.value.split('\n').length;
            const counter = document.getElementById('lineCounter');
            if (counter) counter.textContent = `${lines} Line${lines !== 1 ? 's' : ''}`;
        }
    }

    content.addEventListener('scroll', updateThumb);
    window.addEventListener('resize', updateThumb);

    thumb.addEventListener('mousedown', (e) => {
        isDragging = true;
        startY = e.clientY;
        startScrollTop = content.scrollTop;
        thumb.classList.add('active');
        document.body.style.userSelect = 'none';
        e.preventDefault();
    });

    document.addEventListener('mousemove', (e) => {
        if (!isDragging) return;
        const deltaY = e.clientY - startY;
        const height = content.clientHeight;
        const scrollHeight = content.scrollHeight;
        const thumbHeight = parseFloat(thumb.style.height);
        const scrollRange = scrollHeight - height;
        const thumbRange = height - thumbHeight;
        const scrollDelta = (deltaY / thumbRange) * scrollRange;
        content.scrollTop = startScrollTop + scrollDelta;
    });

    document.addEventListener('mouseup', () => {
        isDragging = false;
        thumb.classList.remove('active');
        document.body.style.userSelect = '';
    });

    track.addEventListener('mousedown', (e) => {
        if (e.target === thumb) return;
        const rect = track.getBoundingClientRect();
        const clickY = e.clientY - rect.top;
        const targetScrollTop = (clickY / rect.height) * content.scrollHeight - (content.clientHeight / 2);
        content.scrollTop = targetScrollTop;
    });

    updateThumb();
    setInterval(updateThumb, 1000);
}

function initAllCustomScrollbars() {
    setupCustomScrollbar('scriptArea', 'customScrollbar', 'customScrollbarThumb');
    setupCustomScrollbar('fileList', 'fileListScrollbar', 'fileListScrollbarThumb');
    setupCustomScrollbar('fileBrowser', 'fileBrowserScrollbar', 'fileBrowserScrollbarThumb');
}



// =============================================
// Live Keyboard (types to the host in real time)
// =============================================
let liveTypingOn = false;
let silentWasOnBeforeLive = false;  // remember to restore when Live toggles off

function updateSilentIndicator(silentOn) {
    const el = document.getElementById('silentIndicator');
    if (!el) return;
    el.textContent = 'Silent start: ' + (silentOn ? 'ON' : 'OFF');
    el.style.color = silentOn ? 'var(--primary)' : 'var(--text-muted)';
}

function toggleLiveTyping() {
    const cb = document.getElementById('liveEnabled');
    liveTypingOn = cb ? cb.checked : false;
    const s = document.getElementById('liveStatus');
    if (s) {
        s.textContent = liveTypingOn ? '● LIVE - keys go to host' : 'Live typing off';
        s.style.color = liveTypingOn ? 'var(--primary)' : 'var(--text-muted)';
    }
    // Grey out "Send All Text" while Live is on - every keystroke already goes.
    const sendBtn = document.querySelector('button[onclick="sendLiveText()"]');
    if (sendBtn) {
        sendBtn.disabled = liveTypingOn;
        sendBtn.style.opacity = liveTypingOn ? '0.5' : '1';
        sendBtn.title = liveTypingOn ? 'Not needed while Live typing is on - each key sends instantly.' : '';
    }
    // Silent-Startup <-> Live-typing interaction: turning Live ON forces silent
    // OFF so the HID actually attaches; turning Live OFF restores silent to
    // whatever it was before Live went on.
    const silentToggle = document.getElementById('silentToggle');
    if (silentToggle) {
        if (liveTypingOn) {
            silentWasOnBeforeLive = silentToggle.checked;
            if (silentToggle.checked) {
                silentToggle.checked = false;
                fetch('/api/set-silent', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({enabled:false})});
                updateSilentIndicator(false);
            }
        } else if (silentWasOnBeforeLive) {
            silentToggle.checked = true;
            fetch('/api/set-silent', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({enabled:true})});
            silentWasOnBeforeLive = false;
            updateSilentIndicator(true);
        }
    }
    if (liveTypingOn) {
        const li = document.getElementById('liveInput');
        if (li) li.focus();
    } else {
        // ask the device to detach the keyboard again if Silent Startup is on
        liveSend({ release: true });
    }
}

function liveSend(payload) {
    return fetch('/api/live-type', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
        keepalive: true
    }).then(async r => {
        // v4.15: SURFACE server errors. Before this, a 409 (HID disabled by
        // ATTACKMODE, or update in progress) was silently swallowed and the
        // user thought their keystrokes just vanished. Now we render the
        // response text into the Live status line and also alert once.
        if (!r.ok) {
            const t = await r.text().catch(() => '');
            const st = document.getElementById('liveStatus');
            if (st) { st.textContent = '⚠ ' + r.status + ': ' + (t || 'error').substring(0, 80); st.style.color = '#ef4444'; }
            // Rate-limit the alert so it doesn't spam every keystroke.
            if (!window.__liveErrShown || (Date.now() - window.__liveErrShown) > 8000) {
                window.__liveErrShown = Date.now();
                setTimeout(() => alert('Live typing rejected by the ESP:\n\n' + r.status + '  ' + t +
                    '\n\nMost common cause: ATTACKMODE has HID disabled. Fix from Settings > ATTACKMODE quick-set > "HID only" or "HID + STORAGE".'), 100);
            }
        }
    }).catch(e => console.error('live-type failed', e));
}

function liveSendSpecial(name) { liveSend({ special: name }); }

function sendLiveText() {
    const li = document.getElementById('liveInput');
    if (!li || !li.value.length) return;
    const text = li.value;
    const st = document.getElementById('liveStatus');
    if (st) { st.textContent = 'Sending ' + text.length + ' chars...'; }
    liveSend({ text }).finally(() => { if (st) st.textContent = liveTypingOn ? '● LIVE - keys go to host' : 'Live typing off'; });
}

const LIVE_SPECIAL_KEYS = {
    'Enter': 'ENTER', 'Backspace': 'BACKSPACE', 'Tab': 'TAB', 'Escape': 'ESC',
    'ArrowUp': 'ARROW_UP', 'ArrowDown': 'ARROW_DOWN',
    'ArrowLeft': 'ARROW_LEFT', 'ArrowRight': 'ARROW_RIGHT',
    'Delete': 'DELETE', 'Home': 'HOME', 'End': 'END'
};

document.addEventListener('DOMContentLoaded', () => {
    const li = document.getElementById('liveInput');
    if (!li) return;
    // Arm live typing from the (default-on) toggle so the box is live immediately.
    toggleLiveTyping();
    // If the browser tab closes with Silent Startup on, release the HID so
    // the device drops back to stealth (fires even when fetch is blocked).
    window.addEventListener('beforeunload', () => {
        try {
            navigator.sendBeacon('/api/live-type',
                new Blob([JSON.stringify({release: true})], {type: 'application/json'}));
        } catch (e) {}
    });
    // v4.16: mobile keyboards (Android GBoard, iOS system keyboard, etc.)
    // do NOT fire keydown for printable characters - they only fire the
    // `input` event. Enter, Backspace, Tab DO fire keydown because they're
    // treated as commands. That's why "Enter works but a/b/c don't" on the
    // user's phone. Fix: listen to BOTH keydown (physical keyboards, special
    // keys) AND input (mobile virtual keyboards). Dedupe so a desktop press
    // doesn't fire both.
    let __lastKeydownAt = 0;
    let __lastKeydownChar = '';

    li.addEventListener('keydown', (e) => {
        if (!liveTypingOn) return;
        if (e.isComposing || e.keyCode === 229 || e.key === 'Dead' || e.key === 'Process') return; // IME/dead keys
        // Ctrl/Alt/Gui combos -> send as a key combination
        if ((e.ctrlKey || e.altKey || e.metaKey) && e.key.length === 1) {
            e.preventDefault();
            const mods = [];
            if (e.ctrlKey) mods.push('CTRL');
            if (e.altKey) mods.push('ALT');
            if (e.metaKey) mods.push('GUI');
            if (e.shiftKey) mods.push('SHIFT');
            liveSend({ combo: mods.join(' ') + ' ' + e.key });
            __lastKeydownAt = Date.now();
            __lastKeydownChar = '';
            return;
        }
        if (LIVE_SPECIAL_KEYS[e.key]) {
            if (e.key === 'Tab') e.preventDefault(); // keep focus in the box
            liveSendSpecial(LIVE_SPECIAL_KEYS[e.key]);
            __lastKeydownAt = Date.now();
            __lastKeydownChar = '';
            return;
        }
        if (e.key.length === 1) {
            liveSend({ k: e.key }); // physical keyboard printable char
            __lastKeydownAt = Date.now();
            __lastKeydownChar = e.key;
        }
    });

    // v4.16: catch mobile virtual-keyboard typing via the `input` event.
    // e.data is the string that was inserted; e.inputType tells us if it
    // was a delete. Dedupe against keydown so desktop presses don't fire
    // both handlers.
    // v4.17: also listen to `beforeinput` because on Android GBoard the
    // Backspace fires that event with inputType='deleteContentBackward'
    // slightly before the `input` event - and my `input` handler's 30 ms
    // keydown-debounce sometimes swallowed it if a stray keydown fired for
    // the "physical" backspace keycode too. Handling both events with a
    // smarter debounce (only skip printable chars, never delete) fixes it.
    const __handleTypingEvent = (e) => {
        if (!liveTypingOn) return;
        const it = e.inputType || '';
        // ALWAYS handle deletes, regardless of keydown timing.
        if (it.startsWith('delete')) {
            liveSendSpecial('BACKSPACE');
            return;
        }
        // Enter via inputType (mobile).
        if (it === 'insertLineBreak' || it === 'insertParagraph') {
            liveSendSpecial('ENTER');
            return;
        }
        // For inserted text: dedupe against a recent keydown with THE SAME char.
        if (e.data && e.data.length > 0) {
            const dt = Date.now() - __lastKeydownAt;
            if (dt < 40 && __lastKeydownChar === e.data) return;   // physical kbd already sent it
            liveSend({ text: e.data });
        }
    };
    li.addEventListener('beforeinput', __handleTypingEvent);
    li.addEventListener('input',       __handleTypingEvent);

    // Also: on mobile, `keyup` may be the only reliable signal for Enter
    // when the browser inserts a newline via input (with type=textarea).
    // We already handle Enter via keydown; the input listener above is the
    // safety net for that.
});

// =============================================
// Bundled firmware/website update (.espkg)
// =============================================

// Human-readable byte size
function fmtBytes(n) {
    if (n < 1024) return n + ' B';
    if (n < 1024*1024) return (n/1024).toFixed(1) + ' KB';
    return (n/1024/1024).toFixed(2) + ' MB';
}

let espkgFileHandle = null;   // the File object user has queued
let espkgManifest   = null;   // parsed manifest for the preview

// v4.12: version-string helpers for the downgrade warning.
// parseVer('4.11') -> [4, 11]; parseVer('4.11.2') -> [4, 11, 2].
// cmpVer([4,11], [4,10]) -> +1  (first is newer)
function parseVer(v) {
    if (v == null) return null;
    const s = String(v).trim().replace(/^v/i, '');
    const parts = s.split(/[.\-]/).map(p => parseInt(p, 10));
    if (parts.some(isNaN)) return null;
    return parts;
}
function cmpVer(a, b) {
    const n = Math.max(a.length, b.length);
    for (let i = 0; i < n; i++) {
        const av = a[i] == null ? 0 : a[i];
        const bv = b[i] == null ? 0 : b[i];
        if (av !== bv) return av < bv ? -1 : 1;
    }
    return 0;
}

async function parseEspkg(file) {
    // .espkg = "ESPKG\x01" magic (6B) + uint32 LE manifest length + manifest JSON + payloads
    const head = await file.slice(0, 10).arrayBuffer();
    const bytes = new Uint8Array(head);
    const magicOK =
        bytes[0]===0x45 && bytes[1]===0x53 && bytes[2]===0x50 &&
        bytes[3]===0x4B && bytes[4]===0x47 && bytes[5]===0x01;
    if (!magicOK) throw new Error('Not a valid .espkg file (bad magic)');
    const manLen = bytes[6] | (bytes[7]<<8) | (bytes[8]<<16) | (bytes[9]<<24);
    if (manLen === 0 || manLen > 8192) throw new Error('Invalid manifest size');
    const manBuf = await file.slice(10, 10 + manLen).arrayBuffer();
    return JSON.parse(new TextDecoder().decode(manBuf));
}

function resetEspkgPicker() {
    espkgFileHandle = null;
    espkgManifest = null;
    const fi = document.getElementById('espkgFile');    if (fi) fi.value = '';
    const p  = document.getElementById('espkgPreview'); if (p)  p.style.display = 'none';
    const bar= document.getElementById('updateProgressBar'); if (bar) bar.style.display = 'none';
    const st = document.getElementById('updateStatusLine'); if (st) st.textContent = 'Idle';
    const btn= document.getElementById('updateApplyBtn'); if (btn) btn.disabled = true;
    const dt = document.getElementById('espkgDropText'); if (dt) dt.innerHTML = 'Click or drop a <code>.espkg</code> file here';
    const dz = document.getElementById('espkgDropZone'); if (dz) dz.style.borderColor = 'var(--glass-border)';
}

async function onEspkgFileChosen(fileArg) {
    const file = fileArg || (document.getElementById('espkgFile').files[0]);
    if (!file) return;
    // Sanity cap: the app partition is ~3MB; a real .espkg tops out around 2MB
    // (web files + firmware.bin). Anything much bigger is almost certainly the
    // wrong file - refuse before eating our whole SD upload buffer.
    const MAX_ESPKG = 6 * 1024 * 1024;   // 6 MB hard cap
    if (file.size > MAX_ESPKG) {
        alert('That file is ' + Math.round(file.size / 1024 / 1024) + ' MB - that is well over the ~2 MB expected for a .espkg. Refusing to upload.');
        resetEspkgPicker();
        return;
    }
    espkgFileHandle = file;
    const preview     = document.getElementById('espkgPreview');
    const pName       = document.getElementById('espkgPreviewName');
    const pSize       = document.getElementById('espkgPreviewSize');
    const pVer        = document.getElementById('espkgPreviewVersion');
    const pContents   = document.getElementById('espkgPreviewContents');
    const btn         = document.getElementById('updateApplyBtn');
    const dt          = document.getElementById('espkgDropText');
    pName.textContent = file.name;
    pSize.textContent = fmtBytes(file.size);
    dt.textContent = file.name;
    try {
        const m = await parseEspkg(file);
        espkgManifest = m;
        pVer.textContent = 'version: ' + (m.version || '?') + (m.created ? '  ·  built ' + m.created : '');
        let lines = [];
        if (m.sd && m.sd.length) {
            m.sd.forEach(f => lines.push('SD  ' + (f.path || '?').padEnd(20) + fmtBytes(f.size || 0)));
        }
        const hasFw = !!(m.fw && m.fw.size);
        if (hasFw) {
            lines.push('FW  firmware.bin      ' + fmtBytes(m.fw.size) + '  -> OTA + REBOOT');
        }
        if (!lines.length) lines.push('(empty package)');
        pContents.textContent = lines.join('\n');
        preview.style.display = 'block';
        // Big obvious banner so the user isn't surprised by web-only vs full.
        const bannerText = hasFw
            ? '✅ Full package: web files + firmware. The ESP will reboot after flashing.'
            : '⚠ WEB-ONLY package: only index.html / style.css / script.js will be updated. No firmware flash, no reboot.';
        const bannerColor = hasFw ? 'var(--primary)' : 'var(--danger)';

        // v4.12: firmware downgrade warning. Compare package version with the
        // currently-running firmware (fetched via /api/stats and cached in
        // window.__currentFwVersion). If the package is OLDER, prepend a
        // separate red warning banner and require the user to confirm at
        // upload time.
        let downgradeBanner = '';
        try {
            const pv = parseVer(m.version);
            const cv = parseVer(window.__currentFwVersion);
            if (pv && cv && cmpVer(pv, cv) < 0) {
                downgradeBanner =
                    '<div style="padding:8px 10px;border-radius:6px;background:rgba(220,38,38,0.15);' +
                    'color:#ef4444;font-weight:600;margin-bottom:8px;white-space:normal;border:1px solid rgba(220,38,38,0.5);">' +
                    '⏪ DOWNGRADE: package version ' + m.version + ' is OLDER than the running firmware ' +
                    (window.__currentFwVersion || '?') + '. Older versions may have bugs / security issues fixed since. ' +
                    'You will be asked to confirm before flash.' +
                    '</div>';
            }
        } catch (e) { /* best-effort */ }

        // Prepend banners to the preview contents area (downgrade first, then hasFw/webonly).
        pContents.innerHTML = downgradeBanner +
            '<div style="padding:8px 10px;border-radius:6px;background:rgba(0,0,0,0.2);color:'
            + bannerColor + ';font-weight:600;margin-bottom:8px;white-space:normal;">' + bannerText
            + '</div><span style="white-space:pre-wrap;">' + pContents.textContent + '</span>';
        btn.disabled = false;
    } catch (e) {
        pVer.textContent = '';
        pContents.innerHTML = '<span style="color: var(--danger);">✗ ' + e.message + '</span>';
        preview.style.display = 'block';
        btn.disabled = true;
    }
}

// Drag-and-drop wiring (attached on load)
document.addEventListener('DOMContentLoaded', () => {
    const dz = document.getElementById('espkgDropZone');
    if (!dz) return;
    ['dragenter','dragover'].forEach(e => dz.addEventListener(e, ev => {
        ev.preventDefault(); ev.stopPropagation();
        dz.style.borderColor = 'var(--primary)';
        dz.style.background = 'rgba(76,175,80,0.05)';
    }));
    ['dragleave','drop'].forEach(e => dz.addEventListener(e, ev => {
        ev.preventDefault(); ev.stopPropagation();
        dz.style.borderColor = 'var(--glass-border)';
        dz.style.background = 'transparent';
    }));
    dz.addEventListener('drop', ev => {
        const f = ev.dataTransfer && ev.dataTransfer.files && ev.dataTransfer.files[0];
        if (f) { document.getElementById('espkgFile').files = ev.dataTransfer.files; onEspkgFileChosen(f); }
    });
});

function uploadUpdatePackage() {
    if (!espkgFileHandle) { alert('Choose a .espkg file first.'); return; }
    const file = espkgFileHandle;
    if (!file.name.endsWith('.espkg')) {
        if (!confirm('That file is not a .espkg. Upload anyway?')) return;
    }
    if (espkgManifest && espkgManifest.fw && espkgManifest.fw.size > 0) {
        if (!confirm('This package includes a firmware image - the device will FLASH & REBOOT.\n\nContinue?')) return;
    }
    // v4.12/v4.16: hard-confirm on downgrade. If the current fw version is
    // UNKNOWN (first-poll race, or the server field is missing), ALSO require
    // YES-confirm - we cannot silently allow a possible downgrade just
    // because a race window swallowed the version string.
    try {
        const pv = parseVer(espkgManifest && espkgManifest.version);
        const cv = parseVer(window.__currentFwVersion);
        let needsHardConfirm = false;
        let reason = '';
        if (!cv) {
            needsHardConfirm = true;
            reason = 'The running firmware version is not yet known (page just loaded?). ' +
                     'Cannot verify this is not a downgrade.\n\n' +
                     'Package version: ' + (espkgManifest && espkgManifest.version) + '\n';
        } else if (pv && cmpVer(pv, cv) < 0) {
            needsHardConfirm = true;
            reason = 'DOWNGRADE WARNING\n\nPackage version: ' + espkgManifest.version + '\n' +
                     'Currently running: ' + window.__currentFwVersion + '\n\n' +
                     'You are flashing an OLDER firmware. Bugs and security issues fixed since then will come back.\n';
        }
        if (needsHardConfirm) {
            if (!confirm(reason + '\nType YES in the next prompt to confirm.')) return;
            const ans = prompt('Type YES (all caps) to confirm:');
            if (ans !== 'YES') { alert('Flash cancelled.'); return; }
        }
    } catch (e) { /* if version parsing fails just proceed */ }

    const bar = document.getElementById('updateProgressBar');
    const fill = document.getElementById('updateProgressFill');
    const statusLine = document.getElementById('updateStatusLine');
    const btn = document.getElementById('updateApplyBtn');
    bar.style.display = 'block';
    fill.style.width = '0%';
    btn.disabled = true;
    statusLine.textContent = 'Uploading package...';

    const form = new FormData();
    form.append('package', file, file.name);
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/update-package');
    const t0 = Date.now();
    xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) {
            const pct = Math.round((e.loaded / e.total) * 100);
            const kbps = e.loaded / Math.max(1, (Date.now() - t0)) * 1000 / 1024;
            fill.style.width = pct + '%';
            statusLine.textContent = `Uploading  ${fmtBytes(e.loaded)} / ${fmtBytes(e.total)}  ·  ${kbps.toFixed(0)} KB/s  ·  ${pct}%`;
        }
    };
    xhr.onload = () => {
        if (xhr.status === 200) {
            statusLine.textContent = 'Upload complete - applying...';
            pollUpdateStatus();
        } else {
            statusLine.textContent = `Upload failed (${xhr.status}). Try again - the package is still on the device only if it was fully received.`;
            btn.disabled = false;
        }
    };
    xhr.onerror = () => {
        statusLine.textContent = 'Upload error (connection lost). If the LED is blinking blue the update may still be applying - wait 30s then reload.';
        btn.disabled = false;
    };
    xhr.send(form);
}

// Big obvious in-page banner shown when a .espkg update completes - success
// or failure. Fixed-position, centered, dismissable. Not an alert() because
// we want the user to actually read the outcome, not click through it.
function showUpdateBanner(kind, title, message) {
    // kind: 'success' | 'error' | 'reboot'
    const existing = document.getElementById('updateCompleteBanner');
    if (existing) existing.remove();
    const bg = {
        success: 'linear-gradient(135deg, #16a34a, #22c55e)',
        reboot:  'linear-gradient(135deg, #2563eb, #3b82f6)',
        error:   'linear-gradient(135deg, #b91c1c, #ef4444)',
    }[kind] || '#333';
    const icon = { success: '✅', reboot: '🔄', error: '❌' }[kind] || 'ℹ';
    const div = document.createElement('div');
    div.id = 'updateCompleteBanner';
    div.style.cssText =
        'position:fixed;top:20px;left:50%;transform:translateX(-50%);z-index:99999;' +
        'padding:18px 28px;border-radius:14px;color:#fff;font-family:inherit;' +
        'box-shadow:0 12px 40px rgba(0,0,0,0.5);max-width:90%;text-align:center;' +
        'background:' + bg + ';animation:fadeSlideIn 0.4s ease;';
    div.innerHTML =
        '<div style="font-size:24px;margin-bottom:6px;">' + icon + ' <b>' + title + '</b></div>' +
        '<div style="font-size:13px;opacity:0.95;">' + message + '</div>' +
        '<button style="margin-top:12px;padding:6px 16px;border:none;border-radius:8px;' +
        'background:rgba(255,255,255,0.2);color:#fff;cursor:pointer;font-size:12px;" ' +
        'onclick="this.parentElement.remove()">Dismiss</button>';
    document.body.appendChild(div);
    // Style @keyframes once - cheap, no external CSS dep.
    if (!document.getElementById('updateBannerAnim')) {
        const s = document.createElement('style');
        s.id = 'updateBannerAnim';
        s.textContent = '@keyframes fadeSlideIn{from{opacity:0;transform:translate(-50%,-20px);}' +
                        'to{opacity:1;transform:translate(-50%,0);}}';
        document.head.appendChild(s);
    }
}

function pollUpdateStatus() {
    const fill = document.getElementById('updateProgressFill');
    const statusLine = document.getElementById('updateStatusLine');
    const btn = document.getElementById('updateApplyBtn');
    let misses = 0;
    const iv = setInterval(() => {
        fetch('/api/update-status', { cache: 'no-store' }).then(r => r.json()).then(s => {
            misses = 0;
            fill.style.width = (s.progress || 0) + '%';
            statusLine.textContent = s.status || 'Applying...';
            const done = !s.applying && (s.progress >= 100 || /done|reboot|website updated/i.test(s.status || ''));
            const err = !s.applying && /error/i.test(s.status || '');
            if (err) {
                clearInterval(iv);
                btn.disabled = false;
                showUpdateBanner('error', 'Update failed', s.status || 'Unknown error - check the console.');
            } else if (done) {
                clearInterval(iv);
                if (/reboot/i.test(s.status || '')) {
                    statusLine.textContent = 'Firmware flashed - device rebooting. Reconnect in ~15s.';
                    showUpdateBanner('reboot', 'Firmware flashed!',
                        'The ESP is rebooting to the new firmware. Reconnect to the AP in ~15 seconds.');
                } else {
                    statusLine.textContent = (s.status || 'Done') + ' - reloading UI...';
                    showUpdateBanner('success', 'Update complete!',
                        'Website files updated. Reloading in a moment...');
                    setTimeout(() => location.reload(true), 1800);
                }
                btn.disabled = false;
            }
        }).catch(() => {
            // during the OTA reboot the device drops off - treat repeated misses as a reboot
            misses++;
            if (misses > 4) {
                clearInterval(iv);
                statusLine.textContent = 'Device rebooting (firmware applied). Reconnect in ~15s.';
                showUpdateBanner('reboot', 'Firmware applied!',
                    'The device dropped off the network - it is rebooting to the new firmware. ' +
                    'Reconnect to the AP in ~15 seconds.');
                btn.disabled = false;
            }
        });
    }, 1000);
}

// Convenience: fire an ATTACKMODE reconfig from a Settings button.
function setAttackMode(mode) {
    if (!confirm('Set ATTACKMODE ' + mode + '?\n\nThe device will reboot to rebuild USB descriptors.')) return;
    fetch('/execute', {
        method: 'POST',
        headers: {'Content-Type': 'text/plain'},
        body: 'ATTACKMODE ' + mode
    }).then(() => {
        // Show a heads-up - the device restarts, so this reply arrives fast then dies.
        const s = document.getElementById('scriptStatus');
        if (s) s.textContent = 'ATTACKMODE ' + mode + ' - rebooting...';
    });
}

// ===== Wiki tab: all supported commands =====
const WIKI_COMMANDS = [
    { cat: 'Typing',        cmd: 'STRING <text>',                       desc: 'Type text at full speed (no newline).' },
    { cat: 'Typing',        cmd: 'STRINGLN <text>',                     desc: 'Type text, then press Enter.' },
    { cat: 'Typing',        cmd: 'DELAY <ms>',                          desc: 'Sleep the given milliseconds.' },
    { cat: 'Typing',        cmd: 'DEFAULTDELAY <ms>',                   desc: 'Set an implicit pause after every command.' },
    { cat: 'Keys',          cmd: 'ENTER / TAB / ESC / BACKSPACE',       desc: 'Send single key.' },
    { cat: 'Keys',          cmd: 'F1 .. F12',                           desc: 'Function keys.' },
    { cat: 'Keys',          cmd: 'UP / DOWN / LEFT / RIGHT',            desc: 'Arrow keys.' },
    { cat: 'Keys',          cmd: 'HOME / END / PAGEUP / PAGEDOWN',      desc: 'Navigation keys.' },
    { cat: 'Keys',          cmd: 'INSERT / DELETE / MENU',              desc: 'Assorted control keys.' },
    { cat: 'Keys',          cmd: 'CTRL <key>',                          desc: 'Ctrl-modified key press (e.g. CTRL c).' },
    { cat: 'Keys',          cmd: 'ALT <key>',                           desc: 'Alt-modified (e.g. ALT F4).' },
    { cat: 'Keys',          cmd: 'GUI <key>',                           desc: 'Windows/Cmd key (e.g. GUI r).' },
    { cat: 'Keys',          cmd: 'SHIFT <key>',                         desc: 'Shift-modified.' },
    { cat: 'Keys',          cmd: 'HOLD <key1> <key2> ... [ms]',         desc: 'Hold multiple keys for a duration.' },
    { cat: 'Keys',          cmd: 'STOPHOLD',                            desc: 'Release all held keys.' },
    { cat: 'Keys',          cmd: 'KEYCODE <hex>',                       desc: 'Send a raw HID keycode.' },
    { cat: 'Layout',        cmd: 'LOCALE <code>',                       desc: 'Switch keymap file (e.g. de, fr, us).' },
    { cat: 'Layout',        cmd: 'LOCALE_EN / LOCALE_DE / ...',         desc: 'Shortcut to load a specific layout.' },
    { cat: 'Flow',          cmd: 'VAR / VARIABLE <name> = <value>',     desc: 'Declare a script variable.' },
    { cat: 'Flow',          cmd: 'IF <cond>',                           desc: 'Start conditional block.' },
    { cat: 'Flow',          cmd: 'ELIF <cond> / ELSE / ENDIF',          desc: 'Standard IF/ELSE/END.' },
    { cat: 'Flow',          cmd: 'REPEAT <n>',                          desc: 'Repeat previous command n times.' },
    { cat: 'Flow',          cmd: 'FOR <var> FROM <a> TO <b> STEP <s>',  desc: 'Numeric loop.' },
    { cat: 'Flow',          cmd: 'ENDFOR / END_FOR',                    desc: 'End numeric loop.' },
    { cat: 'Flow',          cmd: 'FUNCTION <name>',                     desc: 'Define a reusable block.' },
    { cat: 'Flow',          cmd: 'END_FUNCTION / RETURN',               desc: 'End function / early return.' },
    { cat: 'LED',           cmd: 'LED_ON / LED_OFF',                    desc: 'Turn the blue LED on or off.' },
    { cat: 'LED',           cmd: 'LED_BLINK / BLINK_STOP',              desc: 'Start / stop blinking.' },
    { cat: 'LED',           cmd: 'LED_R / LED_G / LED_B / ...',         desc: 'Colour commands (all light the single blue LED).' },
    { cat: 'Network',       cmd: 'WIFI_ON / WIFI_OFF',                  desc: 'Turn WiFi radio on/off.' },
    { cat: 'Network',       cmd: 'JOIN_INTERNET <ssid> <pw>',           desc: 'Connect ESP to a WiFi network.' },
    { cat: 'Network',       cmd: 'LEAVE_INTERNET',                      desc: 'Disconnect the STA client.' },
    { cat: 'Network',       cmd: 'IF_ONLINE / IF_OFFLINE',              desc: 'Guard on internet reachability.' },
    { cat: 'Network',       cmd: 'IF_PRESENT SSID="name"',              desc: 'Check nearby WiFi network is present.' },
    { cat: 'Network',       cmd: 'IF_NOTPRESENT SSID="name"',           desc: 'Inverse of IF_PRESENT.' },
    { cat: 'Network',       cmd: 'HTTP_REQUEST <url>',                  desc: 'GET the URL, ignore body.' },
    { cat: 'Network',       cmd: 'DOWNLOAD_FILE <url> <path>',          desc: 'Download URL body to path on SD.' },
    { cat: 'Network',       cmd: 'UPLOAD_FILE <path> <url>',            desc: 'POST SD file to URL.' },
    { cat: 'Network',       cmd: 'PING <host>',                         desc: 'Ping a host (WiFi client).' },
    { cat: 'Network',       cmd: 'GET_TIME / GET_DAY',                  desc: 'Fetch time/day of week over WiFi.' },
    { cat: 'Network',       cmd: 'RUN_AT_TIME = <hh:mm>',               desc: 'Sleep until wall-clock time reaches value.' },
    { cat: 'Network',       cmd: 'RUN_AT_DAY = <MON..SUN>',             desc: 'Sleep until it is that weekday.' },
    { cat: 'Bluetooth',     cmd: 'BLUETOOTH_ON / BLUETOOTH_OFF',        desc: 'Toggle the BLE radio.' },
    { cat: 'Bluetooth',     cmd: 'BLUETOOTH_DISCOVERY',                 desc: 'Kick off periodic BLE scan.' },
    { cat: 'Bluetooth',     cmd: 'IF_BT_PRESENT "Name"',                desc: 'Match by BLE device name.' },
    { cat: 'Bluetooth',     cmd: 'RUN_WHEN_BT_FOUND',                   desc: 'Fire when a target is nearby.' },
    { cat: 'Presence',      cmd: 'IF_CLIENT_CONNECTED / DISCONNECTED',       desc: 'Any client tied to the ESP AP.' },
    { cat: 'Presence',      cmd: 'IF_CLIENT_CONNECTED_WIFI / _BLUETOOTH',    desc: 'Radio-specific variant.' },
    { cat: 'OS Detect',     cmd: 'DETECT_OS',                           desc: 'Attempt to fingerprint host OS.' },
    { cat: 'OS Detect',     cmd: 'IF_OS <WINDOWS|MAC|LINUX>',           desc: 'Branch on detected OS.' },
    { cat: 'OS Detect',     cmd: 'IF_DETECT_OS_INCLUDES <substr>',      desc: 'Match against the OS name.' },
    { cat: 'USB',           cmd: 'ATTACKMODE HID [STORAGE] [SIZE_XX_GB] [VID_XXXX] [PID_XXXX]', desc: 'Configure USB composition.' },
    { cat: 'USB',           cmd: 'ATTACKMODE STORAGE_ONLY',             desc: 'Storage without HID (explicit, prevents lockout).' },
    { cat: 'USB',           cmd: 'ATTACKMODE OFF',                      desc: 'Drop STORAGE, keep HID.' },
    { cat: 'USB',           cmd: 'ATTACKMODE BLANK',                    desc: 'Tear the device fully off USB - no HID, no drive. Recover via WiFi (ATTACKMODE HID).' },
    { cat: 'USB',           cmd: 'SIZE_<n>_GB / _MB / _KB',             desc: 'Report a custom disk capacity.' },
    { cat: 'USB',           cmd: 'HID_ATTACH / HID_DETACH',             desc: 'Present or hide the keyboard on demand.' },
    { cat: 'USB',           cmd: 'VID_XXXX / PID_XXXX',                 desc: 'Set VID/PID standalone (persists to next boot).' },
    { cat: 'USB',           cmd: 'MAN_"<mfr>" / PRODUCT_"<name>"',      desc: 'USB identity strings.' },
    { cat: 'USB',           cmd: 'RANDOM_VID / RANDOM_PID',             desc: 'Enable randomization at boot.' },
    { cat: 'System',        cmd: 'REBOOT / SHUTDOWN',                   desc: 'Restart or power off the ESP.' },
    { cat: 'System',        cmd: 'SELFDESTRUCT / SELF_DESTRUCT',        desc: 'HARD brick: 3-pass wipe of both OTA app partitions + NVS + SD. Reflash to recover.' },
    { cat: 'System',        cmd: 'FACTORY_RESET / FACTORYRESET',        desc: 'Wipe scripts/uploads/logs + all NVS; KEEP website. LED blinks blue. Reboot.' },
    { cat: 'System',        cmd: 'BEHAVE_BROKEN / BEHAVEBROKEN',        desc: 'Reboots as a plain "SD_READER" USB stick (hides web files, no HID/WiFi). Recovery: hold GPIO0 5 s at boot.' },
    { cat: 'System',        cmd: 'SAVE_CREDENTIALS',                    desc: 'Persist the current STA WiFi credentials.' },
    { cat: 'System',        cmd: 'SET_BOOT_SCRIPT <file>',              desc: 'Change which script runs on boot.' },
    { cat: 'System',        cmd: 'SET_BUTTON_PIN <n>',                  desc: 'Override the stop button GPIO.' },
    { cat: 'System',        cmd: 'RUN_PAYLOAD <file>',                  desc: 'Invoke another script by filename.' },
    { cat: 'System',        cmd: 'RUN_ON_REBOOT ... END_RUN_ON_REBOOT', desc: 'Block that runs once at next boot.' },
    { cat: 'Files',         cmd: 'USE_FILE <path>',                     desc: 'Type the contents of a file.' },
    { cat: 'Files',         cmd: 'COPY_FILE <src> <dst>',               desc: 'Copy on the SD card.' },
    { cat: 'Files',         cmd: 'CUT_FILE <src> <dst>',                desc: 'Move on the SD card.' },
    { cat: 'Files',         cmd: 'PASTE_FILE <dst>',                    desc: 'Complete a prior CUT/COPY into dst.' },
    { cat: 'Files',         cmd: 'CD <dir>',                            desc: 'Change working directory for later ops.' },
    { cat: 'Random',        cmd: 'RANDOM_CHAR',                         desc: 'Type a random letter.' },
    { cat: 'Random',        cmd: 'RANDOM_NUMBER',                       desc: 'Type a random digit.' },
    { cat: 'Random',        cmd: 'RANDOM_SPECIAL',                      desc: 'Type a random symbol.' },
    { cat: 'Automation',    cmd: 'BEGIN_ROWER ... END_ROWER',           desc: 'Multi-script batch queue.' },
    { cat: 'Automation',    cmd: 'WAIT_FOR_EVENT <name>',               desc: 'Sleep until an event fires.' },
    { cat: 'Automation',    cmd: 'WAIT_FOR_SD',                         desc: 'Block until the SD card is present.' },
    { cat: 'Automation',    cmd: 'HOLD_TILL_STRING <text>',             desc: 'Wait until text appears on screen.' },
    { cat: 'Meta',          cmd: 'REM <comment> or // <comment>',       desc: 'Line comment (ignored).' },
];

function renderWiki(filter) {
    const box = document.getElementById('wikiTable');
    if (!box) return;
    const f = (filter || '').trim().toUpperCase();
    const rows = WIKI_COMMANDS.filter(r =>
        !f || r.cmd.toUpperCase().includes(f) || r.cat.toUpperCase().includes(f) || r.desc.toUpperCase().includes(f));
    box.innerHTML = '';
    let lastCat = '';
    rows.forEach(r => {
        if (r.cat !== lastCat) {
            const h = document.createElement('div');
            h.style.cssText = 'color:var(--primary);font-weight:600;padding:12px 0 6px;font-size:13px;';
            h.textContent = r.cat;
            box.appendChild(h);
            lastCat = r.cat;
        }
        const row = document.createElement('div');
        row.className = 'help-item';
        row.style.cssText = 'display:grid;grid-template-columns:minmax(180px,45%) 1fr;gap:12px;padding:8px 10px;border-radius:6px;cursor:pointer;font-size:12px;background:var(--bg-input);margin-bottom:4px;';
        const code = document.createElement('code');
        code.style.cssText = 'color:var(--primary);word-break:break-word;';
        code.textContent = r.cmd;
        const span = document.createElement('span');
        span.style.color = 'var(--text-muted)';
        span.textContent = r.desc;
        row.appendChild(code);
        row.appendChild(span);
        row.title = 'Tap to copy';
        row.onclick = () => {
            // v4.15: use safeCopyText so this works over HTTP AND inside the
            // Android captive-portal browser (where navigator.clipboard is
            // undefined). Also show a quick visual confirmation.
            const orig = row.style.background;
            safeCopyText(r.cmd).then(ok => {
                row.style.background = ok ? 'rgba(76,175,80,0.35)' : 'rgba(220,38,38,0.25)';
                // Brief inline "Copied" pill.
                const pill = document.createElement('span');
                pill.textContent = ok ? 'Copied' : 'Copy failed';
                pill.style.cssText = 'position:absolute;right:8px;top:8px;background:#1e1e28;color:#fff;padding:3px 8px;border-radius:6px;font-size:11px;';
                row.style.position = 'relative';
                row.appendChild(pill);
                setTimeout(() => { try { row.removeChild(pill); } catch (e) {} row.style.background = orig || 'var(--bg-input)'; }, 900);
            });
        };
        box.appendChild(row);
    });
    if (rows.length === 0) {
        box.innerHTML = '<div style="text-align:center;color:var(--text-muted);padding:20px;">No commands match that filter.</div>';
    }
}
function filterWiki() { renderWiki(document.getElementById('wikiSearch').value); }

// Populate Wiki when the tab opens
(function() {
    const originalOpen = window.openTab;
    if (typeof originalOpen === 'function') {
        window.openTab = function(evt, name) {
            originalOpen(evt, name);
            if (name === 'Wiki') renderWiki('');
        };
    }
})();

// ===== Stream tab removed in v4.3 (feature not wanted). =====

// ===== Settings v4.5 additions =====
// "Allow COM connections" - enables USB CDC. PuTTY connects with connection
// type "Serial" to the COM port that appears in Device Manager.
// Toggling reboots the ESP so the USB descriptor rebuilds; HID and STORAGE
// are unavailable while this is on.
function toggleCom() {
    const on = document.getElementById('comToggle').checked;
    const warn = on
      ? 'Enabling COM connections requires a REBOOT.\n\n' +
        'While on:\n' +
        '  • HID (keyboard) is disabled\n' +
        '  • STORAGE (drive) is disabled\n' +
        '  • Device appears as a COM port in Device Manager\n\n' +
        'Connect with PuTTY:\n' +
        '  Connection type: Serial\n' +
        '  Serial line    : the new COM port (e.g. COM7)\n' +
        '  Speed          : 115200\n\n' +
        'To disable: come back to this dashboard and switch the slider off, ' +
        'or type "disable-com" in the PuTTY shell.\n\n' +
        'Proceed?'
      : 'Disable COM connections and reboot to re-enable HID + STORAGE?';
    if (!confirm(warn)) {
        // Roll the checkbox back - the user cancelled.
        document.getElementById('comToggle').checked = !on;
        return;
    }
    // Show a rebooting overlay because the AP will drop as the ESP restarts.
    const o = document.createElement('div');
    o.style.cssText =
        'position:fixed;inset:0;z-index:99999;background:rgba(0,0,0,0.85);color:#fff;' +
        'display:flex;align-items:center;justify-content:center;flex-direction:column;padding:20px;text-align:center;';
    o.innerHTML =
        '<div style="font-size:40px;">🔌</div>' +
        '<h3 style="margin:12px 0;">' + (on ? 'Enabling COM mode…' : 'Disabling COM mode…') + '</h3>' +
        '<p style="opacity:0.85;font-size:13px;max-width:420px;">' +
        'The device is rebooting to rebuild the USB descriptor. ' +
        (on
            ? 'A new COM port will appear in Device Manager - open PuTTY (Serial, that COM, 115200).'
            : 'HID + STORAGE will come back on the next boot.') +
        '</p>';
    document.body.appendChild(o);
    fetch('/api/toggle-com', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ enabled: on })
    }).catch(() => {});   // socket dies during reboot - expected
    // Try to reconnect after ~15 s.
    setTimeout(() => location.reload(true), 15000);
}

// v4.12: Randomize WiFi MAC on STA connect. Persists to NVS; applied at the
// next joinWiFi() call. Also driven by the first-boot setup wizard.
function toggleRndMac() {
    const on = document.getElementById('rndMacToggle').checked;
    fetch('/api/toggle-random-mac', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ enabled: on })
    }).then(r => r.json()).then(() => {
        if (on) alert('Randomize WiFi MAC is ON.\nThe next time the ESP joins a WiFi network, it will use a fresh random MAC address instead of its factory MAC.');
    });
}

function toggleRndUsb() {
    const on = document.getElementById('rndUsbToggle').checked;
    fetch('/api/save-usb', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
            usbVID: document.getElementById('usbVID').value || '0x303a',
            usbPID: document.getElementById('usbPID').value || '0x0002',
            usbRndVID: on, usbRndPID: on,
            usbMfr:  document.getElementById('usbMfr').value  || 'Espressif',
            usbProd: document.getElementById('usbProd').value || 'ESP32-S3'
        })
    }).then(() => {
        if (on) alert('Randomize USB VID/PID enabled.\nA fresh random ID will be rolled at every boot.\n(Silent Startup makes each re-attach roll fresh IDs too.)');
    });
}

function fmtMB(bytes) {
    if (!bytes) return '-';
    if (bytes < 1024*1024) return (bytes/1024).toFixed(0) + ' KB';
    if (bytes < 1024*1024*1024) return (bytes/(1024*1024)).toFixed(0) + ' MB';
    return (bytes/(1024*1024*1024)).toFixed(2) + ' GB';
}
function fmtUptime(secs) {
    if (!secs) return '-';
    const h = Math.floor(secs/3600), m = Math.floor((secs%3600)/60), s = secs%60;
    if (h > 0) return h + 'h ' + m + 'm';
    if (m > 0) return m + 'm ' + s + 's';
    return s + 's';
}
// Extend the existing settings-tab initializer / stats poller: mirror system
// stats into the Settings tab tiles and keep the new toggles synced.
(function() {
    const orig = window.initSettingsTab;
    window.initSettingsTab = function() {
        if (orig) orig.apply(this, arguments);
        // Also fetch once to seed the new fields
        fetch('/api/stats', {cache: 'no-store'}).then(r => r.json()).then(data => {
            const rn = document.getElementById('rndUsbToggle');  if (rn) rn.checked = !!(data.usbRndVid || data.usbRndPid);
            const rm = document.getElementById('rndMacToggle');  if (rm) rm.checked = !!data.randomMac;
            const co = document.getElementById('comToggle');     if (co) co.checked = !!data.comEnabled;
            const cpu = document.getElementById('settCpu');      if (cpu) cpu.textContent = (data.cpuMhz||0) + ' MHz - ' + (data.cpuCores||1) + 'x - ' + (data.cpuBusyPct||0) + '%';
            const ram = document.getElementById('settRam');      if (ram) ram.textContent = fmtMB(data.freeMemory) + ' free / ' + fmtMB(data.totalMemory);
            const fl  = document.getElementById('settFlash');    if (fl)  fl.textContent  = fmtMB(data.flashSize);
            const sd  = document.getElementById('settSd');       if (sd)  sd.textContent  = fmtMB(data.sdUsed) + ' / ' + fmtMB(data.sdTotal);
            const up  = document.getElementById('settUptime');   if (up)  up.textContent  = fmtUptime(data.uptime);
            // v4.10: seed the live-tick baselines so uptime advances every 1s.
            __liveUptimeSec  = data.uptime || 0;
            __liveUptimeSeen = performance.now();
            __liveCpuPct     = data.cpuBusyPct || 0;
            __liveCpuMhz     = data.cpuMhz || 0;
            __liveCpuCores   = data.cpuCores || 1;
        }).catch(() => {});
    };
})();

// ---------------------------------------------------------------
// v4.10: live-updating uptime + CPU% in the Settings stats grid.
// v4.11: adds live CPU die temperature with smooth color fade + overheat
// warning banner.
// ---------------------------------------------------------------
let __liveUptimeSec  = 0;
let __liveUptimeSeen = performance.now();
let __liveCpuPct     = 0;
let __liveCpuMhz     = 0;
let __liveCpuCores   = 1;
let __liveCpuTempC   = 0;
let __liveThermal    = 0;   // 0=normal 1=warm 2=hot 3=critical
let __liveThermalDown = false;

// Map thermal state to a colour + label. The transition itself (1 s ease) is
// declared on the tile CSS; here we just SET the values and the browser
// interpolates between them smoothly. No abrupt colour swaps.
function tempColorForState(state) {
    switch (state) {
        case 3: return { bg: 'rgba(220, 38, 38, 0.85)',  text: '#fff',    border: '#dc2626', label: 'CRITICAL' };
        case 2: return { bg: 'rgba(234, 88, 12, 0.75)',  text: '#fff',    border: '#ea580c', label: 'HOT' };
        case 1: return { bg: 'rgba(202, 138, 4, 0.65)',  text: '#fff8d0', border: '#ca8a04', label: 'WARM' };
        default: return { bg: '',                          text: '',       border: '',        label: 'OK' };
    }
}

// Big top-of-page overheat banner. Shown only in HOT (2) and CRITICAL (3).
function updateOverheatBanner(state, tempC, isShutdown) {
    let b = document.getElementById('overheatBanner');
    if (state < 2 && !isShutdown) { if (b) b.remove(); return; }
    if (!b) {
        b = document.createElement('div');
        b.id = 'overheatBanner';
        b.style.cssText =
            'position:fixed;top:0;left:0;right:0;z-index:99998;padding:12px 20px;' +
            'font-weight:600;text-align:center;font-size:14px;' +
            'transition:background-color 1s ease, color 1s ease;box-shadow:0 4px 20px rgba(0,0,0,0.4);';
        document.body.appendChild(b);
    }
    if (isShutdown || state === 3) {
        b.style.background = '#dc2626';
        b.style.color = '#fff';
        b.textContent = 'CRITICAL OVERHEAT (' + tempC.toFixed(1) + ' C) - device is in shutdown mode. USB and WiFi are off. Unplug to cool, then reconnect.';
    } else if (state === 2) {
        b.style.background = '#ea580c';
        b.style.color = '#fff';
        b.textContent = 'Overheating (' + tempC.toFixed(1) + ' C) - CPU throttled to 80 MHz and Bluetooth disabled. Reduce load or unplug briefly.';
    }
}

// Fold in updates from any /api/stats poll (updateStats() runs every 5 s).
(function () {
    const origUpdateStats = window.updateStats;
    if (typeof origUpdateStats !== 'function') return;
    window.updateStats = function () {
        // We can't intercept the fetch inside updateStats; just do our own
        // lightweight /api/stats poll to refresh the live values.
        fetch('/api/stats', { cache: 'no-store' })
            .then(r => r.json())
            .then(d => {
                __liveUptimeSec  = d.uptime || 0;
                __liveUptimeSeen = performance.now();
                __liveCpuPct     = d.cpuBusyPct || 0;
                __liveCpuMhz     = d.cpuMhz || __liveCpuMhz;
                __liveCpuCores   = d.cpuCores || __liveCpuCores;
                __liveCpuTempC   = (typeof d.cpuTempC === 'number') ? d.cpuTempC : __liveCpuTempC;
                __liveThermal    = d.thermalState || 0;
                __liveThermalDown = !!d.thermalShutdown;
                // v4.12: cache current firmware version for the .espkg
                // downgrade-warning banner.
                if (d.fwVersion) window.__currentFwVersion = d.fwVersion;
                // v4.16: sketch usage stats.
                if (d.sketchSize    != null) window.__sketchSize    = d.sketchSize;
                if (d.sketchTotal   != null) window.__sketchTotal   = d.sketchTotal;
                if (d.sketchUsedPct != null) window.__sketchUsedPct = d.sketchUsedPct;
            })
            .catch(() => {});
        return origUpdateStats.apply(this, arguments);
    };
})();

// 1 Hz ticker: advance uptime by 1 s and repaint the tiles. Runs forever
// after DOMContentLoaded - cheap (a few DOM writes per second).
document.addEventListener('DOMContentLoaded', () => {
    setInterval(() => {
        const elapsed = Math.floor((performance.now() - __liveUptimeSeen) / 1000);
        const uptime = __liveUptimeSec + elapsed;

        const up  = document.getElementById('settUptime');
        if (up)  up.textContent  = fmtUptime(uptime);

        const cpu = document.getElementById('settCpu');
        if (cpu) cpu.textContent = __liveCpuMhz + ' MHz - ' + __liveCpuCores + 'x - ' + __liveCpuPct + '% busy';

        // v4.16: flash-used tile (sketch size / app-partition size).
        const flashUsed = document.getElementById('settFlashUsed');
        if (flashUsed && window.__sketchSize != null) {
            flashUsed.textContent = fmtMB(window.__sketchSize) + ' / ' + fmtMB(window.__sketchTotal) +
                                    ' (' + window.__sketchUsedPct + '%)';
        }

        // v4.11: live temp tile with smooth color fade (CSS transition on
        // background-color + color + border-color takes care of the fade).
        const tempTile = document.getElementById('settTempCard');
        const tempVal  = document.getElementById('settTemp');
        if (tempTile && tempVal) {
            const t = Number(__liveCpuTempC) || 0;
            const c = tempColorForState(__liveThermal);
            if (t > 0) {
                tempVal.textContent = t.toFixed(2) + ' C - ' + c.label;   // v4.16: 2 decimals
            } else {
                tempVal.textContent = '- - unread';
            }
            tempTile.style.backgroundColor = c.bg;
            tempTile.style.borderColor     = c.border;
            tempVal.style.color            = c.text;
        }
        updateOverheatBanner(__liveThermal, Number(__liveCpuTempC) || 0, __liveThermalDown);

        // Also live-update the Stats tab's uptime tile if it exists.
        const upStats = document.getElementById('uptime');
        if (upStats) upStats.textContent = fmtUptime(uptime);

        // v4.16: 1 Hz temp poll via the lightweight /api/temp endpoint. The
        // /api/stats poll is every 5 s (too slow for a live temp reading);
        // /api/temp is a ~40-byte response so hitting it every second is
        // cheap and the tile now updates once a second including the decimal.
        fetch('/api/temp', { cache: 'no-store' })
            .then(r => r.json())
            .then(t => {
                if (typeof t.tempC === 'number') __liveCpuTempC = t.tempC;
                if (typeof t.state === 'number') __liveThermal  = t.state;
                if (typeof t.shutdown === 'boolean') __liveThermalDown = t.shutdown;
            })
            .catch(() => {});
    }, 1000);
});

// ============================================================
// Danger-zone handlers (v4.4)
// index.html has always had `onclick="factoryReset()"` /
// `onclick="clearErrorLog()"` / `onclick="selfDestructPrompt()"` on the
// Danger buttons, but the functions never actually existed in script.js -
// which is why the user reported the buttons "did nothing". Wiring them up.
// ============================================================
function clearErrorLog() {
    if (!confirm('Clear the error log and reset the error counter to 0?\n\nThis does NOT touch scripts, files, or settings.')) return;
    fetch('/api/clear-errors', { method: 'POST' })
        .then(r => r.text())
        .then(msg => {
            const el = document.getElementById('lastError');
            if (el) el.textContent = 'None';
            const ec = document.getElementById('errorCount');
            if (ec) ec.textContent = '0';
            alert('✅ ' + msg);
        })
        .catch(e => alert('❌ Failed: ' + e.message));
}

function factoryReset() {
    const ok = confirm(
        '⚠️  FACTORY RESET\n\n' +
        'Wipes every user script, upload, and log on the SD card, plus every ' +
        'saved WiFi credential, AP password, USB VID/PID, ATTACKMODE, and ' +
        'boot script.\n\n' +
        'The website (index.html, style.css, script.js) is KEPT so the ' +
        'dashboard still works after reboot.\n\n' +
        'The LED will blink blue while wiping.\n\n' +
        'Continue?'
    );
    if (!ok) return;
    if (!confirm('Really? This cannot be undone.')) return;

    // Show a big blocking overlay while the reset runs.
    const overlay = document.createElement('div');
    overlay.id = 'factoryResetOverlay';
    overlay.style.cssText =
        'position:fixed;inset:0;z-index:99999;background:rgba(0,0,0,0.85);' +
        'display:flex;align-items:center;justify-content:center;flex-direction:column;color:#fff;font-size:16px;';
    overlay.innerHTML =
        '<div style="font-size:40px;margin-bottom:12px;animation:pulseBlue 1s infinite;">🔵</div>' +
        '<div style="font-weight:600;">Factory reset in progress…</div>' +
        '<div style="opacity:0.8;margin-top:6px;font-size:13px;">LED is blinking blue on the device. Reboots in ~5 s.</div>' +
        '<div id="factoryCountdown" style="margin-top:10px;font-size:12px;opacity:0.7;"></div>';
    document.body.appendChild(overlay);
    if (!document.getElementById('pulseBlueKf')) {
        const s = document.createElement('style');
        s.id = 'pulseBlueKf';
        s.textContent = '@keyframes pulseBlue{0%,100%{opacity:1;transform:scale(1);}50%{opacity:0.4;transform:scale(0.85);}}';
        document.head.appendChild(s);
    }

    fetch('/api/factory-reset', { method: 'POST' })
        .then(r => r.text())
        .then(msg => {
            const cd = document.getElementById('factoryCountdown');
            let n = 15;
            const iv = setInterval(() => {
                if (cd) cd.textContent = 'Reconnecting in ' + n + ' s…';
                if (--n <= 0) { clearInterval(iv); location.reload(true); }
            }, 1000);
        })
        .catch(() => {
            // The socket dying mid-request IS the success signal - the ESP
            // is rebooting. Just wait and reload.
            setTimeout(() => location.reload(true), 15000);
        });
}

function behaveBrokenPrompt() {
    const ok = confirm(
        '⚠️  BEHAVE BROKEN\n\n' +
        'The device will pretend to be a plain "SD_READER" USB stick on ' +
        'every boot - no HID, no WiFi, no web dashboard.\n\n' +
        'Web files stay on the SD card but are HIDDEN.\n\n' +
        'Recovery: hold the GPIO0 (reset) button for 5 seconds while ' +
        'plugging in, and the device boots normally again.\n\n' +
        'Continue?'
    );
    if (!ok) return;
    const password = prompt('Type the AP password to confirm:');
    if (password === null || password === '') return;

    fetch('/api/behave-broken', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ password: password })
    }).then(r => r.text().then(t => ({ status: r.status, body: t })))
      .then(res => {
        if (res.status === 200) {
            document.body.innerHTML =
                '<div style="height:100vh;display:flex;align-items:center;justify-content:center;flex-direction:column;background:#111;color:#8f8;font-family:monospace;padding:20px;text-align:center;">' +
                '<div style="font-size:60px;">💾</div>' +
                '<h2>SD_READER mode armed</h2>' +
                '<p>The device is rebooting. On next plug-in it will show up ' +
                'as a generic USB storage stick called "SD_READER".</p>' +
                '<p style="opacity:0.7;font-size:13px;">Recovery: hold GPIO0 for 5 s during a boot.</p>' +
                '</div>';
        } else {
            alert('Rejected: ' + res.body);
        }
      })
      .catch(() => {
        document.body.innerHTML =
            '<div style="height:100vh;display:flex;align-items:center;justify-content:center;flex-direction:column;background:#111;color:#8f8;font-family:monospace;padding:20px;text-align:center;">' +
            '<div style="font-size:60px;">💾</div><h2>Device rebooting</h2>' +
            '<p>Should come back as "SD_READER" on next plug-in.</p></div>';
      });
}

function selfDestructPrompt() {
    const ok = confirm(
        '☢️  SELF DESTRUCT\n\n' +
        'This will CORRUPT the firmware.  The device will NOT BOOT again ' +
        'until you reflash it over USB with esptool.\n\n' +
        'Everything is wiped: SD card contents (including this website), ' +
        'all NVS credentials, and BOTH OTA app partitions.\n\n' +
        'To confirm, you will be asked for the AP password.\n\n' +
        'Continue?'
    );
    if (!ok) return;
    const password = prompt('Type the AP password to confirm self-destruct:');
    if (password === null || password === '') return;
    if (!confirm('LAST CHANCE. This will BRICK the device. Reflash required. Proceed?')) return;

    fetch('/selfdestruct', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ password: password })
    })
        .then(r => r.text().then(t => ({ status: r.status, body: t })))
        .then(res => {
            if (res.status === 200) {
                document.body.innerHTML =
                    '<div style="height:100vh;display:flex;align-items:center;justify-content:center;flex-direction:column;background:#111;color:#f88;font-family:monospace;padding:20px;text-align:center;">' +
                    '<div style="font-size:60px;">☠</div>' +
                    '<h2>Self-destruct initiated</h2>' +
                    '<p>Firmware and SD are being wiped.<br>The device will not boot again until reflashed.</p>' +
                    '<p style="opacity:0.6;font-size:12px;">Recovery: hold GPIO0, replug USB, run <code>esptool.py write_flash</code>.</p>' +
                    '</div>';
            } else {
                alert('Self-destruct rejected: ' + res.body);
            }
        })
        .catch(() => {
            // Same pattern as factory reset - dropped socket ≈ success.
            document.body.innerHTML =
                '<div style="height:100vh;display:flex;align-items:center;justify-content:center;flex-direction:column;background:#111;color:#f88;font-family:monospace;padding:20px;text-align:center;">' +
                '<div style="font-size:60px;">☠</div><h2>Device unreachable</h2>' +
                '<p>Assumed self-destruct in progress. Reflash to recover.</p></div>';
        });
}

// ============================================================
// EPIC INTERACTIVE TUTORIAL (v4.10)
// ============================================================
// Full-screen dim + backdrop blur with a shape spotlight (circle, rect, or
// pointer-arrow) cut through the mask over the currently-highlighted element.
// Steps DEMONSTRATE features, not just describe them - they fill in demo
// content, click tab buttons, ask Yes/No prompts, and can group multiple
// elements into one rounded-rect focus.
//
// Design principles:
//   * SOLID stroke, no dashes.
//   * Fast 0.15s transitions so tab switches don't feel laggy.
//   * data-tour="..." attributes in the HTML give the tutorial stable
//     selectors instead of brittle nth-of-type CSS queries.
//   * Each step's `type` chooses the highlight shape.
//   * `action` runs a real demo (click, type, focus).
//   * `prompt` shows a "Ready to try X?" question with a green "Yes, safe
//     to try" and a red "No way" button. `onYes`/`onNo` are step indexes
//     to jump to based on the answer.
// ============================================================
// Full-screen dim + backdrop-blur, with a transparent circular spotlight
// cut over the currently-highlighted element. As the tour advances it
// programmatically clicks tab buttons so the walk-through actually navigates
// through the UI (not just reads text). Skip / Back / Next controls; can be
// relaunched anytime from Settings via the "Restart tutorial" button.
// Em-dashes intentionally NOT used in any user-facing text.

// Step schema:
//   selector : CSS selector string, OR array of selectors (grouped bounding rect).
//   type     : 'circle' (default) | 'rect' | 'pointer'
//   padding  : pixels around the target for the shape
//   scroll   : scroll target into center before spotlighting
//   title    : caption card title (no em dashes anywhere)
//   body     : caption card HTML body
//   action   : 'click' | 'type' | 'focus' | null - runs BEFORE the spotlight moves
//   actionData : { target, text } used by the action
//   prompt   : optional { text, yes, no } - shows Yes/No buttons
//     yes    : label for the green button (default "Yes, safe to try")
//     no     : label for the red button   (default "No way")
//     onYes  : (optional) step index to jump to on Yes (else next)
//     onNo   : (optional) step index to jump to on No  (else next)
const TUTORIAL_STEPS = [
    { title: 'Welcome to ESP32-S3 BadUSB',
      body:  'This tiny board acts like a USB keyboard, optionally as a flash drive, and you control it from this dashboard. Quick tour, about 40 seconds. Every step demonstrates the feature live. Skip anytime.',
      selector: '.container h1', type: 'rect', padding: 24 },

    // ---- Coding tab: click into it, then DEMO fill a script -------------
    { title: 'The Coding tab', action: 'click',
      body:  'This is where you write payloads in DuckyScript. Watch the next step for a live demo.',
      selector: '[data-tour-tab="Script"]', type: 'circle', padding: 10 },

    { title: 'DuckyScript demo',
      action: 'type',
      actionData: { target: '#scriptArea',
                    text: 'REM Hello from the tutorial\nDELAY 300\nSTRING Hello from BadUSB!\nENTER\n' },
      body:  'A tiny DuckyScript filled in for you. STRING types text, ENTER hits Return, DELAY sleeps ms. Autocomplete pops up as you type; try ATTACKMODE and see the arg choices.',
      selector: '.editor-wrapper', type: 'rect', padding: 10, scroll: true },

    { title: 'Ready to run it?',
      body:  'If your ESP is plugged into a host computer, it will TYPE THIS SCRIPT into whatever app has focus on that host. Only tap Yes when your target window is ready. Tapping Yes actually POSTs the script to /execute and the device types it.',
      selector: '.control-panel', type: 'rect', padding: 10,
      prompt: { text: 'Send the demo keystrokes now?', yes: 'Yes, safe to try', no: 'No way', runOnYes: true } },

    // ---- Live tab: point at the textarea with a pointer arrow -----------
    { title: 'Live typing tab', action: 'click',
      body:  'Wireless keyboard mode. Every key you press in the box gets forwarded to the host instantly.',
      selector: '[data-tour-tab="Live"]', type: 'circle', padding: 10 },

    { title: 'Focus and type', action: 'focus',
      actionData: { target: '#liveInput' },
      body:  'This textarea is focused now. Type anywhere and the ESP relays the keystroke over USB HID in real time. Enter, Backspace, Tab, arrows, Esc all forward too.',
      selector: '#liveInput', type: 'pointer', padding: 6 },

    // ---- Scripts + Explorer ---------------------------------------------
    { title: 'Scripts library', action: 'click',
      body:  'Saved DuckyScript payloads on the SD card. Load them into the editor, run them, save new ones. Long-lived attack chains live here.',
      selector: '[data-tour-tab="Scripts"]', type: 'circle', padding: 10 },

    { title: 'File explorer', action: 'click',
      body:  'Whole SD card as a browser: drag and drop upload, delete, rename, copy, paste, mkdir. This is what the host sees when ATTACKMODE STORAGE is on.',
      selector: '[data-tour-tab="File_Manager"]', type: 'circle', padding: 10 },

    { title: 'Boot tab', action: 'click',
      body:  'Pick one or more scripts to auto-run the moment a client joins the WiFi AP. Turns your phone into a physical launch button.',
      selector: '[data-tour-tab="Boot"]', type: 'circle', padding: 10 },

    // ---- Settings: click, then group-highlight sections -----------------
    { title: 'Settings tab', action: 'click',
      body:  'Everything else. Grouped into WiFi, toggles, USB identity, ATTACKMODE, updates, danger zone. Next steps zoom into the ones that matter.',
      selector: '[data-tour-tab="Settings"]', type: 'circle', padding: 10 },

    { title: 'Toggles',
      body:  'Hardware LED, Bluetooth, Bluetooth Discovery, Silent Startup, Randomize USB VID/PID, and Allow COM connections. Silent Startup being ON by default is why the ESP does not chime when you plug it in.',
      selector: '[data-tour="toggles-header"]', type: 'rect', padding: 220, scroll: true },

    { title: 'Silent Startup',
      body:  'The USB PHY is dead at boot so Windows never enumerates the device. HID attaches only when a script or Live typing needs it, then detaches. This is what makes the key stealth.',
      selector: '[data-tour="silent-row"]', type: 'rect', padding: 8, scroll: true },

    { title: 'ATTACKMODE quick-set',
      body:  'Fast USB composition switch. HID only = keyboard. STORAGE only (no HID) = flash drive. HID + STORAGE = both. BLANK = fully off USB. Each change reboots.',
      selector: '[data-tour="attackmode-buttons"]', type: 'rect', padding: 10, scroll: true },

    { title: 'Firmware and Website Update',
      body:  'Drop a bundled .espkg file to update firmware and web UI in one step. LED blinks BLUE while applying, and a big banner appears when done.',
      selector: '#espkgDropZone', type: 'rect', padding: 10, scroll: true },

    { title: 'Danger Zone',
      body:  'Clear Errors resets counters. Factory Reset wipes scripts/uploads/logs + all NVS but keeps the website (this tutorial reappears). Behave Broken pretends to be a plain SD_READER stick with the real files hidden in a sub-region (double-press GPIO0 within 3 s at boot to recover). Self Destruct 3-pass wipes both OTA app partitions, needs a reflash.',
      selector: '[data-tour="danger-buttons"]', type: 'rect', padding: 12, scroll: true },

    { title: 'Syntaxes reference', action: 'click',
      body:  'Every DuckyScript command with a one-line explanation. Click any row to copy it to your clipboard.',
      selector: '[data-tour-tab="Wiki"]', type: 'circle', padding: 10 },

    { title: 'You are set',
      body:  'Replay this tutorial anytime from Settings, under Tutorial. Have fun. Break things. Recover with a double-press. And remember: with great HID power comes great "sorry that was me on the shared PC" energy.',
      selector: '.container h1', type: 'rect', padding: 24 },
];
let tutorialIndex = 0;
// v4.12 FIX: track whether the tutorial is genuinely open. Without this
// flag, the resize/scroll listeners rebuild the overlay after finishTutorial
// has removed it - so tapping into a textbox or scrolling any other tab
// silently RE-CREATED the tutorial overlay with the last step's Title/Body.
// That was the crucial bug the user reported ("starts again after finishing
// it, and also starts when pressing a textbox or scrolling").
let __tutorialActive = false;

// ---- Overlay DOM ----------------------------------------------------------
// The mask is 3 shape primitives (circle, rect, arrow-line) that we
// show/hide per step. All three sit inside one SVG <mask> so the dim layer's
// "hole" reflects the shape we selected. The visible SOLID-stroked outline
// mirrors the same shape. Fast 0.15s CSS transitions on the caption card.
function ensureTutorialOverlay() {
    // v4.12: never recreate the overlay if the tutorial isn't actively
    // running. Prevents the "scroll/focus re-summons the tutorial" bug.
    if (!__tutorialActive) return null;
    let o = document.getElementById('tutorialOverlay');
    if (o) return o;

    // v4.14 big-change: mobile-first card. Instead of a floating box that
    // has to be measured and clamped against the spotlight, the card is a
    // FIXED bottom sheet on phones (max-height 55vh, its own scroll, tall
    // tap targets) and a normal floating popover on desktop. The switch is
    // triggered off matchMedia so it re-evaluates on resize.
    const isPhone = window.matchMedia('(max-width: 600px)').matches;

    o = document.createElement('div');
    o.id = 'tutorialOverlay';
    o.style.cssText = 'position:fixed;inset:0;z-index:100000;pointer-events:auto;' +
                       'touch-action:none;overscroll-behavior:contain;';
    o.innerHTML =
        '<svg id="tutSvg" style="position:absolute;inset:0;width:100%;height:100%;pointer-events:none;overflow:visible;">' +
            '<defs>' +
                '<mask id="tutMask">' +
                    '<rect width="100%" height="100%" fill="white"/>' +
                    '<circle id="tutMaskCircle" cx="-9999" cy="-9999" r="0" fill="black"/>' +
                    '<rect  id="tutMaskRect"  x="-9999" y="-9999" width="0" height="0" rx="12" ry="12" fill="black"/>' +
                '</mask>' +
            '</defs>' +
            '<rect width="100%" height="100%" fill="rgba(0,0,0,0.78)" mask="url(#tutMask)"/>' +
            '<circle id="tutOutlineCircle" cx="-9999" cy="-9999" r="0" fill="none" stroke="#4caf50" stroke-width="3" opacity="0.95" style="transition:cx 0.15s, cy 0.15s, r 0.15s;"/>' +
            '<rect  id="tutOutlineRect"    x="-9999" y="-9999" width="0" height="0" rx="12" ry="12" fill="none" stroke="#4caf50" stroke-width="3" opacity="0.95" style="transition:x 0.15s, y 0.15s, width 0.15s, height 0.15s;"/>' +
            '<g id="tutPointerGroup" style="display:none;">' +
                '<line id="tutPointerLine" x1="0" y1="0" x2="0" y2="0" stroke="#4caf50" stroke-width="3" opacity="0.95"/>' +
                '<circle id="tutPointerDot" cx="0" cy="0" r="7" fill="#4caf50" opacity="0.95"/>' +
            '</g>' +
        '</svg>';

    // Card. On phone we build a bottom-sheet layout (larger fonts, larger
    // taps, sticky bottom-action bar). On desktop we build the floating
    // popover the previous versions had.
    const cardCss = isPhone
        // Bottom sheet: pinned to bottom, safe-area padding, own scroll.
        ? 'position:fixed;left:0;right:0;bottom:0;top:auto;' +
          'background:#1e1e28;color:#fff;' +
          'border-radius:20px 20px 0 0;padding:16px 18px calc(16px + env(safe-area-inset-bottom, 0px));' +
          'box-shadow:0 -12px 40px rgba(0,0,0,0.6);border:1px solid rgba(255,255,255,0.08);border-bottom:none;' +
          'pointer-events:auto;max-height:60vh;overflow-y:auto;touch-action:auto;overscroll-behavior:contain;' +
          'transition:transform 0.2s ease;'
        // Desktop popover: floating box, positioned by tutPositionCard.
        : 'position:absolute;background:#1e1e28;color:#fff;border-radius:14px;' +
          'max-width:460px;width:calc(100% - 32px);padding:20px 22px;' +
          'box-shadow:0 20px 60px rgba(0,0,0,0.6);border:1px solid rgba(255,255,255,0.1);' +
          'pointer-events:auto;transition:top 0.15s ease, left 0.15s ease;';

    // Chevron handle on the phone card so it visually reads as a sheet.
    const chevron = isPhone
        ? '<div style="width:44px;height:4px;background:rgba(255,255,255,0.25);border-radius:2px;margin:0 auto 12px;"></div>'
        : '';

    // Button sizes: bigger touch targets on phone.
    const btnBase   = isPhone ? 'padding:14px 18px;font-size:15px;font-weight:600;' : 'padding:8px 14px;font-size:13px;';
    const btnBaseR  = isPhone ? 'padding:14px 20px;font-size:15px;font-weight:700;' : 'padding:8px 18px;font-size:13px;font-weight:600;';
    const promptBtn = isPhone ? 'padding:16px 18px;font-size:16px;font-weight:700;' : 'padding:10px 14px;font-size:13px;font-weight:600;';
    // Prompt row: stack vertically on phone for full-width tap targets.
    const promptFlex = isPhone ? 'flex-direction:column;' : 'flex-direction:row;';
    // Bottom action row: sticky-ish and full width on phone.
    const actionRow = isPhone
        ? 'display:flex;flex-direction:column;gap:8px;'
        : 'display:flex;justify-content:space-between;gap:8px;flex-wrap:wrap;';

    const card = document.createElement('div');
    card.id = 'tutCard';
    card.style.cssText = cardCss;
    card.innerHTML =
        chevron +
        '<div id="tutProgress" style="font-size:11px;opacity:0.55;margin-bottom:6px;">Step 1 of X</div>' +
        '<h2 id="tutTitle" style="margin:0 0 10px;font-size:' + (isPhone ? '22px' : '20px') + ';line-height:1.25;">Title</h2>' +
        '<div id="tutBody" style="font-size:' + (isPhone ? '15px' : '14px') + ';line-height:1.55;color:rgba(255,255,255,0.9);margin-bottom:14px;">Body</div>' +
        '<div id="tutPromptRow" style="display:none;margin-bottom:14px;gap:10px;' + promptFlex + '">' +
            '<button id="tutPromptYes" style="flex:1;border-radius:10px;background:#22c55e;color:#fff;border:none;cursor:pointer;' + promptBtn + '">Yes, safe to try</button>' +
            '<button id="tutPromptNo"  style="flex:1;border-radius:10px;background:#ef4444;color:#fff;border:none;cursor:pointer;' + promptBtn + '">No way</button>' +
        '</div>' +
        '<div style="' + actionRow + '">' +
            (isPhone
                // Phone: full-width primary Next on top, secondary row below.
                ? '<button id="tutNext" onclick="nextTutorial()" style="width:100%;border-radius:12px;background:#4caf50;color:#fff;border:none;cursor:pointer;' + btnBaseR + '">Next</button>' +
                  '<div style="display:flex;gap:8px;">' +
                      '<button id="tutBack" onclick="prevTutorial()" style="flex:1;border-radius:10px;background:#2a2a35;color:#fff;border:none;cursor:pointer;' + btnBase + '">Back</button>' +
                      '<button onclick="skipTutorial()" style="flex:1;border-radius:10px;background:transparent;color:#999;border:1px solid #444;cursor:pointer;' + btnBase + '">Skip</button>' +
                  '</div>'
                // Desktop: original horizontal layout.
                : '<button onclick="skipTutorial()" style="border-radius:8px;background:transparent;color:#999;border:1px solid #444;cursor:pointer;' + btnBase + '">Skip tutorial</button>' +
                  '<div style="display:flex;gap:8px;">' +
                      '<button id="tutBack" onclick="prevTutorial()" style="border-radius:8px;background:#333;color:#fff;border:none;cursor:pointer;' + btnBase + '">Back</button>' +
                      '<button id="tutNext" onclick="nextTutorial()" style="border-radius:8px;background:#4caf50;color:#fff;border:none;cursor:pointer;' + btnBaseR + '">Next</button>' +
                  '</div>'
            ) +
        '</div>';
    o.appendChild(card);
    document.body.appendChild(o);

    // v4.16 FIX: hoist the rerender closure to a module var so
    // finishTutorial() can removeEventListener it. Prior code created a new
    // closure per ensureTutorialOverlay call with no way to unbind - every
    // "Restart tutorial" stacked another pair that could never be removed,
    // and even after finishTutorial the scroll/resize listener stayed live
    // (albeit no-op because of __tutorialActive gate).
    if (!__tutRerender) __tutRerender = () => renderTutorialStep(true);
    window.addEventListener('resize', __tutRerender);
    window.addEventListener('scroll', __tutRerender, true);
    return o;
}
let __tutRerender = null;

// Compute the union of bounding rects for one selector OR an array of them.
function tutTargetRect(selOrArr) {
    const sels = Array.isArray(selOrArr) ? selOrArr : [selOrArr];
    let minL = Infinity, minT = Infinity, maxR = -Infinity, maxB = -Infinity;
    let found = false;
    for (const s of sels) {
        const el = document.querySelector(s);
        if (!el) continue;
        const r = el.getBoundingClientRect();
        if (r.width === 0 && r.height === 0) continue;
        if (r.left < minL)  minL = r.left;
        if (r.top < minT)   minT = r.top;
        if (r.right > maxR) maxR = r.right;
        if (r.bottom > maxB) maxB = r.bottom;
        found = true;
    }
    if (!found) return null;
    return { left: minL, top: minT, right: maxR, bottom: maxB, width: maxR - minL, height: maxB - minT };
}

function tutScrollIntoViewIfNeeded(selOrArr) {
    const sels = Array.isArray(selOrArr) ? selOrArr : [selOrArr];
    const el = document.querySelector(sels[0]);
    if (!el) return;
    const r = el.getBoundingClientRect();
    const vh = window.innerHeight;
    const isPhone = window.matchMedia('(max-width: 600px)').matches;
    if (isPhone) {
        // v4.14: on phone the bottom sheet eats ~60vh, so the target must
        // land in the top ~35vh. Scroll so it sits ~15vh from the top.
        const card = document.getElementById('tutCard');
        const sheetH = card ? card.offsetHeight : Math.round(vh * 0.6);
        const topZoneMax = vh - sheetH - 40;
        if (r.top < 20 || r.bottom > topZoneMax) {
            // Use block:start with a manual scroll offset via scrollBy after.
            el.scrollIntoView({ behavior: 'smooth', block: 'start' });
            // Then nudge so the element sits comfortably at ~15vh from top.
            setTimeout(() => {
                const r2 = el.getBoundingClientRect();
                const wanted = Math.max(20, Math.round(vh * 0.12));
                const dy = r2.top - wanted;
                if (Math.abs(dy) > 4) window.scrollBy({ top: dy, behavior: 'smooth' });
            }, 260);
        }
    } else {
        if (r.top < 40 || r.bottom > vh - 40) {
            el.scrollIntoView({ behavior: 'smooth', block: 'center' });
        }
    }
}

// Position the caption card so it never overlaps the spotlight.
// v4.12: mobile-aware sizing. On a phone (< 600 px wide) the card grows to
// full width, sits with tighter padding, and pins to the bottom-third to
// leave the spotlight breathing room. Font size stays legible via inline
// styles on the .tut* elements below.
function tutPositionCard(cx, cy, r) {
    const card = document.getElementById('tutCard');
    if (!card) return { top: 0, left: 0, cw: 0, ch: 0 };
    const vw = window.innerWidth, vh = window.innerHeight;
    const isPhone = window.matchMedia('(max-width: 600px)').matches;

    // v4.14: on phone the card is a FIXED bottom sheet (see ensureTutorialOverlay).
    // Don't touch its position - just report the effective top/height so the
    // pointer-arrow math (which draws from card top) still works.
    if (isPhone) {
        const ch = card.offsetHeight || 240;
        return { top: vh - ch, left: 0, cw: vw, ch: ch };
    }

    // Desktop popover: measure + clamp against the spotlight as before.
    const cw = card.offsetWidth || 440;
    const ch = card.offsetHeight || 200;
    const gap = 24;
    let left = Math.max(10, Math.min(vw - cw - 10, cx - cw / 2));
    let top;
    if (cy + r + gap + ch <= vh - 16)      top = cy + r + gap;
    else if (cy - r - gap - ch >= 16)      top = cy - r - gap - ch;
    else                                    top = Math.max(16, vh - ch - 16);
    card.style.top  = top + 'px';
    card.style.left = left + 'px';
    return { top, left, cw, ch };
}

// Render the current step's shape spotlight. All three shape primitives
// exist in the DOM; we hide the ones this step doesn't use and animate the
// active one to the target rect.
function tutRenderShape(step) {
    const rect = tutTargetRect(step.selector);
    const maskCircle = document.getElementById('tutMaskCircle');
    const maskRect   = document.getElementById('tutMaskRect');
    const olCircle   = document.getElementById('tutOutlineCircle');
    const olRect     = document.getElementById('tutOutlineRect');
    const pointerG   = document.getElementById('tutPointerGroup');
    const pointerLn  = document.getElementById('tutPointerLine');
    const pointerDot = document.getElementById('tutPointerDot');

    // Reset all shapes off-screen every render so only the active one shows.
    maskCircle.setAttribute('cx', -9999); maskCircle.setAttribute('cy', -9999); maskCircle.setAttribute('r', 0);
    maskRect  .setAttribute('x',  -9999); maskRect  .setAttribute('y', -9999); maskRect.setAttribute('width', 0); maskRect.setAttribute('height', 0);
    olCircle  .setAttribute('cx', -9999); olCircle  .setAttribute('cy', -9999); olCircle.setAttribute('r', 0);
    olRect    .setAttribute('x',  -9999); olRect    .setAttribute('y', -9999); olRect.setAttribute('width', 0); olRect.setAttribute('height', 0);
    pointerG.style.display = 'none';

    if (!rect) {
        tutPositionCard(window.innerWidth / 2, window.innerHeight / 2, 0);
        return;
    }
    const pad = step.padding || 12;
    const type = step.type || 'circle';
    const cx = rect.left + rect.width / 2;
    const cy = rect.top  + rect.height / 2;

    if (type === 'circle') {
        const r = Math.max(rect.width, rect.height) / 2 + pad;
        maskCircle.setAttribute('cx', cx); maskCircle.setAttribute('cy', cy); maskCircle.setAttribute('r', r);
        olCircle  .setAttribute('cx', cx); olCircle  .setAttribute('cy', cy); olCircle.setAttribute('r', r);
        tutPositionCard(cx, cy, r);
    } else if (type === 'rect') {
        const x = rect.left - pad, y = rect.top - pad;
        const w = rect.width + pad * 2, h = rect.height + pad * 2;
        maskRect.setAttribute('x', x); maskRect.setAttribute('y', y);
        maskRect.setAttribute('width', w); maskRect.setAttribute('height', h);
        olRect  .setAttribute('x', x); olRect  .setAttribute('y', y);
        olRect  .setAttribute('width', w); olRect  .setAttribute('height', h);
        // Use half-height as the "radius" for card positioning below the rect.
        tutPositionCard(cx, cy, h / 2 + 8);
    } else if (type === 'pointer') {
        // Small filled dot on the element + a line from the caption card's
        // top edge to the dot. A subtle circle cutout still lets the element
        // shine through the mask so it's not just an arrow to darkness.
        const holeR = Math.max(rect.width, rect.height) / 2 + pad;
        maskCircle.setAttribute('cx', cx); maskCircle.setAttribute('cy', cy); maskCircle.setAttribute('r', holeR);
        // Place the card first so we know where to draw FROM.
        const pos = tutPositionCard(cx, cy, holeR);
        pointerG.style.display = 'block';
        const cardCenterX = pos.left + pos.cw / 2;
        const cardTopY    = pos.top;
        // Line goes from card's top-center down to the element's top edge.
        const dotX = cx;
        const dotY = rect.top - 8;
        pointerLn.setAttribute('x1', cardCenterX);
        pointerLn.setAttribute('y1', cardTopY);
        pointerLn.setAttribute('x2', dotX);
        pointerLn.setAttribute('y2', dotY);
        pointerDot.setAttribute('cx', dotX);
        pointerDot.setAttribute('cy', dotY);
    }
}

// Run the step's action side-effect BEFORE the shape is drawn so the target
// (e.g. after tab-switch) is definitely in the DOM.
function tutRunAction(step) {
    if (!step.action) return;
    if (step.action === 'click') {
        const t = document.querySelector(step.selector);
        if (t && typeof t.click === 'function') t.click();
    } else if (step.action === 'type' && step.actionData) {
        const t = document.querySelector(step.actionData.target);
        if (t && step.actionData.text != null) {
            t.value = step.actionData.text;
            t.dispatchEvent(new Event('input', { bubbles: true }));
            t.dispatchEvent(new Event('change', { bubbles: true }));
        }
    } else if (step.action === 'focus' && step.actionData) {
        const t = document.querySelector(step.actionData.target);
        if (t && typeof t.focus === 'function') t.focus();
    }
}

function renderTutorialStep(retargetOnly) {
    // v4.12: guard - if the tour was closed (finishTutorial cleared the
    // flag), the resize/scroll listeners must not resurrect it.
    if (!__tutorialActive) return;
    const step = TUTORIAL_STEPS[tutorialIndex];
    if (!ensureTutorialOverlay()) return;

    if (!retargetOnly) {
        tutRunAction(step);
        // Progress + text.
        document.getElementById('tutProgress').textContent =
            'Step ' + (tutorialIndex + 1) + ' of ' + TUTORIAL_STEPS.length;
        document.getElementById('tutTitle').textContent = step.title;
        document.getElementById('tutBody').innerHTML   = step.body;
        document.getElementById('tutBack').style.visibility = tutorialIndex === 0 ? 'hidden' : 'visible';
        document.getElementById('tutNext').textContent =
            (tutorialIndex === TUTORIAL_STEPS.length - 1) ? 'Done' : 'Next';

        // Yes/No prompt row.
        const promptRow = document.getElementById('tutPromptRow');
        const nextBtn   = document.getElementById('tutNext');
        if (step.prompt) {
            promptRow.style.display = 'flex';
            nextBtn.style.display = 'none';
            const yesBtn = document.getElementById('tutPromptYes');
            const noBtn  = document.getElementById('tutPromptNo');
            yesBtn.textContent = step.prompt.yes || 'Yes, safe to try';
            noBtn.textContent  = step.prompt.no  || 'No way';
            yesBtn.onclick = () => {
                promptRow.style.display = 'none';
                nextBtn.style.display = '';
                // v4.14: runOnYes actually POSTs the current editor script
                // to /execute so the device types it. This is what makes the
                // "Yes, safe to try" step demonstrate rather than describe.
                if (step.prompt && step.prompt.runOnYes) {
                    try {
                        // executeScript() lives up in the file and grabs the
                        // scriptArea contents. Call it directly.
                        if (typeof executeScript === 'function') executeScript();
                    } catch (e) { console.error('tutorial Yes-run failed:', e); }
                }
                if (step.onYes != null) { tutorialIndex = step.onYes; renderTutorialStep(); }
                else nextTutorial();
            };
            noBtn.onclick = () => {
                promptRow.style.display = 'none';
                nextBtn.style.display = '';
                if (step.onNo != null) { tutorialIndex = step.onNo; renderTutorialStep(); }
                else nextTutorial();
            };
        } else {
            promptRow.style.display = 'none';
            nextBtn.style.display = '';
        }
    }

    // Give the tab-switch a moment to render, then scroll + draw.
    // v4.14: on phone we ALWAYS scroll (bottom sheet eats half the screen);
    // on desktop only when the step opts in.
    setTimeout(() => {
        const isPhone = window.matchMedia('(max-width: 600px)').matches;
        if (step.scroll || isPhone) tutScrollIntoViewIfNeeded(step.selector);
        setTimeout(() => tutRenderShape(step), isPhone ? 300 : 180);
    }, 40);
}

function startTutorial() {
    __tutorialActive = true;
    tutorialIndex = 0;
    __lockBodyScroll();     // v4.13: no background scroll behind the tutorial
    renderTutorialStep();
}
function showTutorial() { startTutorial(); }
function nextTutorial() {
    if (tutorialIndex >= TUTORIAL_STEPS.length - 1) { finishTutorial(); return; }
    tutorialIndex++; renderTutorialStep();
}
function prevTutorial() {
    if (tutorialIndex <= 0) return;
    tutorialIndex--; renderTutorialStep();
}
function skipTutorial() { finishTutorial(); }
function finishTutorial() {
    __tutorialActive = false;   // v4.12: flag OFF before we remove the overlay
    const o = document.getElementById('tutorialOverlay');
    if (o) o.remove();
    __unlockBodyScroll();       // v4.13: restore page scroll
    // v4.16: unhook the scroll/resize listener so it doesn't stay live for
    // the rest of the session. Only the ONE hoisted __tutRerender is
    // registered, so a single removeEventListener call is sufficient.
    if (__tutRerender) {
        try {
            window.removeEventListener('resize', __tutRerender);
            window.removeEventListener('scroll', __tutRerender, true);
        } catch (e) {}
    }
    fetch('/api/tutorial-done', { method: 'POST' }).catch(() => {});
    try { localStorage.setItem('espTutorialDone', '1'); } catch (e) {}
}

// ============================================================
// FIRST-BOOT SETUP WIZARD (v4.12) - runs BEFORE the tutorial.
// ============================================================
// EULA acceptance + AP naming + AP password + Randomize MAC toggle +
// Silent Startup + Randomize USB VID/PID. POSTs everything at the end to
// /api/setup-complete which persists to NVS and reboots. If the user has
// already been through it (server flag setup_done=true), skips straight
// to the tutorial (or straight into normal use if that's also done).

const SETUP_STEPS = [
    {
        id: 'eula',
        title: 'Before we start',
        subtitle: 'Read this. It matters.',
        // Note: raw HTML so we can format the EULA nicely.
        html:
            '<div style="font-size:13px;line-height:1.55;">' +
            'This device can act as a USB keyboard and mass storage. That means it can type ANYTHING into ' +
            'the computer it is plugged into, silently, in about a second.<br><br>' +
            'It is a security research tool. It is not a toy, not a "cracking" tool, not for stealing ' +
            'passwords off other people\'s laptops, and not for anything you would not do to your own computer ' +
            'in front of a lawyer.<br><br>' +
            'By continuing you agree to:<br>' +
            '- Only use it on computers you own or have written permission to test.<br>' +
            '- Follow the laws of your country. Unauthorized access is a crime almost everywhere.<br>' +
            '- Take full responsibility for whatever you do with it. The author does not.<br><br>' +
            'If any of that is a dealbreaker, unplug now and put the device away.' +
            '</div>',
        controls: [
            { kind: 'checkbox', id: 'eulaOk', label: 'I read this and I accept.' }
        ],
        // "Next" is disabled until the checkbox is ticked.
        gateOn: 'eulaOk'
    },
    {
        id: 'apname',
        title: 'Name your device',
        subtitle: 'This is the WiFi network name you will see on your phone.',
        controls: [
            { kind: 'choice', id: 'apMode', label: 'Access-point SSID',
              options: [
                  { value: 'default', label: 'Keep default (ESP32-S3 BadUSB)' },
                  { value: 'random',  label: 'Randomize (e.g. ESP32-6f4a)' },
                  { value: 'custom',  label: 'Set my own' }
              ], defaultValue: 'default' },
            { kind: 'text', id: 'apSsid', label: 'Custom SSID', placeholder: 'e.g. LabRat', maxlen: 30, showIf: { apMode: 'custom' } }
        ]
    },
    {
        id: 'appass',
        title: 'AP password',
        subtitle: 'Keep it strong. Anyone with this password can drive the device wirelessly.',
        controls: [
            { kind: 'choice', id: 'apPassMode', label: 'Password',
              options: [
                  { value: 'default', label: 'Keep default (BadUSB123!)' },
                  { value: 'random',  label: 'Randomize (12 chars)' },
                  { value: 'custom',  label: 'Set my own (min 8 chars)' }
              ], defaultValue: 'random' },
            { kind: 'password', id: 'apPass', label: 'New password', placeholder: 'min 8 chars', maxlen: 40, showIf: { apPassMode: 'custom' } }
        ]
    },
    {
        id: 'silent',
        title: 'Silent startup',
        subtitle: 'The USB PHY is dead at boot so Windows never sees a device attach. Recommended ON.',
        controls: [
            { kind: 'toggle', id: 'silentOn', label: 'Silent startup (stealth HID)', defaultValue: true }
        ]
    },
    {
        id: 'usbRnd',
        title: 'Randomize USB identity',
        subtitle: 'Every boot rolls a new USB VID/PID. Makes the device less fingerprintable across sessions.',
        controls: [
            { kind: 'toggle', id: 'usbRandom', label: 'Randomize USB VID and PID at every boot', defaultValue: true }
        ]
    },
    {
        id: 'macRnd',
        title: 'Randomize WiFi MAC',
        subtitle: 'When the ESP joins another WiFi network (via /api/join-internet), use a randomized MAC address instead of the factory one. Also togglable later in Settings.',
        controls: [
            { kind: 'toggle', id: 'randomMac', label: 'Randomize MAC address on STA connect', defaultValue: true }
        ]
    },
    {
        id: 'summary',
        title: 'Ready to save',
        subtitle: 'Review, then hit Save & reboot. The ESP will restart with these settings and open the tutorial.',
        html: '<div id="setupSummary" style="font-size:13px;line-height:1.7;color:rgba(255,255,255,0.9);"></div>',
        controls: []
    }
];

let __setupIndex = 0;
let __setupState = {};   // holds all control values across steps

// v4.15: Robust clipboard copy for insecure HTTP context AND Android's
// captive-portal mini-browser, both of which routinely have
// navigator.clipboard === undefined. Falls through to a hidden-textarea +
// document.execCommand('copy') technique that works everywhere.
function safeCopyText(text) {
    try {
        if (navigator.clipboard && window.isSecureContext) {
            return navigator.clipboard.writeText(text).then(() => true).catch(() => __copyFallback(text));
        }
    } catch (e) {}
    return Promise.resolve(__copyFallback(text));
}
function __copyFallback(text) {
    try {
        const ta = document.createElement('textarea');
        ta.value = text;
        // Positioned so it doesn't scroll or trigger keyboard.
        ta.style.cssText = 'position:fixed;top:-1000px;left:0;opacity:0;pointer-events:none;';
        ta.setAttribute('readonly', '');
        document.body.appendChild(ta);
        ta.select();
        ta.setSelectionRange(0, text.length);
        const ok = document.execCommand('copy');
        document.body.removeChild(ta);
        return ok;
    } catch (e) { return false; }
}

// Escape user text for safe HTML injection in the summary.
function escapeHtml(s) {
    return String(s == null ? '' : s)
        .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}
// One-click copy from a <code> element by id. Shows a brief "Copied" state.
function setupCopy(elId, btn) {
    const el = document.getElementById(elId);
    if (!el) return;
    const text = el.textContent || '';
    const orig = btn.textContent;
    safeCopyText(text).then(ok => {
        btn.textContent = ok ? 'Copied' : 'Failed';
        btn.style.background = ok ? '#22c55e' : '#ef4444';
        setTimeout(() => { btn.textContent = orig; btn.style.background = '#333'; }, 1200);
    });
}

// Render random helpers
function __setupRandSsid() {
    const hex = Math.floor(Math.random() * 0xffff).toString(16).padStart(4, '0');
    return 'ESP32-' + hex;
}
function __setupRandPassword() {
    const pool = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789abcdefghjkmnpqrstuvwxyz';
    let p = '';
    for (let i = 0; i < 12; i++) p += pool[Math.floor(Math.random() * pool.length)];
    return p;
}

function ensureSetupOverlay() {
    let o = document.getElementById('setupOverlay');
    if (o) return o;
    o = document.createElement('div');
    o.id = 'setupOverlay';
    // v4.13: `touch-action: none` on the overlay backdrop blocks touch scroll
    // from bubbling to the underlying document. Combined with the body scroll
    // lock in startSetupWizard(), the background page can no longer scroll
    // while the wizard is open. The inner card overrides touch-action so its
    // OWN content is still scrollable when it overflows.
    o.style.cssText =
        'position:fixed;inset:0;z-index:100001;background:rgba(0,0,0,0.85);' +
        'display:flex;align-items:center;justify-content:center;padding:16px;' +
        'touch-action:none;overscroll-behavior:contain;';
    o.innerHTML =
        // touch-action:auto on the card so its internal scroll works even
        // though the backdrop blocks it. overscroll-behavior:contain stops a
        // scroll at the card's edge from chaining back to the body.
        '<div id="setupCard" style="background:#1e1e28;color:#fff;border-radius:16px;max-width:520px;width:100%;' +
        'padding:24px 26px;box-shadow:0 20px 60px rgba(0,0,0,0.6);border:1px solid rgba(255,255,255,0.1);' +
        'max-height:90vh;overflow-y:auto;touch-action:auto;overscroll-behavior:contain;">' +
            '<div id="setupProgress" style="font-size:11px;opacity:0.55;margin-bottom:6px;">Step 1 of X</div>' +
            '<h2 id="setupTitle" style="margin:0 0 4px;font-size:22px;line-height:1.2;">Title</h2>' +
            '<div id="setupSubtitle" style="font-size:13px;opacity:0.7;margin-bottom:16px;"></div>' +
            '<div id="setupBody" style="margin-bottom:18px;"></div>' +
            '<div style="display:flex;justify-content:space-between;gap:8px;">' +
                '<button id="setupBack" onclick="setupPrev()" style="padding:10px 16px;border-radius:8px;background:#333;color:#fff;border:none;cursor:pointer;font-size:14px;">Back</button>' +
                '<button id="setupNext" onclick="setupNext()" style="padding:10px 20px;border-radius:8px;background:#22c55e;color:#fff;border:none;cursor:pointer;font-size:14px;font-weight:600;">Next</button>' +
            '</div>' +
        '</div>';
    document.body.appendChild(o);
    return o;
}

function setupRender() {
    const step = SETUP_STEPS[__setupIndex];
    ensureSetupOverlay();
    document.getElementById('setupProgress').textContent = 'Step ' + (__setupIndex + 1) + ' of ' + SETUP_STEPS.length;
    document.getElementById('setupTitle').textContent = step.title;
    document.getElementById('setupSubtitle').textContent = step.subtitle || '';

    const body = document.getElementById('setupBody');
    body.innerHTML = step.html || '';

    // Render controls.
    (step.controls || []).forEach(c => {
        const wrap = document.createElement('div');
        wrap.style.cssText = 'margin-top:12px;';
        wrap.dataset.controlId = c.id;
        if (c.kind === 'text' || c.kind === 'password') {
            wrap.innerHTML =
                '<label style="display:block;font-size:12px;opacity:0.7;margin-bottom:4px;">' + c.label + '</label>' +
                '<input type="' + (c.kind === 'password' ? 'password' : 'text') + '" ' +
                'id="setup_' + c.id + '" placeholder="' + (c.placeholder||'') + '" ' +
                (c.maxlen ? ('maxlength="' + c.maxlen + '"') : '') + ' ' +
                'style="width:100%;padding:10px;border-radius:8px;border:1px solid #333;background:#111;color:#fff;font-size:14px;box-sizing:border-box;">';
        } else if (c.kind === 'choice') {
            let optsHtml = '';
            c.options.forEach(o => {
                const cur = (__setupState[c.id] || c.defaultValue) === o.value ? 'checked' : '';
                optsHtml +=
                    '<label style="display:flex;align-items:center;gap:8px;padding:8px 10px;background:#111;border-radius:8px;margin-bottom:6px;cursor:pointer;">' +
                    '<input type="radio" name="setup_' + c.id + '" value="' + o.value + '" ' + cur + ' onchange="setupOnChange()"> ' +
                    o.label + '</label>';
            });
            wrap.innerHTML =
                '<label style="display:block;font-size:12px;opacity:0.7;margin-bottom:6px;">' + c.label + '</label>' +
                optsHtml;
        } else if (c.kind === 'toggle') {
            const cur = (__setupState[c.id] != null ? __setupState[c.id] : c.defaultValue) ? 'checked' : '';
            wrap.innerHTML =
                '<label style="display:flex;align-items:center;justify-content:space-between;gap:12px;padding:10px 12px;background:#111;border-radius:8px;cursor:pointer;">' +
                '<span>' + c.label + '</span>' +
                '<input type="checkbox" id="setup_' + c.id + '" ' + cur + ' onchange="setupOnChange()" style="transform:scale(1.35);">' +
                '</label>';
        } else if (c.kind === 'checkbox') {
            const cur = __setupState[c.id] ? 'checked' : '';
            wrap.innerHTML =
                '<label style="display:flex;align-items:center;gap:10px;padding:10px 12px;background:#111;border-radius:8px;cursor:pointer;">' +
                '<input type="checkbox" id="setup_' + c.id + '" ' + cur + ' onchange="setupOnChange()" style="transform:scale(1.25);"> ' +
                '<span>' + c.label + '</span>' +
                '</label>';
        }
        body.appendChild(wrap);
    });

    // Rebind stored text values and showIf visibility.
    (step.controls || []).forEach(c => {
        const el = document.getElementById('setup_' + c.id);
        if (!el) return;
        if (c.kind === 'text' || c.kind === 'password') {
            if (__setupState[c.id] != null) el.value = __setupState[c.id];
            el.addEventListener('input', () => { __setupState[c.id] = el.value; setupUpdateVisibility(); setupUpdateNext(); });
        }
    });
    setupUpdateVisibility();
    setupUpdateNext();

    // Summary step: render current values.
    if (step.id === 'summary') {
        const s = __setupState;
        // v4.12 fix: generate the random SSID / password ONCE when the user
        // lands on the summary, cache them in state, and SHOW them in plain
        // text (with a copy button) so the user can write them down before
        // hitting "Save & reboot". Without this the summary just said
        // "(random on save)" and the user had no way to know the new creds.
        if (s.apMode === 'random' && !s.finalSsid) s.finalSsid = __setupRandSsid();
        if (s.apPassMode === 'random' && !s.finalPass) s.finalPass = __setupRandPassword();

        const ssid = (s.apMode === 'custom' ? (s.apSsid || '(empty)')
                   : s.apMode === 'random'  ? s.finalSsid
                   : 'ESP32-S3 BadUSB');
        const pw   = (s.apPassMode === 'custom' ? (s.apPass || '(empty)')
                   : s.apPassMode === 'random'  ? s.finalPass
                   : 'BadUSB123!');

        // Show plain text + "Copy" buttons. Also a big red warning to write
        // it down: after Save & reboot the AP restarts with the new creds
        // and the user has to reconnect using exactly this SSID and password.
        document.getElementById('setupSummary').innerHTML =
            '<div style="padding:10px 12px;margin-bottom:12px;background:rgba(220,38,38,0.15);' +
            'border:1px solid rgba(220,38,38,0.5);border-radius:8px;color:#f88;font-weight:600;font-size:13px;">' +
            '⚠ WRITE THIS DOWN NOW. After Save and reboot the WiFi restarts with the new SSID and password. ' +
            'If you lose them you will need to reflash to recover.' +
            '</div>' +
            '<div style="display:grid;grid-template-columns:auto 1fr auto;gap:8px 12px;align-items:center;">' +
                '<b>AP SSID</b>' +
                '<code id="setupFinalSsid" style="background:#111;padding:8px 10px;border-radius:6px;font-size:14px;word-break:break-all;">' + escapeHtml(ssid) + '</code>' +
                '<button onclick="setupCopy(\'setupFinalSsid\', this)" style="padding:6px 10px;border-radius:6px;background:#333;color:#fff;border:none;cursor:pointer;font-size:12px;">Copy</button>' +

                '<b>AP password</b>' +
                '<code id="setupFinalPass" style="background:#111;padding:8px 10px;border-radius:6px;font-size:14px;word-break:break-all;">' + escapeHtml(pw) + '</code>' +
                '<button onclick="setupCopy(\'setupFinalPass\', this)" style="padding:6px 10px;border-radius:6px;background:#333;color:#fff;border:none;cursor:pointer;font-size:12px;">Copy</button>' +
            '</div>' +
            '<div style="margin-top:14px;font-size:13px;line-height:1.7;color:rgba(255,255,255,0.9);">' +
                '<b>Silent startup:</b> ' + (s.silentOn ? 'ON' : 'OFF') + '<br>' +
                '<b>Randomize USB VID/PID:</b> ' + (s.usbRandom ? 'ON' : 'OFF') + '<br>' +
                '<b>Randomize WiFi MAC on STA connect:</b> ' + (s.randomMac ? 'ON' : 'OFF') +
            '</div>';
    }

    document.getElementById('setupBack').style.visibility = __setupIndex === 0 ? 'hidden' : 'visible';
    document.getElementById('setupNext').textContent =
        (__setupIndex === SETUP_STEPS.length - 1) ? 'Save and reboot' : 'Next';
}

function setupUpdateVisibility() {
    const step = SETUP_STEPS[__setupIndex];
    (step.controls || []).forEach(c => {
        if (!c.showIf) return;
        const key = Object.keys(c.showIf)[0];
        const want = c.showIf[key];
        const cur = __setupState[key];
        const wrap = document.querySelector('[data-control-id="' + c.id + '"]');
        if (wrap) wrap.style.display = (cur === want) ? '' : 'none';
    });
}

function setupUpdateNext() {
    const step = SETUP_STEPS[__setupIndex];
    const next = document.getElementById('setupNext');
    if (!next) return;
    // Gate: some steps require a specific value before Next enables.
    let ok = true;
    if (step.gateOn && !__setupState[step.gateOn]) ok = false;
    // Custom AP SSID required.
    if (step.id === 'apname' && __setupState.apMode === 'custom' && !(__setupState.apSsid || '').trim()) ok = false;
    // Custom AP password required (min 8).
    if (step.id === 'appass' && __setupState.apPassMode === 'custom' && (__setupState.apPass || '').length < 8) ok = false;
    next.disabled = !ok;
    next.style.opacity = ok ? '' : '0.4';
    next.style.cursor  = ok ? 'pointer' : 'not-allowed';
}

function setupOnChange() {
    const step = SETUP_STEPS[__setupIndex];
    (step.controls || []).forEach(c => {
        const el = document.getElementById('setup_' + c.id);
        if (!el) {
            // Radio group - read via name.
            if (c.kind === 'choice') {
                const sel = document.querySelector('input[name="setup_' + c.id + '"]:checked');
                if (sel) __setupState[c.id] = sel.value;
            }
            return;
        }
        if (c.kind === 'checkbox' || c.kind === 'toggle') {
            __setupState[c.id] = el.checked;
        } else {
            __setupState[c.id] = el.value;
        }
    });
    setupUpdateVisibility();
    setupUpdateNext();
}

function setupNext() {
    // Persist current step's values before moving on.
    setupOnChange();
    if (__setupIndex >= SETUP_STEPS.length - 1) {
        setupComplete();
        return;
    }
    __setupIndex++;
    setupRender();
}
function setupPrev() {
    setupOnChange();
    if (__setupIndex <= 0) return;
    __setupIndex--;
    setupRender();
}

function setupComplete() {
    // v4.12 fix: use the SAME random values the summary displayed. They were
    // cached in s.finalSsid / s.finalPass when the summary rendered. Without
    // this we would generate NEW randoms here and the user would be locked
    // out with a password they never saw.
    const s = __setupState;
    let ssid = 'ESP32-S3 BadUSB';
    if (s.apMode === 'custom')      ssid = (s.apSsid || '').trim();
    else if (s.apMode === 'random') ssid = s.finalSsid || __setupRandSsid();

    let pw = 'BadUSB123!';
    if (s.apPassMode === 'custom')      pw = s.apPass || '';
    else if (s.apPassMode === 'random') pw = s.finalPass || __setupRandPassword();

    const payload = {
        ssid: ssid,
        password: pw,
        silentStartup: !!s.silentOn,
        randomizeUsb:  !!s.usbRandom,
        randomizeMac:  !!s.randomMac
    };
    // Show reboot spinner - INCLUDE the plain-text password one more time so
    // the user has a last chance to write it down. This screen persists for
    // ~20 s (until the ESP finishes rebooting and the page reloads).
    document.getElementById('setupCard').innerHTML =
        '<div style="text-align:center;padding:8px 4px;">' +
            '<div style="font-size:38px;margin-bottom:6px;">💾</div>' +
            '<h2 style="margin:0 0 10px;">Saving and rebooting</h2>' +
            '<p style="opacity:0.8;font-size:13px;margin-bottom:14px;">' +
                'Reconnect to the new WiFi with these credentials when the LED goes green.' +
            '</p>' +
            '<div style="display:grid;grid-template-columns:auto 1fr auto;gap:6px 10px;align-items:center;text-align:left;margin-bottom:12px;">' +
                '<b>SSID</b>' +
                '<code id="rebootFinalSsid" style="background:#111;padding:8px 10px;border-radius:6px;font-size:14px;word-break:break-all;">' + escapeHtml(ssid) + '</code>' +
                '<button onclick="setupCopy(\'rebootFinalSsid\', this)" style="padding:6px 10px;border-radius:6px;background:#333;color:#fff;border:none;cursor:pointer;font-size:12px;">Copy</button>' +
                '<b>Password</b>' +
                '<code id="rebootFinalPass" style="background:#111;padding:8px 10px;border-radius:6px;font-size:14px;word-break:break-all;">' + escapeHtml(pw) + '</code>' +
                '<button onclick="setupCopy(\'rebootFinalPass\', this)" style="padding:6px 10px;border-radius:6px;background:#333;color:#fff;border:none;cursor:pointer;font-size:12px;">Copy</button>' +
            '</div>' +
            '<p style="opacity:0.7;font-size:12px;">The tutorial will pop up right after you reconnect.</p>' +
        '</div>';

    fetch('/api/setup-complete', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
    }).catch(() => {});
    // The ESP reboots ~1 s after this fires, so waiting for a response is
    // pointless - just show the spinner and let the user reconnect.
    // Note: DON'T __unlockBodyScroll() here - the spinner should keep the
    // background locked until the page reloads.
    setTimeout(() => { location.reload(true); }, 20000);
}

// v4.13: body-scroll lock helpers. iOS Safari + Android Chrome sometimes
// scroll the page BEHIND a full-screen overlay via touchmove. Setting
// overflow:hidden on <html>+<body> and remembering the scroll position lets
// us fully restore the page on close.
let __savedScrollY = 0;
function __lockBodyScroll() {
    try {
        __savedScrollY = window.scrollY || document.documentElement.scrollTop || 0;
        document.documentElement.style.overflow = 'hidden';
        document.body.style.overflow = 'hidden';
        document.body.style.position = 'fixed';
        document.body.style.top = -__savedScrollY + 'px';
        document.body.style.left = '0';
        document.body.style.right = '0';
    } catch (e) {}
}
function __unlockBodyScroll() {
    try {
        document.documentElement.style.overflow = '';
        document.body.style.overflow = '';
        document.body.style.position = '';
        document.body.style.top = '';
        document.body.style.left = '';
        document.body.style.right = '';
        window.scrollTo(0, __savedScrollY);
    } catch (e) {}
}

function startSetupWizard() {
    __setupIndex = 0;
    __setupState = {};
    __lockBodyScroll();
    setupRender();
}

// First-boot autoplay: run the setup wizard OR the tutorial depending on
// server state. setup_done=false -> wizard. setup_done=true & tutorial_done
// =false -> tutorial. Both done -> normal use.
document.addEventListener('DOMContentLoaded', () => {
    fetch('/api/stats', { cache: 'no-store' })
        .then(r => r.json())
        .then(d => {
            if (d.setupDone === false) {
                setTimeout(startSetupWizard, 300);
            } else if (d.firstBoot === true) {
                setTimeout(startTutorial, 500);
            }
        })
        .catch(() => {});
});



// ============================================================
// v4.17: Extensions tab (Hak5-compatible reusable payload snippets).
// Stored on the SD under /extensions/. Callable from any DuckyScript
// via `RUN_EXTENSION <name>`.
// ============================================================
let __currentExtension = null;

function refreshExtensions() {
    const box = document.getElementById('extList');
    if (!box) return;
    box.textContent = 'Loading...';
    fetch('/api/list-extensions').then(r => r.json()).then(data => {
        // v4.22: server now returns {hak5:[...], custom:[...], legacy:[...]}
        // Render two grouped sections + a legacy section if anything's left
        // at /extensions/ root from before v4.22.
        const hak5   = Array.isArray(data.hak5)   ? data.hak5   : [];
        const custom = Array.isArray(data.custom) ? data.custom : [];
        const legacy = Array.isArray(data.legacy) ? data.legacy : [];
        if (hak5.length === 0 && custom.length === 0 && legacy.length === 0) {
            box.innerHTML = '<div style="text-align:center;color:var(--text-muted);padding:20px;">No extensions yet. Pull one from URL or upload a .txt/.ext file below.</div>';
            return;
        }
        box.innerHTML = '';
        const renderSection = (title, items, folder, description) => {
            const wrap = document.createElement('div');
            wrap.style.cssText = 'margin-bottom:14px;';
            const h = document.createElement('div');
            h.style.cssText = 'display:flex;align-items:baseline;justify-content:space-between;padding:6px 4px;border-bottom:1px solid rgba(255,255,255,0.08);margin-bottom:6px;';
            h.innerHTML = '<b style="color:var(--primary);">' + title + '</b>' +
                          '<span style="font-size:11px;color:var(--text-muted);">' + items.length + ' file' + (items.length===1?'':'s') + '</span>';
            wrap.appendChild(h);
            if (description) {
                const d = document.createElement('div');
                d.style.cssText = 'font-size:11px;color:var(--text-muted);padding:0 4px 6px;';
                d.textContent = description;
                wrap.appendChild(d);
            }
            if (items.length === 0) {
                const empty = document.createElement('div');
                empty.style.cssText = 'padding:8px 10px;font-size:12px;color:var(--text-muted);';
                empty.textContent = '(empty)';
                wrap.appendChild(empty);
            }
            items.forEach(f => {
                const row = document.createElement('div');
                row.className = 'file-item';
                row.style.cssText = 'display:flex;justify-content:space-between;align-items:center;gap:8px;padding:8px 10px;';
                const nameEl = document.createElement('span');
                nameEl.style.cssText = 'font-family:monospace;flex:1;cursor:pointer;';
                nameEl.textContent = f.name;
                nameEl.title = 'Click to edit this extension';
                nameEl.addEventListener('click', () => loadExtension(f.name, folder));
                const sizeEl = document.createElement('span');
                sizeEl.style.cssText = 'color:var(--text-muted);font-size:11px;';
                sizeEl.textContent = f.size + ' B';
                // v4.33: "Insert" button - drops
                //   EXTENSION <stem>
                //   END_EXTENSION
                // into the Coding-tab editor at the caret, then switches
                // to the Coding tab. Matches the user's ask: "when clicking
                // an extension in the Extensions tab it should be added to
                // the Coding tab and automatically applied END_EXTENSION".
                const insertBtn = document.createElement('button');
                insertBtn.textContent = 'Insert →';
                insertBtn.title = 'Insert an EXTENSION reference into the Coding tab (auto END_EXTENSION)';
                insertBtn.style.cssText = 'padding:4px 10px;font-size:11px;border-radius:6px;background:var(--primary,#4caf50);color:#000;border:none;cursor:pointer;';
                insertBtn.addEventListener('click', (ev) => {
                    ev.stopPropagation();
                    const stem = f.name.replace(/\.(txt|ext|dsx|dd)$/i, '');
                    if (typeof insertAtEditorCursor === 'function') {
                        insertAtEditorCursor('EXTENSION ' + stem + '\nEND_EXTENSION\n');
                    } else {
                        const sa = document.getElementById('scriptArea');
                        if (sa) { sa.value += (sa.value.endsWith('\n') ? '' : '\n') + 'EXTENSION ' + stem + '\nEND_EXTENSION\n'; sa.dispatchEvent(new Event('input', {bubbles:true})); }
                    }
                    if (typeof openTab === 'function') openTab(null, 'Script');
                });
                row.appendChild(nameEl);
                row.appendChild(sizeEl);
                row.appendChild(insertBtn);
                wrap.appendChild(row);
            });
            box.appendChild(wrap);
        };
        renderSection('Hak5 (.txt) - repo-compatible, strict semantics',   hak5,   'hak5',
                      'ATTACKMODE STORAGE inside these = HID-off (Hak5 spec).');
        renderSection('Custom (.ext) - your own, friendly semantics',      custom, 'custom',
                      'ATTACKMODE STORAGE inside these = HID + storage (our default).');
        if (legacy.length > 0) {
            renderSection('Legacy (/extensions root, pre-v4.22)',          legacy, '', '');
        }
        // v4.27: piggy-back on the same list to keep the Script-tab
        // EXTENSION-arg autocomplete in sync.
        refreshExtensionAutocompletePool();
    }).catch(e => { box.textContent = 'Failed to list extensions: ' + e.message; });
}

// v4.22: remember which folder the currently-loaded extension came from so
// Save/Delete round-trip to the SAME folder (not a fresh guess by extension).
let __currentExtensionFolder = '';

function loadExtension(name, folder) {
    const q = '/api/load-extension?name=' + encodeURIComponent(name) +
              (folder ? '&folder=' + encodeURIComponent(folder) : '');
    fetch(q)
        .then(r => r.ok ? r.text() : Promise.reject(r.status))
        .then(txt => {
            __currentExtension = name;
            __currentExtensionFolder = folder || '';
            const ne = document.getElementById('extName');   if (ne) ne.value = name;
            const ed = document.getElementById('extEditor'); if (ed) ed.value = txt;
        })
        .catch(err => alert('Load failed: ' + err));
}

function saveExtension() {
    const name = (document.getElementById('extName').value || '').trim();
    const content = document.getElementById('extEditor').value || '';
    if (!name) { alert('Extension name required (e.g. OS_DETECTION.txt, or myscript.ext)'); return; }
    if (name.indexOf('/') >= 0 || name.indexOf('..') >= 0) { alert('Name must be leaf-only'); return; }
    // Prefer the currently-loaded folder if we know it; otherwise let the
    // server auto-route by suffix (.txt -> hak5, .ext -> custom).
    const payload = { name, content };
    if (__currentExtensionFolder) payload.folder = __currentExtensionFolder;
    fetch('/api/save-extension', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
    }).then(r => r.ok ? r.json() : r.text().then(t => Promise.reject(t)))
      .then(res => { alert('Saved (' + res.size + ' B).'); __currentExtension = name; refreshExtensions(); })
      .catch(err => alert('Save failed: ' + err));
}

function deleteExtension() {
    const name = (document.getElementById('extName').value || '').trim();
    if (!name) { alert('Enter the extension name to delete.'); return; }
    if (!confirm('Delete extension "' + name + '"? Cannot be undone.')) return;
    const q = '/api/delete-extension?name=' + encodeURIComponent(name) +
              (__currentExtensionFolder ? '&folder=' + encodeURIComponent(__currentExtensionFolder) : '');
    fetch(q, { method: 'DELETE' })
        .then(r => r.ok ? r.json() : r.text().then(t => Promise.reject(t)))
        .then(() => { alert('Deleted.'); clearExtEditor(); refreshExtensions(); })
        .catch(err => alert('Delete failed: ' + err));
}

function clearExtEditor() {
    __currentExtension = null;
    __currentExtensionFolder = '';
    document.getElementById('extName').value = '';
    document.getElementById('extEditor').value = '';
}

function uploadExtensionFile() {
    const el = document.getElementById('extFile');
    const f = el && el.files && el.files[0];
    if (!f) return;
    const rd = new FileReader();
    rd.onload = () => {
        document.getElementById('extName').value = f.name;
        document.getElementById('extEditor').value = rd.result || '';
        saveExtension();
    };
    rd.readAsText(f);
    el.value = '';
}

function pullExtension() {
    const url = (document.getElementById('extPullUrl').value || '').trim();
    let saveAs = (document.getElementById('extPullSaveAs').value || '').trim();
    if (!url) { alert('URL required'); return; }
    if (!saveAs) {
        const m = url.match(/\/([^\/?#]+)$/);
        saveAs = (m && m[1]) ? m[1] : 'extension.txt';
    }
    if (!/\.(txt|dsx|dd)$/i.test(saveAs)) saveAs += '.txt';
    if (/\.dd$/i.test(saveAs)) saveAs = saveAs.replace(/\.dd$/i, '.txt');
    fetch('/api/pull-extension', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ url, saveAs })
    }).then(r => r.ok ? r.json() : r.text().then(t => Promise.reject(t)))
      .then(res => { alert('Downloaded (' + res.size + ' B) as ' + saveAs); refreshExtensions(); })
      .catch(err => alert('Pull failed: ' + err));
}

(function () {
    const orig = window.openTab;
    if (typeof orig !== 'function') return;
    window.openTab = function (evt, name) {
        orig(evt, name);
        if (name === 'Extensions') refreshExtensions();
    };
})();

// ============================================================
// v4.17: "Blink LED while executing payloads" toggle
// ============================================================
function toggleBlinkOnRun() {
    const on = document.getElementById('blinkOnRunToggle').checked;
    fetch('/api/toggle-blink-on-run', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ enabled: on })
    }).catch(() => {});
}
(function () {
    const orig = window.initSettingsTab;
    window.initSettingsTab = function () {
        if (orig) orig.apply(this, arguments);
        fetch('/api/stats', { cache: 'no-store' }).then(r => r.json()).then(d => {
            const bt = document.getElementById('blinkOnRunToggle');
            if (bt && typeof d.blinkOnRun === 'boolean') bt.checked = d.blinkOnRun;
        }).catch(() => {});
    };
})();

// ============================================================
// v4.17: File Explorer preview + long-press context menu
// ============================================================
let __fcmTarget = null;

function closeFilePreview() {
    const p = document.getElementById('filePreviewPanel');
    if (p) p.style.display = 'none';
}

function showFilePreview(path, name) {
    fetch('/api/download?file=' + encodeURIComponent(path))
        .then(r => r.ok ? r.text() : Promise.reject(r.status))
        .then(txt => {
            const p = document.getElementById('filePreviewPanel');
            document.getElementById('filePreviewName').textContent = name || path;
            const body = document.getElementById('filePreviewBody');
            body.textContent = txt.length > 65536 ? (txt.substring(0, 65536) + '\n\n... (truncated at 64 KB) ...') : txt;
            p.style.display = 'block';
            p.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
        })
        .catch(err => alert('Preview failed: ' + err));
}

function showFileContextMenu(x, y, path, name) {
    __fcmTarget = { path, name };
    const m = document.getElementById('fileContextMenu');
    if (!m) return;
    m.style.display = 'block';
    const vw = window.innerWidth, vh = window.innerHeight;
    const mw = m.offsetWidth || 200, mh = m.offsetHeight || 240;
    m.style.left = Math.min(x, vw - mw - 10) + 'px';
    m.style.top  = Math.min(y, vh - mh - 10) + 'px';
}

function hideFileContextMenu() {
    const m = document.getElementById('fileContextMenu');
    if (m) m.style.display = 'none';
    __fcmTarget = null;
}
document.addEventListener('click', (e) => {
    const m = document.getElementById('fileContextMenu');
    if (!m || m.style.display !== 'block') return;
    if (!m.contains(e.target)) hideFileContextMenu();
});

function fcmAction(action) {
    const t = __fcmTarget;
    hideFileContextMenu();
    if (!t) return;
    if (action === 'preview')     showFilePreview(t.path, t.name);
    else if (action === 'download') window.location.href = '/api/download?file=' + encodeURIComponent(t.path);
    else if (action === 'delete') {
        if (!confirm('Delete ' + t.name + '?')) return;
        fetch('/api/delete-file?file=' + encodeURIComponent(t.path), { method: 'DELETE' })
            .then(() => { if (typeof refreshFileBrowser === 'function') refreshFileBrowser(); });
    }
    else if (action === 'rename') {
        const nn = prompt('New name for ' + t.name + ':', t.name);
        if (!nn || nn === t.name) return;
        alert('Rename endpoint not yet wired (planned). Path: ' + t.path);
    }
    else if (action === 'duplicate') {
        const nn = prompt('Copy ' + t.name + ' to:', t.name.replace(/(\.[^.]+)?$/, '-copy$1'));
        if (!nn) return;
        const dir = t.path.substring(0, t.path.lastIndexOf('/') + 1);
        const dest = dir + nn;
        const fd = new FormData();
        fd.append('source', t.path); fd.append('destination', dest);
        fetch('/api/copy-file', { method: 'POST', body: fd })
            .then(() => { if (typeof refreshFileBrowser === 'function') refreshFileBrowser(); });
    }
    else if (action === 'copy_path') safeCopyText(t.path).then(ok => { if (ok) alert('Path copied: ' + t.path); });
}

(function () {
    let pressTimer = null;
    let pressPath  = null;
    let pressName  = null;
    const findFileRow = (el) => {
        while (el && el !== document.body) {
            if (el.classList && (el.classList.contains('file-item') || el.dataset.path)) return el;
            el = el.parentElement;
        }
        return null;
    };
    const rowInfo = (row) => {
        const path = row.dataset.path || row.getAttribute('data-path') || '';
        const name = row.dataset.name || row.getAttribute('data-name') ||
                     (path ? path.substring(path.lastIndexOf('/') + 1) : '');
        return { path, name };
    };
    document.addEventListener('touchstart', (e) => {
        // v4.17-post-hunt BUG #9: clear any pending timer FIRST so a fast
        // tap on row B before row A's 500 ms elapses doesn't fire A's menu
        // at A's stale coordinates while pressPath now points at B.
        if (pressTimer) { clearTimeout(pressTimer); pressTimer = null; }
        const row = findFileRow(e.target);
        if (!row) return;
        const info = rowInfo(row);
        if (!info.path) return;
        pressPath = info.path; pressName = info.name;
        // Capture coordinates AT touchstart time so a scroll-cancelled hold
        // that somehow escapes the touchmove/cancel handlers still uses the
        // original tap position, not a stale event's.
        const t0 = e.touches && e.touches[0];
        const cx = t0 ? t0.clientX : 0;
        const cy = t0 ? t0.clientY : 0;
        pressTimer = setTimeout(() => {
            showFileContextMenu(cx, cy, pressPath, pressName);
            pressTimer = null;
        }, 500);
    }, { passive: true });
    document.addEventListener('touchend', () => {
        if (pressTimer) { clearTimeout(pressTimer); pressTimer = null; }
    });
    document.addEventListener('touchmove', () => {
        if (pressTimer) { clearTimeout(pressTimer); pressTimer = null; }
    }, { passive: true });
    // v4.17-post-hunt BUG #9: also handle touchcancel (OS interrupt, gesture,
    // popup). Without this, a cancelled touch still fires the 500 ms menu.
    document.addEventListener('touchcancel', () => {
        if (pressTimer) { clearTimeout(pressTimer); pressTimer = null; }
    });
    document.addEventListener('contextmenu', (e) => {
        const row = findFileRow(e.target);
        if (!row) return;
        const info = rowInfo(row);
        if (!info.path) return;
        e.preventDefault();
        showFileContextMenu(e.clientX, e.clientY, info.path, info.name);
    });
})();

// ============================================================
// v4.17: editor error-overlay overflow guard (mobile)
// ============================================================
// The .error-line / .inline-error rows can extend past the code line width
// on narrow screens. Inject a small style block that clips them.
(function () {
    const s = document.createElement('style');
    s.textContent =
        '.error-line, .warning-line { max-width: 100%; overflow-x: hidden; }' +
        '.inline-error, .inline-warning { max-width: 100%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }' +
        '@media (max-width: 600px) {' +
        '  .inline-error, .inline-warning { font-size: 10px; padding: 2px 6px; }' +
        '  .editor-highlights, .editor-main textarea { font-size: 13px; }' +
        '}';
    document.head.appendChild(s);
})();


// ============================================================
// v4.31: per-block ˅/^ fold at the caret. Toolbar button "˅/^ Fold" and
// Ctrl+K both invoke this. Behaviour:
//   * If the caret is on / inside an `EXTENSION NAME ^ ... END_EXTENSION`
//     expanded block  -> collapse it to a single `EXTENSION NAME ˅` line.
//   * If the caret is on an `EXTENSION NAME ˅` collapsed reference line
//     -> fetch the body from the SD and expand it inline.
//   * Otherwise -> nothing (with a brief non-blocking toast).
// Bulk expand/collapse-all still live in the Extensions ▾ picker; this is
// the per-block arrow the user was looking for.
// ============================================================
function toggleExtensionAtCursor() {
    const sa = document.getElementById('scriptArea');
    if (!sa) return;
    const caretIdx  = sa.selectionStart;
    const src       = sa.value;
    const before    = src.substring(0, caretIdx);
    const caretLine = before.split('\n').length - 1;
    const lines     = src.split('\n');

    // Walk down from the caret to find its enclosing EXTENSION block.
    // Match either the expanded opener (`EXTENSION NAME ^`), the collapsed
    // reference (`EXTENSION NAME ˅`), or a plain `EXTENSION NAME` opener.
    // v4.34 bug-hunt MEDIUM #10: accept `EXTENSION name˅` with NO space
    // between the name and the ˅/^ marker (mobile keyboards drop the
    // space; firmware Pass 0 at DuckyInterpreter.cpp:207 already accepts
    // it, so the two sides need to agree).
    const openRE  = /^(\s*)EXTENSION\s+([A-Za-z0-9_.\-]+)\s*(\^|˅)?\s*$/;
    const closeRE = /^\s*END_EXTENSION\s*$/;

    // If the caret line itself is a collapsed reference, expand it.
    const meMatch = openRE.exec(lines[caretLine] || '');
    if (meMatch && meMatch[3] === '˅') {
        _expandExtensionRefLine(caretLine, meMatch[1], meMatch[2], sa, lines);
        return;
    }

    // Otherwise search upward for an expanded opener that hasn't been closed
    // before the caret.
    let openIdx = -1, indent = '', name = '';
    for (let i = caretLine; i >= 0; i--) {
        const m = openRE.exec(lines[i] || '');
        if (m && (m[3] === '^' || m[3] === undefined)) { openIdx = i; indent = m[1]; name = m[2]; break; }
        if (closeRE.test(lines[i] || '') && i < caretLine) break;
    }
    if (openIdx < 0) {
        _toastFold('Move the caret onto an EXTENSION line (or into its body) first.');
        return;
    }
    // Find matching END_EXTENSION (nesting isn't officially defined by Hak5
    // but be defensive against `EXTENSION` markers appearing inside the body).
    let depth = 1, closeIdx = -1;
    for (let j = openIdx + 1; j < lines.length; j++) {
        const l = lines[j];
        if (openRE.test(l)) depth++;
        else if (closeRE.test(l)) { depth--; if (depth === 0) { closeIdx = j; break; } }
    }
    if (closeIdx < 0) {
        _toastFold('Block has no END_EXTENSION - refusing to collapse.');
        return;
    }

    // Splice: replace [openIdx..closeIdx] with a single `EXTENSION NAME ˅`.
    const newLines = lines.slice(0, openIdx)
        .concat([indent + 'EXTENSION ' + name + ' ˅'])
        .concat(lines.slice(closeIdx + 1));
    sa.value = newLines.join('\n');
    // Put the caret back on the collapsed line.
    let charPos = 0;
    for (let k = 0; k < openIdx; k++) charPos += newLines[k].length + 1;
    sa.selectionStart = sa.selectionEnd = charPos + (indent.length + ('EXTENSION ' + name + ' ˅').length);
    sa.dispatchEvent(new Event('input', { bubbles: true }));
}

function _expandExtensionRefLine(idx, indent, name, sa, lines) {
    // v4.34 bug-hunt MEDIUM #11: folder OUTER, suffix INNER - matches the
    // firmware Pass 0 order (DuckyInterpreter.cpp:268) so a legacy
    // /extensions/foo.txt is found in far fewer round-trips than before,
    // where a legacy root file needed 12 attempts through hak5+custom+bare.
    const suffixes = ['', '.txt', '.ext', '.dsx'];
    const folders  = ['hak5', 'custom', ''];
    const attempts = [];
    for (const f of folders) for (const s of suffixes) attempts.push({ name: name + s, folder: f });
    const one = (i) => {
        if (i >= attempts.length) return Promise.resolve(null);
        const a = attempts[i];
        let url = '/api/load-extension?name=' + encodeURIComponent(a.name);
        if (a.folder) url += '&folder=' + encodeURIComponent(a.folder);
        return fetch(url).then(r => r.ok ? r.text().then(t => ({ name: a.name, body: t }))
                                          : one(i + 1))
                         .catch(() => one(i + 1));
    };
    one(0).then(res => {
        if (!res) { _toastFold('Extension "' + name + '" not found on SD.'); return; }
        const bodyLines = res.body.replace(/\r?\n$/, '').split('\n');
        const stem = res.name.replace(/\.(txt|ext|dsx|dd)$/i, '');
        const spliced = lines.slice(0, idx)
            .concat([indent + 'EXTENSION ' + stem + ' ^'])
            .concat(bodyLines)
            .concat([indent + 'END_EXTENSION'])
            .concat(lines.slice(idx + 1));
        sa.value = spliced.join('\n');
        // Caret onto the opener line.
        let charPos = 0;
        for (let k = 0; k < idx; k++) charPos += spliced[k].length + 1;
        sa.selectionStart = sa.selectionEnd = charPos;
        sa.dispatchEvent(new Event('input', { bubbles: true }));
    });
}

let __foldToast = null;
function _toastFold(msg) {
    if (__foldToast) __foldToast.remove();
    const t = document.createElement('div');
    t.textContent = msg;
    t.style.cssText = 'position:fixed;bottom:20px;left:50%;transform:translateX(-50%);'
        + 'background:#2a2a35;color:#fff;padding:10px 18px;border-radius:8px;'
        + 'font-size:12px;box-shadow:0 6px 24px rgba(0,0,0,0.4);z-index:99999;'
        + 'border:1px solid rgba(255,255,255,0.08);pointer-events:none;';
    document.body.appendChild(t);
    __foldToast = t;
    setTimeout(() => { if (t.parentNode) t.remove(); if (__foldToast === t) __foldToast = null; }, 2200);
}

// Ctrl+K keyboard shortcut wired at load - non-invasive (only fires when the
// caret is in the script editor, so it doesn't interfere with other tabs).
if (typeof window !== 'undefined') {
    window.addEventListener('DOMContentLoaded', () => {
        const sa = document.getElementById('scriptArea');
        if (!sa) return;
        sa.addEventListener('keydown', (e) => {
            if ((e.ctrlKey || e.metaKey) && !e.shiftKey && !e.altKey && (e.key === 'k' || e.key === 'K')) {
                e.preventDefault();
                toggleExtensionAtCursor();
            }
        });
    });
}

// ============================================================
// v4.17b: Coding editor - Extensions dropdown
// ============================================================
// The "Extensions ▾" toolbar button pops a small floating picker of the
// files in /extensions/. Clicking one lets the user choose:
//   * COLLAPSED  -> inserts a single line "EXTENSION NAME ˅" (reference)
//   * INLINE     -> inserts the extension body wrapped in
//                   EXTENSION NAME ^
//                     <body>
//                   END_EXTENSION
// A COLLAPSED reference is expandable at any time via a matching
// "expandCollapsedExtensions()" scanner that finds ˅ markers and swaps
// them for the fetched body. Clicking the ^ (via the Extensions ▾ button
// again -> "Collapse all") reverses it.
function openExtensionInsert(evt) {
    const btn = evt.currentTarget || evt.target;
    let menu = document.getElementById('extInsertMenu');
    if (menu) { menu.remove(); return; }
    menu = document.createElement('div');
    menu.id = 'extInsertMenu';
    menu.style.cssText =
        'position:absolute;z-index:99998;background:#1e1e28;color:#fff;border-radius:10px;' +
        'box-shadow:0 12px 40px rgba(0,0,0,0.5);border:1px solid rgba(255,255,255,0.1);' +
        'min-width:260px;max-height:340px;overflow-y:auto;padding:6px 0;';
    const r = btn.getBoundingClientRect();
    menu.style.left = (r.left + window.scrollX) + 'px';
    menu.style.top  = (r.bottom + 6 + window.scrollY) + 'px';
    menu.innerHTML = '<div style="padding:8px 12px;font-size:11px;opacity:0.55;">Loading extensions…</div>';
    document.body.appendChild(menu);
    // Close on outside click.
    const closeOnce = (e) => {
        if (menu.contains(e.target) || e.target === btn) return;
        menu.remove();
        document.removeEventListener('click', closeOnce, true);
    };
    setTimeout(() => document.addEventListener('click', closeOnce, true), 0);

    fetch('/api/list-extensions').then(r => r.json()).then(data => {
        // v4.22: data is now {hak5:[], custom:[], legacy:[]}. Flatten with
        // a folder-tag so the picker can still show source + insert correctly.
        const flatList = [];
        (data.hak5   || []).forEach(f => flatList.push({...f, folder: 'hak5'}));
        (data.custom || []).forEach(f => flatList.push({...f, folder: 'custom'}));
        (data.legacy || []).forEach(f => flatList.push({...f, folder: ''}));
        menu.innerHTML = '';
        const ctrl = document.createElement('div');
        ctrl.style.cssText = 'display:flex;gap:6px;padding:6px 8px;border-bottom:1px solid rgba(255,255,255,0.08);';
        ctrl.innerHTML =
            '<button onclick="expandAllExtensionRefs()"   style="flex:1;padding:6px 8px;border-radius:6px;background:#2a2a35;color:#fff;border:none;cursor:pointer;font-size:12px;">Expand all ˅</button>' +
            '<button onclick="collapseAllExtensionBodies()" style="flex:1;padding:6px 8px;border-radius:6px;background:#2a2a35;color:#fff;border:none;cursor:pointer;font-size:12px;">Collapse all ^</button>';
        menu.appendChild(ctrl);
        if (flatList.length === 0) {
            const empty = document.createElement('div');
            empty.style.cssText = 'padding:12px;font-size:12px;color:var(--text-muted,#888);text-align:center;';
            empty.textContent = 'No extensions on SD. Open the Extensions tab to upload / pull one.';
            menu.appendChild(empty);
            return;
        }
        flatList.forEach(f => {
            const row = document.createElement('div');
            row.style.cssText = 'padding:8px 12px;cursor:pointer;font-family:monospace;font-size:13px;display:flex;justify-content:space-between;gap:8px;';
            row.innerHTML = '<span>' + escapeHtml(f.name) +
                            (f.folder ? ' <span style="opacity:0.5;font-weight:normal;">(' + f.folder + ')</span>' : '') +
                            '</span>' +
                            '<span style="opacity:0.6;font-size:11px;">' + f.size + ' B</span>';
            row.onmouseover = () => row.style.background = 'rgba(76,175,80,0.15)';
            row.onmouseout  = () => row.style.background = '';
            // v4.27 bug-hunt HIGH #8: replace the synchronous confirm() (which
            // mobile browsers block with "Prevent additional dialogs" and
            // whose Enter-key confirm bubbled back through the outside-click
            // handler tearing down the picker mid-choice) with an inline
            // two-button popover rendered inside the picker itself.
            row.onclick = (ev) => {
                ev.stopPropagation();   // don't wake closeOnce with our own click
                // Replace this row's content with an inline Inline/Collapsed picker.
                row.innerHTML = '';
                row.style.cssText += ';flex-direction:column;align-items:stretch;';
                const label = document.createElement('div');
                label.style.cssText = 'font-size:12px;opacity:0.75;margin-bottom:6px;';
                label.textContent = 'Insert "' + f.name + '" as:';
                const btnRow = document.createElement('div');
                btnRow.style.cssText = 'display:flex;gap:6px;';
                const mkBtn = (text, handler) => {
                    const b = document.createElement('button');
                    b.textContent = text;
                    b.style.cssText = 'flex:1;padding:8px;border-radius:6px;background:#2a2a35;color:#fff;border:none;cursor:pointer;font-size:12px;';
                    b.addEventListener('click', (e) => { e.stopPropagation(); menu.remove(); document.removeEventListener('click', closeOnce, true); handler(); });
                    return b;
                };
                btnRow.appendChild(mkBtn('Inline (expand body)', () => {
                    fetch('/api/load-extension?name=' + encodeURIComponent(f.name) +
                          (f.folder ? '&folder=' + encodeURIComponent(f.folder) : ''))
                        .then(r => r.ok ? r.text() : Promise.reject('HTTP ' + r.status))
                        .then(body => insertAtEditorCursor(
                            'EXTENSION ' + f.name.replace(/\.(txt|ext|dsx|dd)$/i, '') + ' ^\n' +
                            body.replace(/\r?\n$/, '') + '\n' +
                            'END_EXTENSION\n'
                        ))
                        .catch(err => alert('Failed to fetch extension: ' + err));
                }));
                btnRow.appendChild(mkBtn('Collapsed (reference)', () => {
                    insertAtEditorCursor('EXTENSION ' + f.name.replace(/\.(txt|ext|dsx|dd)$/i, '') + ' ˅\n');
                }));
                row.appendChild(label);
                row.appendChild(btnRow);
            };
            menu.appendChild(row);
        });
    }).catch(err => { menu.innerHTML = '<div style="padding:10px;color:#f88;">Failed to list: ' + err + '</div>'; });
}

function insertAtEditorCursor(text) {
    const sa = document.getElementById('scriptArea');
    if (!sa) return;
    const start = sa.selectionStart;
    const end   = sa.selectionEnd;
    const before = sa.value.substring(0, start);
    const after  = sa.value.substring(end);
    sa.value = before + text + after;
    sa.selectionStart = sa.selectionEnd = start + text.length;
    sa.dispatchEvent(new Event('input', { bubbles: true }));
    sa.focus();
}

// Expand all `EXTENSION <NAME> ˅` reference lines to their full inline body.
function expandAllExtensionRefs() {
    const sa = document.getElementById('scriptArea');
    if (!sa) return;
    const lines = sa.value.split('\n');
    const jobs = [];
    lines.forEach((l, idx) => {
        const m = l.match(/^\s*EXTENSION\s+([A-Za-z0-9_.\-]+)\s+˅\s*$/);
        if (m) jobs.push({ idx, name: m[1] });
    });
    if (jobs.length === 0) { alert('No collapsed extension references (˅) found.'); return; }
    // Fetch all bodies then splice into the lines array from bottom up
    // (so indices don't shift).
    // v4.27 bug-hunt HIGH #7: (a) try each folder in turn (hak5, custom,
    // root) so extensions saved to /extensions/custom/*.ext still resolve.
    // Previously the fetch omitted ?folder= and only found /extensions/hak5
    // entries. (b) add an outer .catch so a network drop / ESP reset
    // surfaces to the user instead of silently leaving the editor
    // half-mutated.
    Promise.all(jobs.map(j => {
        const suffixes = ['', '.txt', '.ext', '.dsx'];
        const folders  = ['hak5', 'custom', ''];
        const attempts = [];
        // v4.34 bug-hunt MEDIUM #11: folder OUTER, suffix INNER.
        for (const f of folders) for (const s of suffixes) attempts.push({ name: j.name + s, folder: f });
        const one = (i) => {
            if (i >= attempts.length) return { ok:false, name:j.name };
            const a = attempts[i];
            let url = '/api/load-extension?name=' + encodeURIComponent(a.name);
            if (a.folder) url += '&folder=' + encodeURIComponent(a.folder);
            return fetch(url).then(r => r.ok ? r.text().then(t => ({ ok:true, name:a.name, body:t }))
                                              : one(i + 1))
                             .catch(() => one(i + 1));
        };
        return one(0);
    })).then(results => {
        const failed = [];
        for (let i = jobs.length - 1; i >= 0; i--) {
            const j = jobs[i]; const r = results[i];
            if (!r || !r.ok) { failed.push(j.name); continue; }
            const bodyLines = r.body.replace(/\r?\n$/, '').split('\n');
            lines.splice(j.idx, 1,
                'EXTENSION ' + r.name.replace(/\.(txt|ext|dsx|dd)$/i, '') + ' ^',
                ...bodyLines,
                'END_EXTENSION');
        }
        sa.value = lines.join('\n');
        sa.dispatchEvent(new Event('input', { bubbles: true }));
        if (failed.length) alert('Not found on SD: ' + failed.join(', '));
    }).catch(err => alert('Expand failed: ' + (err && err.message ? err.message : err)));
}

// Collapse each expanded `EXTENSION <NAME> ^ ... END_EXTENSION` block back
// to its single-line `EXTENSION <NAME> ˅` reference form.
function collapseAllExtensionBodies() {
    const sa = document.getElementById('scriptArea');
    if (!sa) return;
    const lines = sa.value.split('\n');
    const out = [];
    let i = 0;
    let count = 0;
    while (i < lines.length) {
        const m = lines[i].match(/^\s*EXTENSION\s+([A-Za-z0-9_.\-]+)\s+\^\s*$/);
        if (m) {
            // Find matching END_EXTENSION.
            let j = i + 1;
            while (j < lines.length && !/^\s*END_EXTENSION\s*$/.test(lines[j])) j++;
            out.push('EXTENSION ' + m[1] + ' ˅');
            count++;
            i = j + 1;
        } else {
            out.push(lines[i]);
            i++;
        }
    }
    if (count === 0) { alert('No expanded extension blocks found.'); return; }
    sa.value = out.join('\n');
    sa.dispatchEvent(new Event('input', { bubbles: true }));
}

// v4.17b: DuckyScript preprocessor complement - at RUN time the firmware
// needs to see EXTENSION <NAME> ˅ (or ^) lines get resolved to the actual
// extension body. But we already have RUN_EXTENSION for the firmware side.
// So: right before /execute POST, rewrite the editor content: any bare
// `EXTENSION <NAME> ˅` line becomes `RUN_EXTENSION <NAME>` so the firmware
// runs it inline via RUN_EXTENSION's file loader.
(function () {
    const orig = window.executeScript;
    if (typeof orig !== 'function') return;
    window.executeScript = function () {
        // Only preprocess the editor content that gets POSTed. We don't
        // touch the visible textarea - just the payload sent to /execute.
        const sa = document.getElementById('scriptArea');
        if (sa) {
            const before = sa.value;
            const rewritten = before.split('\n').map(l => {
                const m = l.match(/^(\s*)EXTENSION\s+([A-Za-z0-9_.\-]+)\s+˅\s*$/);
                if (!m) return l;
                return m[1] + 'RUN_EXTENSION ' + m[2];
            }).join('\n');
            if (rewritten !== before) {
                // Temporarily swap value for the POST, then restore so the
                // editor UI doesn't visibly change.
                sa.value = rewritten;
                try { orig.apply(this, arguments); } finally {
                    sa.value = before;
                    // v4.27 bug-hunt MEDIUM #9: don't dispatch a fake `input`
                    // - nothing changed for the user, and firing it kicks
                    // updateErrorLens / autocomplete / draft-save mid-run.
                    // The lens is already up to date; the textarea is
                    // pixel-identical.
                }
                return;
            }
        }
        return orig.apply(this, arguments);
    };
})();
