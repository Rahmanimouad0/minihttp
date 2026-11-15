#!/usr/bin/env bash
#
# End-to-end tests: run minihttp against a throwaway document root and talk
# to it with curl and with raw TCP through bash's /dev/tcp.
#
#   make integration        (or: bash tests/integration.sh)
#
set -u
cd "$(dirname "$0")/.."

BIN=./minihttp
PORT=${PORT:-18080}
URL="http://127.0.0.1:$PORT"
ROOT=$(mktemp -d)
LOG=$(mktemp)
DOWNLOAD=$(mktemp)
PID=
pass=0
fail=0

cleanup() {
    [[ -n $PID ]] && kill "$PID" 2>/dev/null
    rm -rf "$ROOT" "$LOG" "$DOWNLOAD"
}
trap cleanup EXIT

check() {  # check NAME EXPECTED ACTUAL
    if [[ $2 == "$3" ]]; then
        printf '  \033[32mok\033[0m    %s\n' "$1"
        pass=$((pass + 1))
    else
        printf '  \033[31mFAIL\033[0m  %s\n        expected: %s\n        got:      %s\n' "$1" "$2" "$3"
        fail=$((fail + 1))
    fi
}

start_server() {
    "$BIN" --port "$PORT" --root "$ROOT" "$@" >"$LOG" 2>&1 &
    PID=$!
    for _ in $(seq 50); do
        curl -s -o /dev/null "$URL/" && return 0
        sleep 0.1
    done
    echo "server did not start:" >&2
    cat "$LOG" >&2
    exit 1
}

stop_server() {  # stop_server SIGNAL   (sets $exit_status)
    kill -"$1" "$PID"
    wait "$PID"
    exit_status=$?
    PID=
}

status_of() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
header_of() { curl -s -D - -o /dev/null "${@:2}" | tr -d '\r' | grep -i "^$1:"; }
sha() { sha256sum | cut -d' ' -f1; }

# raw REQUEST: send bytes (printf %b escapes) and print the status line.
raw() {
    exec 3<>"/dev/tcp/127.0.0.1/$PORT"
    printf '%b' "$1" >&3
    local line=
    IFS= read -r -t 5 line <&3
    exec 3<&-
    echo "${line%$'\r'}"
}

# ---- fixture ------------------------------------------------------------
printf '<h1>hello</h1>\n' >"$ROOT/index.html"
printf 'body { color: red; }\n' >"$ROOT/style.css"
printf 'spaces\n' >"$ROOT/file with spaces.txt"
mkdir "$ROOT/docs" "$ROOT/empty"
printf 'docs index\n' >"$ROOT/docs/index.html"
ln -s /etc/passwd "$ROOT/escape"
head -c 32000000 /dev/urandom >"$ROOT/big.bin"
BIG_SHA=$(sha <"$ROOT/big.bin")

start_server

echo "static files"
check "GET /index.html -> 200"          200 "$(status_of "$URL/index.html")"
check "body served verbatim"            "<h1>hello</h1>" "$(curl -s "$URL/index.html")"
check "GET / serves index.html"         "<h1>hello</h1>" "$(curl -s "$URL/")"
check "percent-encoded file name"       spaces "$(curl -s "$URL/file%20with%20spaces.txt")"
check "query string ignored"            200 "$(status_of "$URL/index.html?v=2")"
check "32 MB file arrives intact"       "$BIG_SHA" "$(curl -s "$URL/big.bin" | sha)"

echo "headers"
check "Content-Type (html)"   "Content-Type: text/html; charset=utf-8" "$(header_of Content-Type "$URL/")"
check "Content-Type (css)"    "Content-Type: text/css; charset=utf-8"  "$(header_of Content-Type "$URL/style.css")"
check "Content-Length"        "Content-Length: 15"                     "$(header_of Content-Length "$URL/")"
check "Server"                "Server: minihttp/1.0"                   "$(header_of Server "$URL/")"
check "Date is RFC 9110"      1 "$(header_of Date "$URL/" | grep -cE '^Date: [A-Z][a-z]{2}, [0-9]{2} [A-Z][a-z]{2} [0-9]{4} [0-9:]{8} GMT$')"
check "Last-Modified"         1 "$(header_of Last-Modified "$URL/" | grep -c GMT)"
check "HEAD: 200, no body"    "200 0" "$(curl -s -I -o /dev/null -w '%{http_code} %{size_download}' "$URL/")"
check "HEAD: Content-Length"  "Content-Length: 15" "$(curl -s -I "$URL/" | tr -d '\r' | grep '^Content-Length')"

echo "directories"
check "/docs -> 301 /docs/"         "301 $URL/docs/" "$(curl -s -o /dev/null -w '%{http_code} %{redirect_url}' "$URL/docs")"
check "redirect keeps query"        "Location: /docs/?a=1" "$(header_of Location "$URL/docs?a=1")"
check "/docs/ serves its index"     "docs index" "$(curl -s "$URL/docs/")"
check "no index.html -> 404"        404 "$(status_of "$URL/empty/")"

