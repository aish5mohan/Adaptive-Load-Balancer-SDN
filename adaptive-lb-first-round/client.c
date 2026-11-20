#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BUFFER_SIZE 2048

typedef struct {
    const char *host;
    unsigned port;
    unsigned client_id;
    unsigned requests;
    double total_ms;
    unsigned succeeded;
} client_task;

static double elapsed_ms(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000.0 +
           (end.tv_nsec - start.tv_nsec) / 1000000.0;
}

static void *run_client(void *argument) {
    client_task *task = argument;
    for (unsigned request_id = 1; request_id <= task->requests; ++request_id) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); continue; }

        struct sockaddr_in server = {0};
        server.sin_family = AF_INET;
        server.sin_port = htons((uint16_t)task->port);
        if (inet_pton(AF_INET, task->host, &server.sin_addr) != 1) {
            fprintf(stderr, "Invalid IPv4 address: %s\n", task->host);
            close(fd); break;
        }

        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        if (connect(fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
            fprintf(stderr, "client=%u request=%u connect failed: %s\n",
                    task->client_id, request_id, strerror(errno));
            close(fd); continue;
        }

        char request[BUFFER_SIZE];
        int length = snprintf(request, sizeof(request), "client=%u request=%u\n",
                              task->client_id, request_id);
        if (length < 0 || send(fd, request, (size_t)length, MSG_NOSIGNAL) < 0) {
            perror("send"); close(fd); continue;
        }

        char response[BUFFER_SIZE];
        size_t used = 0;
        while (used + 1 < sizeof(response)) {
            ssize_t n = recv(fd, response + used, sizeof(response) - used - 1, 0);
            if (n <= 0) break;
            used += (size_t)n;
            if (memchr(response, '\n', used)) break;
        }
        response[used] = '\0';
        clock_gettime(CLOCK_MONOTONIC, &end);
        close(fd);

        if (used == 0) {
            fprintf(stderr, "client=%u request=%u empty response\n", task->client_id, request_id);
            continue;
        }
        double latency = elapsed_ms(start, end);
        task->total_ms += latency;
        ++task->succeeded;
        printf("client=%u request=%u latency_ms=%.2f %s",
               task->client_id, request_id, latency, response);
    }
    return NULL;
}

static int parse_positive(const char *text, unsigned *value) {
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || end == text || *end != '\0' || parsed == 0 || parsed > 100000) return -1;
    *value = (unsigned)parsed;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <server-ipv4> <port> <clients> <requests-per-client>\n", argv[0]);
        return EXIT_FAILURE;
    }
    unsigned port, clients, requests;
    if (parse_positive(argv[2], &port) || port > 65535 ||
        parse_positive(argv[3], &clients) || clients > 1000 ||
        parse_positive(argv[4], &requests)) {
        fprintf(stderr, "Invalid port/client/request count.\n");
        return EXIT_FAILURE;
    }

    pthread_t *threads = calloc(clients, sizeof(*threads));
    client_task *tasks = calloc(clients, sizeof(*tasks));
    if (!threads || !tasks) { perror("calloc"); free(threads); free(tasks); return EXIT_FAILURE; }

    unsigned started = 0;
    for (unsigned i = 0; i < clients; ++i) {
        tasks[i] = (client_task){ .host = argv[1], .port = port,
                                  .client_id = i + 1, .requests = requests };
        int error = pthread_create(&threads[i], NULL, run_client, &tasks[i]);
        if (error != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            break;
        }
        ++started;
    }

    unsigned total_success = 0;
    double total_ms = 0.0;
    for (unsigned i = 0; i < started; ++i) {
        pthread_join(threads[i], NULL);
        total_success += tasks[i].succeeded;
        total_ms += tasks[i].total_ms;
    }
    unsigned attempted = started * requests;
    printf("SUMMARY clients_started=%u attempted=%u succeeded=%u failed=%u average_latency_ms=%.2f\n",
           started, attempted, total_success, attempted - total_success,
           total_success ? total_ms / total_success : 0.0);
    free(threads);
    free(tasks);
    return total_success == attempted ? EXIT_SUCCESS : EXIT_FAILURE;
}
