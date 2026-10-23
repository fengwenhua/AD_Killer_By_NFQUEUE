#include "update_job.h"
#include "bounded_log.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
std::string decode(const std::string &encoded) {
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text; unsigned buffer = 0, bits = 0;
    for (char c : encoded) {
        if (c == '=') break;
        const auto digit = alphabet.find(c); check(digit != std::string::npos, "base64 alphabet");
        buffer = (buffer << 6) | static_cast<unsigned>(digit); bits += 6;
        if (bits >= 8) { bits -= 8; text += static_cast<char>((buffer >> bits) & 255); }
    }
    return text;
}
}
int main() {
    char directory[] = "/tmp/adk-job-log.XXXXXX";
    auto *created = mkdtemp(directory); if (!created) return 1;
    const std::string root = created, id = "123-456", path = root + "/update-" + id + ".log";
    try {
        check(read_update_job_log(root, id).status == "not_ready", "missing log isn't an empty successful read");
        std::string content = "中文日志\n"; content += '\0'; content += static_cast<char>(255); content += "tail\n";
        check(append_bounded_log(path, content, 512, 0, true), "epoch log write");
        std::string cursor, assembled;
        for (unsigned count = 0; count < 100; ++count) {
            const auto page = read_update_job_log(root, id, cursor, 2);
            check(page.status == "ok" && page.data_base64.size() <= 4, "bounded binary page");
            check(cursor.empty() ? page.reset && page.reset_reason == "begin" : !page.reset, "cursor advances without reset");
            assembled += decode(page.data_base64); cursor = page.cursor;
            if (!page.more) break;
        }
        check(assembled == content, "split UTF-8, NUL and non-UTF8 bytes survive exact transfer");
        check(decode(read_update_job_log(root, id, cursor).data_base64).empty(), "same cursor never replays old bytes");
        check(append_bounded_log(path, "next", 512, 0, true), "append");
        auto page = read_update_job_log(root, id, cursor);
        check(!page.reset && decode(page.data_base64) == "next", "append reads only new bytes"); cursor = page.cursor;
        check(append_bounded_log(path, std::string(450, 'a'), 512, 0, true) &&
              append_bounded_log(path, std::string(450, 'b'), 512, 0, true), "two in-place rotations");
        page = read_update_job_log(root, id, cursor);
        check(page.status == "ok" && page.reset && page.reset_reason == "rotated" && decode(page.data_base64).find_first_not_of('b') == std::string::npos, "epoch detects truncate/regrowth without relying on inode reuse");
        auto last = read_update_job_log(root, id, "tail", 8);
        check(last.reset && last.reset_reason == "tail" && last.omitted_bytes > 0 && decode(last.data_base64) == "bbbbbbbb", "explicit tail reports omitted bytes");
        for (const auto &bad : {std::string("-1"), std::string("epoch:-1"), std::string("epoch:18446744073709551616"), std::string(200, 'x')})
            check(read_update_job_log(root, id, bad).status == "invalid_cursor", "bad cursor rejected");
        check(read_update_job_log(root, "../escape").status == "invalid_job", "fixed target rejects traversal");
        check(read_update_job_log(root, id, "", 0).status == "invalid_limit", "zero limit invalid");
        fs::remove(path); check(!mkfifo(path.c_str(), 0600), "FIFO fixture");
        check(read_update_job_log(root, id).status == "error", "FIFO cannot block or masquerade as a file"); fs::remove(path);
        std::ofstream(root + "/foreign") << "preserve";
        check(!symlink((root + "/foreign").c_str(), path.c_str()), "link fixture");
        check(read_update_job_log(root, id).status == "error", "symlinks never followed"); fs::remove(path);
        std::ofstream(path) << std::string(1024 * 1024 + 1, 'x');
        check(read_update_job_log(root, id).status == "error", "oversized file rejected before allocating it");
        fs::remove(path); std::ofstream(path) << "legacy text\n";
        check(decode(read_update_job_log(root, id).data_base64) == "legacy text\n", "old completed logs remain readable as snapshots");
    } catch (const std::exception &error) {
        fs::remove_all(root); std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
    fs::remove_all(root);
    std::cout << "job log pages: byte-exact UTF-8/binary, append, rotation epochs, tail omissions, bounds and unsafe targets passed\n";
}
