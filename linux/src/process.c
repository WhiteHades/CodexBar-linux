#define _GNU_SOURCE

#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef enum {
    STOP_NONE,
    STOP_CANCELLED,
    STOP_TIMED_OUT,
    STOP_OUTPUT_LIMIT,
    STOP_INTERNAL_ERROR,
} StopReason;

typedef struct {
    uint32_t working_directory_length;
    uint32_t environment_count;
} ProcessConfiguration;

typedef struct {
    int32_t error_code;
    int32_t target_pid;
} ProcessStartup;

enum {
    MAX_CONFIGURATION_BYTES = 16 * 1024 * 1024,
};

static void close_descriptor(int *descriptor) {
    if (*descriptor < 0) return;
    close(*descriptor);
    *descriptor = -1;
}

static void set_setup_error(GError **error, const char *operation, int code) {
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(code),
                "Failed to configure process %s: %s",
                operation,
                g_strerror(code));
}

static void set_spawn_error(GError **error, const char *program, int code) {
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(code),
                "Failed to start process %s: %s",
                program,
                g_strerror(code));
}

static gboolean set_nonblocking(int descriptor, GError **error) {
    int flags = fcntl(descriptor, F_GETFL);
    if (flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0) return TRUE;
    int code = errno;
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(code),
                "Failed to configure process pipe: %s",
                g_strerror(code));
    return FALSE;
}

static int duplicate_child_descriptor(int descriptor, GError **error) {
    int duplicate = fcntl(descriptor, F_DUPFD_CLOEXEC, 6);
    if (duplicate >= 0) return duplicate;
    int code = errno;
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(code),
                "Failed to isolate process descriptor: %s",
                g_strerror(code));
    return -1;
}

static gboolean wait_for_io(
    int descriptor, gushort events, GCancellable *cancellable, gint64 deadline, GError **error) {
    while (TRUE) {
        if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) return FALSE;
        gint64 now = g_get_monotonic_time();
        if (deadline != G_MAXINT64 && now >= deadline) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Process timed out during startup");
            return FALSE;
        }
        GPollFD cancellation_poll = {.fd = -1};
        gboolean has_cancellation_poll = cancellable && g_cancellable_make_pollfd(cancellable, &cancellation_poll);
        GPollFD poll_descriptors[2] = {
            {.fd = descriptor, .events = events | G_IO_HUP | G_IO_ERR},
            cancellation_poll,
        };
        gint timeout = -1;
        if (deadline != G_MAXINT64) {
            gint64 remaining_us = MAX((gint64)0, deadline - now);
            timeout = (gint)MIN((gint64)G_MAXINT, (remaining_us + 999) / 1000);
        }
        int result;
        do {
            result = g_poll(poll_descriptors, has_cancellation_poll ? 2 : 1, timeout);
        } while (result < 0 && errno == EINTR);
        if (has_cancellation_poll) g_cancellable_release_fd(cancellable);
        if (result > 0 && (poll_descriptors[0].revents & (events | G_IO_HUP | G_IO_ERR))) return TRUE;
        if (result == 0) continue;
        if (result < 0) {
            int code = errno;
            g_set_error(error,
                        G_IO_ERROR,
                        g_io_error_from_errno(code),
                        "Failed to wait for process startup: %s",
                        g_strerror(code));
            return FALSE;
        }
    }
}

