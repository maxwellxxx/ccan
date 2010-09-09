#include "private.h"
#include <ccan/tdb2/tdb2.h>
#include <ccan/build_assert/build_assert.h>
#include <ccan/likely/likely.h>
#include <assert.h>

/* The null return. */
struct tdb_data tdb_null = { .dptr = NULL, .dsize = 0 };

/* all contexts, to ensure no double-opens (fcntl locks don't nest!) */
static struct tdb_context *tdbs = NULL;

PRINTF_ATTRIBUTE(4, 5) static void
null_log_fn(struct tdb_context *tdb,
	    enum tdb_debug_level level, void *priv,
	    const char *fmt, ...)
{
}

static bool tdb_already_open(dev_t device, ino_t ino)
{
	struct tdb_context *i;
	
	for (i = tdbs; i; i = i->next) {
		if (i->device == device && i->inode == ino) {
			return true;
		}
	}

	return false;
}

static uint64_t random_number(struct tdb_context *tdb)
{
	int fd;
	uint64_t ret = 0;
	struct timeval now;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) {
		if (tdb_read_all(fd, &ret, sizeof(ret))) {
			tdb->log(tdb, TDB_DEBUG_TRACE, tdb->log_priv,
				 "tdb_open: random from /dev/urandom\n");
			close(fd);
			return ret;
		}
		close(fd);
	}
	/* FIXME: Untested!  Based on Wikipedia protocol description! */
	fd = open("/dev/egd-pool", O_RDWR);
	if (fd >= 0) {
		/* Command is 1, next byte is size we want to read. */
		char cmd[2] = { 1, sizeof(uint64_t) };
		if (write(fd, cmd, sizeof(cmd)) == sizeof(cmd)) {
			char reply[1 + sizeof(uint64_t)];
			int r = read(fd, reply, sizeof(reply));
			if (r > 1) {
				tdb->log(tdb, TDB_DEBUG_TRACE, tdb->log_priv,
					 "tdb_open: %u random bytes from"
					 " /dev/egd-pool\n", r-1);
				/* Copy at least some bytes. */
				memcpy(&ret, reply+1, r - 1);
				if (reply[0] == sizeof(uint64_t)
				    && r == sizeof(reply)) {
					close(fd);
					return ret;
				}
			}
		}
		close(fd);
	}

	/* Fallback: pid and time. */
	gettimeofday(&now, NULL);
	ret = getpid() * 100132289ULL + now.tv_sec * 1000000ULL + now.tv_usec;
	tdb->log(tdb, TDB_DEBUG_TRACE, tdb->log_priv,
		 "tdb_open: random from getpid and time\n");
	return ret;
}

struct new_database {
	struct tdb_header hdr;
	/* Initial free zone. */
	struct free_zone_header zhdr;
	tdb_off_t free[BUCKETS_FOR_ZONE(INITIAL_ZONE_BITS) + 1];
	struct tdb_free_record frec;
	/* Rest up to 1 << INITIAL_ZONE_BITS is empty. */
	char space[(1 << INITIAL_ZONE_BITS)
		   - sizeof(struct free_zone_header)
		   - sizeof(tdb_off_t) * (BUCKETS_FOR_ZONE(INITIAL_ZONE_BITS)+1)
		   - sizeof(struct tdb_free_record)];
	uint8_t tailer;
	/* Don't count final padding! */
};

