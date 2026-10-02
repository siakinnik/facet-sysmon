#!/usr/bin/env bash
# Protocol smoke test: plays the core's side of a short session (hello, ping,
# shutdown) and checks the plugin's replies. FACET_RUNNER runs foreign-arch
# binaries (e.g. qemu-aarch64-static).
#   scripts/ci/smoke.sh build/sysmon.plugin/sysmon
set -euo pipefail
bin="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
runner=(${FACET_RUNNER:-})
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

input="$(printf '%s\n' \
    "{\"t\":\"hello\",\"api\":2,\"data_dir\":\"$work\",\"locale\":\"en\",\"timezone\":\"UTC\"}" \
    '{"t":"ping","seq":7}' \
    '{"t":"shutdown"}')"
out="$(printf '%s\n' "$input" | FACET_PLUGIN_DATA="$work" timeout 30 "${runner[@]}" "$bin")"
echo "$out"

check() {
    if ! grep -q "$1" <<<"$out"; then
        echo "smoke test failed: missing $2" >&2
        exit 1
    fi
}
check '"t":"hello"' "hello reply"
check '"id":"sysmon"' "plugin id"
check '"api":2' "API version"
check '"sdk":' "SDK version"
check '"t":"pong"' "pong"
check '"seq":7' "ping sequence"
check '"t":"tile"' "tile subtitle"
echo "ok: protocol session passed"
