#!/bin/sh
# perl5-mt: runs benchmark drivers and prints medians or a full
# physical-core sweep table.
#
# Single-point mode (backward-compatible):
#   tools/bench/run.sh <perl> <driver.pl> <workers> <items> [reps] [extra-lib]
#
# Sweep mode (physical-core sweep with preloaded image, fibers, and RSS/worker):
#   tools/bench/run.sh --sweep <mt-perl> [stock-perl] [items] [image-mb] [reps]

HERE=$(cd "$(dirname "$0")" && pwd)

detect_phys_cores() {
    if [ "$(uname -s)" = "Darwin" ]; then
        sysctl -n hw.physicalcpu 2>/dev/null || echo 4
    elif command -v lscpu >/dev/null 2>&1; then
        lscpu -p=CORE,SOCKET 2>/dev/null | grep -v '^#' | sort -u | wc -l | tr -d ' '
    elif command -v nproc >/dev/null 2>&1; then
        nproc
    else
        echo 4
    fi
}

run_one() {
    PERL=$1; DRIVER=$2; N=$3; ITEMS=$4; REPS=${5:-3}; LIB=$6
    TMP=$(mktemp)
    i=0
    while [ "$i" -lt "$REPS" ]; do
        "$PERL" "$DRIVER" "$N" "$ITEMS" "$LIB" >> "$TMP" 2>>"$TMP.err" || {
            echo "run failed:" >&2; cat "$TMP.err" >&2; rm -f "$TMP" "$TMP.err"; exit 1; }
        i=$((i + 1))
    done
    awk '
    function median(a, n,   i, j, t) {
        for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (a[j] < a[i]) { t = a[i]; a[i] = a[j]; a[j] = t }
        return (n % 2) ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2
    }
    /ops_per_s=/ {
        split($0, f, " ");
        for (k in f) {
            split(f[k], kv, "=");
            if (kv[1] == "driver") drv = kv[2];
            if (kv[1] == "json_mode") jm = kv[2];
            if (kv[1] == "image_mb") imb = kv[2];
            if (kv[1] == "workers") w = kv[2];
            if (kv[1] == "fibers") fib = kv[2];
            if (kv[1] == "items") it = kv[2];
            if (kv[1] == "ops_per_s") ops[++no] = kv[2];
            if (kv[1] == "rss_kb") rss[++nr] = kv[2];
            if (kv[1] == "rss_per_worker_kb") rpw[++np] = kv[2];
            if (kv[1] == "startup_ms") st[++ns] = kv[2];
        }
    }
    END {
        printf "driver=%s json_mode=%s image_mb=%s workers=%s fibers=%s items=%s ops_per_s=%.1f rss_kb=%d rss_per_worker_kb=%d startup_ms=%.2f\n",
            drv, jm, imb, w, fib, it, median(ops, no), median(rss, nr), median(rpw, np), median(st, ns);
    }' "$TMP"
    rm -f "$TMP" "$TMP.err"
}

if [ "$1" = "--sweep" ]; then
    MT_PERL=$2
    STOCK_PERL=${3:-$MT_PERL}
    ITEMS=${4:-1000}
    IMAGE_MB=${5:-256}
    REPS=${6:-3}
    CORES=$(detect_phys_cores)
    HOST=$(uname -n)
    ARCH=$(uname -m)
    OS=$(uname -s)

    echo "# perl5-mt Benchmark Sweep"
    echo "# host=$HOST os=$OS arch=$ARCH physical_cores=$CORES image_mb=$IMAGE_MB items=$ITEMS reps=$REPS"
    echo ""
    echo "| Driver | JSON Mode | Workers | Fibers | ops/s | Total RSS (KiB) | RSS/worker (KiB) | Startup (ms) |"
    echo "|---|---|---:|---:|---:|---:|---:|---:|"

    W=1
    while [ "$W" -le "$CORES" ]; do
        for spec in \
            "mt:$MT_PERL:$HERE/mt.pl:1:auto" \
            "mt-fibers:$MT_PERL:$HERE/mt.pl:4:auto" \
            "mt-comp:$MT_PERL:$HERE/mt.pl:1:compartment" \
            "prefork:$STOCK_PERL:$HERE/prefork.pl:1:auto" \
            "ithreads:$STOCK_PERL:$HERE/ithreads.pl:1:auto"
        do
            IFS=: read -r label pbin drv fib jmode <<EOF
$spec
EOF
            LINE=$(PERL_BENCH_IMAGE_MB="$IMAGE_MB" PERL_BENCH_FIBERS="$fib" PERL_BENCH_JSON_MODE="$jmode" \
                   run_one "$pbin" "$drv" "$W" "$ITEMS" "$REPS" "")
            echo "$LINE" | awk -v lbl="$label" '{
                for (i = 1; i <= NF; i++) { split($i, kv, "="); m[kv[1]] = kv[2]; }
                printf "| %s | %s | %s | %s | %s | %s | %s | %s |\n",
                    lbl, m["json_mode"], m["workers"], m["fibers"],
                    m["ops_per_s"], m["rss_kb"], m["rss_per_worker_kb"], m["startup_ms"];
            }'
        done
        if [ "$W" -eq "$CORES" ]; then
            break
        fi
        NEXT=$((W * 2))
        if [ "$NEXT" -gt "$CORES" ]; then
            W=$CORES
        else
            W=$NEXT
        fi
    done
    exit 0
fi

run_one "$1" "$2" "$3" "$4" "$5" "$6"
