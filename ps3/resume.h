#ifndef PS3_RESUME_H
#define PS3_RESUME_H
typedef void (*resume_progress_cb)(int pct, long total);
int ps3_resume_download(const char *url, const char *dest_path, resume_progress_cb progress);
#endif
