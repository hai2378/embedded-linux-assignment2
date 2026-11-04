#define _POSIX_C_SOURCE 200809L
#include <sched.h>
#include <sys/mman.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/ioctl.h>
#include "../driver/sms_sensor.h"
#include <mqueue.h>

#define DEVICE_PATH "/dev/sms_sensor"
#define BUFFER_SIZE 64
#define LOG_FILE "/tmp/sms.log"
#define LOG_MSG_SIZE 256

typedef struct {
    unsigned long long timestamp;
    double temperature;
    double humidity;
} SensorData;

static SensorData *buffer;

static int head = 0;
static int tail = 0;
static int count = 0;

static pthread_mutex_t buffer_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t buffer_cond =
    PTHREAD_COND_INITIALIZER;

static atomic_bool running = ATOMIC_VAR_INIT(1);
static int sampling_interval_ms = 500;
static int verbose_mode = 0;

static unsigned long total_samples = 0;
static unsigned long dropped_samples = 0;
typedef struct {
    unsigned long long timestamp;
    char type[24];
    double value;
    double threshold;
} AlarmMessage;

static mqd_t alarm_mq = (mqd_t)-1;
static mqd_t log_mq = (mqd_t)-1;

static double temp_threshold = 40.0;
static double hum_threshold = 80.0;

static unsigned long temp_high_alerts = 0;
static unsigned long hum_high_alerts = 0;
static unsigned long temp_spike_alerts = 0;

/* TASK 4: measured wake-up delay (jitter) in microseconds.
   Only Sensor Thread writes samples, main reads them after pthread_join. */
#define JITTER_CAP 20000
static long jitter_samples[JITTER_CAP];
static size_t jitter_count = 0;
static int use_fifo = 1; /* -p fifo (default), -p other for comparison */
static int memory_locked = 0;

/* If memory-lock limits prevent pthread stack creation, unlock and retry. */
static int start_thread(pthread_t *id, void *(*worker)(void *))
{
    int err = pthread_create(id, NULL, worker, NULL);
    if (err == EAGAIN && memory_locked) {
        if (munlockall() == 0) {
            memory_locked = 0;
            fprintf(stderr, "[RT] memory lock limit blocked thread; unlocked and retrying\n");
            err = pthread_create(id, NULL, worker, NULL);
        }
    }
    return err;
}

