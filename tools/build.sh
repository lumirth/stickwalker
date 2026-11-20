#!/usr/bin/env bash
set -e
project=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
export IDF_PATH="${IDF_PATH:-$project/.deps/esp-idf}"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$project/.deps/idf-tools}"
export IDF_TARGET=esp32s3
export IDF_COMPONENT_MANAGER=0
release=$(head -n 1 "$project/VERSION")
version="$release-dev"
if commit=$(git -C "$project" rev-parse --short=12 HEAD 2>/dev/null); then
    if [[ -n $(git -C "$project" status --porcelain) ]]; then
        version+="+g$commit.dirty"
    elif git -C "$project" tag --points-at HEAD | grep -qx "firmware-v$release"; then
        version=$release
    else
        version+="+g$commit"
    fi
fi
. "$IDF_PATH/export.sh"
if [[ $# == 0 ]]; then set -- build; fi
exec idf.py -C "$project" -B "$project/build/firmware" \
    -DSDKCONFIG="$project/build/firmware/sdkconfig" -DSTICKWALKER_VERSION="$version" "$@"
