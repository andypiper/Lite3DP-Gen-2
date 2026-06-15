#include "slicer.h"
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG  = "slicer";
static const char *ROOT = "/sdcard";

// ── Internal helpers ───────────────────────────────────────────────────────

static bool file_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

// ── Public API ─────────────────────────────────────────────────────────────

slicer_type_t slicer_detect(const char *folder) {
    char path[64];

    // Prusa: foldersel00000.png
    snprintf(path, sizeof(path), "%s/%s/%s00000.png", ROOT, folder, folder);
    if (file_exists(path)) { ESP_LOGI(TAG, "Prusa"); return SLICER_PRUSA; }

    // Lychee 4-digit: lychee0000.png
    snprintf(path, sizeof(path), "%s/%s/lychee0000.png", ROOT, folder);
    if (file_exists(path)) { ESP_LOGI(TAG, "Lychee4"); return SLICER_LYCHEE4; }

    // Lychee 3-digit: lychee000.png
    snprintf(path, sizeof(path), "%s/%s/lychee000.png", ROOT, folder);
    if (file_exists(path)) { ESP_LOGI(TAG, "Lychee3"); return SLICER_LYCHEE3; }

    // Voxeldance: starts at 0.png
    snprintf(path, sizeof(path), "%s/%s/0.png", ROOT, folder);
    if (file_exists(path)) { ESP_LOGI(TAG, "Voxeldance"); return SLICER_VOXELDANCE; }

    // Chitubox: starts at 1.png (no 0.png)
    snprintf(path, sizeof(path), "%s/%s/1.png", ROOT, folder);
    if (file_exists(path)) { ESP_LOGI(TAG, "Chitubox"); return SLICER_CHITUBOX; }

    ESP_LOGW(TAG, "unknown slicer format in '%s'", folder);
    return SLICER_UNKNOWN;
}

void slicer_layer_path(char *buf, size_t buf_sz,
                       const char *folder, int layer,
                       slicer_type_t type) {
    switch (type) {
        case SLICER_PRUSA:
            // foldersel00000.png — 5-digit zero-padded, 0-based
            snprintf(buf, buf_sz, "%s/%s/%s%05d.png", ROOT, folder, folder, layer);
            break;

        case SLICER_LYCHEE4:
            // lychee0000.png — 4-digit zero-padded, 0-based
            snprintf(buf, buf_sz, "%s/%s/lychee%04d.png", ROOT, folder, layer);
            break;

        case SLICER_LYCHEE3:
            // lychee000.png — 3-digit zero-padded, 0-based
            snprintf(buf, buf_sz, "%s/%s/lychee%03d.png", ROOT, folder, layer);
            break;

        case SLICER_VOXELDANCE:
            // 0.png, 1.png, ... (0-based)
            snprintf(buf, buf_sz, "%s/%s/%d.png", ROOT, folder, layer);
            break;

        case SLICER_CHITUBOX:
            // 1.png, 2.png, ... (1-based)
            snprintf(buf, buf_sz, "%s/%s/%d.png", ROOT, folder, layer + 1);
            break;

        default:
            buf[0] = '\0';
            break;
    }
}

int slicer_count_layers(const char *folder) {
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", ROOT, folder);

    DIR *d = opendir(path);
    if (!d) return 0;

    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type == DT_REG) count++;
    }
    closedir(d);
    return count;
}

int sd_list_folders(char names[][25], int max_entries) {
    DIR *d = opendir(ROOT);
    if (!d) return 0;

    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < max_entries) {
        if (e->d_type == DT_DIR && e->d_name[0] != '.') {
            strncpy(names[count], e->d_name, 24);
            names[count][24] = '\0';
            count++;
        }
    }
    closedir(d);
    return count;
}
