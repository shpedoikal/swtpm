/*
 * ptm - CUSE based TPM PassThrough Multiplexer for QEMU.
 *
 * (c) Copyright IBM Corporation 2014, 2015.
 *
 * This program instantiates one /dev/vtpm* device, and
 * calls libtpms to handle requests
 *
 * The following code was derived from
 * http://fuse.sourceforge.net/doxygen/cusexmp_8c.html
 *
 * It's original header states:
 *
 * CUSE example: Character device in Userspace
 * Copyright (C) 2008-2009 SUSE Linux Products GmbH
 * Copyright (C) 2008-2009 Tejun Heo <tj@kernel.org>
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 *
 *
 * Authors: David Safford safford@us.ibm.com
 *          Stefan Berger stefanb@us.ibm.com
 * 
 */

/*
 * Note: It's possible for multiple process to open access to
 * the same character device. Concurrency problems may arise
 * if those processes all write() to the device and then try
 * to pick up the results. Proper usage of the device is to
 * have one process (QEMU) use ioctl, read and write and have
 * other processes (libvirt, etc.) only use ioctl.
 */
#define FUSE_USE_VERSION 29

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/types.h>
#include <ctype.h>
#include <pwd.h>
#include <grp.h>
#include <arpa/inet.h>

#include <libtpms/tpm_library.h>
#include <libtpms/tpm_tis.h>
#include <libtpms/tpm_error.h>
#include <libtpms/tpm_memory.h>
#include <libtpms/tpm_nvfilename.h>

#include "cuse_lowlevel.h"
#include "fuse_opt.h"
#include "tpm_ioctl.h"
#include "swtpm.h"
#include "swtpm_nvfile.h"
#include "key.h"
#include "logging.h"
#include "main.h"
#include "common.h"

#include <glib.h>

#define TPM_REQ_MAX 4096
static unsigned char *ptm_request, *ptm_response;
static uint32_t ptm_req_len, ptm_res_len, ptm_res_tot;
static TPM_MODIFIER_INDICATOR locality;
static int tpm_running;
static int thread_busy;
static GThreadPool *pool;
static struct passwd *passwd;

#if GLIB_MAJOR_VERSION >= 2
# if GLIB_MINOR_VERSION >= 32

GCond thread_busy_signal;
GMutex thread_busy_lock;
GMutex file_ops_lock;
#  define THREAD_BUSY_SIGNAL &thread_busy_signal
#  define THREAD_BUSY_LOCK &thread_busy_lock
#  define FILE_OPS_LOCK &file_ops_lock

# else

GCond *thread_busy_signal;
GMutex *thread_busy_lock;
GMutex *file_ops_lock;
#  define THREAD_BUSY_SIGNAL thread_busy_signal
#  define THREAD_BUSY_LOCK thread_busy_lock
#  define FILE_OPS_LOCK file_ops_lock

# endif
#else

#error Unsupport glib version

#endif

struct ptm_param {
    unsigned major;
    unsigned minor;
    char *dev_name;
    int is_help;
    const char *prgname;
    char *runas;
    char *logging;
    char *keydata;
    char *migkeydata;
};


enum msg_type {
    MESSAGE_TPM_CMD = 1,
    MESSAGE_IOCTL,
};

struct thread_message {
    enum msg_type type;
    fuse_req_t    req;
};

#define min(a,b) ((a) < (b) ? (a) : (b))

struct stateblob {
    uint8_t type;
    uint8_t *data;
    uint32_t length;
    TPM_BOOL is_encrypted;
};

typedef struct stateblob_desc {
    uint32_t blobtype;
    TPM_BOOL decrypt;
    TPM_BOOL is_encrypted;
    unsigned char *data;
    uint32_t data_length;
} stateblob_desc;

typedef enum tx_state_type {
    TX_STATE_RW_COMMAND = 1,
    TX_STATE_SET_STATE_BLOB = 2,
    TX_STATE_GET_STATE_BLOB = 3,
} tx_state_type;

typedef struct transfer_state {
    tx_state_type state;
    /* while in TX_STATE_GET/SET_STATEBLOB */
    uint32_t blobtype;
    TPM_BOOL blob_is_encrypted;
    /* while in TX_STATE_GET */
    uint32_t offset;
} transfer_state;

/* function prototypes */

static TPM_RESULT
ptm_set_stateblob_append(uint32_t blobtype,
                         const unsigned char *data, uint32_t length,
                         bool is_encrypted, bool is_last);

static int
cached_stateblob_get(uint32_t offset,
                     unsigned char **bufptr, size_t *length);


