
#define _POSIX_C_SOURCE 200809L


#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <locale.h>
#include <netinet/tcp.h>
#include "terminal_ui.h"


#define BOARD_SIZE 10
#define PLAYER_COUNT 2
#define KING_MAX_LIVES 3
#define SHIELD_LIMIT 10
#define CANNONS_PER_TURN 2

typedef struct {
    int row;
    int col;
} Position;

typedef enum {
    EMPTY,
    KING,
    SHIELD,
    CANNON
} Piece;

typedef struct {
    Piece piece;
    int owner;
} Cell;

typedef enum {
    STEP = 1,
    SKEW,
    LEAP,
    AXIS,
    JASMIN
} MoveType;

typedef enum {
    TOOL_NONE,
    TOOL_SHIELD,
    TOOL_CANNON,
    TOOL_KING,
    TOOL_NUKE
} Tool;

typedef struct {
    Cell board[BOARD_SIZE][BOARD_SIZE];

    unsigned char crater[BOARD_SIZE][BOARD_SIZE];
    unsigned char breach[BOARD_SIZE][BOARD_SIZE];

    Position king[PLAYER_COUNT];

    int lives[PLAYER_COUNT];
    int shields_placed[PLAYER_COUNT];
    int enemy_shields_destroyed[PLAYER_COUNT];
    int cannon_placements[PLAYER_COUNT];

    int nuke_used[PLAYER_COUNT];
    Position nuke_target[PLAYER_COUNT];

    int moves_remaining[PLAYER_COUNT][6];
    int king_moved[PLAYER_COUNT];
} Game;

/* ==================== NETWORK PACKETS ==================== */

enum {
    PKT_STATE = 1,
    PKT_COMMAND = 2,
    PKT_FRAME = 3
};

enum {
    ACT_KING = 1,
    ACT_PLACE = 2,
    ACT_UNDO = 3,
    ACT_END = 4,
    ACT_QUIT = 5
};

typedef struct {
    int32_t kind;
    int32_t player;
    int32_t current;
    int32_t phase;
    int32_t round;
    int32_t seconds;
    int32_t winner;

    int32_t action;
    int32_t tool;
    int32_t movement;
    int32_t row;
    int32_t col;

    int32_t bullets[4];
    int32_t bullet_count;

    int32_t impacts[18];
    int32_t impact_count;

    char note[180];

    Game state;
} Packet;

/* ==================== TERMINAL COLORS ==================== */

#define RESET "\033[0m"

#define GRAY_BG "\033[48;5;250m\033[30m"
#define WHITE_BG "\033[48;5;231m\033[30m"
#define YELLOW_BG "\033[48;5;228m\033[30m"
#define BLACK_BG "\033[48;5;234m\033[97m"
#define BULLET_BG "\033[48;5;208m\033[30m"
#define LEGAL_BG "\033[48;5;159m\033[30m"
#define CHARGE_BG "\033[48;5;214m\033[30m"

#define BUTTON_BG "\033[48;5;238m\033[97m"
#define SELECT_BG "\033[48;5;228m\033[30m"

/* ==================== UI STRUCTURES ==================== */

typedef struct {
    int x;
    int y;
    int width;
    int value;
} HitBox;

typedef struct {
    int width;
    int height;

    int cell_width;
    int cell_height;

    int grid_x;
    int grid_y;

    int first_row;
    int first_col;

    int rows_visible;
    int cols_visible;

    int footer_y;

    HitBox actions[6];
    HitBox moves[5];
} Layout;

/* ==================== GLOBAL VARIABLES ==================== */

static struct termios saved_terminal;
static int terminal_active = 0;

static int server_socket = -1;
static int running = 1;
static int have_state = 0;

static Packet latest;
/* The client advances the countdown locally between server updates. */
static struct timespec received_at;
static int previous_timer = -1;

static int seconds_left(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - received_at.tv_sec) +
                     (now.tv_nsec - received_at.tv_nsec) / 1000000000.0;
    int left = latest.seconds - (int)elapsed;
    return left < 0 ? 0 : left;
}


static Tool selected_tool = TOOL_NONE;
static MoveType selected_move = STEP;

static int view_row = 0;
static int view_col = 0;

static Layout layout;

static char local_message[160] = "";

static const char *action_names[6] = {
    "SHIELD",
    "CANNON",
    "MOVE",
    "NUKE",
    "BACK",
    "CANCEL"
};

static const char *move_names[5] = {
    "STEP",
    "SKEW",
    "LEAP",
    "AXIS",
    "JASMIN"
};

/* ==================== TERMINAL SETUP ==================== */

static void restore_terminal(void) {
    if (!terminal_active) {
        return;
    }

    fputs(
        "\033[?1000l"
        "\033[?1049l"
        "\033[?1006l"
        "\033[?25h"
        "\033[0m",
        stdout
    );

    fflush(stdout);

    tcsetattr(
        STDIN_FILENO,
        TCSANOW,
        &saved_terminal
    );

    terminal_active = 0;
}

static void handle_signal(int signal_number) {
    (void)signal_number;

    running = 0;
    restore_terminal();

    _exit(0);
}

