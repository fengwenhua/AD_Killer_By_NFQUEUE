#pragma once
#include <cstddef>
#include <string>

constexpr size_t SOURCE_PREVIEW_DEFAULT = 500;
constexpr size_t SOURCE_PREVIEW_MIN = 100;
constexpr size_t SOURCE_PREVIEW_MAX = 5000;

struct SourcePreview {
    std::string text;
    std::string error;
    size_t characters = 0;
    bool truncated = false;
};

// Optional gzip prefix, strict UTF-8, bounded Unicode code points. A preview
// deliberately does not claim complete gzip/source validity or publish rules.
SourcePreview format_source_preview(const std::string &bytes, bool limited, size_t characters = SOURCE_PREVIEW_DEFAULT);
SourcePreview preview_source(const std::string &url, size_t characters = SOURCE_PREVIEW_DEFAULT);