static const char *usage =
"usage: %s [options]\n"
"\n"
"The following options are supported:\n"
"\n"
"-n NAME|--name=NAME :  device name (mandatory)\n"
"-M MAJ|--maj=MAJ    :  device major number\n"
"-m MIN|--min=MIN    :  device minor number\n"
"--key file=<path>[,mode=aes-cbc][,format=hex|binary][,remove=[true|false]]\n"
"                    :  use an AES key for the encryption of the TPM's state\n"
"                       files; use the given mode for the block encryption;\n"
"                       the key is to be provided as a hex string or in binary\n"
"                       format; the keyfile can be automatically removed using\n"
"                       the remove parameter\n"
"--key pwdfile=<path>[,mode=aes-cbc][,remove=[true|false]]\n"
"                    :  provide a passphrase in a file; the AES key will be\n"
"                       derived from this passphrase\n"
"--migration-key file=<path>,[,mode=aes-cbc][,format=hex|binary][,remove=[true|false]]\n"
"                    :  use an AES key for the encryption of the TPM's state\n"
"                       when it is retrieved from the TPM via ioctls;\n"
"                       Setting this key ensures that the TPM's state will always\n"
"                       be encrypted when migrated\n"
"--migration-key pwdfile=<path>[,mode=aes-cbc][,remove=[true|false]]\n"
"                    :  provide a passphrase in a file; the AES key will be\n"
"                       derived from this passphrase\n"
"--log file=<path>|fd=<filedescriptor>\n"
"                    :  write the TPM's log into the given file rather than\n"
"                       to the console; provide '-' for path to avoid logging\n"
"-h|--help           :  display this help screen and terminate\n"
"\n"
"Make sure that TPM_PATH environment variable points to directory\n"
"where TPM's NV storage file is kept\n"
"\n";

const static unsigned char TPM_Resp_FatalError[] = {
    0x00, 0xC4,                     /* TPM Response */
    0x00, 0x00, 0x00, 0x0A,         /* length (10) */
    0x00, 0x00, 0x00, 0x09          /* TPM_FAIL */
};

const static unsigned char TPM_ResetEstablishmentBit[] = {
    0x00, 0xC1,                     /* TPM Request */
    0x00, 0x00, 0x00, 0x0A,         /* length (10) */
    0x40, 0x00, 0x00, 0x0B          /* TPM_ORD_ResetEstablishmentBit */
};

typedef struct TPM_Response_Header {
    uint16_t tag;
    uint32_t paramSize;
    uint32_t returnCode;
} __attribute__ ((packed)) TPM_Response_Header;

static TPM_RESULT
ptm_io_getlocality(TPM_MODIFIER_INDICATOR *loc, uint32_t tpmnum)
{
    *loc = locality;
    return TPM_SUCCESS;
}

static struct libtpms_callbacks cbs = {
    .sizeOfStruct           = sizeof(struct libtpms_callbacks),
    .tpm_nvram_init         = SWTPM_NVRAM_Init,
    .tpm_nvram_loaddata     = SWTPM_NVRAM_LoadData,
    .tpm_nvram_storedata    = SWTPM_NVRAM_StoreData,
    .tpm_nvram_deletename   = SWTPM_NVRAM_DeleteName,
    .tpm_io_getlocality     = ptm_io_getlocality,
};

static struct thread_message msg;

static transfer_state tx_state;

/* worker_thread_wait_done
 *
 * Wait while the TPM worker thread is busy
 */ 
static void worker_thread_wait_done(void)
{
    g_mutex_lock(THREAD_BUSY_LOCK);
    while (thread_busy) {
#if GLIB_MINOR_VERSION >= 32
        gint64 end_time = g_get_monotonic_time() +
            1 * G_TIME_SPAN_SECOND;
        g_cond_wait_until(THREAD_BUSY_SIGNAL,
                          THREAD_BUSY_LOCK,
                          end_time);
#else
        GTimeVal abs_time;
        /*
         * seems like occasionally the g_cond_signal did not wake up
         * the sleeping task; so we poll [TIS Test in BIOS]
         */
        abs_time.tv_sec = 1;
        abs_time.tv_usec = 0;
        g_cond_timed_wait(THREAD_BUSY_SIGNAL,
                          THREAD_BUSY_LOCK,
                          &abs_time);
#endif
    }
    g_mutex_unlock(THREAD_BUSY_LOCK);
}

/* worker_thread_mark_busy
 *
 * Mark the worker thread as busy; call this with the lock held
 */
static void worker_thread_mark_busy(void)
{
    g_mutex_lock(THREAD_BUSY_LOCK);
    thread_busy = 1;
    g_mutex_unlock(THREAD_BUSY_LOCK);
}

/* work_tread_mark_done
 *
 * Mark the worker thread as done and wake
 * up the waiting thread
 */
static void worker_thread_mark_done(void)
{
    g_mutex_lock(THREAD_BUSY_LOCK);
    thread_busy = 0;
    g_cond_signal(THREAD_BUSY_SIGNAL);
    g_mutex_unlock(THREAD_BUSY_LOCK);
}

/* worker_thread_is_busy
 *
 * Determine whether the worker thread is busy
 */
static int worker_thread_is_busy()
{
    return thread_busy;
}

static void worker_thread(gpointer data, gpointer user_data)
{
    struct thread_message *msg = (struct thread_message *)data;

    switch (msg->type) {
    case MESSAGE_TPM_CMD:
        TPMLIB_Process(&ptm_response, &ptm_res_len, &ptm_res_tot,
                       ptm_request, ptm_req_len);
        break;
    case MESSAGE_IOCTL:
        break;
    }

    /* results are ready */
    worker_thread_mark_done();
}

/* worker_thread_end
 *
 * finish the worker thread
 */
static void worker_thread_end()
{
    if (pool) {
        worker_thread_wait_done();
        g_thread_pool_free(pool, TRUE, TRUE);
        pool = NULL;
    }
}

/* _TPM_IO_TpmEstablished_Reset
 *
 * Reset the TPM Established bit
 */