static void add_interval(struct timespec *ts, long ms)
{
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static long diff_us(const struct timespec *actual,
                    const struct timespec *expected)
{
    long long ns = (long long)(actual->tv_sec - expected->tv_sec) * 1000000000LL
                 + (long long)(actual->tv_nsec - expected->tv_nsec);
    return (long)(ns / 1000LL);
}

static int compare_long(const void *a, const void *b)
{
    const long x = *(const long *)a;
    const long y = *(const long *)b;
    return (x > y) - (x < y);
}

static void print_jitter_report(void)
{
    size_t bins[4] = {0, 0, 0, 0};
    long min, max;
    long long sum = 0;
    size_t n = jitter_count;

    if (n == 0) {
        puts("JITTER_RESULT no samples");
        return;
    }

    for (size_t i = 0; i < n; ++i) {
        long j = jitter_samples[i];
        sum += j;
        if (j < 100) bins[0]++;
        else if (j < 500) bins[1]++;
        else if (j < 1000) bins[2]++;
        else bins[3]++;
    }

    qsort(jitter_samples, n, sizeof(jitter_samples[0]), compare_long);
    min = jitter_samples[0];
    max = jitter_samples[n-1];
    /* Nearest-rank P99: rank ceil(0.99*n), zero-indexed. */
    size_t p99_index = (99*n + 99)/100 - 1;
    printf("JITTER_RESULT policy=%s samples=%zu min_us=%ld avg_us=%.2f max_us=%ld p99_us=%ld\n",
           use_fifo ? "SCHED_FIFO" : "SCHED_OTHER", n,
           min, (double)sum/n, max, jitter_samples[p99_index]);
    printf("JITTER_HIST under_100us=%zu 100_499us=%zu 500_999us=%zu at_least_1000us=%zu\n",
           bins[0], bins[1], bins[2], bins[3]);
    if (n < 1000)
        fprintf(stderr, "JITTER_WARNING: only %zu samples; at least 1000 required by assignment\n", n);
    if (n == JITTER_CAP)
        fprintf(stderr, "JITTER_WARNING: sample buffer full, extra samples ignored\n");
}

/* Sleep for a number of milliseconds */
static void sleep_ms(long ms)
{
    struct timespec req;

    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;

    while (nanosleep(&req, &req) == -1 &&
           errno == EINTR) {
        if (!atomic_load(&running))
            break;
    }
}

/* THREAD 1: Read sensor data */
static void *sensor_thread(void *arg)
{
    char data[128];
    SensorData sample;
    int fd;
    ssize_t bytes;
    int read_error;

    struct timespec expected, actual;
    int sleep_result;

    (void)arg;

    printf("[Sensor] Thread started\n");

    if (clock_gettime(CLOCK_MONOTONIC, &expected) != 0) {
        perror("clock_gettime");
        return NULL;
    }

    while (atomic_load(&running)) {
        /* Absolute periodic deadline avoids accumulated relative-sleep drift. */
        add_interval(&expected, sampling_interval_ms);
        do {
            sleep_result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                           &expected, NULL);
        } while (sleep_result == EINTR && atomic_load(&running));
        if (!atomic_load(&running)) break;
        if (sleep_result != 0) {
            fprintf(stderr, "[Sensor] clock_nanosleep: %s\n", strerror(sleep_result));
            break;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &actual) == 0 &&
            jitter_count < JITTER_CAP) {
            jitter_samples[jitter_count++] = diff_us(&actual, &expected);
        }

        fd = open(DEVICE_PATH, O_RDONLY);

        if (fd < 0) {
            perror("[Sensor] open");
            sleep_ms(500);
            continue;
        }

        bytes = read(fd, data, sizeof(data) - 1);
        read_error = errno;

        close(fd);

        if (bytes > 0) {

            data[bytes] = '\0';

            if (sscanf(data, "%llu,%lf,%lf",
                       &sample.timestamp,
                       &sample.temperature,
                       &sample.humidity) == 3) {

                pthread_mutex_lock(&buffer_mutex);

                if (count < BUFFER_SIZE) {

                    buffer[head] = sample;
                    head = (head + 1) % BUFFER_SIZE;
                    count++;

                    total_samples++;

                    pthread_cond_signal(&buffer_cond);

                } else {
                    dropped_samples++;
                }

                pthread_mutex_unlock(&buffer_mutex);

            } else {
                fprintf(stderr,
                        "[Sensor] Invalid data\n");
            }

        } else if (bytes < 0 &&
                   read_error != EAGAIN &&
                   read_error != EINTR) {

            errno = read_error;
            perror("[Sensor] read");
        }

        /* Next read is scheduled at the next absolute deadline. */
    }

    printf("[Sensor] Thread stopped\n");

    return NULL;
}


/* Send alarm to Alert Thread */
static void send_alarm(unsigned long long timestamp,
                       const char *type,
                       double value,
                       double threshold)
{
    AlarmMessage alarm = {0};

    alarm.timestamp = timestamp;
    snprintf(alarm.type, sizeof(alarm.type), "%s", type);
    alarm.value = value;
    alarm.threshold = threshold;

    if (mq_send(alarm_mq, (const char *)&alarm,
                sizeof(alarm), 0) == -1) {
        perror("[Processor] send alarm");
    }
}

