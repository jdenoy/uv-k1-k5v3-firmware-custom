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

"$CC" -O2 -Wall -Wextra -Werror -o "$TMP/host" "$HERE/host_pocsag.c" "$HERE/../pocsag.c" || exit 1

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
run "combined worst case"        1200 "--msg=$A" --cnr 14 --foff 2000 --invert --clock-ppm 300 --hpf 100 --audio-lpf 3500 -- "${REF_A[@]}"
for seed in 1 2 3 4; do
run "CNR 11 dB, seed $seed"      1200 "--msg=$A" --cnr 11 --seed $seed --         "${REF_A[@]}"
done

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
