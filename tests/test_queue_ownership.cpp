#include "queue_ownership.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sys/stat.h>

int main() {
    auto owners = parse_queue_owners("    0 13268 0 2 768 0 0 42 1\n"
                                     "1 3139568836 0 2 4096 0 0 9 1\n"
                                     "65536 2\n0 -1\n0 0\nheader text\n");
    assert(owners.size() == 2 && owners[1].port_id == 3139568836u);
    char temporary[] = "/tmp/ad_killer_by_nfqueue_queue_owner.XXXXXX";
    assert(mkdtemp(temporary));
    std::string root = temporary;
    for (const auto &path : {"/net", "/net/netfilter", "/13268", "/13268/fd"})
        assert(mkdir((root + path).c_str(), 0700) == 0);
    std::ofstream(root + "/net/netlink") << "sk Eth Pid Groups Rmem Wmem Dump Locks Drops Inode\n"
        "0 12 13268 0 0 0 0 2 0 1001\n";
    std::ofstream(root + "/net/netfilter/nfnetlink_queue") << "0 13268 0 2 768 0 0 42 1\n";
    std::ofstream(root + "/13268/comm") << "YH_hosts\n";
    assert(symlink("socket:[1001]", (root + "/13268/fd/3").c_str()) == 0);
    assert(queue_owner_process(13268, root) == "YH_hosts");
    assert(find_queue_conflict(root).find("YH_hosts") != std::string::npos);
    assert(find_queue_conflict(root, 1).empty());
    auto queue1 = queue_conflict_message(owners, root, 1);
    assert(queue1.find("3139568836") != std::string::npos && queue1.find("YH_hosts") == std::string::npos);
    assert(unlink((root + "/13268/fd/3").c_str()) == 0);
    assert(queue_owner_process(13268, root).empty());
    assert(find_queue_conflict(root).find("YH_hosts") == std::string::npos);
    assert(find_queue_conflict(root + "/absent").empty());
    for (const auto &path : {"/13268/comm", "/net/netlink", "/net/netfilter/nfnetlink_queue"})
        assert(unlink((root + path).c_str()) == 0);
    for (const auto &path : {"/13268/fd", "/13268", "/net/netfilter", "/net"})
        assert(rmdir((root + path).c_str()) == 0);
    assert(rmdir(root.c_str()) == 0);
    std::cout << "queue ownership parsing, verified process identity and queue filtering passed\n";
}
