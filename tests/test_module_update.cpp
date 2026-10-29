#include "module_update.h"
#include "sha256.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;
std::string hash(const std::string &value) {
    Sha256 sha; sha.update(value.data(),value.size()); return sha.finish();
}
void put(const fs::path &path, const std::string &value) { std::ofstream(path,std::ios::binary) << value; }
int main() {
    assert(hash("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(hash("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(hash(std::string(1000000,'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    std::string long_value(8193,'z');
    Sha256 fragmented;
    for (char c : long_value) fragmented.update(&c,1);
    assert(fragmented.finish() == hash(long_value));
    for (const char *value : {"1.0","1.1","1.2.3","999999.0"}) assert(module_update::valid_version(value));
    for (const char *value : {"", "1", "1.2.3.4", "1.0/../../x", "$(id)", "1.2-rc1", "1000000.0", ".1", "1."})
        assert(!module_update::valid_version(value));
    assert(module_update::newer_version("1.10","1.9"));
    assert(!module_update::newer_version("1.0.0","1.0"));
    assert(!module_update::newer_version("1.0","1.1"));
    for (const auto &installed : {"stable", "preview", "unknown"}) {
        assert(module_update::upgrade_allowed("1.2.2","1.2.1",installed,"stable"));
        assert(module_update::upgrade_allowed("1.2.2","1.2.1",installed,"preview"));
        assert(!module_update::upgrade_allowed("1.2.1","1.2.2",installed,"stable"));
        assert(!module_update::upgrade_allowed("1.2.2","1.2.2",installed,"preview"));
        assert(module_update::upgrade_allowed("1.2.2","1.2.2",installed,"stable") == (std::string(installed)=="preview"));
    }
    assert(!module_update::upgrade_allowed("1.2","1.1","preview","unknown"));
    assert(module_update::upgrade_allowed("1.2.10","1.2.9","preview","preview"));
    assert(!module_update::upgrade_allowed("1.2.9","1.2.9","preview","preview"));
    assert(module_update::asset_url("../bad").empty());
    char pattern[] = "/tmp/adk_module_update.XXXXXX";
    char *created = mkdtemp(pattern); assert(created);
    fs::path root(created), directory = root/"updates";
    const std::string bytes = "PK\003\004test-package";
    const auto digest = hash(bytes);
    std::string checksum = digest+"  AD_Killer_By_NFQUEUE-v1.2.zip\n";
    bool limited = false;
    std::string fetch_error, prefix_error, payload = bytes;
    auto prefix = [&](const std::string &url,std::string &out,bool &cut,size_t limit) {
        assert(url == module_update::asset_url("1.2")+".sha256" && limit == 512);
        out = checksum; cut = limited; return prefix_error;
    };
    auto fetch = [&](const std::string &url,const std::string &path) {
        assert(url == module_update::asset_url("1.2"));
        put(path,payload); return fetch_error;
    };
    assert(module_update::download("1.2",directory,fetch,prefix).empty());
    assert(module_update::ready_version(directory) == "1.2");
    assert(module_update::ready_channel(directory) == "stable");
    assert(module_update::download("1.2",directory,fetch,prefix,true).empty());
    assert(module_update::ready_channel(directory) == "preview");
    put(directory/"ready", "1.2\n"+digest+"\ninvalid\n");
    assert(module_update::ready_version(directory).empty());
    put(directory/"ready", "1.2\n"+digest+"\n");
    assert(module_update::ready_version(directory) == "1.2");
    assert(module_update::ready_channel(directory) == "unknown");
    put(directory/"package.zip", bytes+"tampered");
    assert(module_update::ready_version(directory).empty());
    for (int fault = 0; fault < 5; ++fault) {
        limited = fault == 0;
        prefix_error = fault == 1 ? "TLS failed" : "";
        fetch_error = fault == 2 ? "download failed" : "";
        checksum = digest+"  "+(fault == 3 ? "wrong.zip" : "AD_Killer_By_NFQUEUE-v1.2.zip")+"\n";
        payload = fault == 4 ? bytes+"corrupt" : bytes;
        assert(!module_update::download("1.2",directory,fetch,prefix).empty());
        assert(module_update::ready_version(directory).empty());
        assert(!fs::exists(directory/"candidate.zip"));
    }
    put(root/"html", "<html>error</html>");
    assert(!module_update::verify_package(root/"html",hash("<html>error</html>")).empty());
    fs::create_symlink(root/"html",root/"link");
    assert(!module_update::verify_package(root/"link",hash("<html>error</html>")).empty());
    fs::create_directory_symlink(directory,root/"alias");
    assert(!module_update::download("1.2",root/"alias",fetch,prefix).empty());
    fs::remove_all(root);
    std::cout << "module update SHA-256 vectors, version bounds, checksum mismatch, tampering, errors and symlink tests passed\n";
}
