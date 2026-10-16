/*
 * mininod: a small C replacement for the parts of the nod (Rust) disc library
 * that Aurora uses. Supports GameCube plain images (.iso/.gcm) and CISO
 * (.ciso) images. MIT licensed, written for the Metroid Prime Vita port.
 */
#include "nod.h"
#include "mp_vita_prof.h"

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif
#include <stdlib.h>
#include <string.h>

enum HandleKind { H_DISC, H_PARTITION, H_FILE };

#define CISO_MAP_SIZE (0x8000 - 8)

typedef struct Disc {
  NodDiscStream stream;
  uint64_t len;
  /* CISO */
  int is_ciso;
  uint32_t block_size;
  uint32_t* block_offset; /* per logical block: index into packed data, or UINT32_MAX if absent */
  uint32_t nblocks;
  uint8_t header[0x440];
} Disc;

typedef struct Partition {
  Disc* disc;
  uint8_t* boot;      /* 0x440 */
  uint8_t* bi2;       /* 0x2000 */
  uint8_t* apploader; size_t apploader_size;
  uint8_t* dol;       size_t dol_size;
  uint8_t* fst;       size_t fst_size;
  uint32_t nentries;
} Partition;

typedef struct File {
  Disc* disc;
  uint64_t base, size, pos;
} File;

struct NodHandle {
  int kind;
  int owns; /* disc handle owns the Disc */
  union { Disc* disc; Partition* part; File* file; } u;
};

static uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t le32(const uint8_t* p) {
  return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

/* Read `len` bytes at logical disc offset `off`. Returns bytes read or -1. */
static int64_t disc_read_impl(Disc* d, uint64_t off, void* buf, size_t len);
/* The stream's read_at seeks and then reads one shared file position, so two
 * threads reading at once (Aurora's DVD worker, the game's synchronous loads, the
 * movie streams) could read each other's offsets: reads take turns. A kernel
 * lightweight mutex works from any thread, pthread or not. */
#ifdef __vita__
static SceKernelLwMutexWork g_readLock;
static int g_readLockInit;
static void read_lock(void) {
  if (!g_readLockInit) {
    /* First use is during disc open, before any other thread reads. */
    sceKernelCreateLwMutex(&g_readLock, "mininod_read", 0, 0, NULL);
    g_readLockInit = 1;
  }
  sceKernelLockLwMutex(&g_readLock, 1, NULL);
}
static void read_unlock(void) { sceKernelUnlockLwMutex(&g_readLock, 1); }
#else
static void read_lock(void) {}
static void read_unlock(void) {}
#endif
static int64_t disc_read(Disc* d, uint64_t off, void* buf, size_t len) {
  MP_PROF_BEGIN(MP_PROF_DISC_READ);
  read_lock();
  const int64_t r = disc_read_impl(d, off, buf, len);
  read_unlock();
  MP_PROF_END(MP_PROF_DISC_READ);
  return r;
}
static int64_t disc_read_impl(Disc* d, uint64_t off, void* buf, size_t len) {
  if (!d->is_ciso) {
    if (off >= d->len) return 0;
    if (off + len > d->len) len = (size_t)(d->len - off);
    return d->stream.read_at(d->stream.user_data, off, buf, len);
  }
  uint8_t* out = (uint8_t*)buf;
  size_t done = 0;
  while (done < len) {
    uint64_t lo = off + done;
    uint32_t blk = (uint32_t)(lo / d->block_size);
    uint32_t in = (uint32_t)(lo % d->block_size);
    size_t n = d->block_size - in;
    if (n > len - done) n = len - done;
    if (blk >= d->nblocks) break;
    if (d->block_offset[blk] == UINT32_MAX) {
      memset(out + done, 0, n);
    } else {
      uint64_t phys = 0x8000 + (uint64_t)d->block_offset[blk] * d->block_size + in;
      int64_t r = d->stream.read_at(d->stream.user_data, phys, out + done, n);
      if (r < 0) return -1;
      if ((size_t)r < n) { done += (size_t)r; break; }
    }
    done += n;
  }
  return (int64_t)done;
}

static int read_exact(Disc* d, uint64_t off, void* buf, size_t len) {
  return disc_read(d, off, buf, len) == (int64_t)len;
}

static NodHandle* new_handle(int kind) {
  NodHandle* h = (NodHandle*)calloc(1, sizeof(NodHandle));
  if (h) h->kind = kind;
  return h;
}

NodResult nod_disc_open_stream(const NodDiscStream* stream, const NodDiscOptions* options, NodHandle** out) {
  (void)options;
  if (!stream || !out) return NOD_RESULT_ERR_OTHER;
  *out = NULL;
  Disc* d = (Disc*)calloc(1, sizeof(Disc));
  if (!d) return NOD_RESULT_ERR_OTHER;
  d->stream = *stream;
  d->len = stream->stream_len ? (uint64_t)stream->stream_len(stream->user_data) : 0;

  uint8_t magic[8];
  if (stream->read_at(stream->user_data, 0, magic, 8) != 8) { free(d); return NOD_RESULT_ERR_IO; }
  if (memcmp(magic, "CISO", 4) == 0) {
    uint8_t* map = (uint8_t*)malloc(CISO_MAP_SIZE);
    if (!map) { free(d); return NOD_RESULT_ERR_OTHER; }
    if (stream->read_at(stream->user_data, 8, map, CISO_MAP_SIZE) != CISO_MAP_SIZE) { free(map); free(d); return NOD_RESULT_ERR_IO; }
    d->is_ciso = 1;
    d->block_size = le32(magic + 4);
    if (d->block_size == 0 || (d->block_size & (d->block_size - 1))) { free(map); free(d); return NOD_RESULT_ERR_FORMAT; }
    d->nblocks = CISO_MAP_SIZE;
    d->block_offset = (uint32_t*)malloc(sizeof(uint32_t) * d->nblocks);
    uint32_t packed = 0, last = 0;
    for (uint32_t i = 0; i < d->nblocks; i++) {
      if (map[i]) { d->block_offset[i] = packed++; last = i + 1; }
      else d->block_offset[i] = UINT32_MAX;
    }
    d->nblocks = last;
    d->len = (uint64_t)last * d->block_size;
    free(map);
  }
  if (!read_exact(d, 0, d->header, sizeof(d->header))) { free(d->block_offset); free(d); return NOD_RESULT_ERR_IO; }
  /* GameCube magic at 0x1C */
  if (be32(d->header + 0x1C) != 0xC2339F3D) { free(d->block_offset); free(d); return NOD_RESULT_ERR_FORMAT; }

  NodHandle* h = new_handle(H_DISC);
  h->owns = 1;
  h->u.disc = d;
  *out = h;
  return NOD_RESULT_OK;
}

static uint8_t* read_blob(Disc* d, uint64_t off, size_t size) {
  uint8_t* p = (uint8_t*)malloc(size ? size : 1);
  if (p && size && !read_exact(d, off, p, size)) { free(p); return NULL; }
  return p;
}

static size_t dol_size_of(const uint8_t* hdr) {
  size_t end = 0x100;
  for (int i = 0; i < 18; i++) {
    size_t e = (size_t)be32(hdr + i * 4) + be32(hdr + 0x90 + i * 4);
    if (be32(hdr + 0x90 + i * 4) && e > end) end = e;
  }
  return end;
}

NodResult nod_disc_open_partition_kind(NodHandle* disc, uint32_t kind, const NodPartitionOptions* options, NodHandle** out) {
  (void)options;
  if (!disc || disc->kind != H_DISC || !out || kind != NOD_PARTITION_KIND_DATA) return NOD_RESULT_ERR_INVALID_HANDLE;
  Disc* d = disc->u.disc;
  Partition* p = (Partition*)calloc(1, sizeof(Partition));
  p->disc = d;
  p->boot = read_blob(d, 0, 0x440);
  p->bi2 = read_blob(d, 0x440, 0x2000);
  uint8_t aphdr[0x20];
  if (!read_exact(d, 0x2440, aphdr, sizeof aphdr)) goto fail;
  p->apploader_size = 0x20 + (size_t)be32(aphdr + 0x14) + be32(aphdr + 0x18);
  p->apploader = read_blob(d, 0x2440, p->apploader_size);
  {
    uint32_t dol_off = be32(d->header + 0x420);
    uint8_t dolhdr[0x100];
    if (!read_exact(d, dol_off, dolhdr, sizeof dolhdr)) goto fail;
    p->dol_size = dol_size_of(dolhdr);
    p->dol = read_blob(d, dol_off, p->dol_size);
  }
  p->fst_size = be32(d->header + 0x428);
  p->fst = read_blob(d, be32(d->header + 0x424), p->fst_size);
  if (!p->boot || !p->bi2 || !p->apploader || !p->dol || !p->fst || p->fst_size < 12) goto fail;
  p->nentries = be32(p->fst + 8);
  if ((uint64_t)p->nentries * 12 > p->fst_size) goto fail;
  {
    NodHandle* h = new_handle(H_PARTITION);
    h->u.part = p;
    *out = h;
  }
  return NOD_RESULT_OK;
fail:
  free(p->boot); free(p->bi2); free(p->apploader); free(p->dol); free(p->fst); free(p);
  return NOD_RESULT_ERR_FORMAT;
}

NodResult nod_partition_open_file(NodHandle* partition, uint32_t fst_index, NodHandle** out) {
  if (!partition || partition->kind != H_PARTITION || !out) return NOD_RESULT_ERR_INVALID_HANDLE;
  Partition* p = partition->u.part;
  if (fst_index == 0 || fst_index >= p->nentries) return NOD_RESULT_ERR_NOT_FOUND;
  const uint8_t* e = p->fst + (size_t)fst_index * 12;
  if (e[0] != 0) return NOD_RESULT_ERR_NOT_FOUND; /* directory */
  File* f = (File*)calloc(1, sizeof(File));
  f->disc = p->disc;
  f->base = be32(e + 4);
  f->size = be32(e + 8);
  NodHandle* h = new_handle(H_FILE);
  h->u.file = f;
  *out = h;
  return NOD_RESULT_OK;
}

void nod_free(NodHandle* h) {
  if (!h) return;
  switch (h->kind) {
  case H_DISC:
    if (h->owns && h->u.disc) {
      Disc* d = h->u.disc;
      if (d->stream.close) d->stream.close(d->stream.user_data);
      free(d->block_offset);
      free(d);
    }
    break;
  case H_PARTITION: {
    Partition* p = h->u.part;
    free(p->boot); free(p->bi2); free(p->apploader); free(p->dol); free(p->fst); free(p);
    break;
  }
  case H_FILE: free(h->u.file); break;
  }
  free(h);
}

int64_t nod_read(NodHandle* h, uint8_t* buf, size_t len) {
  if (!h || h->kind != H_FILE) return -1;
  File* f = h->u.file;
  if (f->pos >= f->size) return 0;
  if (len > f->size - f->pos) len = (size_t)(f->size - f->pos);
  int64_t r = disc_read(f->disc, f->base + f->pos, buf, len);
  if (r > 0) f->pos += (uint64_t)r;
  return r;
}

int64_t nod_seek(NodHandle* h, int64_t offset, int32_t whence) {
  if (!h || h->kind != H_FILE) return -1;
  File* f = h->u.file;
  int64_t np;
  switch (whence) {
  case 0: np = offset; break;
  case 1: np = (int64_t)f->pos + offset; break;
  case 2: np = (int64_t)f->size + offset; break;
  default: return -1;
  }
  if (np < 0) return -1;
  f->pos = (uint64_t)np;
  return np;
}

NodResult nod_disc_header(const NodHandle* disc, NodDiscHeader* out) {
  if (!disc || disc->kind != H_DISC || !out) return NOD_RESULT_ERR_INVALID_HANDLE;
  const uint8_t* hd = disc->u.disc->header;
  memset(out, 0, sizeof *out);
  memcpy(out->game_id, hd, 6);
  out->disc_num = hd[6];
  out->disc_version = hd[7];
  out->audio_streaming = hd[8];
  out->audio_stream_buf_size = hd[9];
  return NOD_RESULT_OK;
}

NodResult nod_partition_meta(const NodHandle* partition, NodPartitionMeta* out) {
  if (!partition || partition->kind != H_PARTITION || !out) return NOD_RESULT_ERR_INVALID_HANDLE;
  const Partition* p = partition->u.part;
  memset(out, 0, sizeof *out);
  out->raw_boot = (NodBlob){p->boot, 0x440};
  out->raw_bi2 = (NodBlob){p->bi2, 0x2000};
  out->raw_apploader = (NodBlob){p->apploader, p->apploader_size};
  out->raw_dol = (NodBlob){p->dol, p->dol_size};
  out->raw_fst = (NodBlob){p->fst, p->fst_size};
  return NOD_RESULT_OK;
}

void nod_partition_iterate_fst(const NodHandle* partition, NodFstCallback cb, void* user) {
  if (!partition || partition->kind != H_PARTITION || !cb) return;
  const Partition* p = partition->u.part;
  const char* names = (const char*)p->fst + (size_t)p->nentries * 12;
  size_t names_len = p->fst_size - (size_t)p->nentries * 12;
  uint32_t i = 1;
  while (i < p->nentries) {
    const uint8_t* e = p->fst + (size_t)i * 12;
    uint32_t name_off = be32(e) & 0xFFFFFF;
    const char* name = name_off < names_len ? names + name_off : "";
    int is_dir = e[0] != 0;
    uint32_t size = be32(e + 8); /* file: byte size; dir: index of next entry after the dir */
    uint32_t next = cb(i, is_dir ? NOD_NODE_KIND_DIRECTORY : NOD_NODE_KIND_FILE, name, size, user);
    if (next == NOD_FST_STOP) break;
    i = next > i ? next : i + 1;
  }
}

bool nod_partition_is_wii(const NodHandle* partition) {
  (void)partition;
  return false;
}
