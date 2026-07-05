#include <errno.h>
#include <signal.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
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
            "  --mlock      call mlockall(MCL_CURRENT|MCL_FUTURE) before exec\n"
            "  --timeout-sec N terminate child after N seconds and still report\n");
}

static unsigned long long monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
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
    long timeout_sec = -1;
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
        if (strcmp(argv[i], "--timeout-sec") == 0 && i + 1 < argc) {
            if (parse_long(argv[++i], &timeout_sec) != 0 || timeout_sec <= 0) {
                fprintf(stderr, "invalid --timeout-sec value\n");
                return 2;
            }
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

    unsigned long long start_ns = monotonic_ns();
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        return 1;
    }

    if (child == 0) {
        if (cpu >= 0) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(cpu, &set);
            if (sched_setaffinity(0, sizeof(set), &set) != 0) {
                fprintf(stderr, "sched_setaffinity failed: %s\n", strerror(errno));
                _exit(1);
            }
        }

        if (mem_mb > 0) {
            struct rlimit limit;
            limit.rlim_cur = (rlim_t)mem_mb * 1024u * 1024u;
            limit.rlim_max = limit.rlim_cur;
            if (setrlimit(RLIMIT_AS, &limit) != 0) {
                fprintf(stderr, "setrlimit(RLIMIT_AS) failed: %s\n", strerror(errno));
                _exit(1);
            }
        }

        if (do_mlock) {
            if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
                fprintf(stderr, "mlockall failed: %s\n", strerror(errno));
                _exit(1);
            }
        }

        execvp(argv[cmd_index], &argv[cmd_index]);
        fprintf(stderr, "execvp failed: %s\n", strerror(errno));
        _exit(127);
    }

    int status = 0;
    int timed_out = 0;
    for (;;) {
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) {
            break;
        }
        if (done < 0) {
            fprintf(stderr, "waitpid failed: %s\n", strerror(errno));
            return 1;
        }
        unsigned long long elapsed_ns = monotonic_ns() - start_ns;
        if (timeout_sec > 0 && elapsed_ns >= (unsigned long long)timeout_sec * 1000000000ull) {
            timed_out = 1;
            kill(child, SIGTERM);
            struct timespec grace = {.tv_sec = 0, .tv_nsec = 200000000L};
            nanosleep(&grace, NULL);
            if (waitpid(child, &status, WNOHANG) == 0) {
                kill(child, SIGKILL);
            }
            waitpid(child, &status, 0);
            break;
        }
        struct timespec sleep_for = {.tv_sec = 0, .tv_nsec = 10000000L};
        nanosleep(&sleep_for, NULL);
    }
    unsigned long long elapsed_ns = monotonic_ns() - start_ns;
    struct rusage usage;
    memset(&usage, 0, sizeof(usage));
    getrusage(RUSAGE_CHILDREN, &usage);

    fprintf(stderr,
            "dso-run report: elapsed_ms=%llu user_ms=%ld sys_ms=%ld maxrss_kb=%ld cpu=%d mem_mb=%ld timed_out=%d\n",
            elapsed_ns / 1000000ull,
            usage.ru_utime.tv_sec * 1000L + usage.ru_utime.tv_usec / 1000L,
            usage.ru_stime.tv_sec * 1000L + usage.ru_stime.tv_usec / 1000L,
            usage.ru_maxrss,
            cpu,
            mem_mb,
            timed_out);

    if (timed_out) {
        return 124;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "dso-run: child killed by signal %d\n", WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return 1;
}
