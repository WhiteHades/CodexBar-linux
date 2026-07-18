#define _GNU_SOURCE

#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *test_program;
static volatile sig_atomic_t helper_stop;

typedef struct {
    CodexBarProcessRequest request;
    GCancellable *cancellable;
    CodexBarProcessResult *result;
    GError *error;
} ThreadedRun;

static void stop_helper(int signal_number) {
    (void)signal_number;
    helper_stop = 1;
}

static void wait_for_helper_stop(void) {
    sigset_t signals;
    sigset_t previous;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGHUP);
    sigprocmask(SIG_BLOCK, &signals, &previous);
    while (!helper_stop) sigsuspend(&previous);
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

static int helper_capture(void) {
    fputs("standard output", stdout);
    fputs("standard error", stderr);
    return 7;
}

static int helper_inspect(const char *expected_directory, const char *descriptor_text) {
    char directory[PATH_MAX];
    int descriptor = atoi(descriptor_text);
    sigset_t mask;
    struct sigaction pipe_action;
    if (!getcwd(directory, sizeof(directory)) || !g_str_equal(directory, expected_directory)) return 10;
    if (getsid(0) != getppid() || getpgrp() != getppid()) return 11;
    errno = 0;
    if (fcntl(descriptor, F_GETFD) >= 0 || errno != EBADF) return 12;
    if (sigprocmask(SIG_SETMASK, NULL, &mask) < 0 || sigismember(&mask, SIGTERM)) return 13;
    if (sigaction(SIGPIPE, NULL, &pipe_action) < 0 || pipe_action.sa_handler != SIG_DFL) return 14;
    fputs("ok", stdout);
    return 0;
}

static int helper_touch(const char *path) {
    return g_file_set_contents(path, "spawned", -1, NULL) ? 0 : 1;
}

static int helper_environment(const char *expected_directory) {
    char directory[PATH_MAX];
    const char *value = g_getenv("CODEXBAR_TEST_ENVIRONMENT");
    if (!getcwd(directory, sizeof(directory)) || !g_str_equal(directory, expected_directory)) return 1;
    return value && g_str_equal(value, "isolated") ? 0 : 1;
}

static int helper_short_overflow(void) {
    char output[4096];
    memset(output, 'x', sizeof(output));
    size_t written = 0;
    while (written < sizeof(output)) {
        ssize_t count = write(STDOUT_FILENO, output + written, sizeof(output) - written);
        if (count > 0) {
            written += (size_t)count;
        } else if (count < 0 && errno != EINTR) {
            return 1;
        }
    }
    return 0;
}

static int helper_combined_overflow(void) {
    char output[400];
    memset(output, 'x', sizeof(output));
    if (write(STDOUT_FILENO, output, sizeof(output)) != (ssize_t)sizeof(output)) return 1;
    if (write(STDERR_FILENO, output, sizeof(output)) != (ssize_t)sizeof(output)) return 1;
    return 0;
}

