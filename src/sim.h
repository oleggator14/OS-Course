#ifndef SIM_H
#define SIM_H

#include <stdarg.h>
#include <signal.h>

// Размеры поля
#define FIELD_W 20
#define FIELD_H 10

// Максимальное количество сущностей
#define MAX_TANKS 5
#define MAX_GUNS 2
#define MAX_SHELLS 20
#define MAX_EVENTS 20
#define EVENT_LEN 128

// Типы клеток
typedef enum {
    CELL_EMPTY = 0,
    CELL_OBSTACLE,
    CELL_WRECK  // Подбитый танк (препятствие)
} CellType;

// Структура танка
typedef struct {
    int id;
    int x, y;
    int hp;
    int hp_max;
    int alive;
    int dx, dy;  // Направление движения
} Tank;

// Структура орудия
typedef struct {
    int id;
    int x, y;
    int ammo;
    int obs_range;      // Дальность наблюдения
    int dmg_direct;     // Урон прямого попадания
    int dmg_splash;     // Урон близкого разрыва
    int flight_ticks;   // Время полета снаряда
    int target_id;      // ID цели (-1 если нет)
    int last_seen_x;    // Последнее известное положение цели
    int last_seen_y;
    int ready_tick;     // Когда можно стрелять
} Gun;

// Структура снаряда
typedef struct {
    int active;
    int gun_id;
    int target_x, target_y;  // Куда летит
    int impact_tick;         // Когда взорвется
    int dmg_direct;          // Копируется из орудия при выстреле
    int dmg_splash;          // Копируется из орудия при выстреле
} Shell;

// Конфигурация
typedef struct {
    int max_ticks;          // 0 = без ограничения
    int tick_delay_ms;      // Задержка между тактами
    int tank_hp;
    int gun_ammo;
    int num_obstacles;      // Количество препятствий на поле
} Config;

// Статистика
typedef struct {
    int shots_fired;
    int direct_hits;
    int near_hits;
    int misses;
    int tanks_destroyed;
} Stats;

// Мир (все состояние игры)
typedef struct {
    Config cfg;
    int tick;
    CellType field[FIELD_H][FIELD_W];
    Tank tanks[MAX_TANKS];
    int n_tanks;
    Gun guns[MAX_GUNS];
    int n_guns;
    Shell shells[MAX_SHELLS];
    Stats stats;
    char events[MAX_EVENTS][EVENT_LEN]; // Массив строк для событий такта
    int n_events;                       // Счетчик событий
} World;

// Прототипы функций
void world_init(World *w);
void generate_obstacles(World *w);
void update_tanks(World *w);
void update_guns(World *w);
void update_shells(World *w);
void draw_field(const World *w);
void print_stats(const World *w);
void my_print(const char *fmt, ...); 
int shells_in_flight(const World *w);
void add_event(World *w, const char *fmt, ...);

#endif
