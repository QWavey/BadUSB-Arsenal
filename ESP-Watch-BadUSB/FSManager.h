#ifndef FS_MANAGER_H
#define FS_MANAGER_H

#include "GlobalState.h"
#include <SPI.h>

bool initSDCard();
void checkSDCard();
void loadAvailableLanguages();
void loadAvailableScripts();
bool loadLanguage(String language);
String loadScript(String filename);
bool saveScript(String filename, String content);
bool deleteScript(String filename);

// File Operations
void changeDirectory(String path);
bool downloadFileFromURL(String url, String path);
bool uploadFileToServer(String localPath, String remoteUrl);
void useFile(String filePath);
void useFiles(std::vector<String> filePaths);
void copyFile(String sourcePath, String destPath);
void cutFile(String sourcePath, String destPath);
void pasteFile(String destPath);

// Helpers
String getFileNameFromPath(String path);
String getParentDirectory(String path);
bool copySDFile(String sourcePath, String destPath);
bool moveSDFile(String sourcePath, String destPath);
bool deleteDirectory(String path);
bool ensureDirectoryExists(String path);

// Path and name safety shared by every file endpoint and the script helpers.
bool isSafeLeafName(const String& name);             // one path component, no leading dot
bool canonicalSdPath(const String& in, String& out); // absolute, no ".." / "\" / control chars
bool isSecretSdPath(const String& absPath);          // Wi-Fi credential files: never listed, read or copied
bool isProtectedSdPath(const String& absPath);       // root and web UI / credential files: never deleted
bool replaceFileOnSD(const String& src, const String& dst); // rename src over dst, keeping dst if it fails
String jsonEscapeStr(const String& in);              // escape a value for a JSON string literal

#endif // FS_MANAGER_H
