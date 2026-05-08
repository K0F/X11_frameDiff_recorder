#!/bin/sh

#!/bin/bash

# Collect all video files (adjust extension if needed)
files=(*.mp4)
num_files=${#files[@]}

if [ "$num_files" -lt 2 ]; then
    echo "Need at least 2 videos to blend."
    exit 1
fi

# 1. Build the input arguments
input_args=()
for f in "${files[@]}"; do
    input_args+=("-i" "$f")
done

# 2. Build the Filter Complex
# We chain the blend filters: [0:v][1:v] -> [v1]; [v1][2:v] -> [v2], etc.
v_filter="[0:v][1:v]blend=all_mode='addition'[v1]"
for (( i=2; i<$num_files; i++ )); do
    prev=$((i-1))
    v_filter+="; [v$prev][$i:v]blend=all_mode='addition'[v$i]"
done

# The last label is [vN] where N = num_files - 1
last_v_label="[v$((num_files-1))]"

# 3. Build the Audio Mix
# All audio streams are mixed into one
a_filter=""
for (( i=0; i<$num_files; i++ )); do
    a_filter+="[$i:a]"
done
a_filter+="amix=inputs=$num_files[a_out]"

# 4. Execute FFmpeg
ffmpeg "${input_args[@]}" \
-filter_complex "$v_filter; $a_filter" \
-map "$last_v_label" -map "[a_out]" \
-c:v libx264 -crf 18 -pix_fmt yuv420p \
-c:a aac -b:a 192k \
output_blended.mp4
