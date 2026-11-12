// server.c

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* CONFIGURATION */

#define BOARD_SIZE 10
#define PLAYER_COUNT 2
#define TURN_SECONDS 60
#define KING_MAX_LIVES 3
#define SHIELD_LIMIT 10
#define CANNONS_PER_TURN 2
#define UNDO_LIMIT 128

/* GAME STRUCTURES */

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

typedef struct {
    int row;
    int col;
    int direction;
    int owner;
    int active;
} Shot;

/* NETWORK PROTOCOL */

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

/* SERVER STATE */

static Game game;
static Game undo_stack[UNDO_LIMIT];

static int undo_count = 0;
static int active_player = 0;
static int current = 0;
static int round_number = 1;

static int winner = -1;

/*
 * phase:
 * 0 = King placement
 * 1 = Normal gameplay
 * 2 = Game finished
 */
static int phase = 0;

static Tool selected_tool = TOOL_NONE;
static MoveType selected_move = STEP;

static int clients[PLAYER_COUNT] = {-1, -1};

static int last_tick = -1;
static double deadline = 0;

static char message[180] = "";
static char notice[180] = "Waiting for players.";

/* HELPERS */

static int on_board(int row, int col) {
    return row >= 0 &&
           row < BOARD_SIZE &&
           col >= 0 &&
           col < BOARD_SIZE;
}

static int own_territory(int player, int row) {
    return player == 0 ? row < 5 : row >= 5;
}

static int absolute(int value) {
    return value < 0 ? -value : value;
}

static int direction(int value) {
    return (value > 0) - (value < 0);
}

static double seconds_now(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);

    return (double)now.tv_sec +
           (double)now.tv_nsec / 1000000000.0;
}

static void pause_ms(int milliseconds) {
    struct timespec duration = {
        milliseconds / 1000,
        (long)(milliseconds % 1000) * 1000000L
    };

    while (nanosleep(&duration, &duration) &&
           errno == EINTR) {
    }
}

/* INITIALIZE */

static void initialize_game(void) {
    memset(&game, 0, sizeof(game));

    for (int row = 0; row < BOARD_SIZE; row++) {
        for (int col = 0; col < BOARD_SIZE; col++) {
            game.board[row][col].owner = -1;
        }
    }

    for (int player = 0; player < PLAYER_COUNT; player++) {
        game.king[player] = (Position){-1, -1};
        game.nuke_target[player] = (Position){-1, -1};

        game.lives[player] = KING_MAX_LIVES;

        game.moves_remaining[player][SKEW] = 5;
        game.moves_remaining[player][LEAP] = 5;
        game.moves_remaining[player][AXIS] = 5;
        game.moves_remaining[player][JASMIN] = 3;
    }
}

/* UNDO */

static void remember_action(void) {
    if (undo_count < UNDO_LIMIT) {
        undo_stack[undo_count++] = game;
    }
}

static void undo_action(void) {
    if (undo_count == 0) {
        snprintf(
            message,
            sizeof(message),
            "Nothing to undo this turn."
        );
        return;
    }

    game = undo_stack[--undo_count];
    selected_tool = TOOL_NONE;

    snprintf(
        message,
        sizeof(message),
        "Last action undone."
    );
}

/* KING MOVEMENT */

static int move_legal(
    int player,
    MoveType type,
    int dest_row,
    int dest_col,
    int *crater_damage
) {
    Position from = game.king[player];

    int dr = absolute(dest_row - from.row);
    int dc = absolute(dest_col - from.col);

    int valid = 0;
    int damage = 0;

    if (!on_board(dest_row, dest_col)) {
        return 0;
    }

    if (!own_territory(player, dest_row)) {
        return 0;
    }

    if (game.board[dest_row][dest_col].piece != EMPTY) {
        return 0;
    }

    if (game.king_moved[player] || (dr == 0 && dc == 0)) {
        return 0;
    }

    if (type < STEP || type > JASMIN) {
        return 0;
    }

    if (type != STEP &&
        game.moves_remaining[player][type] <= 0) {
        return 0;
    }

    switch (type) {
        case STEP:
            valid = dr + dc == 1;
            break;

        case SKEW:
            valid = dr == 2 && dc == 2;
            break;

        case LEAP:
            valid = (dr == 2 && dc == 1) ||
                    (dr == 1 && dc == 2);
            break;

        case AXIS:
            valid = (dr == 2 && dc == 0) ||
                    (dr == 0 && dc == 2);
            break;

        case JASMIN:
            valid = dr == dc || dr == 0 || dc == 0;
            break;
    }

    if (!valid) {
        return 0;
    }

    /* Knight-style LEAP can jump over pieces. */
    if (type != LEAP) {
        int row_step = direction(dest_row - from.row);
        int col_step = direction(dest_col - from.col);

        for (
            int row = from.row + row_step,
                col = from.col + col_step;

            row != dest_row || col != dest_col;

            row += row_step,
            col += col_step
        ) {
            if (game.board[row][col].piece != EMPTY) {
                return 0;
            }

            if (game.crater[row][col]) {
                damage++;
            }
        }
    }

    if (game.crater[dest_row][dest_col]) {
        damage++;
    }

    if (crater_damage != NULL) {
        *crater_damage = damage;
    }

    return 1;
}

