#!/bin/sh
# Regenerate the binary boundary fixtures for `big <dir>` demos.
# The fixtures are sparse files (truncate = instant, almost no disk use)
# and are NOT committed; run this once after checkout.
set -eu
cd "$(dirname "$0")/fixtures"
mkdir -p sub
truncate -s 60M data.bin
truncate -s 40M data2.bin
truncate -s 3M movie.mp4
truncate -s 25M movie2.mkv
truncate -s 3M photo.jpg
truncate -s 150M setup.exe
truncate -s 90M setup2.exe
truncate -s 1M small.jpg
truncate -s 6M song.mp3
truncate -s 4M song2.mp3
truncate -s 10M sub/pagefile.sys
echo "fixtures ready: $(du -sh . | cut -f1) apparent, $(du -sh --apparent-size . | cut -f1) on disk"