static gboolean write_all(int descriptor,
                          const void *data,
                          size_t length,
                          GCancellable *cancellable,
                          gint64 deadline,
                          GError **error) {
    const guint8 *bytes = data;
    size_t written = 0;
    while (written < length) {
        if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) return FALSE;
        if (deadline != G_MAXINT64 && g_get_monotonic_time() >= deadline) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Process timed out during startup");
            return FALSE;
        }
        ssize_t count = write(descriptor, bytes + written, length - written);
        if (count > 0) {
            written += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_for_io(descriptor, G_IO_OUT, cancellable, deadline, error)) return FALSE;
        } else {
            int code = count < 0 ? errno : EIO;
            g_set_error(error,
                        G_IO_ERROR,
                        g_io_error_from_errno(code),
                        "Failed to configure process supervisor: %s",
                        g_strerror(code));
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean read_all(int descriptor,
                         void *data,
                         size_t length,
                         GCancellable *cancellable,
                         gint64 deadline,
                         GError **error) {
    guint8 *bytes = data;
    size_t received = 0;
    while (received < length) {
        if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) return FALSE;
        if (deadline != G_MAXINT64 && g_get_monotonic_time() >= deadline) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Process timed out during startup");
            return FALSE;
        }
        ssize_t count = read(descriptor, bytes + received, length - received);
        if (count > 0) {
            received += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_for_io(descriptor, G_IO_IN, cancellable, deadline, error)) return FALSE;
        } else {
            int code = count < 0 ? errno : EIO;
            g_set_error(error,
                        G_IO_ERROR,
                        g_io_error_from_errno(code),
                        "Failed to start process supervisor: %s",
                        g_strerror(code));
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean send_configuration(int descriptor,
                                   const CodexBarProcessRequest *request,
                                   uint32_t environment_count,
                                   GCancellable *cancellable,
                                   gint64 deadline,
                                   GError **error) {
    ProcessConfiguration configuration = {
        .working_directory_length = request->working_directory ? (uint32_t)strlen(request->working_directory) : UINT32_MAX,
        .environment_count = request->environment ? environment_count : UINT32_MAX,
    };
    sigset_t blocked;
    sigset_t previous;
    sigset_t pending_before;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    sigpending(&pending_before);
    gboolean sent = write_all(descriptor, &configuration, sizeof(configuration), cancellable, deadline, error);
    if (sent && configuration.working_directory_length != UINT32_MAX) {
        sent = write_all(
            descriptor, request->working_directory, configuration.working_directory_length, cancellable, deadline, error);
    }
    for (uint32_t index = 0; sent && index < environment_count; index++) {
        uint32_t length = (uint32_t)strlen(request->environment[index]);
        sent = write_all(descriptor, &length, sizeof(length), cancellable, deadline, error) &&
               write_all(descriptor, request->environment[index], length, cancellable, deadline, error);
    }
    gboolean generated_sigpipe = !sent && error && *error && g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE);
    if (generated_sigpipe && !sigismember(&pending_before, SIGPIPE)) {
        struct timespec no_wait = {0};
        while (sigtimedwait(&blocked, NULL, &no_wait) < 0 && errno == EINTR) {
        }
    }
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return sent;
}

static gboolean send_acknowledgement(
    int descriptor, GCancellable *cancellable, gint64 deadline, GError **error) {
    sigset_t blocked;
    sigset_t previous;
    sigset_t pending_before;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    sigpending(&pending_before);
    const char acknowledgement = '1';
    gboolean sent = write_all(descriptor, &acknowledgement, 1, cancellable, deadline, error);
    gboolean generated_sigpipe = !sent && error && *error && g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE);
    if (generated_sigpipe && !sigismember(&pending_before, SIGPIPE)) {
        struct timespec no_wait = {0};
        while (sigtimedwait(&blocked, NULL, &no_wait) < 0 && errno == EINTR) {}
    }
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return sent;
}

static char *process_supervisor_path(GError **error) {
    size_t size = 256;
    while (size <= 64U * 1024U) {
        char *executable = g_malloc(size);
        ssize_t length = readlink("/proc/self/exe", executable, size - 1);
        if (length < 0) {
            int code = errno;
            g_free(executable);
            g_set_error(error,
                        G_IO_ERROR,
                        g_io_error_from_errno(code),
                        "Failed to locate process supervisor: %s",
                        g_strerror(code));
            return NULL;
        }
        if ((size_t)length < size - 1) {
            executable[length] = '\0';
            char *directory = g_path_get_dirname(executable);
            char *supervisor = g_build_filename(directory, "codexbar-process-supervisor", NULL);
            g_free(directory);
            g_free(executable);
            return supervisor;
        }
        g_free(executable);
        size *= 2;
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FILENAME_TOO_LONG, "Executable path is too long");
    return NULL;
}

