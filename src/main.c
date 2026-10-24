#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include "sim.h"

// Глобальный флаг для обработки Ctrl+C
volatile sig_atomic_t keep_running = 1;

// Глобальное состояние мира (доступно потокам через extern в sim.h)
World world;

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
    // Инициализация мира
    srand(time(NULL));
    world_init(&world);

    // Настройка обработчика сигнала Ctrl+C
    signal(SIGINT, signal_handler);

    my_print("=== Огонь по движущимся танкам ===\n");
    my_print("Нажмите Ctrl+C для принудительного завершения\n\n");

    // Создаем массивы для хранения идентификаторов потоков
    pthread_t tank_threads[MAX_TANKS];
    pthread_t gun_threads[MAX_GUNS];

    // Запускаем потоки для каждого танка и орудия
    for (int i = 0; i < world.n_tanks; i++) {
        pthread_create(&tank_threads[i], NULL, tank_thread_func, &world.tanks[i]);
    }
    for (int i = 0; i < world.n_guns; i++) {
        pthread_create(&gun_threads[i], NULL, gun_thread_func, &world.guns[i]);
    }

    int total_threads = world.n_tanks + world.n_guns;

    // Главный цикл симуляции
    while (keep_running && !check_natural_end(&world)) {
        pthread_mutex_lock(&world.world_mutex);
        world.n_events = 0;
        world.tick_id++; // Объявляем новый такт
        pthread_cond_broadcast(&world.tick_cond); // Будим все рабочие потоки
        pthread_mutex_unlock(&world.world_mutex);

        // Ждём отчёта каждого потока. Семафор считающий, поэтому просто
        // забираем ровно столько отчётов, сколько у нас потоков.
        for (int i = 0; i < total_threads; i++) {
            // Ctrl+C прерывает sem_wait, поэтому ждём повторно
            while (sem_wait(&world.done_sem) == -1) {
                ;
            }
        }

        // Все потоки сделали ровно по одному действию и снова ждут такт,
        // поэтому мир сейчас никто не меняет
        pthread_mutex_lock(&world.world_mutex);
        update_shells(&world);
        draw_field(&world);
        world.tick++;
        pthread_mutex_unlock(&world.world_mutex);

        // Задержка для анимации (вне мьютекса, чтобы не блокировать систему зря)
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = world.cfg.tick_delay_ms * 1000000L;
        nanosleep(&ts, NULL);
    }

    // Корректное завершение: останавливаем потоки и обязательно будим их,
    // иначе они навсегда остаются в pthread_cond_wait и join зависает
    int was_interrupted = !keep_running;
    keep_running = 0;
    pthread_mutex_lock(&world.world_mutex);
    pthread_cond_broadcast(&world.tick_cond);
    pthread_mutex_unlock(&world.world_mutex);

    // Ждем фактического завершения всех потоков (join)
    for (int i = 0; i < world.n_tanks; i++) {
        pthread_join(tank_threads[i], NULL);
    }
    for (int i = 0; i < world.n_guns; i++) {
        pthread_join(gun_threads[i], NULL);
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
    my_print("Причина: %s\n", was_interrupted ? "прервано пользователем (Ctrl+C)" : "естественное завершение");
    print_stats(&world);

    // Лог на диск
    FILE *log = fopen("tankfire.log", "w");
    if (log) {
        fprintf(log, "=== ОТЧЕТ О СИМУЛЯЦИИ ===\n");
        fprintf(log, "Причина завершения: %s\n", was_interrupted ? "Прервано (Ctrl+C)" : "Естественное");
        fprintf(log, "Всего тактов: %d\n", world.tick);
        fprintf(log, "Выстрелов: %d\n", world.stats.shots_fired);
        fprintf(log, "Прямых попаданий: %d\n", world.stats.direct_hits);
        fprintf(log, "Близких разрывов: %d\n", world.stats.near_hits);
        fprintf(log, "Промахов: %d\n", world.stats.misses);
        fprintf(log, "Уничтожено танков: %d\n", world.stats.tanks_destroyed);
        fclose(log);
        my_print("\n[Лог сохранен в файл tankfire.log]\n");
    }

    // Освобождаем ресурсы синхронизации
    pthread_mutex_destroy(&world.world_mutex);
    pthread_cond_destroy(&world.tick_cond);
    sem_destroy(&world.done_sem);

    return 0;
}
