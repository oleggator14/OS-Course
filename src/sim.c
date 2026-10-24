#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "sim.h"

// Генерация препятствий
void generate_obstacles(World *w) {
    int placed = 0, guard = 0;
    while (placed < w->cfg.num_obstacles && guard < 1000) {
        int x = rand() % FIELD_W;
        int y = rand() % FIELD_H;
        guard++;
        if (y == 3 || (x == 1 && y == 5)) {
            continue; // линия танков и орудие
        }
        if (w->field[y][x] == CELL_EMPTY) {
            w->field[y][x] = CELL_OBSTACLE;
            placed++;
        }
    }
}

// Инициализация мира
void world_init(World *w) {
    // Очищаем поле от мусора в памяти
    memset(w->field, CELL_EMPTY, sizeof(w->field));

    // Настройки по умолчанию
    w->cfg.max_ticks = 10;
    w->cfg.tick_delay_ms = 500;
    w->cfg.tank_hp = 3;
    w->cfg.gun_ammo = 10;

    w->tick = 0;
    w->n_tanks = 3;
    w->n_guns = 1;

    // Расставляем препятствия
    w->cfg.num_obstacles = 15;
    generate_obstacles(w);

    // Создаем танки
    for (int i = 0; i < w->n_tanks; i++) {
        w->tanks[i].id = i + 1;
        w->tanks[i].x = 5 + i * 4;
        w->tanks[i].y = 3;
        w->tanks[i].hp = w->cfg.tank_hp;
        w->tanks[i].hp_max = w->cfg.tank_hp;
        w->tanks[i].alive = 1;
        w->tanks[i].dx = 1;
        w->tanks[i].dy = 0;
    }

    // Создаем орудие
    w->guns[0].id = 1;
    w->guns[0].x = 1;
    w->guns[0].y = 5;
    w->guns[0].ammo = w->cfg.gun_ammo;
    w->guns[0].target_id = -1;
    w->guns[0].ready_tick = 0;
    w->guns[0].obs_range = 8;
    w->guns[0].flight_ticks = 2;
    w->guns[0].dmg_direct = 2;
    w->guns[0].dmg_splash = 1;

    // Очищаем снаряды
    memset(w->shells, 0, sizeof(w->shells));

    // Очищаем статистику
    memset(&w->stats, 0, sizeof(w->stats));

    // Инициализация синхропримитивов для многопоточности
    pthread_mutex_init(&w->world_mutex, NULL);
    pthread_cond_init(&w->tick_cond, NULL); // Раньше её забыли инициализировать
    sem_init(&w->done_sem, 0, 0);
    w->tick_id = 0;
}

