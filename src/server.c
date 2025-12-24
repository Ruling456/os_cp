#include "common.h"
#include <pthread.h>

// Структура для передачи параметров в поток
typedef struct {
    int client_pid;
    ClientChannel *channel;
    int shm_fd;
} ThreadArgs;

// Глобальные переменные
MainState *main_state = NULL;
int shm_fd_main = -1;
sem_t *sem_main = NULL;

// Создание словаря по умолчанию
void create_default_dictionary() {
    FILE *file = fopen("../test/dictionary.txt", "w");
    if (!file) return;
    
    const char *words[] = {
        "cat", "dog", "sun", "moon", "star", "tree", "book", "door", "wall", "floor",
        "apple", "table", "chair", "water", "light", "music", "paper", "river", "smile", "dream",
        "banana", "orange", "garden", "window", "forest", "winter", "summer", "spring", "autumn", "family"
    };
    
    int word_count = sizeof(words) / sizeof(words[0]);
    for (int i = 0; i < word_count; i++) {
        fprintf(file, "%s\n", words[i]);
    }
    
    fclose(file);
}

// Загрузка словаря
int load_dictionary(const char *filename, char dict[][MAX_WORD_LEN], int max_words) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        create_default_dictionary();
        file = fopen(filename, "r");
        if (!file) return 0;
    }
    
    int count = 0;
    char line[MAX_WORD_LEN];
    
    while (count < max_words && fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = 0;
        
        int valid = 1;
        for (int i = 0; line[i]; i++) {
            if (!isalpha(line[i])) {
                valid = 0;
                break;
            }
        }
        
        if (valid && strlen(line) > 0) {
            for (int i = 0; line[i]; i++) {
                dict[count][i] = tolower(line[i]);
            }
            dict[count][strlen(line)] = '\0';
            count++;
        }
    }
    
    fclose(file);
    return count;
}

// Проверка быков и коров
void check_bulls_cows(const char *secret, const char *guess, int *bulls, int *cows) {
    *bulls = 0;
    *cows = 0;
    int secret_len = strlen(secret);
    
    if (secret_len != strlen(guess)) return;
    
    for (int i = 0; i < secret_len; i++) {
        if (secret[i] == guess[i]) {
            (*bulls)++;
        }
    }
    
    int secret_counts[26] = {0};
    int guess_counts[26] = {0};
    
    for (int i = 0; i < secret_len; i++) {
        if (secret[i] != guess[i]) {
            secret_counts[secret[i] - 'a']++;
            guess_counts[guess[i] - 'a']++;
        }
    }
    
    for (int i = 0; i < 26; i++) {
        *cows += (secret_counts[i] < guess_counts[i]) ? secret_counts[i] : guess_counts[i];
    }
}
// Изменяем функцию cleanup() - убираем system() вызов:
void cleanup() {
    printf("\nОчистка ресурсов сервера...\n");
    
    if (main_state != NULL) munmap(main_state, sizeof(MainState));
    if (shm_fd_main != -1) {
        close(shm_fd_main);
        shm_unlink(SHM_MAIN_NAME);
    }
    if (sem_main != NULL) {
        sem_close(sem_main);
        sem_unlink(SEM_MAIN_NAME);
    }
    
    // Убираем system() вызов, чистим только то, что создали
    printf("Сервер завершает работу.\n");
}



