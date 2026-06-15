# PNGdec component wrapper
# Populated by: ../setup.sh (extracts PNGdec-master.zip)

set(PNG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/PNGdec-master/src")

if(NOT EXISTS "${PNG_DIR}/PNGdec.h")
    message(FATAL_ERROR
        "PNGdec source not found. Run Firmware/LITE3DP-G2-IDF/setup.sh first.")
endif()

file(GLOB PNG_SRCS "${PNG_DIR}/*.c" "${PNG_DIR}/*.cpp")

idf_component_register(
    SRCS ${PNG_SRCS}
    INCLUDE_DIRS "${PNG_DIR}"
    REQUIRES espressif__arduino-esp32
)