static int initialize_terminal(void) {
    if (!isatty(STDIN_FILENO) ||
        !isatty(STDOUT_FILENO)) {

        fprintf(
            stderr,
            "Please run the client in a terminal.\n"
        );

        return 0;
    }

    if (tcgetattr(
            STDIN_FILENO,
            &saved_terminal
        ) < 0) {
        return 0;
    }

    struct termios settings = saved_terminal;

    settings.c_lflag &=
        (tcflag_t)~(ICANON | ECHO);

    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;

    if (tcsetattr(
            STDIN_FILENO,
            TCSANOW,
            &settings
        ) < 0) {
        return 0;
    }

    terminal_active = 1;

    /* Enable terminal mouse reporting. */

    fputs(
        "\033[?1049h"
        "\033[2J\033[H"
        "\033[?1000h"
        "\033[?1006h"
        "\033[?25l",
        stdout
    );

    fflush(stdout);

    return 1;
}

static void cursor_at(int row, int col) {
    printf("\033[%d;%dH", row, col);
}

static void write_line(
    int row,
    const char *text
) {
    if (row < 1 || row > layout.height) {
        return;
    }

    cursor_at(row, 1);

    printf(
        "\033[2K%.*s",
        layout.width,
        text
    );
}

/* ==================== NETWORK UTILITIES ==================== */

static int send_all(
    int socket_fd,
    const void *data,
    size_t bytes
) {
    const char *buffer = data;

    while (bytes > 0) {
        ssize_t sent = send(
            socket_fd,
            buffer,
            bytes,
            MSG_NOSIGNAL
        );

        if (sent < 0 && errno == EINTR) {
            continue;
        }

        if (sent <= 0) {
            return 0;
        }

        buffer += sent;
        bytes -= (size_t)sent;
    }

    return 1;
}

static int receive_all(
    int socket_fd,
    void *data,
    size_t bytes
) {
    char *buffer = data;

    while (bytes > 0) {
        ssize_t received = recv(
            socket_fd,
            buffer,
            bytes,
            0
        );

        if (received < 0 && errno == EINTR) {
            continue;
        }

        if (received <= 0) {
            return 0;
        }

        buffer += received;
        bytes -= (size_t)received;
    }

    return 1;
}

/* ==================== CONNECT TO SERVER ==================== */

static int connect_to_server(
    const char *host,
    const char *port
) {
    struct addrinfo hints = {0};
    struct addrinfo *addresses = NULL;
    struct addrinfo *entry;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int result = getaddrinfo(
        host,
        port,
        &hints,
        &addresses
    );

    if (result != 0) {
        fprintf(
            stderr,
            "Address error: %s\n",
            gai_strerror(result)
        );

        return -1;
    }

    int socket_fd = -1;

    for (
        entry = addresses;
        entry != NULL;
        entry = entry->ai_next
    ) {
        socket_fd = socket(
            entry->ai_family,
            entry->ai_socktype,
            entry->ai_protocol
        );

        if (socket_fd < 0) {
            continue;
        }

        if (connect(
                socket_fd,
                entry->ai_addr,
                entry->ai_addrlen
            ) == 0) {
            break;
        }

        close(socket_fd);

        socket_fd = -1;
    }

    freeaddrinfo(addresses);

    return socket_fd;
}

/* ==================== SEND PLAYER ACTION ==================== */

static void send_action(
    int action,
    Tool tool,
    MoveType movement,
    int row,
    int col
) {
    Packet command = {0};

    command.kind = PKT_COMMAND;
    command.action = action;

    command.tool = tool;
    command.movement = movement;

    command.row = row;
    command.col = col;

    if (!send_all(
            server_socket,
            &command,
            sizeof(command)
        )) {
        snprintf(
            local_message,
            sizeof(local_message),
            "Disconnected from server."
        );

        running = 0;
    }
}

/* ==================== TERMINAL DIMENSIONS ==================== */

static void get_terminal_size(
    int *width,
    int *height
) {
    struct winsize size = {0};

    if (ioctl(
            STDOUT_FILENO,
            TIOCGWINSZ,
            &size
        ) == 0 &&
        size.ws_col > 0 &&
        size.ws_row > 0) {

        *width = size.ws_col;
        *height = size.ws_row;

    } else {
        *width = 80;
        *height = 24;
    }
}

static void set_hitbox(
    HitBox *box,
    int x,
    int y,
    int width,
    int value
) {
    box->x = x;
    box->y = y;
    box->width = width;
    box->value = value;
}

/* ==================== BOARD LAYOUT ==================== */

