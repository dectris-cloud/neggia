#!/usr/bin/env bash
# tools/bench_frames.sh — NEGGIA-002: benchmark harness for the neggia plugin.
# Times plugin_open / plugin_get_header / serial ascending frame loop (XDS's
# exact call pattern; spec invariant 3) via a dlopen runner pattern-copied
# from tools/regress_bitexact.sh. CSV to stdout, diagnostics to stderr
# (spec invariant: channel separation). CLOCK_MONOTONIC, ns internal, ms out.
#
# Usage:
#   bench_frames.sh SO MASTER.h5                 single serial run
#   bench_frames.sh --ab SO_A SO_B MASTER.h5     3-rep A/B with ratio table
#   bench_frames.sh --procs P SO MASTER.h5       P forked runners, disjoint ranges
# Exit: 0 ok | 2 usage/missing file/no readable frames | 1 runner failure
set -euo pipefail

MODE=single; PROCS=1
case "${1:-}" in
  --ab)    MODE=ab; shift;;
  --procs) MODE=procs; PROCS="${2:?}"; shift 2;;
esac
if { [ "$MODE" = ab ] && [ $# -ne 3 ]; } || { [ "$MODE" != ab ] && [ $# -ne 2 ]; }; then
  echo "usage: $0 [--ab SO_B|--procs P] SO MASTER.h5" >&2; exit 2
fi
SO_A="$1"; [ "$MODE" = ab ] && { SO_B="$2"; MASTER="$3"; } || MASTER="${2}"
for f in "$SO_A" ${SO_B:+"$SO_B"} "$MASTER"; do
  [ -f "$f" ] || { echo "missing: $f" >&2; exit 2; }
done

WORKDIR="$(mktemp -d)"; trap 'rm -rf "$WORKDIR"' EXIT
cat > "$WORKDIR/runner.cpp" <<'EOF'
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dlfcn.h>
#include <vector>
typedef void (*open_t)(const char*, int[1024], int*);
typedef void (*hdr_t)(int*, int*, int*, float*, float*, int*, int[1024], int*);
typedef void (*data_t)(int*, int*, int*, int*, int[1024], int*);
typedef void (*close_t)(int*);
static double now_ms() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
int main(int argc, char** argv) {
    if (argc < 3) return 2;                     // so master [probe|start end]
    void* h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    open_t open_f = (open_t)dlsym(h, "plugin_open");
    hdr_t hdr_f = (hdr_t)dlsym(h, "plugin_get_header");
    data_t data_f = (data_t)dlsym(h, "plugin_get_data");
    close_t close_f = (close_t)dlsym(h, "plugin_close");
    if (!open_f || !hdr_f || !data_f || !close_f) return 2;
    int info[1024] = {0}; int err = 1;          // pre-set nonzero: silent plugin fails loudly
    double t0 = now_ms();
    open_f(argv[2], info, &err);
    double open_ms = now_ms() - t0;
    if (err != 0) { fprintf(stderr, "plugin_open err=%d\n", err); return 3; }
    int nx, ny, nbytes, total; float qx, qy; err = 1;
    t0 = now_ms();
    hdr_f(&nx, &ny, &nbytes, &qx, &qy, &total, info, &err);
    double hdr_ms = now_ms() - t0;
    if (err != 0 || total <= 0) { fprintf(stderr, "header err=%d total=%d\n", err, total); return err ? 3 : 5; }
    // the plugin banners on stdout during open — sentinel-prefix our own output
    if (argc == 4) { printf("BENCH:%d\n", total); close_f(&err); return 0; }   // probe mode
    int start = 1, end = total;
    if (argc == 5) { start = atoi(argv[3]); end = atoi(argv[4]); }
    if (start < 1 || end > total || end < start) return 5;
    std::vector<int> buf((size_t)nx * (size_t)ny);
    std::vector<double> lat; lat.reserve((size_t)(end - start + 1));
    double loop0 = now_ms();
    for (int f = start; f <= end; ++f) {
        err = 1; t0 = now_ms();
        data_f(&f, &nx, &ny, buf.data(), info, &err);
        lat.push_back(now_ms() - t0);
        if (err != 0) { fprintf(stderr, "get_data(%d) err=%d\n", f, err); return 4; }
    }
    double loop_ms = now_ms() - loop0;
    std::sort(lat.begin(), lat.end());
    size_t n = lat.size();
    printf("BENCH:%.3f,%.3f,%zu,%.3f,%.4f,%.4f,%.4f,%.4f\n", open_ms, hdr_ms, n,
           loop_ms, lat[0], lat[n / 2], lat[(size_t)((double)(n - 1) * 0.95)], lat[n - 1]);
    close_f(&err); dlclose(h); return 0;
}
EOF
CXX="${CXX:-c++}"
"$CXX" -std=c++11 -O2 -o "$WORKDIR/runner" "$WORKDIR/runner.cpp" -ldl

hdr='so,fixture,proc,rep,open_ms,header_ms,frames,loop_ms,min_ms,p50_ms,p95_ms,max_ms'
run_one() { # so rep start end proc label  -> CSV row on stdout
  local row
  row="$("$WORKDIR/runner" "$1" "$MASTER" ${3:+$3 $4} | grep '^BENCH:')" || return $?
  echo "${6:-$(basename "$1")},$(basename "$MASTER"),${5:-0},$2,${row#BENCH:}"
}

case "$MODE" in
  single)
    echo "$hdr"; run_one "$SO_A" 1 ;;
  ab)
    run_one "$SO_A" 0 > /dev/null            # discarded warmup (page-cache first touch)
    echo "$hdr"
    for rep in 1 2 3; do run_one "$SO_A" "$rep" "" "" "" A; run_one "$SO_B" "$rep" "" "" "" B; done |
      tee "$WORKDIR/ab.csv"
    awk -F, '{ if ($1=="A") { oa+=$5; ha+=$6; la+=$8; na++ }
        else { ob+=$5; hb+=$6; lb+=$8; nb++ } } END {
        if (!na || !nb) exit 1
        printf "ratio(B/A): open=%.3f header=%.3f loop=%.3f\n", (ob/nb)/(oa/na), (hb/nb)/(ha/na), (lb/nb)/(la/na) > "/dev/stderr" }' "$WORKDIR/ab.csv" ;;
  procs)
    total="$("$WORKDIR/runner" "$SO_A" "$MASTER" probe | grep '^BENCH:')"; total="${total#BENCH:}"
    [ "$total" -ge "$PROCS" ] || { echo "frames($total) < procs($PROCS)" >&2; exit 2; }
    echo "$hdr"
    t0="$(perl -MTime::HiRes=time -e 'printf "%.0f", time()*1000')"
    pids=(); chunk=$(( (total + PROCS - 1) / PROCS ))
    for ((p=0; p<PROCS; p++)); do
      s=$((p*chunk+1)); e=$(( (p+1)*chunk < total ? (p+1)*chunk : total ))
      [ "$s" -le "$e" ] && run_one "$SO_A" 1 "$s" "$e" "$p" & pids+=($!)
    done
    rc=0; for pid in "${pids[@]}"; do wait "$pid" || rc=1; done
    t1="$(perl -MTime::HiRes=time -e 'printf "%.0f", time()*1000')"
    echo "aggregate_wall_ms=$((t1-t0)) procs=$PROCS frames=$total" >&2
    exit "$rc" ;;
esac
