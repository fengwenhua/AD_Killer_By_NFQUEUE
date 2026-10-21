#pragma once
#include "source_validator.h"
#include "builtin_sources.h"
#include <functional>

using RuleSourceDownload = std::function<std::string(const std::string &, const std::string &)>;
SourceValidationResult merge_hagezi_pair(const std::string &pro_file,
                                        const std::string &fake_file,
                                        const RuleStreamLimits &limits = {});
SourceValidationResult fetch_hagezi_source(const std::string &directory,
                                          const RuleStreamLimits &limits,
                                          const RuleSourceDownload &download,
                                          size_t &downloaded_bytes);
