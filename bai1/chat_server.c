
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

#define MAX_CLIENTS  100
#define BUFFER_SIZE  1024
#define NAME_SIZE    64
#define PORT_DEFAULT 9000

typedef enum {
    STATE_EMPTY,
    STATE_WAIT_NAME,
    STATE_AUTHENTICATED
} ClientState;

typedef struct {
    int           fd;
    ClientState   state;
    char          name[NAME_SIZE];
    char          addr[INET_ADDRSTRLEN];
    int           port;
} ClientInfo;

static ClientInfo clients[MAX_CLIENTS];
static struct pollfd fds[MAX_CLIENTS + 1];
static int nfds = 1;

static void get_timestamp(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buf, len, "%Y/%m/%d %I:%M:%S%p", tm_info);
}

static void send_to_client(int fd, const char *msg)
{
    send(fd, msg, strlen(msg), 0);
}

static void broadcast(const char *msg, int exclude_fd)
{
    int i;
    for (i = 1; i < nfds; i++) {
        if (fds[i].fd != -1 && fds[i].fd != exclude_fd) {
            int j;
            for (j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].fd == fds[i].fd && 
                    clients[j].state == STATE_AUTHENTICATED) {
                    send_to_client(fds[i].fd, msg);
                    break;
                }
            }
        }
    }
}

static int find_empty_slot(void)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].state == STATE_EMPTY)
            return i;
    }
    return -1;
}

static int find_client_by_fd(int fd)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].state != STATE_EMPTY && clients[i].fd == fd)
            return i;
    }
    return -1;
}

static void remove_client(int fd)
{
    int idx = find_client_by_fd(fd);
    char msg[BUFFER_SIZE];
    char timestamp[64];

    if (idx >= 0) {
        get_timestamp(timestamp, sizeof(timestamp));
        if (clients[idx].state == STATE_AUTHENTICATED) {
            snprintf(msg, sizeof(msg), 
                     "%s %s đã rời phòng chat.\n",
                     timestamp, clients[idx].name);
            broadcast(msg, fd);
            printf("[%s] Client '%s' (%s:%d) đã ngắt kết nối.\n",
                   timestamp, clients[idx].name,
                   clients[idx].addr, clients[idx].port);
        } else {
            printf("[%s] Client chưa xác thực (%s:%d) đã ngắt kết nối.\n",
                   timestamp, clients[idx].addr, clients[idx].port);
        }
        clients[idx].state = STATE_EMPTY;
        clients[idx].fd = -1;
        memset(clients[idx].name, 0, NAME_SIZE);
    }

    close(fd);

    int i;
    for (i = 1; i < nfds; i++) {
        if (fds[i].fd == fd) {
            int j;
            for (j = i; j < nfds - 1; j++) {
                fds[j] = fds[j + 1];
            }
            nfds--;
            break;
        }
    }
}

static void handle_name_input(int idx, char *buf)
{
    char name_part[NAME_SIZE];
    char msg[BUFFER_SIZE];
    char timestamp[64];

    buf[strcspn(buf, "\r\n")] = '\0';

    if (strncmp(buf, "client_id: ", 11) != 0) {
        send_to_client(clients[idx].fd,
            "Sai cú pháp! Vui lòng nhập đúng chuỗi \"client_id: <tên_của_bạn>\"\n"
            "(Ví dụ: client_id: messi)\n"
            "Nhập lại: ");
        return;
    }

    char *name_start = buf + 11;
    strncpy(name_part, name_start, NAME_SIZE - 1);
    name_part[NAME_SIZE - 1] = '\0';

    if (strlen(name_part) == 0) {
        send_to_client(clients[idx].fd,
            "Tên client không được để trống! Vui lòng nhập lại (ví dụ client_id: messi): ");
        return;
    }

    int i;
    for (i = 0; i < (int)strlen(name_part); i++) {
        if (name_part[i] == ' ' || name_part[i] == '\t') {
            send_to_client(clients[idx].fd,
                "Tên client phải viết liền (không có khoảng trắng)! Vui lòng nhập lại: ");
            return;
        }
    }

    strncpy(clients[idx].name, name_part, NAME_SIZE - 1);
    clients[idx].name[NAME_SIZE - 1] = '\0';
    clients[idx].state = STATE_AUTHENTICATED;

    get_timestamp(timestamp, sizeof(timestamp));

    snprintf(msg, sizeof(msg),
             "Chào mừng %s! Bạn đã tham gia phòng chat.\n", clients[idx].name);
    send_to_client(clients[idx].fd, msg);

    snprintf(msg, sizeof(msg),
             "%s %s đã tham gia phòng chat.\n",
             timestamp, clients[idx].name);
    broadcast(msg, clients[idx].fd);

    printf("[%s] Client '%s' (%s:%d) đã xác thực thành công.\n",
           timestamp, clients[idx].name,
           clients[idx].addr, clients[idx].port);
}

