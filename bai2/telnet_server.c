#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

#define MAX_CLIENTS 100
#define BUFFER_SIZE 2048
#define DB_FILE "users.txt"
#define PORT_DEFAULT 9000

typedef enum {
    STATE_EMPTY,
    STATE_WAIT_AUTH,
    STATE_AUTHENTICATED
} ClientState;

typedef struct {
    int fd;
    ClientState state;
} ClientInfo;

static ClientInfo clients[MAX_CLIENTS];
static struct pollfd fds[MAX_CLIENTS + 1];
static int nfds = 1;

int check_auth(const char *user, const char *pass) {
    FILE *fp = fopen(DB_FILE, "r");
    if (!fp) {
        perror("Khong the mo file cơ sở dữ liệu (users.txt)");
        return 0;
    }

    char line[256];
    char db_user[128], db_pass[128];

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%s %s", db_user, db_pass) == 2) {
            if (strcmp(user, db_user) == 0 && strcmp(pass, db_pass) == 0) {
                fclose(fp);
                return 1;
            }
        }
    }
    fclose(fp);
    return 0;
}

static void send_to_client(int fd, const char *msg) {
    send(fd, msg, strlen(msg), 0);
}

static int find_empty_slot(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].state == STATE_EMPTY) return i;
    }
    return -1;
}

static int find_client_by_fd(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].state != STATE_EMPTY && clients[i].fd == fd) return i;
    }
    return -1;
}

static void remove_client(int fd) {
    int idx = find_client_by_fd(fd);
    if (idx >= 0) {
        printf("Client fd=%d da ngat ket noi.\n", fd);
        clients[idx].state = STATE_EMPTY;
        clients[idx].fd = -1;
    }
    close(fd);

    for (int i = 1; i < nfds; i++) {
        if (fds[i].fd == fd) {
            for (int j = i; j < nfds - 1; j++) {
                fds[j] = fds[j + 1];
            }
            nfds--;
            break;
        }
    }
}

static void handle_auth(int idx, char *buf) {
    char user[128], pass[128];
    
    buf[strcspn(buf, "\r\n")] = '\0';
    if (strlen(buf) == 0) return;

    if (sscanf(buf, "%s %s", user, pass) == 2) {
        if (check_auth(user, pass)) {
            clients[idx].state = STATE_AUTHENTICATED;
            send_to_client(clients[idx].fd, "Dang nhap thanh cong!\nThu muc hien tai:\n> ");
            printf("Client fd=%d xac thuc thanh cong user: %s\n", clients[idx].fd, user);
        } else {
            send_to_client(clients[idx].fd, "Sai tai khoan hoac mat khau. Vui long nhap lai: ");
        }
    } else {
        send_to_client(clients[idx].fd, "Sai cu phap. Nhap lai (user pass): ");
    }
}

static void handle_command(int idx, char *buf) {
    char cmd[BUFFER_SIZE + 32];
    
    buf[strcspn(buf, "\r\n")] = '\0';
    
    if (strlen(buf) == 0) {
        send_to_client(clients[idx].fd, "\n> ");
        return;
    }

    if (strcmp(buf, "exit") == 0 || strcmp(buf, "quit") == 0) {
        remove_client(clients[idx].fd);
        return;
    }

    printf("Thuc hien lenh tu fd=%d: %s\n", clients[idx].fd, buf);
    
    snprintf(cmd, sizeof(cmd), "%s > out.txt 2>&1", buf);
    system(cmd);
    
    FILE *fp = fopen("out.txt", "r");
    if (fp) {
        char file_buf[1024];
        size_t bytes_read;
        while ((bytes_read = fread(file_buf, 1, sizeof(file_buf), fp)) > 0) {
            send(clients[idx].fd, file_buf, bytes_read, 0);
        }
        fclose(fp);
    } else {
        send_to_client(clients[idx].fd, "Khong the doc ket qua tu file out.txt\n");
    }
    
    send_to_client(clients[idx].fd, "\n> ");
}

int main(int argc, char *argv[]) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int port = (argc >= 2) ? atoi(argv[1]) : PORT_DEFAULT;
    int opt = 1;

    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed"); exit(1);
    }
    listen(listen_fd, 10);

    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].state = STATE_EMPTY;
    for (int i = 0; i < MAX_CLIENTS + 1; i++) fds[i].fd = -1;

    fds[0].fd = listen_fd;
    fds[0].events = POLLIN;

    printf("Telnet Server (poll) dang chay tren port %d...\n", port);

    while (1) {
        if (poll(fds, nfds, -1) < 0) continue;

        if (fds[0].revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int new_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);

            int slot = find_empty_slot();
            if (slot < 0 || nfds >= MAX_CLIENTS + 1) {
                send_to_client(new_fd, "Server da day!\n");
                close(new_fd);
            } else {
                clients[slot].fd = new_fd;
                clients[slot].state = STATE_WAIT_AUTH;
                fds[nfds].fd = new_fd;
                fds[nfds].events = POLLIN;
                nfds++;
                
                printf("Client moi ket noi: fd=%d\n", new_fd);
                send_to_client(new_fd, "--- Telnet Server ---\nNhap \"user pass\" de dang nhap: ");
            }
        }

        for (int i = 1; i < nfds; i++) {
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                char buf[BUFFER_SIZE];
                
                if (fds[i].revents & (POLLHUP | POLLERR) || recv(fds[i].fd, buf, sizeof(buf) - 1, 0) <= 0) {
                    remove_client(fds[i].fd);
                    i--; continue;
                }

                int cidx = find_client_by_fd(fds[i].fd);
                if (cidx < 0) continue;

                if (clients[cidx].state == STATE_WAIT_AUTH) {
                    handle_auth(cidx, buf);
                } else if (clients[cidx].state == STATE_AUTHENTICATED) {
                    handle_command(cidx, buf);
                }
            }
        }
    }
    return 0;
}
