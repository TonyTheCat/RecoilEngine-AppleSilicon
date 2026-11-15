#!/bin/bash
# launcher-test.sh — integration harness for launcher.sh startup gating.
# Stages a fake .app whose helpers (message-check, consent-dialog, spring, …)
# are recording stubs, then drives the REAL launcher through every path:
# versioned consent/notice acks, message-check kill-switch, quit handling,
# assume-consent + skip escapes, and the online-disabled marker. No GUI, no
# network, no engine. This is the coverage the original "disclaimer skipped
# because content already existed" bug needed.
set -uo pipefail
PKG="$(cd "$(dirname "$0")/.." && pwd)"
ROOT=$(mktemp -d)
# kill any launcher/stub child still referencing our sandbox before removing it
# (so a killed run can never orphan a launcher that then pops a GUI dialog)
trap 'pkill -f "$ROOT" 2>/dev/null; rm -rf "$ROOT"' EXIT INT TERM
APP="$ROOT/BAR Launcher.app"; MACOS="$APP/Contents/MacOS"; RES="$APP/Contents/Resources"
mkdir -p "$MACOS" "$RES/vulkan/icd.d"
export CALLS="$ROOT/calls.log"

cp "$PKG/launcher.sh" "$MACOS/launcher"; chmod +x "$MACOS/launcher"

# hermetic PATH: stub system tools the launcher touches (tmutil talks to
# backupd and can hang; osascript pops real GUI dialogs) so the test never
# depends on machine state. The osascript stub records fail_dialog calls.
STUBBIN="$ROOT/bin"; mkdir -p "$STUBBIN"
printf '#!/bin/bash\nexit 0\n' > "$STUBBIN/tmutil"; chmod +x "$STUBBIN/tmutil"
printf '#!/bin/bash\necho "osascript" >> "$CALLS"; exit 0\n' > "$STUBBIN/osascript"; chmod +x "$STUBBIN/osascript"
export PATH="$STUBBIN:$PATH"

# --- recording stubs -------------------------------------------------------
cat > "$MACOS/message-check" <<'S'
#!/bin/bash
echo "message-check" >> "$CALLS"; exit ${STUB_MC_EXIT:-0}
S
cat > "$MACOS/consent-dialog" <<'S'
#!/bin/bash
case "$*" in
  *--notice*) echo "consent-notice" >> "$CALLS"; exit 0;;
  *--server*) echo "consent-server" >> "$CALLS"; exit ${STUB_CONSENT_EXIT:-0};;
esac
S
cat > "$MACOS/spring" <<'S'
#!/bin/bash
echo "spring-launched" >> "$CALLS"; exit 0
S
cat > "$MACOS/error-dialog" <<'S'
#!/bin/bash
echo "error-dialog" >> "$CALLS"; exit 0
S
# progress-window stub: drains stdin like the real helper (exercises the
# launcher's fifo/forwarder path, incl. the no-wait success close)
cat > "$MACOS/progress-window" <<'S'
#!/bin/bash
echo "progress-window" >> "$CALLS"; cat > /dev/null; exit 0
S
chmod +x "$MACOS/progress-window"
cat > "$RES/download-content.sh" <<'S'
#!/bin/bash
case "$*" in *--print-server*) echo "repos-cdn.beyondallreason.dev"; exit 0;; esac
printf '@S go\n@P 100\n@DONE\n'; printf 'CONTENT_OK\n' >&2; exit ${STUB_DL_EXIT:-0}
S
chmod +x "$MACOS/message-check" "$MACOS/consent-dialog" "$MACOS/spring" "$MACOS/error-dialog" "$RES/download-content.sh"
echo '{}' > "$RES/chobby_config.json"
# the content set this "build" installs; the launcher records its digest in the
# sentinel, so a Skip is only offered when THIS set is already on disk
printf 'byar:test\nbyar-chobby:test\n' > "$RES/content_tags"
SIG=$(shasum "$RES/content_tags" | cut -c1-12)
echo '{}' > "$RES/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json"
cat > "$APP/Contents/Info.plist" <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>CFBundleShortVersionString</key><string>0.1</string></dict></plist>
P