static TPM_RESULT
_TPM_IO_TpmEstablished_Reset(fuse_req_t req,
                             TPM_MODIFIER_INDICATOR locty)
{
    TPM_RESULT res = TPM_FAIL;
    TPM_Response_Header *tpmrh;
    TPM_MODIFIER_INDICATOR orig_locality = locality;

    locality = locty;

    ptm_req_len = sizeof(TPM_ResetEstablishmentBit);
    memcpy(ptm_request, TPM_ResetEstablishmentBit, ptm_req_len);
    msg.type = MESSAGE_TPM_CMD;
    msg.req = req;

    worker_thread_mark_busy();

    g_thread_pool_push(pool, &msg, NULL);

    worker_thread_wait_done();

    if (ptm_res_len >= sizeof(TPM_Response_Header)) {
        tpmrh = (TPM_Response_Header *)ptm_response;
        res = ntohl(tpmrh->returnCode);
    }

    locality = orig_locality;

    return res;
}

static int tpm_start(uint32_t flags)
{
    DIR *dir;
    char * tpmdir = NULL;

    /* temporary - the backend script lacks the perms to do this */
    if (tpmdir == NULL) {
        tpmdir = getenv("TPM_PATH");
        if (!tpmdir) {
            logprintf(STDOUT_FILENO,
                      "Error: TPM_PATH is not set\n");
            return -1;
        }
    }
    dir = opendir(tpmdir);
    if (dir) {
        closedir(dir);
    } else {
        if (mkdir(tpmdir, 0775)) {
            logprintf(STDERR_FILENO,
                      "Error: Could not open TPM_PATH dir\n");
            return -1;
        }
    }

    pool = g_thread_pool_new(worker_thread,
                             NULL,
                             1,
                             TRUE,
                             NULL);
    if (!pool) {
        logprintf(STDERR_FILENO,
                  "Error: Could not create the thread pool.\n");
        return -1;
    }

    if (TPMLIB_RegisterCallbacks(&cbs) != TPM_SUCCESS) {
        logprintf(STDERR_FILENO,
                  "Error: Could not register the callbacks.\n");
        goto error_del_pool;
    }

    if (TPMLIB_MainInit() != TPM_SUCCESS) {
        logprintf(STDERR_FILENO,
                  "Error: Could not start the CUSE TPM.\n");
        goto error_del_pool;
    }

    if (flags & INIT_FLAG_DELETE_VOLATILE) {
        uint32_t tpm_number = 0;
        char *name = TPM_VOLATILESTATE_NAME;
        if (SWTPM_NVRAM_DeleteName(tpm_number,
                                   name,
                                   FALSE) != TPM_SUCCESS) {
            logprintf(STDERR_FILENO,
                      "Error: Could not delete the volatile "
                      "state of the TPM.\n");
            goto error_terminate;
        }
    }

    if(!ptm_request)
        ptm_request = malloc(4096);
    if(!ptm_request) {
        logprintf(STDERR_FILENO,
                  "Error: Could not allocate memory for request buffer.\n");
        goto error_terminate;
    }

    logprintf(STDOUT_FILENO,
              "CUSE TPM successfully initialized.\n");

    return 0;

error_del_pool:
    g_thread_pool_free(pool, TRUE, TRUE);
    pool = NULL;

error_terminate:
    TPMLIB_Terminate();
    return -1;
}

/*
 * convert the blobtype integer into a string that libtpms
 * understands
 */
static const char *ptm_get_blobname(uint32_t blobtype)
{
    switch (blobtype) {
    case PTM_BLOB_TYPE_PERMANENT:
        return TPM_PERMANENT_ALL_NAME;
    case PTM_BLOB_TYPE_VOLATILE:
        return TPM_VOLATILESTATE_NAME;
    case PTM_BLOB_TYPE_SAVESTATE:
        return TPM_SAVESTATE_NAME;
    default:
        return NULL;
    }
}

static void ptm_open(fuse_req_t req, struct fuse_file_info *fi)
{
    tx_state.state = TX_STATE_RW_COMMAND;

    fuse_reply_open(req, fi);
}

/* ptm_write_fatal_error_response
 *
 * Write a fatal error response
 */
static void ptm_write_fatal_error_response(void)
{
    if (ptm_response == NULL ||
        ptm_res_tot < sizeof(TPM_Resp_FatalError)) {
        ptm_res_tot = sizeof(TPM_Resp_FatalError);
        TPM_Realloc(&ptm_response, ptm_res_tot);
    }
    if (ptm_response) {
        ptm_res_len = sizeof(TPM_Resp_FatalError);
        memcpy(ptm_response,
               TPM_Resp_FatalError,
               sizeof(TPM_Resp_FatalError));
    }
}

static void ptm_read_cmd(fuse_req_t req, size_t size)
{
    int len;

    if (tpm_running) {
        /* wait until results are ready */
        worker_thread_wait_done();
    }

    len = ptm_res_len;

    if (ptm_res_len > size) {
        len = size;
        ptm_res_len -= size;
    } else {
        ptm_res_len = 0;
    }

    fuse_reply_buf(req, (const char *)ptm_response, len);
}

/*
 * ptm_read_stateblob: get a stateblob via the read() interface
 * @req: the fuse_req_t
 * @size: the number of bytes to read
 *
 * The internal offset into the buffer is advanced by the number
 * of bytes that were copied.
 */