// Генерация случайного слова
void generate_secret(char secret[MAX_WORD_LEN], int length) {
    static char *builtin_dict[] = {
        "apple", "banana", "cherry", "orange", "grape",
        "lemon", "peach", "pear", "plum", "melon",
        "bread", "water", "juice", "sugar", "honey",
        "table", "chair", "house", "world", "light"
    };
    int dict_size = sizeof(builtin_dict) / sizeof(builtin_dict[0]);
    
    static char external_dict[100][MAX_WORD_LEN];
    static int external_dict_loaded = 0;
    static int external_dict_size = 0;
    
    if (!external_dict_loaded) {
        external_dict_size = load_dictionary("../test/dictionary.txt", external_dict, 100);
        external_dict_loaded = 1;
    }
    
    // Ищем слова нужной длины
    char suitable_words[100][MAX_WORD_LEN];
    int suitable_count = 0;
    
    for (int i = 0; i < external_dict_size; i++) {
        if ((int)strlen(external_dict[i]) == length) {
            strcpy(suitable_words[suitable_count], external_dict[i]);
            suitable_count++;
        }
    }
    
    if (suitable_count == 0) {
        for (int i = 0; i < dict_size; i++) {
            if ((int)strlen(builtin_dict[i]) == length) {
                strcpy(suitable_words[suitable_count], builtin_dict[i]);
                suitable_count++;
            }
        }
    }
    
    if (suitable_count == 0) {
        for (int i = 0; i < length; i++) {
            secret[i] = 'a' + rand() % 26;
        }
        secret[length] = '\0';
        return;
    }
    
    int idx = rand() % suitable_count;
    strcpy(secret, suitable_words[idx]);
}

// Создание канала для клиента
int create_client_channel(int client_pid) {
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "%s%d", SHM_CLIENT_PREFIX, client_pid);
    
    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) return -1;
    
    if (ftruncate(shm_fd, sizeof(ClientChannel)) == -1) {
        close(shm_fd);
        return -1;
    }
    
    ClientChannel *channel = mmap(NULL, sizeof(ClientChannel), 
                                  PROT_READ | PROT_WRITE, 
                                  MAP_SHARED, shm_fd, 0);
    if (channel == MAP_FAILED) {
        close(shm_fd);
        return -1;
    }
    
    memset(channel, 0, sizeof(ClientChannel));
    channel->processed = 1;
    
    munmap(channel, sizeof(ClientChannel));
    close(shm_fd);
    
    char sem_name[64];
    snprintf(sem_name, sizeof(sem_name), "%s%d", SEM_CLIENT_PREFIX, client_pid);
    sem_t *sem = sem_open(sem_name, O_CREAT, 0666, 1);
    if (sem == SEM_FAILED) {
        shm_unlink(shm_name);
        return -1;
    }
    
    sem_close(sem);
    return 0;
}

// Удаление канала клиента
void remove_client_channel(int client_pid) {
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "%s%d", SHM_CLIENT_PREFIX, client_pid);
    shm_unlink(shm_name);
    
    char sem_name[64];
    snprintf(sem_name, sizeof(sem_name), "%s%d", SEM_CLIENT_PREFIX, client_pid);
    sem_unlink(sem_name);
}

