#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void usage(FILE *out) {
    fprintf(out,
            "usage: dso-run [options] -- program [args...]\n"
            "\n"
            "Linux user-space contract launcher.\n"
            "\n"
            "Options:\n"
            "  --cpu N        pin process to CPU N with sched_setaffinity\n"
            "  --mem-mb N     set RLIMIT_AS to N MiB before exec\n"
            "  --mlock        call mlockall(MCL_CURRENT|MCL_FUTURE) before exec\n"
            "  --timeout-sec N terminate child after N seconds and still report\n"
            "  --cg-mem N     set cgroup v2 memory.max to N bytes\n"
            "  --cg-cpu Q P   set cgroup v2 cpu.max to Q usec / P usec period\n");
}

static unsigned long long monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

// ── cgroup v2 ──────────────────────────────────────────────────────────────────
// Creates a cgroup under /sys/fs/cgroup/dso-<pid>/ and applies limits.
// Returns the cgroup path on success (caller must free), NULL on failure.
static char *cgroup_setup(pid_t child_pid, long mem_max, long cpu_quota, long cpu_period) {
    char path[256];
    int n = snprintf(path, sizeof(path), "/sys/fs/cgroup/dso-%d", child_pid);
    if (n <= 0 || (size_t)n >= sizeof(path)) return NULL;

    // Create cgroup directory
    if (mkdir(path, 0755) != 0) {
        fprintf(stderr, "cgroup: mkdir %s failed: %s\n", path, strerror(errno));
        return NULL;
    }

    char buf[256];
    int fd;

    // memory.max
    if (mem_max > 0) {
        n = snprintf(buf, sizeof(buf), "%s/memory.max", path);
        fd = open(buf, O_WRONLY);
        if (fd < 0) {
            fprintf(stderr, "cgroup: open memory.max failed: %s\n", strerror(errno));
            rmdir(path);
            return NULL;
        }
        n = snprintf(buf, sizeof(buf), "%ld\n", mem_max);
        write(fd, buf, (size_t)n);
        close(fd);
    }

    // cpu.max
    if (cpu_quota >= 0 && cpu_period > 0) {
        n = snprintf(buf, sizeof(buf), "%s/cpu.max", path);
        fd = open(buf, O_WRONLY);
        if (fd < 0) {
            fprintf(stderr, "cgroup: open cpu.max failed: %s\n", strerror(errno));
            rmdir(path);
            return NULL;
        }
        n = snprintf(buf, sizeof(buf), "%ld %ld\n", cpu_quota, cpu_period);
        write(fd, buf, (size_t)n);
        close(fd);
    }

    // Add child PID to cgroup.procs
    n = snprintf(buf, sizeof(buf), "%s/cgroup.procs", path);
    fd = open(buf, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "cgroup: open cgroup.procs failed: %s\n", strerror(errno));
        rmdir(path);
        return NULL;
    }
    n = snprintf(buf, sizeof(buf), "%d\n", child_pid);
    write(fd, buf, (size_t)n);
    close(fd);

    return strdup(path);
}

static void cgroup_cleanup(const char *path) {
    if (!path) return;
    // Remove the cgroup directory (must be empty — no processes)
    rmdir(path);
    free((void *)path);
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
    long cg_mem = -1;
    long cg_cpu_quota = -1;
    long cg_cpu_period = -1;
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
        if (strcmp(argv[i], "--cg-mem") == 0 && i + 1 < argc) {
            if (parse_long(argv[++i], &cg_mem) != 0 || cg_mem < 0) {
                fprintf(stderr, "invalid --cg-mem value\n");
                return 2;
            }
            continue;
        }
        if (strcmp(argv[i], "--cg-cpu") == 0 && i + 2 < argc) {
            if (parse_long(argv[++i], &cg_cpu_quota) != 0 || cg_cpu_quota < 0) {
                fprintf(stderr, "invalid --cg-cpu quota value\n");
                return 2;
            }
            if (parse_long(argv[++i], &cg_cpu_period) != 0 || cg_cpu_period <= 0) {
                fprintf(stderr, "invalid --cg-cpu period value\n");
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

    // Set up cgroup v2 for the child (before any resource-constrained work)
    char *cgroup_path = NULL;
    if (cg_mem > 0 || cg_cpu_quota >= 0) {
        cgroup_path = cgroup_setup(child, cg_mem, cg_cpu_quota, cg_cpu_period);
        if (!cgroup_path) {
            fprintf(stderr, "dso-run: cgroup setup failed, continuing without cgroup\n");
        }
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

    // Clean up cgroup
    cgroup_cleanup(cgroup_path);

    struct rusage usage;
    memset(&usage, 0, sizeof(usage));
    getrusage(RUSAGE_CHILDREN, &usage);

    fprintf(stderr,
            "dso-run report: elapsed_ms=%llu user_ms=%ld sys_ms=%ld maxrss_kb=%ld "
            "cpu=%d mem_mb=%ld cg_mem=%ld timed_out=%d\n",
            elapsed_ns / 1000000ull,
            usage.ru_utime.tv_sec * 1000L + usage.ru_utime.tv_usec / 1000L,
            usage.ru_stime.tv_sec * 1000L + usage.ru_stime.tv_usec / 1000L,
            usage.ru_maxrss,
            cpu,
            mem_mb,
            cg_mem,
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