/* initialise a new database */
static int tdb_new_database(struct tdb_context *tdb, struct tdb_header *hdr)
{
	/* We make it up in memory, then write it out if not internal */
	struct new_database newdb;
	unsigned int bucket, magic_len, dbsize;

	/* Don't want any extra padding! */
	dbsize = offsetof(struct new_database, tailer) + sizeof(newdb.tailer);

	/* Fill in the header */
	newdb.hdr.version = TDB_VERSION;
	newdb.hdr.hash_seed = random_number(tdb);
	newdb.hdr.hash_test = TDB_HASH_MAGIC;
	newdb.hdr.hash_test = tdb->khash(&newdb.hdr.hash_test,
					 sizeof(newdb.hdr.hash_test),
					 newdb.hdr.hash_seed,
					 tdb->hash_priv);
	memset(newdb.hdr.reserved, 0, sizeof(newdb.hdr.reserved));
	/* Initial hashes are empty. */
	memset(newdb.hdr.hashtable, 0, sizeof(newdb.hdr.hashtable));

	/* Free is mostly empty... */
	newdb.zhdr.zone_bits = INITIAL_ZONE_BITS;
	memset(newdb.free, 0, sizeof(newdb.free));

	/* Create the single free entry. */
	newdb.frec.magic_and_meta = TDB_FREE_MAGIC | INITIAL_ZONE_BITS;
	newdb.frec.data_len = (sizeof(newdb.frec)
				 - sizeof(struct tdb_used_record)
				 + sizeof(newdb.space));

	/* Add it to the correct bucket. */
	bucket = size_to_bucket(INITIAL_ZONE_BITS, newdb.frec.data_len);
	newdb.free[bucket] = offsetof(struct new_database, frec);
	newdb.frec.next = newdb.frec.prev = 0;

	/* Clear free space to keep valgrind happy, and avoid leaking stack. */
	memset(newdb.space, 0, sizeof(newdb.space));

	/* Tailer contains maximum number of free_zone bits. */
	newdb.tailer = INITIAL_ZONE_BITS;

	/* Magic food */
	memset(newdb.hdr.magic_food, 0, sizeof(newdb.hdr.magic_food));
	strcpy(newdb.hdr.magic_food, TDB_MAGIC_FOOD);

	/* This creates an endian-converted database, as if read from disk */
	magic_len = sizeof(newdb.hdr.magic_food);
	tdb_convert(tdb,
		    (char *)&newdb.hdr + magic_len,
		    offsetof(struct new_database, space) - magic_len);

	*hdr = newdb.hdr;

	if (tdb->flags & TDB_INTERNAL) {
		tdb->map_size = dbsize;
		tdb->map_ptr = malloc(tdb->map_size);
		if (!tdb->map_ptr) {
			tdb->ecode = TDB_ERR_OOM;
			return -1;
		}
		memcpy(tdb->map_ptr, &newdb, tdb->map_size);
		return 0;
	}
	if (lseek(tdb->fd, 0, SEEK_SET) == -1)
		return -1;

	if (ftruncate(tdb->fd, 0) == -1)
		return -1;

	if (!tdb_pwrite_all(tdb->fd, &newdb, dbsize, 0)) {
		tdb->ecode = TDB_ERR_IO;
		return -1;
	}
	return 0;
}

struct tdb_context *tdb_open(const char *name, int tdb_flags,
			     int open_flags, mode_t mode,
			     union tdb_attribute *attr)
{
	struct tdb_context *tdb;
	struct stat st;
	int save_errno;
	uint64_t hash_test;
	unsigned v;
	struct tdb_header hdr;

	tdb = malloc(sizeof(*tdb));
	if (!tdb) {
		/* Can't log this */
		errno = ENOMEM;
		goto fail;
	}
	tdb->name = NULL;
	tdb->map_ptr = NULL;
	tdb->direct_access = 0;
	tdb->fd = -1;
	tdb->map_size = sizeof(struct tdb_header);
	tdb->ecode = TDB_SUCCESS;
	tdb->flags = tdb_flags;
	tdb->log = null_log_fn;
	tdb->log_priv = NULL;
	tdb->transaction = NULL;
	tdb_hash_init(tdb);
	/* last_zone will be set below. */
	tdb_io_init(tdb);
	tdb_lock_init(tdb);

	while (attr) {
		switch (attr->base.attr) {
		case TDB_ATTRIBUTE_LOG:
			tdb->log = attr->log.log_fn;
			tdb->log_priv = attr->log.log_private;
			break;
		case TDB_ATTRIBUTE_HASH:
			tdb->khash = attr->hash.hash_fn;
			tdb->hash_priv = attr->hash.hash_private;
			break;
		default:
			tdb->log(tdb, TDB_DEBUG_ERROR, tdb->log_priv,
				 "tdb_open: unknown attribute type %u\n",
				 attr->base.attr);
			errno = EINVAL;
			goto fail;
		}
		attr = attr->base.next;
	}

	if ((open_flags & O_ACCMODE) == O_WRONLY) {
		tdb->log(tdb, TDB_DEBUG_ERROR, tdb->log_priv,
			 "tdb_open: can't open tdb %s write-only\n", name);
		errno = EINVAL;
		goto fail;
	}

	if ((open_flags & O_ACCMODE) == O_RDONLY) {
		tdb->read_only = true;
		/* read only databases don't do locking */
		tdb->flags |= TDB_NOLOCK;
	} else
		tdb->read_only = false;

