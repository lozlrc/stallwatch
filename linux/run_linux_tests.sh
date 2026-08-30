#!/bin/sh
# Build and test stallwatch on Linux in a container, including the remote
# (ptrace + libunwind) path, and refresh bench/results_linux.txt from the run.
# Runs the suite under both g++ and clang++, since the remote unwinder depends
# on the unwind info the compiler emits. The source tree is mounted read-only
# and copied inside the container, so host build artifacts are never touched.
# Requires docker (or a compatible engine) with --cap-add=SYS_PTRACE allowed.
set -e
cd "$(dirname "$0")/.."

docker build -q -t stallwatch-linux -f linux/Dockerfile linux
docker run --rm --cap-add=SYS_PTRACE \
  -v "$PWD":/src:ro -v "$PWD/bench":/out \
  stallwatch-linux sh -c '
    set -e
    cp -r /src /tmp/b && cd /tmp/b
    for cc in g++ clang++; do
      echo "=== $cc ==="
      rm -rf bin
      $cc --version | head -1
      make -s test CXX=$cc
    done
    rm -rf bin && make -s all CXX=g++
    {
      echo "# stallwatch Linux results"
      echo "# $(uname -sm), $(g++ --version | head -1), container via linux/run_linux_tests.sh"
      echo
      ./bin/bench_beat
      echo
      bench/bench_remote.sh
    } | tee /out/results_linux.txt
  '
echo "linux tests: PASS (bench/results_linux.txt updated)"
