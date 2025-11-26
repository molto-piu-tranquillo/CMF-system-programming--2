#define _XOPEN_SOURCE 700
#include <locale.h> //한글 인코딩
#include <ncurses.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

#include "socket_client.h"

#include "dir_manager.h"
#include "chat_manager.h"
#include "input_manager.h"
#include "utils.h"
#include "auth.h"

#ifdef USE_INOTIFY
#include <sys/inotify.h>
#include <fcntl.h>
#endif

static WINDOW *win_dir, *win_file, *win_chat, *win_input;

typedef struct
{
    DirList dl;
    FileList fl;
    ChatState chat;
    FocusArea focus;
    char username[64];
    bool logged_in;
} App;

static int capture_masked_input(WINDOW *win, int y, int x, char *out, int maxlen)
{
    int pos = 0;
    keypad(win, TRUE);
    wmove(win, y, x);
    wrefresh(win);
    while (1)
    {
        int ch = wgetch(win);
        if (ch == '\n' || ch == KEY_ENTER)
        {
            break;
        }
        else if ((ch == KEY_BACKSPACE || ch == 127) && pos > 0)
        {
            pos--;
            mvwaddch(win, y, x + pos, ' ');
            wmove(win, y, x + pos);
            wrefresh(win);
        }
        else if (isprint(ch) && pos < maxlen - 1)
        {
            out[pos++] = (char)ch;
            mvwaddch(win, y, x + pos - 1, '*');
            wrefresh(win);
        }
    }
    out[pos] = '\0';
    return pos;
}

static bool login_prompt(App *app)
{
    int h, w;
    getmaxyx(stdscr, h, w);
    int win_w = 60, win_h = 9;
    int sy = (h - win_h) / 2;
    int sx = (w - win_w) / 2;

    for (int attempt = 0; attempt < 3; attempt++)
    {
        WINDOW *login = newwin(win_h, win_w, sy, sx);
        box(login, 0, 0);
        mvwprintw(login, 0, 2, " 로그인 ");
        mvwprintw(login, 2, 2, "ID : ");
        mvwprintw(login, 3, 2, "PW : ");
        mvwprintw(login, 5, 2, "(demo 계정: admin1 / opslead)");
        wrefresh(login);

        char user[64] = {0};
        char pass[128] = {0};
        echo();
        mvwgetnstr(login, 2, 8, user, (int)sizeof(user) - 1);
        noecho();
        capture_masked_input(login, 3, 8, pass, (int)sizeof(pass));

        char hash[65];
        hash_password(pass, hash);
        memset(pass, 0, sizeof(pass));

        char cmd[256];
        snprintf(cmd, sizeof(cmd), "LOGIN %s %s", user, hash);
        socket_send_cmd(cmd);

        char resp[256];
        int rn = socket_recv_response(resp, sizeof(resp));
        while (rn > 0 && strncmp(resp, "INFO:", 5) == 0)
            rn = socket_recv_response(resp, sizeof(resp));

        if (rn > 0 && strncmp(resp, "OK:", 3) == 0)
        {
            snprintf(app->username, sizeof(app->username), "%s", user);
            app->logged_in = true;
            delwin(login);
            return true;
        }

        const char *err_msg = rn > 0 ? resp : "로그인 응답 없음";
        mvwprintw(login, 6, 2, "서버 응답: %-50.50s", err_msg);
        
        mvwprintw(login, 7, 2, "로그인 실패(%d/3) - 다시 시도", attempt + 1);
        wrefresh(login);
        napms(1000);
        delwin(login);

        if (rn > 0 && strncmp(resp, "ERR: account locked", 20) == 0)
            break;
    }
    return false;
}

/* =======================================================
   레이아웃 구성 (수정됨: 파일 목록을 우측 상단으로 이동)
   ======================================================= */
static void layout_create(void)
{
    int h, w;
    getmaxyx(stdscr, h, w);
    int left_w = w / 3;           // 왼쪽 너비 (디렉토리 목록)
    int right_w = w - left_w;     // 오른쪽 너비 (파일 + 채팅)
    
    int input_h = 3;              // 입력창 높이
    int file_h = h / 2;           // 파일 목록 높이 (화면 절반)
    int chat_h = h - file_h - input_h; // 나머지 채팅창

    // 1. 디렉토리 창 (좌측 전체)
    win_dir = newwin(h, left_w, 0, 0);

    // 2. 파일 목록 창 (우측 상단)
    win_file = newwin(file_h, right_w, 0, left_w);

    // 3. 채팅 창 (우측 하단)
    win_chat = newwin(chat_h, right_w, file_h, left_w);

    // 4. 입력 창 (우측 최하단)
    win_input = newwin(input_h, right_w, h - input_h, left_w);

    box(win_dir, 0, 0);
    mvwprintw(win_dir, 0, 2, " 디렉토리 ");
    
    box(win_file, 0, 0);
    mvwprintw(win_file, 0, 2, " 파일 목록 ");
    
    box(win_chat, 0, 0);
    mvwprintw(win_chat, 0, 2, " 채팅 ");
    
    box(win_input, 0, 0);
    mvwprintw(win_input, 0, 2, " 입력 ");

    wrefresh(win_dir);
    wrefresh(win_file);
    wrefresh(win_chat);
    wrefresh(win_input);
}

