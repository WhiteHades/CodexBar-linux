#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    CONFIGURATION_FD = 3,
    STARTUP_FD = 4,
    ACKNOWLEDGEMENT_FD = 5,
    MAX_CONFIGURATION_BYTES = 16 * 1024 * 1024,
};

typedef struct {
    uint32_t working_directory_length;
    uint32_t environment_count;
} ProcessConfiguration;

typedef struct {
    int32_t error_code;
    int32_t target_pid;
} ProcessStartup;

static volatile sig_atomic_t startup_terminate;
static volatile sig_atomic_t startup_force;

static void handle_startup_signal(int signal_number) {
    startup_terminate = 1;
    if (signal_number == SIGUSR1) startup_force = 1;
}

static bool read_all(int descriptor, void *data, size_t length) {
    unsigned char *bytes = data;
    size_t received = 0;
    while (received < length) {
        ssize_t count = read(descriptor, bytes + received, length - received);
        if (count > 0) {
            received += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool write_all(int descriptor, const void *data, size_t length) {
    const unsigned char *bytes = data;
    size_t written = 0;
    while (written < length) {
        ssize_t count = write(descriptor, bytes + written, length - written);
        if (count > 0) {
            written += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool report_startup(int error_code, pid_t target, bool close_after) {
    ProcessStartup startup = {.error_code = error_code, .target_pid = target};
    bool reported = write_all(STARTUP_FD, &startup, sizeof(startup));
    if (close_after) close(STARTUP_FD);
    return reported;
}

static void target_setup_failed(int descriptor, int error_code) {
    int saved_code = error_code;
    while (write(descriptor, &saved_code, sizeof(saved_code)) < 0 && errno == EINTR) {}
    _exit(127);
}

static void close_target_descriptors(void) {
    if (close_range(4, UINT_MAX, 0) == 0) return;
    long maximum = sysconf(_SC_OPEN_MAX);
    if (maximum < 0) maximum = 1024;
    for (int descriptor = 4; descriptor < maximum; descriptor++) close(descriptor);
}

static void free_environment(char **environment) {
    if (!environment) return;
    for (size_t index = 0; environment[index]; index++) free(environment[index]);
    free(environment);
}

static int read_configuration(char **working_directory, char ***environment, bool *inherit_environment) {
    ProcessConfiguration configuration;
    if (!read_all(CONFIGURATION_FD, &configuration, sizeof(configuration))) return EIO;
    size_t consumed = 0;
    if (configuration.working_directory_length != UINT32_MAX) {
        consumed = configuration.working_directory_length;
        if (consumed > MAX_CONFIGURATION_BYTES) return E2BIG;
        *working_directory = malloc(consumed + 1);
        if (!*working_directory) return ENOMEM;
        if (!read_all(CONFIGURATION_FD, *working_directory, consumed)) return EIO;
        (*working_directory)[consumed] = '\0';
    }
    if (configuration.environment_count == UINT32_MAX) {
        *inherit_environment = true;
        close(CONFIGURATION_FD);
        return 0;
    }
    if (configuration.environment_count > 131072) return E2BIG;
    *environment = calloc((size_t)configuration.environment_count + 1, sizeof(char *));
    if (!*environment) return ENOMEM;
    for (uint32_t index = 0; index < configuration.environment_count; index++) {
        uint32_t length = 0;
        if (!read_all(CONFIGURATION_FD, &length, sizeof(length))) return EIO;
        consumed += sizeof(length) + length;
        if (consumed > MAX_CONFIGURATION_BYTES) return E2BIG;
        (*environment)[index] = malloc((size_t)length + 1);
        if (!(*environment)[index]) return ENOMEM;
        if (!read_all(CONFIGURATION_FD, (*environment)[index], length)) return EIO;
        (*environment)[index][length] = '\0';
    }
    close(CONFIGURATION_FD);
    return 0;
}

static long long monotonic_milliseconds(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) < 0) return 0;
    return (long long)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static bool process_is_direct_child(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return false;
    char buffer[4096];
    ssize_t length = read(descriptor, buffer, sizeof(buffer) - 1);
    close(descriptor);
    if (length <= 0) return false;
    buffer[length] = '\0';
    char *name_end = strrchr(buffer, ')');
    if (!name_end) return false;
    char state = '\0';
    long parent = -1;
    return sscanf(name_end + 2, "%c %ld", &state, &parent) == 2 && parent == (long)getpid() && state != 'Z';
}

static void signal_direct_children(int signal_number) {
    char path[96];
    snprintf(path, sizeof(path), "/proc/self/task/%ld/children", (long)getpid());
    FILE *children = fopen(path, "re");
    if (!children) return;
    long child;
    while (fscanf(children, "%ld", &child) == 1) {
        if (child > 0 && child <= INT_MAX && process_is_direct_child((pid_t)child)) {
            kill((pid_t)child, signal_number);
        }
    }
    fclose(children);
}

static void signal_owned_tree(pid_t target, bool target_exited, int signal_number) {
    if (signal_number == SIGKILL) {
        if (!target_exited) kill(target, signal_number);
    } else {
        kill(-getpid(), signal_number);
        sigset_t own_signal;
        sigemptyset(&own_signal);
        sigaddset(&own_signal, signal_number);
        struct timespec no_wait = {0};
        sigtimedwait(&own_signal, NULL, &no_wait);
    }
    signal_direct_children(signal_number);
}

static bool reap_available(pid_t target, bool *target_exited, int *target_status, bool *children_remain) {
    while (true) {
        int status = 0;
        pid_t child = waitpid(-1, &status, WNOHANG);
        if (child > 0) {
            if (child == target) {
                *target_exited = true;
                *target_status = status;
            }
            continue;
        }
        if (child == 0) {
            *children_remain = true;
            return true;
        }
        if (errno == EINTR) continue;
        if (errno == ECHILD) {
            *children_remain = false;
            return true;
        }
        return false;
    }
}

static int return_target_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        int signal_number = WTERMSIG(status);
        struct sigaction action = {.sa_handler = SIG_DFL};
        sigemptyset(&action.sa_mask);
        sigaction(signal_number, &action, NULL);
        sigset_t unblocked;
        sigemptyset(&unblocked);
        sigaddset(&unblocked, signal_number);
        sigprocmask(SIG_UNBLOCK, &unblocked, NULL);
        raise(signal_number);
        return 128 + signal_number;
    }
    return 125;
}

static int print_usage(const char *program) {
    fprintf(stderr,
            "Usage: %s <parent-pid> <--session|--group> <grace-ms> -- <program> [arguments...]\n",
            program);
    return 125;
}

int main(int argc, char **argv) {
    if (argc < 6 || (strcmp(argv[2], "--session") != 0 && strcmp(argv[2], "--group") != 0) ||
        strcmp(argv[4], "--") != 0) {
        return print_usage(argv[0]);
    }
    char *end = NULL;
    errno = 0;
    long parsed_parent = strtol(argv[1], &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed_parent <= 0 || parsed_parent > INT_MAX) {
        return print_usage(argv[0]);
    }
    pid_t expected_parent = (pid_t)parsed_parent;
    end = NULL;
    errno = 0;
    unsigned long parsed_grace = strtoul(argv[3], &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed_grace > 60000) return print_usage(argv[0]);
    unsigned int grace_milliseconds = (unsigned int)parsed_grace;

    sigset_t controls;
    sigemptyset(&controls);
    sigaddset(&controls, SIGCHLD);
    sigaddset(&controls, SIGTERM);
    sigaddset(&controls, SIGINT);
    sigaddset(&controls, SIGHUP);
    sigaddset(&controls, SIGUSR1);
    sigaddset(&controls, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &controls, NULL) < 0 || prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        report_startup(errno, -1, true);
        return 125;
    }
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0) {
        report_startup(errno, -1, true);
        return 125;
    }
    if (getppid() != expected_parent) {
        report_startup(ESRCH, -1, true);
        return 125;
    }

    char *working_directory = NULL;
    char **configured_environment = NULL;
    bool inherit_environment = false;
    int code = read_configuration(&working_directory, &configured_environment, &inherit_environment);
    if (code != 0) {
        report_startup(code, -1, true);
        free(working_directory);
        free_environment(configured_environment);
        return 125;
    }
    if (getppid() != expected_parent) {
        report_startup(ESRCH, -1, true);
        free(working_directory);
        free_environment(configured_environment);
        return 125;
    }

    int execution_pipe[2];
    if (pipe2(execution_pipe, O_CLOEXEC) < 0) {
        code = errno;
        report_startup(code, -1, true);
        free(working_directory);
        free_environment(configured_environment);
        return 125;
    }
    pid_t supervisor_pid = getpid();
    pid_t target = fork();
    if (target < 0) {
        code = errno;
        close(execution_pipe[0]);
        close(execution_pipe[1]);
        report_startup(code, -1, true);
        free(working_directory);
        free_environment(configured_environment);
        return 125;
    }
    if (target == 0) {
        close(execution_pipe[0]);
        if (execution_pipe[1] != CONFIGURATION_FD) {
            if (dup3(execution_pipe[1], CONFIGURATION_FD, O_CLOEXEC) < 0) _exit(127);
            close(execution_pipe[1]);
        }
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != supervisor_pid) {
            target_setup_failed(CONFIGURATION_FD, ESRCH);
        }
        sigset_t empty_mask;
        sigemptyset(&empty_mask);
        if (sigprocmask(SIG_SETMASK, &empty_mask, NULL) < 0) target_setup_failed(CONFIGURATION_FD, errno);
        struct sigaction default_action = {.sa_handler = SIG_DFL};
        sigemptyset(&default_action.sa_mask);
        for (int signal_number = 1; signal_number < NSIG; signal_number++) {
            if (signal_number != SIGKILL && signal_number != SIGSTOP) {
                sigaction(signal_number, &default_action, NULL);
            }
        }
        if (working_directory && chdir(working_directory) < 0) target_setup_failed(CONFIGURATION_FD, errno);
        int null_descriptor = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (null_descriptor < 0 || dup2(null_descriptor, STDIN_FILENO) < 0) {
            target_setup_failed(CONFIGURATION_FD, errno);
        }
        if (null_descriptor > STDERR_FILENO) close(null_descriptor);
        close_target_descriptors();
        raise(SIGSTOP);
        char **target_environment = inherit_environment ? environ : configured_environment;
        execvpe(argv[5], argv + 5, target_environment);
        target_setup_failed(CONFIGURATION_FD, errno);
    }
    close(execution_pipe[1]);
    free(working_directory);
    free_environment(configured_environment);

    int target_status = 0;
    pid_t waited;
    do {
        waited = waitpid(target, &target_status, WUNTRACED);
    } while (waited < 0 && errno == EINTR);
    if (waited != target || !WIFSTOPPED(target_status)) {
        int execution_error = ECHILD;
        read(execution_pipe[0], &execution_error, sizeof(execution_error));
        close(execution_pipe[0]);
        report_startup(execution_error, -1, true);
        return 125;
    }
    if (!report_startup(0, target, false)) {
        kill(target, SIGKILL);
        while (waitpid(target, &target_status, 0) < 0 && errno == EINTR) {}
        close(execution_pipe[0]);
        return 125;
    }
    char acknowledgement = 0;
    ssize_t acknowledged;
    do {
        acknowledged = read(ACKNOWLEDGEMENT_FD, &acknowledgement, 1);
    } while (acknowledged < 0 && errno == EINTR);
    close(ACKNOWLEDGEMENT_FD);
    if (acknowledged != 1 || acknowledgement != '1') {
        kill(target, SIGKILL);
        while (waitpid(target, &target_status, 0) < 0 && errno == EINTR) {}
        close(execution_pipe[0]);
        close(STARTUP_FD);
        return 125;
    }
    sigset_t startup_signals;
    sigemptyset(&startup_signals);
    sigaddset(&startup_signals, SIGTERM);
    sigaddset(&startup_signals, SIGINT);
    sigaddset(&startup_signals, SIGHUP);
    sigaddset(&startup_signals, SIGUSR1);
    struct sigaction startup_action = {.sa_handler = handle_startup_signal};
    sigemptyset(&startup_action.sa_mask);
    sigaction(SIGTERM, &startup_action, NULL);
    sigaction(SIGINT, &startup_action, NULL);
    sigaction(SIGHUP, &startup_action, NULL);
    sigaction(SIGUSR1, &startup_action, NULL);
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != expected_parent) startup_terminate = 1;
    sigprocmask(SIG_UNBLOCK, &startup_signals, NULL);
    kill(target, SIGCONT);
    int execution_error = 0;
    ssize_t execution_result;
    do {
        execution_result = read(execution_pipe[0], &execution_error, sizeof(execution_error));
    } while (execution_result < 0 && errno == EINTR && !startup_terminate);
    sigprocmask(SIG_BLOCK, &controls, NULL);
    close(execution_pipe[0]);
    if (execution_result != 0 && !startup_terminate) {
        while (waitpid(target, &target_status, 0) < 0 && errno == EINTR) {}
        report_startup(execution_result == (ssize_t)sizeof(execution_error) ? execution_error : EIO, -1, true);
        return 125;
    }
    if (startup_terminate) {
        close(STARTUP_FD);
    } else if (!report_startup(0, target, true)) {
        startup_terminate = 1;
    }

    bool target_exited = false;
    bool children_remain = true;
    bool terminating = startup_terminate;
    bool force = startup_force;
    target_status = 0;
    long long force_at = terminating ? monotonic_milliseconds() + grace_milliseconds : 0;
    while (!target_exited || children_remain) {
        if (!reap_available(target, &target_exited, &target_status, &children_remain)) {
            fprintf(stderr, "Failed to reap target processes: %s\n", strerror(errno));
            return 125;
        }
        if (target_exited && !terminating && children_remain) {
            terminating = true;
            force_at = monotonic_milliseconds() + grace_milliseconds;
        }
        if (terminating && !force && monotonic_milliseconds() >= force_at) force = true;
        if (terminating) signal_owned_tree(target, target_exited, force ? SIGKILL : SIGTERM);
        if (target_exited && !children_remain) break;

        struct timespec wait = {.tv_sec = 0, .tv_nsec = 25 * 1000 * 1000};
        int signal_number = sigtimedwait(&controls, NULL, &wait);
        if (signal_number == SIGUSR1) {
            terminating = true;
            force = true;
        } else if (signal_number == SIGTERM || signal_number == SIGINT || signal_number == SIGHUP) {
            if (!terminating) {
                terminating = true;
                force_at = monotonic_milliseconds() + grace_milliseconds;
            }
        } else if (signal_number < 0 && errno != EAGAIN && errno != EINTR) {
            fprintf(stderr, "Failed to wait for target processes: %s\n", strerror(errno));
            return 125;
        }
    }
    return return_target_status(target_status);
}