static void ptm_read_stateblob(fuse_req_t req, size_t size)
{
    unsigned char *bufptr = NULL;
    size_t numbytes;
    size_t tocopy;

    if (cached_stateblob_get(tx_state.offset, &bufptr, &numbytes) < 0) {
        fuse_reply_err(req, EIO);
        tx_state.state = TX_STATE_RW_COMMAND;
    } else {
        tocopy = MIN(size, numbytes);
        tx_state.offset += tocopy;

        fuse_reply_buf(req, (char *)bufptr, tocopy);
        /* last transfer indicated by less bytes available than requested */
        if (numbytes < size) {
            tx_state.state = TX_STATE_RW_COMMAND;
        }
    }
}

static void ptm_read(fuse_req_t req, size_t size, off_t off,
                     struct fuse_file_info *fi)
{
    switch (tx_state.state) {
    case TX_STATE_RW_COMMAND:
        ptm_read_cmd(req, size);
        break;
    case TX_STATE_SET_STATE_BLOB:
        fuse_reply_err(req, EIO);
        tx_state.state = TX_STATE_RW_COMMAND;
        break;
    case TX_STATE_GET_STATE_BLOB:
        ptm_read_stateblob(req, size);
        break;
    }
}

static void ptm_write_cmd(fuse_req_t req, const char *buf, size_t size)
{
    ptm_req_len = size;
    ptm_res_len = 0;

    /* prevent other threads from writing or doing ioctls */
    g_mutex_lock(FILE_OPS_LOCK);

    if (tpm_running) {
        /* ensure that we only ever work on one TPM command */
        if (worker_thread_is_busy()) {
            fuse_reply_err(req, EBUSY);
            goto cleanup;
        }

        /* have command processed by thread pool */
        if (ptm_req_len > TPM_REQ_MAX)
            ptm_req_len = TPM_REQ_MAX;

        memcpy(ptm_request, buf, ptm_req_len);
        msg.type = MESSAGE_TPM_CMD;
        msg.req = req;

        worker_thread_mark_busy();

        g_thread_pool_push(pool, &msg, NULL);

        fuse_reply_write(req, ptm_req_len);
    } else {
        /* TPM not initialized; return error */
        ptm_write_fatal_error_response();
        fuse_reply_write(req, ptm_req_len);
    }

cleanup:
    g_mutex_unlock(FILE_OPS_LOCK);

    return;
}

/*
 * ptm_write_stateblob: Write the state blob using the write() interface
 *
 * @req: the fuse_req_t
 * @buf: the buffer with the data
 * @size: the number of bytes in the buffer
 *
 * The data are appended to an existing buffer that was created with the
 * initial ioctl().
 */
static void ptm_write_stateblob(fuse_req_t req, const char *buf, size_t size)
{
    TPM_RESULT res;

    res = ptm_set_stateblob_append(tx_state.blobtype,
                                   (unsigned char *)buf, size,
                                   tx_state.blob_is_encrypted,
                                   (size == 0));
    if (res) {
        tx_state.state = TX_STATE_RW_COMMAND;
        fuse_reply_err(req, EIO);
    } else {
        fuse_reply_write(req, size);
    }
}

/*
 * ptm_write: low-level write() interface; calls approriate function depending
 *            on what is being transferred using the write()
 */
static void ptm_write(fuse_req_t req, const char *buf, size_t size,
                      off_t off, struct fuse_file_info *fi)
{
    switch (tx_state.state) {
    case TX_STATE_RW_COMMAND:
        ptm_write_cmd(req, buf, size);
        break;
    case TX_STATE_GET_STATE_BLOB:
        fuse_reply_err(req, EIO);
        tx_state.state = TX_STATE_RW_COMMAND;
        break;
    case TX_STATE_SET_STATE_BLOB:
        ptm_write_stateblob(req, buf, size);
        break;
    }
}

static stateblob_desc cached_stateblob;

static bool
cached_stateblob_is_loaded(uint32_t blobtype, TPM_BOOL decrypt)
{
    return (cached_stateblob.data != NULL) &&
           (cached_stateblob.blobtype == blobtype) &&
           (cached_stateblob.decrypt == decrypt);
}

/*
 * cached_stateblob_free: Free any previously loaded state blob
 */
static void
cached_stateblob_free(void)
{
    TPM_Free(cached_stateblob.data);
    cached_stateblob.data = NULL;
    cached_stateblob.data_length = 0;
}

/*
 * cached_stateblob_get_bloblength: get the total length of the cached blob
 */
static uint32_t
cached_stateblob_get_bloblength(void)
{
    return cached_stateblob.data_length;
}

/*
 * cached_statblob_get: get stateblob data without copying them
 *
 * @offset: at which offset to get the data
 * @bufptr: pointer to a buffer pointer used to return buffer start
 * @length: pointer used to return number of available bytes in returned buffer
 */
static int
cached_stateblob_get(uint32_t offset,
                     unsigned char **bufptr, size_t *length)
{
    if (cached_stateblob.data == NULL ||
        offset > cached_stateblob.data_length)
        return -1;

    *bufptr = &cached_stateblob.data[offset];
    *length = cached_stateblob.data_length - offset;

    return 0;
}

/*
 * cached_stateblob_load: load a state blob into the cache
 *
 * blobtype: the type of blob
 * decrypt: whether the blob is to be decrypted
 */