/* WINNER */

static void update_victory(void) {
    if (game.lives[0] <= 0 && game.lives[1] <= 0) {
        winner = 2;
    } else if (game.lives[0] <= 0) {
        winner = 1;
    } else if (game.lives[1] <= 0) {
        winner = 0;
    }

    if (winner == 2) {
        snprintf(message, sizeof(message), "DRAW!");
    } else if (winner >= 0) {
        snprintf(
            message,
            sizeof(message),
            "PLAYER %d WINS!",
            winner + 1
        );
    }
}

/* TOOL VALIDATION */

static void choose_tool(Tool tool) {
    int player = active_player;

    selected_tool = TOOL_NONE;

    if (tool == TOOL_SHIELD &&
        game.shields_placed[player] >= SHIELD_LIMIT) {
        snprintf(
            message,
            sizeof(message),
            "No shields remaining."
        );
        return;
    }

    if (tool == TOOL_CANNON &&
        game.cannon_placements[player] >= CANNONS_PER_TURN) {
        snprintf(
            message,
            sizeof(message),
            "Both cannons already placed."
        );
        return;
    }

    if (tool == TOOL_KING && game.king_moved[player]) {
        snprintf(
            message,
            sizeof(message),
            "King already moved this round."
        );
        return;
    }

    if (tool == TOOL_NUKE &&
        (game.nuke_used[player] ||
         game.enemy_shields_destroyed[player] < 3)) {
        snprintf(
            message,
            sizeof(message),
            "Nuke locked or already used."
        );
        return;
    }

    selected_tool = tool;
}

/* BOARD ACTIONS */

static void perform_board_action(
    int row,
    int col,
    int placing_king
) {
    int player = active_player;

    if (!on_board(row, col)) {
        return;
    }

    /* Initial king placement */

    if (placing_king) {
        if (!own_territory(player, row) ||
            game.board[row][col].piece != EMPTY) {

            snprintf(
                message,
                sizeof(message),
                "Invalid king placement."
            );
            return;
        }

        game.king[player] = (Position){row, col};

        game.board[row][col] = (Cell){KING, player};

        snprintf(
            message,
            sizeof(message),
            "King placed at %c%d.",
            'A' + col,
            row + 1
        );

        return;
    }

    if (selected_tool == TOOL_NONE) {
        return;
    }

    /* Nuke targeting */

    if (selected_tool == TOOL_NUKE) {
        remember_action();

        game.nuke_target[player] = (Position){row, col};
        game.nuke_used[player] = 1;

        selected_tool = TOOL_NONE;

        snprintf(
            message,
            sizeof(message),
            "Nuke aimed at %c%d.",
            'A' + col,
            row + 1
        );

        return;
    }

    /* King movement */

    if (selected_tool == TOOL_KING) {
        int damage = 0;

        if (!move_legal(
                player,
                selected_move,
                row,
                col,
                &damage
            )) {
            snprintf(
                message,
                sizeof(message),
                "Illegal king movement."
            );
            return;
        }

        remember_action();

        Position old = game.king[player];

        game.board[old.row][old.col] =
            (Cell){EMPTY, -1};

        game.board[row][col] =
            (Cell){KING, player};

        game.king[player] = (Position){row, col};
        game.king_moved[player] = 1;

        if (selected_move != STEP) {
            game.moves_remaining[player][selected_move]--;
        }

        game.lives[player] -= damage;

        selected_tool = TOOL_NONE;

        update_victory();

        snprintf(
            message,
            sizeof(message),
            "King moved. Crater damage: %d.",
            damage
        );

        return;
    }

    /* Shield and cannon placement */

    if (!own_territory(player, row) ||
        game.board[row][col].piece != EMPTY ||
        game.crater[row][col] ||
        game.breach[row][col]) {

        snprintf(
            message,
            sizeof(message),
            "Invalid placement."
        );
        return;
    }

    if (selected_tool == TOOL_SHIELD &&
        game.shields_placed[player] >= SHIELD_LIMIT) {
        return;
    }

    if (selected_tool == TOOL_CANNON &&
        game.cannon_placements[player] >= CANNONS_PER_TURN) {
        return;
    }

    remember_action();

    game.board[row][col] = (Cell){
        selected_tool == TOOL_SHIELD ? SHIELD : CANNON,
        player
    };

    if (selected_tool == TOOL_SHIELD) {
        game.shields_placed[player]++;
    } else {
        game.cannon_placements[player]++;
    }

    if (selected_tool == TOOL_CANNON &&
        game.cannon_placements[player] >= CANNONS_PER_TURN) {
        selected_tool = TOOL_NONE;
    }

    if (selected_tool == TOOL_SHIELD &&
        game.shields_placed[player] >= SHIELD_LIMIT) {
        selected_tool = TOOL_NONE;
    }

    snprintf(
        message,
        sizeof(message),
        "Piece placed at %c%d.",
        'A' + col,
        row + 1
    );
}