static void calculate_layout(void) {
    get_terminal_size(
        &layout.width,
        &layout.height
    );

    int width = layout.width;
    int height = layout.height;

    layout.cell_width = 7;
    layout.cell_height = 3;

    layout.grid_x = 5;
    layout.grid_y = 9;

    /* Action buttons */

    for (int i = 0; i < 6; i++) {
        int slot = width >= 66 ? 11 : 9;

        int per_line = width >= 66
            ? 6
            : (width >= 39 ? 4 : 2);

        set_hitbox(
            &layout.actions[i],
            2 + (i % per_line) * slot,
            4 + i / per_line,
            slot - 1,
            i
        );
    }

    /* Movement buttons */

    for (int i = 0; i < 5; i++) {
        int per_line = width >= 55
            ? 5
            : (width >= 32 ? 3 : 2);

        int slot = width >= 55 ? 11 : 9;

        set_hitbox(
            &layout.moves[i],
            2 + (i % per_line) * slot,
            6 + i / per_line,
            slot - 1,
            i + 1
        );
    }

    int available_width =
        width - layout.grid_x - 1;

    int available_height =
        height - layout.grid_y - 2;

    if (available_width < 1) {
        available_width = 1;
    }

    if (available_height < 1) {
        available_height = 1;
    }

    layout.cols_visible =
        available_width / (layout.cell_width + 1);

    layout.rows_visible =
        available_height / (layout.cell_height + 1);

    if (layout.cols_visible < 1) {
        layout.cols_visible = 1;
    }

    if (layout.rows_visible < 1) {
        layout.rows_visible = 1;
    }

    if (layout.cols_visible > BOARD_SIZE) {
        layout.cols_visible = BOARD_SIZE;
    }

    if (layout.rows_visible > BOARD_SIZE) {
        layout.rows_visible = BOARD_SIZE;
    }

    if (view_row >
        BOARD_SIZE - layout.rows_visible) {
        view_row =
            BOARD_SIZE - layout.rows_visible;
    }

    if (view_col >
        BOARD_SIZE - layout.cols_visible) {
        view_col =
            BOARD_SIZE - layout.cols_visible;
    }

    if (view_row < 0) {
        view_row = 0;
    }

    if (view_col < 0) {
        view_col = 0;
    }

    layout.first_row = view_row;
    layout.first_col = view_col;

    layout.footer_y =
        layout.grid_y +
        layout.rows_visible *
        (layout.cell_height + 1) + 1;
}

/* ==================== ATTACK ANIMATIONS ==================== */

static int has_projectile(int square) {
    if (latest.kind != PKT_FRAME) {
        return 0;
    }

    for (
        int i = 0;
        i < latest.bullet_count && i < 4;
        i++
    ) {
        if (latest.bullets[i] == square) {
            return 1;
        }
    }

    return 0;
}

static int has_impact(int square) {
    if (latest.kind != PKT_FRAME) {
        return 0;
    }

    for (
        int i = 0;
        i < latest.impact_count && i < 18;
        i++
    ) {
        if (latest.impacts[i] == square) {
            return 1;
        }
    }

    return 0;
}

/* Draw movement hints on the client's own visible battlefield. */
static int legal_destination(int row, int col) {
    if (!have_state || latest.kind == PKT_FRAME ||
        latest.phase != 1 || latest.current != latest.player ||
        selected_tool != TOOL_KING) return 0;
    int p = latest.player;
    Position k = latest.state.king[p];
    if (k.row < 0 || k.col < 0 || latest.state.king_moved[p] ||
        (p == 0 ? row >= 5 : row < 5) ||
        latest.state.board[row][col].piece != EMPTY) return 0;
    if (latest.state.breach[row][col] &&
        !latest.state.crater[row][col]) return 0;
    int dr = row - k.row, dc = col - k.col;
    int a = dr < 0 ? -dr : dr;
    int b = dc < 0 ? -dc : dc;
    if (!a && !b) return 0;
    if (selected_move != STEP &&
        latest.state.moves_remaining[p][selected_move] <= 0) return 0;
    int valid = 0;
    switch (selected_move) {
        case STEP:   valid = a + b == 1; break;
        case SKEW:   valid = a == 2 && b == 2; break;
        case LEAP:   valid = (a == 2 && b == 1) || (a == 1 && b == 2); break;
        case AXIS:   valid = (a == 2 && !b) || (!a && b == 2); break;
        case JASMIN: valid = a == b || !a || !b; break;
        default: break;
    }
    if (!valid) return 0;
    if (selected_move != LEAP) {
        int sy = (dr > 0) - (dr < 0), sx = (dc > 0) - (dc < 0);
        for (int y = k.row + sy, x = k.col + sx;
             y != row || x != col; y += sy, x += sx) {
            if (latest.state.board[y][x].piece != EMPTY) return 0;
        }
    }
    return 1;
}

static int is_nuke_frame(void) {
    return latest.kind == PKT_FRAME && latest.tool == TOOL_NUKE;
}

/* ==================== BOARD COLORS ==================== */

static const char *square_color(
    int row,
    int col
) {
    int square = row * BOARD_SIZE + col;

    if (has_impact(square)) {
        return is_nuke_frame() && latest.action == 1 ? CHARGE_BG : YELLOW_BG;
    }

    if (has_projectile(square)) {
        return BULLET_BG;
    }

    if (latest.state.crater[row][col]) {
        return legal_destination(row, col)
            ? "\033[48;5;214m\033[30m"
            : "\033[48;5;130m\033[97m";
    }

    if (latest.state.breach[row][col]) return BLACK_BG;
    if (legal_destination(row, col)) return LEGAL_BG;

    /* Only highlight your own territory when it is your turn. */
    int owner = row < 5 ? 0 : 1;
    if (latest.kind != PKT_FRAME && latest.phase != 2 &&
        latest.current == latest.player && owner == latest.player)
        return WHITE_BG;

    return GRAY_BG;
}