static void handle_message(int idx, char *buf)
{
    char msg[BUFFER_SIZE];
    char timestamp[64];

    buf[strcspn(buf, "\r\n")] = '\0';

    if (strlen(buf) == 0)
        return;

    get_timestamp(timestamp, sizeof(timestamp));

    snprintf(msg, sizeof(msg), "%s %s: %s\n",
             timestamp, clients[idx].name, buf);

    printf("%s", msg);

    broadcast(msg, clients[idx].fd);
}

static int create_listen_socket(int port)
{
    int listen_fd;
    struct sockaddr_in server_addr;
    int opt = 1;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket()");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt()");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind()");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(listen_fd, 10) < 0) {
        perror("listen()");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    return listen_fd;
}

int main(int argc, char *argv[])
{
    int listen_fd;
    int port;
    int i;
    int ret;

    if (argc >= 2)
        port = atoi(argv[1]);
    else
        port = PORT_DEFAULT;

    for (i = 0; i < MAX_CLIENTS; i++) {
        clients[i].state = STATE_EMPTY;
        clients[i].fd = -1;
    }

    for (i = 0; i < MAX_CLIENTS + 1; i++) {
        fds[i].fd = -1;
        fds[i].events = 0;
        fds[i].revents = 0;
    }

    listen_fd = create_listen_socket(port);
    printf("Chat Server đang chạy trên cổng %d...\n", port);
    printf("Sử dụng: telnet localhost %d để kết nối.\n", port);

    fds[0].fd = listen_fd;
    fds[0].events = POLLIN;

    while (1) {
        ret = poll(fds, nfds, -1);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            perror("poll()");
            break;
        }

        if (fds[0].revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int new_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);

            if (new_fd < 0) {
                perror("accept()");
            } else {
                int slot = find_empty_slot();
                if (slot < 0 || nfds >= MAX_CLIENTS + 1) {
                    send_to_client(new_fd, "Server đầy! Không thể kết nối.\n");
                    close(new_fd);
                } else {
                    char timestamp[64];
                    get_timestamp(timestamp, sizeof(timestamp));

                    clients[slot].fd = new_fd;
                    clients[slot].state = STATE_WAIT_NAME;
                    inet_ntop(AF_INET, &client_addr.sin_addr,
                              clients[slot].addr, INET_ADDRSTRLEN);
                    clients[slot].port = ntohs(client_addr.sin_port);

                    fds[nfds].fd = new_fd;
                    fds[nfds].events = POLLIN;
                    fds[nfds].revents = 0;
                    nfds++;

                    printf("[%s] Kết nối mới từ %s:%d (fd=%d)\n",
                           timestamp, clients[slot].addr,
                           clients[slot].port, new_fd);

                    send_to_client(new_fd,
                        "=== Chào mừng đến với Chat Server ===\n"
                        "Vui lòng nhập tên theo cú pháp: client_id: <tên_của_bạn>\n"
                        "(Ví dụ: client_id: messi)\n"
                        "Nhập cú pháp: ");
                }
            }
        }

        for (i = 1; i < nfds; i++) {
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                char buf[BUFFER_SIZE];
                int nbytes;

                if (fds[i].revents & (POLLHUP | POLLERR)) {
                    remove_client(fds[i].fd);
                    i--;
                    continue;
                }

                nbytes = recv(fds[i].fd, buf, sizeof(buf) - 1, 0);

                if (nbytes <= 0) {
                    remove_client(fds[i].fd);
                    i--;
                } else {
                    buf[nbytes] = '\0';

                    int cidx = find_client_by_fd(fds[i].fd);
                    if (cidx < 0)
                        continue;

                    if (clients[cidx].state == STATE_WAIT_NAME) {
                        handle_name_input(cidx, buf);
                    } else if (clients[cidx].state == STATE_AUTHENTICATED) {
                        handle_message(cidx, buf);
                    }
                }
            }
        }
    }

    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].state != STATE_EMPTY) {
            close(clients[i].fd);
        }
    }
    close(listen_fd);

    return 0;
}
