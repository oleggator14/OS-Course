#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include "sim.h"

// Глобальный флаг для обработки Ctrl+C
volatile sig_atomic_t keep_running = 1;

// Обработчик сигнала SIGINT (Ctrl+C)
void signal_handler(int sig) {
    (void) sig; // Подавляем предупреждение о неиспользуемом параметре
    keep_running = 0;
}

// Простая функция вывода через write()
void my_print(const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        if ((size_t) len >= sizeof(buf)) {
            len = sizeof(buf) - 1; // Защита от выхода за границы буфера
        }
        write(STDOUT_FILENO, buf, len);
    }
}

// Проверка условий естественного завершения
int check_natural_end(const World *w) {
    // 1. Все танки уничтожены
    int alive_tanks = 0;
    for (int i = 0; i < w->n_tanks; i++) {
        if (w->tanks[i].alive) {
            alive_tanks++;
        }
    }
    if (alive_tanks == 0) {
        return 1;
    }

    // 2. У всех орудий кончились патроны
    int total_ammo = 0;
    for (int i = 0; i < w->n_guns; i++) {
        total_ammo += w->guns[i].ammo;
    }
    if (total_ammo == 0) {
        return 1;
    }

    // 3. Достигнут лимит тактов
    if (w->cfg.max_ticks > 0 && w->tick > w->cfg.max_ticks) {
        return 1;
    }

    return 0;
}

int main() {
    World world;

    // Инициализация мира
    srand(time(NULL));
    world_init(&world);

    // Настройка обработчика сигнала Ctrl+C
    signal(SIGINT, signal_handler);

    my_print("=== Огонь по движущимся танкам ===\n");
    my_print("Нажмите Ctrl+C для принудительного завершения\n\n");

    // Главный цикл симуляции
    while (keep_running && !check_natural_end(&world)) {
        world.n_events = 0;

        // 1. Обновляем состояние
        update_tanks(&world);
        update_guns(&world);
        update_shells(&world);

        // 2. Рисуем кадр
        draw_field(&world);

        // 3. Задержка
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = world.cfg.tick_delay_ms * 1000000L;
        nanosleep(&ts, NULL);

        world.tick++;
    }

    /* Докручиваем летящие снаряды: мир "застыл", но разрывы должны состояться.
    Иначе статистика не сойдётся при выходе по лимиту тактов. */
    while (shells_in_flight(&world)) {
        world.n_events = 0;
        world.tick++;
        update_shells(&world);
    }

    // Вывод итоговой статистики
    my_print("\n=== Симуляция завершена ===\n");
    my_print("Причина: %s\n", keep_running ? "естественное завершение" : "прервано пользователем (Ctrl+C)");
    print_stats(&world);

    // Лог на диск
    FILE *log = fopen("tankfire.log", "w");
    if (log) {
        fprintf(log, "=== ОТЧЕТ О СИМУЛЯЦИИ ===\n");
        fprintf(log, "Причина завершения: %s\n", keep_running ? "Естественное" : "Прервано (Ctrl+C)");
        fprintf(log, "Всего тактов: %d\n", world.tick);
        fprintf(log, "Выстрелов: %d\n", world.stats.shots_fired);
        fprintf(log, "Прямых попаданий: %d\n", world.stats.direct_hits);
        fprintf(log, "Близких разрывов: %d\n", world.stats.near_hits);
        fprintf(log, "Промахов: %d\n", world.stats.misses);
        fprintf(log, "Уничтожено танков: %d\n", world.stats.tanks_destroyed);
        fclose(log);
        my_print("\n[Лог сохранен в файл tankfire.log]\n");
    }

    return 0;
}