/* ==================== PIECE LABELS ==================== */

static const char *square_label(
    int row,
    int col
) {
    int square = row * BOARD_SIZE + col;

    if (has_impact(square)) {
        return is_nuke_frame() && latest.action == 1 ? "..." : "BOOM";
    }

    if (has_projectile(square)) {
        return latest.action == 2 ? "!" : "^";
    }

    if (legal_destination(row, col)) return "*";

    Cell cell = latest.state.board[row][col];

    if (cell.piece == KING) {
        return MB_CUR_MAX > 1 ? "👑" : "K";
    }

    if (cell.piece == SHIELD) {
        return MB_CUR_MAX > 1 ? "🛡️" : "[S]";
    }

    if (cell.piece == CANNON) {
        return MB_CUR_MAX > 1 ? "💣" : "C";
    }

    if (latest.state.crater[row][col]) {
        return "###";
    }

    if (latest.state.breach[row][col]) {
        return "...";
    }

    return "";
}

/* Emoji labels occupy two terminal columns, not their UTF-8 byte count. */
static int label_columns(const char *label) {
    if (!strcmp(label, "👑") || !strcmp(label, "🛡️") ||
        !strcmp(label, "💣")) return 2;
    return (int)strlen(label);
}

/* ==================== BOARD BORDERS ==================== */

static void draw_border(int row) {
    if (row < 1 || row > layout.height) {
        return;
    }

    cursor_at(row, layout.grid_x);

    putchar('+');

    for (
        int col = 0;
        col < layout.cols_visible;
        col++
    ) {
        for (
            int j = 0;
            j < layout.cell_width;
            j++
        ) {
            putchar('-');
        }

        putchar('+');
    }
}

/* ==================== DRAW BOARD ==================== */

static void draw_board(void) {
    int y = layout.grid_y;

    /* Column labels A-J */

    cursor_at(y - 1, 1);

    printf("     ");

    for (
        int col = 0;
        col < layout.cols_visible;
        col++
    ) {
        int board_col =
            layout.first_col + col;

        printf(
            "%*c%*s",
            (layout.cell_width + 1) / 2,
            'A' + board_col,
            (layout.cell_width + 1) / 2,
            ""
        );
    }

    draw_border(y);

    /* Print board rows */

    for (
        int r = 0;
        r < layout.rows_visible;
        r++
    ) {
        int board_row =
            layout.first_row + r;

        for (
            int inner = 0;
            inner < layout.cell_height;
            inner++
        ) {
            int line = y + 1 + inner;

            if (line > layout.height) {
                return;
            }

            cursor_at(line, 1);

            if (inner ==
                layout.cell_height / 2) {
                printf(
                    "%3d ",
                    board_row + 1
                );

            } else {
                printf("    ");
            }

            putchar('|');

            for (
                int c = 0;
                c < layout.cols_visible;
                c++
            ) {
                int board_col =
                    layout.first_col + c;

                const char *label = "";

                if (inner ==
                    layout.cell_height / 2) {
                    label = square_label(
                        board_row,
                        board_col
                    );
                }

                const char *background =
                    square_color(
                        board_row,
                        board_col
                    );

                int label_length =
                    label_columns(label);

                if (label_length > layout.cell_width) {
                    label = "";
                    label_length = 0;
                }


                int cell_x = layout.grid_x + 1 +
                             c * (layout.cell_width + 1);
                cursor_at(line, cell_x);
                fputs(background, stdout);
                for (int k = 0; k < layout.cell_width; k++) {
                    putchar(' ');
                }
                if (label_length > 0) {
                    int padding = (layout.cell_width - label_length) / 2;
                    cursor_at(line, cell_x + padding);
                    fputs(label, stdout);
                }
                fputs(RESET, stdout);
                cursor_at(line, cell_x + layout.cell_width);
                putchar('|');
            }
        }

        y += layout.cell_height + 1;

        draw_border(y);
    }
}

/* ==================== DRAW BUTTONS ==================== */

static void draw_button(
    const HitBox *box,
    const char *name,
    int active
) {
    if (box->y > layout.height ||
        box->x > layout.width) {
        return;
    }

    cursor_at(box->y, box->x);

    fputs(
        active ? SELECT_BG : BUTTON_BG,
        stdout
    );

    printf(
        "%-*.*s",
        box->width,
        box->width,
        name
    );

    fputs(RESET, stdout);
}

static void draw_side_clock(void) {
    int x = layout.grid_x +
        layout.cols_visible * (layout.cell_width + 1) + 3;
    if (!have_state || x + 17 > layout.width || layout.grid_y + 5 > layout.height)
        return;
    int left = seconds_left();
    cursor_at(layout.grid_y + 1, x);
    printf("\033[1m TURN CLOCK \033[0m");
    cursor_at(layout.grid_y + 2, x);
    printf(left <= 10 ? "\033[1;31m  %02d seconds  \033[0m" :
                       "  %02d seconds  ", left);
    cursor_at(layout.grid_y + 3, x);
    putchar('[');
    int filled = left * 12 / 60;
    for (int i = 0; i < 12; i++)
        putchar(i < filled ? '=' : ' ');
    putchar(']');
    cursor_at(layout.grid_y + 5, x);
    printf("ENTER: END TURN");
}

