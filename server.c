/*
 * SSNFS server: stateful file server with virtual disk.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rpc/rpc.h>
#include <unistd.h>
#include <fcntl.h>
#include "ssnfs.h"

#define BLOCK_SIZE      512
#define DISK_SIZE       (16 * 1024 * 1024)
#define TOTAL_BLOCKS    (DISK_SIZE / BLOCK_SIZE)
#define BLOCKS_PER_FILE 64
#define MAX_USERS       10
#define MAX_FILES_USER  10
#define MAX_OPEN_FILES  20
#define VDISK_NAME      "virtual_disk.bin"
static int file_max_size(void) { return BLOCKS_PER_FILE * BLOCK_SIZE; }

typedef struct {
    char file_name[FILE_NAME_SIZE];
    int  start_block;          /* -1 if unused */
} file_meta_t;

typedef struct {
    char       user_name[USER_NAME_SIZE];
    int        in_use;
    file_meta_t files[MAX_FILES_USER];
} user_meta_t;

typedef struct {
    int  in_use;
    int  fd;
    char user_name[USER_NAME_SIZE];
    char file_name[FILE_NAME_SIZE];
    int  start_block;
    int  current_pos;
} open_entry_t;

static int          disk_fd = -1;
static user_meta_t  users[MAX_USERS];
static open_entry_t open_table[MAX_OPEN_FILES];
static int          next_fd = 3;
static int          block_used[TOTAL_BLOCKS]; /* 0 free, 1 used */


/* forward declarations */
static void init_disk(void);
static void load_metadata(void);
static void save_metadata(void);
static user_meta_t *find_or_create_user(const char *user);
static user_meta_t *find_user(const char *user);
static file_meta_t *find_file(user_meta_t *u, const char *fname);
static file_meta_t *create_file_meta(user_meta_t *u, const char *fname, int *err);
static int  allocate_blocks(void);
static void free_blocks(int start_block);
static open_entry_t *find_open_by_fd(int fd);
static open_entry_t *alloc_open_entry(void);

/* called from RPCs when disk_fd < 0 */
static void init_disk(void) {
    int i, exists = 0;
    if (access(VDISK_NAME, F_OK) == 0)
        exists = 1;

    disk_fd = open(VDISK_NAME, O_RDWR | O_CREAT, 0666);
    if (disk_fd < 0) {
        perror("open virtual_disk.bin");
        exit(1);
    }
    if (!exists) {
        if (ftruncate(disk_fd, DISK_SIZE) < 0) {
            perror("ftruncate");
            exit(1);
        }
        memset(block_used, 0, sizeof(block_used));
        memset(users, 0, sizeof(users));
        save_metadata();
    } else {
        load_metadata();
    }
    for (i = 0; i < MAX_OPEN_FILES; i++)
        open_table[i].in_use = 0;
}

/* simple metadata layout: block 0 reserved for block_used[],
   block 1..N for users/files */
static void load_metadata(void) {
    ssize_t sz;
    lseek(disk_fd, 0, SEEK_SET);
    sz = read(disk_fd, block_used, sizeof(block_used));
    if (sz != sizeof(block_used)) {
        memset(block_used, 0, sizeof(block_used));
    }
    sz = read(disk_fd, users, sizeof(users));
    if (sz != sizeof(users)) {
        memset(users, 0, sizeof(users));
    }
}

static void save_metadata(void) {
    lseek(disk_fd, 0, SEEK_SET);
    write(disk_fd, block_used, sizeof(block_used));
    write(disk_fd, users, sizeof(users));
    fsync(disk_fd);
}

static user_meta_t *find_user(const char *user) {
    int i;
    for (i = 0; i < MAX_USERS; i++) {
        if (users[i].in_use &&
            strncmp(users[i].user_name, user, USER_NAME_SIZE) == 0)
            return &users[i];
    }
    return NULL;
}