	/* internal databases don't need any of the rest. */
	if (tdb->flags & TDB_INTERNAL) {
		tdb->flags |= (TDB_NOLOCK | TDB_NOMMAP);
		if (tdb_new_database(tdb, &hdr) != 0) {
			tdb->log(tdb, TDB_DEBUG_ERROR, tdb->log_priv,
				 "tdb_open: tdb_new_database failed!");
			goto fail;
		}
		tdb_convert(tdb, &hdr.hash_seed, sizeof(hdr.hash_seed));
		tdb->hash_seed = hdr.hash_seed;
		tdb_zone_init(tdb);
		return tdb;
	}

	if ((tdb->fd = open(name, open_flags, mode)) == -1) {
		tdb->log(tdb, TDB_DEBUG_WARNING, tdb->log_priv,
			 "tdb_open: could not open file %s: %s\n",
			 name, strerror(errno));
		goto fail;	/* errno set by open(2) */
	}

	/* on exec, don't inherit the fd */
	v = fcntl(tdb->fd, F_GETFD, 0);
        fcntl(tdb->fd, F_SETFD, v | FD_CLOEXEC);

	/* ensure there is only one process initialising at once */
	if (tdb_lock_open(tdb) == -1) {
		tdb->log(tdb, TDB_DEBUG_ERROR, tdb->log_priv,
			 "tdb_open: failed to get open lock on %s: %s\n",
			 name, strerror(errno));
		goto fail;	/* errno set by tdb_brlock */
	}

	if (!tdb_pread_all(tdb->fd, &hdr, sizeof(hdr), 0)
	    || strcmp(hdr.magic_food, TDB_MAGIC_FOOD) != 0) {
		if (!(open_flags & O_CREAT) || tdb_new_database(tdb, &hdr) == -1) {
			if (errno == 0) {
				errno = EIO; /* ie bad format or something */
			}
			goto fail;
		}
	} else if (hdr.version != TDB_VERSION) {
		if (hdr.version == bswap_64(TDB_VERSION))
			tdb->flags |= TDB_CONVERT;
		else {
			/* wrong version */
			tdb->log(tdb, TDB_DEBUG_ERROR, tdb->log_priv,
				 "tdb_open: %s is unknown version 0x%llx\n",
				 name, (long long)hdr.version);
			errno = EIO;
			goto fail;
		}
	}

	tdb_convert(tdb, &hdr, sizeof(hdr));
	tdb->hash_seed = hdr.hash_seed;
	hash_test = TDB_HASH_MAGIC;
	hash_test = tdb_hash(tdb, &hash_test, sizeof(hash_test));
	if (hdr.hash_test != hash_test) {
		/* wrong hash variant */
		tdb->log(tdb, TDB_DEBUG_ERROR, tdb->log_priv,
			 "tdb_open: %s uses a different hash function\n",
			 name);
		errno = EIO;
		goto fail;
	}

	if (fstat(tdb->fd, &st) == -1)
		goto fail;

	/* Is it already in the open list?  If so, fail. */
	if (tdb_already_open(st.st_dev, st.st_ino)) {
		/* FIXME */
		tdb->log(tdb, TDB_DEBUG_ERROR, tdb->log_priv,
			 "tdb_open: %s (%d,%d) is already open in this process\n",
			 name, (int)st.st_dev, (int)st.st_ino);
		errno = EBUSY;
		goto fail;
	}

	tdb->name = strdup(name);
	if (!tdb->name) {
		errno = ENOMEM;
		goto fail;
	}

	tdb->device = st.st_dev;
	tdb->inode = st.st_ino;
	tdb_unlock_open(tdb);

	/* This make sure we have current map_size and mmap. */
	tdb->methods->oob(tdb, tdb->map_size + 1, true);

	/* Now we can pick a random free zone to start from. */
	if (tdb_zone_init(tdb) == -1)
		goto fail;

	tdb->next = tdbs;
	tdbs = tdb;
	return tdb;

 fail:
	save_errno = errno;

	if (!tdb)
		return NULL;

#ifdef TDB_TRACE
	close(tdb->tracefd);
#endif
	if (tdb->map_ptr) {
		if (tdb->flags & TDB_INTERNAL) {
			free(tdb->map_ptr);
		} else
			tdb_munmap(tdb);
	}
	free((char *)tdb->name);
	if (tdb->fd != -1)
		if (close(tdb->fd) != 0)
			tdb->log(tdb, TDB_DEBUG_ERROR, tdb->log_priv,
				 "tdb_open: failed to close tdb->fd"
				 " on error!\n");
	free(tdb);
	errno = save_errno;
	return NULL;
}

