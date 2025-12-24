#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>

// Имена shared memory объектов и семафоров
#define SHM_MAIN_NAME "/bulls_cows_main"
#define SHM_GAME_PREFIX "/bulls_cows_game_"
#define SHM_CLIENT_PREFIX "/bulls_cows_client_"
#define SEM_MAIN_NAME "/bulls_cows_sem_main"
#define SEM_GAME_PREFIX "/bulls_cows_sem_game_"
#define SEM_CLIENT_PREFIX "/bulls_cows_sem_client_"

// Максимальные значения
#define MAX_PLAYERS 4
#define MAX_GAMES 10
#define MAX_NAME_LEN 32
#define MAX_WORD_LEN 20
#define MAX_WORDS_IN_DICT 1000

// Типы сообщений
typedef enum {
    MSG_CONNECT = 1,
    MSG_GUESS,
    MSG_RESULT,
    MSG_DISCONNECT,
    MSG_GAME_OVER,
    MSG_WAIT_TURN,
    MSG_YOUR_TURN,
    MSG_GAME_INFO,
    MSG_CREATE_GAME,      // Новый тип: создание игры
    MSG_LIST_GAMES,       // Новый тип: запрос списка игр
    MSG_ERROR             // Новый тип: сообщение об ошибке
} MessageType;

// Структура для одного хода
typedef struct {
    char guess[MAX_WORD_LEN];  // предположенное слово
    int player_id;             // ID игрока
    int bulls;                 // быки
    int cows;                  // коровы
} Move;

// Структура состояния игры
typedef struct {
    int id;                            // ID игры
    char name[MAX_NAME_LEN];           // имя игры
    char secret[MAX_WORD_LEN];         // загаданное слово
    int word_length;                   // длина слова
    int players[MAX_PLAYERS];          // PID игроков
    int active[MAX_PLAYERS];           // активен ли игрок (0/1)
    int current_player;                // чей сейчас ход (индекс)
    Move moves[100];                   // история ходов
    int move_count;                    // количество ходов
    int game_over;                     // игра завершена?
    int winner;                        // победитель (PID)
    int max_players;                   // максимальное количество игроков
    int connected_players;             // сколько подключено
    char player_names[MAX_PLAYERS][MAX_NAME_LEN]; // имена игроков
} GameState;

// Структура для главной shared memory (список игр)
typedef struct {
    GameState games[MAX_GAMES];
    int game_count;
} MainState;

// Структура для обмена сообщениями между клиентом и сервером
typedef struct {
    MessageType type;
    int player_id;
    int game_id;
    union {
        char guess[MAX_WORD_LEN];          // для MSG_GUESS
        int result[2];                     // для MSG_RESULT (быки, коровы)
        char game_name[MAX_NAME_LEN];      // для создания игры
        struct {
            char name[MAX_NAME_LEN];
            int word_length;
            int max_players;
        } create_game;                     // для создания игры с параметрами
        struct {
            char player_name[MAX_NAME_LEN];
            int player_pid;
        } connect_info;                    // для подключения
    } data;
} ClientMessage;

// Структура для канала связи клиент-сервер
typedef struct {
    ClientMessage request;     // запрос от клиента
    ClientMessage response;    // ответ сервера
    int processed;             // флаг обработки (0 - не обработан, 1 - обработан)
} ClientChannel;

#endif // COMMON_H