static user_meta_t *find_or_create_user(const char *user) {
    user_meta_t *u = find_user(user);
    int i;
    if (u) return u;
    for (i = 0; i < MAX_USERS; i++) {
        if (!users[i].in_use) {
            users[i].in_use = 1;
            strncpy(users[i].user_name, user, USER_NAME_SIZE - 1);
            users[i].user_name[USER_NAME_SIZE - 1] = '\0';
            memset(users[i].files, 0, sizeof(users[i].files));
            save_metadata();
            return &users[i];
        }
    }
    return NULL;
}

static file_meta_t *find_file(user_meta_t *u, const char *fname) {
    int i;
    for (i = 0; i < MAX_FILES_USER; i++) {
        if (u->files[i].start_block >= 0 &&
            strncmp(u->files[i].file_name, fname, FILE_NAME_SIZE) == 0)
            return &u->files[i];
    }
    return NULL;
}

static file_meta_t *create_file_meta(user_meta_t *u, const char *fname, int *err) {
    int i;
    if (find_file(u, fname) != NULL) {
        *err = 1; /* already exists */
        return NULL;
    }
    for (i = 0; i < MAX_FILES_USER; i++) {
        if (u->files[i].start_block < 0 || u->files[i].file_name[0] == '\0') {
            strncpy(u->files[i].file_name, fname, FILE_NAME_SIZE - 1);
            u->files[i].file_name[FILE_NAME_SIZE - 1] = '\0';
            u->files[i].start_block = -1;
            *err = 0;
            return &u->files[i];
        }
    }
    *err = 2; /* too many files */
    return NULL;
}

static int allocate_blocks(void) {
    int i, run = 0, start = -1;
    for (i = 2; i < TOTAL_BLOCKS; i++) { /* 0/1 reserved for metadata */
        if (!block_used[i]) {
            if (run == 0) start = i;
            run++;
            if (run == BLOCKS_PER_FILE) {
                int j;
                for (j = start; j < start + BLOCKS_PER_FILE; j++)
                    block_used[j] = 1;
                save_metadata();
                return start;
            }
        } else {
            run = 0;
            start = -1;
        }
    }
    return -1;
}

static void free_blocks(int start_block) {
    int i;
    if (start_block < 0) return;
    for (i = start_block; i < start_block + BLOCKS_PER_FILE && i < TOTAL_BLOCKS; i++)
        block_used[i] = 0;
    save_metadata();
}

static open_entry_t *find_open_by_fd(int fd) {
    int i;
    for (i = 0; i < MAX_OPEN_FILES; i++) {
        if (open_table[i].in_use && open_table[i].fd == fd)
            return &open_table[i];
    }
    return NULL;
}

static open_entry_t *alloc_open_entry(void) {
    int i;
    for (i = 0; i < MAX_OPEN_FILES; i++) {
        if (!open_table[i].in_use)
            return &open_table[i];
    }
    return NULL;
}

/* RPC implementations */

open_output *open_file_1_svc(open_input *argp, struct svc_req *rqstp) {
    static open_output result;
    user_meta_t *u;
    file_meta_t *fm;
    open_entry_t *oe;
    char msg[128];
    fprintf(stderr, "DEBUG: open_file_1_svc user=%s file=%s\n",argp->user_name, argp->file_name);
    memset(&result, 0, sizeof(result));
    if (disk_fd < 0) {
        init_disk();
    }
    result.fd = -1;

    u = find_user(argp->user_name);
    if (!u) {
        snprintf(msg, sizeof(msg), "User directory not found");
        goto ret_err;
    }
    fm = find_file(u, argp->file_name);
    if (!fm || fm->start_block < 0) {
        snprintf(msg, sizeof(msg), "File not found");
        goto ret_err;
    }
    oe = alloc_open_entry();
    if (!oe) {
        snprintf(msg, sizeof(msg), "Open file table full");
        goto ret_err;
    }
    oe->in_use = 1;
    oe->fd = next_fd++;
    strncpy(oe->user_name, argp->user_name, USER_NAME_SIZE);
    strncpy(oe->file_name, argp->file_name, FILE_NAME_SIZE);
    oe->start_block = fm->start_block;
    oe->current_pos = 0;

    result.fd = oe->fd;
    snprintf(msg, sizeof(msg), "File opened");

ret_err:
    if (result.out_msg.out_msg_val != NULL) {
        free(result.out_msg.out_msg_val);
    }
    result.out_msg.out_msg_len = strlen(msg) + 1;
    result.out_msg.out_msg_val = strdup(msg);
    return &result;
}