static TPM_RESULT
cached_stateblob_load(uint32_t blobtype, TPM_BOOL decrypt)
{
    TPM_RESULT res = 0;
    const char *blobname = ptm_get_blobname(blobtype);
    uint32_t tpm_number = 0;

    if (!blobname)
        return TPM_BAD_PARAMETER;

    cached_stateblob_free();

    if (blobtype == PTM_BLOB_TYPE_VOLATILE)
        res = SWTPM_NVRAM_Store_Volatile();

    if (res == 0)
        res = SWTPM_NVRAM_GetStateBlob(&cached_stateblob.data,
                                       &cached_stateblob.data_length,
                                       tpm_number, blobname, decrypt,
                                       &cached_stateblob.is_encrypted);

    /* make sure the volatile state file is gone */
    if (blobtype == PTM_BLOB_TYPE_VOLATILE)
        SWTPM_NVRAM_DeleteName(tpm_number, blobname, FALSE);

    if (res == 0) {
        cached_stateblob.blobtype = blobtype;
        cached_stateblob.decrypt = decrypt;
    }

    return res;
}

/*
 * cached_state_blob_copy: copy the cached state blob to a destination buffer
 *
 * dest: destination buffer
 * destlen: size of the buffer
 * srcoffset: offset to copy from
 * copied: variable to return the number of copied bytes
 * is_encrypted: variable to return whether the blob is encrypted
 */
static int
cached_stateblob_copy(void *dest, size_t destlen, uint32_t srcoffset,
                      uint32_t *copied, TPM_BOOL *is_encrypted)
{
    int ret = -1;

    *copied = 0;

    if (cached_stateblob.data != NULL && cached_stateblob.data_length > 0) {

        if (srcoffset < cached_stateblob.data_length) {
            *copied = min(cached_stateblob.data_length - srcoffset, destlen);

            memcpy(dest, &cached_stateblob.data[srcoffset], *copied);

            *is_encrypted = cached_stateblob.is_encrypted;
        }

        ret = 0;
    }

    return ret;
}

/*
 * ptm_get_stateblob_part: get part of a state blob
 */
static TPM_RESULT
ptm_get_stateblob_part(uint32_t blobtype,
                       unsigned char *buffer, size_t buffer_size,
                       uint32_t offset, uint32_t *copied,
                       TPM_BOOL decrypt, TPM_BOOL *is_encrypted)
{
    TPM_RESULT res = 0;

    if (!cached_stateblob_is_loaded(blobtype, decrypt)) {
        res = cached_stateblob_load(blobtype, decrypt);
    }

    if (res == 0) {
        cached_stateblob_copy(buffer, buffer_size,
                              offset, copied, is_encrypted);
    }

    return res;
}

/*
 * ptm_get_stateblob: Get the state blob from the TPM using ioctl()
 */
static void
ptm_get_stateblob(fuse_req_t req, ptm_getstate *pgs)
{
    TPM_RESULT res = 0;
    uint32_t blobtype = pgs->u.req.type;
    TPM_BOOL decrypt = ((pgs->u.req.state_flags & STATE_FLAG_DECRYPTED) != 0);
    TPM_BOOL is_encrypted = FALSE;
    uint32_t copied = 0;
    uint32_t offset = pgs->u.req.offset;
    uint32_t totlength;

    res = ptm_get_stateblob_part(blobtype,
                                 pgs->u.resp.data, sizeof(pgs->u.resp.data),
                                 pgs->u.req.offset, &copied,
                                 decrypt, &is_encrypted);

    totlength = cached_stateblob_get_bloblength();

    pgs->u.resp.state_flags = 0;
    if (is_encrypted)
        pgs->u.resp.state_flags |= STATE_FLAG_ENCRYPTED;

    pgs->u.resp.length = copied;
    pgs->u.resp.totlength = totlength;
    pgs->u.resp.tpm_result = res;

    if (res == 0) {
        if (offset + copied < totlength) {
            /* last byte was not copied */
            tx_state.state = TX_STATE_GET_STATE_BLOB;
            tx_state.blobtype = pgs->u.req.type;
            tx_state.blob_is_encrypted = is_encrypted;
            tx_state.offset = copied;
        } else {
            /* last byte was copied */
            tx_state.state = TX_STATE_RW_COMMAND;
        }
    } else {
        /* error occurred */
        tx_state.state = TX_STATE_RW_COMMAND;
    }

    fuse_reply_ioctl(req, 0, pgs, sizeof(pgs->u.resp));
}

/*
 * ptm_set_stateblob_append: Append a piece of TPM state blob and transfer to TPM
 *
 * blobtype: the type of blob
 * data: the data to append
 * length: length of the data
 * is_encrypted: whether the blob is encrypted
 * is_last: whether this is the last part of the TPM state blob; if it is, the TPM
 *          state blob will then be transferred to the TPM
 */