// Создание новой игры (ТОЛЬКО через IPC!)
int create_game(const char *name, int max_players, int word_length) {
    if (word_length < 3 || word_length > MAX_WORD_LEN) return -1;
    
    sem_wait(sem_main);
    
    if (main_state->game_count >= MAX_GAMES) {
        sem_post(sem_main);
        return -2;
    }
    
    GameState *game = &main_state->games[main_state->game_count];
    game->id = main_state->game_count;
    strncpy(game->name, name, MAX_NAME_LEN - 1);
    game->name[MAX_NAME_LEN - 1] = '\0';
    
    game->word_length = word_length;
    generate_secret(game->secret, word_length);
    
    game->max_players = max_players;
    game->connected_players = 0;
    game->game_over = 0;
    game->winner = -1;
    game->move_count = 0;
    game->current_player = 0;
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        game->players[i] = -1;
        game->active[i] = 0;
        game->player_names[i][0] = '\0';
    }
    
    printf("Создана игра: %s (ID: %d), игроков: %d, длина: %d\n", 
           name, main_state->game_count, max_players, word_length);
    
    // Создаем shared memory для игры
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "%s%d", SHM_GAME_PREFIX, game->id);
    
    int shm_fd_game = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd_game == -1) {
        sem_post(sem_main);
        return -3;
    }
    
    ftruncate(shm_fd_game, sizeof(GameState));
    GameState *game_state = mmap(NULL, sizeof(GameState), 
                                 PROT_READ | PROT_WRITE, 
                                 MAP_SHARED, shm_fd_game, 0);
    if (game_state == MAP_FAILED) {
        close(shm_fd_game);
        sem_post(sem_main);
        return -4;
    }
    
    memcpy(game_state, game, sizeof(GameState));
    
    char sem_name[64];
    snprintf(sem_name, sizeof(sem_name), "%s%d", SEM_GAME_PREFIX, game->id);
    sem_t *sem_game = sem_open(sem_name, O_CREAT, 0666, 1);
    if (sem_game == SEM_FAILED) {
        munmap(game_state, sizeof(GameState));
        close(shm_fd_game);
        sem_post(sem_main);
        return -5;
    }
    
    munmap(game_state, sizeof(GameState));
    close(shm_fd_game);
    sem_close(sem_game);
    
    int game_id = main_state->game_count;
    main_state->game_count++;
    
    sem_post(sem_main);
    return game_id;
}
// Подключение клиента к игре
int connect_client_to_game(int game_id, int client_pid, const char *player_name) {
    sem_wait(sem_main);
    
    if (game_id < 0 || game_id >= main_state->game_count) {
        sem_post(sem_main);
        return -1;
    }
    
    GameState *game = &main_state->games[game_id];
    
    if (game->game_over) {
        sem_post(sem_main);
        return -2;
    }
    
    // Считаем только АКТИВНЫХ игроков
    int active_players = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game->players[i] != -1 && game->active[i]) {
            active_players++;
        }
    }
    
    if (active_players >= game->max_players) {
        sem_post(sem_main);
        return -3;
    }
    
    // Проверяем, не подключен ли уже
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game->players[i] == client_pid) {
            // Если уже подключен, но неактивен - делаем активным
            if (!game->active[i]) {
                game->active[i] = 1;
                strncpy(game->player_names[i], player_name, MAX_NAME_LEN - 1);
                game->player_names[i][MAX_NAME_LEN - 1] = '\0';
                sem_post(sem_main);
                return i;
            }
            sem_post(sem_main);
            return -4;
        }
    }
    
    // Находим свободный слот или неактивного игрока
    int player_slot = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game->players[i] == -1) {
            player_slot = i;
            break;
        } else if (!game->active[i]) {
            // Занимаем место неактивного игрока
            player_slot = i;
            break;
        }
    }
    
    if (player_slot == -1) {
        sem_post(sem_main);
        return -5;
    }
    
    // Подключаем/переподключаем игрока
    game->players[player_slot] = client_pid;
    game->active[player_slot] = 1;
    strncpy(game->player_names[player_slot], player_name, MAX_NAME_LEN - 1);
    game->player_names[player_slot][MAX_NAME_LEN - 1] = '\0';
    
    // Обновляем connected_players только если добавляем нового игрока
    if (game->connected_players < game->max_players) {
        game->connected_players++;
    }
    
    printf("Игрок %s подключился к игре %s (слот %d)\n", 
           player_name, game->name, player_slot);
    
    // Обновляем shared memory игры
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "%s%d", SHM_GAME_PREFIX, game_id);
    
    int shm_fd = shm_open(shm_name, O_RDWR, 0666);
    if (shm_fd != -1) {
        GameState *game_state = mmap(NULL, sizeof(GameState), 
                                     PROT_READ | PROT_WRITE, 
                                     MAP_SHARED, shm_fd, 0);
        if (game_state != MAP_FAILED) {
            memcpy(game_state, game, sizeof(GameState));
            munmap(game_state, sizeof(GameState));
        }
        close(shm_fd);
    }
    
    sem_post(sem_main);
    return player_slot;
}