read_output *read_file_1_svc(read_input *argp, struct svc_req *rqstp) {
    static read_output result;
    open_entry_t *oe;
    char msg[128];
    int maxsize = file_max_size();
    int to_read, offset;
    ssize_t r;
    fprintf(stderr, "DEBUG: read_file_1_svc user=%s fd=%d numbytes=%d\n",
        argp->user_name, argp->fd, argp->numbytes);
    memset(&result, 0, sizeof(result));
    if (disk_fd < 0) {
        init_disk();
    }
    result.success = -1;

    oe = find_open_by_fd(argp->fd);
    if (!oe) {
        snprintf(msg, sizeof(msg), "Invalid file descriptor");
        goto ret_msg;
    }
    if (argp->numbytes <= 0) {
        snprintf(msg, sizeof(msg), "Nothing to read");
        goto ret_msg;
    }
    if (oe->current_pos >= maxsize) {
        snprintf(msg, sizeof(msg), "End of file");
        goto ret_msg;
    }

    if (oe->current_pos + argp->numbytes > maxsize)
        to_read = maxsize - oe->current_pos;
    else
        to_read = argp->numbytes;

    offset = oe->start_block * BLOCK_SIZE + oe->current_pos;
    if (offset < 0 || offset + to_read > DISK_SIZE) {
      snprintf(msg, sizeof(msg), "Read offset out of range");
      goto ret_msg;
    }
    if (lseek(disk_fd, offset, SEEK_SET) < 0) {
        perror("lseek read");
        snprintf(msg, sizeof(msg), "Seek error");
        goto ret_msg;
    }

    result.buffer.buffer_val = malloc(to_read);
    if (result.buffer.buffer_val == NULL) {
        snprintf(msg, sizeof(msg), "Read alloc failed");
        goto ret_msg;
    }

    r = read(disk_fd, result.buffer.buffer_val, to_read);
    if (r < 0) {
        perror("read");
        free(result.buffer.buffer_val);
        result.buffer.buffer_val = NULL;
        snprintf(msg, sizeof(msg), "Read error");
        goto ret_msg;
    }

    result.buffer.buffer_len = (u_int)r;
    oe->current_pos += r;
    result.success = 1;
    snprintf(msg, sizeof(msg), "Read ok");

ret_msg:
    if (result.out_msg.out_msg_val != NULL) {
        free(result.out_msg.out_msg_val);
    }
    result.out_msg.out_msg_len = strlen(msg) + 1;
    result.out_msg.out_msg_val = strdup(msg);
    return &result;
}