pass=0; fail=0
ok()  { pass=$((pass+1)); printf "  ok   %s\n" "$1"; }
bad() { fail=$((fail+1)); printf "  FAIL %s\n" "$1"; [ -n "${2:-}" ] && printf "       calls: %s\n" "$2"; }

# run <writedir> [env assignments...]  -> populates $CALLS (reset each run)
run() { : > "$CALLS"; local wd="$1"; shift; timeout 15 env "$@" BAR_WRITEDIR_OVERRIDE="$wd" \
        "$MACOS/launcher" >/dev/null 2>&1 </dev/null; [ $? -eq 124 ] && echo "TIMEOUT" >> "$CALLS"; }
has()  { grep -qx "$1" "$CALLS"; }
calls(){ tr '\n' ',' < "$CALLS"; }
# installed <writedir> — a COMPLETE install of the current content set: sentinel
# carrying this build's signature + package indexes on disk (what makes a Skip safe)
installed() { mkdir -p "$1/packages"; printf '%s\n' "$SIG" > "$1/.lobby-installed"; : > "$1/packages/x.sdp"; }
online_on()  { : > "$RES/.online-play-disabled"; }
online_off() { rm -f "$RES/.online-play-disabled"; }

echo "== first run: notice + disclaimer shown, acks written, engine launched =="
online_on; WD="$ROOT/w1"; run "$WD"
{ has message-check && has consent-notice && has consent-server && has spring-launched; } \
  && ok "message-check + notice + disclaimer + launch" || bad "first run" "$(calls)"
[ "$(cat "$WD/.consent-ack" 2>/dev/null)" = "1" ] && ok ".consent-ack written (v1)" || bad "consent-ack"
[ "$(cat "$WD/.notice-ack"  2>/dev/null)" = "1" ] && ok ".notice-ack written (v1)"  || bad "notice-ack"

echo "== second run (same writedir): acked dialogs NOT reshown =="
run "$WD"
{ has message-check && ! has consent-notice && ! has consent-server && has spring-launched; } \
  && ok "notice + disclaimer suppressed once acked; message-check still runs" || bad "second run" "$(calls)"

echo "== the original bug: content present but consent NOT yet acked =="
WD2="$ROOT/w2"; mkdir -p "$WD2"; : > "$WD2/.lobby-installed"   # content 'already installed'
run "$WD2"
has consent-server && ok "disclaimer shows despite .lobby-installed (bug fixed)" || bad "bug regression" "$(calls)"

echo "== version bump re-asks once =="
echo 0 > "$WD/.consent-ack"; echo 0 > "$WD/.notice-ack"   # simulate CONSENT/NOTICE_VERSION bump
run "$WD"
{ has consent-server && has consent-notice; } && ok "stale ack -> both re-shown once" || bad "bump" "$(calls)"
run "$WD"
{ ! has consent-server && ! has consent-notice; } && ok "re-acked -> quiet again" || bad "bump re-ack" "$(calls)"

echo "== disclaimer Quit stops launch =="
WD3="$ROOT/w3"; run "$WD3" STUB_CONSENT_EXIT=1
{ has consent-server && ! has spring-launched; } && ok "Quit at disclaimer -> engine NOT launched" || bad "quit" "$(calls)"

echo "== kill-switch (message-check exit 2) stops launch, before dialogs =="
WD4="$ROOT/w4"; run "$WD4" STUB_MC_EXIT=2
{ has message-check && ! has consent-server && ! has spring-launched; } \
  && ok "message-check=2 -> quit before disclaimer, no launch" || bad "killswitch" "$(calls)"

echo "== online-disabled marker gates the notice =="
online_off; WD5="$ROOT/w5"; run "$WD5"
{ ! has consent-notice && has consent-server && has spring-launched; } \
  && ok "no marker -> no online notice; disclaimer still shown" || bad "no-marker" "$(calls)"
online_on

echo "== BAR_ASSUME_CONSENT / BAR_SKIP_CONTENT_CHECK escapes =="
WD6="$ROOT/w6"; run "$WD6" BAR_ASSUME_CONSENT=1
{ ! has message-check && ! has consent-server && ! has consent-notice && has spring-launched; } \
  && ok "assume-consent -> no dialogs, still launches" || bad "assume-consent" "$(calls)"