echo "errors"
check "missing file -> 404"             404 "$(status_of "$URL/nope.html")"
check "POST -> 405"                     405 "$(status_of -d x "$URL/")"
check "405 lists allowed methods"       "Allow: GET, HEAD" "$(header_of Allow -X DELETE "$URL/")"
# --path-as-is stops curl from resolving the dot segments itself.
check "../ traversal -> 400"            400 "$(status_of --path-as-is "$URL/../../etc/passwd")"
check "%2e%2e traversal -> 400"         400 "$(status_of --path-as-is "$URL/%2e%2e/%2e%2e/etc/passwd")"
check "%2f-hidden traversal -> 400"     400 "$(status_of --path-as-is "$URL/docs/..%2f..%2fetc/passwd")"
check "symlink out of root -> 403"      403 "$(status_of "$URL/escape")"
check "encoded NUL -> 400"              400 "$(status_of "$URL/index.html%00.png")"
check "garbage -> 400"                  "HTTP/1.1 400 Bad Request" "$(raw 'NONSENSE\r\n\r\n')"
check "HTTP/1.1 without Host -> 400"    "HTTP/1.1 400 Bad Request" "$(raw 'GET / HTTP/1.1\r\n\r\n')"
check "malformed header -> 400"         "HTTP/1.1 400 Bad Request" "$(raw 'GET / HTTP/1.1\r\nHost: x\r\nno colon\r\n\r\n')"
check "HTTP/2.0 -> 505"                 "HTTP/1.1 505 HTTP Version Not Supported" "$(raw 'GET / HTTP/2.0\r\nHost: x\r\n\r\n')"
check "HTTP/1.0 without Host -> 200"    "HTTP/1.1 200 OK" "$(raw 'GET / HTTP/1.0\r\n\r\n')"
big_header=$(head -c 9000 /dev/zero | tr '\0' a)
check "9 KB of headers -> 431"          "HTTP/1.1 431 Request Header Fields Too Large" \
      "$(raw "GET / HTTP/1.1\r\nHost: x\r\nX-Big: $big_header\r\n\r\n")"

echo "connections"
check "keep-alive: 3 requests, 1 connection" "1 0 0" \
      "$(curl -s -o /dev/null -o /dev/null -o /dev/null -w '%{num_connects} ' "$URL/" "$URL/style.css" "$URL/docs/" | xargs)"
pipelined() {
    exec 3<>"/dev/tcp/127.0.0.1/$PORT"
    printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /style.css HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' >&3
    timeout 5 cat <&3 | tr -d '\r' | grep -c '^HTTP/1.1 200 OK$'
    exec 3<&-
}
check "2 pipelined requests, 2 responses" 2 "$(pipelined)"

echo "concurrency"
# Park idle connections: a single-threaded server would be stuck on them.
exec 4<>"/dev/tcp/127.0.0.1/$PORT" 5<>"/dev/tcp/127.0.0.1/$PORT" 6<>"/dev/tcp/127.0.0.1/$PORT"
check "served while 3 clients sit idle"  200 "$(status_of --max-time 2 "$URL/")"
exec 4<&- 5<&- 6<&-
check "200 parallel requests all 200"    "200 x200" \
      "$(seq 200 | xargs -P 50 -I{} curl -s -o /dev/null -w '%{http_code}\n' "$URL/" | sort | uniq -c | awk '{print $2 " x" $1}')"
check "8 parallel 32 MB downloads intact" "$BIG_SHA" \
      "$(seq 8 | xargs -P 8 -I{} sh -c "curl -s '$URL/big.bin' | sha256sum | cut -d' ' -f1" | sort -u)"

echo "graceful shutdown"
# A rate-limited download is still in flight when SIGTERM arrives.
curl -s --limit-rate 8M -o "$DOWNLOAD" "$URL/big.bin" &
CURL_PID=$!
sleep 1
stop_server TERM
wait "$CURL_PID"
check "SIGTERM -> exit status 0"        0 "$exit_status"
check "server waited for the download"  1 "$(grep -c 'waiting for 1 active' "$LOG")"
check "in-flight download completed"    "$BIG_SHA" "$(sha <"$DOWNLOAD")"
check "port is closed afterwards"       000 "$(status_of "$URL/")"

start_server
exec 4<>"/dev/tcp/127.0.0.1/$PORT"      # idle keep-alive style connection
sleep 0.2
started=$(date +%s%N)
stop_server INT
elapsed_ms=$(( ($(date +%s%N) - started) / 1000000 ))
exec 4<&-
check "SIGINT -> exit status 0"         0 "$exit_status"
check "idle client doesn't delay exit"  fast "$( ((elapsed_ms < 1000)) && echo fast || echo "${elapsed_ms} ms")"

echo "connection limit"
start_server --max-clients 1
exec 4<>"/dev/tcp/127.0.0.1/$PORT"      # takes the only slot
sleep 0.2
check "over --max-clients -> 503"       503 "$(status_of "$URL/")"
exec 4<&-
sleep 0.2
check "slot is reusable once freed"     200 "$(status_of "$URL/")"
stop_server TERM

echo
echo "integration tests: $pass passed, $fail failed"
((fail == 0))