static gboolean observe_child(pid_t pid, gboolean *exited, gboolean *signaled, GError **error) {
    if (*exited) return TRUE;
    siginfo_t information = {0};
    int result;
    do {
        result = waitid(P_PID, (id_t)pid, &information, WEXITED | WNOHANG | WNOWAIT);
    } while (result < 0 && errno == EINTR);
    if (result == 0 && information.si_pid == pid) {
        *exited = TRUE;
        *signaled = information.si_code == CLD_KILLED || information.si_code == CLD_DUMPED;
        return TRUE;
    }
    if (result == 0) return TRUE;
    int code = errno;
    if (!error || !*error) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(code), "Failed to observe process: %s", g_strerror(code));
    }
    return FALSE;
}

static gboolean reap_child(pid_t pid, int *status, GError **error) {
    pid_t result;
    do {
        result = waitpid(pid, status, 0);
    } while (result < 0 && errno == EINTR);
    if (result == pid) return TRUE;
    int code = errno;
    if (!error || !*error) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(code), "Failed to reap process: %s", g_strerror(code));
    }
    return FALSE;
}

static void abort_startup(pid_t pid) {
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

static gpointer reap_children(gpointer data) {
    GAsyncQueue *queue = data;
    GArray *children = g_array_new(FALSE, FALSE, sizeof(pid_t));
    while (TRUE) {
        gpointer item = children->len == 0 ? g_async_queue_pop(queue) : g_async_queue_try_pop(queue);
        while (item) {
            pid_t pid = (pid_t)GPOINTER_TO_INT(item);
            g_array_append_val(children, pid);
            item = g_async_queue_try_pop(queue);
        }
        for (guint index = children->len; index > 0; index--) {
            pid_t pid = g_array_index(children, pid_t, index - 1);
            int status;
            pid_t result;
            do {
                result = waitpid(pid, &status, WNOHANG);
            } while (result < 0 && errno == EINTR);
            if (result == pid || (result < 0 && errno == ECHILD)) g_array_remove_index_fast(children, index - 1);
        }
        if (children->len > 0) g_usleep(100 * 1000);
    }
    return NULL;
}

static gpointer create_reaper(gpointer data) {
    (void)data;
    GAsyncQueue *queue = g_async_queue_new();
    GThread *thread = g_thread_new("process-reaper", reap_children, queue);
    g_thread_unref(thread);
    return queue;
}

static void reap_child_later(pid_t pid) {
    static GOnce reaper = G_ONCE_INIT;
    GAsyncQueue *queue = g_once(&reaper, create_reaper, NULL);
    g_async_queue_push(queue, GINT_TO_POINTER((int)pid));
}

static gboolean child_has_exited(pid_t pid) {
    siginfo_t information = {0};
    int result;
    do {
        result = waitid(P_PID, (id_t)pid, &information, WEXITED | WNOHANG | WNOWAIT);
    } while (result < 0 && errno == EINTR);
    return result == 0 && information.si_pid == pid;
}

static void cancel_started_supervisor(pid_t pid, guint grace_milliseconds) {
    kill(-pid, SIGTERM);
    gint64 deadline = g_get_monotonic_time() + (gint64)grace_milliseconds * G_TIME_SPAN_MILLISECOND;
    while (!child_has_exited(pid) && g_get_monotonic_time() < deadline) g_usleep(10 * 1000);
    if (!child_has_exited(pid)) {
        kill(pid, SIGUSR1);
        deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
        while (!child_has_exited(pid) && g_get_monotonic_time() < deadline) g_usleep(10 * 1000);
    }
    if (child_has_exited(pid)) {
        kill(-pid, SIGKILL);
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    } else {
        reap_child_later(pid);
    }
}

static int open_process_handle(pid_t pid) {
#ifdef SYS_pidfd_open
    return (int)syscall(SYS_pidfd_open, pid, 0);
#else
    (void)pid;
    errno = ENOSYS;
    return -1;
#endif
}

static gboolean process_handle_is_alive(int descriptor) {
#ifdef SYS_pidfd_send_signal
    return descriptor >= 0 && syscall(SYS_pidfd_send_signal, descriptor, 0, NULL, 0) == 0;
#else
    (void)descriptor;
    return FALSE;
#endif
}

static void signal_fallback_target(int descriptor, pid_t supervisor_pid, int signal_number) {
    kill(-supervisor_pid, signal_number);
#ifdef SYS_pidfd_send_signal
    if (descriptor >= 0) syscall(SYS_pidfd_send_signal, descriptor, signal_number, NULL, 0);
#endif
}

static gboolean drain_pipe(int *descriptor,
                           GByteArray *output,
                           size_t *captured,
                           size_t maximum,
                           gboolean keep_output,
                           gboolean *overflow,
                           GError **error) {
    if (*descriptor < 0) return TRUE;
    guint8 chunk[8192];
    size_t budget = 64U * 1024U;
    while (budget > 0) {
        ssize_t count = read(*descriptor, chunk, sizeof(chunk));
        if (count > 0) {
            size_t bytes = (size_t)count;
            budget -= MIN(budget, bytes);
            if (keep_output && !*overflow) {
                if (bytes > maximum - MIN(maximum, *captured)) {
                    *overflow = TRUE;
                } else {
                    g_byte_array_append(output, chunk, (guint)bytes);
                    *captured += bytes;
                }
            }
            continue;
        }
        if (count == 0) {
            close_descriptor(descriptor);
            return TRUE;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return TRUE;
        int code = errno;
        close_descriptor(descriptor);
        if (!error || !*error) {
            g_set_error(
                error, G_IO_ERROR, g_io_error_from_errno(code), "Failed to read process output: %s", g_strerror(code));
        }
        return FALSE;
    }
    return TRUE;
}

static char *finish_output(GByteArray *output, size_t *length) {
    *length = output->len;
    g_byte_array_append(output, (const guint8 *)"", 1);
    return (char *)g_byte_array_free(output, FALSE);
}

CodexBarProcessResult *codexbar_process_run(
    const CodexBarProcessRequest *request, GCancellable *cancellable, GError **error) {
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    if (!request || !request->arguments || !request->arguments[0] || request->arguments[0][0] == '\0' ||
        request->maximum_output_bytes == 0 || request->maximum_output_bytes >= G_MAXUINT ||
        request->termination_grace_milliseconds > 60000 ||
        (request->working_directory && strlen(request->working_directory) > UINT32_MAX)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Process request is invalid");
        return NULL;
    }
    uint32_t environment_count = 0;
    size_t configuration_bytes = request->working_directory ? strlen(request->working_directory) : 0;
    if (request->environment) {
        while (request->environment[environment_count]) {
            size_t length = strlen(request->environment[environment_count]);
            if (length > UINT32_MAX || environment_count == UINT32_MAX - 1 ||
                length > MAX_CONFIGURATION_BYTES - MIN((size_t)MAX_CONFIGURATION_BYTES, configuration_bytes) ||
                sizeof(uint32_t) >
                    MAX_CONFIGURATION_BYTES - MIN((size_t)MAX_CONFIGURATION_BYTES, configuration_bytes + length)) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Process environment is invalid");
                return NULL;
            }
            configuration_bytes += sizeof(uint32_t) + length;
            environment_count++;
        }
    }
    if (configuration_bytes > MAX_CONFIGURATION_BYTES) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Process configuration is too large");
        return NULL;
    }
    if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) return NULL;
    gint64 started_at = g_get_monotonic_time();
    gint64 timeout_at = request->timeout_milliseconds > 0
                              ? started_at + (gint64)request->timeout_milliseconds * G_TIME_SPAN_MILLISECOND
                              : G_MAXINT64;

    char *supervisor = process_supervisor_path(error);
    if (!supervisor) return NULL;
    size_t argument_count = 0;
    while (request->arguments[argument_count]) argument_count++;
    char *expected_parent = g_strdup_printf("%ld", (long)getpid());
    char *grace = g_strdup_printf("%u", request->termination_grace_milliseconds);
    char **supervisor_arguments = g_new0(char *, argument_count + 6);
    supervisor_arguments[0] = supervisor;
    supervisor_arguments[1] = expected_parent;
    supervisor_arguments[2] = request->new_session ? "--session" : "--group";
    supervisor_arguments[3] = grace;
    supervisor_arguments[4] = "--";
    for (size_t index = 0; index < argument_count; index++) {
        supervisor_arguments[index + 5] = (char *)request->arguments[index];
    }

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    int configuration_pipe[2] = {-1, -1};
    int startup_pipe[2] = {-1, -1};
    int acknowledgement_pipe[2] = {-1, -1};
    int child_stdout = -1;
    int child_stderr = -1;
    int child_configuration = -1;
    int child_startup = -1;
    int child_acknowledgement = -1;
    if (pipe2(stdout_pipe, O_CLOEXEC) < 0 || pipe2(stderr_pipe, O_CLOEXEC) < 0 ||
        pipe2(configuration_pipe, O_CLOEXEC) < 0 || pipe2(startup_pipe, O_CLOEXEC) < 0 ||
        pipe2(acknowledgement_pipe, O_CLOEXEC) < 0) {
        int code = errno;
        close_descriptor(&stdout_pipe[0]);
        close_descriptor(&stdout_pipe[1]);
        close_descriptor(&stderr_pipe[0]);
        close_descriptor(&stderr_pipe[1]);
        close_descriptor(&configuration_pipe[0]);
        close_descriptor(&configuration_pipe[1]);
        close_descriptor(&startup_pipe[0]);
        close_descriptor(&startup_pipe[1]);
        close_descriptor(&acknowledgement_pipe[0]);
        close_descriptor(&acknowledgement_pipe[1]);
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(code), "Failed to create process pipes: %s", g_strerror(code));
        goto arguments_failed;
    }
    if (!set_nonblocking(stdout_pipe[0], error) || !set_nonblocking(stderr_pipe[0], error) ||
        !set_nonblocking(configuration_pipe[1], error) || !set_nonblocking(startup_pipe[0], error) ||
        !set_nonblocking(acknowledgement_pipe[1], error)) {
        goto setup_failed;
    }
    child_stdout = duplicate_child_descriptor(stdout_pipe[1], error);
    if (child_stdout >= 0) child_stderr = duplicate_child_descriptor(stderr_pipe[1], error);
    if (child_stderr >= 0) child_configuration = duplicate_child_descriptor(configuration_pipe[0], error);
    if (child_configuration >= 0) child_startup = duplicate_child_descriptor(startup_pipe[1], error);
    if (child_startup >= 0) child_acknowledgement = duplicate_child_descriptor(acknowledgement_pipe[0], error);
    if (child_acknowledgement < 0) goto setup_failed;

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    gboolean actions_initialized = FALSE;
    gboolean attributes_initialized = FALSE;
    int code = posix_spawn_file_actions_init(&actions);
    if (code != 0) {
        set_setup_error(error, "file actions", code);
        goto setup_failed;
    }
    actions_initialized = TRUE;
    code = posix_spawnattr_init(&attributes);
    if (code != 0) {
        set_setup_error(error, "attributes", code);
        goto spawn_setup_failed;
    }
    attributes_initialized = TRUE;

    code = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (code == 0) code = posix_spawn_file_actions_adddup2(&actions, child_stdout, STDOUT_FILENO);
    if (code == 0) code = posix_spawn_file_actions_adddup2(&actions, child_stderr, STDERR_FILENO);
    if (code == 0) code = posix_spawn_file_actions_adddup2(&actions, child_configuration, 3);
    if (code == 0) code = posix_spawn_file_actions_adddup2(&actions, child_startup, 4);
    if (code == 0) code = posix_spawn_file_actions_adddup2(&actions, child_acknowledgement, 5);
    if (code == 0) code = posix_spawn_file_actions_addclosefrom_np(&actions, 6);
    if (code != 0) {
        set_setup_error(error, "file descriptors", code);
        goto spawn_setup_failed;
    }

    sigset_t empty_mask;
    sigset_t default_signals;
    sigemptyset(&empty_mask);
    sigfillset(&default_signals);
    sigdelset(&default_signals, SIGKILL);
    sigdelset(&default_signals, SIGSTOP);
    code = posix_spawnattr_setsigmask(&attributes, &empty_mask);
    if (code == 0) code = posix_spawnattr_setsigdefault(&attributes, &default_signals);
    short spawn_flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (request->new_session) {
        spawn_flags |= POSIX_SPAWN_SETSID;
    } else {
        spawn_flags |= POSIX_SPAWN_SETPGROUP;
        if (code == 0) code = posix_spawnattr_setpgroup(&attributes, 0);
    }
    if (code == 0) code = posix_spawnattr_setflags(&attributes, spawn_flags);
    if (code != 0) {
        set_setup_error(error, "attributes", code);
        goto spawn_setup_failed;
    }

    pid_t pid = -1;
    code = posix_spawn(&pid,
                       supervisor,
                       &actions,
                       &attributes,
                       supervisor_arguments,
                       environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close_descriptor(&stdout_pipe[1]);
    close_descriptor(&stderr_pipe[1]);
    close_descriptor(&configuration_pipe[0]);
    close_descriptor(&startup_pipe[1]);
    close_descriptor(&acknowledgement_pipe[0]);
    close_descriptor(&child_stdout);
    close_descriptor(&child_stderr);
    close_descriptor(&child_configuration);
    close_descriptor(&child_startup);
    close_descriptor(&child_acknowledgement);
    g_free(supervisor_arguments);
    g_free(expected_parent);
    g_free(grace);
    g_free(supervisor);
    if (code != 0) {
        set_spawn_error(error, "codexbar-process-supervisor", code);
        close_descriptor(&stdout_pipe[0]);
        close_descriptor(&stderr_pipe[0]);
        close_descriptor(&configuration_pipe[1]);
        close_descriptor(&startup_pipe[0]);
        close_descriptor(&acknowledgement_pipe[1]);
        return NULL;
    }

    gboolean configured = send_configuration(
        configuration_pipe[1], request, environment_count, cancellable, timeout_at, error);
    close_descriptor(&configuration_pipe[1]);
    ProcessStartup startup = {0};
    gboolean started = configured && read_all(startup_pipe[0], &startup, sizeof(startup), cancellable, timeout_at, error);
    if (!started || startup.error_code != 0 || startup.target_pid <= 0) {
        close_descriptor(&startup_pipe[0]);
        close_descriptor(&acknowledgement_pipe[1]);
        abort_startup(pid);
        close_descriptor(&stdout_pipe[0]);
        close_descriptor(&stderr_pipe[0]);
        if (started && startup.error_code != 0) {
            set_spawn_error(error, request->arguments[0], startup.error_code);
        } else if (started && (!error || !*error)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Process supervisor returned invalid startup data");
        }
        return NULL;
    }
    pid_t target_pid = (pid_t)startup.target_pid;
    int target_handle = open_process_handle(target_pid);
    if (target_handle < 0) {
        int handle_code = errno;
        close_descriptor(&startup_pipe[0]);
        close_descriptor(&acknowledgement_pipe[1]);
        abort_startup(pid);
        close_descriptor(&stdout_pipe[0]);
        close_descriptor(&stderr_pipe[0]);
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(handle_code),
                    "Failed to pin process identity: %s",
                    g_strerror(handle_code));
        return NULL;
    }
    gboolean acknowledged = send_acknowledgement(acknowledgement_pipe[1], cancellable, timeout_at, error);
    close_descriptor(&acknowledgement_pipe[1]);
    ProcessStartup execution = {0};
    gboolean executed =
        acknowledged && read_all(startup_pipe[0], &execution, sizeof(execution), cancellable, timeout_at, error);
    close_descriptor(&startup_pipe[0]);
    if (!executed || execution.error_code != 0 || execution.target_pid != startup.target_pid) {
        if (!executed) {
            if (acknowledged) {
                cancel_started_supervisor(pid, request->termination_grace_milliseconds);
            } else {
                abort_startup(pid);
            }
        } else {
            int ignored_status;
            while (waitpid(pid, &ignored_status, 0) < 0 && errno == EINTR) {}
        }
        close_descriptor(&target_handle);
        close_descriptor(&stdout_pipe[0]);
        close_descriptor(&stderr_pipe[0]);
        if (executed && execution.error_code != 0) {
            set_spawn_error(error, request->arguments[0], execution.error_code);
        } else if (executed && (!error || !*error)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Process supervisor returned invalid execution data");
        }
        return NULL;
    }

    GByteArray *standard_output = g_byte_array_sized_new(4096);
    GByteArray *standard_error = g_byte_array_sized_new(4096);
    GPollFD cancellation_poll = {.fd = -1};
    gboolean has_cancellation_poll = cancellable && g_cancellable_make_pollfd(cancellable, &cancellation_poll);
    gboolean output_overflow = FALSE;
    gboolean supervisor_exited = FALSE;
    gboolean supervisor_signaled = FALSE;
    gboolean terminating = FALSE;
    gboolean force_requested = FALSE;
    gboolean reaping_later = FALSE;
    size_t captured = 0;
    int wait_status = 0;
    StopReason reason = STOP_NONE;
    gint64 terminate_at = G_MAXINT64;
    gint64 cleanup_deadline = G_MAXINT64;

    while (TRUE) {
        gboolean keep_output = reason == STOP_NONE;
        if (!drain_pipe(&stdout_pipe[0],
                        standard_output,
                        &captured,
                        request->maximum_output_bytes,
                        keep_output,
                        &output_overflow,
                        error) ||
            !drain_pipe(&stderr_pipe[0],
                        standard_error,
                        &captured,
                        request->maximum_output_bytes,
                        keep_output,
                        &output_overflow,
                        error) ||
            !observe_child(pid, &supervisor_exited, &supervisor_signaled, error)) {
            if (reason == STOP_NONE) reason = STOP_INTERNAL_ERROR;
        }

        if (supervisor_exited &&
            (!drain_pipe(&stdout_pipe[0],
                         standard_output,
                         &captured,
                         request->maximum_output_bytes,
                         reason == STOP_NONE,
                         &output_overflow,
                         error) ||
             !drain_pipe(&stderr_pipe[0],
                         standard_error,
                         &captured,
                         request->maximum_output_bytes,
                         reason == STOP_NONE,
                         &output_overflow,
                         error))) {
            if (reason == STOP_NONE) reason = STOP_INTERNAL_ERROR;
        }

        gint64 now = g_get_monotonic_time();
        if (reason == STOP_NONE && supervisor_signaled && (stdout_pipe[0] >= 0 || stderr_pipe[0] >= 0)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Process supervisor exited before its target tree");
            reason = STOP_INTERNAL_ERROR;
        }
        if (reason == STOP_NONE && supervisor_exited && process_handle_is_alive(target_handle)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Process supervisor exited before its target");
            reason = STOP_INTERNAL_ERROR;
        }
        if (reason == STOP_NONE && output_overflow) reason = STOP_OUTPUT_LIMIT;
        if (reason == STOP_NONE && cancellable && g_cancellable_is_cancelled(cancellable)) reason = STOP_CANCELLED;
        if (reason == STOP_NONE && now >= timeout_at) reason = STOP_TIMED_OUT;

        if (!terminating && reason != STOP_NONE) {
            if (supervisor_exited) {
                signal_fallback_target(target_handle, pid, SIGTERM);
            } else {
                kill(pid, SIGTERM);
            }
            terminating = TRUE;
            terminate_at = now + (gint64)request->termination_grace_milliseconds * G_TIME_SPAN_MILLISECOND;
        }
        if (terminating && !force_requested && now >= terminate_at) {
            if (supervisor_exited) {
                signal_fallback_target(target_handle, pid, SIGKILL);
            } else {
                kill(pid, SIGUSR1);
            }
            force_requested = TRUE;
            cleanup_deadline = now + G_TIME_SPAN_SECOND;
        }
        if (supervisor_exited && stdout_pipe[0] < 0 && stderr_pipe[0] < 0) break;
        if (force_requested && now >= cleanup_deadline) {
            if (!supervisor_exited) {
                reap_child_later(pid);
                reaping_later = TRUE;
            }
            break;
        }

        GPollFD poll_descriptors[3] = {
            {.fd = stdout_pipe[0], .events = G_IO_IN | G_IO_HUP | G_IO_ERR},
            {.fd = stderr_pipe[0], .events = G_IO_IN | G_IO_HUP | G_IO_ERR},
            cancellation_poll,
        };
        gint64 next_event_at = reason == STOP_NONE ? timeout_at : G_MAXINT64;
        if (terminating && !force_requested) next_event_at = MIN(next_event_at, terminate_at);
        if (force_requested) next_event_at = MIN(next_event_at, cleanup_deadline);
        gint poll_timeout = 25;
        if (next_event_at != G_MAXINT64) {
            gint64 remaining_us = MAX((gint64)0, next_event_at - now);
            poll_timeout = MIN(poll_timeout, (gint)((remaining_us + 999) / 1000));
        }
        int poll_result;
        do {
            poll_result = g_poll(poll_descriptors, has_cancellation_poll ? 3 : 2, poll_timeout);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0 && reason == STOP_NONE) {
            int poll_code = errno;
            g_set_error(error,
                        G_IO_ERROR,
                        g_io_error_from_errno(poll_code),
                        "Failed to wait for process output: %s",
                        g_strerror(poll_code));
            reason = STOP_INTERNAL_ERROR;
        }
    }

    close_descriptor(&stdout_pipe[0]);
    close_descriptor(&stderr_pipe[0]);
    close_descriptor(&target_handle);
    if (has_cancellation_poll) g_cancellable_release_fd(cancellable);
    if (!reaping_later && !reap_child(pid, &wait_status, error)) reason = STOP_INTERNAL_ERROR;

    if (reason != STOP_NONE) {
        g_byte_array_unref(standard_output);
        g_byte_array_unref(standard_error);
        if (error && *error) return NULL;
        switch (reason) {
        case STOP_CANCELLED:
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Process execution was cancelled");
            break;
        case STOP_TIMED_OUT:
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_TIMED_OUT,
                        "Process timed out after %u ms",
                        request->timeout_milliseconds);
            break;
        case STOP_OUTPUT_LIMIT:
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_MESSAGE_TOO_LARGE,
                        "Process output exceeded %zu bytes",
                        request->maximum_output_bytes);
            break;
        case STOP_INTERNAL_ERROR:
            if (!error || !*error) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Process execution failed");
            }
            break;
        case STOP_NONE: break;
        }
        return NULL;
    }

    CodexBarProcessResult *result = g_new0(CodexBarProcessResult, 1);
    result->standard_output = finish_output(standard_output, &result->standard_output_length);
    result->standard_error = finish_output(standard_error, &result->standard_error_length);
    result->exit_status = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
    result->termination_signal = WIFSIGNALED(wait_status) ? WTERMSIG(wait_status) : 0;
    return result;

