/*
 * SSNFS client: implements Create, Open, Read, Write, Seek, List, Delete, Close
 * and then runs the instructor's test code in main.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <rpc/rpc.h>

#include "ssnfs.h"


CLIENT *clnt;

/* connect to server */
void ssnfsprog_1(char *host) {
    clnt = clnt_create(host, SSNFSPROG, SSNFSVER, "tcp");
    if (clnt == NULL) {
        clnt_pcreateerror(host);
        exit(1);
    }
}

/* helper: current login name */
static void get_login(char *dst) {
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name) {
        strncpy(dst, pw->pw_name, USER_NAME_SIZE - 1);
        dst[USER_NAME_SIZE - 1] = '\0';
    } else {
        strncpy(dst, "unknown", USER_NAME_SIZE - 1);
        dst[USER_NAME_SIZE - 1] = '\0';
    }
}

/* returns fd >= 0 on success, -1 on failure */
int Open(char *filename_to_open) {
    open_output *result;
    open_input   arg;

    get_login(arg.user_name);
    strncpy(arg.file_name, filename_to_open, FILE_NAME_SIZE - 1);
    arg.file_name[FILE_NAME_SIZE - 1] = '\0';

    result = open_file_1(&arg, clnt);
    if (result == NULL) {
        clnt_perror(clnt, "open_file_1 failed");
        return -1;
    }
    printf("Open: %s\n", result->out_msg.out_msg_val);
    return result->fd;
}

/* returns 1 on success, -1 on failure */
int Create(char *filename_to_create) {
    create_output *result;
    create_input   arg;

    get_login(arg.user_name);
    strncpy(arg.file_name, filename_to_create, FILE_NAME_SIZE - 1);
    arg.file_name[FILE_NAME_SIZE - 1] = '\0';

    result = create_file_1(&arg, clnt);
    if (result == NULL) {
        clnt_perror(clnt, "create_file_1 failed");
        return -1;
    }
    printf("Create: %s\n", result->out_msg.out_msg_val);
    return result->success;
}

/* returns number of bytes written or -1 */
int Write(int fd, const char *buf, int n) {
    write_output *result;
    write_input   arg;
    char          tmp[1024];    /* big enough for n */

    if (n > (int)sizeof(tmp)) {
        fprintf(stderr, "Write error: n=%d too large\n", n);
        return -1;
    }

    get_login(arg.user_name);
    arg.fd = fd;
    arg.numbytes = n;

    /* Copy into a writable buffer; XDR will operate on this */
    memcpy(tmp, buf, n);
    arg.buffer.buffer_len = n;
    arg.buffer.buffer_val = tmp;

    result = write_file_1(&arg, clnt);
    if (result == NULL) {
        clnt_perror(clnt, "write_file_1 failed");
        return -1;
    }

    printf("Write: %s\n", result->out_msg.out_msg_val);
    return (result->success == 1) ? n : -1;
}

/* returns number of bytes read or -1 */
int Read(int fd, char *buf, int n) {
    read_output *result;
    read_input   arg;
    int          bytes;

    get_login(arg.user_name);
    arg.fd = fd;
    arg.numbytes = n;

    result = read_file_1(&arg, clnt);
    if (result == NULL) {
        clnt_perror(clnt, "read_file_1 failed");
        return -1;
    }
    if (result->success != 1) {
        printf("Read error: %s\n", result->out_msg.out_msg_val);
        return -1;
    }
    bytes = (int)result->buffer.buffer_len;
    if (bytes > n) bytes = n;
    memcpy(buf, result->buffer.buffer_val, bytes);
    return bytes;
}

/* returns new position or -1 */
int Seek(int fd, int pos) {
    seek_output *result;
    seek_input   arg;

    get_login(arg.user_name);
    arg.fd = fd;
    arg.position = pos;

    result = seek_position_1(&arg, clnt);
    if (result == NULL) {
        clnt_perror(clnt, "seek_position_1 failed");
        return -1;
    }
    if (result->success != 1) {
        printf("Seek error: %s\n", result->out_msg.out_msg_val);
        return -1;
    }
    return pos;
}

/* returns 1 on success, -1 on failure (spec wants void) */
void Close(int fd) {
    close_output *result;
    close_input   arg;

    get_login(arg.user_name);
    arg.fd = fd;

    result = close_file_1(&arg, clnt);
    if (result == NULL) {
        clnt_perror(clnt, "close_file_1 failed");
        return;
    }
    printf("Close: %s\n", result->out_msg.out_msg_val);
}

