#!/bin/bash
#
# Runner for the CMake-built Max-Flow binaries.
# Expects binaries at $BIN_DIR/{dinic,push-relabel}.
# Build via:  cmake --build <cmake-build-dir> --target dinic push-relabel

# Usage function
usage() {
    echo "Usage: $0 [OPTIONS]"
    echo "Options:"
    echo "  -g, --graph GRAPH        Specify input graph file (required)"
    echo "  -a, --algorithm ALGO     Algorithm: basic, nondet, or dinic (default: run all)"
    echo "  -t, --threads THREADS    Thread counts (comma-separated, e.g., 1,2,4,8)"
    echo "  -p, --path PATH          Graph path directory (default: /data/graphs/)"
    echo "  -b, --bin-dir DIR        CMake binary dir containing dinic and push-relabel"
    echo "                           (default: ../../build/src/Max-Flow)"
    echo "  -s, --symmetrized        Use symmetrized graph flag"
    echo "  -o, --output FILE        Output TSV file prefix (default: max-flow-results)"
    echo "  --timeout SECONDS        Timeout for each run in seconds (default: 600)"
    echo "  -h, --help               Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 -g twitter_sym.bin -a nondet -t 1,2,4,8"
    echo "  $0 -g asia_sym.bin -a basic -t 4,16,64 -s"
    echo "  $0 -g soc-LiveJournal1_sym.bin -t 1,4,8,16   # Run all algorithms"
}

# Default values
GRAPH=""
ALGORITHM=""          # Empty means run all algorithms
THREAD_COUNTS="1,4,16,64"
GRAPH_PATH="/data/graphs/"
BIN_DIR="../../build/src/Max-Flow"
SYMMETRIZED=""
OUTPUT_PREFIX="max-flow-results"
TIMEOUT="600"

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -g|--graph)        GRAPH="$2"; shift 2 ;;
        -a|--algorithm)    ALGORITHM="$2"; shift 2 ;;
        -t|--threads)      THREAD_COUNTS="$2"; shift 2 ;;
        -p|--path)         GRAPH_PATH="$2"; shift 2 ;;
        -b|--bin-dir)      BIN_DIR="$2"; shift 2 ;;
        -s|--symmetrized)  SYMMETRIZED="-s"; shift ;;
        -o|--output)       OUTPUT_PREFIX="$2"; shift 2 ;;
        --timeout)         TIMEOUT="$2"; shift 2 ;;
        -h|--help)         usage; exit 0 ;;
        *)                 echo "Unknown option: $1"; usage; exit 1 ;;
    esac
done

if [[ -z "$GRAPH" ]]; then
    echo "Error: Graph file must be specified with -g or --graph"
    usage
    exit 1
fi

# Verify binaries exist (built by CMake)
DINIC_BIN="$BIN_DIR/dinic"
PR_BIN="$BIN_DIR/push-relabel"
if [[ ! -x "$DINIC_BIN" || ! -x "$PR_BIN" ]]; then
    echo "Error: binaries not found under $BIN_DIR"
    echo "Build them first:"
    echo "    cmake --build <cmake-build-dir> --target dinic push-relabel"
    exit 1
fi

IFS=',' read -ra THREADS <<< "$THREAD_COUNTS"

# Determine which algorithms to run
VALID_ALGOS=("basic" "nondet" "dinic")
if [[ -z "$ALGORITHM" ]]; then
    ALGORITHMS=("${VALID_ALGOS[@]}")
    echo "No algorithm specified. Running all algorithms: ${ALGORITHMS[*]}"
else
    if [[ ! " ${VALID_ALGOS[*]} " =~ " $ALGORITHM " ]]; then
        echo "Error: Algorithm must be one of: ${VALID_ALGOS[*]}"
        exit 1
    fi
    ALGORITHMS=("$ALGORITHM")
fi

FULL_GRAPH_PATH="${GRAPH_PATH}${GRAPH}"

echo "=== Max Flow Thread Scaling Test ==="
echo "Graph: $FULL_GRAPH_PATH"
echo "Binaries: $BIN_DIR"
echo "Algorithms: ${ALGORITHMS[*]}"
echo "Thread counts: ${THREADS[*]}"
echo "Symmetrized: ${SYMMETRIZED:-"No"}"
echo "Timeout: ${TIMEOUT:-"No timeout"}"
echo "========================================"

get_output_filename() {
    local algo=$1
    case $algo in
        "basic")  echo "Result_push-relabel-basic.tsv" ;;
        "nondet") echo "Result_push-relabel-nondet.tsv" ;;
        "dinic")  echo "Result_dinic.tsv" ;;
    esac
}

if [[ ${#ALGORITHMS[@]} -gt 1 ]]; then
    COMBINED_OUTPUT="Results.tsv"
    echo -e "Algorithm\tGraph\tThreads\tMaxFlow\tTime(s)" > "$COMBINED_OUTPUT"
    echo "Combined results will be saved to: $COMBINED_OUTPUT"
fi

get_taskset_cmd() {
    local threads=$1
    case $threads in
        1)  echo "taskset -c 0-3:4"  ;;
        2)  echo "taskset -c 0-7:4"  ;;
        4)  echo "taskset -c 0-15:4" ;;
        8)  echo "taskset -c 0-31:4" ;;
        16) echo "taskset -c 0-63:4" ;;
        24) echo "taskset -c 0-95:4" ;;
        48) echo "taskset -c 0-95:2" ;;
        96) echo "taskset -c 0-95"   ;;
        *)  echo "" ;;
    esac
}

