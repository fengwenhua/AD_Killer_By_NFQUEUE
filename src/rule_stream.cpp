#include "rule_stream.h"
#include "work_budget.h"
#include "operation_gate.h"

#include <cstdio>
#include <sys/stat.h>
#include <zlib.h>

namespace {

class LineSplitter {
public:
    LineSplitter(const RuleLineCallback &callback, const RuleStreamLimits &limits,
                 RuleStreamResult &result)
        : callback_(callback), limits_(limits), result_(result) {}

    bool consume(const char *data, size_t size) {
        if (WorkBudget::expired() || operation_closing()) {
            result_.error = "cancelled_or_time_budget";
            return false;
        }
        if (size > limits_.output_bytes - result_.output_bytes) {
            result_.output_bytes = limits_.output_bytes;
            result_.error = "decompressed_size_limit";
            return false;
        }
        result_.output_bytes += size;
        for (size_t i = 0; i < size; ++i) {
            if (data[i] == '\n') {
                if (!emit()) return false;
                continue;
            }
            if (line_.size() < limits_.line_bytes) line_.push_back(data[i]);
            else overlong_ = true;
        }
        return true;
    }

    bool finish() {
        if (!line_.empty() || overlong_) return emit();
        return true;
    }

private:
    bool emit() {
        if (result_.lines >= limits_.lines) {
            result_.error = "line_count_limit";
            return false;
        }
        result_.lines++;
        if (overlong_) result_.overlong_lines++;
        callback_(line_, overlong_);
        line_.clear();
        overlong_ = false;
        return true;
    }

    const RuleLineCallback &callback_;
    const RuleStreamLimits &limits_;
    RuleStreamResult &result_;
    std::string line_;
    bool overlong_ = false;
};

bool detect_gzip(const char *path, bool &gzip) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    unsigned char magic[2] = {0, 0};
    size_t read = fread(magic, 1, sizeof(magic), file);
    bool failed = ferror(file) != 0;
    fclose(file);
    if (failed) return false;
    gzip = read == sizeof(magic) && magic[0] == 0x1f && magic[1] == 0x8b;
    return true;
}

}  // namespace

bool stream_rule_file(const char *path, bool require_gzip,
                      const RuleLineCallback &callback,
                      RuleStreamResult *result_out,
                      const RuleStreamLimits &limits) {
    RuleStreamResult result;
    if (!limits.output_bytes || !limits.lines) {
        result.error = !limits.output_bytes ? "decompressed_size_limit" : "line_count_limit";
        if (result_out) *result_out = result;
        return false;
    }
    struct stat info {};
    if (!path || stat(path, &info) != 0 || info.st_size <= 0) {
        result.error = "empty_or_unreadable";
        if (result_out) *result_out = result;
        return false;
    }
    result.input_bytes = static_cast<size_t>(info.st_size);
    if (result.input_bytes > limits.input_bytes) {
        result.error = "compressed_size_limit";
        if (result_out) *result_out = result;
        return false;
    }
    if (!detect_gzip(path, result.gzip)) {
        result.error = "read_failed";
        if (result_out) *result_out = result;
        return false;
    }
    if (require_gzip && !result.gzip) {
        result.error = "gzip_required";
        if (result_out) *result_out = result;
        return false;
    }

    LineSplitter splitter(callback, limits, result);
    char buffer[64u * 1024u];
    bool ok = true;
    if (result.gzip) {
        gzFile file = gzopen(path, "rb");
        if (!file) {
            result.error = "read_failed";
            ok = false;
        } else {
            int count = 0;
            while ((count = gzread(file, buffer, sizeof(buffer))) > 0) {
                if (!splitter.consume(buffer, static_cast<size_t>(count))) {
                    ok = false;
                    break;
                }
            }
            int zerror = Z_OK;
            if (ok && count < 0) {
                gzerror(file, &zerror);
                result.error = "gzip_decode_failed";
                ok = false;
            } else if (ok && !gzeof(file)) {
                result.error = "gzip_decode_failed";
                ok = false;
            }
            if (gzclose(file) != Z_OK && ok) {
                result.error = "gzip_decode_failed";
                ok = false;
            }
        }
    } else {
        FILE *file = fopen(path, "rb");
        if (!file) {
            result.error = "read_failed";
            ok = false;
        } else {
            size_t count = 0;
            while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
                if (!splitter.consume(buffer, count)) {
                    ok = false;
                    break;
                }
            }
            if (ok && ferror(file)) {
                result.error = "read_failed";
                ok = false;
            }
            fclose(file);
        }
    }
    if (ok && !splitter.finish()) ok = false;
    if (ok && result.output_bytes == 0) {
        result.error = "empty_content";
        ok = false;
    }
    if (result_out) *result_out = result;
    return ok;
}