WD7="$ROOT/w7"; run "$WD7" BAR_SKIP_CONTENT_CHECK=1
{ ! has message-check && ! has consent-server && has spring-launched; } \
  && ok "skip-content-check -> whole block skipped, still launches" || bad "skip" "$(calls)"

echo "== progress window path: helper used, launch not blocked =="
WD8="$ROOT/w8"; run "$WD8" BAR_ASSUME_CONSENT=1
{ has progress-window && has spring-launched && ! has TIMEOUT; } \
  && ok "fifo/forwarder path drives helper and still launches" || bad "fifo path" "$(calls)"

echo "== first-run download failure -> error-dialog, NO launch =="
WD9="$ROOT/w9"; run "$WD9" BAR_ASSUME_CONSENT=1 STUB_DL_EXIT=7
{ has error-dialog && ! has spring-launched; } \
  && ok "first-run dl fail -> error dialog, engine NOT launched" || bad "first-run fail" "$(calls)"

echo "== update-check failure on existing install -> soft, still launches =="
WD10="$ROOT/w10"; mkdir -p "$WD10"; : > "$WD10/.lobby-installed"
run "$WD10" BAR_ASSUME_CONSENT=1 STUB_DL_EXIT=7
{ ! has error-dialog && has spring-launched; } \
  && ok "update fail -> logged, plays on existing content" || bad "soft fail" "$(calls)"
grep -q "update check failed" "$WD10/first-run-download.log" \
  && ok "soft failure recorded in log for support" || bad "soft fail log"

echo "== missing engine binary -> dialog, no silent exit 127 =="
mv "$MACOS/spring" "$MACOS/spring.bak"
WD11="$ROOT/w11"; run "$WD11" BAR_ASSUME_CONSENT=1 BAR_SKIP_CONTENT_CHECK=1
{ has osascript && ! has spring-launched; } \
  && ok "missing spring -> fail_dialog shown" || bad "missing spring" "$(calls)"
mv "$MACOS/spring.bak" "$MACOS/spring"

