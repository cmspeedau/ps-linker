/*
 * PS Linker - resumable HTTP download (PS3 / PSL1GHT)
 *
 * Downloads url to dest_path, continuing an existing partial file with
 * "Range: bytes=<size>-" when the server honours it (206). A 200 response
 * means the server ignored Range: the file is rewritten from the start.
 *
 * The finished file is always hashed (SHA-256) and the digest is logged via
 * NETDEBUG when built with -DNASDL_NETDEBUG. If the response carried
 * X-SHA256 / Content-SHA256, the digest is compared and a mismatch fails.
 */

#ifndef PS3_RESUME_H
#define PS3_RESUME_H

/* Optional progress callback: pct (0-100, -1 if unknown), bytes so far. */
typedef void (*resume_progress_cb)(int pct, long total);

/*
 * Returns 0 on success, <0 on error:
 *   -1 bad URL            -2 DNS failure       -3 socket failure
 *   -4 connect failure    -5 send failure      -6 cannot open destination
 *   -7 unexpected status   -8 header parse      -9 incomplete/truncated body
 *  -10 cannot hash file   -11 SHA-256 mismatch with the response digest
 */
int ps3_resume_download(const char *url, const char *dest_path, resume_progress_cb progress);

#endif
