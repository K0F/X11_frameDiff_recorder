#!/bin/sh
# -j 207360 skips to the middle line
# -N 720 reads one full scanline
# We use a loop to keep the 'oscillator' running

# This reads the whole buffer but filters for the middle freq
cat /dev/shm/sc_diff_buffer | play -t raw -r 7200 -e unsigned -b 8 -c 1 - bandpass 2880 100
