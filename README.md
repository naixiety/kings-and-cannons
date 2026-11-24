# KINGS & CANNONS

**A Two-Player Terminal-Based Strategy Game**

Kings & Cannons is a two-player, chess-inspired strategy game developed in C for the Linux terminal. Players must protect their kings while strategically using shields, cannons, and special attacks to defeat their opponent.

This project is developed as a course requirement for CSMC312 - Principles of Operating Systems at Ateneo de Naga University, Department of Computer Science.


## Game Overview

| Category | Description |
|---|---|
| Genre | Turn-Based Strategy |
| Platform | Linux Terminal |
| Programming Language | C |
| Players | 2 |
| Architecture | Client-Server |
| Communication | TCP Sockets |


## Gameplay

The game takes place on a 10×10 battlefield divided into two territories, one for each player.
Players strategically position their kings, use shields for defense, and launch attacks using cannons and special abilities.


### Game Elements

- **King:** The primary piece that players must protect, featuring chess-inspired movement abilities.
- **Shields:** Defensive structures used to protect the king from incoming attacks.
- **Cannons:** Offensive weapons used to attack the opponent's territory.
- **Nuke:** Limited-use wide-range offensive attack that can be unlocked upon destroying 3 enemy shields.

**Objective:** Defeat the opposing player's king.


## System Architecture

Kings & Cannons is designed to use a client-server architecture consisting of **one server and two clients**.

- **Server:** Hosts the game, maintains the battlefield, processes player actions, and synchronizes the game state.
- **Client 1:** Handles Player 1's input and displays the battlefield.
- **Client 2:** Handles Player 2's input and displays the battlefield.

Both clients communicate with the same server using TCP sockets.

The game is designed to support two deployment configurations depending on the number of available computers.


## Project Structure

```text
kings-and-cannons/
├── src/
│   ├── client.c
│   └── server.c
├── README.md
└── .gitignore
```


## Requirements


- Linux operating system (Ubuntu or equivalent)
- GCC compiler
- Two or three computers connected to the same local network
- Network access to the server's configured TCP port


## Compilation

Clone the repository:

```bash
git clone https://github.com/naixiety/kings-and-cannons.git
cd kings-and-cannons
```

Compile the server:

```bash
gcc src/server.c -o server
```

Compile the client:

```bash
gcc src/client.c -o client
```

Run the compilation commands from the repository's root directory.

## Running the Game

The game supports two deployment configurations.


### Option A: Three-PC Setup


**1 Server + 2 Clients on separate computers**


| Computer | Role | Program |
|---|---|---|
| PC 1 | Dedicated Server | `server` |
| PC 2 | Player 1 | `client` |
| PC 3 | Player 2 | `client` |


**Step 1: Start the server on PC 1**


```bash
./server <PORT>
```

Choose a port number of your choice (e.g. `5555`)

**Step 2: Start Client 1 on PC 2**


```bash
./client <SERVER_IP> <PORT>
```


**Step 3: Start Client 2 on PC 3**


```bash
./client <SERVER_IP> <PORT>
```


Replace `<SERVER_IP>` with PC 1's local IP address.
Replace `<PORT>` with your port number same as the server


Both clients connect to the same server.


### Option B: Two-PC Setup


**1 Server + Client 1 on PC 1, Client 2 on PC 2**


| Computer | Role | Programs |
|---|---|---|
| PC 1 | Server + Player 1 | `server` and `client` |
| PC 2 | Player 2 | `client` |


**Step 1: Start the server on PC 1**


Open a terminal:


```bash
./server <PORT>
```

Choose a port number of your choice (e.g. `5555`)

**Step 2: Start Client 1 on PC 1**


Open a second terminal:


```bash
./client <SERVER_IP> <PORT>
```


**Step 3: Start Client 2 on PC 2**


```bash
./client <SERVER_IP> <PORT>
```

Replace `<SERVER_IP>` with PC 1's local IP address.
Replace `<PORT>` with your port number same as the server


Both clients now communicate with the server running on PC 1.


## Finding the Server IP Address


On the server computer, run:


```bash
hostname -I
```


Identify the local IPv4 address used by the network connecting the computers.


Example:


```text
192.168.1.10
```


The client computers must be able to reach this address and the server's configured TCP port.


## Important Notes


- Start the server before launching the clients.
- Both clients must connect to the same server address and port.
- The server must support two simultaneous client connections.
- Ensure the server's firewall permits connections to the configured TCP port.
- The game requires two client sessions regardless of whether two or three physical computers are used.
- The execution commands assume the client accepts the server IP address as a command-line argument.

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
