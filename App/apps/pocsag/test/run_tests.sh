#!/usr/bin/env bash
# Host tests for the POCSAG decoder: synthesize PA4-like ADC audio with
# genpocsag.py for a set of channel conditions, decode with host_pocsag, check
# the decoded messages. Needs python3 + numpy + scipy and a C compiler.
# multimon-ng, when installed, cross-checks the generator (reference decoder).
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CC="${CC:-clang}"

"$CC" -O2 -Wall -Wextra -Werror -DPOC_STATS -o "$TMP/host" "$HERE/host_pocsag.c" "$HERE/../pocsag.c" || exit 1

A="1234567:3:A:ADRASEC 27 exercice SATER 14h00 Evreux"
N="200:0:N:0612345678"
REF_A=("RIC 1234567 F3" "alpha: [ADRASEC 27 exercice SATER 14h00 Evreux]")
REF_N=("RIC 0000200 F0" "num  : [0612345678]")

pass=0; fail=0
run() {   # run <name> <rate> [corner=HZ] <gen args...> -- <expected...>
    local name="$1" rate="$2" corner=60; shift 2
    [[ "$1" == corner=* ]] && { corner="${1#corner=}"; shift; }
    local args=() out
    while [ "$1" != "--" ]; do args+=("$1"); shift; done
    shift
    python3 "$HERE/genpocsag.py" --rate "$rate" --out "$TMP/s.u16" "${args[@]}"
    out="$("$TMP/host" "$rate" "$TMP/s.u16" "$corner")"
    for want in "$@"; do
        if ! grep -qF -- "$want" <<<"$out"; then
            printf '  ❌ %-30s missing: %s\n' "$name" "$want"
            fail=$((fail + 1)); return
        fi
    done
    printf '  ✅ %s\n' "$name"; pass=$((pass + 1))
}

echo "POCSAG host tests"
for r in 512 1200 2400; do
run "clean $r bps"               $r "--msg=$A" "--msg=$N" --                        "${REF_A[@]}" "${REF_N[@]}" "messages  : 2"
run "CNR 12 dB $r bps"           $r "--msg=$A" "--msg=$N" --cnr 12 --               "${REF_A[@]}" "${REF_N[@]}"
run "inverted chain $r bps"      $r "--msg=$A" --invert --                        "${REF_A[@]}" "inverted 1"
run "AC coupling 100 Hz $r bps"  $r "--msg=$A" --hpf 100 --                       "${REF_A[@]}"
run "clock +500 ppm $r bps"      $r "--msg=$A" --clock-ppm 500 --                 "${REF_A[@]}"
run "clock -500 ppm $r bps"      $r "--msg=$A" --clock-ppm -500 --                "${REF_A[@]}"
done
run "offset +3 kHz"              1200 "--msg=$A" --foff 3000 --                   "${REF_A[@]}"
run "offset -3 kHz"              1200 "--msg=$A" --foff -3000 --                  "${REF_A[@]}"
run "audio LPF 3 kHz"            1200 "--msg=$A" --audio-lpf 3000 --              "${REF_A[@]}"
run "AC coupling 150 Hz"         1200 "--msg=$A" --hpf 150 --                     "${REF_A[@]}"
run "half level (gain /2)"       1200 "--msg=$A" --gain 0.032 --                  "${REF_A[@]}"
run "1 bit error corrected"      1200 "--msg=$A" --flip 520 --                    "${REF_A[@]}" "bits 280 fixed"
run "2 bit errors, same word"    1200 "--msg=$A" --flip 520 --flip 525 --         "RIC 1234567 F3 bits 280 BAD"
run "tone only"                  1200 "--msg=4711:1:T:" --                        "RIC 0004711 F1 bits 0" "messages  : 1"
run "3 transmissions"            1200 "--msg=$A" --tx 3 --                        "${REF_A[@]}" "messages  : 3"
run "3 messages in one batch"    1200 "--msg=8:0:N:112" "--msg=17:2:A:Test" "--msg=$N" -- \
                                 "RIC 0000008 F0" "num  : [112]" "RIC 0000017 F2" "alpha: [Test]" "${REF_N[@]}"
