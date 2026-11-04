# Embedded Linux — Assignment 2
## Sensor Monitoring System and Real-Time Analysis

**Basis:** source files and test screenshots previously shared in the conversation. This is a reconstructed report, not the original `Assignment2_Report.md` from Ubuntu.

### 1. Overview
The project implements a sensor monitoring system for Linux 5.15 on QEMU Versatile Express Cortex-A9, with a character device driver and four POSIX worker threads: Sensor, Processor, Alert, and Logger. The application uses message queues, condition variables, and synchronization.

### 2. Environment
- Target: QEMU `vexpress-a9`, ARM Cortex-A9, ARMv7
- Kernel: Linux 5.15
- Toolchain: `arm-linux-gnueabi-gcc`
- Host-side tools: GDB, Valgrind, strace, perf
- Real-device application and separate host mock for test instrumentation

### 3. Functional observations
QEMU screenshots show the `sms_sensor_driver` module loaded, `/proc/misc` reporting `125 sms_sensor`, the manual `/dev/sms_sensor` node, four worker threads running, sensor data and alerts, and graceful shutdown. A SCHED_FIFO test showed scheduling policy 1 on the four worker threads with priorities 5, 10, 15, and 20.

### 4. Debugging and memory analysis
The mock application was checked with Valgrind (18 allocations, 18 frees, 0 bytes at exit, 0 errors in the recorded run). GDB hit breakpoints in `sensor_thread()` and `send_alarm()`; examples included humidity 85% exceeding 80% and temperature 42 C exceeding 40 C. Both were correctly flagged. These are **mock-side tests**, not on-target Valgrind results.

### 5. System-call tracing and profiling
A host mock strace run highlighted `clock_nanosleep`, `mq_timedreceive`, `futex`, `mq_timedsend`, and `write`. `perf stat` measured 9.91 ms task-clock, 202 context switches, 12 migrations, and no page faults during the recorded attachment interval. `perf record` collected 105 samples with none lost; this is a small sample and should not be overinterpreted.

### 6. Real-time jitter experiment

| Metric | SCHED_OTHER | SCHED_FIFO |
|---|---:|---:|
| Samples | 1428 | 1458 |
| Minimum (microseconds) | 298 | 197 |
| Mean (microseconds) | 5847.62 | 5193.47 |
| Maximum (microseconds) | 24300 | 10313 |
| P99 (microseconds) | 19058 | 9915 |

In the recorded runs, FIFO had about 11.2% lower mean jitter and 48.0% lower P99 jitter. These were separate QEMU measurements, not a controlled benchmark on physical hardware. Each exceeded the 1,000 sample requirement.

### 7. Limitations and missing original files
The original sensor driver implementation (`sms_sensor_driver.c`), complete run logs, prebuilt kernel image, and initramfs from Ubuntu were not present in the chat workspace when this archive was prepared. The package contains the source files recovered from this chat, a header, transcribed results, and evidence screenshots, but **is not a complete buildable target system** until the missing driver and binary images are provided.

### 8. Conclusion
The screenshots and captured host-side tests support functional multi-threaded sensor monitoring and comparative real-time jitter measurements in QEMU. Further work should restore original build artifacts and repeat timing tests under controlled conditions.
