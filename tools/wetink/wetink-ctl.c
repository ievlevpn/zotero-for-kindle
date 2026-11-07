// Research tool, not built (see docs/scribe-pen.md).
// wetink-ctl TYPE HEXBYTES  -> one SysV message to WetInkHandler's queue (key 0x537a6f)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: wetink-ctl TYPE [HEX]\n");
        return 2;
    }
    struct { long type; unsigned char data[512]; } m;
    if (!strcmp(argv[1], "sem")) {  // wetink-ctl sem NUM DELTA: one semop on WetInkHandler's set
        int id = semget(0x537a70, 13, 0666);
        struct sembuf op = {(unsigned short)atoi(argv[2]), (short)atoi(argv[3]), IPC_NOWAIT};
        int r = semop(id, &op, 1);
        printf("semop %d %+d = %d\n", op.sem_num, op.sem_op, r);
        return r < 0;
    }
    if (!strcmp(argv[1], "drain")) {  // remove everything waiting in the queue
        int q = msgget(0x537a6f, 0666);
        int n = 0;
        while (msgrcv(q, &m, sizeof m.data, 0, IPC_NOWAIT | MSG_NOERROR) >= 0) ++n;
        printf("drained %d\n", n);
        return 0;
    }
    m.type = strtol(argv[1], 0, 0);
    size_t n = 0;
    const char* h = argc > 2 ? argv[2] : "";
    while (h[0] && h[1] && n < sizeof m.data) {
        if (h[0] == ' ') { ++h; continue; }
        unsigned v;
        sscanf(h, "%2x", &v);
        m.data[n++] = (unsigned char)v;
        h += 2;
    }
    int q = msgget(0x537a6f, 0666);
    if (q < 0) { perror("msgget"); return 1; }
    if (msgsnd(q, &m, n, IPC_NOWAIT) < 0) { perror("msgsnd"); return 1; }
    printf("sent type %ld, %zu bytes\n", m.type, n);
    return 0;
}