echo "== partial-update integrity: failure rolls rapid/ back =="
WD12="$ROOT/w12"; mkdir -p "$WD12/rapid/host/byar"; : > "$WD12/.lobby-installed"
echo "OLD-VERSION" > "$WD12/rapid/host/byar/versions.gz"
cat > "$RES/download-content.sh" <<'S'
#!/bin/bash
case "$*" in *--print-server*) echo "x"; exit 0;; esac
WD=""; while [ $# -gt 0 ]; do case "$1" in --writedir) WD=$2; shift 2;; *) shift;; esac; done
mkdir -p "$WD/rapid/host/byar"
echo "NEW-BROKEN" > "$WD/rapid/host/byar/versions.gz"   # metadata refreshed...
printf '@E:content boom\n'; exit 7                       # ...then download dies
S
chmod +x "$RES/download-content.sh"
run "$WD12" BAR_ASSUME_CONSENT=1
{ [ "$(cat "$WD12/rapid/host/byar/versions.gz")" = "OLD-VERSION" ] \
  && [ ! -d "$WD12/rapid.pre-update" ] && has spring-launched; } \
  && ok "failed update -> old rapid metadata restored, game launches" || bad "rollback" "$(calls)"

echo "== partial-update integrity: success keeps new metadata =="
cat > "$RES/download-content.sh" <<'S'
#!/bin/bash
case "$*" in *--print-server*) echo "x"; exit 0;; esac
WD=""; while [ $# -gt 0 ]; do case "$1" in --writedir) WD=$2; shift 2;; *) shift;; esac; done
mkdir -p "$WD/rapid/host/byar"; echo "NEW-GOOD" > "$WD/rapid/host/byar/versions.gz"
printf '@S go\n@DONE\n'; exit 0
S
chmod +x "$RES/download-content.sh"
run "$WD12" BAR_ASSUME_CONSENT=1
{ [ "$(cat "$WD12/rapid/host/byar/versions.gz")" = "NEW-GOOD" ] \
  && [ ! -d "$WD12/rapid.pre-update" ]; } \
  && ok "successful update -> new metadata kept, snapshot removed" || bad "commit" "$(calls)"

echo "== crashed mid-update (leftover snapshot) -> restored on next launch =="
echo "NEWER-PARTIAL" > "$WD12/rapid/host/byar/versions.gz"
mkdir -p "$WD12/rapid.pre-update/host/byar"
echo "NEW-GOOD" > "$WD12/rapid.pre-update/host/byar/versions.gz"   # what a crash leaves
run "$WD12" BAR_ASSUME_CONSENT=1
[ "$(cat "$WD12/rapid/host/byar/versions.gz")" = "NEW-GOOD" ] \
  && ok "leftover snapshot rolled back before checking" || bad "crash recovery"

echo "== skip button: downloader killed, rollback, game launches =="
cat > "$RES/download-content.sh" <<'S'
#!/bin/bash
case "$*" in *--print-server*) echo "x"; exit 0;; esac
WD=""; while [ $# -gt 0 ]; do case "$1" in --writedir) WD=$2; shift 2;; *) shift;; esac; done
mkdir -p "$WD/rapid/host/byar"; echo "HALF-DONE" > "$WD/rapid/host/byar/versions.gz"
printf '@S Downloading…\n'
sleep 20   # long download: only Skip can end this quickly
printf '@DONE\n'; exit 0
S
chmod +x "$RES/download-content.sh"
# helper stub: records the skip-file arg, clicks Skip after 0.5s, drains stdin
cat > "$MACOS/progress-window" <<'S'
#!/bin/bash
SKIP=""; while [ $# -gt 0 ]; do case "$1" in --skip-file) SKIP=$2; shift 2;; *) shift;; esac; done
echo "progress-window${SKIP:+-skippable}" >> "$CALLS"
[ -n "$SKIP" ] && { (sleep 0.5; touch "$SKIP") & }
cat > /dev/null; exit 0
S
chmod +x "$MACOS/progress-window"
WD13="$ROOT/w13"; mkdir -p "$WD13/rapid/host/byar"; installed "$WD13"
echo "OLD-VERSION" > "$WD13/rapid/host/byar/versions.gz"
T0=$(date +%s)
run "$WD13" BAR_ASSUME_CONSENT=1
T1=$(date +%s)
{ has progress-window-skippable && has spring-launched && [ $((T1-T0)) -lt 12 ] \
  && [ "$(cat "$WD13/rapid/host/byar/versions.gz")" = "OLD-VERSION" ] \
  && grep -q "update skipped by user" "$WD13/first-run-download.log"; } \
  && ok "skip -> downloader stopped ($((T1-T0))s), old version intact, launched" \
  || bad "skip" "$(calls) t=$((T1-T0))s"

echo "== first run: skip button NOT offered =="
cat > "$RES/download-content.sh" <<'S'
#!/bin/bash
case "$*" in *--print-server*) echo "x"; exit 0;; esac
printf '@S go\n@DONE\n'; exit 0
S
chmod +x "$RES/download-content.sh"
WD14="$ROOT/w14"; run "$WD14" BAR_ASSUME_CONSENT=1
{ has progress-window && ! has progress-window-skippable; } \
  && ok "first run -> no skip-file passed to the window" || bad "first-run skip" "$(calls)"
[ "$(cat "$WD14/.lobby-installed")" = "$SIG" ] \
  && ok "success records WHICH content set is installed" || bad "sentinel signature"
mkdir -p "$WD14/packages"; : > "$WD14/packages/x.sdp"   # what the real downloader leaves
run "$WD14" BAR_ASSUME_CONSENT=1
has progress-window-skippable \
  && ok "next launch (same content set) -> skip offered" || bad "skip after install" "$(calls)"

echo "== the reported bug: v0.11-style sentinel, game not actually installed =="
# v0.11 fetched the dummy byar:stable and wrote a BARE sentinel; the v0.12 tag
# fix makes the next launch the user's FIRST real game download — offering Skip
# there strands them with no game at all.
WD17="$ROOT/w17"; mkdir -p "$WD17/packages"; : > "$WD17/.lobby-installed"; : > "$WD17/packages/x.sdp"
run "$WD17" BAR_ASSUME_CONSENT=1
{ has progress-window && ! has progress-window-skippable; } \
  && ok "legacy sentinel -> no skip during the first real download" || bad "legacy skip" "$(calls)"

echo "== required content set changed -> not skippable until it is installed =="
WD18="$ROOT/w18"; installed "$WD18"; echo "some-other-sig" > "$WD18/.lobby-installed"
run "$WD18" BAR_ASSUME_CONSENT=1
{ ! has progress-window-skippable && has spring-launched; } \
  && ok "stale content signature -> no skip, launch still allowed" || bad "sig change" "$(calls)"

echo "== content wiped from disk -> no skip even with a current sentinel =="
WD19="$ROOT/w19"; installed "$WD19"; rm -rf "$WD19/packages"
run "$WD19" BAR_ASSUME_CONSENT=1
{ has progress-window && ! has progress-window-skippable; } \
  && ok "no package indexes -> nothing to fall back on, no skip" || bad "wiped content" "$(calls)"

echo "== stale signature must NOT make a failure fatal (soft-fail preserved) =="
# CAN_SKIP is deliberately stricter than FIRST_RUN: a stale signature must never
# turn a flaky network into "you cannot launch" on an install that still works.
cp "$RES/download-content.sh" "$ROOT/dl-ok.sh"
cat > "$RES/download-content.sh" <<'S'
#!/bin/bash
case "$*" in *--print-server*) echo "x"; exit 0;; esac
printf '@S go\n@E:network no connection\n'; exit 5
S
chmod +x "$RES/download-content.sh"
WD20="$ROOT/w20"; installed "$WD20"; echo "some-other-sig" > "$WD20/.lobby-installed"
run "$WD20" BAR_ASSUME_CONSENT=1
{ ! has error-dialog && has spring-launched; } \
  && ok "stale sig + download failure -> still plays offline" || bad "stale sig soft fail" "$(calls)"