static int helper_double_fork(const char *ready_path, const char *stopped_path) {
    int channel[2];
    if (pipe(channel) != 0) return 1;
    pid_t intermediate = fork();
    if (intermediate < 0) return 1;
    if (intermediate == 0) {
        close(channel[0]);
        pid_t child = fork();
        if (child < 0) _exit(1);
        if (child == 0) {
            if (setsid() < 0) _exit(1);
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            helper_stop = 0;
            struct sigaction action = {.sa_handler = stop_helper};
            sigemptyset(&action.sa_mask);
            if (sigaction(SIGTERM, &action, NULL) < 0 || sigaction(SIGINT, &action, NULL) < 0 ||
                sigaction(SIGHUP, &action, NULL) < 0) {
                _exit(1);
            }
            pid_t own_pid = getpid();
            if (write(channel[1], &own_pid, sizeof(own_pid)) != (ssize_t)sizeof(own_pid)) _exit(1);
            close(channel[1]);
            wait_for_helper_stop();
            g_file_set_contents(stopped_path, "stopped", -1, NULL);
            _exit(0);
        }
        close(channel[1]);
        _exit(0);
    }

    close(channel[1]);
    pid_t child = 0;
    ssize_t received;
    do {
        received = read(channel[0], &child, sizeof(child));
    } while (received < 0 && errno == EINTR);
    close(channel[0]);
    int status = 0;
    while (waitpid(intermediate, &status, 0) < 0 && errno == EINTR) {}
    if (received != (ssize_t)sizeof(child) || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 1;
    char *ready = g_strdup_printf("%ld %ld", (long)getpid(), (long)child);
    gboolean wrote_ready = g_file_set_contents(ready_path, ready, -1, NULL);
    g_free(ready);
    return wrote_ready ? 0 : 1;
}

static int helper_group(const char *mode, const char *ready_path, const char *stopped_path) {
    helper_stop = 0;
    struct sigaction action = {.sa_handler = g_str_equal(mode, "resist") ? SIG_IGN : stop_helper};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) < 0 || sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGHUP, &action, NULL) < 0) {
        return 20;
    }
    int ready_pipe[2];
    if (pipe2(ready_pipe, O_CLOEXEC) < 0) return 21;
    pid_t child = fork();
    if (child < 0) return 22;
    if (child == 0) {
        close(ready_pipe[0]);
        if (g_str_equal(mode, "escape") && setsid() < 0) _exit(28);
        char ready = '1';
        if (write(ready_pipe[1], &ready, 1) != 1) _exit(23);
        close(ready_pipe[1]);
        wait_for_helper_stop();
        _exit(g_file_set_contents(stopped_path, "stopped", -1, NULL) ? 0 : 24);
    }

    close(ready_pipe[1]);
    char ready = 0;
    ssize_t ready_count;
    do {
        ready_count = read(ready_pipe[0], &ready, 1);
    } while (ready_count < 0 && errno == EINTR);
    close(ready_pipe[0]);
    if (ready_count != 1 || ready != '1') return 25;
    char *pids = g_strdup_printf("%d %d", getpid(), child);
    gboolean wrote_pids = g_file_set_contents(ready_path, pids, -1, NULL);
    g_free(pids);
    if (!wrote_pids) return 26;
    if (g_str_equal(mode, "natural") || g_str_equal(mode, "escape")) return 0;
    if (g_str_equal(mode, "kill-supervisor") && kill(getppid(), SIGKILL) < 0) return 29;
    if (g_str_equal(mode, "overflow")) {
        char output[4096];
        memset(output, 'x', sizeof(output));
        size_t written = 0;
        while (written < sizeof(output)) {
            ssize_t count = write(STDOUT_FILENO, output + written, sizeof(output) - written);
            if (count > 0) {
                written += (size_t)count;
            } else if (count < 0 && errno != EINTR) {
                return 27;
            }
        }
    }
    if (g_str_equal(mode, "continuous")) {
        char output[8192];
        memset(output, 'x', sizeof(output));
        while (!helper_stop) {
            ssize_t count = write(STDOUT_FILENO, output, sizeof(output));
            if (count < 0 && errno != EINTR) break;
        }
    }
    wait_for_helper_stop();
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return 0;
}

static gpointer run_process(gpointer data) {
    ThreadedRun *run = data;
    run->result = codexbar_process_run(&run->request, run->cancellable, &run->error);
    return NULL;
}

static char *wait_for_file(const char *path) {
    gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    char *contents = NULL;
    while (g_get_monotonic_time() < deadline) {
        if (g_file_get_contents(path, &contents, NULL, NULL)) return contents;
        g_usleep(10000);
    }
    g_error("Timed out waiting for helper file: %s", path);
    return NULL;
}

static void parse_helper_pids(const char *contents, pid_t *root, pid_t *child) {
    int root_value = 0;
    int child_value = 0;
    g_assert_cmpint(sscanf(contents, "%d %d", &root_value, &child_value), ==, 2);
    *root = (pid_t)root_value;
    *child = (pid_t)child_value;
}