// Обработка хода игрока
int process_player_guess(int game_id, int player_slot, const char *guess) {
    sem_wait(sem_main);
    
    if (game_id < 0 || game_id >= main_state->game_count) {
        sem_post(sem_main);
        return -1;
    }
    
    GameState *game = &main_state->games[game_id];
    
    if (game->game_over) {
        sem_post(sem_main);
        return -2;
    }
    
    if (player_slot < 0 || player_slot >= MAX_PLAYERS || game->players[player_slot] == -1) {
        sem_post(sem_main);
        return -3;
    }
    
    if (game->current_player != player_slot) {
        sem_post(sem_main);
        return -4;
    }
    
    if ((int)strlen(guess) != game->word_length) {
        sem_post(sem_main);
        return -5;
    }
    
    for (int i = 0; guess[i]; i++) {
        if (!isalpha(guess[i])) {
            sem_post(sem_main);
            return -6;
        }
    }
    
    char guess_lower[MAX_WORD_LEN];
    for (int i = 0; i <= (int)strlen(guess); i++) {
        guess_lower[i] = tolower(guess[i]);
    }
    
    int bulls, cows;
    check_bulls_cows(game->secret, guess_lower, &bulls, &cows);
    
    if (game->move_count < 100) {
        Move *move = &game->moves[game->move_count];
        strcpy(move->guess, guess_lower);
        move->player_id = game->players[player_slot];
        move->bulls = bulls;
        move->cows = cows;
        game->move_count++;
    }
    
    if (bulls == game->word_length) {
        game->game_over = 1;
        game->winner = game->players[player_slot];
        printf("Игрок %s угадал слово! Игра %s завершена.\n", 
               game->player_names[player_slot], game->name);
    } else {
        int next_player = (player_slot + 1) % MAX_PLAYERS;
        int attempts = 0;
        
        while (attempts < MAX_PLAYERS) {
            if (game->players[next_player] != -1 && game->active[next_player]) {
                game->current_player = next_player;
                break;
            }
            next_player = (next_player + 1) % MAX_PLAYERS;
            attempts++;
        }
    }
    
    // Обновляем shared memory
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "%s%d", SHM_GAME_PREFIX, game_id);
    
    int shm_fd = shm_open(shm_name, O_RDWR, 0666);
    if (shm_fd != -1) {
        GameState *game_state = mmap(NULL, sizeof(GameState), 
                                     PROT_READ | PROT_WRITE, 
                                     MAP_SHARED, shm_fd, 0);
        if (game_state != MAP_FAILED) {
            memcpy(game_state, game, sizeof(GameState));
            munmap(game_state, sizeof(GameState));
        }
        close(shm_fd);
    }
    
    sem_post(sem_main);
    return (bulls << 16) | cows;
}

// Отключение клиента от игры
void disconnect_client_from_game(int game_id, int player_slot) {
    sem_wait(sem_main);
    
    if (game_id >= 0 && game_id < main_state->game_count) {
        GameState *game = &main_state->games[game_id];
        
        if (player_slot >= 0 && player_slot < MAX_PLAYERS && 
            game->players[player_slot] != -1) {
            
            printf("Игрок %s отключился от игры %s\n", 
                   game->player_names[player_slot], game->name);
            
            game->active[player_slot] = 0;
            
            if (game->current_player == player_slot && !game->game_over) {
                int next_player = (player_slot + 1) % MAX_PLAYERS;
                int attempts = 0;
                
                while (attempts < MAX_PLAYERS) {
                    if (game->players[next_player] != -1 && game->active[next_player]) {
                        game->current_player = next_player;
                        break;
                    }
                    next_player = (next_player + 1) % MAX_PLAYERS;
                    attempts++;
                }
            }
            
            // Обновляем shared memory
            char shm_name[64];
            snprintf(shm_name, sizeof(shm_name), "%s%d", SHM_GAME_PREFIX, game_id);
            
            int shm_fd = shm_open(shm_name, O_RDWR, 0666);
            if (shm_fd != -1) {
                GameState *game_state = mmap(NULL, sizeof(GameState), 
                                             PROT_READ | PROT_WRITE, 
                                             MAP_SHARED, shm_fd, 0);
                if (game_state != MAP_FAILED) {
                    memcpy(game_state, game, sizeof(GameState));
                    munmap(game_state, sizeof(GameState));
                }
                close(shm_fd);
            }
        }
    }
    
    sem_post(sem_main);
}

