#!/bin/sh
# Roda os testes de host (Fases 2/3/4 do TASKS2.md) dentro do Docker.
# Uso: docker run --rm --network host -v <repo>:/repo gcc:13 sh /repo/tests/run_host_tests.sh
set -e
cd /repo

gcc -std=gnu99 -D_DEFAULT_SOURCE -Wall -Wextra -g -O1 \
    -Isrc -Itests \
    tests/test_host.c tests/posix_net.c tests/host_assets.c \
    src/net/net_stream.c src/net/netmsg.c src/net/xtea.c src/net/tibia_rsa.c \
    src/net/login.c src/net/game_protocol.c \
    src/poc/game_map.c src/poc/dat_loader.c \
    src/thirdparty/bn.c \
    -o /tmp/test_host -lm

LOGIN_PORT=19772
GAME_PORT=19773
python3 tools/mock_tibia_server.py --login-port "$LOGIN_PORT" --game-port "$GAME_PORT" &
MOCK_PID=$!
sleep 0.5

/tmp/test_host /repo/Tibia.dat 127.0.0.1 "$LOGIN_PORT" "$GAME_PORT"
STATUS=$?

kill $MOCK_PID 2>/dev/null || true
exit $STATUS
