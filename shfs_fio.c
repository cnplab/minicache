/*
 *
 */
#include <target/sys.h>

#include "likely.h"
#include "shfs_fio.h"
#include "shfs.h"
#include "shfs_btable.h"
#include "shfs_cache.h"

#ifdef SHFS_STATS
#include "shfs_stats.h"
#endif

/*
 * Register open on bentry and return it on success
 */
static inline SHFS_FD _shfs_fio_open_bentry(struct shfs_bentry *bentry)
{
#ifdef SHFS_STATS
	struct shfs_el_stats *estats;
#endif

	if (bentry->update) {
		/* entry update in progress */
#ifdef SHFS_STATS
		++shfs_vol.mstats.e;
#endif
		errno = EBUSY;
		return NULL;
	}

	++shfs_nb_open;
	if (bentry->refcount == 0) {
		trydown(&bentry->updatelock); /* lock file for updates */
		shfs_fio_clear_cookie(bentry);
	}
	++bentry->refcount;
#ifdef SHFS_STATS
	estats = shfs_stats_from_bentry(bentry);
	estats->laccess = gettimestamp_s();
	++estats->h;
#endif
	return (SHFS_FD) bentry;
}

static inline __attribute__((always_inline))
struct shfs_bentry *_shfs_lookup_bentry_by_hash(hash512_t h)
{
	struct shfs_bentry *bentry;
#ifdef SHFS_STATS
	struct shfs_el_stats *estats;
#endif

	bentry = shfs_btable_lookup(shfs_vol.bt, h);
#ifdef SHFS_STATS
	if (unlikely(!bentry)) {
		estats = shfs_stats_from_mstats(h);
		if (likely(estats != NULL)) {
			estats->laccess = gettimestamp_s();
			++estats->m;
		}
	}
#endif
	return bentry;
}

/*
 * As long as we do not any operation that might call
 * schedule() (e.g., printf()), we do not need to
 * down/up the shfs_mount_lock semaphore -> coop.
 * scheduler
 *
 * Note: This function should never be called from interrupt context
 */
SHFS_FD shfs_fio_open(const char *path)
{
	hash512_t h;
	struct shfs_bentry *bentry;

	if (unlikely(!shfs_mounted)) {
		errno = ENODEV;
		return NULL;
	}

	/* lookup bentry (either by name or hash) */
	if ((path[0] == SHFS_HASH_INDICATOR_PREFIX) &&
	    (path[1] != '\0')) {
		if (hash_parse(path + 1, h, shfs_vol.hlen) < 0) {
#ifdef SHFS_STATS
			++shfs_vol.mstats.i;
#endif
			return NULL;
		}
		bentry = _shfs_lookup_bentry_by_hash(h);
	} else {
		if ((path[0] == '\0') ||
		    (path[0] == SHFS_HASH_INDICATOR_PREFIX && path[1] == '\0')) {
			/* empty filename -> use default file */
			bentry = shfs_vol.def_bentry;
#ifdef SHFS_STATS
			if (!bentry)
				++shfs_vol.mstats.i;
#endif
		} else {
#ifdef SHFS_OPENBYNAME
			bentry = shfs_btable_lookup_byname(shfs_vol.bt, shfs_vol.htable_chunk_cache, path);
#else
			bentry = NULL;
#ifdef SHFS_STATS
			if (unlikely(!bentry))
				++shfs_vol.mstats.i;
#endif
#endif
		}
	}
	if (!bentry) {
		errno = ENOENT;
		return NULL;
	}

	return _shfs_fio_open_bentry(bentry);
}

SHFS_FD shfs_fio_openh(hash512_t h)
{
	struct shfs_bentry *bentry;

	bentry = _shfs_lookup_bentry_by_hash(h);
	if (!bentry) {
		errno = ENOENT;
		return NULL;
	}

	return _shfs_fio_open_bentry(bentry);
}

/*
 * Opens a clone of an already opened file descriptor
 * This clone has to be closed by shfs_fio_close(), too.
 */
SHFS_FD shfs_fio_openf(SHFS_FD f)
{
	if (!f) {
		errno = EINVAL;
		return NULL;
	}
	if (!shfs_mounted) {
		errno = ENODEV;
		return NULL;
	}

	++f->refcount;
	++shfs_nb_open;
	return f;
}

/*
 * Note: This function should never be called from interrupt context
 */
