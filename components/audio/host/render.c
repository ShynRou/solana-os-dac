// Render an event script to a WAV file, to listen to the engine on a PC.
//
//   make -C components/audio/host render
//   ./build/render scripts/waves.txt waves.wav
//
// Script format, one command per line, # starts a comment:
//   init <rate> [channels]          start an engine (1 MB pool)
//   synth <n> key=value ...         patch n: wave=sine|pulse|saw|triangle|noise filter=0|1
//                                   key=<0..1> lfo_cutoff=<0..1> targets=<mask> and any
//                                   parameter by name (cutoff=0.5 attack=5 ...; times in ms)
//   bank <n>                        an empty bank n
//   sample <n> file=x.wav root=60 lo=0 hi=127 loop=a,b release=ms
//   assign <ch> <n>
//   at <ms>                         render up to this time; later commands happen then
//   on <ch> <note> <vel> [param=value ...]   note on with a bundle; remembered as "last"
//   off <ch> <note> | offh          note off (offh: the last note's handle)
//   cc <ch> <cc> <value> | bend <ch> <-8192..8191> | param <ch> <name> <value>
//   nparam <name> <value> | nramp <name> <value> <ms>     on the last note
//   alloff [kill] | master <gain>

#include "harness.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: render <script> <out.wav>\n");
        return 2;
    }
    if (h_script(argv[1]) != 0 || !H.e) {
        return 1;
    }
    aud_stats_t st;
    aud_stats(H.e, &st);
    printf("%s: %zu frames, %u Hz, %u ch; dropped %u, stale %u, clipped %u\n", argv[2], H.frames, st.rate,
           st.channels, st.dropped_events, st.stale, st.clipped);
    return h_wav_write(argv[2]) == 0 ? 0 : 1;
}