static void assert_process_exited(pid_t pid) {
    gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    while (kill(pid, 0) == 0 && g_get_monotonic_time() < deadline) g_usleep(10000);
    g_assert_cmpint(kill(pid, 0), ==, -1);
    g_assert_cmpint(errno, ==, ESRCH);
}

static void remove_helper_directory(const char *directory, const char *ready_path, const char *stopped_path) {
    unlink(ready_path);
    unlink(stopped_path);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_captures_output_and_nonzero_status(void) {
    const char *arguments[] = {test_program, "--helper-capture", NULL};
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .timeout_milliseconds = 2000,
        .termination_grace_milliseconds = 100,
        .maximum_output_bytes = 1024,
        .new_session = TRUE,
    };
    GError *error = NULL;
    CodexBarProcessResult *result = codexbar_process_run(&request, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(result);
    g_assert_cmpuint(result->standard_output_length, ==, strlen("standard output"));
    g_assert_cmpstr(result->standard_output, ==, "standard output");
    g_assert_cmpuint(result->standard_error_length, ==, strlen("standard error"));
    g_assert_cmpstr(result->standard_error, ==, "standard error");
    g_assert_cmpint(result->exit_status, ==, 7);
    g_assert_cmpint(result->termination_signal, ==, 0);
    g_assert_false(codexbar_process_result_succeeded(result));
    codexbar_process_result_free(result);
}

static void test_reports_spawn_failure_separately_from_exit_127(void) {
    const char *missing_arguments[] = {"/codexbar/definitely-missing", NULL};
    CodexBarProcessRequest missing_request = {
        .arguments = missing_arguments,
        .timeout_milliseconds = 2000,
        .termination_grace_milliseconds = 100,
        .maximum_output_bytes = 1024,
        .new_session = TRUE,
    };
    GError *error = NULL;
    CodexBarProcessResult *result = codexbar_process_run(&missing_request, NULL, &error);
    g_assert_null(result);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);

    const char *exit_arguments[] = {"/bin/sh", "-c", "exit 127", NULL};
    CodexBarProcessRequest exit_request = missing_request;
    exit_request.arguments = exit_arguments;
    result = codexbar_process_run(&exit_request, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(result);
    g_assert_cmpint(result->exit_status, ==, 127);
    codexbar_process_result_free(result);
}

static void test_applies_environment_and_directory_only_to_target(void) {
    GError *error = NULL;
    char *directory = g_dir_make_tmp("codexbar-process-XXXXXX", &error);
    g_assert_no_error(error);
    const char *arguments[] = {test_program, "--helper-environment", directory, NULL};
    const char *environment[] = {"CODEXBAR_TEST_ENVIRONMENT=isolated", NULL};
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .environment = environment,
        .working_directory = directory,
        .timeout_milliseconds = 2000,
        .termination_grace_milliseconds = 100,
        .maximum_output_bytes = 1024,
        .new_session = TRUE,
    };
    CodexBarProcessResult *result = codexbar_process_run(&request, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(result);
    g_assert_true(codexbar_process_result_succeeded(result));
    codexbar_process_result_free(result);
    g_assert_cmpint(rmdir(directory), ==, 0);
    g_free(directory);
}

static void test_isolates_session_directory_signals_and_descriptors(void) {
    GError *error = NULL;
    char *directory = g_dir_make_tmp("codexbar-process-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(directory);
    int source = open("/dev/null", O_RDONLY | O_CLOEXEC);
    g_assert_cmpint(source, >=, 0);
    int inherited_descriptor = fcntl(source, F_DUPFD, 128);
    close(source);
    g_assert_cmpint(inherited_descriptor, >=, 128);
    char descriptor_text[32];
    g_snprintf(descriptor_text, sizeof(descriptor_text), "%d", inherited_descriptor);

    sigset_t blocked;
    sigset_t previous_mask;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    g_assert_cmpint(sigprocmask(SIG_BLOCK, &blocked, &previous_mask), ==, 0);
    struct sigaction ignored = {.sa_handler = SIG_IGN};
    struct sigaction previous_pipe_action;
    sigemptyset(&ignored.sa_mask);
    g_assert_cmpint(sigaction(SIGPIPE, &ignored, &previous_pipe_action), ==, 0);

    const char *arguments[] = {test_program, "--helper-inspect", directory, descriptor_text, NULL};
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .working_directory = directory,
        .timeout_milliseconds = 2000,
        .termination_grace_milliseconds = 100,
        .maximum_output_bytes = 1024,
        .new_session = TRUE,
    };
    CodexBarProcessResult *result = codexbar_process_run(&request, NULL, &error);
    g_assert_cmpint(sigaction(SIGPIPE, &previous_pipe_action, NULL), ==, 0);
    g_assert_cmpint(sigprocmask(SIG_SETMASK, &previous_mask, NULL), ==, 0);
    close(inherited_descriptor);
    g_assert_cmpint(rmdir(directory), ==, 0);
    g_free(directory);

    g_assert_no_error(error);
    g_assert_nonnull(result);
    g_assert_true(codexbar_process_result_succeeded(result));
    g_assert_cmpstr(result->standard_output, ==, "ok");
    codexbar_process_result_free(result);
}

static void test_pre_cancelled_request_does_not_spawn(void) {
    GError *error = NULL;
    char *directory = g_dir_make_tmp("codexbar-process-XXXXXX", &error);
    g_assert_no_error(error);
    char *marker = g_build_filename(directory, "spawned", NULL);
    const char *arguments[] = {test_program, "--helper-touch", marker, NULL};
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .timeout_milliseconds = 2000,
        .termination_grace_milliseconds = 100,
        .maximum_output_bytes = 1024,
        .new_session = TRUE,
    };
    GCancellable *cancellable = g_cancellable_new();
    g_cancellable_cancel(cancellable);
    CodexBarProcessResult *result = codexbar_process_run(&request, cancellable, &error);
    g_assert_null(result);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));
    g_clear_error(&error);
    g_object_unref(cancellable);
    g_free(marker);
    g_assert_cmpint(rmdir(directory), ==, 0);
    g_free(directory);
}

