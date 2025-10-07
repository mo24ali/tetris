#include <ncurses.h>  
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BOARD_W 10
#define BOARD_H 20
#define TET_SIZE 4


#define BASE_DELAY 800
#define LEVEL_SPEED_STEP 60 /* ms less per level */

/* Game state */
int board[BOARD_H][BOARD_W]; /* 0 = empty, >0 = block id */
int score = 0;
int level = 1;
int lines_cleared = 0;
int game_over = 0;
int paused = 0;

/* Tetrominos definitions: 7 pieces, each as 4x4 matrix (row-major) */
const int tetrominos[7][TET_SIZE*TET_SIZE] = {
    /* I */
    {0,0,0,0,
     1,1,1,1,
     0,0,0,0,
     0,0,0,0},
    /* J */
    {2,0,0,0,
     2,2,2,0,
     0,0,0,0,
     0,0,0,0},
    /* L */
    {0,0,3,0,
     3,3,3,0,
     0,0,0,0,
     0,0,0,0},
    /* O */
    {0,4,4,0,
     0,4,4,0,
     0,0,0,0,
     0,0,0,0},
    /* S */
    {0,5,5,0,
     5,5,0,0,
     0,0,0,0,
     0,0,0,0},
    /* T */
    {0,6,0,0,
     6,6,6,0,
     0,0,0,0,
     0,0,0,0},
    /* Z */
    {7,7,0,0,
     0,7,7,0,
     0,0,0,0,
     0,0,0,0}
};

/* Current falling piece */
typedef struct {
    int id; /* 1..7 */
    int shape[TET_SIZE*TET_SIZE];
    int x, y; /* x: column (0..BOARD_W-1), y: row (0..BOARD_H-1) top-left of 4x4 matrix */
} Piece;

Piece cur, next_piece;

/* ncurses windows */
WINDOW *win_board, *win_info;

/* helper: copy tetromino into piece */
void set_piece_from_template(Piece *p, int id) {
    p->id = id;
    memcpy(p->shape, tetrominos[id-1], sizeof(p->shape));
}

/* rotate piece shape clockwise (4x4) */
void rotate_piece_cw(int shape[TET_SIZE*TET_SIZE]) {
    int tmp[TET_SIZE*TET_SIZE];
    for (int r=0;r<TET_SIZE;r++){
        for (int c=0;c<TET_SIZE;c++){
            tmp[c*TET_SIZE + (TET_SIZE-1-r)] = shape[r*TET_SIZE + c];
        }
    }
    memcpy(shape, tmp, sizeof(int)*TET_SIZE*TET_SIZE);
}

/* rotate counterclockwise */
void rotate_piece_ccw(int shape[TET_SIZE*TET_SIZE]) {
    int tmp[TET_SIZE*TET_SIZE];
    for (int r=0;r<TET_SIZE;r++){
        for (int c=0;c<TET_SIZE;c++){
            tmp[(TET_SIZE-1-c)*TET_SIZE + r] = shape[r*TET_SIZE + c];
        }
    }
    memcpy(shape, tmp, sizeof(int)*TET_SIZE*TET_SIZE);
}

/* collision check: returns 1 if collides or out of bounds */
int collides(const Piece *p, int board_state[BOARD_H][BOARD_W]) {
    for (int r=0;r<TET_SIZE;r++){
        for (int c=0;c<TET_SIZE;c++){
            int val = p->shape[r*TET_SIZE + c];
            if (!val) continue;
            int by = p->y + r;
            int bx = p->x + c;
            if (bx < 0 || bx >= BOARD_W || by >= BOARD_H) return 1;
            if (by >= 0) { /* allow piece partly above top */
                if (board_state[by][bx]) return 1;
            }
        }
    }
    return 0;
}

/* lock piece into board */
void lock_piece(const Piece *p) {
    for (int r=0;r<TET_SIZE;r++){
        for (int c=0;c<TET_SIZE;c++){
            int val = p->shape[r*TET_SIZE + c];
            if (!val) continue;
            int by = p->y + r;
            int bx = p->x + c;
            if (by >= 0 && by < BOARD_H && bx >= 0 && bx < BOARD_W) {
                board[by][bx] = p->id;
            }
        }
    }
}

