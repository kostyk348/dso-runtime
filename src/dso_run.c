#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

static void usage(FILE *out) {
    fprintf(out,
            "usage: dso-run [--cpu N] [--mem-mb N] [--mlock] -- program [args...]\n"
            "\n"
            "Linux user-space contract launcher.\n"
            "\n"
            "Options:\n"
            "  --cpu N      pin process to CPU N with sched_setaffinity\n"
            "  --mem-mb N   set RLIMIT_AS to N MiB before exec\n"
            "  --mlock      call mlockall(MCL_CURRENT|MCL_FUTURE) before exec\n");
}

static int parse_long(const char *text, long *out) {
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    *out = value;
    return 0;
}

int main(int argc, char **argv) {
    int cpu = -1;
    long mem_mb = -1;
    int do_mlock = 0;
    int cmd_index = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            cmd_index = i + 1;
            break;
        }
        if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
            long value = 0;
            if (parse_long(argv[++i], &value) != 0 || value < 0) {
                fprintf(stderr, "invalid --cpu value\n");
                return 2;
            }
            cpu = (int)value;
            continue;
        }
        if (strcmp(argv[i], "--mem-mb") == 0 && i + 1 < argc) {
            if (parse_long(argv[++i], &mem_mb) != 0 || mem_mb <= 0) {
                fprintf(stderr, "invalid --mem-mb value\n");
                return 2;
            }
            continue;
        }
        if (strcmp(argv[i], "--mlock") == 0) {
            do_mlock = 1;
            continue;
        }
        fprintf(stderr, "unknown option: %s\n", argv[i]);
        usage(stderr);
        return 2;
    }

    if (cmd_index < 0 || cmd_index >= argc) {
        usage(stderr);
        return 2;
    }

    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0) {
            fprintf(stderr, "sched_setaffinity failed: %s\n", strerror(errno));
            return 1;
        }
    }

    if (mem_mb > 0) {
        struct rlimit limit;
        limit.rlim_cur = (rlim_t)mem_mb * 1024u * 1024u;
        limit.rlim_max = limit.rlim_cur;
        if (setrlimit(RLIMIT_AS, &limit) != 0) {
            fprintf(stderr, "setrlimit(RLIMIT_AS) failed: %s\n", strerror(errno));
            return 1;
        }
    }

    if (do_mlock) {
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
            fprintf(stderr, "mlockall failed: %s\n", strerror(errno));
            return 1;
        }
    }

    execvp(argv[cmd_index], &argv[cmd_index]);
    fprintf(stderr, "execvp failed: %s\n", strerror(errno));
    return 127;
}