/* THREAD 2: Process sensor data */
static void *processor_thread(void *arg)
{
    SensorData sample;
    double temp_window[10] = {0};
    double hum_window[10] = {0};
    double sum_temp = 0;
    double sum_hum = 0;
    double previous_temp = 0;
    double avg_temp, avg_hum, delta;
    int window_index = 0;
    int window_count = 0;
    int has_previous = 0;
    char message[LOG_MSG_SIZE];

    (void)arg;

    printf("[Processor] Thread started\n");

    while (1) {
        pthread_mutex_lock(&buffer_mutex);

        while (count == 0 && atomic_load(&running)) {
            pthread_cond_wait(&buffer_cond, &buffer_mutex);
        }

        if (count == 0 && !atomic_load(&running)) {
            pthread_mutex_unlock(&buffer_mutex);
            break;
        }

        sample = buffer[tail];
        tail = (tail + 1) % BUFFER_SIZE;
        count--;

        pthread_mutex_unlock(&buffer_mutex);

        /* Rolling average of 10 samples */
        if (window_count == 10) {
            sum_temp -= temp_window[window_index];
            sum_hum -= hum_window[window_index];
        } else {
            window_count++;
        }

        temp_window[window_index] = sample.temperature;
        hum_window[window_index] = sample.humidity;

        sum_temp += sample.temperature;
        sum_hum += sample.humidity;

        window_index = (window_index + 1) % 10;

        avg_temp = sum_temp / window_count;
        avg_hum = sum_hum / window_count;

        /* Temperature alert */
        if (sample.temperature > temp_threshold) {
            send_alarm(sample.timestamp, "TEMP_HIGH",
                       sample.temperature, temp_threshold);
        }

        /* Humidity alert */
        if (sample.humidity > hum_threshold) {
            send_alarm(sample.timestamp, "HUMID_HIGH",
                       sample.humidity, hum_threshold);
        }

        /* Temperature spike alert */
        if (has_previous) {
            delta = sample.temperature - previous_temp;

            if (delta < 0)
                delta = -delta;

            if (delta > 5.0) {
                send_alarm(sample.timestamp, "TEMP_SPIKE",
                           delta, 5.0);
            }
        }

        previous_temp = sample.temperature;
        has_previous = 1;

        if (verbose_mode)
            printf("[Processor] Temp: %.2f C | Hum: %.2f %%\n",
               sample.temperature, sample.humidity);

        if (verbose_mode)
            printf("[Processor] Avg10 Temp: %.2f | Hum: %.2f\n",
               avg_temp, avg_hum);

        /* Send sample to Logger Thread */
        snprintf(message, sizeof(message),
                 "SAMPLE ts=%llu temp=%.3f hum=%.3f "
                 "avg_temp=%.3f avg_hum=%.3f",
                 sample.timestamp,
                 sample.temperature,
                 sample.humidity,
                 avg_temp,
                 avg_hum);

        if (mq_send(log_mq, message,
                    strlen(message) + 1, 0) == -1) {
            perror("[Processor] send log");
        }

        fflush(stdout);
    }

    printf("[Processor] Thread stopped\n");
    return NULL;
}

/* THREAD 3: Handle sensor alerts */
static void *alert_thread(void *arg)
{
    AlarmMessage alarm;
    char message[LOG_MSG_SIZE];
    ssize_t n;

    (void)arg;

    printf("[Alert] Thread started\n");

    while (1) {
        n = mq_receive(alarm_mq,
                       (char *)&alarm,
                       sizeof(alarm),
                       NULL);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            perror("[Alert] mq_receive");
            break;
        }

        if (strcmp(alarm.type, "STOP") == 0)
            break;

        fprintf(stderr,
                "[ALERT][%llu] TYPE: %s "
                "VALUE: %.3f THRESHOLD: %.3f\n",
                alarm.timestamp,
                alarm.type,
                alarm.value,
                alarm.threshold);

        if (strcmp(alarm.type, "TEMP_HIGH") == 0)
            temp_high_alerts++;

        else if (strcmp(alarm.type, "HUMID_HIGH") == 0)
            hum_high_alerts++;

        else if (strcmp(alarm.type, "TEMP_SPIKE") == 0)
            temp_spike_alerts++;

        snprintf(message, sizeof(message),
                 "[ALERT][%llu] %s %.3f threshold %.3f",
                 alarm.timestamp,
                 alarm.type,
                 alarm.value,
                 alarm.threshold);

        if (mq_send(log_mq, message,
                    strlen(message) + 1, 0) < 0) {
            perror("[Alert] mq_send");
        }
    }

    printf("[Alert] Thread stopped\n");
    return NULL;
}

