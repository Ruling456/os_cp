#include "common.h"

// Глобальные переменные
MainState *main_state = NULL;
int shm_fd_main = -1;
sem_t *sem_main = NULL;

int game_id = -1;
int player_slot = -1;
int client_pid;

// Прототипы функций
void cleanup_client();
int connect_to_server();
int send_request_to_server(ClientMessage *request, ClientMessage *response);
void check_server_status();
void list_games();
void create_new_game();
void join_game();
void get_game_info();
void send_guess();
void game_loop();
void main_menu();

// Очистка ресурсов
void cleanup_client() {
    if (game_id != -1 && player_slot != -1) {
        char shm_name[64];
        snprintf(shm_name, sizeof(shm_name), "%s%d", SHM_CLIENT_PREFIX, client_pid);
        
        int shm_fd = shm_open(shm_name, O_RDWR, 0666);
        if (shm_fd != -1) {
            ClientChannel *channel = mmap(NULL, sizeof(ClientChannel), 
                                          PROT_READ | PROT_WRITE, 
                                          MAP_SHARED, shm_fd, 0);
            if (channel != MAP_FAILED) {
                char sem_name[64];
                snprintf(sem_name, sizeof(sem_name), "%s%d", SEM_CLIENT_PREFIX, client_pid);
                sem_t *sem = sem_open(sem_name, 0);
                
                if (sem != SEM_FAILED) {
                    sem_wait(sem);
                    channel->request.type = MSG_DISCONNECT;
                    channel->request.game_id = game_id;
                    channel->request.player_id = player_slot;
                    channel->processed = 0;
                    sem_post(sem);
                    sem_close(sem);
                }
                munmap(channel, sizeof(ClientChannel));
            }
            close(shm_fd);
        }
    }
    
    if (main_state != NULL) munmap(main_state, sizeof(MainState));
    if (shm_fd_main != -1) close(shm_fd_main);
    if (sem_main != NULL) sem_close(sem_main);
    
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "%s%d", SHM_CLIENT_PREFIX, client_pid);
    shm_unlink(shm_name);
    
    char sem_name[64];
    snprintf(sem_name, sizeof(sem_name), "%s%d", SEM_CLIENT_PREFIX, client_pid);
    sem_unlink(sem_name);
}

// Обновляем connect_to_server():
int connect_to_server() {
    printf("[CLIENT] Подключение к серверу...\n");
    
    shm_fd_main = shm_open(SHM_MAIN_NAME, O_RDWR, 0666);
    if (shm_fd_main == -1) {
        perror("[CLIENT] shm_open main");
        return -1;
    }
    
    main_state = mmap(NULL, sizeof(MainState), 
                      PROT_READ | PROT_WRITE, 
                      MAP_SHARED, shm_fd_main, 0);
    if (main_state == MAP_FAILED) {
        perror("[CLIENT] mmap main");
        close(shm_fd_main);
        return -1;
    }
    
    sem_main = sem_open(SEM_MAIN_NAME, 0);
    if (sem_main == SEM_FAILED) {
        perror("[CLIENT] sem_open main");
        munmap(main_state, sizeof(MainState));
        close(shm_fd_main);
        return -1;
    }
    
    client_pid = getpid();
    printf("[CLIENT] Подключено. PID: %d, Игр: %d\n", client_pid, main_state->game_count);
    return 0;
}

// Исправляем send_request_to_server():
int send_request_to_server(ClientMessage *request, ClientMessage *response) {
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "%s%d", SHM_CLIENT_PREFIX, client_pid);
    
    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("[CLIENT] shm_open");
        return -1;
    }
    
    if (ftruncate(shm_fd, sizeof(ClientChannel)) == -1) {
        perror("[CLIENT] ftruncate");
        close(shm_fd);
        return -1;
    }
    
    ClientChannel *channel = mmap(NULL, sizeof(ClientChannel), 
                                  PROT_READ | PROT_WRITE, 
                                  MAP_SHARED, shm_fd, 0);
    if (channel == MAP_FAILED) {
        perror("[CLIENT] mmap");
        close(shm_fd);
        return -1;
    }
    
    char sem_name[64];
    snprintf(sem_name, sizeof(sem_name), "%s%d", SEM_CLIENT_PREFIX, client_pid);
    
    // ВАЖНО: НЕ удаляем семафор, только открываем
    sem_t *sem = sem_open(sem_name, O_CREAT, 0666, 1);
    if (sem == SEM_FAILED) {
        perror("[CLIENT] sem_open");
        munmap(channel, sizeof(ClientChannel));
        close(shm_fd);
        return -1;
    }
    
    sem_wait(sem);
    memcpy(&channel->request, request, sizeof(ClientMessage));
    channel->processed = 0;
    sem_post(sem);
    
    int attempts = 0;
    while (attempts < 50) {  // 10 секундов максимум
        usleep(200000); // 0.2 секунды
        
        sem_wait(sem);
        if (channel->processed == 1) {
            memcpy(response, &channel->response, sizeof(ClientMessage));
            sem_post(sem);
            
            sem_close(sem);
            munmap(channel, sizeof(ClientChannel));
            close(shm_fd);
            return 0;
        }
        sem_post(sem);
        attempts++;
    }
    
    printf("[CLIENT] Таймаут ожидания ответа\n");
    sem_close(sem);
    munmap(channel, sizeof(ClientChannel));
    close(shm_fd);
    return -2;
}