static void test_cancellation_cleans_process_group(void) {
    GError *error = NULL;
    char *directory = g_dir_make_tmp("codexbar-process-XXXXXX", &error);
    g_assert_no_error(error);
    char *ready_path = g_build_filename(directory, "ready", NULL);
    char *stopped_path = g_build_filename(directory, "stopped", NULL);
    const char *arguments[] = {test_program, "--helper-group", "wait", ready_path, stopped_path, NULL};
    GCancellable *cancellable = g_cancellable_new();
    ThreadedRun run = {
        .request = {
            .arguments = arguments,
            .timeout_milliseconds = 5000,
            .termination_grace_milliseconds = 500,
            .maximum_output_bytes = 1024,
            .new_session = TRUE,
        },
        .cancellable = cancellable,
    };
    GThread *thread = g_thread_new("process-cancel", run_process, &run);
    char *pids = wait_for_file(ready_path);
    pid_t root = 0;
    pid_t child = 0;
    parse_helper_pids(pids, &root, &child);
    g_free(pids);
    g_cancellable_cancel(cancellable);
    g_thread_join(thread);

    g_assert_null(run.result);
    g_assert_error(run.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&run.error);
    char *stopped = wait_for_file(stopped_path);
    g_assert_cmpstr(stopped, ==, "stopped");
    g_free(stopped);
    assert_process_exited(root);
    assert_process_exited(child);
    g_object_unref(cancellable);
    remove_helper_directory(directory, ready_path, stopped_path);
    g_free(stopped_path);
    g_free(ready_path);
    g_free(directory);
}

