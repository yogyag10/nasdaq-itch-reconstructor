#!/usr/bin/env bash
# Print the benchmark environment (CPU, cores, compiler, flags, kernel, governor).
cd "$(dirname "$0")/.."
echo "date: $(date -u +%FT%TZ)"
echo "uname: $(uname -srm)"
if [[ "$(uname)" == "Darwin" ]]; then
  echo "cpu: $(sysctl -n machdep.cpu.brand_string)"
  echo "cores: $(sysctl -n hw.ncpu) logical, $(sysctl -n hw.physicalcpu) physical"
  echo "memory_bytes: $(sysctl -n hw.memsize)"
  echo "governor: n/a (macOS has no user-visible cpufreq governor; cannot pin to a core)"
else
  echo "cpu: $(lscpu | awk -F: '/Model name/ {gsub(/^ +/,"",$2); print $2}')"
  echo "cores: $(nproc) logical"
  echo "memory: $(awk '/MemTotal/ {print $2" kB"}' /proc/meminfo)"
  G=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
  echo "governor: $( [[ -r $G ]] && cat $G || echo unavailable)"
fi
CXX_BIN="${CXX:-c++}"
echo "compiler: $($CXX_BIN --version | head -1)"
if [[ -f build-rel/CMakeCache.txt ]]; then
  echo "build_type: $(grep CMAKE_BUILD_TYPE: build-rel/CMakeCache.txt | cut -d= -f2)"
  echo "flags_release: $(grep CMAKE_CXX_FLAGS_RELEASE: build-rel/CMakeCache.txt | cut -d= -f2)"
fi
echo "git: $(git rev-parse --short HEAD 2>/dev/null || echo uncommitted)"
