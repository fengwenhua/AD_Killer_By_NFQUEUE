#pragma once
#if !defined(__APPLE__)
#error "The prctl audit stub is only for macOS; use the system header on Linux."
#endif
// Only allows engine.cpp to compile for isolated macOS audit probes. The real
// run_daemon entry point is not called by the engine probe.
#define PR_SET_NAME 15
inline int prctl(int, ...) { return 0; }