/* THREAD 4: Write system logs */
static void *logger_thread(void *arg)
{
    FILE *fp;
    char message[LOG_MSG_SIZE];
    char timestamp[64];
    struct tm tm_now;
    time_t now;
    time_t last_flush;
    ssize_t n;

    (void)arg;

    printf("[Logger] Thread started\n");

    fp = fopen(LOG_FILE, "a");

    if (fp == NULL) {
        perror("[Logger] fopen");
        return NULL;
    }

    last_flush = time(NULL);

    while (1) {
        n = mq_receive(log_mq,
                       message,
                       sizeof(message),
                       NULL);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            perror("[Logger] mq_receive");
            break;
        }

        if (n >= LOG_MSG_SIZE)
            n = LOG_MSG_SIZE - 1;

        message[n] = '\0';

        if (strcmp(message, "STOP") == 0)
            break;

        now = time(NULL);
        localtime_r(&now, &tm_now);

        strftime(timestamp, sizeof(timestamp),
                 "%Y-%m-%d %H:%M:%S",
                 &tm_now);

        fprintf(fp, "[%s] %s\n",
                timestamp, message);

        if (difftime(now, last_flush) >= 5) {
            fflush(fp);
            last_flush = now;
        }

        if (ftell(fp) > 1024 * 1024) {
            fflush(fp);
            fclose(fp);

            rename(LOG_FILE, "/tmp/sms.log.1");

            fp = fopen(LOG_FILE, "a");

            if (fp == NULL) {
                perror("[Logger] rotate");
                return NULL;
            }
        }
    }

    fflush(fp);
    fclose(fp);

    printf("[Logger] Thread stopped\n");
    return NULL;
}
/* Handle Ctrl+C */
static volatile sig_atomic_t stop_requested = 0;

static void stop_handler(int sig)
{
    (void)sig;
    stop_requested = 1;
}

