#!/usr/bin/env bash
# Any audio file to a sample the badge's loader reads: mono WAV, PCM16 or
# IMA ADPCM (4 bits, a quarter of the size), at the rate you pick. Needs ffmpeg.
#
#   audio_prep.sh in.mp3 out.wav                 # PCM16, 22050 Hz
#   audio_prep.sh -r 32000 -a in.flac out.wav    # IMA ADPCM, 32 kHz
#   audio_prep.sh -t 1.5 -g -3 in.wav out.wav    # first 1.5 s, peak at -3 dB
#
# Pool cost once loaded (everything is decoded to PCM16): 2 bytes per frame,
# so 1 s at 22050 Hz is 43 KB whichever format the file is. Loading reads the
# file inside the app's callback: 1 VM step per 4 bytes (an ADPCM file costs a
# quarter of the PCM16 one), 200 000 steps per callback.
set -euo pipefail

rate=22050 adpcm=0 dur="" gain=""
usage() { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }
while getopts "r:at:g:h" o; do
    case $o in
        r) rate=$OPTARG ;;
        a) adpcm=1 ;;
        t) dur=$OPTARG ;;
        g) gain=$OPTARG ;;
        h) usage 0 ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 2 ] || usage
command -v ffmpeg >/dev/null || { echo "ffmpeg not found" >&2; exit 1; }
case $rate in 8000|11025|16000|22050|32000|44100|48000) ;; *) echo "rate: up to 48000" >&2; exit 1 ;; esac

args=(-hide_banner -loglevel error -y -i "$1" -ac 1 -ar "$rate")
[ -n "$dur" ] && args+=(-t "$dur")
filters=()
if [ -n "$gain" ]; then
    # Normalise the peak to $gain dBFS (two passes: measure, then scale).
    peak=$(ffmpeg -hide_banner -i "$1" -ac 1 -af volumedetect -f null - 2>&1 | sed -n 's/.*max_volume: \(-\{0,1\}[0-9.]*\) dB/\1/p')
    filters+=("volume=$(python3 -c "print(${gain} - (${peak:-0}))")dB")
fi
[ ${#filters[@]} -gt 0 ] && args+=(-af "$(IFS=,; echo "${filters[*]}")")
if [ $adpcm = 1 ]; then
    # 256-byte blocks (505 frames): the layout the badge's decoder reads.
    args+=(-c:a adpcm_ima_wav -block_size 256)
else
    args+=(-c:a pcm_s16le)
fi
ffmpeg "${args[@]}" "$2"
frames=$(ffprobe -v error -count_packets -show_entries stream=duration_ts -of csv=p=0 "$2" 2>/dev/null || echo "?")
echo "$2: $(stat -c %s "$2" 2>/dev/null || stat -f %z "$2") bytes, ${rate} Hz mono$( [ $adpcm = 1 ] && echo ", IMA ADPCM"), pool $((${frames:-0} * 2 / 1024)) KB"
