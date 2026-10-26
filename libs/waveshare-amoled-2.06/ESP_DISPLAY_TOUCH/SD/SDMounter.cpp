#include "SDMounter.h"
#include "pin_config.h"

// Global instance definition
SDMounter SDCard;

// Limits that stop card contents from exhausting heap or stack
static const size_t kMaxReadFileBytes = 64 * 1024; // readFile(path) refuses larger files
static const size_t kMaxListEntries = 512;         // entries returned by one listing
static const uint8_t kMaxDirDepth = 8;             // recursion depth for listing and delete

// Resolves '.' and '..' segments and keeps the result inside the card root.
// A '..' above the root is dropped, so no path can climb out of the mounted volume.
static String normalizeCardPath(const String& raw) {
    String out = "";
    size_t start = 0;
    while (start <= raw.length()) {
        int slash = raw.indexOf('/', start);
        if (slash < 0) slash = raw.length();
        String seg = raw.substring(start, slash);
        start = slash + 1;

        if (seg.length() == 0 || seg == ".") continue;
        if (seg == "..") {
            int last = out.lastIndexOf('/');
            out = (last > 0) ? out.substring(0, last) : String("");
            continue;
        }
        out += "/" + seg;
    }
    return out.length() ? out : String("/");
}

SDMounter::SDMounter() 
    : mounted(false), 
      auto_mount_enabled(false),
      hotswap_enabled(false),
      debug_mode(false),
      last_card_state(false),
      last_hotswap_check(0),
      mount_point("/sdcard"),
      current_dir("/"),
      last_error_code(0),
      mode_1bit(true),
      on_mount_callback(nullptr),
      on_unmount_callback(nullptr),
      on_card_inserted_callback(nullptr),
      on_card_removed_callback(nullptr) {
}

bool SDMounter::mount(bool format_if_failed, const char* mp, bool mode1bit) {
    if (mounted) {
        setError(1, "SD card already mounted");
        return true; // Already mounted is not an error
    }
    
    clearError();
    mount_point = String(mp);
    mode_1bit = mode1bit;
    
    Serial.println("[SDMounter] Initializing SD card...");
    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);  // Pins from pin_config.h
    
    if (!SD_MMC.begin(mp, mode1bit)) {
        // format_if_failed is accepted for compatibility, but no format is performed yet.
        // Say so in the error instead of claiming an attempt was made.
        if (format_if_failed) {
            setError(4, "SD card mount failed (format_if_failed is not implemented)");
        } else {
            setError(4, "SD card mount failed");
        }
        return false;
    }
    
    mounted = true;
    current_dir = "/";
    last_card_state = true; // Card is present after successful mount
    Serial.println("[SDMounter] SD card mounted successfully");
    Serial.printf("[SDMounter] Total: %.2f MB, Used: %.2f MB\n", 
                  getTotalBytes() / 1048576.0, getUsedBytes() / 1048576.0);
    
    triggerMountCallback();
    return true;
}

bool SDMounter::unmount() {
    if (!mounted) {
        setError(5, "SD card not mounted");
        return false;
    }
    
    clearError();
    SD_MMC.end();
    mounted = false;
    current_dir = "/";
    Serial.println("[SDMounter] SD card unmounted");
    
    triggerUnmountCallback();
    return true;
}

bool SDMounter::remount() {
    Serial.println("[SDMounter] Remounting SD card...");
    if (mounted) {
        unmount();
        delay(100);
    }
    return mount(false, mount_point.c_str(), mode_1bit);
}