void List(void) {
    list_output *result;
    list_input   arg;

    get_login(arg.user_name);

    result = list_files_1(&arg, clnt);
    if (result == NULL) {
        clnt_perror(clnt, "list_files_1 failed");
        return;
    }
    printf("List:\n%s\n", result->out_msg.out_msg_val);
}

void Delete(const char *name) {
    delete_output *result;
    delete_input   arg;

    get_login(arg.user_name);
    strncpy(arg.file_name, name, FILE_NAME_SIZE - 1);
    arg.file_name[FILE_NAME_SIZE - 1] = '\0';

    result = delete_file_1(&arg, clnt);
    if (result == NULL) {
        clnt_perror(clnt, "delete_file_1 failed");
        return;
    }
    printf("Delete: %s\n", result->out_msg.out_msg_val);
}

int main(int argc, char *argv[]) {
    char *host;
    int i, j;
    int fd1, fd2, fd3, fd4;
    char buffer[100];

    if (argc < 2) {
        printf("usage: %s server_host\n", argv[0]);
        exit(1);
    }
    host = argv[1];
    ssnfsprog_1(host);

    /* instructor's sample main logic */
    if (Create("File1") == 1) {
        printf("File1 created successfully\n");
    } else {
        printf("File1 not created\n");
    }
    if (Create("File2") == 1) {
        printf("File2 created\n");
    } else {
        printf("File2 not created\n");
    }
    if (Create("File3") == 1) {
        printf("File3 created\n");
    } else {
        printf("File3 not created\n");
    }

    fd1 = Open("File1");
    fd2 = Open("File2");
    fd3 = Open("File3");

    if (fd1 < 0 || fd2 < 0 || fd3 < 0) {
        printf("Error: failed to open one or more files\n");
        return 1;
    }
    printf("DEBUG: starting first write loop\n");

    /* Write the full intended string to File1 (not just 15 bytes) */
    const char *file1_msg = "This is a test program for cs570 assignment 4";
    int file1_len = (int)strlen(file1_msg);
    for (i = 0; i < 20; i++) {
        if (Write(fd1, file1_msg, file1_len) < 0) {
            printf("Write to File1 failed at iteration %d\n", i);
            break;
        }
    }
    printf("DEBUG: finished first write loop\n");

    Close(fd1);

    int n = Read(fd1, buffer, 20);
    if (n < 0) {
        printf("As expected: Read on closed fd1 failed\n");
    } else {
        buffer[(n < (int)sizeof(buffer) - 1) ? n : (int)sizeof(buffer) - 1] = '\0';
        printf("Unexpected read on closed fd1: %s\n", buffer);
    }

    fd4 = Open("File1");
    if (fd4 < 0) {
        printf("Failed to reopen File1\n");
        return 1;
    }

    /* read File1 from beginning in 20-byte chunks */
    for (j = 0; j < 20; j++) {
        n = Read(fd4, buffer, 20);
        if (n <= 0) {
            break;
        }
        buffer[(n < (int)sizeof(buffer) - 1) ? n : (int)sizeof(buffer) - 1] = '\0';
        printf("%s\n", buffer);
    }

    /* Write the full intended string to File2 (not a truncated 25) */
    const char *file2_msg = "Welcome to University of Kentucky";
    int file2_len = (int)strlen(file2_msg);
    for (i = 0; i < 50; i++) {
        if (Write(fd2, file2_msg, file2_len) < 0) {
            printf("Write to File2 failed at iteration %d\n", i);
            break;
        }
    }

    /* Reset file pointer to beginning before reading back File2 */
    if (Seek(fd2, 0) < 0) {
        printf("Seek on File2 failed (reset to 0)\n");
    }

    /* Read File2 in 20-byte chunks */
    for (j = 0; j < 20; j++) {
        n = Read(fd2, buffer, 20);
        if (n <= 0) {
            break;
        }
        buffer[(n < (int)sizeof(buffer) - 1) ? n : (int)sizeof(buffer) - 1] = '\0';
        printf("%s\n", buffer);
    }

    if (Seek(fd2, 40) < 0) {
        printf("Seek on File2 failed\n");
    } else {
        n = Read(fd2, buffer, 20);
        if (n > 0) {
            buffer[(n < (int)sizeof(buffer) - 1) ? n : (int)sizeof(buffer) - 1] = '\0';
            printf("%s\n", buffer);
        } else {
            printf("Read after Seek returned %d\n", n);
        }
    }

    Close(fd2);
    List();
    Delete("File1");
    List();
    Close(fd3);
    Close(fd4);

    return 0;
}
