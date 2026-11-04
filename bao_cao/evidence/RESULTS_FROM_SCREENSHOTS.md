# Observed results transcribed from user's terminal screenshots

These summaries were recreated from screenshots in the conversation; **they are not the original .txt logs from Ubuntu/QEMU**. The original logs remain on the user's Ubuntu computer.

## Memory checker: Valgrind 3.22.0 (x86-64 host mock)
- Command: `./app/sms_app_mock -d 5 -i 100 -v`
- Total heap usage: 18 allocs, 18 frees, 16,611 bytes allocated
- In use at exit: 0 bytes in 0 blocks
- All heap blocks were freed; no leaks are possible
- ERROR SUMMARY: 0 errors from 0 contexts

## GDB (host mock)
- `sensor_thread()` breakpoint reached at `app/sms_app_mock.c:84`; four worker threads observed.
- `send_alarm()` breakpoint reached at `app/sms_app_mock.c:181`
- HUMID_HIGH: 85 > 80; TEMP_HIGH: 42 > 40; GDB printed `$1 = 1` for the condition.

## perf stat (host mock; attached process for approximately 5 s)
- 9.91 msec task-clock
- 202 context-switches
- 12 cpu-migrations
- 0 page-faults
- ~5.00656 seconds elapsed

## perf record (host mock)
- cpu-clock event: 105 collected samples, 0 lost samples
- The displayed no-children report includes `finish_task_switch.isra.0` 8.57% Self overhead. This is an observed row, not proof of the global maximum.

## strace -f -c (host mock; 30 s)
| syscall | calls | % time |
|---|---:|---:|
| clock_nanosleep | 437 | 40.07 |
| mq_timedreceive | 477 | 37.01 |
| futex | 968 | 17.17 |
| mq_timedsend | 477 | 2.59 |
| write | 110 | 1.41 |

strace listed 31 syscall errors; these are not necessarily application failures.

## Jitter (QEMU vexpress-a9)

See `jitter_comparison_qemu.csv` for exact values. Policies were tested in the same QEMU environment, but with different time windows. Observed worker-thread SCHED_FIFO priorities 5, 10, 15, 20; main thread SCHED_OTHER.

Caveat: The sample sets may be affected by QEMU and the host machine. These observations do not establish deterministic latency or physical-hardware behavior.