/* ==================== PLAYER TURN ==================== */

static int can_play(void) {
    return have_state &&
           latest.phase != 2 &&
           latest.current == latest.player;
}

/* A high-contrast banner without changing the board dimensions. */
static void draw_turn_status(const char *text) {
    cursor_at(2, 1);
    fputs("\033[2K", stdout);
    if (latest.kind == PKT_FRAME)
        fputs("\033[1;33m", stdout);
    else if (can_play())
        fputs("\033[1;30;107m", stdout);
    else
        fputs("\033[1m", stdout);
    printf("%-*.*s", layout.width > 1 ? layout.width - 1 : 1,
           layout.width > 1 ? layout.width - 1 : 1, text);
    fputs(RESET, stdout);
}

/* ==================== RESULT SCREEN ==================== */

static void draw_end_screen(void) {

    static const char *victory[] = {
        "oooooo     oooo ooooo   .oooooo.   ooooooooooooo   .oooooo.   ooooooooo.   oooooo   oooo",
        " `888.     .8'  `888'  d8P'  `Y8b  8'   888   `8  d8P'  `Y8b  `888   `Y88.  `888.   .8'",
        "  `888.   .8'    888  888               888      888      888  888   .d88'   `888. .8'",
        "   `888. .8'     888  888               888      888      888  888ooo88P'     `888.8'",
        "    `888.8'      888  888               888      888      888  888`88b.        `888'",
        "     `888'       888  `88b    ooo       888      `88b    d88'  888  `88b.       888",
        "      `8'       o888o  `Y8bood8P'      o888o      `Y8bood8P'  o888o  o888o     o888o"
    };

    static const char *defeat[] = {
        "oooooooooo.   oooooooooooo oooooooooooo oooooooooooo       .o.       ooooooooooooo",
        "`888'   `Y8b  `888'     `8 `888'     `8 `888'     `8      .888.      8'   888   `8",
        " 888      888  888          888          888             .8\"888.          888",
        " 888      888  888oooo8     888oooo8     888oooo8       .8' `888.         888",
        " 888      888  888    \"     888    \"     888    \"      .88ooo8888.        888",
        " 888     d88'  888       o  888          888       o  .8'     `888.       888",
        "o888bood8P'   o888ooooood8 o888o        o888ooooood8 o88o     o8888o     o888o"
    };
    
    static const char *draw[] = {
        "oooooooooo.   ooooooooo.         .o.       oooooo   oooooo     oooo",
        "`888'   `Y8b  `888   `Y88.      .888.       `888.    `888.     .8'",  
        " 888      888  888   .d88'     .8\"888.       `888.   .8888.   .8'",   
        " 888      888  888ooo88P'     .8' `888.       `888  .8'`888. .8'",    
        " 888      888  888`88b.      .88ooo8888.       `888.8'  `888.8'",     
        " 888     d88'  888  `88b.   .8'     `888.       `888'    `888'",      
        "o888bood8P'   o888o  o888o o88o     o8888o       `8'      `8'"                                                                                                        
    };

    const char **art;
    const char *color;
    const char *word;

    int art_height = 7;
    int art_width = 0;

    /* Determine the game result. */

    if (latest.winner == 2) {
        art = draw;
        word = "DRAW";
        color = "\033[1;93m";
    } else if (latest.winner == latest.player) {
        art = victory;
        word = "VICTORY";
        color = "\033[1;92m";
    } else {
        art = defeat;
        word = "DEFEAT";
        color = "\033[1;91m";
    }

    /* Get terminal dimensions. */

    int screen_width, screen_height;

    get_terminal_size(
        &screen_width,
        &screen_height
    );

    if (art != NULL) {
        for (int i = 0; i < art_height; i++) {
            int length = (int)strlen(art[i]);

            if (length > art_width) {
                art_width = length;
            }
        }
    }

    /* Clear the terminal. */

    fputs("\033[2J\033[H", stdout);

    /* Draw the large result text. */

    int bottom;

    if (art != NULL &&
        screen_width > art_width &&
        screen_height >= art_height + 5) {

        int x = (screen_width - art_width) / 2 + 1;
        int y = (screen_height - art_height - 3) / 2 + 1;

        fputs(color, stdout);

        for (int i = 0; i < art_height; i++) {
            cursor_at(y + i, x);
            printf("%s", art[i]);
        }

        fputs(RESET, stdout);

        bottom = y + art_height;

    } else {

        /* Fallback for smaller terminals or a draw. */

        int x = (screen_width - (int)strlen(word)) / 2 + 1;
        int y = screen_height / 2;

        if (x < 1) x = 1;
        if (y < 1) y = 1;

        cursor_at(y, x);
        printf("%s%s%s", color, word, RESET);

        bottom = y + 1;
    }

    /* Exit instruction. */

    const char *message = "Press ENTER or Q to exit.";

    int message_x =
        (screen_width - (int)strlen(message)) / 2 + 1;

    if (message_x < 1) message_x = 1;

    if (bottom + 2 <= screen_height) {
        cursor_at(bottom + 2, message_x);
        printf("\033[90m%.*s%s",
               screen_width, message, RESET);
    }

    fflush(stdout);
}


