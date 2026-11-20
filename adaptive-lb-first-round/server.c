#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define BACKLOG 128
#define MESSAGE_SIZE 1024

static volatile sig_atomic_t running = 1;
static int listen_fd = -1;
static pthread_mutex_t active_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned active_clients = 0;

typedef struct {
    int fd;
    unsigned delay_ms;
    unsigned server_id;
} client_task;

static void handle_signal(int signal_number) {
    (void)signal_number;
    running = 0;
    if (listen_fd >= 0) close(listen_fd);
}

static int read_line(int fd, char *buffer, size_t capacity) {
    size_t used = 0;
    while (used + 1 < capacity) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 0) return used == 0 ? 0 : (buffer[used] = '\0', 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (c == '\n') break;
        buffer[used++] = c;
    }
    buffer[used] = '\0';
    return 1;
}

static void sleep_ms(unsigned milliseconds) {
    struct timespec remaining = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L
    };
    while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR) {}
}

static void *serve_client(void *argument) {
    client_task *task = argument;
    char request[MESSAGE_SIZE];
    int read_result = read_line(task->fd, request, sizeof(request));

    pthread_mutex_lock(&active_lock);
    unsigned active = active_clients;
    pthread_mutex_unlock(&active_lock);

    if (read_result > 0) {
        sleep_ms(task->delay_ms);
        char response[MESSAGE_SIZE];
        int length = snprintf(response, sizeof(response),
                              "server=%u pid=%ld active=%u request=%s\n",
                              task->server_id, (long)getpid(), active, request);
        if (length > 0) (void)send(task->fd, response, (size_t)length, MSG_NOSIGNAL);
        printf("server=%u active=%u handled: %s\n", task->server_id, active, request);
        fflush(stdout);
    }

    close(task->fd);
    pthread_mutex_lock(&active_lock);
    --active_clients;
    pthread_mutex_unlock(&active_lock);
    free(task);
    return NULL;
}

static int parse_unsigned(const char *text, unsigned *value) {
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || end == text || *end != '\0' || parsed > 600000) return -1;
    *value = (unsigned)parsed;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <port> <server-id> [delay-ms]\n", argv[0]);
        return EXIT_FAILURE;
    }

    unsigned port, server_id, delay_ms = 0;
    if (parse_unsigned(argv[1], &port) || port == 0 || port > 65535 ||
        parse_unsigned(argv[2], &server_id) ||
        (argc == 4 && parse_unsigned(argv[3], &delay_ms))) {
        fprintf(stderr, "Invalid port, server ID, or delay.\n");
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return EXIT_FAILURE; }
    int reuse = 1;
    (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind"); close(listen_fd); return EXIT_FAILURE;
    }
    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen"); close(listen_fd); return EXIT_FAILURE;
    }

    printf("server=%u listening on port=%u delay_ms=%u pid=%ld\n",
           server_id, port, delay_ms, (long)getpid());
    fflush(stdout);

    while (running) {
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_length);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (!running || errno == EBADF) break;
            perror("accept"); continue;
        }
        client_task *task = malloc(sizeof(*task));
        if (!task) { perror("malloc"); close(client_fd); continue; }
        task->fd = client_fd;
        task->delay_ms = delay_ms;
        task->server_id = server_id;
        pthread_mutex_lock(&active_lock);
        ++active_clients;
        pthread_mutex_unlock(&active_lock);
        pthread_t thread;
        int thread_error = pthread_create(&thread, NULL, serve_client, task);
        if (thread_error != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(thread_error));
            close(client_fd);
            free(task);
            pthread_mutex_lock(&active_lock);
            --active_clients;
            pthread_mutex_unlock(&active_lock);
            continue;
        }
        pthread_detach(thread);
    }

    if (listen_fd >= 0) close(listen_fd);
    puts("server shutting down");
    return EXIT_SUCCESS;
}