bool SDMounter::format() {
    Serial.println("[SDMounter] Formatting SD card (deleting all files)...");
    
    if (!mounted) {
        setError(6, "SD card must be mounted to format");
        return false;
    }
    
    // SD_MMC doesn't have a format() method, so we delete everything
    Serial.println("[SDMounter] Deleting all files and directories...");
    
    File root = SD_MMC.open("/");
    if (!root || !root.isDirectory()) {
        setError(7, "Failed to open root directory");
        return false;
    }
    
    // Delete all files and directories in root
    int failures = 0;
    File file = root.openNextFile();
    while (file) {
        String name = String(file.name());
        String path = "/" + name;
        
        if (file.isDirectory()) {
            file.close();
            if (!deleteDirectoryRecursive(path.c_str())) {
                Serial.printf("[SDMounter] Failed to delete directory: %s\n", path.c_str());
                failures++;
            }
        } else {
            file.close();
            if (!SD_MMC.remove(path.c_str())) {
                Serial.printf("[SDMounter] Failed to delete file: %s\n", path.c_str());
                failures++;
            }
        }
        
        file = root.openNextFile();
    }
    root.close();
    
    current_dir = "/";

    // Report success only when every entry was removed
    if (failures > 0) {
        setError(49, String("Format incomplete: ") + String(failures) + " entries could not be deleted");
        return false;
    }

    Serial.println("[SDMounter] Format complete (all files deleted)");
    clearError();
    return true;
}

bool SDMounter::check() {
    if (!mounted) {
        setError(8, "SD card not mounted");
        return false;
    }
    
    clearError();
    
    // Basic integrity check - try to read root directory
    File root = SD_MMC.open("/");
    if (!root) {
        setError(9, "Failed to open root directory");
        return false;
    }
    
    if (!root.isDirectory()) {
        root.close();
        setError(10, "Root is not a directory");
        return false;
    }
    
    // Try to enumerate files
    int file_count = 0;
    File file = root.openNextFile();
    while (file) {
        file_count++;
        file.close();
        file = root.openNextFile();
        if (file_count > 10000) break; // Safety limit
    }
    root.close();
    
    Serial.printf("[SDMounter] Check passed - %d items in root\n", file_count);
    return true;
}

void SDMounter::autoMount() {
    if (auto_mount_enabled && !mounted) {
        Serial.println("[SDMounter] Auto-mounting SD card...");
        mount(false, mount_point.c_str(), mode_1bit);
    }
}

String SDMounter::getFsType() {
    if (!mounted) return "NONE";
    
    uint8_t cardType = SD_MMC.cardType();
    switch (cardType) {
        case CARD_NONE: return "NONE";
        case CARD_MMC: return "MMC";
        case CARD_SD: return "SD";
        case CARD_SDHC: return "SDHC";
        case CARD_UNKNOWN:
        default: return "UNKNOWN";
    }
}

String SDMounter::getFsLabel() {
    if (!mounted) return "";
    // SD_MMC doesn't directly provide label, return mount point
    return mount_point;
}

uint32_t SDMounter::getBlockSize() {
    if (!mounted) return 0;
    return 512; // Standard SD card sector size
}

uint64_t SDMounter::getSectorCount() {
    if (!mounted) return 0;
    return SD_MMC.totalBytes() / 512;
}

uint64_t SDMounter::getTotalBytes() {
    if (!mounted) return 0;
    return SD_MMC.totalBytes();
}

uint64_t SDMounter::getUsedBytes() {
    if (!mounted) return 0;
    return SD_MMC.usedBytes();
}

uint64_t SDMounter::getFreeBytes() {
    if (!mounted) return 0;
    uint64_t total = getTotalBytes();
    uint64_t used = getUsedBytes();
    return (total > used) ? (total - used) : 0;
}

File SDMounter::openFile(const char* path, const char* mode) {
    if (!mounted) {
        setError(11, "SD card not mounted");
        return File();
    }
    
    String full_path = getFullPath(path);
    File file = SD_MMC.open(full_path.c_str(), mode);
    
    if (!file) {
        setError(12, String("Failed to open file: ") + full_path);
    } else {
        clearError();
    }
    
    return file;
}

bool SDMounter::closeFile(File& file) {
    if (!file) {
        setError(13, "Invalid file handle");
        return false;
    }
    file.close();
    clearError();
    return true;
}

