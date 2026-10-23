/*
 * PS Linker - PS3 downloader main application.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <net/net.h>
#include <io/pad.h>
#include "resume.h"

int main(void) {
    return ps3_resume_download(NULL, NULL, NULL);
}
