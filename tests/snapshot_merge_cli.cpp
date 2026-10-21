#include "snapshot_merge.h"
int main(int argc, char **argv) {
    return argc == 3 ? merge_rule_snapshot(argv[1], argv[2]) : 1;
}
