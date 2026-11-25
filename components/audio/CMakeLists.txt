# Solana OS audio: an optional component (menuconfig "Solana OS audio").
# The OS hooks in at four places: audio_boot() at start-up, the Lua `audio`
# module (main/lua/lua_audio.c), audio_release() when an app exits, and the
# Sound page in Settings. With CONFIG_SOLANA_AUDIO off the port is a stub,
# audio_available() is false and every call returns AUD_ERR_NODEV.
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

set(srcs ${SOLANA_AUDIO_SRCS})
if(CONFIG_SOLANA_AUDIO)
    list(APPEND srcs port/audio_port_esp.c port/dac_tlv320.c)
else()
    list(APPEND srcs port/audio_port_none.c)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${SOLANA_AUDIO_INCLUDE_DIRS}
    PRIV_INCLUDE_DIRS port
    PRIV_REQUIRES bsp esp_driver_i2s esp_driver_i2c nvs_flash esp_timer)

# The mixer is the one hot loop: speed over size, whatever the project picks.
set_source_files_properties(${SOLANA_AUDIO_CORE_SRCS} PROPERTIES COMPILE_OPTIONS "-O2")
