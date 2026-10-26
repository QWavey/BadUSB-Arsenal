#ifndef WEB_SERVER_HANDLERS_H
#define WEB_SERVER_HANDLERS_H

#include "GlobalState.h"

// True when the most recent upload was refused or could not be stored.
extern bool uploadFailed;

// API Handlers
void handleFileUpload();
void handleFileDownload();
void handleChangeDirectory();
void handleGetCurrentDirectory();
void handleDetectOS();
void handleUseFile();
void handleCopyFile();
void handleCutFile();
void handlePasteFile();
void handleJoinInternet();
void handleLeaveInternet();
void handleListFiles();
void handleDeleteFile();
void handleCreateDirectory();
void handleFileInfo();

#endif // WEB_SERVER_HANDLERS_H
