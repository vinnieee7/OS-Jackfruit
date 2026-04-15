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
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "monitor_ioctl.h"

#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define CONTROL_MESSAGE_LEN 4096
#define CONTAINER_ID_LEN 32
#define STACK_SIZE (1024 * 1024)
#define BUFFER_SIZE 10

/* ================= LOG BUFFER ================= */

typedef struct {
    char data[BUFFER_SIZE][256];
    int in, out, count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} log_buffer_t;

log_buffer_t logbuf;

void init_buffer() {
    logbuf.in = logbuf.out = logbuf.count = 0;
    pthread_mutex_init(&logbuf.lock, NULL);
    pthread_cond_init(&logbuf.not_empty, NULL);
    pthread_cond_init(&logbuf.not_full, NULL);
}

void produce_log(const char *msg) {
    pthread_mutex_lock(&logbuf.lock);

    while (logbuf.count == BUFFER_SIZE)
        pthread_cond_wait(&logbuf.not_full, &logbuf.lock);

    strncpy(logbuf.data[logbuf.in], msg, 255);
    logbuf.in = (logbuf.in + 1) % BUFFER_SIZE;
    logbuf.count++;

    pthread_cond_signal(&logbuf.not_empty);
    pthread_mutex_unlock(&logbuf.lock);
}

void *logger_thread(void *arg) {
    while (1) {
        pthread_mutex_lock(&logbuf.lock);

        while (logbuf.count == 0)
            pthread_cond_wait(&logbuf.not_empty, &logbuf.lock);

        char msg[256];
        strcpy(msg, logbuf.data[logbuf.out]);

        logbuf.out = (logbuf.out + 1) % BUFFER_SIZE;
        logbuf.count--;

        pthread_cond_signal(&logbuf.not_full);
        pthread_mutex_unlock(&logbuf.lock);

        printf("%s", msg);
    }
}

/* ================= KERNEL INTERFACE ================= */

void monitor_register(pid_t pid, const char *id) {
    int fd = open("/dev/monitor", O_RDWR);
    if (fd < 0) return;

    struct monitor_request req;
    req.pid = pid;
    req.soft_limit_bytes = 0;
    req.hard_limit_bytes = 0;
    strncpy(req.container_id, id, MONITOR_NAME_LEN - 1);
    req.container_id[MONITOR_NAME_LEN - 1] = '\0';

    ioctl(fd, MONITOR_REGISTER, &req);
    close(fd);
}

void monitor_unregister(pid_t pid, const char *id) {
    int fd = open("/dev/monitor", O_RDWR);
    if (fd < 0) return;

    struct monitor_request req;
    req.pid = pid;
    strncpy(req.container_id, id, MONITOR_NAME_LEN - 1);
    req.container_id[MONITOR_NAME_LEN - 1] = '\0';

    ioctl(fd, MONITOR_UNREGISTER, &req);
    close(fd);
}

/* ================= STRUCTS ================= */

typedef enum { CMD_START, CMD_STOP, CMD_LOGS } command_kind_t;

typedef struct {
    char rootfs[256];
    char command[128];
    int pipefd[2];
} child_args_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t pid;
    int pipe_fd;

    char logs[100][256];
    int log_count;

    struct container_record *next;
} container_record_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[256];
    char command[128];
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

container_record_t *head = NULL;

/* ================= CHILD ================= */

int child_fn(void *arg) {
    child_args_t *cargs = (child_args_t *)arg;

    dup2(cargs->pipefd[1], STDOUT_FILENO);
    dup2(cargs->pipefd[1], STDERR_FILENO);
    close(cargs->pipefd[0]);
    close(cargs->pipefd[1]);

    chroot(cargs->rootfs);
    chdir("/");

    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    execl(cargs->command, cargs->command, NULL);
    return 1;
}

/* ================= CONTAINER MGMT ================= */

void add_container(const char *id, pid_t pid, int pipe_fd) {
    container_record_t *node = malloc(sizeof(container_record_t));
    strncpy(node->id, id, CONTAINER_ID_LEN - 1);
    node->id[CONTAINER_ID_LEN - 1] = '\0';
    node->pid = pid;
    node->pipe_fd = pipe_fd;
    node->log_count = 0;
    node->next = head;
    head = node;
}

