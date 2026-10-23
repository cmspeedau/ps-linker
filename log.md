# Development Log

Running log of build, verification, and on-device testing work on PS Linker.
Newest entries first. This complements the README roadmap — it records what was actually run and observed, including bugs found while testing.

---

## 2026-10-23 (resume) — PS3 Range resume restored + verified on RPCS3 (206, SHA-256)

*Closes the "PS3 resume (Range/206) is gone" item flagged in the netdebug entry below.* Pulled the new `ps3/resume.c`, `ps3/resume.h` and updated `ps3/main.c` (the download path goes through `ps3_resume_download()` again), then built, ran, and tested it for real.

The local verification log records the build and emulator results, including the 206 Partial Content response and final SHA-256 match.

---

## 2026-10-23 — PS3/PSP downloader development log

See the preceding entries for build, emulator, and navigation verification details.
