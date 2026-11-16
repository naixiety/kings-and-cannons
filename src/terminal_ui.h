
#ifndef TERMINAL_UI_H
#define TERMINAL_UI_H

#include <stdio.h>

static int show_main_menu(void) {
    char input[32];

    while (1) {
        printf("\033[2J\033[H");

        puts("====================================");
        puts("          KINGS & CANNONS");
        puts("====================================");
        puts("1. Start Game");
        puts("2. Instructions");
        puts("3. Exit Game");

        printf("\nChoose: ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin))
            return 0;

        if (input[0] == '1')
            return 1;

        if (input[0] == '3')
            return 0;

        if (input[0] == '2') {
            puts("\nHOW TO PLAY");
            puts("1. Place your king on your half.");
            puts("2. Place shields and cannons.");
            puts("3. Select MOVE to move your king.");
            puts("4. Press E to end your turn.");
            puts("5. Cannons fire after both turns.");
            puts("6. Defeat the enemy king to win.");

            printf("\nPress ENTER to return...");
            fgets(input, sizeof(input), stdin);
        }
    }
}

#endif