// Функция проверки сервера:
void check_server_status() {
    printf("[CLIENT] Проверка сервера...\n");
    
    // Проверяем главную shared memory
    int fd = shm_open(SHM_MAIN_NAME, O_RDONLY, 0666);
    if (fd == -1) {
        printf("[CLIENT] Сервер НЕ запущен (не найден %s)\n", SHM_MAIN_NAME);
        return;
    }
    
    MainState *check_state = mmap(NULL, sizeof(MainState), 
                                  PROT_READ, 
                                  MAP_SHARED, fd, 0);
    if (check_state == MAP_FAILED) {
        printf("[CLIENT] Ошибка чтения shared memory\n");
        close(fd);
        return;
    }
    
    printf("[CLIENT] Сервер запущен. Игр: %d\n", check_state->game_count);
    
    munmap(check_state, sizeof(MainState));
    close(fd);
}
// Вывод списка игр
void list_games() {
    sem_wait(sem_main);
    
    printf("\n=== Доступные игры ===\n");
    if (main_state->game_count == 0) {
        printf("Нет доступных игр.\n");
    } else {
        printf("ID | Название игры                 | Игроков | Длина слова | Статус\n");
        printf("---+-------------------------------+---------+-------------+---------\n");
        
        for (int i = 0; i < main_state->game_count; i++) {
            GameState *game = &main_state->games[i];
            
            // Считаем только активных игроков
            int active_players = 0;
            for (int j = 0; j < MAX_PLAYERS; j++) {
                if (game->players[j] != -1 && game->active[j]) {
                    active_players++;
                }
            }
            
            char status[30];
            if (game->game_over) {
                strcpy(status, "Завершена");
            } else if (active_players >= game->max_players) {
                strcpy(status, "Полная");
            } else {
                snprintf(status, sizeof(status), "Ждет (%d/%d)", 
                        active_players, game->max_players);
            }
            
            printf("%2d | %-30s | %7d | %11d | %s\n", 
                   game->id, game->name, 
                   game->max_players, game->word_length, status);
        }
    }
    
    sem_post(sem_main);
}

// Создание новой игры
void create_new_game() {
    char name[MAX_NAME_LEN];
    int max_players, word_length;
    
    printf("\n=== Создание новой игры ===\n");
    
    printf("Введите имя игры: ");
    if (scanf("%s", name) != 1) {
        while (getchar() != '\n');
        return;
    }
    
    printf("Введите количество игроков (1-%d): ", MAX_PLAYERS);
    if (scanf("%d", &max_players) != 1) {
        while (getchar() != '\n');
        return;
    }
    
    if (max_players < 1 || max_players > MAX_PLAYERS) {
        printf("Неверное количество игроков\n");
        return;
    }
    
    printf("Введите длину слова (3-%d): ", MAX_WORD_LEN);
    if (scanf("%d", &word_length) != 1) {
        while (getchar() != '\n');
        return;
    }
    
    if (word_length < 3 || word_length > MAX_WORD_LEN) {
        printf("Неверная длина слова\n");
        return;
    }
    
    ClientMessage request, response;
    memset(&request, 0, sizeof(ClientMessage));
    memset(&response, 0, sizeof(ClientMessage));
    
    request.type = MSG_CREATE_GAME;
    strcpy(request.data.create_game.name, name);
    request.data.create_game.word_length = word_length;
    request.data.create_game.max_players = max_players;
    
    printf("\nСоздание игры...\n");
    
    if (send_request_to_server(&request, &response) == 0) {
        if (response.type == MSG_CREATE_GAME && response.data.result[1] == 0) {
            int new_game_id = response.data.result[0];
            printf("\n✅ Игра создана! ID: %d\n", new_game_id);
            
            printf("Подключиться к игре? (y/n): ");
            char choice;
            scanf(" %c", &choice);
            while (getchar() != '\n');
            
            if (choice == 'y' || choice == 'Y') {
                char player_name[MAX_NAME_LEN];
                printf("Введите ваше имя: ");
                if (scanf("%s", player_name) != 1) {
                    while (getchar() != '\n');
                    return;
                }
                
                memset(&request, 0, sizeof(ClientMessage));
                request.type = MSG_CONNECT;
                request.game_id = new_game_id;
                strcpy(request.data.connect_info.player_name, player_name);
                
                if (send_request_to_server(&request, &response) == 0) {
                    if (response.type == MSG_CONNECT && response.data.result[1] == 0) {
                        player_slot = response.data.result[0];
                        game_id = new_game_id;
                        printf("\n✅ Подключение успешно! Вы игрок №%d\n", player_slot + 1);
                        game_loop();
                        game_id = -1;
                        player_slot = -1;
                    } else {
                        printf("❌ Ошибка подключения\n");
                    }
                } else {
                    printf("❌ Не удалось отправить запрос серверу\n");
                }
            }
        } else {
            printf("❌ Ошибка создания игры\n");
        }
    } else {
        printf("❌ Не удалось отправить запрос серверу\n");
    }
}