// Добавляем отладку в process_single_client():
void* process_single_client(void *arg) {
    ThreadArgs *args = (ThreadArgs*)arg;
    int client_pid = args->client_pid;
    ClientChannel *channel = args->channel;
    
    char sem_name[64];
    snprintf(sem_name, sizeof(sem_name), "%s%d", SEM_CLIENT_PREFIX, client_pid);
    sem_t *sem = sem_open(sem_name, 0);
    
    if (sem == SEM_FAILED) {
        printf("[SERVER] Не удалось открыть семафор для PID %d\n", client_pid);
        free(args);
        return NULL;
    }
    
    sem_wait(sem);
    
    if (channel->processed == 0) {
        ClientMessage *req = &channel->request;
        ClientMessage *res = &channel->response;
        
        printf("[SERVER] Обрабатываю запрос от PID %d, тип: %d\n", 
               client_pid, req->type);
        
        memset(res, 0, sizeof(ClientMessage));
        res->type = req->type;
        res->player_id = req->player_id;
        res->game_id = req->game_id;
        
        switch (req->type) {
            case MSG_CREATE_GAME: {
                printf("[SERVER] Создание игры: %s\n", req->data.create_game.name);
                
                int result = create_game(
                    req->data.create_game.name,
                    req->data.create_game.max_players,
                    req->data.create_game.word_length
                );
                
                if (result >= 0) {
                    res->type = MSG_CREATE_GAME;
                    res->data.result[0] = result;
                    res->data.result[1] = 0;
                    printf("[SERVER] Игра создана с ID: %d\n", result);
                } else {
                    res->type = MSG_ERROR;
                    res->data.result[0] = result;
                    res->data.result[1] = 1;
                    printf("[SERVER] Ошибка создания игры: код %d\n", result);
                }
                break;
            }
            
            case MSG_CONNECT: {
                printf("[SERVER] Подключение к игре %d, имя: %s\n",
                       req->game_id, req->data.connect_info.player_name);
                
                int result = connect_client_to_game(req->game_id, client_pid, 
                                                  req->data.connect_info.player_name);
                if (result >= 0) {
                    res->type = MSG_CONNECT;
                    res->data.result[0] = result;
                    res->data.result[1] = 0;
                    printf("[SERVER] Успешное подключение, слот: %d\n", result);
                } else {
                    res->type = MSG_CONNECT;
                    res->data.result[0] = result;
                    res->data.result[1] = 1;
                    printf("[SERVER] Ошибка подключения: код %d\n", result);
                }
                break;
            }
            
            case MSG_GUESS: {
                printf("[SERVER] Ход от игрока %d в игре %d\n",
                       req->player_id, req->game_id);
                
                int result = process_player_guess(req->game_id, req->player_id, 
                                                 req->data.guess);
                if (result >= 0) {
                    res->type = MSG_RESULT;
                    res->data.result[0] = (result >> 16) & 0xFFFF;
                    res->data.result[1] = result & 0xFFFF;
                } else {
                    res->type = MSG_RESULT;
                    res->data.result[0] = -1;
                    res->data.result[1] = -result;
                }
                break;
            }
            
            case MSG_DISCONNECT: {
                printf("[SERVER] Отключение игрока %d от игры %d\n",
                       req->player_id, req->game_id);
                
                disconnect_client_from_game(req->game_id, req->player_id);
                res->type = MSG_DISCONNECT;
                res->data.result[0] = 0;
                res->data.result[1] = 0;
                remove_client_channel(client_pid);
                printf("[SERVER] Канал PID %d удален\n", client_pid);
                break;
            }
        }
        
        channel->processed = 1;
        printf("[SERVER] Запрос от PID %d обработан\n", client_pid);
    }
    
    sem_post(sem);
    sem_close(sem);
    
    munmap(channel, sizeof(ClientChannel));
    close(args->shm_fd);
    free(args);
    
    return NULL;
}// Увеличиваем частоту сканирования и уменьшаем диапазон
void process_client_requests() {
    pthread_t threads[50];
    int thread_count = 0;
    
    // Сканируем только разумный диапазон PID
    for (int pid = getpid()-1000; pid <= getpid()+1000; pid++) {
        if (pid <= 0) continue;
        
        char shm_name[64];
        snprintf(shm_name, sizeof(shm_name), "%s%d", SHM_CLIENT_PREFIX, pid);
        
        int shm_fd = shm_open(shm_name, O_RDWR, 0666);
        if (shm_fd == -1) continue;
        
        ClientChannel *channel = mmap(NULL, sizeof(ClientChannel), 
                                      PROT_READ | PROT_WRITE, 
                                      MAP_SHARED, shm_fd, 0);
        if (channel == MAP_FAILED) {
            close(shm_fd);
            continue;
        }
        
        char sem_name[64];
        snprintf(sem_name, sizeof(sem_name), "%s%d", SEM_CLIENT_PREFIX, pid);
        sem_t *sem = sem_open(sem_name, 0);
        
        if (sem != SEM_FAILED) {
            sem_wait(sem);
            int has_request = (channel->processed == 0);
            sem_post(sem);
            sem_close(sem);
            
            if (has_request) {
                printf("[SERVER] Обрабатываю запрос от PID %d\n", pid);
                
                ThreadArgs *args = malloc(sizeof(ThreadArgs));
                args->client_pid = pid;
                args->channel = channel;
                args->shm_fd = shm_fd;
                
                if (pthread_create(&threads[thread_count], NULL, 
                                 process_single_client, args) == 0) {
                    thread_count++;
                    if (thread_count >= 50) break;
                } else {
                    free(args);
                    munmap(channel, sizeof(ClientChannel));
                    close(shm_fd);
                }
            } else {
                munmap(channel, sizeof(ClientChannel));
                close(shm_fd);
            }
        } else {
            munmap(channel, sizeof(ClientChannel));
            close(shm_fd);
        }
    }
    
    for (int i = 0; i < thread_count; i++) {
        pthread_join(threads[i], NULL);
    }
}