/* clear full lines and update score */
int clear_full_lines() {
    int cleared = 0;
    for (int r = BOARD_H-1; r >= 0; r--) {
        int full = 1;
        for (int c=0;c<BOARD_W;c++){
            if (!board[r][c]) { full = 0; break; }
        }
        if (full) {
            cleared++;
            /* move everything above down */
            for (int rr = r; rr > 0; rr--) {
                memcpy(board[rr], board[rr-1], sizeof(board[0]));
            }
            memset(board[0], 0, sizeof(board[0]));
            r++; /* re-check same row index after shift */
        }
    }
    if (cleared > 0) {
        lines_cleared += cleared;
        /* scoring: classic: 1->40*(level+1), 2->100*(level+1), 3->300*(level+1), 4->1200*(level+1) */
        int points = 0;
        switch (cleared) {
            case 1: points = 40 * (level+1); break;
            case 2: points = 100 * (level+1); break;
            case 3: points = 300 * (level+1); break;
            case 4: points = 1200 * (level+1); break;
            default: points = 1200 * cleared * (level+1); break;
        }
        score += points;
        /* level up every 10 lines */
        level = 1 + lines_cleared / 10;
    }
    return cleared;
}

/* spawn new piece from next */
void spawn_next_piece() {
    cur = next_piece;
    /* initial position: x centered, y = -1 (so piece can be partially above top) */
    cur.x = (BOARD_W / 2) - (TET_SIZE/2);
    cur.y = -1;
    /* generate new next */
    int id = (rand() % 7) + 1;
    set_piece_from_template(&next_piece, id);
}

/* initialize game */
void init_game() {
    memset(board, 0, sizeof(board));
    score = 0;
    level = 1;
    lines_cleared = 0;
    game_over = 0;
    paused = 0;
    /* seed random */
    srand((unsigned)time(NULL));
    int id = (rand() % 7) + 1;
    set_piece_from_template(&next_piece, id);
    spawn_next_piece();
}

/* draw board and current piece */
void draw_game() {
    werase(win_board);
    box(win_board, 0, 0);

    /* draw board cells */
    for (int r=0;r<BOARD_H;r++){
        for (int c=0;c<BOARD_W;c++){
            int val = board[r][c];
            if (val) {
                mvwprintw(win_board, 1 + r, 1 + c*2, "[]");
            } else {
                mvwprintw(win_board, 1 + r, 1 + c*2, "  ");
            }
        }
    }

    /* draw current piece */
    for (int r=0;r<TET_SIZE;r++){
        for (int c=0;c<TET_SIZE;c++){
            int val = cur.shape[r*TET_SIZE + c];
            if (!val) continue;
            int by = cur.y + r;
            int bx = cur.x + c;
            if (by >= 0 && by < BOARD_H && bx >= 0 && bx < BOARD_W) {
                mvwprintw(win_board, 1 + by, 1 + bx*2, "[]");
            }
        }
    }

    /* grid lines (optional) */
    //for (int r=0;r<=BOARD_H;r++) mvwprintw(win_board, 1 + r, 1 + BOARD_W*2, "|");

    wrefresh(win_board);

    /* info window */
    werase(win_info);
    box(win_info, 0, 0);
    mvwprintw(win_info, 1, 2, "Score: %d", score);
    mvwprintw(win_info, 2, 2, "Level: %d", level);
    mvwprintw(win_info, 3, 2, "Lines: %d", lines_cleared);
    mvwprintw(win_info, 5, 2, "Next:");

    /* draw next piece small */
    for (int r=0;r<TET_SIZE;r++){
        for (int c=0;c<TET_SIZE;c++){
            int val = next_piece.shape[r*TET_SIZE + c];
            if (val) {
                mvwprintw(win_info, 7 + r, 2 + c*2, "[]");
            } else {
                mvwprintw(win_info, 7 + r, 2 + c*2, "  ");
            }
        }
    }

    mvwprintw(win_info, 13, 2, "Controls:");
    mvwprintw(win_info, 14, 2, "<- -> : move");
    mvwprintw(win_info, 15, 2, "z/x : rotate");
    mvwprintw(win_info, 16, 2, "down : soft drop");
    mvwprintw(win_info, 17, 2, "space : hard drop");
    mvwprintw(win_info, 18, 2, "p : pause  q : quit");

    if (paused) {
        mvwprintw(win_info, 20, 2, "PAUSED");
    }
    if (game_over) {
        mvwprintw(win_info, 20, 2, "GAME OVER! q to quit");
    }

    wrefresh(win_info);
}

/* attempt to rotate with simple wall-kick (shift left/right if collision) */
void attempt_rotate(Piece *p, int cw) {
    int backup[TET_SIZE*TET_SIZE];
    memcpy(backup, p->shape, sizeof(backup));
    if (cw) rotate_piece_cw(p->shape); else rotate_piece_ccw(p->shape);
    if (!collides(p, board)) return;

    /* try kick offsets */
    const int kicks[] = { -1, 1, -2, 2 };
    for (int i=0;i<sizeof(kicks)/sizeof(kicks[0]);i++){
        p->x += kicks[i];
        if (!collides(p, board)) return;
        p->x -= kicks[i];
    }
    /* fail -> restore */
    memcpy(p->shape, backup, sizeof(backup));
}

