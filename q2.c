#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>

#define N 5
#define NUM_READERS 20
#define NUM_WRITERS 10

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
    sem_t gate;    // open if val > 0, closed if val == 0
    sem_t mutex;   // protects val
} counting_sem_t;

void csem_init(counting_sem_t *cs, int k) {
    cs->val = k;
    sem_init(&cs->gate,  0, k > 0 ? 1 : 0);  // min(1, k)
    sem_init(&cs->mutex, 0, 1);
}

void csem_wait(counting_sem_t *cs) {      // Pc
    sem_wait(&cs->gate);                  // block before touching val
    sem_wait(&cs->mutex);
    cs->val--;
    if (cs->val > 0)
        sem_post(&cs->gate);              // more slots remain, reopen
    sem_post(&cs->mutex);
}

void csem_post(counting_sem_t *cs) {      // Vc
    sem_wait(&cs->mutex);
    cs->val++;
    if (cs->val == 1)
        sem_post(&cs->gate);              // just went 0→1, open gate
    sem_post(&cs->mutex);
}

void csem_destroy(counting_sem_t *cs) {
    sem_destroy(&cs->gate);
    sem_destroy(&cs->mutex);
}

/* ── Shared state ── */
counting_sem_t readerLimit;
counting_sem_t wrt;
pthread_mutex_t mutex;

int cnt       = 1;
int numreader = 0;
bool test_succeeded = true;

double average_blocked_wait_time_numerator     = 0.0;
int    average_blocked_wait_time_denominator   = 0;
double average_unblocked_wait_time_numerator   = 0.0;
int    average_unblocked_wait_time_denominator = 0;

/* ── Writer ── */
void *writer(void *wno) {
    csem_wait(&wrt);
    cnt = cnt * 2;
    printf("Writer %d modified cnt to %d\n", *((int *)wno), cnt);
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

    double time_waited_ns = (time_end.tv_sec - time_start.tv_sec) * 1e9 +
                            (time_end.tv_nsec - time_start.tv_nsec);

    pthread_mutex_lock(&mutex);

    if (will_wait) {
        printf("Reader %d BLOCKED and waited %.2f ns.\n", *((int *)rno), time_waited_ns);
        average_blocked_wait_time_numerator += time_waited_ns;
        average_blocked_wait_time_denominator++;
    } else {
        printf("Reader %d acquired immediately (%.2f ns overhead).\n", *((int *)rno), time_waited_ns);
        average_unblocked_wait_time_numerator += time_waited_ns;
        average_unblocked_wait_time_denominator++;
    }

    numreader++;
    if (numreader > N) test_succeeded = false;
    printf("  Reader %d ENTERED (active readers: %d)\n", *((int *)rno), numreader);

    // NOTE: csem_wait called inside mutex — safe only because writers never acquire mutex
    if (numreader == 1)
        csem_wait(&wrt);

    pthread_mutex_unlock(&mutex);

    printf("Reader %d: read cnt as %d\n", *((int *)rno), cnt);
    struct timespec sleep_time = {0, 5000000};  // 5ms simulated read
    nanosleep(&sleep_time, NULL);

    pthread_mutex_lock(&mutex);
    numreader--;
    if (numreader == 0)
        csem_post(&wrt);
    pthread_mutex_unlock(&mutex);

    csem_post(&readerLimit);
    return NULL;
}

/* ── Main ── */
int main() {
    csem_init(&readerLimit, N);
    csem_init(&wrt, 1);
    pthread_mutex_init(&mutex, NULL);

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

    printf("\n%s: No more than %d readers active simultaneously.\n",
           test_succeeded ? "Test PASSED" : "Test FAILED", N);

    double avg_blocked   = average_blocked_wait_time_denominator > 0
                         ? average_blocked_wait_time_numerator / average_blocked_wait_time_denominator : 0.0;
    double avg_unblocked = average_unblocked_wait_time_denominator > 0
                         ? average_unblocked_wait_time_numerator / average_unblocked_wait_time_denominator : 0.0;

    printf("Average wait — BLOCKED:   %.2f ns  (n=%d)\n", avg_blocked,   average_blocked_wait_time_denominator);
    printf("Average wait — UNBLOCKED: %.2f ns  (n=%d)\n", avg_unblocked, average_unblocked_wait_time_denominator);
    printf("BLOCKED readers waited %.2f ns longer on average.\n", avg_blocked - avg_unblocked);

    csem_destroy(&readerLimit);
    csem_destroy(&wrt);
    pthread_mutex_destroy(&mutex);
    return 0;
}