/* =======================================================
   초기화
   ======================================================= */
static void app_init(App *a)
{
    a->focus = FOCUS_DIR;

    // [수정] 시작 디렉토리를 현재 폴더('.')로 변경
    const char *start_dir = ".";
    char absdir[PATH_MAX];
    abspath(absdir, start_dir);

    // 디렉토리 목록 초기화
    dirlist_init(&a->dl);
    dirlist_scan(&a->dl, absdir);

    // 파일 목록 초기화
    filelist_init(&a->fl);
    const char *base_dir = (a->dl.count > 0) ? a->dl.items[a->dl.selected] : absdir;
    filelist_scan(&a->fl, base_dir);

    // 채팅창 초기화
    chat_init(&a->chat, base_dir);

    // 즉시 전체 화면 갱신
    dirlist_draw(win_dir, &a->dl, true);
    filelist_draw(win_file, &a->fl, false);
    chat_draw(win_chat, &a->chat);
    input_draw(win_input);

    wrefresh(win_dir);
    wrefresh(win_file);
    wrefresh(win_chat);
    wrefresh(win_input);

    status_bar(win_chat, "[Tab] 포커스 이동  [Enter] 선택/전송  [Backspace] 상위  [q] 종료");
}

/* =======================================================
   종료 처리
   ======================================================= */
static void app_free(App *a)
{
    dirlist_free(&a->dl);
    filelist_free(&a->fl);
}

/* =======================================================
   포커스 이동
   ======================================================= */
static void change_focus(App *a, int dir)
{
    int f = (int)a->focus;
    f = (f + dir + 4) % 4;
    a->focus = (FocusArea)f;
    dirlist_draw(win_dir, &a->dl, a->focus == FOCUS_DIR);
    filelist_draw(win_file, &a->fl, a->focus == FOCUS_FILE);
    chat_draw(win_chat, &a->chat);
}

/* =======================================================
   디렉토리 선택 및 상위 이동
   ======================================================= */
static void open_selected_dir(App *a)
{
    if (a->dl.selected < 0 || a->dl.selected >= a->dl.count)
        return;
    const char *dir_abs = a->dl.items[a->dl.selected];
    filelist_scan(&a->fl, dir_abs);
    filelist_draw(win_file, &a->fl, a->focus == FOCUS_FILE);
    chat_init(&a->chat, dir_abs);
    chat_draw(win_chat, &a->chat);
}

static void go_parent_dir(App *a)
{
    char parent[PATH_MAX];
    dirname_of(parent, a->dl.cwd);
    
    if (socket_is_connected())
    {
        if (strcmp(parent, a->dl.cwd) == 0)
            return;
    }
    else if (!is_directory(parent) || strcmp(parent, a->dl.cwd) == 0)
    {
        return;
    }
    dirlist_scan(&a->dl, parent);
    dirlist_draw(win_dir, &a->dl, a->focus == FOCUS_DIR);
    open_selected_dir(a);
}

/* =======================================================
   inotify (Linux용)
   ======================================================= */
#ifdef USE_INOTIFY
static int setup_inotify(const char *path)
{
    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0)
        return -1;
    inotify_add_watch(fd, path, IN_MODIFY | IN_CLOSE_WRITE | IN_MOVE_SELF | IN_DELETE_SELF);
    return fd;
}
#endif

/* =======================================================
   메인 루프
   ======================================================= */
