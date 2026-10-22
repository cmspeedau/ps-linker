/*
 * PS Linker - resumable HTTP download (PSP)
 *
 * Downloads a URL to dest_path, resuming from an existing partial
 * file when the server supports Range requests (206 responses).
 * Self-contained: apps call psp_resume_download() instead of a
 * hand-rolled sceHttp loop.
 */

#ifndef RESUME_H
#define RESUME_H

/* Optional progress callback: pct (0-100, -1 if unknown), bytes so far. */
typedef void (*resume_progress_cb)(int pct, long total);

/*
 * Download url to dest_path with resume support.
 * Returns 0 on success, <0 on error:
 *   -1 template/connection failure
 *   -2 request/send failure
 *   -3 cannot open destination file
 *   -4 server returned unexpected status
 *   -5 write error mid-transfer
 */
int psp_resume_download(const char *url, const char *dest_path,
                       resume_progress_cb progress);

#endif /* RESUME_H */
