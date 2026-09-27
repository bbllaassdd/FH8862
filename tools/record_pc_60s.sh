#!/bin/sh
set -e

# The default is the main stream. Use "./record_pc_60s.sh 1235" for substream.
PORT="${1:-1234}"
OUTPUT="project_pc_port${PORT}_60s.ts"

echo "Recording udp://0.0.0.0:${PORT} for 60 seconds..."
ffmpeg -y -t 60 -i "udp://0.0.0.0:${PORT}?fifo_size=1000000&overrun_nonfatal=1" \
       -map 0:v:0 -c copy "$OUTPUT"
echo "Finished: $OUTPUT"
