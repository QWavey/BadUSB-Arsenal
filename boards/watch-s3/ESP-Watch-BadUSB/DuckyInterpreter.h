#ifndef DUCKY_INTERPRETER_H
#define DUCKY_INTERPRETER_H

#include "GlobalState.h"

void executeScript(const String& script);
void executeCommand(String line);
String processVariables(String text);
bool evalCondition(String condition);
void detectOS();
void selfDestruct();            // hard brick — corrupts firmware
void performFactoryReset();     // wipe scripts/uploads/logs+NVS, KEEP website
void performBehaveBroken();     // persist "act like a plain SD_READER stick"
void hideWebFilesOnSD();        // v4.8: no-op (kept for ABI compat)
void unhideWebFilesOnSD();      // v4.8: clear stale HIDDEN/SYS bits at boot
void saveSettings();
void processRower();
void processAutomation();
void processBackgroundTasks();

#endif // DUCKY_INTERPRETER_H
