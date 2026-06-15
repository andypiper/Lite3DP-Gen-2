# SdFat component wrapper
# Populated by: ../setup.sh (extracts SDFat.zip)

set(SDFAT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SDFat/src")

if(NOT EXISTS "${SDFAT_DIR}/SdFat.h")
    message(FATAL_ERROR
        "SdFat source not found. Run Firmware/LITE3DP-G2-IDF/setup.sh first.")
endif()

file(GLOB_RECURSE SDFAT_SRCS "${SDFAT_DIR}/*.cpp")

idf_component_register(
    SRCS ${SDFAT_SRCS}
    INCLUDE_DIRS "${SDFAT_DIR}"
    REQUIRES espressif__arduino-esp32
)
