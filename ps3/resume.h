/*
 * PS Linker - resumable HTTP download (PS3 / PSL1GHT)
 *
 * Downloads a URL to dest_path, resuming from an existing partial
 * file when the server supports Range requests (206 responses).
 * Self-contained BSD-socket HTTP client so apps can call
 * ps3_resume_download() without touching their own net helpers.
 */

#ifndef RESUME_H
#define RESUME_H

/* Optional progress callback: pct (0-100, -1 if unknown), bytes so far. */
typedef void (*resume_progress_cb)(int pct, long total);

/*
 * Download url to dest_path with resume support.
 * Returns 0 on success, <0 on error:
 *   -1 bad URL
 *   -2 DNS failure
 *   -3 socket failure
 *   -4 connect failure
 *   -5 request send failure
 *   -6 cannot open destination file
 *   -7 server returned unexpected status
 *   -8 header parse error
 *   -9 write error mid-transfer
 */
int ps3_resume_download(const char *url, const char *dest_path,
                        resume_progress_cb progress);

#endif /* RESUME_H */
