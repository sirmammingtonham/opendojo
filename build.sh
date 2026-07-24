#!/bin/sh
set -e
cd "$(dirname "$0")/dll"
cmake -B build -A x64 -DOPENDOJO_DEPLOY_DIR="C:/Program Files (x86)/Steam/steamapps/common/TEKKEN 8/Polaris/Binaries/Win64"
cmake --build build --config Release
