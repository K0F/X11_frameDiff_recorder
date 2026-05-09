#!/bin/bash

# 1. Get all videos and find the duration of the longest one
files=(*.mp4)
num_files=${#files[@]}
max_dur=0

for f in "${files[@]}"; do
    dur=$(ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "$f")
    if (( $(echo "$dur > $max_dur" | bc -l) )); then
        max_dur=$dur
    fi
done

echo "Longest video is $max_dur seconds. Padding others..."

input_args=()
v_prep=""

# 2. Prepare inputs: Grayscale + pad to the specific max duration
for (( i=0; i<$num_files; i++ )); do
    input_args+=("-i" "${files[$i]}")
    # tpad=stop_duration forces it to match the longest video exactly
    v_prep+="[$i:v]format=gray,tpad=stop_mode=add:stop_duration=$max_dur[v$i]; "
done

# 3. Build the Blend Chain
v_blend="[v0][v1]blend=all_mode='addition'[b1]"
for (( i=2; i<$num_files; i++ )); do
    prev=$((i-1))
    v_blend+="; [b$prev][v$i]blend=all_mode='addition'[b$i]"
done
last_b_label="[b$((num_files-1))]"

# 4. Audio Mix
a_mix=""
for (( i=0; i<$num_files; i++ )); do
    a_mix+="[$i:a]"
done
a_mix+="amix=inputs=$num_files:duration=longest:dropout_transition=0[a_out]"

# 5. Final Execution (No -shortest needed now)
ffmpeg "${input_args[@]}" \
-filter_complex "$v_prep $v_blend; $a_mix" \
-map "$last_b_label" -map "[a_out]" \
-c:v libx264 -crf 18 -pix_fmt yuv420p \
-c:a aac -b:a 192k \
output_fixed_exit.mp4
