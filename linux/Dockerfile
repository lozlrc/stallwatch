# Linux build and test environment for stallwatch, including the
# non-cooperative capture path (libunwind-ptrace). Used by run_linux_tests.sh;
# the container needs --cap-add=SYS_PTRACE for the remote tests.
FROM debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ make libunwind-dev binutils \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