/* SOCKET UTILITIES */

static int send_exact(
    int socket_fd,
    const void *data,
    size_t size
) {
    const char *buffer = data;

    while (size > 0) {
        ssize_t sent = send(
            socket_fd,
            buffer,
            size,
            MSG_NOSIGNAL
        );

        if (sent < 0 && errno == EINTR) {
            continue;
        }

        if (sent <= 0) {
            return 0;
        }

        buffer += sent;
        size -= (size_t)sent;
    }

    return 1;
}

static int recv_exact(
    int socket_fd,
    void *data,
    size_t size
) {
    char *buffer = data;

    while (size > 0) {
        ssize_t received = recv(
            socket_fd,
            buffer,
            size,
            0
        );

        if (received < 0 && errno == EINTR) {
            continue;
        }

        if (received <= 0) {
            return 0;
        }

        buffer += received;
        size -= (size_t)received;
    }

    return 1;
}

/* PLAYER PRIVACY */

static void send_view(
    int player,
    int kind,
    const int *bullets,
    int bullet_count,
    const int *impacts,
    int impact_count
) {
    Packet packet = {0};

    packet.kind = kind;
    packet.player = player;
    packet.current = current;
    packet.phase = phase;
    packet.round = round_number;
    packet.winner = winner;

    packet.seconds = (int)(
        deadline - seconds_now() + 0.999
    );

    if (packet.seconds < 0) {
        packet.seconds = 0;
    }

    /* Copy the complete board first. */

    packet.state = game;

    int opponent = 1 - player;

    /* Hide every opponent piece. */

    for (int row = 0; row < BOARD_SIZE; row++) {
        for (int col = 0; col < BOARD_SIZE; col++) {
            Cell *cell = &packet.state.board[row][col];

            if (cell->owner == opponent) {
                cell->piece = EMPTY;
                cell->owner = -1;
            }
        }
    }

    /* Hide the opponent's king coordinates. */

    packet.state.king[opponent] =
        (Position){-1, -1};

    /* Hide private enemy statistics. */

    packet.state.lives[opponent] = -1;
    packet.state.shields_placed[opponent] = -1;
    packet.state.enemy_shields_destroyed[opponent] = -1;
    packet.state.cannon_placements[opponent] = -1;
    packet.state.nuke_used[opponent] = -1;

    packet.state.nuke_target[opponent] =
        (Position){-1, -1};

    packet.state.king_moved[opponent] = -1;

    for (int i = 0; i < 6; i++) {
        packet.state.moves_remaining[opponent][i] = -1;
    }

    (void)bullets;
    (void)bullet_count;
    (void)impacts;
    (void)impact_count;

    packet.bullet_count = 0;
    packet.impact_count = 0;

    if (phase == 2) {
        snprintf(
            packet.note,
            sizeof(packet.note),
            "Game finished."
        );

    } else if (kind == PKT_FRAME) {
        snprintf(
            packet.note,
            sizeof(packet.note),
            "Resolving simultaneous attacks..."
        );

    } else if (player != current) {
        snprintf(
            packet.note,
            sizeof(packet.note),
            "Waiting for Player %d.",
            current + 1
        );

    } else {
        snprintf(
            packet.note,
            sizeof(packet.note),
            "%s",
            notice
        );
    }

    if (clients[player] >= 0 &&
        !send_exact(
            clients[player],
            &packet,
            sizeof(packet)
        )) {

        close(clients[player]);
        clients[player] = -1;
    }
}