void shfs_fio_close(SHFS_FD f)
{
	struct shfs_bentry *bentry = (struct shfs_bentry *) f;

	--bentry->refcount;
	if (bentry->refcount == 0) /* unlock file for updates */
		up(&bentry->updatelock);
	--shfs_nb_open;
}

void shfs_fio_name(SHFS_FD f, char *out, size_t outlen)
{
	struct shfs_bentry *bentry = (struct shfs_bentry *) f;
	struct shfs_hentry *hentry = bentry->hentry;

	outlen = min(outlen, sizeof(hentry->name) + 1);
	strncpy(out, hentry->name, outlen - 1);
	out[outlen - 1] = '\0';
}

void shfs_fio_mime(SHFS_FD f, char *out, size_t outlen)
{
	struct shfs_bentry *bentry = (struct shfs_bentry *) f;
	struct shfs_hentry *hentry = bentry->hentry;

	outlen = min(outlen, sizeof(hentry->f_attr.mime) + 1);
	strncpy(out, hentry->f_attr.mime, outlen - 1);
	out[outlen - 1] = '\0';
}

void shfs_fio_size(SHFS_FD f, uint64_t *out)
{
	struct shfs_bentry *bentry = (struct shfs_bentry *) f;
	struct shfs_hentry *hentry = bentry->hentry;

	*out = SHFS_HENTRY_ISLINK(hentry) ? 0 : hentry->f_attr.len;
}

void shfs_fio_hash(SHFS_FD f, hash512_t out)
{
	struct shfs_bentry *bentry = (struct shfs_bentry *) f;
	struct shfs_hentry *hentry = bentry->hentry;

	hash_copy(out, hentry->hash, shfs_vol.hlen);
}

void  shfs_fio_link_rpath(SHFS_FD f, char *out, size_t outlen)
{
	struct shfs_bentry *bentry = (struct shfs_bentry *) f;
	struct shfs_hentry *hentry = bentry->hentry;

	outlen = min(outlen, sizeof(hentry->l_attr.rpath) + 1);
	strncpy(out, hentry->l_attr.rpath, outlen - 1);
	out[outlen - 1] = '\0';
}

/*
 * Slow sync I/O file read functions
 * Warning: These functions are using busy-waiting
 */
int shfs_fio_read(SHFS_FD f, uint64_t offset, void *buf, uint64_t len)
{
	struct shfs_bentry *bentry = (struct shfs_bentry *) f;
	struct shfs_hentry *hentry = bentry->hentry;
	void     *chk_buf;
	chk_t    chk_off;
	uint64_t byt_off;
	uint64_t buf_off;
	uint64_t left;
	uint64_t rlen;
	int ret = 0;

	/* check if entry is link to remote file */
	if (SHFS_HENTRY_ISLINK(hentry))
		return -EINVAL;

	/* check boundaries */
	if ((offset > hentry->f_attr.len) ||
	    ((offset + len) > hentry->f_attr.len))
		return -EINVAL;

	/* pick chunk I/O buffer from pool */
	chk_buf = target_malloc(shfs_vol.ioalign, shfs_vol.chunksize);
	if (!chk_buf)
		return -ENOMEM;

	/* perform the I/O chunk-wise */
	chk_off = shfs_volchk_foff(f, offset);
	byt_off = shfs_volchkoff_foff(f, offset);
	left = len;
	buf_off = 0;

	while (left) {
		ret = shfs_read_chunk(chk_off, 1, chk_buf);
		if (ret < 0)
			goto out;

		rlen = min(shfs_vol.chunksize - byt_off, left);
		shfs_memcpy((uint8_t *) buf + buf_off,
			    (uint8_t *) chk_buf + byt_off,
			    rlen);
		left -= rlen;

		++chk_off;   /* go to next chunk */
		byt_off = 0; /* byte offset is set on the first chunk only */
		buf_off += rlen;
	}

 out:
	target_free(chk_buf);
	return ret;
}