static void test_timeout_cleans_process_group(void) {
    GError *error = NULL;
    char *directory = g_dir_make_tmp("codexbar-process-XXXXXX", &error);
    g_assert_no_error(error);
    char *ready_path = g_build_filename(directory, "ready", NULL);
    char *stopped_path = g_build_filename(directory, "stopped", NULL);
    const char *arguments[] = {test_program, "--helper-group", "wait", ready_path, stopped_path, NULL};
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .timeout_milliseconds = 1000,
        .termination_grace_milliseconds = 500,
        .maximum_output_bytes = 1024,
        .new_session = TRUE,
    };
    CodexBarProcessResult *result = codexbar_process_run(&request, NULL, &error);
    g_assert_null(result);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_clear_error(&error);
    char *pids = wait_for_file(ready_path);
    pid_t root = 0;
    pid_t child = 0;
    parse_helper_pids(pids, &root, &child);
    g_free(pids);
    char *stopped = wait_for_file(stopped_path);
    g_free(stopped);
    assert_process_exited(root);
    assert_process_exited(child);
    remove_helper_directory(directory, ready_path, stopped_path);
    g_free(stopped_path);
    g_free(ready_path);
    g_free(directory);
}

static void test_output_limit_cleans_process_group(void) {
    GError *error = NULL;
    char *directory = g_dir_make_tmp("codexbar-process-XXXXXX", &error);
    g_assert_no_error(error);
    char *ready_path = g_build_filename(directory, "ready", NULL);
    char *stopped_path = g_build_filename(directory, "stopped", NULL);
    const char *arguments[] = {test_program, "--helper-group", "overflow", ready_path, stopped_path, NULL};
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .timeout_milliseconds = 2000,
        .termination_grace_milliseconds = 500,
        .maximum_output_bytes = 512,
        .new_session = TRUE,
    };
    CodexBarProcessResult *result = codexbar_process_run(&request, NULL, &error);
    g_assert_null(result);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
    g_clear_error(&error);
    char *pids = wait_for_file(ready_path);
    pid_t root = 0;
    pid_t child = 0;
    parse_helper_pids(pids, &root, &child);
    g_free(pids);
    char *stopped = wait_for_file(stopped_path);
    g_free(stopped);
    assert_process_exited(root);
    assert_process_exited(child);
    remove_helper_directory(directory, ready_path, stopped_path);
    g_free(stopped_path);
    g_free(ready_path);
    g_free(directory);
}

static void test_short_process_output_limit_is_not_truncated(void) {
    const char *arguments[] = {test_program, "--helper-short-overflow", NULL};
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .timeout_milliseconds = 2000,
        .termination_grace_milliseconds = 100,
        .maximum_output_bytes = 512,
        .new_session = TRUE,
    };
    GError *error = NULL;
    CodexBarProcessResult *result = codexbar_process_run(&request, NULL, &error);
    g_assert_null(result);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
    g_clear_error(&error);
}

static void test_combined_output_limit(void) {
    const char *arguments[] = {test_program, "--helper-combined-overflow", NULL};
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .timeout_milliseconds = 2000,
        .termination_grace_milliseconds = 100,
        .maximum_output_bytes = 700,
        .new_session = TRUE,
    };
    GError *error = NULL;
    CodexBarProcessResult *result = codexbar_process_run(&request, NULL, &error);
    g_assert_null(result);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
    g_clear_error(&error);
}

static void test_continuous_output_cannot_starve_limit_cleanup(void) {
    GError *error = NULL;
    char *directory = g_dir_make_tmp("codexbar-process-XXXXXX", &error);
    g_assert_no_error(error);
    char *ready_path = g_build_filename(directory, "ready", NULL);
    char *stopped_path = g_build_filename(directory, "stopped", NULL);
    const char *arguments[] = {test_program, "--helper-group", "continuous", ready_path, stopped_path, NULL};
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .timeout_milliseconds = 2000,
        .termination_grace_milliseconds = 500,
        .maximum_output_bytes = 512,
        .new_session = TRUE,
    };
    CodexBarProcessResult *result = codexbar_process_run(&request, NULL, &error);
    g_assert_null(result);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
    g_clear_error(&error);
    char *pids = wait_for_file(ready_path);
    pid_t root = 0;
    pid_t child = 0;
    parse_helper_pids(pids, &root, &child);
    g_free(pids);
    char *stopped = wait_for_file(stopped_path);
    g_free(stopped);
    assert_process_exited(root);
    assert_process_exited(child);
    remove_helper_directory(directory, ready_path, stopped_path);
    g_free(stopped_path);
    g_free(ready_path);
    g_free(directory);
}

