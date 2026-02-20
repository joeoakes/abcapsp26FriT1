

set -e  # Exit immediately on error

SRC="https/maze_https_mongo.c"
OUT="maze_https_mongo"

echo "Compiling $SRC -> $OUT"

gcc -O2 -Wall -Wextra -std=c11 \
    "$SRC" \
    -o "$OUT" \
    $(pkg-config --cflags --libs libmicrohttpd mongoc2 gnutls)

echo "Build successful."