int main(int argc, char *argv[])
{

    char host[256] = "127.0.0.1";
    int port = 5050;

    if (argc >= 3)
    { 
        strncpy(host, argv[1], sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
        int p = atoi(argv[2]);
        if (p > 0)
            port = p;
    }
    else if (argc >= 2)
    { 
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

    if (socket_connect_to(host, port) < 0)
    {
        fprintf(stderr, "[tui] connect failed: %s:%d\n", host, port);
        return 1;
    }

    setlocale(LC_ALL, "");
    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(200); 

    clear();
    refresh();

    App app;
    memset(&app, 0, sizeof(app));

    if (!login_prompt(&app))
    {
        endwin();
        socket_close();
        fprintf(stderr, "[tui] login failed\n");
        return 1;
    }

    clear();
    refresh();
    layout_create();

    app_init(&app);

    refresh();

#ifdef USE_INOTIFY
    int inofd = setup_inotify(app.chat.log_path);
#endif

    char linebuf[4096] = {0};

    for (;;)
    {
        chat_check_update(&app.chat);
        if (app.chat.dirty)
        {
            app.chat.dirty = 0;
            chat_draw(win_chat, &app.chat);
        }

#ifdef USE_INOTIFY
        if (inofd >= 0)
        {
            char buf[1024];
            int n = read(inofd, buf, sizeof(buf));
            if (n > 0)
                app.chat.dirty = 1;
        }
#endif

        int ch = getch();
        if (ch == ERR)
            continue;
        if (ch == 'q' || ch == 'Q')
            break;

        switch (app.focus)
        {
        case FOCUS_DIR:
            if (ch == KEY_UP)
            {
                if (app.dl.selected > 0)
                    app.dl.selected--;
                dirlist_draw(win_dir, &app.dl, true);
            }
            else if (ch == KEY_DOWN)
            {
                if (app.dl.selected < app.dl.count - 1)
                    app.dl.selected++;
                dirlist_draw(win_dir, &app.dl, true);
            }
            else if (ch == '\n' || ch == KEY_RIGHT)
            {
                open_selected_dir(&app);
                app.focus = FOCUS_FILE;
                filelist_draw(win_file, &app.fl, true);
            }
            else if (ch == KEY_BACKSPACE || ch == 127)
            {
                go_parent_dir(&app);
            }
            break;

        case FOCUS_FILE:
            if (ch == KEY_UP)
            {
                if (app.fl.selected > 0)
                    app.fl.selected--;
                filelist_draw(win_file, &app.fl, true);
            }
            else if (ch == KEY_DOWN)
            {
                if (app.fl.selected < app.fl.count - 1)
                    app.fl.selected++;
                filelist_draw(win_file, &app.fl, true);
            }
            else if (ch == '\n')
            {
                if (app.fl.selected >= 0 && app.fl.selected < app.fl.count)
                {
                    char tgt[PATH_MAX];
                    path_join(tgt, app.fl.base, app.fl.items[app.fl.selected]);
                    
                    if (socket_is_connected() || is_directory(tgt))
                    {
                        dirlist_scan(&app.dl, tgt);
                        dirlist_draw(win_dir, &app.dl, app.focus == FOCUS_DIR);
                        open_selected_dir(&app);
                    }
                    else
                    {
                        status_bar(win_chat, "파일은 열지 않고, 채팅의 기준 경로만 유지합니다.");
                    }
                }
            }
            else if (ch == KEY_LEFT)
            {
                app.focus = FOCUS_DIR;
                dirlist_draw(win_dir, &app.dl, true);
            }
            break;

        case FOCUS_CHAT:
            if (ch == '\t' || ch == KEY_RIGHT || ch == '\n')
            {
                app.focus = FOCUS_INPUT;
                input_draw(win_input);
            }
            else if (ch == KEY_LEFT)
            {
                app.focus = FOCUS_FILE;
                filelist_draw(win_file, &app.fl, true);
            }
            break;

        case FOCUS_INPUT:
            if (ch == '\n')
            {
            }
            else
            {
                input_draw(win_input);
                wmove(win_input, 1, 4);
                linebuf[0] = '\0';
                input_capture_line(win_input, linebuf, sizeof(linebuf));
                
                if (strncmp(linebuf, "cd ", 3) == 0 || strncmp(linebuf, "mkdir ", 6) == 0 || strncmp(linebuf, "ls", 2) == 0)
                {
                    socket_send_cmd(linebuf);

                    char response[2048];
                    while (socket_recv_response(response, sizeof(response)) > 0)
                    {
                        // [수정됨] 수동 ls 명령 시 ENDLS를 만나면 루프 종료
                        if (strstr(response, "ENDLS")) {
                            char *p = strstr(response, "ENDLS");
                            *p = '\0'; // 화면에 ENDLS는 출력하지 않음
                            if (strlen(response) > 0) 
                                chat_append(&app.chat, "server", response);
                            break;
                        }

                        chat_append(&app.chat, "server", response);
                        if (strstr(response, "OK") || strstr(response, "ERR"))
                            break;
                    }
                    app.chat.dirty = 1;
                }
                else
                {
                    const char *user = app.username[0] ? app.username : safe_username();
                    if (strlen(linebuf) > 0)
                    {
                        chat_append(&app.chat, user, linebuf);
                        app.chat.dirty = 1;
                    }
                }
                app.focus = FOCUS_CHAT;
            }
            break;
        }

        if (ch == '\t')
            change_focus(&app, +1);
        else if (ch == KEY_BTAB)
            change_focus(&app, -1);
    }

    app_free(&app);
    endwin();
    return 0;
}