/* ==================== COMPLETE UI ==================== */

static void redraw(void) {
    calculate_layout();

    static int previous_width = -1, previous_height = -1;
    if (previous_width != layout.width || previous_height != layout.height) {
        fputs("\033[2J", stdout);
        previous_width = layout.width;
        previous_height = layout.height;
    }
    fputs("\033[?2026h\033[H", stdout);

    char text[256];

    if (!have_state) {
        write_line(
            1,
            "KINGS AND CANNONS | CONNECTING..."
        );

        fputs("\033[?2026l", stdout);
        fflush(stdout);
        return;
    }

    if (latest.phase == 2) {
        fputs("\033[?2026l", stdout);
        draw_end_screen();
        return;
    }

    int player = latest.player;

    /* Header */

    snprintf(
        text,
        sizeof(text),
        "KINGS AND CANNONS | ROUND %d | PLAYER %d",
        latest.round,
        player + 1
    );

    write_line(1, text);

    /* Attack animation banner or turn status. */

    if (latest.kind == PKT_FRAME && latest.tool == TOOL_NUKE) {
        snprintf(text, sizeof text,
                 latest.action == 1 ? "!!! NUKE CHARGING !!!" :
                 latest.action == 2 ? "!!! NUCLEAR BOOM BOOM !!!" :
                                      "!!! NUCLEAR AFTERSHOCK !!!");
    } else if (latest.kind == PKT_FRAME && latest.tool == TOOL_CANNON) {
        snprintf(text, sizeof text, "!!! CANNONS FIRING - BOOM !!!");
    } else if (latest.phase == 2) {
        if (latest.winner == 2) {
            snprintf(
                text,
                sizeof(text),
                "DRAW | Q: Quit"
            );

        } else {
            snprintf(
                text,
                sizeof(text),
                "GAME OVER | PLAYER %d WINS",
                latest.winner + 1
            );
        }

    } else if (can_play()) {
        snprintf(
            text,
            sizeof(text),
            ">>> YOUR TURN <<< | %02ds | ENTER: END TURN",
            seconds_left()
        );

    } else {
        snprintf(
            text,
            sizeof(text),
            "WAITING - PLAYER %d IS PLAYING | %02ds",
            latest.current + 1,
            seconds_left()
        );
    }

    draw_turn_status(text);

    snprintf(
        text,
        sizeof(text),
        "LIVES %d/3 | SHIELDS %d/10 | CANNONS %d/2 | NUKE %s (%d/3)",
        latest.state.lives[player],
        SHIELD_LIMIT -
            latest.state.shields_placed[player],
        CANNONS_PER_TURN -
            latest.state.cannon_placements[player],
        latest.state.nuke_used[player]
            ? "USED"
            : latest.state.enemy_shields_destroyed[player] >= 3
                ? "READY"
                : "LOCKED",
        latest.state.enemy_shields_destroyed[player]
    );

    write_line(3, text);

    /* Toolbar */

    for (int i = 0; i < 6; i++) {
        int active =
            can_play() &&
            i < 4 &&
            selected_tool == (Tool)(i + 1);

        draw_button(
            &layout.actions[i],
            action_names[i],
            active
        );
    }

    /* Movement choices */

    for (int i = 0; i < 5; i++) {
        int active =
            selected_tool == TOOL_KING &&
            selected_move == (MoveType)(i + 1);

        draw_button(
            &layout.moves[i],
            move_names[i],
            active
        );
    }

    /* Battlefield */

    draw_board();
    draw_side_clock();

    /* Footer */

    snprintf(
        text,
        sizeof(text),
        "%s",
        local_message[0]
            ? local_message
            : latest.note
    );

    write_line(
        layout.footer_y,
        text
    );

    if (layout.footer_y + 1 <=
        layout.height) {
        write_line(
            layout.footer_y + 1,
            "Arrows/wheel: Scroll | ENTER: End Turn | Q: Quit"
        );
    }

    fputs("\033[?2026l", stdout);
    fflush(stdout);
}

static void update_timer_line(void) {
    if (!have_state || latest.kind == PKT_FRAME || latest.phase == 2)
        return;
    char line[150];
    if (can_play())
        snprintf(line, sizeof line,
                 ">>> YOUR TURN <<< | %02ds | ENTER: END TURN",
                 seconds_left());
    else
        snprintf(line, sizeof line,
                 "WAITING - PLAYER %d IS PLAYING | %02ds",
                 latest.current + 1, seconds_left());
    fputs("\033[?2026h", stdout);
    draw_turn_status(line);
    draw_side_clock();
    fputs("\033[?2026l", stdout);
    fflush(stdout);
}

/* ==================== MOUSE HITBOX ==================== */

static int inside_hitbox(
    const HitBox *box,
    int x,
    int y
) {
    return y == box->y &&
           x >= box->x &&
           x < box->x + box->width;
}

