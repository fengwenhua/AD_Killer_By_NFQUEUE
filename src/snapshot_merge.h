#pragma once
#include <string>

// A non-root, offline diagnostic. Writes only OUTPUT; no DATA_DIR access,
// networking, publication, configuration changes or daemon signalling.
int merge_rule_snapshot(const std::string &input, const std::string &output);
