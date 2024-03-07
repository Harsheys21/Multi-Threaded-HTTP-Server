#include "asgn4_helper_funcs.h"
#include "connection.h"
#include "debug.h"
#include "request.h"
#include "response.h"
#include "queue.h"

#include <sys/file.h>
#include <err.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#define DEFAULT_THREAD_COUNT 4
#define RID                  "Request-Id"

queue_t *queue = NULL;
int temp;

void handle_connection(void);
void handle_get(conn_t *);
void handle_put(conn_t *);
void handle_unsupported(conn_t *);

char *get_rid(conn_t *conn) {
    char *id = conn_get_header(conn, RID);
    if (id == NULL) {
        id = "0";
    }
    return id;
}

void audit_log(char *name, char *uri, int code, char *id) {
    fprintf(stderr, "%s,/%s,%d,%s\n", name, uri, code, id);
}

void usage(FILE *stream, char *exec) {
    fprintf(stream, "usage: %s [-t threads] <port>\n", exec);
}

int main(int argc, char **argv) {
    temp = creat("input_two.txt", 0600);
    int opt = 0;
    int threads = DEFAULT_THREAD_COUNT;
    pthread_t *threadids;

    if (argc < 2) {
        warnx("wrong arguments: %s port_num", argv[0]);
        fprintf(stderr, "%s", argv[0]);
        return EXIT_FAILURE;
    }

    while ((opt = getopt(argc, argv, "t:h")) != -1) {
        switch (opt) {
        case 't':
            threads = strtol(optarg, NULL, 10);
            if (threads <= 0) {
                errx(EXIT_FAILURE, "bad number of threads");
            }
            break;
        case 'h': usage(stdout, argv[0]); return EXIT_SUCCESS;
        default: usage(stderr, argv[0]); return EXIT_FAILURE;
        }
    }

    if (optind >= argc) {
        warnx("wrong arguments: %s port_num", argv[0]);
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    char *endptr = NULL;
    size_t port = (size_t) strtoull(argv[optind], &endptr, 10);
    if (endptr && *endptr != '\0') {
        warnx("invalid port number: %s", argv[1]);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    Listener_Socket sock;
    if (listener_init(&sock, port) < 0) {
        warnx("Cannot open listener sock: %s", argv[0]);
        return EXIT_FAILURE;
    }

    threadids = malloc(sizeof(pthread_t) * threads);
    queue = queue_new(threads);

    for (int i = 0; i < threads; i++) {
        int rc = pthread_create(threadids + i, NULL, (void *(*) (void *) ) handle_connection, NULL);
        if (rc != 0) {
            warnx("Cannot create %d pthreads", threads);
            free(threadids); // Free threadids in case of failure
            queue_delete(&queue); // Free queue in case of failure
            return EXIT_FAILURE;
        }
    }

    while (1) {
        uintptr_t connfd = listener_accept(&sock);
        // debug("accepted %lu\n", connfd);
        queue_push(queue, (void *) connfd);
    }

    queue_delete(&queue);
    close(temp);
    remove("input_two.txt");
    free(threadids);
    return EXIT_SUCCESS;
}

void handle_connection(void) {
    while (true) {
        uintptr_t connfd = 0;
        conn_t *conn = NULL;

        queue_pop(queue, (void **) &connfd);

        // debug("popped off %lu", connfd);
        conn = conn_new(connfd);

        const Response_t *res = conn_parse(conn);

        if (res != NULL) {
            conn_send_response(conn, res);
        } else {
            const Request_t *req = conn_get_request(conn);
            if (req == &REQUEST_GET) {
                handle_get(conn);
            } else if (req == &REQUEST_PUT) {
                handle_put(conn);
            } else {
                handle_unsupported(conn);
            }
        }

        conn_delete(&conn);
        close(connfd);
    }
}

void handle_get(conn_t *conn) {
    // TODO: Implement GET
    // obtain url
    char *uri = conn_get_uri(conn);
    // debug("GET %s", uri);
    int temp = open("input_two.txt", O_WRONLY, 0666);
    flock(temp, LOCK_EX);
    // open file
    int fone = open(uri, O_RDONLY, 0666);
    if (fone < 0) {
        if (errno == ENOENT) {
            conn_send_response(conn, &RESPONSE_NOT_FOUND);
            close(fone);
            char *id = get_rid(conn);
            int code = response_get_code(&RESPONSE_NOT_FOUND);
            audit_log("GET", uri, code, id);
            flock(temp, LOCK_UN);
            close(temp);
            return;
        } else {
            conn_send_response(conn, &RESPONSE_FORBIDDEN);
            close(fone);
            char *id = get_rid(conn);
            int code = response_get_code(&RESPONSE_FORBIDDEN);
            audit_log("GET", uri, code, id);
            flock(temp, LOCK_UN);
            close(temp);
            return;
        }
    }
    flock(fone, LOCK_SH);
    flock(temp, LOCK_UN);
    close(temp);

    // check if it's a directory
    struct stat file_info;

    stat(uri, &file_info);
    if (S_ISDIR(file_info.st_mode)) {
        conn_send_response(conn, &RESPONSE_FORBIDDEN);
        close(fone);
        char *id = get_rid(conn);
        int code = response_get_code(&RESPONSE_FORBIDDEN);
        audit_log("GET", uri, code, id);
        flock(fone, LOCK_UN);
        close(fone);
        return;
    }

    //give output
    conn_send_file(conn, fone, file_info.st_size);
    char *id = get_rid(conn);
    int code = response_get_code(&RESPONSE_OK);
    audit_log("GET", uri, code, id);
    flock(fone, LOCK_UN);
    close(fone);
    return;
}

void handle_put(conn_t *conn) {
    // TODO: Implement PUT
    const Response_t *res = NULL;
    // obtain url
    char *uri = conn_get_uri(conn);
    // debug("PUT %s", uri);
    int temp = open("input_two.txt", O_WRONLY, 0666);
    flock(temp, LOCK_EX);
    bool exists = access(uri, F_OK);

    int fone = open(uri, O_CREAT | O_WRONLY);

    if (fone < 0) {
        res = (const Response_t *) &RESPONSE_FORBIDDEN;

        goto out;
    }
    flock(fone, LOCK_EX);
    flock(temp, LOCK_UN);
    close(temp);

    int rc = ftruncate(fone, 0);
    assert(rc == 0);

    conn_recv_file(conn, fone);
    if (exists && res == NULL) {
        conn_send_response(conn, &RESPONSE_CREATED);
        char *id = get_rid(conn);
        int code = response_get_code(&RESPONSE_CREATED);
        audit_log("PUT", uri, code, id);
    }

    if (!exists && res == NULL) {
        conn_send_response(conn, &RESPONSE_OK);
        char *id = get_rid(conn);
        int code = response_get_code(&RESPONSE_OK);
        audit_log("PUT", uri, code, id);
    }

out:
    if (res != NULL) {
        conn_send_response(conn, res);
        char *id = get_rid(conn);
        int code = response_get_code(res);
        audit_log("PUT", uri, code, id);
    }

    if (fone > 0) {
        flock(fone, LOCK_UN);
        close(fone);
    } else {
        flock(temp, LOCK_UN);
        close(temp);
    }
    return;
}

void handle_unsupported(conn_t *conn) {
    // debug("Unsupported request");
    conn_send_response(conn, &RESPONSE_NOT_IMPLEMENTED);
    return;
}