// Подключение к игре
void join_game() {
    int selected_game_id;
    char player_name[MAX_NAME_LEN];
    
    list_games();
    
    printf("\nВведите ID игры (-1 для отмены): ");
    if (scanf("%d", &selected_game_id) != 1) {
        while (getchar() != '\n');
        return;
    }
    
    if (selected_game_id == -1) return;
    
    sem_wait(sem_main);
    
    if (selected_game_id < 0 || selected_game_id >= main_state->game_count) {
        printf("Неверный ID игры.\n");
        sem_post(sem_main);
        return;
    }
    
    GameState *game = &main_state->games[selected_game_id];
    
    if (game->game_over) {
        printf("Игра завершена.\n");
        sem_post(sem_main);
        return;
    }
    
    if (game->connected_players >= game->max_players) {
        printf("Игра полная.\n");
        sem_post(sem_main);
        return;
    }
    
    sem_post(sem_main);
    
    printf("Введите ваше имя: ");
    if (scanf("%s", player_name) != 1) {
        while (getchar() != '\n');
        return;
    }
    
    ClientMessage request, response;
    memset(&request, 0, sizeof(ClientMessage));
    memset(&response, 0, sizeof(ClientMessage));
    
    request.type = MSG_CONNECT;
    request.game_id = selected_game_id;
    strcpy(request.data.connect_info.player_name, player_name);
    
    printf("Подключение...\n");
    
    if (send_request_to_server(&request, &response) == 0) {
        if (response.type == MSG_CONNECT && response.data.result[1] == 0) {
            player_slot = response.data.result[0];
            game_id = selected_game_id;
            printf("\n✅ Подключение успешно! Вы игрок №%d\n", player_slot + 1);
            game_loop();
            game_id = -1;
            player_slot = -1;
        } else {
            printf("❌ Ошибка подключения\n");
        }
    } else {
        printf("❌ Не удалось отправить запрос серверу\n");
    }
}

// Получение информации об игре
void get_game_info() {
    if (game_id == -1) {
        printf("Вы не в игре.\n");
        return;
    }
    
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "%s%d", SHM_GAME_PREFIX, game_id);
    
    int shm_fd = shm_open(shm_name, O_RDONLY, 0666);
    if (shm_fd == -1) {
        printf("Игра не найдена.\n");
        return;
    }
    
    GameState *game_state = mmap(NULL, sizeof(GameState), 
                                 PROT_READ, 
                                 MAP_SHARED, shm_fd, 0);
    if (game_state == MAP_FAILED) {
        close(shm_fd);
        return;
    }
    
    printf("\n=== Игра: %s ===\n", game_state->name);
    printf("Длина слова: %d\n", game_state->word_length);
    printf("Игроков: %d/%d\n", game_state->connected_players, game_state->max_players);
    
    printf("\nИгроки:\n");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game_state->players[i] != -1) {
            printf("%d. %s", i + 1, game_state->player_names[i]);
            if (i == player_slot) printf(" (Вы)");
            if (!game_state->active[i]) printf(" [ВЫШЕЛ]");
            if (game_state->current_player == i && !game_state->game_over) printf(" <- ХОДИТ");
            printf("\n");
        }
    }
    
    if (game_state->move_count > 0) {
        printf("\nПоследние ходы:\n");
        int start = (game_state->move_count > 5) ? game_state->move_count - 5 : 0;
        for (int i = start; i < game_state->move_count; i++) {
            Move *move = &game_state->moves[i];
            char player_name[MAX_NAME_LEN] = "?";
            for (int j = 0; j < MAX_PLAYERS; j++) {
                if (game_state->players[j] == move->player_id) {
                    strcpy(player_name, game_state->player_names[j]);
                    break;
                }
            }
            printf("  %s: %s -> %d быков, %d коров\n", 
                   player_name, move->guess, move->bulls, move->cows);
        }
    }
    
    if (game_state->game_over) {
        if (game_state->winner != -1) {
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (game_state->players[i] == game_state->winner) {
                    printf("\n🎉 Победитель: %s 🎉\n", game_state->player_names[i]);
                    break;
                }
            }
        }
    } else if (game_state->current_player == player_slot) {
        printf("\n>>> Ваш ход!\n");
    } else {
        printf("\nОжидайте хода...\n");
    }
    
    munmap(game_state, sizeof(GameState));
    close(shm_fd);
}