/* move piece horizontally if possible */
void try_move_h(Piece *p, int dx) {
    p->x += dx;
    if (collides(p, board)) p->x -= dx;
}

/* soft drop; returns 1 if moved down */
int soft_drop(Piece *p) {
    p->y += 1;
    if (collides(p, board)) {
        p->y -= 1;
        return 0;
    }
    return 1;
}

/* hard drop: drop until can't */
void hard_drop(Piece *p) {
    while (1) {
        p->y += 1;
        if (collides(p, board)) {
            p->y -= 1;
            lock_piece(p);
            return;
        }
    }
}

void init_ncurses() {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(0); /* getch non-blocking */
    start_color();
    use_default_colors();

    int winw = 2 + BOARD_W*2 + 2; /* border + cells */
    int winh = 2 + BOARD_H + 2;
    int infow = 30;
    int infoh = winh;

    int startx = (COLS - (winw + infow + 2)) / 2;
    if (startx < 0) startx = 0;
    int starty = (LINES - winh) / 2;
    if (starty < 0) starty = 0;

    win_board = newwin(winh, winw, starty, startx);
    win_info = newwin(infoh, infow, starty, startx + winw + 1);
}

/* cleanup ncurses */
void end_ncurses() {
    delwin(win_board);
    delwin(win_info);
    endwin();
}

/* main */
int main() {
    init_ncurses();
    init_game();

    /* frame timing */
    struct timespec last_tick;
    clock_gettime(CLOCK_MONOTONIC, &last_tick);
    long accumulated = 0;

    draw_game();

    int ch;
    while (!game_over) {
        /* input */
        ch = getch();
        if (ch != ERR) {
            if (ch == 'q' || ch == 'Q') {
                break;
            } else if (ch == 'p' || ch == 'P') {
                paused = !paused;
            }
            if (!paused) {
                if (ch == KEY_LEFT) { try_move_h(&cur, -1); }
                else if (ch == KEY_RIGHT) { try_move_h(&cur, 1); }
                else if (ch == KEY_DOWN) { /* soft drop */ if (soft_drop(&cur)) { score += 1; } }
                else if (ch == 'z' || ch == 'Z') { attempt_rotate(&cur, 0); }
                else if (ch == 'x' || ch == 'X') { attempt_rotate(&cur, 1); }
                else if (ch == ' ') {
                    hard_drop(&cur);
                    int cleared = clear_full_lines();
                    if (cleared) score += 10*cleared;
                    spawn_next_piece();
                    if (collides(&cur, board)) { game_over = 1; }
                }
            }
        }

        /* tick timing */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long delta_ms = (now.tv_sec - last_tick.tv_sec) * 1000 + (now.tv_nsec - last_tick.tv_nsec) / 1000000;
        if (delta_ms <= 0) delta_ms = 1;
        last_tick = now;

        if (!paused) {
            /* compute delay based on level */
            int delay = BASE_DELAY - (level-1) * LEVEL_SPEED_STEP;
            if (delay < 80) delay = 80;

            static long accumulator = 0;
            accumulator += delta_ms;
            if (accumulator >= delay) {
                accumulator = 0;
                /* try move down */
                cur.y += 1;
                if (collides(&cur, board)) {
                    cur.y -= 1;
                    /* lock piece */
                    lock_piece(&cur);
                    int cleared = clear_full_lines();
                    if (cleared) score += 100 * cleared;
                    /* spawn next */
                    spawn_next_piece();
                    if (collides(&cur, board)) {
                        game_over = 1;
                    }
                }
            }
        }

        draw_game();

        /* sleep a tiny bit to lower CPU */
        struct timespec sleep_for = {0, 10 * 1000000}; /* 10ms */
        nanosleep(&sleep_for, NULL);
    }

    /* final screen */
    werase(win_info);
    box(win_info, 0, 0);
    mvwprintw(win_info, 2, 2, "GAME OVER");
    mvwprintw(win_info, 4, 2, "Score: %d", score);
    mvwprintw(win_info, 5, 2, "Lines: %d", lines_cleared);
    mvwprintw(win_info, 7, 2, "Press q to quit");
    wrefresh(win_info);

    while (1) {
        int c = getch();
        if (c == 'q' || c == 'Q') break;
        usleep(100000);
    }

    end_ncurses();
    return 0;
}