write_output *write_file_1_svc(write_input *argp, struct svc_req *rqstp) {
    static write_output result;
    open_entry_t *oe;
    char msg[128];
    int maxsize = file_max_size();
    int to_write, offset;
    ssize_t w;
    fprintf(stderr, "DEBUG: write_file_1_svc user=%s fd=%d numbytes=%d\n",argp->user_name, argp->fd, argp->numbytes);
    memset(&result, 0, sizeof(result));
    if (disk_fd < 0) {
        init_disk();
    }
    result.success = -1;
    
    oe = find_open_by_fd(argp->fd);
    if (!oe) {
        snprintf(msg, sizeof(msg), "Invalid file descriptor");
        goto ret_err;
    }
    if (argp->numbytes <= 0 || argp->buffer.buffer_val == NULL) {
        snprintf(msg, sizeof(msg), "Nothing to write");
        goto ret_err;
    }

    if (oe->current_pos + argp->numbytes > maxsize)
        to_write = maxsize - oe->current_pos;
    else
        to_write = argp->numbytes;

    if (to_write <= 0) {
        snprintf(msg, sizeof(msg), "No space left in file");
        goto ret_err;
    }

    offset = oe->start_block * BLOCK_SIZE + oe->current_pos;
    fprintf(stderr, "DEBUG: write_file_1_svc offset=%d to_write=%d\n", offset, to_write);
    if (offset < 0 || offset + to_write > DISK_SIZE) {
     snprintf(msg, sizeof(msg), "Write offset out of range");
     goto ret_err;
    }

    if (lseek(disk_fd, offset, SEEK_SET) < 0) {
        perror("lseek write");
        snprintf(msg, sizeof(msg), "Seek error");
        goto ret_err;
    }
    w = write(disk_fd, argp->buffer.buffer_val, to_write);
    if (w < 0) {
        perror("write");
        snprintf(msg, sizeof(msg), "Write error");
        goto ret_err;
    }

    oe->current_pos += w;
    result.success = 1;
    snprintf(msg, sizeof(msg), "Write ok (%ld bytes)", (long)w);

ret_err:
    if (result.out_msg.out_msg_val != NULL) {
        free(result.out_msg.out_msg_val);
    }
    result.out_msg.out_msg_len = strlen(msg) + 1;
    result.out_msg.out_msg_val = strdup(msg);
    return &result;
}

list_output *list_files_1_svc(list_input *argp, struct svc_req *rqstp) {
    static list_output result;
    user_meta_t *u;
    char *buf;
    size_t sz;
    int i;

    memset(&result, 0, sizeof(result));
    if (disk_fd < 0) {
        init_disk();
    }

    u = find_user(argp->user_name);
    if (!u) {
        const char *msg = "User directory empty\n";
        if (result.out_msg.out_msg_val != NULL) {
            free(result.out_msg.out_msg_val);
        }
        result.out_msg.out_msg_len = strlen(msg) + 1;
        result.out_msg.out_msg_val = strdup(msg);
        return &result;
    }

    buf = malloc(1024);
    if (buf == NULL) {
        const char *msg = "List alloc failed\n";
        if (result.out_msg.out_msg_val != NULL) {
            free(result.out_msg.out_msg_val);
        }
        result.out_msg.out_msg_len = strlen(msg) + 1;
        result.out_msg.out_msg_val = strdup(msg);
        return &result;
    }
    buf[0] = '\0';

    for (i = 0; i < MAX_FILES_USER; i++) {
        if (u->files[i].start_block >= 0 &&
            u->files[i].file_name[0] != '\0') {
            strcat(buf, u->files[i].file_name);
            strcat(buf, "\n");
        }
    }

    sz = strlen(buf) + 1;
    if (result.out_msg.out_msg_val != NULL) {
        free(result.out_msg.out_msg_val);
    }
    result.out_msg.out_msg_len = sz;
    result.out_msg.out_msg_val = buf;
    return &result;
}

delete_output *delete_file_1_svc(delete_input *argp, struct svc_req *rqstp) {
    static delete_output result;
    user_meta_t *u;
    file_meta_t *fm;
    char msg[128];
    int i;

    memset(&result, 0, sizeof(result));
    if (disk_fd < 0) {
        init_disk();
    }

    u = find_user(argp->user_name);
    if (!u) {
        snprintf(msg, sizeof(msg), "User directory not found");
        goto ret_done;
    }
    fm = find_file(u, argp->file_name);
    if (!fm) {
        snprintf(msg, sizeof(msg), "File not found");
        goto ret_done;
    }

    /* ensure not open */
    for (i = 0; i < MAX_OPEN_FILES; i++) {
        if (open_table[i].in_use &&
            strncmp(open_table[i].user_name, argp->user_name, USER_NAME_SIZE) == 0 &&
            strncmp(open_table[i].file_name, argp->file_name, FILE_NAME_SIZE) == 0) {
            snprintf(msg, sizeof(msg), "Cannot delete open file");
            goto ret_done;
        }
    }

    free_blocks(fm->start_block);
    fm->start_block = -1;
    fm->file_name[0] = '\0';
    save_metadata();
    snprintf(msg, sizeof(msg), "File deleted");

ret_done:
    if (result.out_msg.out_msg_val != NULL) {
        free(result.out_msg.out_msg_val);
    }
    result.out_msg.out_msg_len = strlen(msg) + 1;
    result.out_msg.out_msg_val = strdup(msg);
    return &result;
}