/* FIXME: modify, don't rewrite! */
static int update_rec_hdr(struct tdb_context *tdb,
			  tdb_off_t off,
			  tdb_len_t keylen,
			  tdb_len_t datalen,
			  struct tdb_used_record *rec,
			  uint64_t h)
{
	uint64_t dataroom = rec_data_length(rec) + rec_extra_padding(rec);

	if (set_header(tdb, rec, keylen, datalen, keylen + dataroom, h,
		       rec_zone_bits(rec)))
		return -1;

	return tdb_write_convert(tdb, off, rec, sizeof(*rec));
}

/* Returns -1 on error, 0 on OK */
static int replace_data(struct tdb_context *tdb,
			struct hash_info *h,
			struct tdb_data key, struct tdb_data dbuf,
			tdb_off_t old_off, tdb_len_t old_room,
			unsigned old_zone,
			bool growing)
{
	tdb_off_t new_off;

	/* Allocate a new record. */
	new_off = alloc(tdb, key.dsize, dbuf.dsize, h->h, growing);
	if (unlikely(new_off == TDB_OFF_ERR))
		return -1;

	/* We didn't like the existing one: remove it. */
	if (old_off) {
		add_free_record(tdb, old_zone, old_off,
				sizeof(struct tdb_used_record)
				+ key.dsize + old_room);
		if (replace_in_hash(tdb, h, new_off) == -1)
			return -1;
	} else {
		if (add_to_hash(tdb, h, new_off) == -1)
			return -1;
	}

	new_off += sizeof(struct tdb_used_record);
	if (tdb->methods->write(tdb, new_off, key.dptr, key.dsize) == -1)
		return -1;

	new_off += key.dsize;
	if (tdb->methods->write(tdb, new_off, dbuf.dptr, dbuf.dsize) == -1)
		return -1;

	/* FIXME: tdb_increment_seqnum(tdb); */
	return 0;
}

int tdb_store(struct tdb_context *tdb,
	      struct tdb_data key, struct tdb_data dbuf, int flag)
{
	struct hash_info h;
	tdb_off_t off;
	tdb_len_t old_room = 0;
	struct tdb_used_record rec;
	int ret;

	off = find_and_lock(tdb, key, F_WRLCK, &h, &rec);
	if (unlikely(off == TDB_OFF_ERR))
		return -1;

	/* Now we have lock on this hash bucket. */
	if (flag == TDB_INSERT) {
		if (off) {
			tdb->ecode = TDB_ERR_EXISTS;
			goto fail;
		}
	} else {
		if (off) {
			old_room = rec_data_length(&rec)
				+ rec_extra_padding(&rec);
			if (old_room >= dbuf.dsize) {
				/* Can modify in-place.  Easy! */
				if (update_rec_hdr(tdb, off,
						   key.dsize, dbuf.dsize,
						   &rec, h.h))
					goto fail;
				if (tdb->methods->write(tdb, off + sizeof(rec)
							+ key.dsize,
							dbuf.dptr, dbuf.dsize))
					goto fail;
				tdb_unlock_hashes(tdb, h.hlock_start,
						  h.hlock_range, F_WRLCK);
				return 0;
			}
			/* FIXME: See if right record is free? */
		} else {
			if (flag == TDB_MODIFY) {
				/* if the record doesn't exist and we
				   are in TDB_MODIFY mode then we should fail
				   the store */
				tdb->ecode = TDB_ERR_NOEXIST;
				goto fail;
			}
		}
	}

	/* If we didn't use the old record, this implies we're growing. */
	ret = replace_data(tdb, &h, key, dbuf, off, old_room,
			   rec_zone_bits(&rec), off != 0);
	tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range, F_WRLCK);
	return ret;

fail:
	tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range, F_WRLCK);
	return -1;
}

int tdb_append(struct tdb_context *tdb,
	       struct tdb_data key, struct tdb_data dbuf)
{
	struct hash_info h;
	tdb_off_t off;
	struct tdb_used_record rec;
	tdb_len_t old_room = 0, old_dlen;
	unsigned char *newdata;
	struct tdb_data new_dbuf;
	int ret;

	off = find_and_lock(tdb, key, F_WRLCK, &h, &rec);
	if (unlikely(off == TDB_OFF_ERR))
		return -1;

