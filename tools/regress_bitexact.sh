#!/usr/bin/env bash
# tools/regress_bitexact.sh
# NEGGIA-001 AT-6: bit-exact regression of post-patch plugin against
# a pre-patch baseline. For each fixture master file under the given
# fixture directories, call plugin_get_data for every available frame
# via both .so files and assert byte-equality.
#
# Usage:
#   tools/regress_bitexact.sh <baseline_so> <candidate_so> <fixture_dir1> [<fixture_dir2> ...]
#
# Where each <fixture_dir> contains one or more subdirectories with a
# master *.h5 file (e.g. src/dectris/neggia/test/h5-testfiles/datasets_eiger1).
#
# Exit 0 on full success; exit 1 on first mismatch or plugin error.

set -euo pipefail

if [ "$#" -lt 3 ]; then
  echo "usage: $0 <baseline_so> <candidate_so> <fixture_dir1> [<fixture_dir2> ...]" >&2
  exit 2
fi

BASELINE_SO="$1"; shift
CANDIDATE_SO="$1"; shift
[ -f "$BASELINE_SO" ] || { echo "FATAL: baseline .so not found: $BASELINE_SO" >&2; exit 2; }
[ -f "$CANDIDATE_SO" ] || { echo "FATAL: candidate .so not found: $CANDIDATE_SO" >&2; exit 2; }

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

# Inline C++ helper: load a .so, open a master.h5, dump each frame to stdout.
# Output format per-frame: 8-byte size, then raw int32 pixels.
RUNNER_SRC="$WORKDIR/runner.cpp"
RUNNER_BIN="$WORKDIR/runner"
cat > "$RUNNER_SRC" <<'EOF'
#include <dlfcn.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

typedef void (*plugin_open_t)(const char*, int[1024], int*);
typedef void (*plugin_get_header_t)(int*, int*, int*, float*, float*, int*,
                                    int[1024], int*);
typedef void (*plugin_get_data_t)(int*, int*, int*, int*, int[1024], int*);
typedef void (*plugin_close_t)(int*);

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <so> <master.h5>\n", argv[0]);
        return 2;
    }
    void* h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 2; }
    auto open_f = (plugin_open_t)dlsym(h, "plugin_open");
    auto hdr_f = (plugin_get_header_t)dlsym(h, "plugin_get_header");
    auto data_f = (plugin_get_data_t)dlsym(h, "plugin_get_data");
    auto close_f = (plugin_close_t)dlsym(h, "plugin_close");
    if (!open_f || !hdr_f || !data_f || !close_f) {
        fprintf(stderr, "dlsym failed\n"); return 2;
    }
    int info[1024] = {0};
    int err = 1;
    open_f(argv[2], info, &err);
    if (err != 0) { fprintf(stderr, "plugin_open err=%d\n", err); return 3; }
    int nx, ny, nbytes, total;
    float qx, qy;
    hdr_f(&nx, &ny, &nbytes, &qx, &qy, &total, info, &err);
    if (err != 0) { fprintf(stderr, "plugin_get_header err=%d\n", err); return 3; }
    fprintf(stderr, "runner: nx=%d ny=%d total_frames=%d\n", nx, ny, total);
    int* buf = (int*)malloc((size_t)nx * (size_t)ny * sizeof(int));
    if (!buf) { fprintf(stderr, "OOM\n"); return 3; }
    for (int f = 1; f <= total; ++f) {
        data_f(&f, &nx, &ny, buf, info, &err);
        if (err != 0) {
            fprintf(stderr, "plugin_get_data(frame=%d) err=%d\n", f, err);
            free(buf); return 4;
        }
        size_t bytes = (size_t)nx * (size_t)ny * sizeof(int);
        fwrite(&bytes, sizeof(size_t), 1, stdout);
        fwrite(buf, 1, bytes, stdout);
    }
    free(buf);
    close_f(&err);
    dlclose(h);
    return 0;
}
EOF

# Compile the helper once.
CXX="${CXX:-c++}"
"$CXX" -std=c++11 -O2 -o "$RUNNER_BIN" "$RUNNER_SRC" -ldl

discovered_fixtures=0
mismatches=0
errors=0

for fixture_dir in "$@"; do
  if [ ! -d "$fixture_dir" ]; then
    echo "WARN: fixture dir not found, skipping: $fixture_dir" >&2
    continue
  fi
  # Each dataset subdir under fixture_dir is expected to contain a master_*.h5
  # (or similar; we look for *master*.h5).
  while IFS= read -r master; do
    discovered_fixtures=$((discovered_fixtures + 1))
    echo "=== fixture: $master ===" >&2
    BASELINE_OUT="$WORKDIR/baseline.bin"
    CANDIDATE_OUT="$WORKDIR/candidate.bin"
    if ! "$RUNNER_BIN" "$BASELINE_SO" "$master" > "$BASELINE_OUT" 2>"$WORKDIR/baseline.log"; then
      echo "ERROR: baseline runner failed on $master" >&2
      cat "$WORKDIR/baseline.log" >&2
      errors=$((errors + 1)); continue
    fi
    if ! "$RUNNER_BIN" "$CANDIDATE_SO" "$master" > "$CANDIDATE_OUT" 2>"$WORKDIR/candidate.log"; then
      echo "ERROR: candidate runner failed on $master" >&2
      cat "$WORKDIR/candidate.log" >&2
      errors=$((errors + 1)); continue
    fi
    if cmp -s "$BASELINE_OUT" "$CANDIDATE_OUT"; then
      echo "OK: $master byte-identical" >&2
    else
      echo "MISMATCH: $master" >&2
      mismatches=$((mismatches + 1))
    fi
  done < <(find "$fixture_dir" -type f -name '*master*.h5')
done

echo "=== summary: $discovered_fixtures fixture(s) checked; $mismatches mismatch(es); $errors error(s) ===" >&2

if [ "$discovered_fixtures" -eq 0 ]; then
  echo "FATAL: no fixtures discovered under: $*" >&2
  exit 2
fi
if [ "$errors" -gt 0 ] || [ "$mismatches" -gt 0 ]; then
  exit 1
fi
echo "PASS: all fixtures byte-identical across baseline + candidate"
exit 0