// Отправка хода
void send_guess() {
    if (game_id == -1 || player_slot == -1) {
        printf("Вы не в игре!\n");
        return;
    }
    
    sem_wait(sem_main);
    int word_length = main_state->games[game_id].word_length;
    sem_post(sem_main);
    
    char guess[MAX_WORD_LEN];
    printf("\nВведите слово из %d букв: ", word_length);
    if (scanf("%s", guess) != 1) {
        while (getchar() != '\n');
        return;
    }
    
    if ((int)strlen(guess) != word_length) {
        printf("Неверная длина!\n");
        return;
    }
    
    for (int i = 0; guess[i]; i++) {
        if (!isalpha(guess[i])) {
            printf("Только буквы!\n");
            return;
        }
    }
    
    ClientMessage request, response;
    memset(&request, 0, sizeof(ClientMessage));
    memset(&response, 0, sizeof(ClientMessage));
    
    request.type = MSG_GUESS;
    request.game_id = game_id;
    request.player_id = player_slot;
    strcpy(request.data.guess, guess);
    
    printf("Отправка хода...\n");
    
    if (send_request_to_server(&request, &response) == 0) {
        if (response.type == MSG_RESULT) {
            if (response.data.result[0] >= 0) {
                int bulls = response.data.result[0];
                int cows = response.data.result[1];
                printf("\nРезультат: %d быков, %d коров\n", bulls, cows);
                
                if (bulls == word_length) {
                    printf("🎉 Поздравляем! Вы угадали слово! 🎉\n");
                }
            } else {
                printf("❌ Ошибка обработки хода\n");
            }
        }
    } else {
        printf("❌ Не удалось отправить ход\n");
    }
}

// Игровой цикл
void game_loop() {
    printf("\n=== ИГРА НАЧАЛАСЬ ===\n");
    printf("Команды: info, guess, exit\n");
    
    char command[20];
    
    while (1) {
        printf("\n> ");
        if (scanf("%s", command) != 1) {
            while (getchar() != '\n');
            continue;
        }
        
        if (strcmp(command, "info") == 0) {
            get_game_info();
        } else if (strcmp(command, "guess") == 0) {
            send_guess();
            get_game_info();
        } else if (strcmp(command, "exit") == 0) {
            printf("Выход...\n");
            break;
        }
        
        // Проверяем завершение игры
        if (game_id != -1) {
            char shm_name[64];
            snprintf(shm_name, sizeof(shm_name), "%s%d", SHM_GAME_PREFIX, game_id);
            
            int shm_fd = shm_open(shm_name, O_RDONLY, 0666);
            if (shm_fd != -1) {
                GameState *game_state = mmap(NULL, sizeof(GameState), 
                                           PROT_READ, 
                                           MAP_SHARED, shm_fd, 0);
                if (game_state != MAP_FAILED) {
                    if (game_state->game_over) {
                        printf("\nИгра завершена.\n");
                        munmap(game_state, sizeof(GameState));
                        close(shm_fd);
                        break;
                    }
                    munmap(game_state, sizeof(GameState));
                }
                close(shm_fd);
            }
        }
    }
}

// Главное меню
void main_menu() {
    int choice;
    
    while (1) {
        printf("\n=== БЫКИ И КОРОВЫ ===\n");
        printf("1. Создать игру\n");
        printf("2. Присоединиться к игре\n");
        printf("3. Список игр\n");
        printf("4. Выйти\n");
        printf("Выбор: ");
        
        if (scanf("%d", &choice) != 1) {
            while (getchar() != '\n');
            continue;
        }
        
        switch (choice) {
            case 1:
                create_new_game();
                break;
            case 2:
                join_game();
                break;
            case 3:
                list_games();
                break;
            case 4:
                printf("Выход...\n");
                return;
        }
    }
}

int main() {
    atexit(cleanup_client);
    
    printf("=== Клиент игры 'Быки и коровы' ===\n");
    
    // Сначала проверяем сервер
    check_server_status();
    
    if (connect_to_server() < 0) {
        printf("Не удалось подключиться к серверу.\n");
        printf("Убедитесь, что сервер запущен командой: ./server\n");
        return 1;
    }
    
    main_menu();
    return 0;
}