/* STATE BROADCAST */

static void publish(void) {
    for (int player = 0; player < PLAYER_COUNT; player++) {
        send_view(
            player,
            PKT_STATE,
            NULL,
            0,
            NULL,
            0
        );
    }
}

static void visual_frame(
    const int *bullets,
    int bullet_count,
    const int *impacts,
    int impact_count
) {
    for (int player = 0; player < PLAYER_COUNT; player++) {
        send_view(
            player,
            PKT_FRAME,
            bullets,
            bullet_count,
            impacts,
            impact_count
        );
    }

    pause_ms(210);
}

/* TURN MANAGEMENT */

static void begin_turn(void) {
    deadline = seconds_now() + TURN_SECONDS;
    last_tick = -1;

    snprintf(
        notice,
        sizeof(notice),
        "Player %d: %s",
        current + 1,
        phase == 0
            ? "Place your king."
            : "Your turn. Press E to finish."
    );

    publish();
}

static void finish_game(void) {
    phase = 2;
    deadline = seconds_now();
    publish();
}

/* SIMULTANEOUS COMBAT */

static void resolve_volley(void) {
    Shot shots[4];
    int shot_count = 0;

    /* Collect all cannons from both players. */

    for (int row = 0; row < BOARD_SIZE; row++) {
        for (int col = 0; col < BOARD_SIZE; col++) {
            Cell cell = game.board[row][col];

            if (cell.piece != CANNON || shot_count >= 4) {
                continue;
            }

            shots[shot_count++] = (Shot){
                row,
                col,
                cell.owner == 0 ? 1 : -1,
                cell.owner,
                1
            };
        }
    }

    /* NUKES */

    int nuclear_kill[2] = {0, 0};
    int blast[18];
    int blast_count = 0;

    for (int player = 0; player < PLAYER_COUNT; player++) {
        Position target = game.nuke_target[player];

        if (target.row < 0) {
            continue;
        }

        for (
            int row = target.row - 1;
            row <= target.row + 1;
            row++
        ) {
            for (
                int col = target.col - 1;
                col <= target.col + 1;
                col++
            ) {
                if (!on_board(row, col)) {
                    continue;
                }

                Cell cell = game.board[row][col];

                if (cell.piece == KING) {
                    nuclear_kill[cell.owner] = 1;
                }

                int square = row * BOARD_SIZE + col;
                int exists = 0;

                for (int i = 0; i < blast_count; i++) {
                    if (blast[i] == square) {
                        exists = 1;
                        break;
                    }
                }

                if (!exists) {
                    blast[blast_count++] = square;
                }
            }
        }
    }

    if (blast_count > 0) {
        snprintf(
            notice,
            sizeof(notice),
            "NUKE EXPLOSION!"
        );

        visual_frame(NULL, 0, blast, blast_count);

        for (int i = 0; i < blast_count; i++) {
            int row = blast[i] / BOARD_SIZE;
            int col = blast[i] % BOARD_SIZE;

            game.board[row][col] =
                (Cell){EMPTY, -1};

            game.crater[row][col] = 1;
            game.breach[row][col] = 1;
        }
    }

    /* Cannons destroyed by a nuke cannot fire. */

    for (int i = 0; i < shot_count; i++) {
        if (game.crater[shots[i].row][shots[i].col]) {
            shots[i].active = 0;
        }
    }

    /* CANNON FIRE */

    int king_hits[PLAYER_COUNT] = {0, 0};

    for (int step = 1; step <= BOARD_SIZE; step++) {
        int bullets[4];
        int impacts[18];

        int bullet_count = 0;
        int impact_count = 0;
        int any_active = 0;

        int broken[BOARD_SIZE][BOARD_SIZE] = {{0}};
        int credit[BOARD_SIZE][BOARD_SIZE];

        for (int row = 0; row < BOARD_SIZE; row++) {
            for (int col = 0; col < BOARD_SIZE; col++) {
                credit[row][col] = -1;
            }
        }

        for (int i = 0; i < shot_count; i++) {
            Shot *shot = &shots[i];

            if (!shot->active) {
                continue;
            }

            int row = shot->row + shot->direction * step;
            int col = shot->col;

            if (!on_board(row, col)) {
                shot->active = 0;
                continue;
            }

            any_active = 1;

            Cell target = game.board[row][col];

            /*
             * Projectiles pass through: empty squares, other cannons, own king, friendly shields
             */

            if (target.piece == EMPTY ||
                target.piece == CANNON ||
                (target.piece == KING &&
                 target.owner == shot->owner) ||
                (target.piece == SHIELD &&
                 target.owner == shot->owner)) {

                bullets[bullet_count++] =
                    row * BOARD_SIZE + col;

                continue;
            }

            /* An enemy piece was hit. */

            shot->active = 0;

            impacts[impact_count++] =
                row * BOARD_SIZE + col;

            if (target.piece == SHIELD) {
                broken[row][col] = 1;

                if (credit[row][col] < 0) {
                    credit[row][col] = shot->owner;
                }
            }

            if (target.piece == KING) {
                king_hits[target.owner]++;
            }
        }

        if (!any_active) {
            break;
        }

        snprintf(
            notice,
            sizeof(notice),
            "Simultaneous cannon fire..."
        );

        visual_frame(
            bullets,
            bullet_count,
            impacts,
            impact_count
        );

        /* Resolve destroyed shields after each step. */

        for (int row = 0; row < BOARD_SIZE; row++) {
            for (int col = 0; col < BOARD_SIZE; col++) {
                if (!broken[row][col]) {
                    continue;
                }

                game.board[row][col] =
                    (Cell){EMPTY, -1};

                game.breach[row][col] = 1;

                if (credit[row][col] >= 0) {
                    game.enemy_shields_destroyed[
                        credit[row][col]
                    ]++;
                }
            }
        }
    }

    /* Cannons disappear after firing. */

    for (int row = 0; row < BOARD_SIZE; row++) {
        for (int col = 0; col < BOARD_SIZE; col++) {
            if (game.board[row][col].piece == CANNON) {
                game.board[row][col] =
                    (Cell){EMPTY, -1};
            }
        }
    }

    /* Prepare for the next round. */

    for (int player = 0; player < PLAYER_COUNT; player++) {
        game.cannon_placements[player] = 0;
        game.king_moved[player] = 0;

        game.nuke_target[player] =
            (Position){-1, -1};

        game.lives[player] -= king_hits[player];

        if (nuclear_kill[player] ||
            game.lives[player] < 0) {
            game.lives[player] = 0;
        }
    }

    update_victory();
    round_number++;

    snprintf(
        notice,
        sizeof(notice),
        "%s",
        winner < 0
            ? "Round finished. Cannons reloaded."
            : "Game over!"
    );

    publish();
}

