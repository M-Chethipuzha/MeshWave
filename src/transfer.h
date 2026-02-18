/* transfer.h
 * Chunked file transfer with pause, resume, and per-chunk retry.
 */

#ifndef TRANSFER_H
#define TRANSFER_H

#include "protocol.h"

#define MAX_TRANSFERS 16

typedef void (*TransferEventCb)(int xfer_id, XferState state,
                                 uint32_t done, uint32_t total);

#ifdef __cplusplus
extern "C" {
#endif

void transfer_init(TransferEventCb cb);

int  transfer_send_file(int sock_fd, const char *filepath,
                        const char *peer_name);

int  transfer_recv_meta(int xfer_id, const char *sender,
                        const char *filename, uint32_t total_chunks,
                        uint64_t file_size, const char *save_dir);

int  transfer_recv_chunk(int xfer_id, uint32_t chunk_seq,
                         const uint8_t *data, int data_len);

int  transfer_pause(int xfer_id);
int  transfer_resume(int xfer_id);

int  transfer_get_all(Transfer *out, int max);
Transfer *transfer_find(int xfer_id);

int  transfer_next_id(void);

#ifdef __cplusplus
}
#endif

#endif /* TRANSFER_H */
