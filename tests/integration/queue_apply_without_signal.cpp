// Test-only crash boundary: the real durable submission succeeds, but the
// client exits before sending SIGHUP. No production command exposes this.
#include "apply_state.h"
#include "config_input_lock.h"
#include <cstdio>
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    ConfigInputLock inputs(argv[1]);
    std::string revision, error;
    if (!inputs.ok() || !apply_state::queue(argv[1], revision, error)) return 1;
    puts(revision.c_str()); return 0;
}
