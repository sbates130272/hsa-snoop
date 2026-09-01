#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
int main(int argc, char** argv) {
    int pid = atoi(argv[1]);
    char path[64]; snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE* f = fopen(path, "r");
    if (!f) { perror("maps"); return 1; }
    snprintf(path, sizeof path, "/proc/%d/mem", pid);
    int memfd = open(path, O_RDONLY);
    if (memfd < 0) perror("open /proc/pid/mem");
    char line[512];
    printf("%-34s %8s %-5s %-14s %-14s %s\n","range","size","perm","backing","process_vm_readv","pread(/proc/pid/mem)");
    while (fgets(line, sizeof line, f)) {
        unsigned long a,b; char perm[8]={0}, back[256]={0};
        if (sscanf(line,"%lx-%lx %7s",&a,&b,perm)!=3) continue;
        if (perm[0]!='r') continue;
        unsigned long sz=b-a;
        if (sz < (3UL<<20)) continue;
        char* sl=strchr(line,'/');
        if (sl){ strncpy(back,sl,sizeof(back)-1); back[strcspn(back,"\n")]=0; }
        const char* base = back[0] ? strrchr(back,'/')+1 : "anon";
        unsigned char buf[64];
        struct iovec l={buf,sizeof buf}, r={(void*)a,sizeof buf};
        ssize_t n1 = process_vm_readv(pid,&l,1,&r,1,0);
        char e1[64]; snprintf(e1,sizeof e1,"%s", n1<0?strerror(errno):"OK");
        ssize_t n2 = memfd>=0 ? pread(memfd, buf, sizeof buf, (off_t)a) : -1;
        char e2[64]; snprintf(e2,sizeof e2,"%s", n2<0?strerror(errno):"OK");
        printf("%016lx-%016lx %7.1fM %-5s %-14.14s %-14s %s\n", a,b,sz/1048576.0,perm,base,e1,e2);
    }
    return 0;
}