String SDMounter::readFile(const char* path) {
    File file = openFile(path, FILE_READ);
    if (!file) return "";
    
    // Refuse files that would not fit the heap, instead of growing the String until it fails
    if (file.size() > kMaxReadFileBytes) {
        file.close();
        setError(46, "File too large for readFile(), use readFile(path, buffer, len)");
        return "";
    }

    String content = "";
    while (file.available()) {
        content += (char)file.read();
    }
    file.close();
    
    clearError();
    return content;
}

size_t SDMounter::readFile(const char* path, uint8_t* buffer, size_t max_len) {
    File file = openFile(path, FILE_READ);
    if (!file) return 0;
    
    size_t bytes_read = file.read(buffer, max_len);
    file.close();
    
    clearError();
    return bytes_read;
}

bool SDMounter::writeFile(const char* path, const char* content) {
    return writeFile(path, (const uint8_t*)content, strlen(content));
}

bool SDMounter::writeFile(const char* path, const uint8_t* data, size_t len) {
    File file = openFile(path, FILE_WRITE);
    if (!file) return false;
    
    size_t written = file.write(data, len);
    file.close();
    
    if (written != len) {
        setError(14, "Write size mismatch");
        return false;
    }
    
    clearError();
    return true;
}

bool SDMounter::appendFile(const char* path, const char* content) {
    return appendFile(path, (const uint8_t*)content, strlen(content));
}

bool SDMounter::appendFile(const char* path, const uint8_t* data, size_t len) {
    File file = openFile(path, FILE_APPEND);
    if (!file) return false;
    
    size_t written = file.write(data, len);
    file.close();
    
    if (written != len) {
        setError(15, "Append size mismatch");
        return false;
    }
    
    clearError();
    return true;
}

bool SDMounter::seekFile(File& file, size_t position) {
    if (!file) {
        setError(34, "Invalid file handle for seek");
        return false;
    }
    
    if (!file.seek(position)) {
        setError(35, "Seek operation failed");
        return false;
    }
    
    clearError();
    return true;
}

size_t SDMounter::tellFile(File& file) {
    if (!file) {
        setError(36, "Invalid file handle for tell");
        return 0;
    }
    
    clearError();
    return file.position();
}

bool SDMounter::truncateFile(const char* path, size_t size) {
    if (!mounted) {
        setError(37, "SD card not mounted");
        return false;
    }
    
    String full_path = getFullPath(path);
    
    // Read existing data up to size
    File src = SD_MMC.open(full_path.c_str(), FILE_READ);
    if (!src) {
        setError(38, "Failed to open source file for truncate");
        return false;
    }
    
    size_t current_size = src.size();
    
    if (current_size <= size) {
        // File is already smaller or equal, just close and return
        src.close();
        clearError();
        return true;
    }
    
    // Create temporary file
    String temp_path = full_path + ".tmp";
    File dst = SD_MMC.open(temp_path.c_str(), FILE_WRITE);
    if (!dst) {
        src.close();
        setError(39, "Failed to create temp file for truncate");
        return false;
    }
    
    // Copy only 'size' bytes, checking every read and write
    uint8_t buffer[512];
    size_t remaining = size;
    bool ok = true;

    while (remaining > 0 && src.available()) {
        size_t to_read = (remaining < sizeof(buffer)) ? remaining : sizeof(buffer);
        size_t bytes_read = src.read(buffer, to_read);
        if (bytes_read == 0 || dst.write(buffer, bytes_read) != bytes_read) {
            ok = false;
            break;
        }
        remaining -= bytes_read;
    }

    src.close();
    dst.close();

    // The replacement must hold exactly 'size' bytes before the original is touched
    if (remaining > 0) ok = false;
    if (ok) {
        File check = SD_MMC.open(temp_path.c_str(), FILE_READ);
        ok = (check && check.size() == size);
        check.close();
    }
    if (!ok) {
        SD_MMC.remove(temp_path.c_str());
        setError(41, "Truncate failed - replacement could not be written");
        return false;
    }

    // Keep the original under a backup name until the replacement is in place.
    // An existing file with the backup name may be user data, so never delete it.
    String backup_path = full_path + ".bak";
    if (SD_MMC.exists(backup_path.c_str())) {
        SD_MMC.remove(temp_path.c_str());
        setError(42, String("Truncate failed - backup name already in use: ") + backup_path);
        return false;
    }
    if (!SD_MMC.rename(full_path.c_str(), backup_path.c_str())) {
        SD_MMC.remove(temp_path.c_str());
        setError(43, "Truncate failed - could not move original aside");
        return false;
    }
    if (!SD_MMC.rename(temp_path.c_str(), full_path.c_str())) {
        // Put the original back before reporting failure
        SD_MMC.rename(backup_path.c_str(), full_path.c_str());
        SD_MMC.remove(temp_path.c_str());
        setError(44, "Truncate failed - could not move replacement into place");
        return false;
    }

    if (!SD_MMC.remove(backup_path.c_str())) {
        // The file is truncated; only the old copy is left behind
        setError(45, String("Truncated, but backup was left behind: ") + backup_path);
        return true;
    }

    clearError();
    return true;
}