for current_algo in "${ALGORITHMS[@]}"; do
    echo ""
    echo "=========================================="
    echo "Running algorithm: $current_algo"
    echo "=========================================="

    OUTPUT_FILE=$(get_output_filename "$current_algo")
    echo -e "Graph\tThreads\tMaxFlow\tTime(s)" > "$OUTPUT_FILE"
    echo "Results for $current_algo will be saved to: $OUTPUT_FILE"

    for thread_count in "${THREADS[@]}"; do
        echo ""
        echo "Testing $current_algo with $thread_count threads..."

        # Drop caches if running as root
        if [[ $EUID -eq 0 ]]; then
            echo "Dropping caches..."
            sync && echo 3 > /proc/sys/vm/drop_caches
        fi

        export PARLAY_NUM_THREADS=$thread_count

        taskset_cmd=$(get_taskset_cmd $thread_count)

        if [[ "$current_algo" == "dinic" ]]; then
            cmd_base="numactl -i all $DINIC_BIN -i $FULL_GRAPH_PATH $SYMMETRIZED"
        else
            cmd_base="numactl -i all $PR_BIN -i $FULL_GRAPH_PATH $SYMMETRIZED -a $current_algo"
        fi

        if [[ -z "$taskset_cmd" ]]; then
            cmd="$cmd_base"
        else
            cmd="$taskset_cmd $cmd_base"
        fi

        echo "Running: $cmd"
        if [[ -n "$TIMEOUT" ]]; then
            echo "Timeout: $TIMEOUT seconds"
        else
            echo "Timeout: No timeout"
        fi

        # Run
        if [[ -n "$TIMEOUT" ]]; then
            output=$(timeout "${TIMEOUT}s" $cmd 2>&1)
            exit_code=$?
        else
            output=$($cmd 2>&1)
            exit_code=$?
        fi

        if [[ $exit_code -eq 124 && -n "$TIMEOUT" ]]; then
            echo "Warning: Command timed out after $TIMEOUT seconds"
            output="TIMEOUT: Command exceeded $TIMEOUT seconds"
        fi

        # Extract results
        if [[ $exit_code -eq 124 && -n "$TIMEOUT" ]]; then
            avg_time="TIMEOUT"
            max_flow="TIMEOUT"
        else
            avg_time=$(echo "$output" | grep "Average time:" | tail -1 | awk '{print $3}')
            max_flow=$(echo "$output" | grep "Max flow:" | tail -1 | awk '{print $3}')
        fi

        if [[ "$avg_time" == "TIMEOUT" ]]; then
            echo -e "${GRAPH}\t${thread_count}\tTIMEOUT\tTIMEOUT" >> "$OUTPUT_FILE"
            echo "Results: TIMEOUT (exceeded ${TIMEOUT} seconds)"
            if [[ ${#ALGORITHMS[@]} -gt 1 ]]; then
                echo -e "${current_algo}\t${GRAPH}\t${thread_count}\tTIMEOUT\tTIMEOUT" >> "$COMBINED_OUTPUT"
            fi
        elif [[ -n "$avg_time" && -n "$max_flow" ]]; then
            echo -e "${GRAPH}\t${thread_count}\t${max_flow}\t${avg_time}" >> "$OUTPUT_FILE"
            echo "Results: Max Flow = $max_flow, Average Time = $avg_time seconds"
            if [[ ${#ALGORITHMS[@]} -gt 1 ]]; then
                echo -e "${current_algo}\t${GRAPH}\t${thread_count}\t${max_flow}\t${avg_time}" >> "$COMBINED_OUTPUT"
            fi
        else
            echo "Warning: Could not extract results from output"
            echo -e "${GRAPH}\t${thread_count}\tERROR\tERROR" >> "$OUTPUT_FILE"
            if [[ ${#ALGORITHMS[@]} -gt 1 ]]; then
                echo -e "${current_algo}\t${GRAPH}\t${thread_count}\tERROR\tERROR" >> "$COMBINED_OUTPUT"
            fi
        fi

        echo "Completed $current_algo with $thread_count threads"
    done

    echo "Completed all tests for algorithm: $current_algo"
    echo "Individual results saved to: $OUTPUT_FILE"
done

echo ""
echo "=========================================="
echo "All tests completed!"
echo "=========================================="

if [[ ${#ALGORITHMS[@]} -gt 1 ]]; then
    echo "Combined results saved to: $COMBINED_OUTPUT"
    echo "Individual results saved to:"
    for algo in "${ALGORITHMS[@]}"; do
        echo "  - $(get_output_filename "$algo")"
    done
else
    echo "Results saved to: $(get_output_filename "${ALGORITHMS[0]}")"
fi
