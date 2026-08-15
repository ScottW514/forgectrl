#!/bin/sh
# fflog_e2e.sh - host end-to-end check of the ForgeFIRM logging path
# Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
# SPDX-License-Identifier: MIT
#
# Runs a private rsyslogd on the shipped rsyslog.conf (from the
# forgefirm layer) plus rules rendered by `forgectrl --render-syslog`
# from a test settings file, feeds it through the real emitter
# (fflog_e2e_test) and a `logger` relay, and checks that the lines land
# in the right per-logger files, in the ff_line format, filtered by
# level. Needs rsyslogd on the host; run as a user that may write the
# LOGS_ROOT paths (they are absolute), e.g. inside a container or the
# build VM:
#
#   tests/fflog_e2e.sh <build-dir> <path/to/rsyslog.conf>
#
# Not part of CI (rsyslogd availability varies); a manual proof step.
set -e
BUILD="${1:?build dir}"
RSCONF="${2:?shipped rsyslog.conf}"
T=$(mktemp -d)
trap 'kill $RSPID 2>/dev/null; rm -rf "$T"' EXIT

# settings under test: grblhal at debug, forgectrl at warning
cat > "$T/forgefirm.conf" <<EOF
log_grblhal_disk = debug
log_forgectrl_disk = warning
log_gfcloud_disk = off
EOF
FORGECTRL_CONF="$T/forgefirm.conf" "$BUILD/forgectrl" --render-syslog

# private rsyslog: the shipped conf with the socket moved and imklog
# dropped (needs root)
sed -e "s|SysSock.Use=\"on\"|SysSock.Use=\"on\" SysSock.Name=\"$T/log\"|" \
    -e '/imklog/d' "$RSCONF" > "$T/rsyslog.conf"
rsyslogd -f "$T/rsyslog.conf" -i "$T/rsyslogd.pid" -n >"$T/rsyslogd.out" 2>&1 &
RSPID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -S "$T/log" ] && break
    sleep 0.2
done
[ -S "$T/log" ] || { echo "rsyslogd did not open $T/log"; cat "$T/rsyslogd.out"; exit 1; }

FFLOG_CONF="$T/forgefirm.conf" FFLOG_SOCK="$T/log" "$BUILD/fflog_e2e_test" grblhal
FFLOG_CONF="$T/forgefirm.conf" FFLOG_SOCK="$T/log" "$BUILD/fflog_e2e_test" forgectrl
FFLOG_CONF="$T/forgefirm.conf" FFLOG_SOCK="$T/log" "$BUILD/fflog_e2e_test" gfcloud
echo "relay traceback line" | logger -u "$T/log" -t gfhome -p daemon.warning
sleep 2

fail=0
check() { if eval "$1"; then echo "  ok: $2"; else echo "  FAIL: $2"; fail=1; fi; }
G=/data/log/forgefirm/grblhal/grblhal.log
F=/data/log/forgefirm/forgectrl/forgectrl.log
C=/data/log/forgefirm/gfcloud/gfcloud.log
H=/data/log/forgefirm/gfhome/gfhome.log
echo "--- grblhal.log"; cat "$G" | tail -8
check "grep -q ' grblhal\[[0-9]*\] DEBUG e2e debug line$' $G" "grblhal debug line, ff_line format"
check "grep -q ' grblhal\[[0-9]*\] ERR e2e error line$' $G" "grblhal error line"
check "grep -q 'INFO e2e info line with newline$' $G" "trailing newline stripped"
check "grep -q 'e2e long xxxx' $G" "long line delivered"
check "grep -qE '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.]+[+-][0-9:]+ grblhal' $G" "RFC 3339 timestamp"
check "! grep -q 'e2e' $F || ! grep -q 'e2e info line' $F" "forgectrl info line filtered at warning"
check "grep -q 'WARNING e2e warning line' $F" "forgectrl warning line kept"
check "! grep -q 'e2e' $F || ! grep -q 'e2e debug line' $F" "forgectrl debug line filtered"
check "[ ! -s $C ]" "gfcloud off: nothing written"
check "grep -q ' gfhome\[[0-9-]*\] WARNING relay traceback line$' $H" "logger relay routed by tag"
[ $fail -eq 0 ] && echo PASSED || { echo FAILED; exit 1; }