close_output *close_file_1_svc(close_input *argp, struct svc_req *rqstp) {
    static close_output result;
    open_entry_t *oe;
    char msg[128];

    memset(&result, 0, sizeof(result));
    if (disk_fd < 0) {
        init_disk();
    }

    oe = find_open_by_fd(argp->fd);
    if (!oe) {
        snprintf(msg, sizeof(msg), "Invalid file descriptor");
    } else {
        oe->in_use = 0;
        snprintf(msg, sizeof(msg), "File closed");
    }
    if (result.out_msg.out_msg_val != NULL) {
        free(result.out_msg.out_msg_val);
    }
    result.out_msg.out_msg_len = strlen(msg) + 1;
    result.out_msg.out_msg_val = strdup(msg);
    return &result;
}

seek_output *seek_position_1_svc(seek_input *argp, struct svc_req *rqstp) {
    static seek_output result;
    open_entry_t *oe;
    char msg[128];
    int maxsize = file_max_size();

    memset(&result, 0, sizeof(result));
    if (disk_fd < 0) {
        init_disk();
    }
    result.success = -1;

    oe = find_open_by_fd(argp->fd);
    if (!oe) {
        snprintf(msg, sizeof(msg), "Invalid file descriptor");
        goto ret_done;
    }
    if (argp->position < 0 || argp->position > maxsize) {
        snprintf(msg, sizeof(msg), "Invalid position");
        goto ret_done;
    }
    oe->current_pos = argp->position;
    result.success = 1;
    snprintf(msg, sizeof(msg), "Seek ok");

ret_done:
    if (result.out_msg.out_msg_val != NULL) {
        free(result.out_msg.out_msg_val);
    }
    result.out_msg.out_msg_len = strlen(msg) + 1;
    result.out_msg.out_msg_val = strdup(msg);
    return &result;
}

create_output *create_file_1_svc(create_input *argp, struct svc_req *rqstp) {
    static create_output result;
    user_meta_t *u;
    file_meta_t *fm;
    char msg[128];
    int err = 0;
    int start;

    memset(&result, 0, sizeof(result));
    result.success = -1;

    if (disk_fd < 0) {
        init_disk();
    }

    u = find_or_create_user(argp->user_name);
    if (!u) {
        snprintf(msg, sizeof(msg), "Too many users");
        goto ret_done;
    }
    fm = create_file_meta(u, argp->file_name, &err);
    if (!fm) {
        if (err == 1)
            snprintf(msg, sizeof(msg), "File already exists");
        else
            snprintf(msg, sizeof(msg), "Max files per user reached");
        goto ret_done;
    }

    start = allocate_blocks();
    if (start < 0) {
        snprintf(msg, sizeof(msg), "No space on disk");
        goto ret_done;
    }
    fm->start_block = start;
    save_metadata();
    result.success = 1;
    snprintf(msg, sizeof(msg), "File created");

ret_done:
    if (result.out_msg.out_msg_val != NULL) {
        free(result.out_msg.out_msg_val);
    }
    result.out_msg.out_msg_len = strlen(msg) + 1;
    result.out_msg.out_msg_val = strdup(msg);
    return &result;
}