/* NEXT TURN */

static void next_turn(void) {
    undo_count = 0;
    selected_tool = TOOL_NONE;

    if (phase == 0) {
        if (current == 0) {
            current = 1;
        } else {
            phase = 1;
            current = 0;
        }

        begin_turn();
        return;
    }

    if (current == 0) {
        current = 1;
        begin_turn();
        return;
    }

    /* Both players completed their turns. */

    deadline = seconds_now() + 10;

    snprintf(
        notice,
        sizeof(notice),
        "Firing phase..."
    );

    publish();

    resolve_volley();

    if (winner >= 0) {
        finish_game();
    } else {
        current = 0;
        begin_turn();
    }
}

/* KING TIMEOUT */

static void fallback_king(void) {
    int row = current == 0 ? 2 : 7;
    int col = 4;

    if (game.king[current].row < 0) {
        game.king[current] = (Position){row, col};
        game.board[row][col] =
            (Cell){KING, current};
    }
}

/* PROCESS COMMANDS */

static void on_command(
    int player,
    const Packet *command
) {
    if (command->action == ACT_QUIT) {
        winner = 1 - player;
        finish_game();
        return;
    }

    if (player != current || phase == 2) {
        send_view(
            player,
            PKT_STATE,
            NULL,
            0,
            NULL,
            0
        );
        return;
    }

    active_player = player;

    /* King placement phase */

    if (phase == 0) {
        if (command->action != ACT_KING) {
            send_view(
                player,
                PKT_STATE,
                NULL,
                0,
                NULL,
                0
            );
            return;
        }

        perform_board_action(
            command->row,
            command->col,
            1
        );

        if (game.king[player].row >= 0) {
            next_turn();
            return;
        }

        snprintf(notice, sizeof(notice), "%s", message);

        send_view(
            player,
            PKT_STATE,
            NULL,
            0,
            NULL,
            0
        );

        return;
    }

    if (command->action == ACT_END) {
        next_turn();
        return;
    }

    if (command->action == ACT_UNDO) {
        undo_action();

        snprintf(notice, sizeof(notice), "%s", message);

        send_view(
            player,
            PKT_STATE,
            NULL,
            0,
            NULL,
            0
        );
        return;
    }

    if (command->action != ACT_PLACE) {
        return;
    }

    if (command->tool < TOOL_SHIELD ||
        command->tool > TOOL_NUKE) {
        return;
    }

    selected_move =
        command->movement >= STEP &&
        command->movement <= JASMIN
            ? (MoveType)command->movement
            : STEP;

    selected_tool = TOOL_NONE;

    choose_tool((Tool)command->tool);

    if (selected_tool != (Tool)command->tool) {
        snprintf(notice, sizeof(notice), "%s", message);

        send_view(
            player,
            PKT_STATE,
            NULL,
            0,
            NULL,
            0
        );
        return;
    }

    perform_board_action(
        command->row,
        command->col,
        0
    );

    snprintf(notice, sizeof(notice), "%s", message);

    if (winner >= 0) {
        finish_game();
        return;
    }

    /* Return updated state to this player only. */

    send_view(
        player,
        PKT_STATE,
        NULL,
        0,
        NULL,
        0
    );
}