cp "$ROOT/dl-ok.sh" "$RES/download-content.sh"

echo "== single-instance lock: second launcher refused while first runs =="
cat > "$RES/download-content.sh" <<'S'
#!/bin/bash
case "$*" in *--print-server*) echo "x"; exit 0;; esac
printf '@S slow\n'; sleep 4; printf '@DONE\n'; exit 0
S
chmod +x "$RES/download-content.sh"
rm -f "$MACOS/progress-window"   # no helper: no skip, keeps this test simple
WD15="$ROOT/w15"; mkdir -p "$WD15"; : > "$WD15/.lobby-installed"
: > "$CALLS"
env BAR_ASSUME_CONSENT=1 BAR_WRITEDIR_OVERRIDE="$WD15" "$MACOS/launcher" >/dev/null 2>&1 </dev/null & L1=$!
sleep 1
env BAR_ASSUME_CONSENT=1 BAR_WRITEDIR_OVERRIDE="$WD15" "$MACOS/launcher" >/dev/null 2>&1 </dev/null
wait "$L1" 2>/dev/null
{ has osascript && [ "$(grep -cx spring-launched "$CALLS")" = "1" ]; } \
  && ok "second instance -> dialog + refused; exactly one engine launch" || bad "lock" "$(calls)"

echo "== stale lock from a dead process is reclaimed =="
WD16="$ROOT/w16"; mkdir -p "$WD16/.launcher-lock"; : > "$WD16/.lobby-installed"
echo 99999999 > "$WD16/.launcher-lock/pid"    # no such pid
run "$WD16" BAR_ASSUME_CONSENT=1
has spring-launched && ok "dead-pid lock reclaimed, launch proceeds" || bad "stale lock" "$(calls)"

echo "== stale lock whose pid now belongs to an unrelated process is reclaimed =="
WD17="$ROOT/w17"; mkdir -p "$WD17/.launcher-lock"; : > "$WD17/.lobby-installed"
sleep 30 & OTHER=$!                              # a live pid that is not a launcher or engine
echo "$OTHER" > "$WD17/.launcher-lock/pid"
run "$WD17" BAR_ASSUME_CONSENT=1
kill "$OTHER" 2>/dev/null
{ ! has osascript && has spring-launched; } \
  && ok "reused-pid lock reclaimed, launch proceeds" || bad "reused-pid lock" "$(calls)"

echo
echo "RESULT: $pass passed, $fail failed"
[ "$fail" = 0 ]
