/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 4096
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)
#define PIPE_READ 0
#define PIPE_WRITE 1

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    int stop_requested;
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int stdout_pipe[2];
    int stderr_pipe[2];
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

// Producer thread data structure - MUST be defined before use
typedef struct producer_data {
    int fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} producer_data_t;

static supervisor_ctx_t g_ctx;
static volatile sig_atomic_t g_signal_received = 0;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }
    
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;
    
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }
    
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;
    
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    FILE *log_files[256] = {NULL};
    char log_path[PATH_MAX];
    
    while (1) {
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0) {
            break;
        }
        
        int idx = item.container_id[0] % 256;
        if (log_files[idx] == NULL) {
            snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);
            log_files[idx] = fopen(log_path, "a");
            if (log_files[idx] == NULL) {
                fprintf(stderr, "Failed to open log file for %s\n", item.container_id);
                continue;
            }
        }
        
        fwrite(item.data, 1, item.length, log_files[idx]);
        fflush(log_files[idx]);
    }
    
    for (int i = 0; i < 256; i++) {
        if (log_files[i] != NULL) {
            fclose(log_files[i]);
        }
    }
    
    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;
    char *argv[4];
    
    if (config->nice_value != 0) {
        (void)nice(config->nice_value);
    }
    
    dup2(config->stdout_pipe[PIPE_WRITE], STDOUT_FILENO);
    dup2(config->stderr_pipe[PIPE_WRITE], STDERR_FILENO);
    close(config->stdout_pipe[PIPE_READ]);
    close(config->stdout_pipe[PIPE_WRITE]);
    close(config->stderr_pipe[PIPE_READ]);
    close(config->stderr_pipe[PIPE_WRITE]);
    
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        perror("mount");
        return 1;
    }
    
    if (chroot(config->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    
    if (chdir("/") != 0) {
        perror("chdir");
        return 1;
    }
    
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount proc");
        return 1;
    }
    
    (void)sethostname(config->id, strlen(config->id));
    
    argv[0] = config->command;
    argv[1] = NULL;
    
    execvp(config->command, argv);
    
    perror("execvp");
    return 1;
}