static void test_sigkill_escalation_cleans_resistant_group(void) {
    GError *error = NULL;
    char *directory = g_dir_make_tmp("codexbar-process-XXXXXX", &error);
    g_assert_no_error(error);
    char *ready_path = g_build_filename(directory, "ready", NULL);
    char *stopped_path = g_build_filename(directory, "stopped", NULL);
    const char *arguments[] = {test_program, "--helper-group", "resist", ready_path, stopped_path, NULL};
    GCancellable *cancellable = g_cancellable_new();
    ThreadedRun run = {
        .request = {
            .arguments = arguments,
            .timeout_milliseconds = 5000,
            .termination_grace_milliseconds = 100,
            .maximum_output_bytes = 1024,
            .new_session = TRUE,
        },
        .cancellable = cancellable,
    };
    GThread *thread = g_thread_new("process-kill", run_process, &run);
    char *pids = wait_for_file(ready_path);
    pid_t root = 0;
    pid_t child = 0;
    parse_helper_pids(pids, &root, &child);
    g_free(pids);
    g_cancellable_cancel(cancellable);
    g_thread_join(thread);
    g_assert_null(run.result);
    g_assert_error(run.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&run.error);
    assert_process_exited(root);
    assert_process_exited(child);
    g_assert_false(g_file_test(stopped_path, G_FILE_TEST_EXISTS));
    g_object_unref(cancellable);
    remove_helper_directory(directory, ready_path, stopped_path);
    g_free(stopped_path);
    g_free(ready_path);
    g_free(directory);
}

static void test_natural_exit_cleans_residual_group(void) {
    GError *error = NULL;
    char *directory = g_dir_make_tmp("codexbar-process-XXXXXX", &error);
    g_assert_no_error(error);
    char *ready_path = g_build_filename(directory, "ready", NULL);
    char *stopped_path = g_build_filename(directory, "stopped", NULL);
    const char *arguments[] = {test_program, "--helper-group", "natural", ready_path, stopped_path, NULL};
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .timeout_milliseconds = 2000,
        .termination_grace_milliseconds = 500,
        .maximum_output_bytes = 1024,
        .new_session = TRUE,
    };
    CodexBarProcessResult *result = codexbar_process_run(&request, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(result);
    g_assert_true(codexbar_process_result_succeeded(result));
    codexbar_process_result_free(result);
    char *pids = wait_for_file(ready_path);
    pid_t root = 0;
    pid_t child = 0;
    parse_helper_pids(pids, &root, &child);
    g_free(pids);
    char *stopped = wait_for_file(stopped_path);
    g_free(stopped);
    assert_process_exited(root);
    assert_process_exited(child);
    remove_helper_directory(directory, ready_path, stopped_path);
    g_free(stopped_path);
    g_free(ready_path);
    g_free(directory);
}

static void test_natural_exit_cleans_session_escaped_output_holder(void) {
    GError *error = NULL;
    char *directory = g_dir_make_tmp("codexbar-process-XXXXXX", &error);
    g_assert_no_error(error);
    char *ready_path = g_build_filename(directory, "ready", NULL);
    char *stopped_path = g_build_filename(directory, "stopped", NULL);
    const char *arguments[] = {test_program, "--helper-group", "escape", ready_path, stopped_path, NULL};
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .timeout_milliseconds = 2000,
        .termination_grace_milliseconds = 500,
        .maximum_output_bytes = 1024,
        .new_session = TRUE,
    };
    CodexBarProcessResult *result = codexbar_process_run(&request, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(result);
    g_assert_true(codexbar_process_result_succeeded(result));
    codexbar_process_result_free(result);
    char *pids = wait_for_file(ready_path);
    pid_t root = 0;
    pid_t child = 0;
    parse_helper_pids(pids, &root, &child);
    g_free(pids);
    char *stopped = wait_for_file(stopped_path);
    g_free(stopped);
    assert_process_exited(root);
    assert_process_exited(child);
    remove_helper_directory(directory, ready_path, stopped_path);
    g_free(stopped_path);
    g_free(ready_path);
    g_free(directory);
}

