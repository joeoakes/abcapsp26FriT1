

#!/usr/bin/env bash
set -e  # Exit immediately on error

echo "=== Building Project Apps ==="
echo

# Create output folder
mkdir -p bin

############################################
# 1) Build maze_sdl2
############################################
echo "Compiling maze/testmaze_sdl2.c -> bin/testmaze_sdl2"

gcc -O2 -Wall -Wextra -std=c11 \
    maze/testmaze_sdl2.c \
    -o bin/testmaze_sdl2 \
    $(pkg-config --cflags --libs sdl2) -lcurl

echo "maze_testsdl2 built successfully."
echo

############################################
# 2) Build maze_https_mongo
############################################
echo "Compiling https/maze_https_mongo.c -> bin/maze_https_mongo"

gcc -O2 -Wall -Wextra -std=c11 \
    https/maze_https_mongo.c \
    -o bin/maze_https_mongo \
    $(pkg-config --cflags --libs libmicrohttpd libmongoc-1.0 gnutls)

echo "maze_https_mongo built successfully."
echo

############################################
# 3) Build maze_https_redis
############################################
echo "Compiling https/maze_https_redis.c -> bin/maze_https_redis"

gcc -O2 -Wall -Wextra -std=c11 \
    https/maze_https_redis.c \
    -o bin/maze_https_redis \
    $(pkg-config --cflags --libs libmicrohttpd hiredis libmongoc-1.0 gnutls)

echo "maze_https_redis built successfully."
echo

############################################
# 4) Build maze_https_minipupper
############################################
echo "Compiling https/maze_https_minipupper.c -> bin/maze_https_minipupper"

gcc -O2 -Wall -Wextra -std=c11 \
    https/maze_https_minipupper.c \
    -o bin/maze_https_minipupper \
    $(pkg-config --cflags --libs libmicrohttpd gnutls libcjson)

echo "maze_https_minipupper built successfully."
echo

echo "=== All builds complete ==="
ls -lh bin
