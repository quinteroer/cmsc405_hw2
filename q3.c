#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

#define LOG_FILE "reader_writer_q3.log"
#define SHM_DATA "/rw_data"
#define SHM_STATE "/rw_state"
#define DATA_LEN 256
#define WRITES_PER_WRITER 5

typedef struct {
    sem_t wrt;
    sem_t read_gate;
    sem_t mutex;
    sem_t log_mutex;
    int   numreader;
    int   writers_done;
    int   num_writers;
    int   num_readers;
    bool  terminate;
} shared_state_t;

static FILE *logfile = NULL;
static char *shm_data = NULL;
static shared_state_t *state = NULL;
static int num_readers = 0;
static int num_writers = 0;
static pthread_t *reader_tids = NULL;

static void log_print(const char *fmt, ...) {
    if (!logfile) return;
    va_list a1, a2;
    va_start(a1, fmt);
    va_copy(a2, a1);
    if (state) sem_wait(&state->log_mutex);
    vprintf(fmt, a1);
    vfprintf(logfile, fmt, a2);
    fflush(stdout);
    fflush(logfile);
    if (state) sem_post(&state->log_mutex);
    va_end(a1);
    va_end(a2);
}

static void log_divider(void) {
    log_print("+------------+------------+------------------------------+------------+\n");
}
static void log_header(void) {
    log_divider();
    log_print("| %-10s | %-10s | %-28s | %-10s |\n",
              "ENTITY","EVENT","DATA / NOTE","PID/TID");
    log_divider();
}
static void log_row(const char *entity, const char *event, const char *data, long id) {
    log_print("| %-10s | %-10s | %-28s | %-10ld |\n", entity, event, data, id);
}