static void test_natural_exit_cleans_double_forked_closed_output_descendant(void) {
    GError *error = NULL;
    char *directory = g_dir_make_tmp("codexbar-process-XXXXXX", &error);
    g_assert_no_error(error);
    char *ready_path = g_build_filename(directory, "ready", NULL);
    char *stopped_path = g_build_filename(directory, "stopped", NULL);
    const char *arguments[] = {test_program, "--helper-double-fork", ready_path, stopped_path, NULL};
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .timeout_milliseconds = 2000,
        .termination_grace_milliseconds = 500,
        .maximum_output_bytes = 1024,
        .new_session = FALSE,
    };
    CodexBarProcessResult *result = codexbar_process_run(&request, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(result);
    g_assert_true(codexbar_process_result_succeeded(result));
    codexbar_process_result_free(result);
    char *pids = wait_for_file(ready_path);
    pid_t root = 0;
    pid_t child = 0;
    parse_helper_pids(pids, &root, &child);
    g_free(pids);
    char *stopped = wait_for_file(stopped_path);
    g_assert_cmpstr(stopped, ==, "stopped");
    g_free(stopped);
    assert_process_exited(root);
    assert_process_exited(child);
    remove_helper_directory(directory, ready_path, stopped_path);
    g_free(stopped_path);
    g_free(ready_path);
    g_free(directory);
}

static void test_parent_death_cleans_process_group(void) {
    GError *error = NULL;
    char *directory = g_dir_make_tmp("codexbar-process-XXXXXX", &error);
    g_assert_no_error(error);
    char *ready_path = g_build_filename(directory, "ready", NULL);
    char *stopped_path = g_build_filename(directory, "stopped", NULL);
    pid_t runner = fork();
    g_assert_cmpint(runner, >=, 0);
    if (runner == 0) {
        const char *arguments[] = {test_program, "--helper-group", "wait", ready_path, stopped_path, NULL};
        CodexBarProcessRequest request = {
            .arguments = arguments,
            .timeout_milliseconds = 0,
            .termination_grace_milliseconds = 500,
            .maximum_output_bytes = 1024,
            .new_session = TRUE,
        };
        CodexBarProcessResult *result = codexbar_process_run(&request, NULL, NULL);
        codexbar_process_result_free(result);
        _exit(1);
    }

    char *pids = wait_for_file(ready_path);
    pid_t root = 0;
    pid_t child = 0;
    parse_helper_pids(pids, &root, &child);
    g_free(pids);
    g_assert_cmpint(kill(runner, SIGKILL), ==, 0);
    int status = 0;
    while (waitpid(runner, &status, 0) < 0 && errno == EINTR) {}
    g_assert_true(WIFSIGNALED(status));
    char *stopped = wait_for_file(stopped_path);
    g_assert_cmpstr(stopped, ==, "stopped");
    g_free(stopped);
    assert_process_exited(root);
    assert_process_exited(child);
    remove_helper_directory(directory, ready_path, stopped_path);
    g_free(stopped_path);
    g_free(ready_path);
    g_free(directory);
}