container_record_t* find_container(const char *id) {
    container_record_t *curr = head;
    while (curr) {
        if (strcmp(curr->id, id) == 0)
            return curr;
        curr = curr->next;
    }
    return NULL;
}

void remove_container(const char *id) {
    container_record_t *curr = head, *prev = NULL;

    while (curr) {
        if (strcmp(curr->id, id) == 0) {
            if (prev) prev->next = curr->next;
            else head = curr->next;

            close(curr->pipe_fd);
            free(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

/* ================= SUPERVISOR ================= */

static int run_supervisor(void) {
    init_buffer();
    pthread_t tid;
    pthread_create(&tid, NULL, logger_thread, NULL);

    int server_fd, client_fd;
    struct sockaddr_un addr;

    unlink(CONTROL_PATH);
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);

    printf("Supervisor running...\n");

    while (1) {
        control_request_t req;
        control_response_t res;

        client_fd = accept(server_fd, NULL, NULL);
        read(client_fd, &req, sizeof(req));

        if (req.kind == CMD_START) {
            int pipefd[2];
            pipe(pipefd);

            child_args_t *cargs = malloc(sizeof(child_args_t));
            strcpy(cargs->rootfs, req.rootfs);
            strcpy(cargs->command, req.command);
            cargs->pipefd[0] = pipefd[0];
            cargs->pipefd[1] = pipefd[1];

            void *stack = malloc(STACK_SIZE);
            void *stack_top = stack + STACK_SIZE;

            pid_t pid = clone(child_fn, stack_top,
                              CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                              cargs);

            close(pipefd[1]);
            add_container(req.container_id, pid, pipefd[0]);

            monitor_register(pid, req.container_id);

            snprintf(res.message, sizeof(res.message),
                     "Started container %s PID=%d",
                     req.container_id, pid);
        }

        else if (req.kind == CMD_STOP) {
            container_record_t *c = find_container(req.container_id);

            if (c) {
                kill(c->pid, SIGKILL);
                monitor_unregister(c->pid, req.container_id);
                remove_container(req.container_id);

                snprintf(res.message, sizeof(res.message),
                         "Stopped container %s", req.container_id);
            } else {
                snprintf(res.message, sizeof(res.message),
                         "Container not found");
            }
        }

        else if (req.kind == CMD_LOGS) {
            container_record_t *c = find_container(req.container_id);

            if (!c) {
                snprintf(res.message, sizeof(res.message),
                         "Container not found");
            } else {
                res.message[0] = '\0';

                for (int i = 0; i < c->log_count; i++) {
                    strncat(res.message, c->logs[i],
                            sizeof(res.message) - strlen(res.message) - 1);
                }
            }
        }

        write(client_fd, &res, sizeof(res));
        close(client_fd);

        container_record_t *curr = head;
        while (curr) {
            char buffer[256];

            while (1) {
                int n = read(curr->pipe_fd, buffer, sizeof(buffer) - 1);
                if (n <= 0) break;

                buffer[n] = '\0';

                char logline[300];
                snprintf(logline, sizeof(logline),
                         "[LOG %s]: %s", curr->id, buffer);

                produce_log(logline);

                if (curr->log_count < 100) {
                    strcpy(curr->logs[curr->log_count], logline);
                    curr->log_count++;
                }
            }
            curr = curr->next;
        }
    }
}

/* ================= CLIENT ================= */

static int send_control_request(const control_request_t *req) {
    int sock;
    struct sockaddr_un addr;
    control_response_t res;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    connect(sock, (struct sockaddr *)&addr, sizeof(addr));

    write(sock, req, sizeof(*req));
    read(sock, &res, sizeof(res));

    printf("%s\n", res.message);

    close(sock);
    return 0;
}

/* ================= CLI ================= */

static int cmd_start(int argc, char *argv[]) {
    control_request_t req = {CMD_START};
    strcpy(req.container_id, argv[2]);
    strcpy(req.rootfs, argv[3]);
    strcpy(req.command, argv[4]);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[]) {
    control_request_t req = {CMD_STOP};
    strcpy(req.container_id, argv[2]);
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[]) {
    control_request_t req = {CMD_LOGS};
    strcpy(req.container_id, argv[2]);
    return send_control_request(&req);
}

/* ================= MAIN ================= */

int main(int argc, char *argv[]) {
    if (strcmp(argv[1], "supervisor") == 0)
        return run_supervisor();

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    return 0;
}
