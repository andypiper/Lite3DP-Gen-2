# TFT_eSPI component wrapper
# Populated by: ../setup.sh (extracts TFT_eSPI-master.zip)

set(TFT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/TFT_eSPI-master")

if(NOT EXISTS "${TFT_DIR}/TFT_eSPI.cpp")
    message(FATAL_ERROR
        "TFT_eSPI source not found. Run Firmware/LITE3DP-G2-IDF/setup.sh first.")
endif()

idf_component_register(
    SRCS
        "${TFT_DIR}/TFT_eSPI.cpp"
        "${TFT_DIR}/Extensions/Button.cpp"
        "${TFT_DIR}/Extensions/Smooth_font.cpp"
        "${TFT_DIR}/Extensions/Sprite.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"  # our User_Setup.h comes first
        "${TFT_DIR}"
        "${TFT_DIR}/Extensions"
    REQUIRES
        espressif__arduino-esp32
)
