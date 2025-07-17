#!/bin/sh

set -e
. fsdeps/system.sh

fsdeps/packages/gettext
fsdeps/packages/glib
fsdeps/packages/libmpeg2
fsdeps/packages/libpng
fsdeps/packages/libsamplerate
#if [ "$SYSTEM_OS" = "macOS" ]; then
#echo "Skipping OpenAL on macOS"
#else
#fsdeps/packages/openal
#fi
fsdeps/packages/portmidi
fsdeps/packages/SDL2
fsdeps/packages/SDL2_ttf
