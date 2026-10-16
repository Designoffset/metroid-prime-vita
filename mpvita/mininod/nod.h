#ifndef MP_ANDROID_PROBE_NOD_H
#define MP_ANDROID_PROBE_NOD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NOD_PARTITION_KIND_DATA 0
#define NOD_FST_STOP UINT32_MAX

typedef enum NodResult {
    NOD_RESULT_OK,
    NOD_RESULT_ERR_IO,
    NOD_RESULT_ERR_FORMAT,
    NOD_RESULT_ERR_NOT_FOUND,
    NOD_RESULT_ERR_INVALID_HANDLE,
    NOD_RESULT_ERR_OTHER,
} NodResult;

typedef enum NodNodeKind {
    NOD_NODE_KIND_FILE,
    NOD_NODE_KIND_DIRECTORY,
} NodNodeKind;

typedef struct NodHandle NodHandle;

typedef struct NodDiscOptions {
    int partition_encryption;
    uint32_t preloader_threads;
} NodDiscOptions;

typedef int64_t (*NodDiscStreamReadAtCallback)(void*, uint64_t, void*, size_t);
typedef int64_t (*NodDiscStreamLenCallback)(void*);
typedef void (*NodDiscStreamCloseCallback)(void*);

typedef struct NodDiscStream {
    void* user_data;
    NodDiscStreamReadAtCallback read_at;
    NodDiscStreamLenCallback stream_len;
    NodDiscStreamCloseCallback close;
} NodDiscStream;

typedef struct NodPartitionOptions {
    bool validate_hashes;
} NodPartitionOptions;

typedef struct NodDiscHeader {
    char game_id[6];
    uint8_t disc_num;
    uint8_t disc_version;
    uint8_t audio_streaming;
    uint8_t audio_stream_buf_size;
    uint8_t reserved[86];
} NodDiscHeader;

typedef struct NodBlob {
    const uint8_t* data;
    size_t size;
} NodBlob;

typedef struct NodPartitionMeta {
    NodBlob raw_boot;
    NodBlob raw_bi2;
    NodBlob raw_apploader;
    NodBlob raw_dol;
    NodBlob raw_fst;
    NodBlob raw_ticket;
    NodBlob raw_tmd;
    NodBlob raw_cert_chain;
    NodBlob raw_h3_table;
} NodPartitionMeta;

typedef uint32_t (*NodFstCallback)(uint32_t, NodNodeKind, const char*, uint32_t, void*);

#ifdef __cplusplus
extern "C" {
#endif

NodResult nod_disc_open_stream(const NodDiscStream* stream, const NodDiscOptions* options, NodHandle** out);
NodResult nod_disc_open_partition_kind(NodHandle* disc, uint32_t kind,
                                       const NodPartitionOptions* options, NodHandle** out);
NodResult nod_partition_open_file(NodHandle* partition, uint32_t fst_index, NodHandle** out);
void nod_free(NodHandle* handle);
int64_t nod_read(NodHandle* handle, uint8_t* buf, size_t len);
int64_t nod_seek(NodHandle* handle, int64_t offset, int32_t whence);
NodResult nod_disc_header(const NodHandle* disc, NodDiscHeader* out);
NodResult nod_partition_meta(const NodHandle* partition, NodPartitionMeta* out);
void nod_partition_iterate_fst(const NodHandle* partition, NodFstCallback callback, void* user_data);
bool nod_partition_is_wii(const NodHandle* partition);

#ifdef __cplusplus
}
#endif

#endif
