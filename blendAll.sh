#!/bin/bash

# Collect files
files=(*.mp4)
num_files=${#files[@]}

if [ "$num_files" -lt 2 ]; then
    echo "Need at least 2 videos."
    exit 1
fi

input_args=()
v_prep=""

# 1. Force every input to GRAYSCALE immediately
for (( i=0; i<$num_files; i++ )); do
    input_args+=("-i" "${files[$i]}")
    v_prep+="[$i:v]format=gray[v$i]; "
done

# 2. Chain the blend (now only working on the Y plane)
v_blend="[v0][v1]blend=all_mode='addition'[b1]"
for (( i=2; i<$num_files; i++ )); do
    prev=$((i-1))
    v_blend+="; [b$prev][v$i]blend=all_mode='addition'[b$i]"
done

last_b_label="[b$((num_files-1))]"

# 3. Audio Mix
a_mix=""
for (( i=0; i<$num_files; i++ )); do
    a_mix+="[$i:a]"
done
a_mix+="amix=inputs=$num_files[a_out]"

# 4. Final Command
# We convert back to yuv420p at the end just for player compatibility
ffmpeg "${input_args[@]}" \
-filter_complex "$v_prep $v_blend; $a_mix" \
-map "$last_b_label" -map "[a_out]" \
-c:v libx264 -crf 18 -pix_fmt yuv420p \
-c:a aac -b:a 192k \
output_pure_bw.mp4