/* ==================== BOARD CLICK ==================== */

static void board_click(
    int row,
    int col
) {
    if (!can_play()) {
        return;
    }

    /* Place initial king */

    if (latest.phase == 0) {
        send_action(
            ACT_KING,
            TOOL_NONE,
            STEP,
            row,
            col
        );

        return;
    }

    if (selected_tool == TOOL_NONE) {
        snprintf(
            local_message,
            sizeof(local_message),
            "Choose SHIELD, CANNON, MOVE or NUKE first."
        );

        return;
    }

    send_action(
        ACT_PLACE,
        selected_tool,
        selected_move,
        row,
        col
    );
}

/* ==================== HANDLE MOUSE CLICK ==================== */

static void mouse_click(int x, int y) {
    if (!have_state) {
        return;
    }

    /* Toolbar controls */

    if (latest.phase != 2 && can_play()) {
        for (int i = 0; i < 6; i++) {
            if (!inside_hitbox(
                    &layout.actions[i],
                    x,
                    y
                )) {
                continue;
            }

            local_message[0] = '\0';

            if (i == 4) {
                /* Undo previous action */

                send_action(
                    ACT_UNDO,
                    TOOL_NONE,
                    STEP,
                    -1,
                    -1
                );

            } else if (i == 5) {
                /* Cancel tool selection */

                selected_tool = TOOL_NONE;

            } else {
                /* Select a tool */

                selected_tool = (Tool)(i + 1);
            }

            return;
        }

        /* King movement buttons */

        for (int i = 0; i < 5; i++) {
            if (!inside_hitbox(
                    &layout.moves[i],
                    x,
                    y
                )) {
                continue;
            }

            selected_move = (MoveType)(i + 1);
            selected_tool = TOOL_KING;

            local_message[0] = '\0';

            return;
        }
    }

    /* Convert mouse coordinates to board square. */

    int first_y = layout.grid_y + 1;
    int first_x = layout.grid_x + 1;

    if (y < first_y || x < first_x) {
        return;
    }

    int dy = y - first_y;
    int dx = x - first_x;

    int row_in_view =
        dy / (layout.cell_height + 1);

    int col_in_view =
        dx / (layout.cell_width + 1);

    if (row_in_view >= layout.rows_visible ||
        col_in_view >= layout.cols_visible) {
        return;
    }

    /* Ignore clicks on grid lines. */

    if (dy % (layout.cell_height + 1) >=
            layout.cell_height ||
        dx % (layout.cell_width + 1) >=
            layout.cell_width) {
        return;
    }

    board_click(
        layout.first_row + row_in_view,
        layout.first_col + col_in_view
    );
}

/* ==================== SCROLL BOARD ==================== */

static void scroll_board(
    int row_change,
    int col_change
) {
    view_row += row_change;
    view_col += col_change;

    if (view_row < 0) {
        view_row = 0;
    }

    if (view_col < 0) {
        view_col = 0;
    }

    if (view_row >
        BOARD_SIZE - layout.rows_visible) {
        view_row =
            BOARD_SIZE - layout.rows_visible;
    }

    if (view_col >
        BOARD_SIZE - layout.cols_visible) {
        view_col =
            BOARD_SIZE - layout.cols_visible;
    }

    if (view_row < 0) {
        view_row = 0;
    }

    if (view_col < 0) {
        view_col = 0;
    }
}

/* ==================== KEYBOARD INPUT ==================== */

static int parser_state = 0;
static int parser_length = 0;

static char parser_buffer[64];

static void handle_key(char key) {
    if (key == 'q' || key == 'Q') {
        send_action(
            ACT_QUIT,
            TOOL_NONE,
            STEP,
            -1,
            -1
        );

        running = 0;

    } else if (
        (key == 'e' ||
         key == 'E' ||
         key == '\r' ||
         key == '\n') &&
        can_play() &&
        latest.phase == 1
    ) {
        selected_tool = TOOL_NONE;

        send_action(
            ACT_END,
            TOOL_NONE,
            STEP,
            -1,
            -1
        );
    }
}

/* ==================== MOUSE EVENT PARSER ==================== */

static void handle_byte(char key) {
    /* Normal character */

    if (parser_state == 0) {
        if (key == 27) {
            parser_state = 1;
        } else {
            handle_key(key);
        }

        return;
    }

    /* ESC */

    if (parser_state == 1) {
        parser_state =
            key == '[' ? 2 : 0;

        return;
    }

    /* Arrow or SGR mouse escape */

    if (parser_state == 2) {
        if (key == '<') {
            parser_state = 3;
            parser_length = 0;

        } else {
            if (key == 'A') {
                scroll_board(-1, 0);
            }

            if (key == 'B') {
                scroll_board(1, 0);
            }

            if (key == 'C') {
                scroll_board(0, 1);
            }

            if (key == 'D') {
                scroll_board(0, -1);
            }

            parser_state = 0;
        }

        return;
    }

    if (parser_state == 3) {
        if ((key >= '0' && key <= '9') ||
            key == ';') {

            if (parser_length <
                (int)sizeof(parser_buffer) - 1) {
                parser_buffer[parser_length++] = key;
            } else {
                parser_state = 0;
            }

            return;
        }

        parser_buffer[parser_length] = '\0';

        parser_state = 0;


        if (key != 'M') {
            return;
        }

        int button;
        int x;
        int y;

        if (sscanf(
                parser_buffer,
                "%d;%d;%d",
                &button,
                &x,
                &y
            ) != 3) {
            return;
        }

        if (button == 64) {
            scroll_board(-1, 0);

        } else if (button == 65) {
            scroll_board(1, 0);

        } else if (button == 0) {
            mouse_click(x, y);
        }
    }
}

