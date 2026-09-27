#!/usr/bin/env bash
# Smoke test of the real helper binary: two "players" (separate helper
# processes and data dirs) share one filesystem store. Player A creates the
# world and presses Play (-> HOST), publishes join info; player B presses
# Play (-> JOIN). Then A stops (final upload + release) and B hosts rev 2.
set -euo pipefail
cd "$(dirname "$0")/.."
WORK=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$WORK"' EXIT
go build -o "$WORK/helper" ./cmd/shared-world-helper
go build -o "$WORK/synthsave" ./tools/synthsave
mkdir -p "$WORK/cloud" "$WORK/a/data" "$WORK/a/saves" "$WORK/b/data" "$WORK/b/saves"
for p in a b; do
cat > "$WORK/$p/data/config.json" <<JSON
{"schemaVersion":1,"provider":{"type":"filesystem","root":"$WORK/cloud"},
 "worlds":[{"id":"our-factory","name":"Our Factory"}]}
JSON
"$WORK/helper" --data-dir "$WORK/$p/data" 2>"$WORK/$p/stderr.log" &
done
for p in a b; do for _ in $(seq 50); do [ -f "$WORK/$p/data/discovery.json" ] && break; sleep 0.1; done; done

call() { # player method path [json]
  local d="$WORK/$1/data/discovery.json"
  local port tok; port=$(python3 -c "import json;print(json.load(open('$d'))['port'])"); tok=$(python3 -c "import json;print(json.load(open('$d'))['token'])")
  curl -sS -X "$2" -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' ${4:+-d "$4"} "http://127.0.0.1:$port$3"
}
field() { python3 -c "import json,sys;v=json.load(sys.stdin);print(eval(sys.argv[1]))" "$1"; }
wait_state() { # player state
  for _ in $(seq 200); do s=$(call "$1" GET /v1/worlds/our-factory/session | field "v['state']"); [ "$s" = "$2" ] && return 0; sleep 0.1; done
  echo "timeout waiting for $1 -> $2 (state $s)"; call "$1" GET /v1/worlds/our-factory/session; exit 1
}
player() { echo "{\"playerId\":\"$1\",\"displayName\":\"$2\",\"platform\":\"steam\",\"saveDirectory\":\"$WORK/$3/saves\",\"gamePid\":$$}"; }

echo "== health (unauthenticated)"; curl -sS "http://127.0.0.1:$(python3 -c "import json;print(json.load(open('$WORK/a/data/discovery.json'))['port'])")/v1/health"; echo
echo "== second helper instance on same data dir is refused"
out=$("$WORK/helper" --data-dir "$WORK/a/data" 2>&1 || true)
if grep -q "already running" <<<"$out"; then echo ok; else echo "FAIL: $out"; exit 1; fi

"$WORK/synthsave" "$WORK/a/saves/OldSave.sav" "the factory, revision one"
echo "== A creates world from OldSave"
call a POST /v1/worlds "{\"worldId\":\"our-factory\",\"worldName\":\"Our Factory\",\"importSaveName\":\"OldSave\",$(player pa Reece a | sed 's/^{//')" | field "v['revision']['number']"

echo "== A presses Play"
call a POST /v1/worlds/our-factory/play "$(player pa Reece a)" >/dev/null
wait_state a READY_TO_HOST
call a GET /v1/worlds/our-factory/session | field "(v['decision'], v['revision'], v['saveName'])"
call a POST /v1/worlds/our-factory/session/started '{"join":{"kind":"online-session-id","value":"EOS-SESSION-1","backend":"EOS"}}' | field "v['state']"

echo "== B presses Play"
call b POST /v1/worlds/our-factory/play "$(player pb Vojta b)" >/dev/null
wait_state b JOIN_READY
call b GET /v1/worlds/our-factory/session | field "(v['decision'], v['hostName'], v['join'])"
[ ! -e "$WORK/b/saves/SharedWorld_our-factory.sav" ] && echo "B downloaded nothing: ok"
call b GET /v1/worlds | field "[(w['status'], w['hostName'], w['revision']) for w in v['worlds']]"

echo "== A saves and stops"
"$WORK/synthsave" "$WORK/a/saves/SharedWorld_our-factory.sav" "the factory, revision two"
call a POST /v1/worlds/our-factory/session/saved '{"saveName":"SharedWorld_our-factory","final":true}' | field "v['state']"
wait_state a IDLE
call b POST /v1/worlds/our-factory/session/ack >/dev/null
call b GET /v1/worlds | field "[(w['status'], w['revision'], w['lastHostName']) for w in v['worlds']]"

echo "== B presses Play again -> now HOST with revision 2"
call b POST /v1/worlds/our-factory/play "$(player pb Vojta b)" >/dev/null
wait_state b READY_TO_HOST
call b GET /v1/worlds/our-factory/session | field "(v['decision'], v['revision'])"
cmp "$WORK/a/saves/SharedWorld_our-factory.sav" "$WORK/b/saves/SharedWorld_our-factory.sav" && echo "B has A's revision 2: ok"

echo "== cloud layout"; (cd "$WORK/cloud" && find . -type f | sort)
echo "== sample helper log lines (A)"; grep -E 'lease_acquired|revision_committed|lease_released' "$WORK/a/data/logs/helper.log" | head -5
if grep -q "$(python3 -c "import json;print(json.load(open('$WORK/a/data/discovery.json'))['token'])")" "$WORK/a/data/logs/helper.log"; then echo "TOKEN LEAKED TO LOG"; exit 1; else echo "token not in logs: ok"; fi
echo SMOKE PASSED
