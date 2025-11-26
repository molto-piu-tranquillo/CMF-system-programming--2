#define _XOPEN_SOURCE 700
#include "dir_manager.h"
#include "utils.h"
#include "socket_client.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

// 1. 프로토콜 정의: 서버가 ls 끝에 이 마커를 보내야 함
#define LS_END_MARKER "ENDLS\n"

/* ============================================================
   벡터 유틸 (동적 배열 관리)
   ============================================================ */
static void vec_push(char ***arr, int *count, int *cap, const char *s)
{
    if (*count + 1 > *cap)
    {
        *cap = (*cap == 0) ? 16 : (*cap * 2);
        *arr = realloc(*arr, sizeof(char *) * (*cap));
    }
    (*arr)[*count] = strdup(s);
    (*count)++;
}

static int cmp_str(const void *a, const void *b)
{
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcasecmp(sa, sb);
}

/* ============================================================
   공통: 소켓 유틸 및 데이터 수신 함수
   ============================================================ */
extern int sockfd; // socket_client.c에 정의된 변수 참조

int socket_is_connected(void)
{
    return (sockfd >= 0);
}

// 2. 개선된 수신 함수: 마커가 나올 때까지 데이터를 모두 받음 (잘림 방지)
static void recv_ls_all(char *recvbuf, size_t bufsize)
{
    recvbuf[0] = '\0';
    char chunk[4096];

    for (;;)
    {
        // 소켓에서 조금씩 읽어옴
        int n = socket_recv_response(chunk, sizeof(chunk));
        if (n <= 0)
            break;

        // 버퍼 오버플로우 방지
        if (strlen(recvbuf) + (size_t)n + 1 >= bufsize)
        {
            strncat(recvbuf, chunk, bufsize - strlen(recvbuf) - 1);
            break;
        }

        strncat(recvbuf, chunk, bufsize - strlen(recvbuf) - 1);

        // 종료 마커("ENDLS")가 포함되어 있으면 수신 중단
        if (strstr(recvbuf, LS_END_MARKER))
            break;
    }

    // 마커 부분은 데이터에서 제거 (화면에 출력되지 않게)
    char *p = strstr(recvbuf, LS_END_MARKER);
    if (p)
        *p = '\0';
}

/* ============================================================
   상단: 디렉토리 목록 (dirlist)
   ============================================================ */
void dirlist_init(DirList *dl)
{
    memset(dl, 0, sizeof(*dl));
    dl->selected = 0;
}

void dirlist_free(DirList *dl)
{
    for (int i = 0; i < dl->count; i++)
        free(dl->items[i]);
    free(dl->items);
    memset(dl, 0, sizeof(*dl));
}

void dirlist_scan(DirList *dl, const char *cwd_abs)
{
    dirlist_free(dl);
    dirlist_init(dl);
    snprintf(dl->cwd, sizeof(dl->cwd), "%s", cwd_abs);

    if (socket_is_connected())
    {
        // 3. 최적화: 불필요한 'cd' 명령 제거하고 바로 'ls' 요청
        socket_send_cmd("ls -al");

        // 4. 대용량 버퍼로 전체 데이터 수신
        char recvbuf[16384];
        recv_ls_all(recvbuf, sizeof(recvbuf));

        // 파싱
        char *line = strtok(recvbuf, "\n");
        while (line)
        {
            // 'd'로 시작하는 라인 (디렉토리)
            if (line[0] == 'd')
            {
                char perms[11];
                char name[256];

                // 5. 정교한 파싱: 권한 문자열과 파일명 추출
                // 예: drwxr-xr-x ... filename
                if (sscanf(line, "%10s %*s %*s %*s %*s %*s %*s %*s %255s",
                           perms, name) == 2)
                {
                    // . 과 .. 은 제외
                    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                    {
                        line = strtok(NULL, "\n");
                        continue;
                    }
                    vec_push(&dl->items, &dl->count, &dl->cap, name);
                }
            }
            line = strtok(NULL, "\n");
        }
    }
    else
    {
        // 로컬 모드
        DIR *d = opendir(cwd_abs);
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d)))
        {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            
            char p[PATH_MAX];
            path_join(p, cwd_abs, e->d_name);
            if (is_directory(p))
                vec_push(&dl->items, &dl->count, &dl->cap, p);
        }
        closedir(d);
    }

    qsort(dl->items, dl->count, sizeof(char *), cmp_str);
    dl->selected = (dl->count > 0) ? 0 : -1;
}

void dirlist_draw(WINDOW *win, const DirList *dl, bool focused)
{
    werase(win);
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " 현재위치: %s ", dl->cwd);
    int h, w;
    getmaxyx(win, h, w);
    for (int i = 0; i < dl->count && i < h - 2; i++)
    {
        const char *name = dl->items[i];
        int sel = (i == dl->selected);
        if (sel && focused) wattron(win, A_REVERSE);
        mvwprintw(win, i + 1, 2, "%c %.*s", sel ? '>' : ' ', w - 4, name);
        if (sel && focused) wattroff(win, A_REVERSE);
    }
    wrefresh(win);
}

/* ============================================================
   하단: 파일 목록 (filelist)
   ============================================================ */
void filelist_init(FileList *fl)
{
    memset(fl, 0, sizeof(*fl));
    fl->selected = 0;
}

void filelist_free(FileList *fl)
{
    for (int i = 0; i < fl->count; i++)
        free(fl->items[i]);
    free(fl->items);
    memset(fl, 0, sizeof(*fl));
}

void filelist_scan(FileList *fl, const char *dir_abs)
{
    filelist_free(fl);
    filelist_init(fl);
    snprintf(fl->base, sizeof(fl->base), "%s", dir_abs);

    if (socket_is_connected())
    {
        // 서버 요청
        socket_send_cmd("ls -al");

        char recvbuf[16384];
        recv_ls_all(recvbuf, sizeof(recvbuf));

        char *line = strtok(recvbuf, "\n");
        while (line)
        {
            // '-'로 시작하는 라인 (일반 파일)
            if (line[0] == '-')
            {
                char perms[11];
                char name[256];
                if (sscanf(line, "%10s %*s %*s %*s %*s %*s %*s %*s %255s",
                           perms, name) == 2)
                {
                    vec_push(&fl->items, &fl->count, &fl->cap, name);
                }
            }
            line = strtok(NULL, "\n");
        }
    }
    else
    {
        // 로컬 모드
        DIR *d = opendir(dir_abs);
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d)))
        {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            vec_push(&fl->items, &fl->count, &fl->cap, e->d_name);
        }
        closedir(d);
    }

    qsort(fl->items, fl->count, sizeof(char *), cmp_str);
    fl->selected = (fl->count > 0) ? 0 : -1;
}

void filelist_draw(WINDOW *win, const FileList *fl, bool focused)
{
    werase(win);
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " 선택한 디렉토리: %s ", fl->base);
    int h, w;
    getmaxyx(win, h, w);
    for (int i = 0; i < fl->count && i < h - 2; i++)
    {
        const char *name = fl->items[i];
        int sel = (i == fl->selected);
        if (sel && focused) wattron(win, A_REVERSE);
        mvwprintw(win, i + 1, 2, "%c %.*s", sel ? '>' : ' ', w - 4, name);
        if (sel && focused) wattroff(win, A_REVERSE);
    }
    wrefresh(win);
}