void add_event(World *w, const char *fmt, ...) {
    if (w->n_events >= MAX_EVENTS) {
        return; // лента переполнена - молча пропускаем
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(w->events[w->n_events], EVENT_LEN, fmt, args);
    va_end(args);
    w->n_events++;
}

// Обновление танков, в потоках
void *tank_thread_func(void *arg) {
    Tank *t = (Tank *) arg;
    int dx[4] = {1, -1, 0, 0};
    int dy[4] = {0, 0, 1, -1};

    int last_tick = 0; // Номер такта, который этот поток уже отработал

    while (1) {
        pthread_mutex_lock(&world.world_mutex);

        // Ждём именно НОВЫЙ номер такта. Флага "можно действовать" мало:
        // поток, отработав, тут же видел его снова взведённым и ходил
        // по несколько раз за такт.
        while (world.tick_id == last_tick && keep_running) {
            pthread_cond_wait(&world.tick_cond, &world.world_mutex);
        }

        // Если программа завершается, выходим из потока
        if (!keep_running) {
            pthread_mutex_unlock(&world.world_mutex);
            break;
        }
        last_tick = world.tick_id;

        // --- Логика движения (ровно 1 раз за такт) ---
        if (t->alive) {
            if (rand() % 100 < 20) {
                int dir = rand() % 4;
                t->dx = dx[dir];
                t->dy = dy[dir];
            }
            int nx = t->x + t->dx;
            int ny = t->y + t->dy;

            if (nx >= 0 && nx < FIELD_W && ny >= 0 && ny < FIELD_H && world.field[ny][nx] != CELL_OBSTACLE &&
                world.field[ny][nx] != CELL_WRECK) {
                add_event(&world, "Танк %d: (%d,%d) -> (%d,%d)", t->id, t->x, t->y, nx, ny);
                t->x = nx;
                t->y = ny;
            } else {
                int dir = rand() % 4;
                t->dx = dx[dir];
                t->dy = dy[dir];
                add_event(&world, "Танк %d: упёрся в границу/препятствие, меняет курс", t->id);
            }
        }

        // Сообщаем главному потоку, что мы закончили действие на этом такте
        pthread_mutex_unlock(&world.world_mutex);
        sem_post(&world.done_sem);
    }
    return NULL;
}

// Обновление орудий, в потоках
void *gun_thread_func(void *arg) {
    Gun *g = (Gun *) arg;

    int last_tick = 0;

    while (1) {
        pthread_mutex_lock(&world.world_mutex);

        // Орудие ждёт новый такт той же условной переменной, что и танки.
        // Раньше оно висело на семафоре, который никто не открывал,
        // поэтому орудия вообще ни разу не стреляли.
        while (world.tick_id == last_tick && keep_running) {
            pthread_cond_wait(&world.tick_cond, &world.world_mutex);
        }

        if (!keep_running) {
            pthread_mutex_unlock(&world.world_mutex);
            break;
        }
        last_tick = world.tick_id;

        if (g->ammo <= 0 || world.tick < g->ready_tick) {
            pthread_mutex_unlock(&world.world_mutex);
            sem_post(&world.done_sem); // Отчитаться обязаны в любом случае
            continue;
        }

        // Ищем ближайшего живого танка
        int best_id = -1, best_dist = 999999;
        for (int j = 0; j < world.n_tanks; j++) {
            Tank *t = &world.tanks[j];
            if (!t->alive) {
                continue;
            }
            int dist = abs(t->x - g->x) + abs(t->y - g->y);
            if (dist < best_dist && dist <= g->obs_range) {
                best_dist = dist;
                best_id = t->id;
            }
        }

        if (best_id < 0) {
            if (g->target_id != -1) {
                add_event(&world, "Орудие %d: цель потеряна", g->id);
            }
            g->target_id = -1;
            pthread_mutex_unlock(&world.world_mutex);
            sem_post(&world.done_sem);
            continue;
        }

        g->target_id = best_id;
        Tank *target = &world.tanks[best_id - 1];
        g->last_seen_x = target->x;
        g->last_seen_y = target->y;

        // Ищем свободный слот для снаряда
        int slot = -1;
        for (int k = 0; k < MAX_SHELLS; k++) {
            if (!world.shells[k].active) {
                slot = k;
                break;
            }
        }

        if (slot >= 0) {
            Shell *s = &world.shells[slot];
            s->active = 1;
            s->gun_id = g->id;
            s->target_x = target->x;
            s->target_y = target->y;
            s->impact_tick = world.tick + g->flight_ticks;
            s->dmg_direct = g->dmg_direct;
            s->dmg_splash = g->dmg_splash;
            g->ammo--;
            g->ready_tick = world.tick + 3;
            world.stats.shots_fired++;
            add_event(&world, "Орудие %d: ВЫСТРЕЛ по (%d,%d), разрыв на такте %d, снарядов осталось %d", g->id,
                      s->target_x, s->target_y, s->impact_tick, g->ammo);
        }

        pthread_mutex_unlock(&world.world_mutex);
        sem_post(&world.done_sem);
    }

    return NULL;
}

// Обновление снарядов
void update_shells(World *w) {
    for (int i = 0; i < MAX_SHELLS; i++) {
        Shell *s = &w->shells[i];
        if (!s->active || s->impact_tick > w->tick) {
            continue;
        }
        s->active = 0;

        int outcome = 0; // 0 промах, 1 близкий, 2 прямое
        for (int j = 0; j < w->n_tanks; j++) {
            Tank *t = &w->tanks[j];
            if (!t->alive) {
                continue;
            }
            int dist = abs(t->x - s->target_x) + abs(t->y - s->target_y);

            if (dist == 0) {
                t->hp -= s->dmg_direct;
                outcome = 2;
                add_event(w, "Разрыв (%d,%d): ПРЯМОЕ ПОПАДАНИЕ в танк %d, урон %d, прочность %d/%d", s->target_x,
                          s->target_y, t->id, s->dmg_direct, t->hp > 0 ? t->hp : 0, t->hp_max);
            } else if (dist == 1) {
                t->hp -= s->dmg_splash;
                if (outcome < 1) {
                    outcome = 1;
                }
                add_event(w, "Разрыв (%d,%d): близкий разрыв у танка %d, урон %d, прочность %d/%d", s->target_x,
                          s->target_y, t->id, s->dmg_splash, t->hp > 0 ? t->hp : 0, t->hp_max);
            }

            if (t->hp <= 0) {
                t->hp = 0;
                t->alive = 0;
                w->field[t->y][t->x] = CELL_WRECK;
                w->stats.tanks_destroyed++;
                add_event(w, "Танк %d УНИЧТОЖЕН в (%d,%d), остаётся препятствием", t->id, t->x, t->y);
            }
        }

        if (outcome == 2) {
            w->stats.direct_hits++;
        } else if (outcome == 1) {
            w->stats.near_hits++;
        } else {
            w->stats.misses++;
            add_event(w, "Разрыв (%d,%d): ПРОМАХ, танков рядом нет", s->target_x, s->target_y);
        }
    }
}

// Отрисовка поля
void draw_field(const World *w) {
    my_print("\033[H\033[2J"); // очистка экрана
    my_print("=== Огонь по движущимся танкам | такт %d", w->tick);
    if (w->cfg.max_ticks > 0) {
        my_print(" / %d", w->cfg.max_ticks);
    }
    my_print(" ===\n\n  ");
    for (int x = 0; x < FIELD_W; x++) {
        my_print("%d", x % 10);
    }
    my_print("\n");

    for (int y = 0; y < FIELD_H; y++) {
        my_print("%2d", y);
        for (int x = 0; x < FIELD_W; x++) {
            int drawn = 0;
            for (int i = 0; i < w->n_tanks && !drawn; i++) {
                if (w->tanks[i].alive && w->tanks[i].x == x && w->tanks[i].y == y) {
                    my_print("%d", w->tanks[i].id);
                    drawn = 1;
                }
            }
            for (int i = 0; i < w->n_guns && !drawn; i++) {
                if (w->guns[i].x == x && w->guns[i].y == y) {
                    my_print("G");
                    drawn = 1;
                }
            }
            if (!drawn) {
                if (w->field[y][x] == CELL_OBSTACLE) {
                    my_print("#");
                } else if (w->field[y][x] == CELL_WRECK) {
                    my_print("X");
                } else {
                    my_print(".");
                }
            }
        }
        my_print("\n");
    }
    my_print("Легенда: # препятствие, G орудие, цифра - танк, X - подбитый танк\n");

    my_print("----------------------------------------\n");
    my_print("Орудия:\n");
    for (int i = 0; i < w->n_guns; i++) {
        my_print("  G%d: pos(%d,%d) снарядов:%d цель:%d\n", w->guns[i].id, w->guns[i].x, w->guns[i].y, w->guns[i].ammo,
                 w->guns[i].target_id);
    }

    my_print("Танки:\n");
    for (int i = 0; i < w->n_tanks; i++) {
        if (w->tanks[i].alive) {
            my_print("  T%d: pos(%d,%d) HP:%d/%d\n", w->tanks[i].id, w->tanks[i].x, w->tanks[i].y, w->tanks[i].hp,
                     w->tanks[i].hp_max);
        } else {
            my_print("  T%d: [WRECK] pos(%d,%d)\n", w->tanks[i].id, w->tanks[i].x, w->tanks[i].y);
        }
    }

    my_print("Препятствия: ");
    int cnt = 0;
    for (int y = 0; y < FIELD_H; y++) {
        for (int x = 0; x < FIELD_W; x++) {
            if (w->field[y][x] == CELL_OBSTACLE) {
                my_print("(%d,%d) ", x, y);
                if (++cnt % 6 == 0) {
                    my_print("\n             ");
                }
            }
        }
    }
    if (cnt == 0) {
        my_print("(нет)");
    }
    my_print("\n");

    my_print("----------------------------------------\n");
    my_print("События такта %d:\n", w->tick);
    if (w->n_events == 0) {
        my_print("  (нет)\n");
    }
    for (int i = 0; i < w->n_events; i++) {
        my_print("  > %s\n", w->events[i]);
    }
    my_print("----------------------------------------\n");
}

// Проверка летящих снарядов
int shells_in_flight(const World *w) {
    for (int i = 0; i < MAX_SHELLS; i++) {
        if (w->shells[i].active) {
            return 1;
        }
    }
    return 0;
}

// Вывод статистики
void print_stats(const World *w) {
    my_print("\n=== Статистика ===\n");
    my_print("Выстрелов: %d\n", w->stats.shots_fired);
    my_print("Прямых попаданий: %d\n", w->stats.direct_hits);
    my_print("Близких разрывов: %d\n", w->stats.near_hits);
    my_print("Промахов: %d\n", w->stats.misses);
    my_print("Уничтожено танков: %d\n", w->stats.tanks_destroyed);
}