	if (off) {
		old_dlen = rec_data_length(&rec);
		old_room = old_dlen + rec_extra_padding(&rec);

		/* Fast path: can append in place. */
		if (rec_extra_padding(&rec) >= dbuf.dsize) {
			if (update_rec_hdr(tdb, off, key.dsize,
					   old_dlen + dbuf.dsize, &rec, h.h))
				goto fail;

			off += sizeof(rec) + key.dsize + old_dlen;
			if (tdb->methods->write(tdb, off, dbuf.dptr,
						dbuf.dsize) == -1)
				goto fail;

			/* FIXME: tdb_increment_seqnum(tdb); */
			tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range,
					  F_WRLCK);
			return 0;
		}
		/* FIXME: Check right record free? */

		/* Slow path. */
		newdata = malloc(key.dsize + old_dlen + dbuf.dsize);
		if (!newdata) {
			tdb->ecode = TDB_ERR_OOM;
			tdb->log(tdb, TDB_DEBUG_FATAL, tdb->log_priv,
				 "tdb_append: cannot allocate %llu bytes!\n",
				 (long long)key.dsize + old_dlen + dbuf.dsize);
			goto fail;
		}
		if (tdb->methods->read(tdb, off + sizeof(rec) + key.dsize,
				       newdata, old_dlen) != 0) {
			free(newdata);
			goto fail;
		}
		memcpy(newdata + old_dlen, dbuf.dptr, dbuf.dsize);
		new_dbuf.dptr = newdata;
		new_dbuf.dsize = old_dlen + dbuf.dsize;
	} else {
		newdata = NULL;
		new_dbuf = dbuf;
	}

	/* If they're using tdb_append(), it implies they're growing record. */
	ret = replace_data(tdb, &h, key, new_dbuf, off,
			   old_room, rec_zone_bits(&rec), true);
	tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range, F_WRLCK);
	free(newdata);

	return ret;

fail:
	tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range, F_WRLCK);
	return -1;
}

struct tdb_data tdb_fetch(struct tdb_context *tdb, struct tdb_data key)
{
	tdb_off_t off;
	struct tdb_used_record rec;
	struct hash_info h;
	struct tdb_data ret;

	off = find_and_lock(tdb, key, F_RDLCK, &h, &rec);
	if (unlikely(off == TDB_OFF_ERR))
		return tdb_null;

	if (!off) {
		tdb->ecode = TDB_ERR_NOEXIST;
		ret = tdb_null;
	} else {
		ret.dsize = rec_data_length(&rec);
		ret.dptr = tdb_alloc_read(tdb, off + sizeof(rec) + key.dsize,
					  ret.dsize);
	}

	tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range, F_RDLCK);
	return ret;
}

int tdb_delete(struct tdb_context *tdb, struct tdb_data key)
{
	tdb_off_t off;
	struct tdb_used_record rec;
	struct hash_info h;

	off = find_and_lock(tdb, key, F_WRLCK, &h, &rec);
	if (unlikely(off == TDB_OFF_ERR))
		return -1;

	if (!off) {
		tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range, F_WRLCK);
		tdb->ecode = TDB_ERR_NOEXIST;
		return -1;
	}

	if (delete_from_hash(tdb, &h) == -1)
		goto unlock_err;

	/* Free the deleted entry. */
	if (add_free_record(tdb, rec_zone_bits(&rec), off,
			    sizeof(struct tdb_used_record)
			    + rec_key_length(&rec)
			    + rec_data_length(&rec)
			    + rec_extra_padding(&rec)) != 0)
		goto unlock_err;

	tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range, F_WRLCK);
	return 0;

unlock_err:
	tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range, F_WRLCK);
	return -1;
}

int tdb_close(struct tdb_context *tdb)
{
	struct tdb_context **i;
	int ret = 0;

	/* FIXME:
	if (tdb->transaction) {
		tdb_transaction_cancel(tdb);
	}
	*/
	tdb_trace(tdb, "tdb_close");

	if (tdb->map_ptr) {
		if (tdb->flags & TDB_INTERNAL)
			free(tdb->map_ptr);
		else
			tdb_munmap(tdb);
	}
	free((char *)tdb->name);
	if (tdb->fd != -1) {
		ret = close(tdb->fd);
		tdb->fd = -1;
	}
	free(tdb->lockrecs);

	/* Remove from contexts list */
	for (i = &tdbs; *i; i = &(*i)->next) {
		if (*i == tdb) {
			*i = tdb->next;
			break;
		}
	}

#ifdef TDB_TRACE
	close(tdb->tracefd);
#endif
	free(tdb);

	return ret;
}

enum TDB_ERROR tdb_error(struct tdb_context *tdb)
{
	return tdb->ecode;
}