bool SDMounter::deleteFile(const char* path) {
    if (!mounted) {
        setError(16, "SD card not mounted");
        return false;
    }
    
    String full_path = getFullPath(path);
    
    if (!SD_MMC.remove(full_path.c_str())) {
        setError(17, String("Failed to delete: ") + full_path);
        return false;
    }
    
    clearError();
    return true;
}

bool SDMounter::renameFile(const char* old_path, const char* new_path) {
    if (!mounted) {
        setError(18, "SD card not mounted");
        return false;
    }
    
    String full_old = getFullPath(old_path);
    String full_new = getFullPath(new_path);
    
    if (!SD_MMC.rename(full_old.c_str(), full_new.c_str())) {
        setError(19, "Rename failed");
        return false;
    }
    
    clearError();
    return true;
}

bool SDMounter::copyFile(const char* src, const char* dst) {
    // Refuse to copy a file onto itself. Opening the destination for writing
    // would truncate the source before it is read.
    String src_path = getFullPath(src);
    String dst_path = getFullPath(dst);
    if (src_path == dst_path) {
        setError(40, "Copy source and destination are the same file");
        return false;
    }

    File src_file = openFile(src, FILE_READ);
    if (!src_file) return false;
    size_t expected = src_file.size();

    File dst_file = openFile(dst, FILE_WRITE);
    if (!dst_file) {
        src_file.close();
        return false;
    }

    size_t copied = copyFileInternal(src_file, dst_file);

    src_file.close();
    dst_file.close();

    // Success means every byte of the source was written. moveFile() deletes
    // the source based on this result, so a short copy must not pass.
    if (copied != expected) {
        SD_MMC.remove(dst_path.c_str()); // do not leave a truncated copy behind
        setError(20, "Copy failed - incomplete copy");
        return false;
    }

    clearError();
    return true;
}

bool SDMounter::moveFile(const char* src, const char* dst) {
    // copyFile() only succeeds for a complete copy, so the source is deleted only after that
    if (!copyFile(src, dst)) return false;
    return deleteFile(src);
}

bool SDMounter::existsFile(const char* path) {
    if (!mounted) return false;
    
    String full_path = getFullPath(path);
    return SD_MMC.exists(full_path.c_str());
}

size_t SDMounter::getFileSize(const char* path) {
    File file = openFile(path, FILE_READ);
    if (!file) return 0;
    
    size_t size = file.size();
    file.close();
    return size;
}