/* MAIN PROGRAM */
int main(int argc, char *argv[])
{
    pthread_t sensor_id;
    pthread_t processor_id;
    pthread_t alert_id;
    pthread_t logger_id;

    int duration = 60;
    int opt;
    int fd;
    int err;
    int result = 0;

    int sensor_started = 0;
    int processor_started = 0;
    int alert_started = 0;
    int logger_started = 0;

    char alarm_name[64];
    char log_name[64];

    struct mq_attr alarm_attr = {0};
    struct mq_attr log_attr = {0};

    AlarmMessage stop_alarm = {0};

    time_t start_time;

    /* Command line arguments */
    while ((opt = getopt(argc, argv,
                         "i:t:h:d:vp:")) != -1) {
        switch (opt) {
        case 'i':
            sampling_interval_ms = atoi(optarg);
            break;

        case 't':
            temp_threshold = atof(optarg);
            break;

        case 'h':
            hum_threshold = atof(optarg);
            break;

        case 'd':
            duration = atoi(optarg);
            break;

        case 'v':
            verbose_mode = 1;
            break;
        case 'p':
            if (strcmp(optarg, "fifo") == 0) use_fifo = 1;
            else if (strcmp(optarg, "other") == 0) use_fifo = 0;
            else {
                fprintf(stderr, "Use -p fifo or -p other\n");
                return 1;
            }
            break;

        default:
            fprintf(stderr, "Invalid arguments\n");
            return 1;
        }
    }

    if (sampling_interval_ms < 10 ||
        sampling_interval_ms > 60000 ||
        duration < 0) {
        fprintf(stderr, "Invalid configuration\n");
        return 1;
    }

    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);


    /* Allocate shared buffer */
    buffer = calloc(BUFFER_SIZE, sizeof(SensorData));

    if (buffer == NULL) {
        perror("calloc");
        return 1;
    }

    /* Configure sensor driver */
    fd = open(DEVICE_PATH, O_RDONLY);

    if (fd < 0) {
        perror("open sensor");
        result = 1;
        goto cleanup;
    }

    if (ioctl(fd, SMS_SET_INTERVAL,
              &sampling_interval_ms) < 0) {
        perror("ioctl");
        close(fd);
        result = 1;
        goto cleanup;
    }

    close(fd);

    /* Check log file */
    FILE *fp = fopen(LOG_FILE, "a");

    if (fp == NULL) {
        perror("open log");
        result = 1;
        goto cleanup;
    }

    fclose(fp);

    /* Create message queues */
    snprintf(alarm_name, sizeof(alarm_name),
             "/sms_alarm_%ld", (long)getpid());

    snprintf(log_name, sizeof(log_name),
             "/sms_log_%ld", (long)getpid());

    alarm_attr.mq_maxmsg = 10;
    alarm_attr.mq_msgsize = sizeof(AlarmMessage);

    log_attr.mq_maxmsg = 10;
    log_attr.mq_msgsize = LOG_MSG_SIZE;

    alarm_mq = mq_open(alarm_name,
                       O_CREAT | O_EXCL | O_RDWR,
                       0600, &alarm_attr);

    if (alarm_mq == (mqd_t)-1) {
        perror("mq_open alarm");
        result = 1;
        goto cleanup;
    }

    log_mq = mq_open(log_name,
                     O_CREAT | O_EXCL | O_RDWR,
                     0600, &log_attr);

    if (log_mq == (mqd_t)-1) {
        perror("mq_open log");
        result = 1;
        goto cleanup;
    }

    /* TASK 4: lock current/future memory to reduce page-fault latency.
       This might fail without CAP_IPC_LOCK or sufficient RLIMIT_MEMLOCK. */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "[RT] mlockall: %s (continuing without lock)\n", strerror(errno));
    } else {
        memory_locked = 1;
    }

    printf("=== SENSOR MONITORING SYSTEM ===\n");

    /* Start Logger Thread */
    err = start_thread(&logger_id, logger_thread);

    if (err != 0) {
        fprintf(stderr, "Logger: %s\n", strerror(err));
        result = 1;
        goto shutdown;
    }

    logger_started = 1;

    /* Start Alert Thread */
    err = start_thread(&alert_id, alert_thread);

    if (err != 0) {
        fprintf(stderr, "Alert: %s\n", strerror(err));
        result = 1;
        goto shutdown;
    }

    alert_started = 1;

    /* Start Processor Thread */
    err = start_thread(&processor_id, processor_thread);

    if (err != 0) {
        fprintf(stderr, "Processor: %s\n", strerror(err));
        result = 1;
        goto shutdown;
    }

    processor_started = 1;

    /* Start Sensor Thread */
    err = start_thread(&sensor_id, sensor_thread);

    if (err != 0) {
        fprintf(stderr, "Sensor: %s\n", strerror(err));
        result = 1;
        goto shutdown;
    }

    sensor_started = 1;

    printf("All 4 threads started\n");
    /* Configure SCHED_FIFO priorities; -p other leaves SCHED_OTHER inherited. */
    if (use_fifo) {
        struct sched_param param;

    param.sched_priority = 20;
    err = pthread_setschedparam(sensor_id, SCHED_FIFO, &param);
    if (err != 0)
        fprintf(stderr, "Sensor SCHED_FIFO: %s\n", strerror(err));

    param.sched_priority = 15;
    err = pthread_setschedparam(processor_id, SCHED_FIFO, &param);
    if (err != 0)
        fprintf(stderr, "Processor SCHED_FIFO: %s\n", strerror(err));

    param.sched_priority = 10;
    err = pthread_setschedparam(alert_id, SCHED_FIFO, &param);
    if (err != 0)
        fprintf(stderr, "Alert SCHED_FIFO: %s\n", strerror(err));

    param.sched_priority = 5;
    err = pthread_setschedparam(logger_id, SCHED_FIFO, &param);
    if (err != 0)
        fprintf(stderr, "Logger SCHED_FIFO: %s\n", strerror(err));
}
    start_time = time(NULL);

    /* Wait until duration ends or Ctrl+C */
    while (!stop_requested) {
        if (duration > 0 &&
            time(NULL) - start_time >= duration)
            break;

        sleep_ms(200);
    }

shutdown:
    /* Stop producing sensor data */
    atomic_store(&running, 0);

    if (sensor_started)
        pthread_join(sensor_id, NULL);

    print_jitter_report();

    /* Wake Processor and finish pending data */
    pthread_mutex_lock(&buffer_mutex);
    pthread_cond_broadcast(&buffer_cond);
    pthread_mutex_unlock(&buffer_mutex);

    if (processor_started)
        pthread_join(processor_id, NULL);

    /* Stop Alert after Processor */
    if (alert_started) {
        strcpy(stop_alarm.type, "STOP");
        mq_send(alarm_mq, (char *)&stop_alarm,
                sizeof(stop_alarm), 0);

        pthread_join(alert_id, NULL);
    }

    /* Stop Logger last */
    if (logger_started) {
        mq_send(log_mq, "STOP", 5, 0);
        pthread_join(logger_id, NULL);
    }

cleanup:
    if (alarm_mq != (mqd_t)-1) {
        mq_close(alarm_mq);
        mq_unlink(alarm_name);
    }

    if (log_mq != (mqd_t)-1) {
        mq_close(log_mq);
        mq_unlink(log_name);
    }

    free(buffer);

    printf("=== SYSTEM STOPPED ===\n");

    return result;
}
