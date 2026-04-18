#!/bin/bash

echo "---------------------------------------"
echo "🛠️  Team 1 Friday Build Freeze #2"
echo "---------------------------------------"

HTTPS_DIR="../../https"
MAZE_DIR="../../maze"

# 1. Maze SDL2
if [ -f "$MAZE_DIR/maze_sdl2.c" ]; then
    echo "Compiling maze..."
    gcc "$MAZE_DIR/maze_sdl2.c" -o maze_sdl2 $(pkg-config --cflags --libs sdl2)
    [ $? -eq 0 ] && echo "✅ maze_sdl2 built"
fi

# 2. Redis server
if [ -f "$HTTPS_DIR/maze_https_redis.c" ]; then
    echo "Compiling redis server..."
    gcc "$HTTPS_DIR/maze_https_redis.c" -o maze_https_redis \
      -lmicrohttpd -lhiredis -lgnutls \
      $(PKG_CONFIG_PATH="/usr/local/Cellar/mongo-c-driver/2.3.0/lib/pkgconfig" pkg-config --cflags --libs mongoc2 bson2)
    [ $? -eq 0 ] && echo "✅ maze_https_redis built"
fi

# 3. MiniPupper server
if [ -f "$HTTPS_DIR/maze_https_minipupper.c" ]; then
    echo "Compiling minipupper..."
    gcc "$HTTPS_DIR/maze_https_minipupper.c" -o maze_https_minipupper \
      -lmicrohttpd -lcjson
    [ $? -eq 0 ] && echo "✅ maze_https_minipupper built"
fi

# Copy certs
if [ -d "$HTTPS_DIR/certs" ]; then
    cp -r "$HTTPS_DIR/certs" .
    echo "✅ certs copied"
fi

echo "---------------------------------------"
echo "🎉 Build complete"
