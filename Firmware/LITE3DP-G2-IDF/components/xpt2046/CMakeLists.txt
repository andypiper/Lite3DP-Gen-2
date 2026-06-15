# XPT2046_Touchscreen component wrapper
# Populated by: ../setup.sh (extracts XPT2046_Touchscreen-master.zip)

set(XPT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/XPT2046_Touchscreen-master")

if(NOT EXISTS "${XPT_DIR}/XPT2046_Touchscreen.h")
    message(FATAL_ERROR
        "XPT2046 source not found. Run Firmware/LITE3DP-G2-IDF/setup.sh first.")
endif()

idf_component_register(
    SRCS "${XPT_DIR}/XPT2046_Touchscreen.cpp"
    INCLUDE_DIRS "${XPT_DIR}"
    REQUIRES espressif__arduino-esp32
)