spawn_setup_failed:
    if (attributes_initialized) posix_spawnattr_destroy(&attributes);
    if (actions_initialized) posix_spawn_file_actions_destroy(&actions);
setup_failed:
    close_descriptor(&stdout_pipe[0]);
    close_descriptor(&stdout_pipe[1]);
    close_descriptor(&stderr_pipe[0]);
    close_descriptor(&stderr_pipe[1]);
    close_descriptor(&configuration_pipe[0]);
    close_descriptor(&configuration_pipe[1]);
    close_descriptor(&startup_pipe[0]);
    close_descriptor(&startup_pipe[1]);
    close_descriptor(&acknowledgement_pipe[0]);
    close_descriptor(&acknowledgement_pipe[1]);
    close_descriptor(&child_stdout);
    close_descriptor(&child_stderr);
    close_descriptor(&child_configuration);
    close_descriptor(&child_startup);
    close_descriptor(&child_acknowledgement);
arguments_failed:
    g_free(supervisor_arguments);
    g_free(expected_parent);
    g_free(grace);
    g_free(supervisor);
    return NULL;
}

gboolean codexbar_process_result_succeeded(const CodexBarProcessResult *result) {
    return result && result->exit_status == 0 && result->termination_signal == 0;
}

void codexbar_process_result_free(CodexBarProcessResult *result) {
    if (!result) return;
    g_free(result->standard_output);
    g_free(result->standard_error);
    g_free(result);
}