bool SDMounter::mkdir(const char* path) {
    if (!mounted) {
        setError(21, "SD card not mounted");
        return false;
    }
    
    String full_path = getFullPath(path);
    
    if (!SD_MMC.mkdir(full_path.c_str())) {
        setError(22, String("Failed to create directory: ") + full_path);
        return false;
    }
    
    clearError();
    return true;
}

bool SDMounter::rmdir(const char* path) {
    if (!mounted) {
        setError(23, "SD card not mounted");
        return false;
    }
    
    String full_path = getFullPath(path);
    
    if (!SD_MMC.rmdir(full_path.c_str())) {
        setError(24, String("Failed to remove directory: ") + full_path);
        return false;
    }
    
    clearError();
    return true;
}

bool SDMounter::rmdirRecursive(const char* path) {
    if (!mounted) {
        setError(25, "SD card not mounted");
        return false;
    }
    
    String full_path = getFullPath(path);
    return deleteDirectoryRecursive(full_path.c_str());
}

String SDMounter::listDir(const char* path, bool recursive) {
    if (!mounted) {
        setError(26, "SD card not mounted");
        return "";
    }

    clearError();
    size_t budget = kMaxListEntries;
    return listDirTree(path, recursive, 0, budget);
}

String SDMounter::listDirTree(const char* path, bool recursive, uint8_t depth, size_t& budget) {
    String full_path = getFullPath(path);
    String listing = "";

    File root = SD_MMC.open(full_path.c_str());
    if (!root || !root.isDirectory()) {
        setError(27, String("Not a directory: ") + full_path);
        return "";
    }

    File file = root.openNextFile();
    while (file) {
        // The shared budget caps the total number of entries across all recursion levels
        if (budget == 0) {
            setError(47, "Directory listing truncated at entry limit");
            file.close();
            break;
        }
        budget--;

        if (file.isDirectory()) {
            listing += "DIR : " + String(file.name()) + "\n";
            if (recursive) {
                if (depth >= kMaxDirDepth) {
                    setError(48, "Directory nesting too deep, not expanded");
                } else {
                    String sub_listing = listDirTree(file.name(), true, depth + 1, budget);
                    if (sub_listing.length() > 0) {
                        listing += sub_listing;
                    }
                }
            }
        } else {
            listing += "FILE: " + String(file.name()) + " (" + String(file.size()) + " bytes)\n";
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();

    return listing;
}

std::vector<String> SDMounter::listDirVector(const char* path) {
    std::vector<String> items;

    if (!mounted) {
        setError(28, "SD card not mounted");
        return items;
    }

    String full_path = getFullPath(path);
    File root = SD_MMC.open(full_path.c_str());

    if (!root || !root.isDirectory()) {
        setError(29, String("Not a directory: ") + full_path);
        return items;
    }

    bool truncated = false;
    File file = root.openNextFile();
    while (file) {
        if (items.size() >= kMaxListEntries) {
            truncated = true;
            file.close();
            break;
        }
        items.push_back(String(file.name()));
        file.close();
        file = root.openNextFile();
    }
    root.close();

    if (truncated) {
        setError(47, "Directory listing truncated at entry limit");
    } else {
        clearError();
    }
    return items;
}

File SDMounter::openDir(const char* path) {
    if (!mounted) {
        setError(30, "SD card not mounted");
        return File();
    }
    
    String full_path = getFullPath(path);
    File dir = SD_MMC.open(full_path.c_str());
    
    if (!dir || !dir.isDirectory()) {
        setError(31, String("Not a directory: ") + full_path);
        return File();
    }
    
    clearError();
    return dir;
}

bool SDMounter::changeDir(const char* path) {
    if (!mounted) {
        setError(32, "SD card not mounted");
        return false;
    }
    
    String new_dir = getFullPath(path);
    File dir = SD_MMC.open(new_dir.c_str());
    
    if (!dir || !dir.isDirectory()) {
        dir.close();
        setError(33, String("Not a directory: ") + new_dir);
        return false;
    }
    
    dir.close();
    current_dir = new_dir;
    if (!current_dir.endsWith("/")) {
        current_dir += "/";
    }
    
    clearError();
    Serial.printf("[SDMounter] Changed directory to: %s\n", current_dir.c_str());
    return true;
}

bool SDMounter::stressTest(uint32_t iterations) {
    if (!mounted) {
        Serial.println("[SDMounter] Stress test failed: not mounted");
        return false;
    }
    
    Serial.printf("[SDMounter] Starting stress test with %u iterations...\n", iterations);
    const char* test_file = "/stress_test.tmp";
    const char* test_data = "STRESS_TEST_DATA_0123456789";
    uint32_t failures = 0;
    
    unsigned long start = millis();
    
    for (uint32_t i = 0; i < iterations; i++) {
        // Write test
        if (!writeFile(test_file, test_data)) {
            failures++;
            continue;
        }
        
        // Read test
        String content = readFile(test_file);
        if (content != String(test_data)) {
            failures++;
        }
        
        // Delete test
        if (!deleteFile(test_file)) {
            failures++;
        }
        
        if (i % 10 == 0) {
            Serial.printf(".");
        }
    }
    
    unsigned long elapsed = millis() - start;
    
    Serial.printf("\n[SDMounter] Stress test complete:\n");
    Serial.printf("  Iterations: %u\n", iterations);
    Serial.printf("  Failures: %u\n", failures);
    Serial.printf("  Time: %lu ms\n", elapsed);
    Serial.printf("  Avg: %.2f ms/iteration\n", (float)elapsed / iterations);
    
    return failures == 0;
}

float SDMounter::readSpeedTest(size_t block_size, uint32_t iterations) {
    if (!mounted) {
        Serial.println("[SDMounter] Read speed test failed: not mounted");
        return 0.0f;
    }
    
    Serial.printf("[SDMounter] Read speed test (block: %u, iterations: %u)...\n", 
                  block_size, iterations);
    
    const char* test_file = "/speed_test.tmp";
    uint8_t* buffer = (uint8_t*)malloc(block_size);
    if (!buffer) {
        Serial.println("[SDMounter] Memory allocation failed");
        return 0.0f;
    }
    
    // Create test file
    File file = openFile(test_file, FILE_WRITE);
    if (!file) {
        free(buffer);
        return 0.0f;
    }
    
    for (size_t i = 0; i < block_size; i++) {
        buffer[i] = i % 256;
    }
    
    for (uint32_t i = 0; i < iterations; i++) {
        file.write(buffer, block_size);
    }
    file.close();
    
    // Read speed test
    unsigned long start = millis();
    file = openFile(test_file, FILE_READ);
    if (!file) {
        free(buffer);
        return 0.0f;
    }
    
    size_t total_read = 0;
    while (file.available()) {
        total_read += file.read(buffer, block_size);
    }
    file.close();
    
    unsigned long elapsed = millis() - start;
    
    deleteFile(test_file);
    free(buffer);
    
    float speed_kbps = (total_read / 1024.0) / (elapsed / 1000.0);
    Serial.printf("[SDMounter] Read speed: %.2f KB/s\n", speed_kbps);
    
    return speed_kbps;
}

float SDMounter::writeSpeedTest(size_t block_size, uint32_t iterations) {
    if (!mounted) {
        Serial.println("[SDMounter] Write speed test failed: not mounted");
        return 0.0f;
    }
    
    Serial.printf("[SDMounter] Write speed test (block: %u, iterations: %u)...\n", 
                  block_size, iterations);
    
    const char* test_file = "/speed_test.tmp";
    uint8_t* buffer = (uint8_t*)malloc(block_size);
    if (!buffer) {
        Serial.println("[SDMounter] Memory allocation failed");
        return 0.0f;
    }
    
    for (size_t i = 0; i < block_size; i++) {
        buffer[i] = i % 256;
    }
    
    unsigned long start = millis();
    File file = openFile(test_file, FILE_WRITE);
    if (!file) {
        free(buffer);
        return 0.0f;
    }
    
    size_t total_written = 0;
    for (uint32_t i = 0; i < iterations; i++) {
        total_written += file.write(buffer, block_size);
    }
    file.close();
    
    unsigned long elapsed = millis() - start;
    
    deleteFile(test_file);
    free(buffer);
    
    float speed_kbps = (total_written / 1024.0) / (elapsed / 1000.0);
    Serial.printf("[SDMounter] Write speed: %.2f KB/s\n", speed_kbps);
    
    return speed_kbps;
}

void SDMounter::dumpFsInfo() {
    Serial.println("\n========== SD CARD INFO ==========");
    Serial.printf("Status: %s\n", mounted ? "MOUNTED" : "NOT MOUNTED");
    
    if (!mounted) {
        Serial.println("==================================\n");
        return;
    }
    
    Serial.printf("Mount Point: %s\n", mount_point.c_str());
    Serial.printf("Current Dir: %s\n", current_dir.c_str());
    Serial.printf("Type: %s\n", getFsType().c_str());
    Serial.printf("Block Size: %u bytes\n", getBlockSize());
    Serial.printf("Sector Count: %llu\n", getSectorCount());
    Serial.printf("Total Space: %.2f MB\n", getTotalBytes() / 1048576.0);
    Serial.printf("Used Space: %.2f MB\n", getUsedBytes() / 1048576.0);
    Serial.printf("Free Space: %.2f MB\n", getFreeBytes() / 1048576.0);
    Serial.printf("Usage: %.1f%%\n", (getUsedBytes() * 100.0) / getTotalBytes());
    Serial.printf("Last Error: [%d] %s\n", last_error_code, last_error_msg.c_str());
    Serial.println("==================================\n");
}

bool SDMounter::isInserted() {
    // With hot-swap polling on, checkHotSwap() keeps last_card_state current,
    // so read that instead of probing the bus again.
    if (hotswap_enabled) return last_card_state;
    return checkCardPresent();
}

bool SDMounter::isRemoved() {
    return !isInserted();
}

void SDMounter::checkHotSwap() {
    if (!hotswap_enabled) return;
    
    unsigned long now = millis();
    // Poll every 200 ms while a card is present. While the slot is empty, poll once a
    // second, because every probe re-initializes the SD controller.
    unsigned long interval = last_card_state ? 200 : 1000;
    if (now - last_hotswap_check < interval) return;
    last_hotswap_check = now;
    
    if (debug_mode) {
        Serial.printf("[DEBUG] Checking card... last_state=%d mounted=%d\n", last_card_state, mounted);
    }
    
    bool current_state = checkCardPresent();
    
    if (debug_mode) {
        Serial.printf("[DEBUG] Card check result: current_state=%d\n", current_state);
    }
    
    // Card was removed
    if (last_card_state && !current_state) {
        Serial.println("[SDMounter] ⚠️ SD card removed!");
        if (mounted) {
            SD_MMC.end(); // Force unmount
            mounted = false;
            triggerUnmountCallback();
        }
        triggerCardRemovedCallback();
        last_card_state = false;
        return;
    }
    
    // Card was inserted
    if (!last_card_state && current_state) {
        Serial.println("[SDMounter] ✅ SD card inserted!");
        
        triggerCardInsertedCallback();
        
        // Mount only when auto-mount is enabled. The probe in checkCardPresent() releases
        // the driver, so the card is not mounted at this point.
        if (auto_mount_enabled && !mounted) {
            delay(200); // Give card time to stabilize
            Serial.println("[SDMounter] Auto-mounting inserted card...");
            mount(false, mount_point.c_str(), mode_1bit);
        }
        
        last_card_state = true;
        return;
    }
    
    // Update state
    last_card_state = current_state;
}

// Private helper methods
void SDMounter::setError(int code, const String& msg) {
    last_error_code = code;
    last_error_msg = msg;
    Serial.printf("[SDMounter] Error %d: %s\n", code, msg.c_str());
}

void SDMounter::clearError() {
    last_error_code = 0;
    last_error_msg = "";
}

String SDMounter::getFullPath(const char* path) {
    String raw = isAbsolutePath(path) ? String(path) : current_dir + String(path);
    return normalizeCardPath(raw);
}

bool SDMounter::isAbsolutePath(const char* path) {
    return (path != nullptr && path[0] == '/');
}

void SDMounter::triggerMountCallback() {
    if (on_mount_callback) {
        on_mount_callback();
    }
}

void SDMounter::triggerUnmountCallback() {
    if (on_unmount_callback) {
        on_unmount_callback();
    }
}

void SDMounter::triggerCardInsertedCallback() {
    if (on_card_inserted_callback) {
        on_card_inserted_callback();
    }
}

void SDMounter::triggerCardRemovedCallback() {
    if (on_card_removed_callback) {
        on_card_removed_callback();
    }
}

bool SDMounter::checkCardPresent() {
    // Simplified detection method
    
    if (!mounted) {
        // Not mounted - try a full mount to check
        // This is the only reliable way on ESP32 SD_MMC
        if (debug_mode) {
            Serial.println("[DEBUG] checkCardPresent: Not mounted, attempting begin()...");
        }
        
        SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
        
        if (SD_MMC.begin(mount_point.c_str(), mode_1bit)) {
            // Card is there. Release the driver again: the probe must not leave the
            // card mounted behind a mounted flag that says otherwise.
            SD_MMC.end();
            if (debug_mode) {
                Serial.println("[DEBUG] checkCardPresent: begin() SUCCESS - card present");
            }
            return true;
        } else {
            // No card
            if (debug_mode) {
                Serial.println("[DEBUG] checkCardPresent: begin() FAILED - no card");
            }
            return false;
        }
    } else {
        // Already mounted - verify it's still accessible
        if (debug_mode) {
            Serial.println("[DEBUG] checkCardPresent: Already mounted, verifying...");
        }
        
        // Quick check: card type
        uint8_t cardType = SD_MMC.cardType();
        if (cardType == CARD_NONE) {
            if (debug_mode) {
                Serial.println("[DEBUG] checkCardPresent: cardType is NONE - card removed");
            }
            return false;
        }
        
        // Deeper check: try to access root
        File root = SD_MMC.open("/");
        bool accessible = (root && root.isDirectory());
        if (root) root.close();
        
        if (debug_mode) {
            Serial.printf("[DEBUG] checkCardPresent: Root accessible=%d\n", accessible);
        }
        
        return accessible;
    }
}

size_t SDMounter::copyFileInternal(File& src, File& dst) {
    size_t total = 0;
    uint8_t buffer[512];
    
    while (src.available()) {
        size_t bytes_read = src.read(buffer, sizeof(buffer));
        size_t bytes_written = dst.write(buffer, bytes_read);
        total += bytes_written;
        
        if (bytes_written != bytes_read) {
            break;
        }
    }
    
    return total;
}

bool SDMounter::deleteDirectoryRecursive(const char* path, uint8_t depth) {
    // Refuse to recurse without bound: a deep tree would exhaust the task stack
    if (depth > kMaxDirDepth) return false;

    File dir = SD_MMC.open(path);
    if (!dir || !dir.isDirectory()) {
        return false;
    }

    File file = dir.openNextFile();
    while (file) {
        String file_path = String(path) + "/" + String(file.name());

        if (file.isDirectory()) {
            file.close();
            if (!deleteDirectoryRecursive(file_path.c_str(), depth + 1)) {
                dir.close();
                return false;
            }
        } else {
            file.close();
            if (!SD_MMC.remove(file_path.c_str())) {
                dir.close();
                return false;
            }
        }

        file = dir.openNextFile();
    }
    dir.close();

    return SD_MMC.rmdir(path);
}
