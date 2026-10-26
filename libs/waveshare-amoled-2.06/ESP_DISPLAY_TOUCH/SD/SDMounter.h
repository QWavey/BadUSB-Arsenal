#pragma once
#include <Arduino.h>
#include <SD_MMC.h>
#include <FS.h>
#include <functional>

// NOTE: Pins (SDMMC_CLK, SDMMC_CMD, SDMMC_DATA) must be defined in pin_config.h
// Include your pin_config.h before including this library

class SDMounter {
public:
    // Constructor
    SDMounter();
    
    // Core mounting operations
    bool mount(bool format_if_failed = false, const char* mount_point = "/sdcard", bool mode1bit = true);
    bool unmount();
    bool remount();
    bool format();
    bool check();
    
    // Auto-mount control
    void enableAutoMount(bool enable = true) { auto_mount_enabled = enable; }
    bool isAutoMountEnabled() const { return auto_mount_enabled; }
    void autoMount(); // Call this in setup() if you want auto-mount
    
    // File system info
    String getFsType();
    String getFsLabel();
    uint32_t getBlockSize();
    uint64_t getSectorCount();
    uint64_t getTotalBytes();
    uint64_t getUsedBytes();
    uint64_t getFreeBytes();
    
    // File operations
    File openFile(const char* path, const char* mode = FILE_READ);
    bool closeFile(File& file);
    String readFile(const char* path);
    size_t readFile(const char* path, uint8_t* buffer, size_t max_len);
    bool writeFile(const char* path, const char* content);
    bool writeFile(const char* path, const uint8_t* data, size_t len);
    bool appendFile(const char* path, const char* content);
    bool appendFile(const char* path, const uint8_t* data, size_t len);
    bool seekFile(File& file, size_t position);
    size_t tellFile(File& file);
    bool truncateFile(const char* path, size_t size);
    bool deleteFile(const char* path);
    bool renameFile(const char* old_path, const char* new_path);
    bool copyFile(const char* src, const char* dst);
    bool moveFile(const char* src, const char* dst);
    bool existsFile(const char* path);
    size_t getFileSize(const char* path);
    
    // Directory operations
    bool mkdir(const char* path);
    bool rmdir(const char* path);
    bool rmdirRecursive(const char* path);
    String listDir(const char* path, bool recursive = false);
    std::vector<String> listDirVector(const char* path);
    File openDir(const char* path);
    bool changeDir(const char* path);
    String getCurrentDir() const { return current_dir; }
    
    // Diagnostics & Debug
    int getErrorCode() const { return last_error_code; }
    String getLastError() const { return last_error_msg; }
    bool stressTest(uint32_t iterations = 100);
    float readSpeedTest(size_t block_size = 4096, uint32_t iterations = 100);
    float writeSpeedTest(size_t block_size = 4096, uint32_t iterations = 100);
    void dumpFsInfo();
    
    // Hot-plug & Events
    bool isInserted();
    bool isRemoved();
    void checkHotSwap(); // Call this periodically in loop()
    void enableHotSwapDetection(bool enable = true) { hotswap_enabled = enable; }
    bool isHotSwapEnabled() const { return hotswap_enabled; }
    void setDebugMode(bool enable) { debug_mode = enable; }
    void onMount(std::function<void()> callback) { on_mount_callback = callback; }
    void onUnmount(std::function<void()> callback) { on_unmount_callback = callback; }
    void onCardInserted(std::function<void()> callback) { on_card_inserted_callback = callback; }
    void onCardRemoved(std::function<void()> callback) { on_card_removed_callback = callback; }
    
    // Status check
    bool isMounted() const { return mounted; }
    
    // Unchecked escape hatch: returns the raw SD_MMC filesystem. It bypasses the
    // mounted checks and the path normalization that the methods above apply.
    fs::SDMMCFS& getSD() { return SD_MMC; }

private:
    bool mounted;
    bool auto_mount_enabled;
    bool hotswap_enabled;
    bool debug_mode;
    bool last_card_state;
    unsigned long last_hotswap_check;
    String mount_point;
    String current_dir;
    int last_error_code;
    String last_error_msg;
    bool mode_1bit;
    
    std::function<void()> on_mount_callback;
    std::function<void()> on_unmount_callback;
    std::function<void()> on_card_inserted_callback;
    std::function<void()> on_card_removed_callback;
    
    // Helper methods
    void setError(int code, const String& msg);
    void clearError();
    String getFullPath(const char* path);
    bool isAbsolutePath(const char* path);
    void triggerMountCallback();
    void triggerUnmountCallback();
    void triggerCardInsertedCallback();
    void triggerCardRemovedCallback();
    size_t copyFileInternal(File& src, File& dst);
    bool deleteDirectoryRecursive(const char* path, uint8_t depth = 0);
    String listDirTree(const char* path, bool recursive, uint8_t depth, size_t& budget);
    bool checkCardPresent();
};

// Global instance (optional - user can create their own)
extern SDMounter SDCard;
