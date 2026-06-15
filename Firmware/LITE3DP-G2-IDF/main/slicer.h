#pragma once
#include "shared.h"
#include <stddef.h>   // size_t

// Detect the slicer format by inspecting known filenames inside folder.
// folder is relative to SD mount point, e.g. "MyPrint".
slicer_type_t slicer_detect(const char *folder);

// Build the full SD path for a given layer number.
// buf must be at least 80 bytes (Prusa with 24-char folder needs 67). layer is 0-based relative to the slicer's
// own numbering (Chitubox offset is applied internally).
void slicer_layer_path(char *buf, size_t buf_sz,
                       const char *folder, int layer,
                       slicer_type_t type);

// Count printable files (non-directory entries) in a folder.
int slicer_count_layers(const char *folder);

// List top-level directories on the SD card into names[][25].
// Returns the number of entries found (up to max_entries).
int sd_list_folders(char names[][25], int max_entries);
