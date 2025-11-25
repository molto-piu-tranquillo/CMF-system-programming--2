// chat_server.c — TalkShell ChatOps Server
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h> // mkdir
#include <dirent.h>   // opendir/readdir for optional checks
#include <errno.h>

// #define PORT 5050
#define MAX_CLIENTS 20

void error_handling(char *message);

static int clients[MAX_CLIENTS];
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void broadcast(const char *msg, int sender_sock)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i] > 0 && clients[i] != sender_sock)
            send(clients[i], msg, strlen(msg), 0);
    }
    pthread_mutex_unlock(&lock);
}

void *client_handler(void *arg)
{
    int sock = *(int *)arg;
    free(arg);

    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getpeername(sock, (struct sockaddr *)&addr, &len);

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, client_ip, sizeof(client_ip));
    int client_port = ntohs(addr.sin_port);

    printf("🟢 Client connected: %s:%d\n", client_ip, client_port);

    char buf[1024];
    char msg[1100];

    while (1)
    {
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
            break; // 클라이언트 종료 또는 오류
        buf[n] = 0;

        // ========== 명령어 처리 ==========
        if (strncmp(buf, "cd ", 3) == 0)
        {
            if (chdir(buf + 3) == 0)
                send(sock, "OK: changed directory\n", 23, 0);
            else
                send(sock, "ERR: invalid path\n", 19, 0);
        }
        else if (strncmp(buf, "mkdir ", 6) == 0)
        {
            if (mkdir(buf + 6, 0755) == 0)
                send(sock, "OK: dir created\n", 17, 0);
            else
                send(sock, "ERR: mkdir failed\n", 19, 0);
        }
        else if (strncmp(buf, "ls", 2) == 0)
        {
            FILE *fp = popen("ls -al", "r");
            while (fgets(buf, sizeof(buf), fp))
                send(sock, buf, strlen(buf), 0);
            pclose(fp);
        }
        else
        {
            // 일반 메시지: 서버 콘솔 출력 + 다른 클라이언트에게 브로드캐스트
            printf("[%s:%d] %s\n", client_ip, client_port, buf);
            snprintf(msg, sizeof(msg), "client: %s\n", buf);
            broadcast(msg, sock);
            send(sock, "ACK: message received\n", 23, 0);
        }
    }

    // 연결 종료 로그
    printf("🔴 Client disconnected: %s:%d\n", client_ip, client_port);

    close(sock);
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i] == sock)
            clients[i] = 0;
    pthread_mutex_unlock(&lock);

    return NULL;
}

int main(int argc, char *argv[])
{
    // 호스트는 로컬 루프백으로, 포트는 5050으로 기본경로를 설정
    char host[256] = "127.0.0.1";
    int port = 5050;

    // 그 외에 다른 호스트 주소랑 포트를 사용자가 입력했다면, 그 주소:포트로 기본경로 덮어쓰기
    if (argc >= 3)
    { // 인자가 3개 이하(예 make run-client 127.0.0.1 9190) -> 형식: host port
        strncpy(host, argv[1], sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
        int p = atoi(argv[2]);
        if (p > 0)
            port = p;
    }
    else if (argc >= 2)
    { // 인자가 2개 이하 -> 즉, host[:port] 처럼 호스트, 포트 붙여 보내거나 호스트 ip만 보낼 때
        strncpy(host, argv[1], sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
        char *colon = strrchr(host, ':');
        if (colon)
        {
            *colon = '\0';
            int p = atoi(colon + 1);
            if (p > 0)
                port = p;
        }
    }

    int serv_sock;
    int clnt_sock;

    struct sockaddr_in serv_addr;
    struct sockaddr_in clnt_addr;
    socklen_t clnt_addr_size;

    // ✅ 서버 시작 시 자동으로 /home 디렉토리로 이동
    (void)chdir("/home"); // 반환값 무시
    printf("📁 Server base directory: /home\n");

    /* 서버 소켓(리스닝 소켓) 생성 */
    serv_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (serv_sock == -1)
        error_handling("socket() error");
    int opt = 1;

    setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* 주소 정보 초기화 */
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    // serv_addr.sin_port = htons(atoi(argv[1]));
    serv_addr.sin_port = htons(port);

    /* 주소 정보 할당 */
    if (bind(serv_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1)
        error_handling("bind() error");

    if (listen(serv_sock, 5) == -1)
        error_handling("listen() error");

    printf("🚀 ChatOps server listening on port %d...\n", port);

    while (1)
    {
        clnt_addr_size = sizeof(clnt_addr);
        clnt_sock = accept(serv_sock, (struct sockaddr *)&clnt_addr, &clnt_addr_size);
        if (clnt_sock == -1)
            error_handling("accept() error");

        // 🔗 클라이언트 접속 로그
        printf("🔗 New client connected from %s:%d\n",
               inet_ntoa(clnt_addr.sin_addr),
               ntohs(clnt_addr.sin_port));

        pthread_mutex_lock(&lock);
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i] == 0)
            {
                clients[i] = clnt_sock;
                break;
            }
        pthread_mutex_unlock(&lock);

        pthread_t tid;
        int *arg = malloc(sizeof(int));
        *arg = clnt_sock;
        pthread_create(&tid, NULL, client_handler, arg);
        pthread_detach(tid);
    }

    close(clnt_sock);
    close(serv_sock);
    return 0;
}

void error_handling(char *message)
{
    fputs(message, stderr);
    fputc('\n', stderr);
    exit(1);
}