// Изменяем init_server() - убираем system() вызов:
int init_server() {
    srand(time(NULL));
    
    // Убираем system() вызов
    // Просто пытаемся удалить наши объекты
    shm_unlink(SHM_MAIN_NAME);
    
    shm_fd_main = shm_open(SHM_MAIN_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd_main == -1) {
        perror("shm_open main");
        return -1;
    }
    
    if (ftruncate(shm_fd_main, sizeof(MainState)) == -1) {
        perror("ftruncate main");
        close(shm_fd_main);
        return -1;
    }
    
    main_state = mmap(NULL, sizeof(MainState), 
                      PROT_READ | PROT_WRITE, 
                      MAP_SHARED, shm_fd_main, 0);
    if (main_state == MAP_FAILED) {
        perror("mmap main");
        close(shm_fd_main);
        return -1;
    }
    
    memset(main_state, 0, sizeof(MainState));
    main_state->game_count = 0;
    
    // Удаляем старый семафор, если он есть
    sem_unlink(SEM_MAIN_NAME);
    
    sem_main = sem_open(SEM_MAIN_NAME, O_CREAT, 0666, 1);
    if (sem_main == SEM_FAILED) {
        perror("sem_open main");
        munmap(main_state, sizeof(MainState));
        close(shm_fd_main);
        return -1;
    }
    
    // Создаем тестовые игры (тоже через create_game для consistency)
    create_game("Короткие слова", 2, 4);
    create_game("Средние слова", 3, 6);
    create_game("Длинные слова", 2, 8);
    
    printf("Сервер инициализирован\n");
    printf("Создано %d тестовых игр\n", main_state->game_count);
    return 0;
}

// Основной цикл сервера
void run_server() {
    printf("\nСервер запущен. Ожидание подключений...\n");
    printf("Нажмите Ctrl+C для завершения.\n\n");
    
    while (1) {
        process_client_requests();
        usleep(100000);
    }
}

int main() {
    atexit(cleanup);
    
    printf("=== Сервер игры 'Быки и коровы' (слова) ===\n");
    
    if (init_server() < 0) {
        fprintf(stderr, "Ошибка инициализации сервера\n");
        return 1;
    }
    
    run_server();
    return 0;
}