run "long message (truncated)"   1200 "--msg=99:3:A:$(printf 'x%.0s' {1..100})" -- "RIC 0000099 F3 bits 560 trunc"
run "no AC coupling, corner off" 1200 corner=0 "--msg=$A" --hpf 0 --             "${REF_A[@]}"
for r in 512 1200 2400; do
run "AC 250 Hz, corner 250 $r bps" $r corner=250 "--msg=$A" --hpf 250 --cnr 12 -- "${REF_A[@]}"
done
# The K1 audio path is close to a differentiator (about 1 kHz high-pass, measured
# on a real recording): the app default corner is 1000 Hz.
for r in 512 1200 2400; do
run "K1-like: AC 1 kHz, corner 1000 $r bps" $r corner=1000 "--msg=$A" --hpf 1000 --cnr 12 -- "${REF_A[@]}"
run "K1-like: AC 1.5 kHz, corner 1000 $r bps" $r corner=1000 "--msg=$A" --hpf 1500 --cnr 15 -- "${REF_A[@]}"
done
run "80 chars, K1-like, 1200 bps"  1200 corner=1000 "--msg=1234:3:A:this is a very very long text string to test text wraping. do this even work ?" --hpf 1200 --cnr 20 -- \
                                 "RIC 0001234 F3" "alpha: [this is a very very long text string to test text wraping. do this even work ?]"
# The app pauses sampling for keys and display when the decoder is not busy:
# POC_GAP emulates it (600 samples = 62 ms per pause). v1.2 paused during the
# preamble too and lost 512 bps pages on the K1.
for r in 512 1200 2400; do
    python3 "$HERE/genpocsag.py" --rate "$r" --msg "1234:3:A:test" --hpf 1000 --cnr 20 --tx 4 --gap-ms 0 --out "$TMP/g.u16"
    out="$(POC_GAP=600 "$TMP/host" "$r" "$TMP/g.u16" 2)"
    if grep -qF "messages  : 4" <<<"$out"; then echo "  ✅ UI pauses, 4 pages back to back, $r bps"; pass=$((pass + 1))
    else echo "  ❌ UI pauses, 4 pages back to back, $r bps"; echo "$out" | tail -2; fail=$((fail + 1)); fi
done
# Edge latch (each bit edge is a pulse; the K1 needs it at 512 bps) and auto.
for r in 512 1200 2400; do
run "edge latch, AC 1 kHz, CNR 15, $r bps" $r corner=1 "--msg=$A" --hpf 1000 --cnr 15 -- "${REF_A[@]}"
run "auto, AC 1 kHz, CNR 12, $r bps"       $r corner=2 "--msg=$A" --hpf 1000 --cnr 12 -- "${REF_A[@]}"
done
run "combined worst case"        1200 "--msg=$A" --cnr 14 --foff 2000 --invert --clock-ppm 300 --hpf 100 --audio-lpf 3500 -- "${REF_A[@]}"
for seed in 1 2 3 4; do
run "CNR 11 dB, seed $seed"      1200 "--msg=$A" --cnr 11 --seed $seed --         "${REF_A[@]}"
done

# Real captures from the UV-K1 (POCSAG Rec v1.0, rpitx "1234:test",
# 2026-10-01). 1200 bps: must decode clean (it holds a sync).
for c in 1000 1500 2; do      # 2 = auto (1000 Hz at 1200 bps)
    out="$("$TMP/host" 1200 "$HERE/k1/k1_1234_test_1200.u16" "$c")"
    if grep -qF "RIC 0001234 F3 bits 40" <<<"$out" && grep -qF "alpha: [test]" <<<"$out" \
       && grep -qF "fixed 0 bad 0" <<<"$out"; then
        echo "  ✅ real K1 capture, corner $c"; pass=$((pass + 1))
    else
        echo "  ❌ real K1 capture, corner $c"; echo "$out"; fail=$((fail + 1))
    fi
done

# 512 bps: the windows hold no sync word, so the decoder's bits are compared
# with the bits rebuilt from the pulses (ground truth, checked against the
# expected address and message words).
out="$(python3 "$HERE/k1_bits512.py" "$HERE/.." 2>&1)"
if grep -qF "OK" <<<"$out"; then echo "  ✅ real K1 512 bps captures, edge latch: $out"; pass=$((pass + 1))
else echo "  ❌ real K1 512 bps captures: $out"; fail=$((fail + 1)); fi

if command -v multimon-ng >/dev/null; then
    python3 "$HERE/genpocsag.py" --msg "$A" --msg "$N" --out "$TMP/s.u16" --wav22k "$TMP/s.raw"
    ref="$(multimon-ng -q -t raw -a POCSAG1200 "$TMP/s.raw" 2>&1)"
    if grep -qF "Address: 1234567  Function: 3  Alpha:   ADRASEC 27 exercice SATER 14h00 Evreux" <<<"$ref" \
       && grep -qF "Numeric: 0612345678" <<<"$ref"; then
        echo "  ✅ generator cross-check with multimon-ng"; pass=$((pass + 1))
    else
        echo "  ❌ generator cross-check with multimon-ng"; echo "$ref"; fail=$((fail + 1))
    fi
fi

echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
