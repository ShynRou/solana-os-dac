# Source lists shared by the badge build (CMakeLists.txt here) and the
# browser emulator (emulator/CMakeLists.txt): the portable engine and the
# service over it. Each build adds its own audio_port.
set(SOLANA_AUDIO_CORE_SRCS
    core/aud_engine.c core/aud_voice.c core/aud_chan.c core/aud_timeline.c
    core/aud_song.c core/aud_wav.c core/aud_tables.c)
set(SOLANA_AUDIO_SRCS audio.c ${SOLANA_AUDIO_CORE_SRCS})
set(SOLANA_AUDIO_INCLUDE_DIRS include core)
