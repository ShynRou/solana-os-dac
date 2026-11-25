#!/bin/sh
# Regenerate the WAV fixtures for test_a8_wav (needs ffmpeg). Checked in, so
# the tests don't need ffmpeg.
set -e
cd "$(dirname "$0")/fixtures"
src="-f lavfi -i sine=frequency=441:sample_rate=22050:duration=0.25"
ff="ffmpeg -loglevel error -y"
$ff $src -af volume=0.7 -ac 1 -c:a pcm_s16le -bitexact -map_metadata -1 pcm16.wav
$ff -i pcm16.wav -c:a pcm_u8 -bitexact -map_metadata -1 pcm8.wav
$ff -i pcm16.wav -c:a adpcm_ima_wav -bitexact -map_metadata -1 adpcm.wav
$ff -i adpcm.wav -f s16le -c:a pcm_s16le adpcm_ref.raw
$ff $src -ac 2 -t 0.01 -c:a pcm_s16le -bitexact -map_metadata -1 stereo.wav
ls -l