static void shm_create(void) {
    shm_unlink(SHM_DATA);
    shm_unlink(SHM_STATE);

    int fd = shm_open(SHM_DATA, O_CREAT|O_RDWR, 0666);
    if (fd < 0) { perror("shm_open DATA"); exit(1); }
    ftruncate(fd, DATA_LEN);
    shm_data = mmap(NULL, DATA_LEN, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    strncpy(shm_data, "initial", DATA_LEN);

    fd = shm_open(SHM_STATE, O_CREAT|O_RDWR, 0666);
    if (fd < 0) { perror("shm_open STATE"); exit(1); }
    ftruncate(fd, sizeof(shared_state_t));
    state = mmap(NULL, sizeof(shared_state_t), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    sem_init(&state->wrt, 1, 1);
    sem_init(&state->read_gate, 1, 0);
    sem_init(&state->mutex, 1, 1);
    sem_init(&state->log_mutex, 1, 1);
    state->numreader = 0;
    state->writers_done = 0;
    state->num_writers = num_writers;
    state->num_readers = num_readers;
    state->terminate = false;
}

static void shm_open_existing(void) {
    int fd = shm_open(SHM_DATA, O_RDWR, 0666);
    if (fd < 0) { perror("shm_open DATA child"); exit(1); }
    shm_data = mmap(NULL, DATA_LEN, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    fd = shm_open(SHM_STATE, O_RDWR, 0666);
    if (fd < 0) { perror("shm_open STATE child"); exit(1); }
    state = mmap(NULL, sizeof(shared_state_t), PROT_READ|PROT_WRITE,MAP_SHARED, fd, 0);
    close(fd);
}

static void shm_destroy(void) {
    if (state) {
        sem_destroy(&state->wrt);
        sem_destroy(&state->read_gate);
        sem_destroy(&state->mutex);
        sem_destroy(&state->log_mutex);
        munmap(state, sizeof(shared_state_t));
        state = NULL;
    }
    if (shm_data) { munmap(shm_data, DATA_LEN); shm_data = NULL; }
    shm_unlink(SHM_DATA);
    shm_unlink(SHM_STATE);
}

static void sigint_handler(int sig) {
    (void)sig;
    if (state) {
        state->terminate = true;
        int nr = state->num_readers;
        for (int i = 0; i < nr; i++) {
            sem_post(&state->read_gate);
        }
    }
    if (logfile) {
        fprintf(logfile, "\n[SIGNAL] SIGINT — graceful shutdown initiated\n");
        fflush(logfile);
    }
    printf("\n[SIGNAL] SIGINT — graceful shutdown initiated\n");
    fflush(stdout);
}

typedef struct {
    int    id;
    double total_wait_s;
    int    read_count;
} reader_arg_t;

void *reader_thread(void *arg) {
    reader_arg_t *ra = (reader_arg_t *)arg;
    char note[96], tid_str[16];
    snprintf(tid_str, sizeof(tid_str), "R-%d", ra->id);

    while (1) {
        if (state->terminate) break;

        struct timespec t0, t1, deadline;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        clock_gettime(CLOCK_REALTIME,  &deadline);
        deadline.tv_sec += 1;

        int rc = sem_timedwait(&state->read_gate, &deadline);
        clock_gettime(CLOCK_MONOTONIC, &t1);

        if (rc != 0) continue;
        if (state->terminate) break;

        double waited = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        ra->total_wait_s += waited;

        sem_wait(&state->mutex);
        state->numreader++;
        if (state->numreader == 1) {
            sem_post(&state->mutex);
            sem_wait(&state->wrt);
            sem_wait(&state->mutex);
        }
        sem_post(&state->mutex);

        snprintf(note, sizeof(note), "\"%.20s\" gate=%.4fs", shm_data, waited);
        log_row(tid_str, "READ", note, (long)ra->id);
        ra->read_count++;

        struct timespec work = {0, 2000000};
        nanosleep(&work, NULL);

        sem_wait(&state->mutex);
        state->numreader--;
        if (state->numreader == 0) sem_post(&state->wrt);
        sem_post(&state->mutex);
    }

    snprintf(note, sizeof(note), "total reads=%d", ra->read_count);
    log_row(tid_str, "EXITED", note, (long)ra->id);
    return NULL;
}

static void writer_process(int id) {
    logfile = fopen(LOG_FILE, "a");
    shm_open_existing();

    char data[DATA_LEN], note[DATA_LEN+8], wid_str[16];
    snprintf(wid_str, sizeof(wid_str), "W-%d", id);
    struct timespec gap = {0, 500000000};

    for (int i = 1; i <= WRITES_PER_WRITER && !state->terminate; i++) {
        sem_wait(&state->wrt);

        snprintf(data, DATA_LEN, "w%d_write%d", id, i);
        strncpy(shm_data, data, DATA_LEN);
        snprintf(note, sizeof(note), "\"%.26s\"", data);
        log_row(wid_str, "WROTE", note, (long)getpid());

        sem_post(&state->wrt);
        sem_post(&state->read_gate);  /* wake one reader per write (part d) */
        nanosleep(&gap, NULL);
    }

        char done_note[32];
        snprintf(done_note, sizeof(done_note), "exited after %d writes", i-1);
        log_row(wid_str, "DONE", state->terminate ? done_note : "all writes complete", (long)getpid());

    /* Termination detection (part f) — release mutex BEFORE posting
     * read_gate to avoid deadlock with readers waiting on mutex */
    sem_wait(&state->mutex);
    state->writers_done++;
    int all_done = (state->writers_done >= state->num_writers);
    int nr       = state->num_readers;
    sem_post(&state->mutex);

    if (all_done) {
        state->terminate = true;
        for (int r = 0; r < nr; r++)
            sem_post(&state->read_gate);
        sem_wait(&state->log_mutex);
        fprintf(logfile,
                "\n[INFO] All %d writers done — readers terminating\n",
                state->num_writers);
        printf("\n[INFO] All %d writers done — readers terminating\n",
               state->num_writers);
        fflush(logfile);
        sem_post(&state->log_mutex);
    }

    if (logfile) fclose(logfile);
    munmap(shm_data, DATA_LEN);
    munmap(state, sizeof(shared_state_t));
    exit(0);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <num_readers> <num_writers>\n", argv[0]);
        return 1;
    }
    num_readers = atoi(argv[1]);
    num_writers = atoi(argv[2]);
    if (num_readers <= 0 || num_writers <= 0) {
        fprintf(stderr, "Error: both arguments must be > 0\n");
        return 1;
    }

    logfile = fopen(LOG_FILE, "w");
    if (!logfile) { perror("fopen log"); return 1; }

    shm_create();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    log_print("\n=== Q3: Multi-Process / Multi-Thread Reader-Writer ===\n");
    log_print("    Readers (threads): %d    Writers (processes): %d"
              "    Writes per writer: %d\n\n",
              num_readers, num_writers, WRITES_PER_WRITER);
    log_header();

    reader_tids         = malloc(num_readers * sizeof(pthread_t));
    reader_arg_t *rargs = malloc(num_readers * sizeof(reader_arg_t));
    for (int i = 0; i < num_readers; i++) {
        rargs[i] = (reader_arg_t){.id=i+1, .total_wait_s=0.0, .read_count=0};
        pthread_create(&reader_tids[i], NULL, reader_thread, &rargs[i]);
    }

    pid_t *wpids = malloc(num_writers * sizeof(pid_t));
    for (int i = 0; i < num_writers; i++) {
        wpids[i] = fork();
        if (wpids[i] < 0) {
            perror("fork");
            state->terminate = true;
            for (int r = 0; r < num_readers; r++)
                sem_post(&state->read_gate);
            break;
        }
        if (wpids[i] == 0) {
            free(reader_tids); free(rargs); free(wpids);
            writer_process(i + 1);
        }
    }

    for (int i = 0; i < num_writers; i++)
        if (wpids[i] > 0) waitpid(wpids[i], NULL, 0);

    for (int i = 0; i < num_readers; i++)
        pthread_join(reader_tids[i], NULL);

    log_divider();
    log_print("\n=== SUMMARY ===\n");
    log_print("%-38s \"%s\"\n", "Final shared string:", shm_data);
    log_print("%-38s %s\n", "Termination status:",
              state->terminate ? "CLEAN" : "INCOMPLETE");

    double total_wait = 0.0;
    int    total_reads = 0;
    for (int i = 0; i < num_readers; i++) {
        total_wait  += rargs[i].total_wait_s;
        total_reads += rargs[i].read_count;
        log_print("  Reader %-3d  reads=%-4d  avg gate wait=%.6f s\n",
                  rargs[i].id, rargs[i].read_count,
                  rargs[i].read_count > 0
                      ? rargs[i].total_wait_s / rargs[i].read_count : 0.0);
    }
    log_print("%-38s %d\n",   "Total reads:", total_reads);
    log_print("%-38s %.6f s\n","Overall avg gate wait:",
              total_reads > 0 ? total_wait / total_reads : 0.0);

    shm_destroy();
    free(reader_tids); free(rargs); free(wpids);
    fclose(logfile);
    printf("\nLog written to %s\n", LOG_FILE);
    return 0;
}