static TPM_RESULT
ptm_set_stateblob_append(uint32_t blobtype,
                         const unsigned char *data, uint32_t length,
                         bool is_encrypted, bool is_last)
{
    const char *blobname;
    TPM_RESULT res = 0;
    static struct stateblob stateblob;

    if (stateblob.type != blobtype) {
        /* clear old data */
        TPM_Free(stateblob.data);
        stateblob.data = NULL;
        stateblob.length = 0;
        stateblob.type = blobtype;
        stateblob.is_encrypted = is_encrypted;

        /*
         * on the first call for a new state blob we allow 0 bytes to be written
         * this allows the user to transfer via write()
         */
        if (length == 0)
            return 0;
    }

    /* append */
    res = TPM_Realloc(&stateblob.data, stateblob.length + length);
    if (res != 0) {
        /* error */
        TPM_Free(stateblob.data);
        stateblob.data = NULL;
        stateblob.length = 0;
        stateblob.type = 0;

        return res;
    }

    memcpy(&stateblob.data[stateblob.length], data, length);
    stateblob.length += length;

    if (!is_last) {
        /* full packet -- expecting more data */
        return res;
    }
    blobname = ptm_get_blobname(blobtype);

    if (blobname) {
        res = SWTPM_NVRAM_SetStateBlob(stateblob.data,
                                       stateblob.length,
                                       stateblob.is_encrypted,
                                       0 /* tpm_number */,
                                       blobname);
    } else {
        res = TPM_BAD_PARAMETER;
    }

    TPM_Free(stateblob.data);
    stateblob.data = NULL;
    stateblob.length = 0;
    stateblob.type = 0;

    /* transfer of blob is complete */
    tx_state.state = TX_STATE_RW_COMMAND;

    return res;
}

static void
ptm_set_stateblob(fuse_req_t req, ptm_setstate *pss)
{
    TPM_RESULT res = 0;
    TPM_BOOL is_encrypted = ((pss->u.req.state_flags & STATE_FLAG_ENCRYPTED) != 0);
    bool is_last = (sizeof(pss->u.req.data) != pss->u.req.length);

    if (pss->u.req.length > sizeof(pss->u.req.data)) {
        res = TPM_BAD_PARAMETER;
        goto send_response;
    }

    /* transfer of blob initiated */
    tx_state.state = TX_STATE_SET_STATE_BLOB;
    tx_state.blobtype = pss->u.req.type;
    tx_state.blob_is_encrypted = is_encrypted;
    tx_state.offset = 0;

    res = ptm_set_stateblob_append(pss->u.req.type,
                                   pss->u.req.data,
                                   pss->u.req.length,
                                   is_encrypted,
                                   is_last);

    if (res)
        tx_state.state = TX_STATE_RW_COMMAND;

 send_response:
    pss->u.resp.tpm_result = res;

    fuse_reply_ioctl(req, 0, pss, sizeof(*pss));
}

/*
 * ptm_ioctl : ioctl execution
 *
 * req: the fuse_req_t used to send response with
 * cmd: the ioctl request code
 * arg: the pointer the application used for calling the ioctl (3rd param)
 * fi:
 * flags: some flags provided by fuse
 * in_buf: the copy of the input buffer
 * in_bufsz: size of the input buffer; provided by fuse and has size of
 *           needed buffer
 * out_bufsz: size of the output buffer; provided by fuse and has size of
 *            needed buffer
 */
