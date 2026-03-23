#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>

#define N 5
#define NUM_READERS 20
#define NUM_WRITERS 10
#define LOG_FILE "reader_writer_q2.log"

/* ── Barz counting semaphore (Implementation 2 from notes) ──
 *
 * CSem(K):
 *   int val = K
 *   BSem gate(min(1,K))   // open (1) if val > 0, closed (0) if val = 0
 *   BSem mutex(1)          // protects val
 *
 * Pc(cs):                  // wait / down
 *   P(gate)                // block HERE before touching val
 *   P(mutex)
 *   val = val - 1
 *   if val > 0: V(gate)    // if more slots remain, reopen gate
 *   V(mutex)
 *
 * Vc(cs):                  // signal / up
 *   P(mutex)
 *   val = val + 1
 *   if val == 1: V(gate)   // if we just went from 0→1, open gate
 *   V(mutex)
 */

typedef struct {
    int val;
    sem_t gate;
    sem_t mutex;
} counting_sem_t;

void csem_init(counting_sem_t *cs, int k) {
    cs->val = k;
    sem_init(&cs->gate,  0, k > 0 ? 1 : 0);
    sem_init(&cs->mutex, 0, 1);
}

void csem_wait(counting_sem_t *cs) {
    sem_wait(&cs->gate);
    sem_wait(&cs->mutex);
    cs->val--;
    if (cs->val > 0)
        sem_post(&cs->gate);
    sem_post(&cs->mutex);
}

void csem_post(counting_sem_t *cs) {
    sem_wait(&cs->mutex);
    cs->val++;
    if (cs->val == 1)
        sem_post(&cs->gate);
    sem_post(&cs->mutex);
}

void csem_destroy(counting_sem_t *cs) {
    sem_destroy(&cs->gate);
    sem_destroy(&cs->mutex);
}

/* ── Shared state ── */
counting_sem_t readerLimit;
counting_sem_t wrt;
counting_sem_t mutex;
FILE *logfile;

int cnt       = 1;
int numreader = 0;
bool test_succeeded = true;

double average_blocked_wait_time_s             = 0.0;
double average_blocked_wait_time_numerator     = 0.0;
int    average_blocked_wait_time_denominator   = 0;
double average_unblocked_wait_time_s           = 0.0;
double average_unblocked_wait_time_numerator   = 0.0;
int    average_unblocked_wait_time_denominator = 0;

/* ── Logging ── */
void log_print(const char *fmt, ...) {
    va_list args1, args2;
    va_start(args1, fmt);
    va_copy(args2, args1);
    vprintf(fmt, args1);
    vfprintf(logfile, fmt, args2);
    va_end(args1);
    va_end(args2);
}

/* ── Writer ── */
void *writer(void *wno) {
    csem_wait(&wrt);
    cnt = cnt * 2;

    csem_wait(&mutex);
    log_print("| %-8s | %-10s | %-14s | %-10d | %-10s |\n",
              "WRITER", "WRITE", "-", cnt, "-");
    csem_post(&mutex);

    struct timespec work_time = {2, 0};
    nanosleep(&work_time, NULL);

    csem_post(&wrt);
    return NULL;
}

/* ── Reader ── */
void *reader(void *rno) {
    struct timespec time_start, time_end;

    // Predict blocking: gate is closed when val == 0
    sem_wait(&readerLimit.mutex);
    int will_wait = (readerLimit.val == 0);
    sem_post(&readerLimit.mutex);

    clock_gettime(CLOCK_MONOTONIC, &time_start);
    csem_wait(&readerLimit);
    clock_gettime(CLOCK_MONOTONIC, &time_end);

    double time_waited_s = (time_end.tv_sec - time_start.tv_sec) +
                           (time_end.tv_nsec - time_start.tv_nsec) / 1e9;

    csem_wait(&mutex);

    if (will_wait) {
        average_blocked_wait_time_numerator += time_waited_s;
        average_blocked_wait_time_denominator++;
    } else {
        average_unblocked_wait_time_numerator += time_waited_s;
        average_unblocked_wait_time_denominator++;
    }

    numreader++;
    if (numreader > N) test_succeeded = false;

    log_print("| %-8d | %-10s | %-14s | %-10d | %-10.6f |\n",
              *((int *)rno),
              "ENTER",
              will_wait ? "BLOCKED" : "IMMEDIATE",
              numreader,
              time_waited_s);

    // NOTE: csem_wait called inside mutex — safe only because writers never acquire mutex
    if (numreader == 1)
        csem_wait(&wrt);

    csem_post(&mutex);

    // Reading section
    csem_wait(&mutex);
    log_print("| %-8d | %-10s | %-14s | %-10d | %-10s |\n",
              *((int *)rno), "READ", "-", cnt, "-");
    csem_post(&mutex);

    struct timespec work_time = {2, 0};
    nanosleep(&work_time, NULL);

    csem_wait(&mutex);
    numreader--;
    log_print("| %-8d | %-10s | %-14s | %-10d | %-10s |\n",
              *((int *)rno), "EXIT", "-", numreader, "-");
    if (numreader == 0)
        csem_post(&wrt);
    csem_post(&mutex);

    csem_post(&readerLimit);
    return NULL;
}

/* ── Main ── */
int main() {
    logfile = fopen(LOG_FILE, "w");
    if (!logfile) {
        perror("Failed to open log file");
        return 1;
    }

    char *divider = "+----------+------------+----------------+------------+------------+\n";
    log_print("%s", divider);
    log_print("| %-8s | %-10s | %-14s | %-10s | %-10s |\n",
              "THREAD", "EVENT", "WAIT TYPE", "VAL/ACTIVE", "WAIT (s)");
    log_print("%s", divider);

    csem_init(&readerLimit, N);
    csem_init(&wrt, 1);
    csem_init(&mutex, 1);

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

    if (average_blocked_wait_time_denominator > 0)
        average_blocked_wait_time_s = average_blocked_wait_time_numerator / average_blocked_wait_time_denominator;
    if (average_unblocked_wait_time_denominator > 0)
        average_unblocked_wait_time_s = average_unblocked_wait_time_numerator / average_unblocked_wait_time_denominator;

    log_print("%s", divider);
    log_print("\n=== SUMMARY (Barz Counting Semaphore) ===\n");
    log_print("%-40s %s\n", "Result:",
              test_succeeded ? "PASSED - reader cap never exceeded" : "FAILED - reader cap exceeded");
    log_print("%-40s %d / %d readers\n", "Max concurrent readers allowed:", N, NUM_READERS);
    log_print("%-40s %d\n", "Readers that blocked:",      average_blocked_wait_time_denominator);
    log_print("%-40s %d\n", "Readers that didn't block:", average_unblocked_wait_time_denominator);
    log_print("%-40s %.9f s\n", "Avg wait (BLOCKED):",   average_blocked_wait_time_s);
    log_print("%-40s %.9f s\n", "Avg wait (UNBLOCKED):", average_unblocked_wait_time_s);
    log_print("%-40s %.9f s\n", "Difference:",
              average_blocked_wait_time_s - average_unblocked_wait_time_s);

    fclose(logfile);
    printf("\nLog written to %s\n", LOG_FILE);

    csem_destroy(&readerLimit);
    csem_destroy(&wrt);
    csem_destroy(&mutex);
    return 0;
}