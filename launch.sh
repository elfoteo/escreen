#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
if make -C "$DIR" -q 2>/dev/null; then
    : # Up to date
else
    notify-send "Building escreen..."
    if ! make -C "$DIR"; then
        notify-send "escreen compilation failed"
        exit 1
    fi
fi
"$DIR/escreen" &