static void ptm_ioctl(fuse_req_t req, int cmd, void *arg,
                      struct fuse_file_info *fi, unsigned flags,
                      const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
    TPM_RESULT res;
    bool exit_prg = FALSE;
    ptm_init *init_p;

    if (flags & FUSE_IOCTL_COMPAT) {
        fuse_reply_err(req, ENOSYS);
        return;
    }

    /* some commands have to wait until the worker thread is done */
    switch(cmd) {
    case PTM_GET_CAPABILITY:
    case PTM_SET_LOCALITY:
    case PTM_CANCEL_TPM_CMD:
    case PTM_GET_CONFIG:
        /* no need to wait */
        break;
    case PTM_INIT:
    case PTM_SHUTDOWN:
    case PTM_GET_TPMESTABLISHED:
    case PTM_RESET_TPMESTABLISHED:
    case PTM_HASH_START:
    case PTM_HASH_DATA:
    case PTM_HASH_END:
    case PTM_STORE_VOLATILE:
    case PTM_GET_STATEBLOB:
    case PTM_SET_STATEBLOB:
        if (tpm_running)
            worker_thread_wait_done();
        break;
    }

    /* prevent other threads from writing or doing ioctls */
    g_mutex_lock(FILE_OPS_LOCK);

    switch (cmd) {
    case PTM_GET_CAPABILITY:
        if (!out_bufsz) {
            struct iovec iov = { arg, sizeof(uint8_t) };
            fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
        } else {
            ptm_cap ptm_caps;
            ptm_caps = PTM_CAP_INIT | PTM_CAP_SHUTDOWN
                | PTM_CAP_GET_TPMESTABLISHED
                | PTM_CAP_SET_LOCALITY
                | PTM_CAP_HASHING 
                | PTM_CAP_CANCEL_TPM_CMD
                | PTM_CAP_STORE_VOLATILE
                | PTM_CAP_RESET_TPMESTABLISHED
                | PTM_CAP_GET_STATEBLOB
                | PTM_CAP_SET_STATEBLOB
                | PTM_CAP_STOP
                | PTM_CAP_GET_CONFIG;
            fuse_reply_ioctl(req, 0, &ptm_caps, sizeof(ptm_caps));
        }
        break;

    case PTM_INIT:
        init_p = (ptm_init *)in_buf;

        worker_thread_end();

        TPMLIB_Terminate();

        tpm_running = 0;
        if ((res = tpm_start(init_p->u.req.init_flags))) {
            logprintf(STDERR_FILENO,
                      "Error: Could not initialize the TPM.\n");
        } else {
            tpm_running = 1;
        }
        fuse_reply_ioctl(req, 0, &res, sizeof(res));
        break;

    case PTM_STOP:
        worker_thread_end();

        res = TPM_SUCCESS;
        TPMLIB_Terminate();

        tpm_running = 0;

        TPM_Free(ptm_response);
        ptm_response = NULL;

        fuse_reply_ioctl(req, 0, &res, sizeof(res));

        break;

    case PTM_SHUTDOWN:
        worker_thread_end();

        res = TPM_SUCCESS;
        TPMLIB_Terminate();

        TPM_Free(ptm_response);
        ptm_response = NULL;

        fuse_reply_ioctl(req, 0, &res, sizeof(res));
        exit_prg = TRUE;

        break;

    case PTM_GET_TPMESTABLISHED:
        if (!tpm_running)
            goto error_not_running;

        if (!out_bufsz) {
            struct iovec iov = { arg, sizeof(uint8_t) };
            fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
        } else {
            ptm_est te;
            te.tpm_result = TPM_IO_TpmEstablished_Get(&te.bit);
            fuse_reply_ioctl(req, 0, &te, sizeof(te));
        }
        break;

    case PTM_RESET_TPMESTABLISHED:
        if (!tpm_running)
            goto error_not_running;

        if (!in_bufsz) {
            struct iovec iov = { arg, sizeof(uint32_t) };
            fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
        } else {
            ptm_reset_est *re = (ptm_reset_est *)in_buf;
            if (re->u.req.loc > 4) {
                res = TPM_BAD_LOCALITY;
            } else {
                res = _TPM_IO_TpmEstablished_Reset(req, re->u.req.loc);
                fuse_reply_ioctl(req, 0, &res, sizeof(res));
            }
        }
        break;

    case PTM_SET_LOCALITY:
        if (!in_bufsz) {
            struct iovec iov = { arg, sizeof(uint32_t) };
            fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
        } else {
            ptm_loc *l = (ptm_loc *)in_buf;
            if (l->u.req.loc > 4) {
                res = TPM_BAD_LOCALITY;
            } else {
                res = 0;
                locality = l->u.req.loc;
            }
            fuse_reply_ioctl(req, 0, &res, sizeof(res));
        }
        break;

    case PTM_HASH_START:
        if (!tpm_running)
            goto error_not_running;

        res = TPM_IO_Hash_Start();
        fuse_reply_ioctl(req, 0, &res, sizeof(res));
        break;

    case PTM_HASH_DATA:
        if (!tpm_running)
            goto error_not_running;

        if (!in_bufsz) {
            struct iovec iov = { arg, sizeof(uint32_t) };
            fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
        } else {
            ptm_hdata *data = (ptm_hdata *)in_buf;
            if (data->u.req.length <= sizeof(data->u.req.data)) {
                res = TPM_IO_Hash_Data(data->u.req.data,
                                       data->u.req.length);
            } else {
                res = TPM_FAIL;
            }
            fuse_reply_ioctl(req, 0, &res, sizeof(res));
        }
        break;

    case PTM_HASH_END:
        if (!tpm_running)
            goto error_not_running;

        res = TPM_IO_Hash_End();
        fuse_reply_ioctl(req, 0, &res, sizeof(res));
        break;

    case PTM_CANCEL_TPM_CMD:
        if (!tpm_running)
            goto error_not_running;

        /* for cancellation to work, the TPM would have to
         * execute in another thread that polls on a cancel
         * flag
         */
        res = TPM_FAIL;
        fuse_reply_ioctl(req, 0, &res, sizeof(res));
        break;

    case PTM_STORE_VOLATILE:
        if (!tpm_running)
            goto error_not_running;

        res = SWTPM_NVRAM_Store_Volatile();
        fuse_reply_ioctl(req, 0, &res, sizeof(res));

        cached_stateblob_free();
        break;

    case PTM_GET_STATEBLOB:
        if (!tpm_running)
            goto error_not_running;

        if (in_bufsz != sizeof(ptm_getstate)) {
            struct iovec iov = { arg, sizeof(uint32_t) };
            fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
        } else {
            ptm_get_stateblob(req, (ptm_getstate *)in_buf);
        }
        break;

    case PTM_SET_STATEBLOB:
        if (tpm_running)
            goto error_running;

        /* tpm state dir must be set */
        SWTPM_NVRAM_Init();

        if (in_bufsz != sizeof(ptm_setstate)) {
            struct iovec iov = { arg, sizeof(uint32_t) };
            fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
        } else {
            ptm_set_stateblob(req, (ptm_setstate *)in_buf);
        }
        break;

    case PTM_GET_CONFIG:
        if (out_bufsz != sizeof(ptm_getconfig)) {
            struct iovec iov = { arg, sizeof(uint32_t) };
            fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
        } else {
            ptm_getconfig pgs;
            pgs.u.resp.tpm_result = 0;
            pgs.u.resp.flags = 0;
            if (SWTPM_NVRAM_Has_FileKey())
                pgs.u.resp.flags |= CONFIG_FLAG_FILE_KEY;
            if (SWTPM_NVRAM_Has_MigrationKey())
                pgs.u.resp.flags |= CONFIG_FLAG_MIGRATION_KEY;
            fuse_reply_ioctl(req, 0, &pgs, sizeof(pgs));
        }
        break;

    default:
        fuse_reply_err(req, EINVAL);
    }

cleanup:
    g_mutex_unlock(FILE_OPS_LOCK);

    if (exit_prg) {
        logprintf(STDOUT_FILENO,
                  "CUSE TPM is shutting down.\n");
        exit(0);
    }

    return;

error_running:
error_not_running:
    res = TPM_BAD_ORDINAL;
    fuse_reply_ioctl(req, 0, &res, sizeof(res));

    goto cleanup;
}

