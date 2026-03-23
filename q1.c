#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>

#define N 5
#define NUM_READERS 20
#define NUM_WRITERS 10

sem_t readerLimit;
sem_t wrt;
pthread_mutex_t mutex;
int cnt = 1;
int numreader = 0;
bool test_succeeded = true;

double average_blocked_wait_time_s = 0.0;
double average_blocked_wait_time_numerator = 0.0;
int average_blocked_wait_time_denominator = 0;

double average_unblocked_wait_time_s = 0.0;
double average_unblocked_wait_time_numerator = 0.0;
int average_unblocked_wait_time_denominator = 0;

// Print and log a line to both stdout and the log file
void log_print(const char *fmt, ...) {
    va_list args1, args2;
    va_start(args1, fmt);
    va_copy(args2, args1);
    vprintf(fmt, args1);
    va_end(args1);
    va_end(args2);
}

void *writer(void *wno) {
    sem_wait(&wrt);
    cnt = cnt * 2;

    pthread_mutex_lock(&mutex);
    log_print("| %-8s | %-10s | %-14s | %-10d | %-10s |\n",
              "WRITER", "WRITE", "-", cnt, "-");
    pthread_mutex_unlock(&mutex);

    struct timespec work_time = {2, 0};
    nanosleep(&work_time, NULL);

    sem_post(&wrt);
    return NULL;
}

void *reader(void *rno) {
    int sval;
    struct timespec time_start, time_end;

    sem_getvalue(&readerLimit, &sval);
    int will_wait = (sval == 0);

    clock_gettime(CLOCK_MONOTONIC, &time_start);
    sem_wait(&readerLimit);
    clock_gettime(CLOCK_MONOTONIC, &time_end);

    double time_waited_s = (time_end.tv_sec - time_start.tv_sec) +
                           (time_end.tv_nsec - time_start.tv_nsec) / 1e9;

    pthread_mutex_lock(&mutex);

    if (will_wait) {
        average_blocked_wait_time_numerator += time_waited_s;
        average_blocked_wait_time_denominator++;
    } else {
        average_unblocked_wait_time_numerator += time_waited_s;
        average_unblocked_wait_time_denominator++;
    }

    sem_getvalue(&readerLimit, &sval);
    numreader++;
    if (numreader > N) test_succeeded = false;

    log_print("| %-8d | %-10s | %-14s | %-10d | %-10.6f |\n",
              *((int *)rno),
              "ENTER",
              will_wait ? "BLOCKED" : "IMMEDIATE",
              numreader,
              time_waited_s);

    if (numreader == 1)
        sem_wait(&wrt);

    pthread_mutex_unlock(&mutex);

    // Reading section
    pthread_mutex_lock(&mutex);
    log_print("| %-8d | %-10s | %-14s | %-10d | %-10s |\n",
              *((int *)rno), "READ", "-", cnt, "-");
    pthread_mutex_unlock(&mutex);

    struct timespec work_time = {2, 0};
    nanosleep(&work_time, NULL);

    pthread_mutex_lock(&mutex);
    numreader--;
    log_print("| %-8d | %-10s | %-14s | %-10d | %-10s |\n",
              *((int *)rno), "EXIT", "-", numreader, "-");
    if (numreader == 0)
        sem_post(&wrt);
    pthread_mutex_unlock(&mutex);

    sem_post(&readerLimit);
    return NULL;
}

int main() {
    logfile = fopen(LOG_FILE, "w");
    if (!logfile) {
        perror("Failed to open log file");
        return 1;
    }

    // Print header
    char *divider = "+----------+------------+----------------+------------+------------+\n";
    log_print("%s", divider);
    log_print("| %-8s | %-10s | %-14s | %-10s | %-10s |\n",
              "THREAD", "EVENT", "WAIT TYPE", "VAL/ACTIVE", "WAIT (s)");
    log_print("%s", divider);

    sem_init(&readerLimit, 0, N);
    pthread_mutex_init(&mutex, NULL);
    sem_init(&wrt, 0, 1);

    pthread_t read[NUM_READERS], write[NUM_WRITERS];
    int a[NUM_READERS], b[NUM_WRITERS];
    for (int i = 0; i < NUM_READERS; i++) a[i] = i + 1;
    for (int i = 0; i < NUM_WRITERS;  i++) b[i] = i + 1;

    for (int i = 0; i < NUM_READERS; i++)
        pthread_create(&read[i],  NULL, (void *)reader, (void *)&a[i]);
    for (int i = 0; i < NUM_WRITERS; i++)
        pthread_create(&write[i], NULL, (void *)writer, (void *)&b[i]);

    for (int i = 0; i < NUM_READERS; i++) pthread_join(read[i],  NULL);
    for (int i = 0; i < NUM_WRITERS; i++) pthread_join(write[i], NULL);

    // Summary
    if (average_blocked_wait_time_denominator > 0)
        average_blocked_wait_time_s = average_blocked_wait_time_numerator / average_blocked_wait_time_denominator;
    if (average_unblocked_wait_time_denominator > 0)
        average_unblocked_wait_time_s = average_unblocked_wait_time_numerator / average_unblocked_wait_time_denominator;

    log_print("%s", divider);
    log_print("\n=== SUMMARY ===\n");
    log_print("%-40s %s\n", "Result:",
              test_succeeded ? "PASSED - reader cap never exceeded" : "FAILED - reader cap exceeded");
    log_print("%-40s %d / %d readers\n", "Max concurrent readers allowed:", N, NUM_READERS);
    log_print("%-40s %d\n", "Readers that blocked:",   average_blocked_wait_time_denominator);
    log_print("%-40s %d\n", "Readers that didn't block:", average_unblocked_wait_time_denominator);
    log_print("%-40s %.9f s\n", "Avg wait (BLOCKED):",   average_blocked_wait_time_s);
    log_print("%-40s %.9f s\n", "Avg wait (UNBLOCKED):", average_unblocked_wait_time_s);
    log_print("%-40s %.9f s\n", "Difference:",
              average_blocked_wait_time_s - average_unblocked_wait_time_s);

    fclose(logfile);
    printf("\nLog written to %s\n", LOG_FILE);

    pthread_mutex_destroy(&mutex);
    sem_destroy(&wrt);
    sem_destroy(&readerLimit);
    return 0;
}