static void add_container_record(supervisor_ctx_t *ctx, const char *id, pid_t pid,
                                  unsigned long soft_limit, unsigned long hard_limit)
{
    container_record_t *record = malloc(sizeof(container_record_t));
    if (!record) {
        perror("malloc");
        return;
    }
    
    strncpy(record->id, id, CONTAINER_ID_LEN - 1);
    record->id[CONTAINER_ID_LEN - 1] = '\0';
    record->host_pid = pid;
    record->started_at = time(NULL);
    record->state = CONTAINER_STARTING;
    record->soft_limit_bytes = soft_limit;
    record->hard_limit_bytes = hard_limit;
    record->exit_code = 0;
    record->exit_signal = 0;
    record->stop_requested = 0;
    snprintf(record->log_path, sizeof(record->log_path), "%s/%s.log", LOG_DIR, id);
    record->next = NULL;
    
    pthread_mutex_lock(&ctx->metadata_lock);
    record->next = ctx->containers;
    ctx->containers = record;
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static container_record_t *find_container_record(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *curr = ctx->containers;
    
    pthread_mutex_lock(&ctx->metadata_lock);
    while (curr) {
        if (strcmp(curr->id, id) == 0) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            return curr;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
    return NULL;
}

static void update_container_state(supervisor_ctx_t *ctx, pid_t pid, int status)
{
    container_record_t *curr = ctx->containers;
    
    pthread_mutex_lock(&ctx->metadata_lock);
    while (curr) {
        if (curr->host_pid == pid) {
            if (WIFEXITED(status)) {
                curr->exit_code = WEXITSTATUS(status);
                curr->state = CONTAINER_EXITED;
                if (curr->exit_code == 9 && !curr->stop_requested) {
                    curr->state = CONTAINER_KILLED;
                } else if (curr->stop_requested) {
                    curr->state = CONTAINER_STOPPED;
                }
            } else if (WIFSIGNALED(status)) {
                curr->exit_signal = WTERMSIG(status);
                if (curr->exit_signal == 9 && !curr->stop_requested) {
                    curr->state = CONTAINER_KILLED;
                } else {
                    curr->state = CONTAINER_STOPPED;
                }
            }
            break;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static void *producer_thread_fn(void *arg)
{
    producer_data_t *data = (producer_data_t *)arg;
    char buffer[LOG_CHUNK_SIZE];
    ssize_t bytes;
    log_item_t item;
    
    strncpy(item.container_id, data->container_id, CONTAINER_ID_LEN - 1);
    item.container_id[CONTAINER_ID_LEN - 1] = '\0';
    
    while ((bytes = read(data->fd, buffer, LOG_CHUNK_SIZE)) > 0) {
        item.length = bytes;
        memcpy(item.data, buffer, bytes);
        bounded_buffer_push(data->buffer, &item);
    }
    
    close(data->fd);
    free(data);
    return NULL;
}

static int start_container(supervisor_ctx_t *ctx, const control_request_t *req)
{
    char stack[STACK_SIZE];
    pid_t pid;
    child_config_t config;
    int stdout_pipe[2], stderr_pipe[2];
    
    if (find_container_record(ctx, req->container_id) != NULL) {
        fprintf(stderr, "Container %s already exists\n", req->container_id);
        return -1;
    }
    
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        perror("pipe");
        return -1;
    }
    
    memset(&config, 0, sizeof(config));
    strncpy(config.id, req->container_id, CONTAINER_ID_LEN - 1);
    config.id[CONTAINER_ID_LEN - 1] = '\0';
    strncpy(config.rootfs, req->rootfs, PATH_MAX - 1);
    config.rootfs[PATH_MAX - 1] = '\0';
    strncpy(config.command, req->command, CHILD_COMMAND_LEN - 1);
    config.command[CHILD_COMMAND_LEN - 1] = '\0';
    config.nice_value = req->nice_value;
    config.stdout_pipe[0] = stdout_pipe[0];
    config.stdout_pipe[1] = stdout_pipe[1];
    config.stderr_pipe[0] = stderr_pipe[0];
    config.stderr_pipe[1] = stderr_pipe[1];
    
    pid = clone(child_fn, stack + STACK_SIZE,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                &config);
    
    if (pid == -1) {
        perror("clone");
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return -1;
    }
    
    close(stdout_pipe[PIPE_WRITE]);
    close(stderr_pipe[PIPE_WRITE]);
    
    add_container_record(ctx, req->container_id, pid,
                         req->soft_limit_bytes, req->hard_limit_bytes);
    
    if (ctx->monitor_fd >= 0) {
        struct monitor_request req_mon;
        memset(&req_mon, 0, sizeof(req_mon));
        req_mon.pid = pid;
        req_mon.soft_limit_bytes = req->soft_limit_bytes;
        req_mon.hard_limit_bytes = req->hard_limit_bytes;
        strncpy(req_mon.container_id, req->container_id, MONITOR_NAME_LEN - 1);
        
        if (ioctl(ctx->monitor_fd, MONITOR_REGISTER, &req_mon) < 0) {
            fprintf(stderr, "Failed to register PID %d with monitor\n", pid);
        }
    }
    
    pthread_t producer_thread;
    producer_data_t *pdata = malloc(sizeof(producer_data_t));
    if (pdata) {
        pdata->fd = stdout_pipe[PIPE_READ];
        strncpy(pdata->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        pdata->container_id[CONTAINER_ID_LEN - 1] = '\0';
        pdata->buffer = &ctx->log_buffer;
        pthread_create(&producer_thread, NULL, producer_thread_fn, pdata);
        pthread_detach(producer_thread);
    }
    
    container_record_t *record = find_container_record(ctx, req->container_id);
    if (record) {
        record->state = CONTAINER_RUNNING;
    }
    
    return pid;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    (void)monitor_fd;
    (void)container_id;
    (void)host_pid;
    (void)soft_limit_bytes;
    (void)hard_limit_bytes;
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    (void)monitor_fd;
    (void)container_id;
    (void)host_pid;
    return 0;
}

static void signal_handler(int sig)
{
    g_signal_received = sig;
    g_ctx.should_stop = 1;
    
    if (sig == SIGCHLD) {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            update_container_state(&g_ctx, pid, status);
        }
    }
}

static int setup_control_socket(void)
{
    struct sockaddr_un addr;
    int fd;
    
    unlink(CONTROL_PATH);
    
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    
    if (listen(fd, 10) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    
    return fd;
}

static void handle_control_request(int client_fd)
{
    control_request_t req;
    control_response_t resp;
    ssize_t n;
    
    n = read(client_fd, &req, sizeof(req));
    if (n != sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Invalid request");
        (void)write(client_fd, &resp, sizeof(resp));
        close(client_fd);
        return;
    }
    
    memset(&resp, 0, sizeof(resp));
    
    switch (req.kind) {
    case CMD_START:
        if (start_container(&g_ctx, &req) > 0) {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message), "Container %s started\n", req.container_id);
        } else {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "Failed to start %s\n", req.container_id);
        }
        break;
        
    case CMD_RUN:
        {
            pid_t pid = start_container(&g_ctx, &req);
            if (pid > 0) {
                int status;
                waitpid(pid, &status, 0);
                resp.status = WEXITSTATUS(status);
                snprintf(resp.message, sizeof(resp.message), "Container %s exited with status %d\n",
                         req.container_id, resp.status);
            } else {
                resp.status = -1;
                snprintf(resp.message, sizeof(resp.message), "Failed to run %s\n", req.container_id);
            }
        }
        break;
        
    case CMD_PS:
        {
            container_record_t *curr = g_ctx.containers;
            char output[4096] = "";
            snprintf(output, sizeof(output), "%-20s %-8s %-12s %-10s %-10s\n",
                     "ID", "PID", "STATE", "SOFT(MB)", "HARD(MB)");
            pthread_mutex_lock(&g_ctx.metadata_lock);
            while (curr && strlen(output) < 4000) {
                char line[256];
                snprintf(line, sizeof(line), "%-20s %-8d %-12s %-10lu %-10lu\n",
                         curr->id, curr->host_pid, state_to_string(curr->state),
                         curr->soft_limit_bytes >> 20, curr->hard_limit_bytes >> 20);
                strncat(output, line, sizeof(output) - strlen(output) - 1);
                curr = curr->next;
            }
            pthread_mutex_unlock(&g_ctx.metadata_lock);
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message), "%s", output);
        }
        break;
        
    case CMD_LOGS:
        {
            char log_path[PATH_MAX];
            snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req.container_id);
            FILE *f = fopen(log_path, "r");
            if (f) {
                char log_content[4096] = "";
                char line[1024];
                while (fgets(line, sizeof(line), f) && strlen(log_content) < 4000) {
                    strncat(log_content, line, sizeof(log_content) - strlen(log_content) - 1);
                }
                fclose(f);
                snprintf(resp.message, sizeof(resp.message), "%s", log_content);
                resp.status = 0;
            } else {
                resp.status = -1;
                snprintf(resp.message, sizeof(resp.message), "No logs for %s\n", req.container_id);
            }
        }
        break;
        
    case CMD_STOP:
        {
            container_record_t *record = find_container_record(&g_ctx, req.container_id);
            if (record) {
                record->stop_requested = 1;
                if (kill(record->host_pid, SIGTERM) == 0) {
                    resp.status = 0;
                    snprintf(resp.message, sizeof(resp.message), "Stopped %s\n", req.container_id);
                } else {
                    resp.status = -1;
                    snprintf(resp.message, sizeof(resp.message), "Failed to stop %s\n", req.container_id);
                }
            } else {
                resp.status = -1;
                snprintf(resp.message, sizeof(resp.message), "Container %s not found\n", req.container_id);
            }
        }
        break;
        
    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Unknown command\n");
    }
    
    (void)write(client_fd, &resp, sizeof(resp));
    close(client_fd);
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    int rc;

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.server_fd = -1;
    g_ctx.monitor_fd = -1;
    
    mkdir(LOG_DIR, 0755);

    rc = pthread_mutex_init(&g_ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&g_ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&g_ctx.metadata_lock);
        return 1;
    }

    /*
     * TODO:
     *   1) open /dev/container_monitor
     *   2) create the control socket / FIFO / shared-memory channel
     *   3) install SIGCHLD / SIGINT / SIGTERM handling
     *   4) spawn the logger thread
     *   5) enter the supervisor event loop
     */
    
    g_ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (g_ctx.monitor_fd < 0) {
        fprintf(stderr, "Warning: Could not open /dev/container_monitor (module loaded?)\n");
    }
    
    g_ctx.server_fd = setup_control_socket();
    if (g_ctx.server_fd < 0) {
        fprintf(stderr, "Failed to setup control socket\n");
        bounded_buffer_destroy(&g_ctx.log_buffer);
        pthread_mutex_destroy(&g_ctx.metadata_lock);
        return 1;
    }
    
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    
    pthread_create(&g_ctx.logger_thread, NULL, logging_thread, &g_ctx);
    
    printf("Supervisor running on %s\n", CONTROL_PATH);
    printf("Base rootfs: %s\n", rootfs);
    
    while (!g_ctx.should_stop) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(g_ctx.server_fd, &readfds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        if (select(g_ctx.server_fd + 1, &readfds, NULL, NULL, &timeout) > 0) {
            int client_fd = accept(g_ctx.server_fd, NULL, NULL);
            if (client_fd >= 0) {
                handle_control_request(client_fd);
            }
        }
    }
    
    printf("Shutting down supervisor...\n");
    
    bounded_buffer_begin_shutdown(&g_ctx.log_buffer);
    pthread_join(g_ctx.logger_thread, NULL);
    bounded_buffer_destroy(&g_ctx.log_buffer);
    pthread_mutex_destroy(&g_ctx.metadata_lock);
    close(g_ctx.server_fd);
    unlink(CONTROL_PATH);
    if (g_ctx.monitor_fd >= 0)
        close(g_ctx.monitor_fd);
    
    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    struct sockaddr_un addr;
    int fd;
    control_response_t resp;
    ssize_t n;
    
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is supervisor running?)");
        close(fd);
        return 1;
    }
    
    n = write(fd, req, sizeof(*req));
    if (n != sizeof(*req)) {
        perror("write");
        close(fd);
        return 1;
    }
    
    n = read(fd, &resp, sizeof(resp));
    if (n != sizeof(resp)) {
        perror("read");
        close(fd);
        return 1;
    }
    
    printf("%s", resp.message);
    close(fd);
    return resp.status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    req.nice_value = 0;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    req.nice_value = 0;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    return send_control_request(&req);
}
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