static void ptm_init_done(void *userdata) {
    if (passwd) {
        if (initgroups(passwd->pw_name, passwd->pw_gid) < 0) {
            logprintf(STDERR_FILENO,
                      "Error: initgroups(%s, %d) failed.\n",
                  passwd->pw_name, passwd->pw_gid);
            exit(-10);
        }
        if (setgid(passwd->pw_gid) < 0) {
            logprintf(STDERR_FILENO,
                      "Error: setgid(%d) failed.\n",
                      passwd->pw_gid);
            exit(-11);
        }
        if (setuid(passwd->pw_uid) < 0) {
            logprintf(STDERR_FILENO,
                      "Error: setuid(%d) failed.\n",
                      passwd->pw_uid);
            exit(-12);
        }
    }
}

static const struct cuse_lowlevel_ops ptm_clop = {
    .open      = ptm_open,
    .read      = ptm_read,
    .write     = ptm_write,
    .ioctl     = ptm_ioctl,
    .init_done = ptm_init_done,
};

#define PTM_OPT(t, p) { t, offsetof(struct ptm_param, p), 1 }

static const struct fuse_opt ptm_opts[] = {
    PTM_OPT("-M %u",      major),
    PTM_OPT("--maj=%u",   major),
    PTM_OPT("-m %u",      minor),
    PTM_OPT("--min=%u",   minor),
    PTM_OPT("-n %s",      dev_name),
    PTM_OPT("--name=%s",  dev_name),
    PTM_OPT("-r %s",      runas),
    PTM_OPT("--runas=%s", runas),
    PTM_OPT("--log %s",   logging),
    PTM_OPT("--key %s",   keydata),
    PTM_OPT("--migration-key %s",   migkeydata),
    FUSE_OPT_KEY("-h",        0),
    FUSE_OPT_KEY("--help",    0),
    FUSE_OPT_KEY("-v",        1),
    FUSE_OPT_KEY("--version", 1),
    FUSE_OPT_END
};

static int ptm_process_arg(void *data, const char *arg, int key,
                           struct fuse_args *outargs)
{
    struct ptm_param *param = data;

    switch (key) {
    case 0:
        param->is_help = 1;
        fprintf(stdout, usage, param->prgname);
        return fuse_opt_add_arg(outargs, "-ho");
    case 1:
        param->is_help = 1;
        fprintf(stdout, "TPM emulator CUSE interface version %d.%d.%d, "
                "Copyright (c) 2014 IBM Corp.\n",
                SWTPM_VER_MAJOR,
                SWTPM_VER_MINOR,
                SWTPM_VER_MICRO);
        return 0;
    default:
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct ptm_param param = {
        .major = 0,
        .minor = 0,
        .dev_name = NULL,
        .is_help = 0,
        .prgname = argv[0],
        .runas = NULL,
        .logging = NULL,
        .keydata = NULL,
        .migkeydata = NULL,
    };
    char dev_name[128] = "DEVNAME=";
    const char *dev_info_argv[] = { dev_name };
    struct cuse_info ci;
    int ret;

    if ((ret = fuse_opt_parse(&args, &param, ptm_opts, ptm_process_arg))) {
        fprintf(stderr, "Error: Could not parse option\n");
        return ret;
    }

    if (!param.is_help) {
        if (!param.dev_name) {
            fprintf(stderr, "Error: device name missing\n");
            return -2;
        }
        strncat(dev_name, param.dev_name, sizeof(dev_name) - 9);
    } else {
        return 0;
    }

    if (handle_log_options(param.logging) < 0 ||
        handle_key_options(param.keydata) < 0 ||
        handle_migration_key_options(param.migkeydata) < 0)
        return -3;

    if (setuid(0)) {
        fprintf(stderr, "Error: Unable to setuid root. uid = %d, "
                "euid = %d, gid = %d\n", getuid(), geteuid(), getgid());
        return -4;
    }

    if (param.runas) {
        if (!(passwd = getpwnam(param.runas))) {
            fprintf(stderr, "User '%s' does not exist\n",
                    param.runas);
            return -5;
        }
    }

    memset(&ci, 0, sizeof(ci));
    ci.dev_major = param.major;
    ci.dev_minor = param.minor;
    ci.dev_info_argc = 1;
    ci.dev_info_argv = dev_info_argv;

#if GLIB_MINOR_VERSION >= 32
    g_mutex_init(THREAD_BUSY_LOCK);
    g_cond_init(THREAD_BUSY_SIGNAL);
    g_mutex_init(FILE_OPS_LOCK);
#else
    g_thread_init(NULL);
    THREAD_BUSY_LOCK = g_mutex_new();
    THREAD_BUSY_SIGNAL = g_cond_new();
    FILE_OPS_LOCK = g_mutex_new();
#endif

    return cuse_lowlevel_main(args.argc, args.argv, &ci, &ptm_clop,
                              &param);
}