/* ==================== MAIN CLIENT FUNCTION ==================== */

int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "");
    if (argc != 3) {
        fprintf(
            stderr,
            "Usage: %s SERVER_IP PORT\n",
            argv[0]
        );

        return 1;
    }

    if (!show_main_menu()) return 0;

    signal(SIGPIPE, SIG_IGN);

    /* Connect to the authoritative server. */

    server_socket = connect_to_server(
        argv[1],
        argv[2]
    );

    if (server_socket >= 0) {
        int flag = 1;
        setsockopt(server_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof flag);
    }

    if (server_socket < 0) {
        fprintf(
            stderr,
            "Could not connect to %s:%s\n",
            argv[1],
            argv[2]
        );

        return 1;
    }

    atexit(restore_terminal);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (!initialize_terminal()) {
        close(server_socket);
        return 1;
    }

    redraw();

    /* Main network and terminal event loop. */

    while (running) {
        fd_set read_fds;

        FD_ZERO(&read_fds);

        FD_SET(
            STDIN_FILENO,
            &read_fds
        );

        FD_SET(
            server_socket,
            &read_fds
        );

        int highest_fd =
            server_socket > STDIN_FILENO
                ? server_socket
                : STDIN_FILENO;

        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = 100000
        };

        int ready = select(
            highest_fd + 1,
            &read_fds,
            NULL,
            NULL,
            &timeout
        );

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }

            break;
        }

        int screen_changed = 0;

        /* Receive server-authoritative state. */

        if (ready > 0 &&
            FD_ISSET(
                server_socket,
                &read_fds
            )) {
            Packet incoming;
            if (!receive_all(
                    server_socket,
                    &incoming,
                    sizeof(incoming)
                )) {
                snprintf(
                    local_message,
                    sizeof(local_message),
                    "Server disconnected."
                );

                break;
            }

            if (incoming.kind != PKT_STATE &&
                incoming.kind != PKT_FRAME) continue;

            int timer_only = 0;
            if (have_state && incoming.kind == PKT_STATE &&
                latest.kind == PKT_STATE) {
                Packet a = latest, b = incoming;
                b.seconds = a.seconds;
                timer_only = memcmp(&a, &b, sizeof a) == 0;
            }
            screen_changed = !have_state ||
                (!timer_only && memcmp(&latest, &incoming, sizeof latest) != 0);
            latest = incoming;
            have_state = 1;
            if (incoming.kind == PKT_STATE) {
                clock_gettime(CLOCK_MONOTONIC, &received_at);
            }
            if (timer_only) update_timer_line();

            local_message[0] = '\0';

            if (!can_play()) {
                selected_tool = TOOL_NONE;
            }
        }

        /* Handle player keyboard and mouse input. */

        if (ready > 0 &&
            FD_ISSET(
                STDIN_FILENO,
                &read_fds
            )) {
            char buffer[128];

            ssize_t count = read(
                STDIN_FILENO,
                buffer,
                sizeof(buffer)
            );

            for (ssize_t i = 0; i < count; i++) {
                handle_byte(buffer[i]);
            }
            if (count > 0) screen_changed = 1;
        }

        if (ready == 0) {
            int width, height;
            get_terminal_size(&width, &height);
            if (width != layout.width || height != layout.height)
                screen_changed = 1;
        }
        if (screen_changed) redraw();


        if (have_state && latest.phase == 2) {
            redraw();
            while (1) {
                fd_set keys;
                FD_ZERO(&keys);
                FD_SET(STDIN_FILENO, &keys);
                struct timeval wait = { .tv_sec = 0, .tv_usec = 100000 };
                int ready_key = select(STDIN_FILENO + 1, &keys,
                                       NULL, NULL, &wait);
                if (ready_key > 0 && FD_ISSET(STDIN_FILENO, &keys)) {
                    char key;
                    if (read(STDIN_FILENO, &key, 1) == 1 &&
                        (key == '\r' || key == '\n' ||
                         key == 'q' || key == 'Q')) {
                        break;
                    }
                }
            }
            break;
        }

        if (have_state && latest.kind == PKT_STATE && latest.phase != 2) {
            int left = seconds_left();
            if (left != previous_timer) {
                previous_timer = left;
                update_timer_line();
            }
        }
    }

    /* Cleanup */

    restore_terminal();

    if (server_socket >= 0) {
        close(server_socket);
    }

    printf(
        "\n%s\n",
        local_message[0]
            ? local_message
            : "Client closed."
    );

    return 0;
}
