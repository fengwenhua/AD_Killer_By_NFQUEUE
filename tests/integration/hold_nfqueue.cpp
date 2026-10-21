#include "nfnetlink.h"
#include <cstdio>
#include <unistd.h>

int main() {
    int sockets[2] = {-1, -1};
    for (int queue = 0; queue < 2; queue++) {
        sockets[queue] = nfq_socket_open();
        if (sockets[queue] < 0 || !nfq_queue_bind(sockets[queue], static_cast<uint16_t>(queue))) {
            perror("hold queue");
            return 1;
        }
    }
    puts("READY");
    fflush(stdout);
    char release;
    (void)read(STDIN_FILENO, &release, 1);
    for (int queue = 0; queue < 2; queue++) {
        nfq_queue_unbind(sockets[queue], static_cast<uint16_t>(queue));
        close(sockets[queue]);
    }
    return 0;
}
