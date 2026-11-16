
#ifndef TERMINAL_UI_H
#define TERMINAL_UI_H

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define BOARD_SIZE 10

// Clear terminal screen
void clearScreen() {
    printf("\033[2J\033[H");
}

// Display game title
void displayTitle() {
    printf("\n");
    printf("====================================\n");
    printf("          KINGS & CANNONS           \n");
    printf("====================================\n");
}

// Main menu
int displayMenu() {
    int choice;

    displayTitle();

    printf("\n1. Start Game");
    printf("\n2. Instructions");
    printf("\n3. Exit Game");
    printf("\n\nEnter choice: ");

    if (scanf("%d", &choice) != 1) {
        while (getchar() != '\n');
        return 0;
    }

    return choice;
}

// Display instructions
void displayInstructions() {
    clearScreen();

    displayTitle();

    printf("\nINSTRUCTIONS\n");
    printf("------------------------------------\n");
    printf("1. Two players compete on a 10x10 board.\n");
    printf("2. Take turns making your moves.\n");
    printf("3. Use your available abilities wisely.\n");
    printf("4. Try to defeat your opponent's king.\n");
    printf("------------------------------------\n");

    printf("\nPress ENTER to return to menu...");
    getchar();
    getchar();
}

// Display battlefield
void displayBattlefield(char board[BOARD_SIZE][BOARD_SIZE]) {
    int row, col;

    printf("\n             BATTLEFIELD\n\n");

    printf("    ");
    for (col = 0; col < BOARD_SIZE; col++) {
        printf("%2d ", col + 1);
    }

    printf("\n   --------------------------------\n");

    for (row = 0; row < BOARD_SIZE; row++) {
        printf("%2d | ", row + 1);

        for (col = 0; col < BOARD_SIZE; col++) {
            printf(" %c ", board[row][col]);
        }

        printf("|\n");
    }

    printf("   --------------------------------\n");
}

// Display game information
void displayGameStatus(
    int currentPlayer,
    int score1,
    int score2,
    int ability1,
    int ability2,
    int remainingTime
) {
    printf("\n====================================\n");
    printf("             GAME STATUS            \n");
    printf("====================================\n");

    printf("Current Player : Player %d\n", currentPlayer);
    printf("Player 1 Score : %d\n", score1);
    printf("Player 2 Score : %d\n", score2);

    printf("Player 1 Abilities Remaining: %d\n", ability1);
    printf("Player 2 Abilities Remaining: %d\n", ability2);

    printf("Time Remaining : %d seconds\n", remainingTime);

    printf("====================================\n");
}

// Get valid keyboard input
int getPlayerInput(const char *message, int min, int max) {
    int input;

    while (1) {
        printf("%s", message);

        if (scanf("%d", &input) == 1) {
            if (input >= min && input <= max) {
                return input;
            }
        } else {
            while (getchar() != '\n');
        }

        printf("Invalid input! Please enter a number from %d to %d.\n",
               min, max);
    }
}

// Display game over
void displayGameOver(int winner) {
    printf("\n====================================\n");
    printf("              GAME OVER             \n");
    printf("====================================\n");

    if (winner == 0) {
        printf("             DRAW GAME!\n");
    } else {
        printf("          PLAYER %d WINS!\n", winner);
    }

    printf("====================================\n");
}

#endif
