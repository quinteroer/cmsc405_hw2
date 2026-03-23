#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>

#define N 5 // Maximum number of readers that can read at the same time
#define NUM_READERS 20
#define NUM_WRITERS 10

sem_t readerLimit; // Semaphore to limit the number of readers that can read at the same time
sem_t wrt;
pthread_mutex_t mutex;
int cnt = 1;
int numreader = 0;
bool test_succeeded = true;

double average_blocked_wait_time_ns = 0.0;
double average_blocked_wait_time_numerator = 0.0;
int average_blocked_wait_time_denominator = 0;

double average_unblocked_wait_time_ns = 0.0;
double average_unblocked_wait_time_numerator = 0.0;
int average_unblocked_wait_time_denominator = 0;


void *writer(void *wno)
{   
    sem_wait(&wrt);
    cnt = cnt*2;
    printf("Writer %d modified cnt to %d\n",(*((int *)wno)),cnt);
    sem_post(&wrt);
    return NULL;
}
void *reader(void *rno)
{   
    int sval;
    struct timespec time_start, time_end;

    sem_getvalue(&readerLimit, &sval);
    int will_wait = (sval == 0);

    clock_gettime(CLOCK_MONOTONIC, &time_start); // Start the timer to measure how long the reader waits
    sem_wait(&readerLimit); // Wait if there are already N readers reading
    clock_gettime(CLOCK_MONOTONIC, &time_end); // End the timer after acquiring the readerLimit semaphore

    double time_waited_ns = (time_end.tv_sec - time_start.tv_sec) * 1e9 + 
                            (time_end.tv_nsec - time_start.tv_nsec);

    pthread_mutex_lock(&mutex);

    // calculate metrics for average wait time for blocked and unblocked readers
    if (will_wait) {
        printf("Reader %d BLOCKED and waited %.2f ns.\n", *((int *)rno), time_waited_ns);
        average_blocked_wait_time_numerator += time_waited_ns;
        average_blocked_wait_time_denominator++;
    } else {
        printf("Reader %d acquired immediately (%.2f ns overhead).\n", *((int *)rno), time_waited_ns);
        average_unblocked_wait_time_numerator += time_waited_ns;
        average_unblocked_wait_time_denominator++;
    }

    // Get the current value of readerLimit to calculate the number of active readers
    sem_getvalue(&readerLimit, &sval);
    printf("  Reader %d ENTERED (Slots active: %d)\n", *((int *)rno), N - sval);

    // Check if the number of active readers exceeds N, which should not happen
    numreader++;
    if (numreader > N) {
        test_succeeded = false;
    }
    if(numreader == 1) {
        sem_wait(&wrt); // If this is the first reader, then it will block the writer
    }

    pthread_mutex_unlock(&mutex);

    // Reading Section
    printf("Reader %d: read cnt as %d\n",*((int *)rno),cnt);

    // Simulate reading time by sleeping for a short duration
    struct timespec sleep_time = {0, 5000000}; // 5 milliseconds
    nanosleep(&sleep_time, NULL);

    // Reader acquire the lock before modifying numreader
    pthread_mutex_lock(&mutex);
    numreader--;
    if(numreader == 0) {
        sem_post(&wrt); // If this is the last reader, it will wake up the writer.
    }
    
    pthread_mutex_unlock(&mutex);

    sem_post(&readerLimit); // Signal that this reader is done, allowing another reader to read
    return NULL;
}

int main()
{   
    sem_init(&readerLimit,0,N);
    pthread_t read[NUM_READERS],write[NUM_WRITERS];
    pthread_mutex_init(&mutex, NULL);
    sem_init(&wrt,0,1);

    int a[NUM_READERS]; //Just used for numbering the producer and consumer
    int b[NUM_WRITERS];

    for(int i = 0; i < NUM_READERS; i++) {
        a[i] = i + 1;
    }
    for(int i = 0; i < NUM_WRITERS; i++) {
        b[i] = i + 1;
    }

    for(int i = 0; i < NUM_READERS; i++) {
        pthread_create(&read[i], NULL, (void *)reader, (void *)&a[i]);
    }

    for(int i = 0; i < NUM_WRITERS; i++) {
        pthread_create(&write[i], NULL, (void *)writer, (void *)&b[i]);
    }

    for(int i = 0; i < NUM_READERS; i++) {
        pthread_join(read[i], NULL);
    }

    for(int i = 0; i < NUM_WRITERS; i++) {
        pthread_join(write[i], NULL);
    }

    if (test_succeeded) {
        printf("Test succeeded: No more than %d readers were active at the same time.\n", N);
    } else {
        printf("Test failed: More than %d readers were active at the same time.\n", N);
    }

    if (average_blocked_wait_time_denominator > 0) {
        average_blocked_wait_time_ns = average_blocked_wait_time_numerator / average_blocked_wait_time_denominator;
    }

    if (average_unblocked_wait_time_denominator > 0) {
        average_unblocked_wait_time_ns = average_unblocked_wait_time_numerator / average_unblocked_wait_time_denominator;
    }

    printf("Average wait time for BLOCKED readers: %.2f ns\n", average_blocked_wait_time_ns);
    printf("Average wait time for UNBLOCKED readers: %.2f ns\n", average_unblocked_wait_time_ns);

    printf("BLOCKED readers waited on average: %.2f ns longer than UNBLOCKED readers on average.\n", 
           (average_blocked_wait_time_ns - average_unblocked_wait_time_ns));

    pthread_mutex_destroy(&mutex);
    sem_destroy(&wrt);
    sem_destroy(&readerLimit); // Clean up the semaphore and mutex resources
    return 0;
}