/* MAIN SERVER */

int main(int argc, char **argv) {
    int port = argc >= 2 ? atoi(argv[1]) : 5555;

    if (port < 1024 || port > 65535) {
        fprintf(stderr, "Port must be 1024-65535.\n");
        return 1;
    }

    /* Create TCP listening socket. */

    int listener = socket(AF_INET, SOCK_STREAM, 0);

    if (listener < 0) {
        perror("socket");
        return 1;
    }

    int reuse = 1;

    setsockopt(
        listener,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse,
        sizeof(reuse)
    );

    struct sockaddr_in address = {0};

    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(
            listener,
            (struct sockaddr *)&address,
            sizeof(address)
        ) < 0) {
        perror("bind");
        close(listener);
        return 1;
    }

    if (listen(listener, 2) < 0) {
        perror("listen");
        close(listener);
        return 1;
    }

    printf("\n====================================\n");
    printf("       KINGS AND CANNONS SERVER\n");
    printf("====================================\n");

    printf("\nListening on port %d\n", port);
    printf("Waiting for two players...\n\n");
    fflush(stdout);

    /* Accept Player 1 and Player 2. */

    for (int player = 0; player < PLAYER_COUNT; player++) {
        clients[player] = accept(
            listener,
            NULL,
            NULL
        );

        if (clients[player] < 0) {
            perror("accept");
            close(listener);
            return 1;
        }

        printf("Player %d connected!\n", player + 1);
        fflush(stdout);
    }

    close(listener);

    initialize_game();

    current = 0;
    phase = 0;

    begin_turn();

    /* Main server loop. */

    while (phase != 2) {
        if (clients[0] < 0 || clients[1] < 0) {
            winner = clients[0] < 0 ? 1 : 0;
            finish_game();
            break;
        }

        if (seconds_now() >= deadline) {
            if (phase == 0) {
                fallback_king();
            }

            next_turn();
            continue;
        }

        int remaining = (int)(
            deadline - seconds_now() + 0.999
        );

        if (remaining != last_tick) {
            last_tick = remaining;
            publish();
        }

        /* Watch both player sockets. */

        fd_set read_fds;

        FD_ZERO(&read_fds);

        int highest_fd = 0;

        for (int player = 0; player < PLAYER_COUNT; player++) {
            FD_SET(clients[player], &read_fds);

            if (clients[player] > highest_fd) {
                highest_fd = clients[player];
            }
        }

        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = 150000
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

            perror("select");
            break;
        }

        /* Receive commands from clients. */

        for (int player = 0; player < PLAYER_COUNT; player++) {
            if (clients[player] < 0 ||
                !FD_ISSET(clients[player], &read_fds)) {
                continue;
            }

            Packet incoming;

            if (!recv_exact(
                    clients[player],
                    &incoming,
                    sizeof(incoming)
                )) {
                printf(
                    "Player %d disconnected.\n",
                    player + 1
                );

                close(clients[player]);
                clients[player] = -1;
                break;
            }

            if (incoming.kind == PKT_COMMAND) {
                on_command(player, &incoming);
            }
        }
    }

    /* Cleanup. */

    for (int player = 0; player < PLAYER_COUNT; player++) {
        if (clients[player] >= 0) {
            close(clients[player]);
        }
    }

    printf("\nServer stopped.\n");

    return 0;
}