static void test_supervisor_death_cleans_process_group(void) {
    GError *error = NULL;
    char *directory = g_dir_make_tmp("codexbar-process-XXXXXX", &error);
    g_assert_no_error(error);
    char *ready_path = g_build_filename(directory, "ready", NULL);
    char *stopped_path = g_build_filename(directory, "stopped", NULL);
    const char *arguments[] = {test_program, "--helper-group", "kill-supervisor", ready_path, stopped_path, NULL};
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .timeout_milliseconds = 2000,
        .termination_grace_milliseconds = 500,
        .maximum_output_bytes = 1024,
        .new_session = TRUE,
    };
    CodexBarProcessResult *result = codexbar_process_run(&request, NULL, &error);
    g_assert_null(result);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&error);
    char *pids = wait_for_file(ready_path);
    pid_t root = 0;
    pid_t child = 0;
    parse_helper_pids(pids, &root, &child);
    g_free(pids);
    char *stopped = wait_for_file(stopped_path);
    g_assert_cmpstr(stopped, ==, "stopped");
    g_free(stopped);
    assert_process_exited(root);
    assert_process_exited(child);
    remove_helper_directory(directory, ready_path, stopped_path);
    g_free(stopped_path);
    g_free(ready_path);
    g_free(directory);
}

int main(int argc, char **argv) {
    if (argc == 2 && g_str_equal(argv[1], "--helper-capture")) return helper_capture();
    if (argc == 4 && g_str_equal(argv[1], "--helper-inspect")) return helper_inspect(argv[2], argv[3]);
    if (argc == 3 && g_str_equal(argv[1], "--helper-touch")) return helper_touch(argv[2]);
    if (argc == 3 && g_str_equal(argv[1], "--helper-environment")) return helper_environment(argv[2]);
    if (argc == 2 && g_str_equal(argv[1], "--helper-short-overflow")) return helper_short_overflow();
    if (argc == 2 && g_str_equal(argv[1], "--helper-combined-overflow")) return helper_combined_overflow();
    if (argc == 4 && g_str_equal(argv[1], "--helper-double-fork")) return helper_double_fork(argv[2], argv[3]);
    if (argc == 5 && g_str_equal(argv[1], "--helper-group")) return helper_group(argv[2], argv[3], argv[4]);
    test_program = strchr(argv[0], '/') ? g_canonicalize_filename(argv[0], NULL) : g_find_program_in_path(argv[0]);
    g_assert_nonnull(test_program);
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/process/captures-output-and-nonzero-status", test_captures_output_and_nonzero_status);
    g_test_add_func("/process/distinguishes-spawn-failure-from-exit-127", test_reports_spawn_failure_separately_from_exit_127);
    g_test_add_func(
        "/process/applies-environment-and-directory-only-to-target",
        test_applies_environment_and_directory_only_to_target);
    g_test_add_func(
        "/process/isolates-session-directory-signals-and-descriptors",
        test_isolates_session_directory_signals_and_descriptors);
    g_test_add_func("/process/pre-cancel-does-not-spawn", test_pre_cancelled_request_does_not_spawn);
    g_test_add_func("/process/cancel-cleans-process-group", test_cancellation_cleans_process_group);
    g_test_add_func("/process/timeout-cleans-process-group", test_timeout_cleans_process_group);
    g_test_add_func("/process/output-limit-cleans-process-group", test_output_limit_cleans_process_group);
    g_test_add_func("/process/short-output-limit-is-not-truncated", test_short_process_output_limit_is_not_truncated);
    g_test_add_func("/process/combined-output-limit", test_combined_output_limit);
    g_test_add_func(
        "/process/continuous-output-cannot-starve-limit-cleanup",
        test_continuous_output_cannot_starve_limit_cleanup);
    g_test_add_func("/process/sigkill-cleans-resistant-group", test_sigkill_escalation_cleans_resistant_group);
    g_test_add_func("/process/natural-exit-cleans-residual-group", test_natural_exit_cleans_residual_group);
    g_test_add_func(
        "/process/natural-exit-cleans-session-escaped-output-holder",
        test_natural_exit_cleans_session_escaped_output_holder);
    g_test_add_func(
        "/process/natural-exit-cleans-double-forked-closed-output-descendant",
        test_natural_exit_cleans_double_forked_closed_output_descendant);
    g_test_add_func("/process/parent-death-cleans-process-group", test_parent_death_cleans_process_group);
    g_test_add_func("/process/supervisor-death-cleans-process-group", test_supervisor_death_cleans_process_group);
    return g_test_run();
}
