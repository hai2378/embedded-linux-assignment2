# Embedded Linux Assignment 2 — Recovered Project Files

## Structure

- `app/` — four application sources: original, host mock, real-time, and real-time mock
- `driver/sms_sensor.h` — recovered driver ioctl header (the driver implementation is missing)
- `bao_cao/Assignment2_Report.md` — reconstructed English report
- `bao_cao/evidence/` — real values transcribed from screenshots, including QEMU jitter comparison CSV
- `bao_cao/screenshots/` — original screenshots provided in the conversation, useful as supporting evidence
- `MISSING_FILES.md` — what still needs to be recovered from Ubuntu

## Recorded experimental results

| Metric | SCHED_OTHER (QEMU) | SCHED_FIFO (QEMU) |
|---|---:|---:|
| Samples | 1428 | 1458 |
| Avg jitter (us) | 5847.62 | 5193.47 |
| Max jitter (us) | 24300 | 10313 |
| P99 jitter (us) | 19058 | 9915 |

In a host-side mock Valgrind run, 18 allocations and 18 frees were recorded with 0 bytes remaining and 0 errors. GDB, strace, and perf evidence was also gathered on the mock.

## Compiling the host mock on Ubuntu (optional)

```bash
gcc -g -O0 -Wall -Wextra -pthread -o sms_app_rt_mock app/sms_app_rt_mock.c -lrt -lm
./sms_app_rt_mock -p other -d 13 -i 10
```

## Compiling the real target app (requires driver and target image)

```bash
arm-linux-gnueabi-gcc -O2 -Wall -Wextra -static -pthread \
  -o sms_app_rt app/sms_app_rt.c -lrt -lm
```

These commands do not rebuild the missing kernel, driver `.ko`, or initramfs.

## Uploading to GitHub using Windows

Extract this archive. Open an empty GitHub repository and select **Add file > Upload files**. Drag the *contents* of the extracted folder (the `app`, `driver`, `bao_cao` folders and Markdown files), not the enclosing folder and not just the ZIP itself, then select **Commit changes**. GitHub web uploads do not automatically unzip a ZIP.