int shfs_fio_read_nosched(SHFS_FD f, uint64_t offset, void *buf, uint64_t len)
{
	struct shfs_bentry *bentry = (struct shfs_bentry *) f;
	struct shfs_hentry *hentry = bentry->hentry;
	void     *chk_buf;
	chk_t    chk_off;
	uint64_t byt_off;
	uint64_t buf_off;
	uint64_t left;
	uint64_t rlen;
	int ret = 0;

	/* check if entry is link to remote file */
	if (SHFS_HENTRY_ISLINK(hentry))
		return -EINVAL;

	/* check boundaries */
	if ((offset > hentry->f_attr.len) ||
	    ((offset + len) > hentry->f_attr.len))
		return -EINVAL;

	/* pick chunk I/O buffer from pool */
	chk_buf = target_malloc(shfs_vol.ioalign, shfs_vol.chunksize);
	if (!chk_buf)
		return -ENOMEM;

	/* perform the I/O chunk-wise */
	chk_off = shfs_volchk_foff(f, offset);
	byt_off = shfs_volchkoff_foff(f, offset);
	left = len;
	buf_off = 0;

	while (left) {
		ret = shfs_read_chunk_nosched(chk_off, 1, chk_buf);
		if (ret < 0)
			goto out;

		rlen = min(shfs_vol.chunksize - byt_off, left);
		shfs_memcpy((uint8_t *) buf + buf_off,
			    (uint8_t *) chk_buf + byt_off,
			    rlen);
		left -= rlen;

		++chk_off;   /* go to next chunk */
		byt_off = 0; /* byte offset is set on the first chunk only */
		buf_off += rlen;
	}

 out:
	target_free(chk_buf);
	return ret;
}

int shfs_fio_cache_read(SHFS_FD f, uint64_t offset, void *buf, uint64_t len)
{
	struct shfs_bentry *bentry = (struct shfs_bentry *) f;
	struct shfs_hentry *hentry = bentry->hentry;
	struct shfs_cache_entry *cce;
	chk_t    chk_off;
	uint64_t byt_off;
	uint64_t buf_off;
	uint64_t left;
	uint64_t rlen;
	int ret = 0;

	/* check if entry is link to remote file */
	if (SHFS_HENTRY_ISLINK(hentry))
		return -EINVAL;

	/* check boundaries */
	if ((offset > hentry->f_attr.len) ||
	    ((offset + len) > hentry->f_attr.len))
		return -EINVAL;

	/* perform the I/O chunk-wise */
	chk_off = shfs_volchk_foff(f, offset);
	byt_off = shfs_volchkoff_foff(f, offset);
	left = len;
	buf_off = 0;

	while (left) {
		cce = shfs_cache_read(chk_off);
		if (!cce) {
			ret = -errno;
			goto out;
		}

		rlen = min(shfs_vol.chunksize - byt_off, left);
		shfs_memcpy((uint8_t *) buf + buf_off,
			    (uint8_t *) cce->buffer + byt_off,
			    rlen);
		left -= rlen;

		shfs_cache_release(cce);

		++chk_off;   /* go to next chunk */
		byt_off = 0; /* byte offset is set on the first chunk only */
		buf_off += rlen;
	}

 out:
	return ret;
}

int shfs_fio_cache_read_nosched(SHFS_FD f, uint64_t offset, void *buf, uint64_t len)
{
	struct shfs_bentry *bentry = (struct shfs_bentry *) f;
	struct shfs_hentry *hentry = bentry->hentry;
	struct shfs_cache_entry *cce;
	chk_t    chk_off;
	uint64_t byt_off;
	uint64_t buf_off;
	uint64_t left;
	uint64_t rlen;
	int ret = 0;

	/* check if entry is link to remote file */
	if (SHFS_HENTRY_ISLINK(hentry))
		return -EINVAL;

	/* check boundaries */
	if ((offset > hentry->f_attr.len) ||
	    ((offset + len) > hentry->f_attr.len))
		return -EINVAL;

	/* perform the I/O chunk-wise */
	chk_off = shfs_volchk_foff(f, offset);
	byt_off = shfs_volchkoff_foff(f, offset);
	left = len;
	buf_off = 0;

	while (left) {
		cce = shfs_cache_read_nosched(chk_off);
		if (!cce) {
			ret = -errno;
			goto out;
		}

		rlen = min(shfs_vol.chunksize - byt_off, left);
		shfs_memcpy((uint8_t *) buf + buf_off,
			    (uint8_t *) cce->buffer + byt_off,
			    rlen);
		left -= rlen;

		shfs_cache_release(cce);

		++chk_off;   /* go to next chunk */
		byt_off = 0; /* byte offset is set on the first chunk only */
		buf_off += rlen;
	}

 out:
	return ret;
}
