# KINGS & CANNONS

**A Two-Player Terminal-Based Strategy Game**

Kings & Cannons is a two-player, chess-inspired strategy game developed in C for the Linux terminal. Players must protect their kings while strategically using shields, cannons, and special attacks to defeat their opponent.

The game is developed as a course requirement for CSMC312 - Principles Operating Systems at Ateneo de Naga University.

## Game Overview

| Category | Description |
|---|---|
| Genre | Turn-Based Strategy |
| Platform | Linux Terminal |
| Programming Language | C |
| Players | 2 |
| Architecture | Client-Server |
| Communication | TCP Sockets |
| Development Status | In Progress |

## Gameplay

The game takes place on a 10×10 battlefield divided into two territories, one for each player.

Players use a combination of defensive and offensive strategies to protect their kings and attack the opposing side.

The planned gameplay features include:

- **King:** The primary piece that players must protect, with chess-inspired movement abilities.
- **Shields:** Defensive structures used to protect the king from incoming attacks.
- **Cannons:** Offensive weapons used to attack the opponent's territory.

The objective is to defeat the opposing king.

## System Architecture

Kings & Cannons uses a client-server architecture designed for two computers communicating through TCP sockets.

- **Server (Player 1):** Hosts the game, manages the battlefield, validates player actions, and communicates game updates.
- **Client (Player 2):** Connects to the server, sends player actions, and receives updated game information.

The server maintains the authoritative game state to keep both players synchronized.

## Requirements

- Linux operating system
- GCC compiler
- Two computers connected to the same local network

## Compilation and Execution

### 1. Compile the programs

From the repository's root directory, run:

```bash
gcc src/server.c -o server
gcc src/client.c -o client
```

### 2. Start the server

On Player 1's computer:

```bash
./server
```

### 3. Connect the client

On Player 2's computer:

```bash
./client
```

The client must connect to the server's IP address and configured port.

**Note:** The exact connection commands may vary depending on the current implementation.

## Documentation

The complete Game Design and Technical Documentation is maintained separately in Google Docs.

**Project Documentation:** [View Google Docs](https://docs.google.com/document/d/1oaelYW6e8gjBXE_P8wQ5cT8O-Pj1EmimlmhasUEAq3w/edit?usp=sharing)

## Development Status

The project is currently under development. Features and gameplay mechanics are being implemented and tested incrementally.

## Developers

1. Christian Dane R. Naz - BS Computer Science
2. Josh Warren S. Quinto - BS Information Technology
3. Raphael Julius Q. Sibulo - BS Computer Science
4. Aj M. Villafuerte - BS Computer Science
