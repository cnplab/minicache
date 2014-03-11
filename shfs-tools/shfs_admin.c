#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <uuid/uuid.h>
#include <mhash.h>

#include "shfs_admin.h"
#include "shfs_htable.h"
#include "shfs_alloc.h"

unsigned int verbosity = 0;
int force = 0;

static struct vol_info shfs_vol;

/******************************************************************************
 * ARGUMENT PARSING                                                           *
 ******************************************************************************/
const char *short_opts = "h?vVfa:r:m:n:li";

static struct option long_opts[] = {
	{"help",	no_argument,		NULL,	'h'},
	{"version",	no_argument,		NULL,	'V'},
	{"verbose",	no_argument,		NULL,	'v'},
	{"force",	no_argument,		NULL,	'f'},
	{"add-file",	required_argument,	NULL,	'a'},
	{"rm-file",	required_argument,	NULL,	'r'},
	{"mime",	required_argument,	NULL,	'm'},
	{"name",	required_argument,	NULL,	'n'},
	{"ls",	        no_argument,            NULL,	'l'},
	{"info",	no_argument,            NULL,	'i'},
	{NULL, 0, NULL, 0} /* end of list */
};

static inline void print_version()
{
	printf("%s (build: %s %s)\n", STR_VERSION, __DATE__, __TIME__);
}

static void print_usage(char *argv0)
{
	printf("Usage: %s [OPTION]... [DEVICE]...\n", argv0);
	printf("Administration of an SHFS volume.\n");
	printf("\n");
	printf("Mandatory arguments to long options are mandatory for short options too.\n");
	printf("  -h, --help                 displays this help and exit\n");
	printf("  -V, --version              displays program version and exit\n");
	printf("  -v, --verbose              increases verbosity level (max. %d times)\n", D_MAX);
	printf("  -f, --force                suppresses warnings and user questions\n");
	printf("  -a, --add-file [FILE]      add a file to the volume\n");
	printf("  For each add-file token:\n");
	printf("    -m, --mime [MIME]        sets the MIME type of a file\n");
	printf("    -n, --name [NAME]        sets an additionally name to a file\n");
	printf("  -r, --rm-file [HASH]       removes a file from the volume\n");
	printf("  -l, --ls                   lists the volume contents\n");
	printf("  -i, --info                 shows volume information\n");
	/* printf("  -r, --rm-file [HASH]       Remove a file from the volume\n"); */
	printf("\n");
	printf("Example (adding a file):\n");
	printf(" %s --add-file song.mp4 /dev/ram14 /dev/ram15\n", argv0);

}

static void release_args(struct args *args)
{
	struct job *cjob;
	struct job *njob;

	cjob = args->jobs;

	/* release job list */
	while (cjob) {
		if (cjob->path)
			free(cjob->path);
		if (cjob->optstr0)
			free(cjob->optstr0);
		if (cjob->optstr1)
			free(cjob->optstr1);
		njob = cjob->next;
		free(cjob);
		cjob = njob;
	}
	memset(args, 0, sizeof(*args));
}

/**
 * Adds a job to args job list
 *
 * ljob: Current last job of the list
 *       If it is NULL, it is assumed that there is no list currently
 *       -> this function creates the first element and adds it to args
 */
static inline struct job *args_add_job(struct job *ljob, struct args *args)
{
	struct job *njob;

	njob = calloc(1, sizeof(*njob));
	if (!njob)
		die();

	if (ljob)
		ljob->next = njob;
	else
		args->jobs = njob;

	return njob;
}

static int parse_args(int argc, char **argv, struct args *args)
/*
 * Parse arguments on **argv (number of args on argc)
 * with GNUOPTS to *args
 *
 * This function will exit the program for itself
 * when -h or -V is parsed or on fatal errors
 * (such as ENOMEM)
 *
 * -EINVAL will be returned on parsing errors or
 * invalid options
 *
 * *args has to be passed in a cleared state
 */
{
	int opt, opt_index = 0;
	struct job *cjob;
	/*
	 * set default values
	 */
	args->nb_devs = 0;
	args->jobs = NULL;
	cjob = args->jobs;

	/*
	 * Parse options
	 */
	while (1) {
		opt = getopt_long(argc, argv, short_opts, long_opts, &opt_index);

		if (opt == -1)    /* end of options */
			break;

		switch (opt) {
		case 'h':
		case '?': /* usage */
			print_usage(argv[0]);
			exit(EXIT_SUCCESS);
		case 'V': /* version */
			print_version();
			exit(EXIT_SUCCESS);
		case 'v': /* verbosity */
			if (verbosity < D_MAX)
				verbosity++;
			break;
		case 'f': /* force */
			force = 1;
			break;
		case 'a': /* add-file */
			cjob = args_add_job(cjob, args);
			cjob->action = ADDFILE;
			if (parse_args_setval_str(&cjob->path, optarg) < 0)
				die();
			break;
		case 'r': /* rm-file */
			cjob = args_add_job(cjob, args);
			cjob->action = RMFILE;
			if (parse_args_setval_str(&cjob->path, optarg) < 0)
				die();
			break;
		case 'm': /* mime */
			if (!cjob || cjob->action != ADDFILE) {
				eprintf("Please set mime after an add-file token\n");
				return -EINVAL;
			}
			if (parse_args_setval_str(&cjob->optstr0, optarg) < 0)
				die();
			break;
		case 'n': /* name */
			if (!cjob || cjob->action != ADDFILE) {
				eprintf("Please set name after an add-file token\n");
				return -EINVAL;
			}
			if (parse_args_setval_str(&cjob->optstr1, optarg) < 0)
				die();
			break;
		case 'l': /* ls */
			cjob = args_add_job(cjob, args);
			cjob->action = LSFILES;
			break;
		case 'i': /* info */
			cjob = args_add_job(cjob, args);
			cjob->action = SHOWINFO;
			break;
		default:
			/* unknown option */
			return -EINVAL;
		}
	}

	/* extra arguments are devices... just add a reference of those to args */
	if (argc <= optind) {
		eprintf("Path to volume member device(s) not specified\n");
		return -EINVAL;
	}
	args->devpath = &argv[optind];
	args->nb_devs = argc - optind;

	/* check job list, if mandatory argements were given */
	for (cjob = args->jobs; cjob != NULL; cjob = cjob->next) {
		switch(cjob->action) {
		case ADDFILE:
			/* nothing to check (mime is optional) */
			break;
		default:
			break; /* unsupported job but should never happen */
		}
	}

	return 0;
}


/******************************************************************************
 * SIGNAL HANDLING                                                            *
 ******************************************************************************/

static volatile int cancel = 0;

void sigint_handler(int signum) {
	printf("Caught abort signal: Cancelling...\n");
	cancel = 1;
}


/******************************************************************************
 * DISK I/O                                                                   *
 ******************************************************************************/
/**
 * Performs I/O on the member disks of a volume
 *
 * This function can only be called, after load_vol_cconf
 * established successfully the low-level setup of a volume
 */
static int shfs_sync_io_chunk(chk_t start, chk_t len, int owrite, void *buffer)
{
	chk_t end, c;
	off_t startb;
	size_t lenb;
	unsigned int m;
	uint8_t *wptr = buffer;

	end = start + len;
	for (c = start; c < end; c++) {
		for (m = 0; m < shfs_vol.nb_members; ++m) {
			startb = c * shfs_vol.stripesize;
			lenb = shfs_vol.stripesize;
			dprintf(D_L1, "blkdev_sync_io member=%u, "
			        "start=%lu (%lu KiB), "
			        "len=%lu (%lu KiB), "
			        "wptr=%p\n",
			        m,
			        start, startb / 1024,
			        len, lenb / 1024,
			        wptr);

			if (lseek(shfs_vol.member[m].d->fd, startb, SEEK_SET) < 0) {
				eprintf("Could not seek on %s: %s\n", shfs_vol.member[m].d->path, strerror(errno));
				return -1;
			}
			if (owrite) {
				if (write(shfs_vol.member[m].d->fd, wptr, shfs_vol.stripesize) < 0) {
					eprintf("Could not write to %s: %s\n", shfs_vol.member[m].d->path, strerror(errno));
					return -1;
				}
			} else {
				if (read(shfs_vol.member[m].d->fd, wptr, shfs_vol.stripesize) < 0) {
					eprintf("Could not read from %s: %s\n", shfs_vol.member[m].d->path, strerror(errno));
					return -1;
				}
			}

			wptr += shfs_vol.stripesize;
		}
	}

	return 0;
}
#define shfs_sync_read_chunk(start, len, buffer) \
	shfs_sync_io_chunk((start), (len), 0, (buffer))
#define shfs_sync_write_chunk(start, len, buffer) \
	shfs_sync_io_chunk((start), (len), 1, (buffer))


/******************************************************************************
 * MOUNT / UMOUNT                                                             *
 ******************************************************************************/
/**
 * This function tries to open a blkdev and checks if it has a valid SHFS label
 * It returns the opened blkdev descriptor and the read disk chk0
 *  on *chk0
 *
 * Note: chk0 has to be a buffer of 4096 bytes and be aligned to 4096 bytes
 */
static struct disk *checkopen_disk(const char *path, void *chk0)
{
	struct disk *d;
	int ret;

	d = open_disk(path, O_RDWR);
	if (!d)
		dief("Could not open %s\n", path);

	/* incompatible device? */
	if (d->blksize < 512 || !POWER_OF_2(d->blksize))
		dief("%s has a incompatible block size\n", path);

	/* read first chunk (considered as 4K) */
	if (lseek(d->fd, 0, SEEK_SET) < 0)
		dief("Could not seek on %s: %s\n", path, strerror(errno));

	if (read(d->fd, chk0, 4096) < 0)
		dief("Could not read from %s: %s\n", path, strerror(errno));

	/* Try to detect the SHFS disk label */
	ret = shfs_detect_hdr0(chk0);
	if (ret < 0)
		dief("Invalid or unsupported SHFS label detected on %s: %d\n", path, ret);

	return d;
}

/**
 * This function iterates over disks, tries to detect the SHFS label
 * and does the low-level setup for mounting a volume
 */
static void load_vol_cconf(char *path[], unsigned int count)
{
	struct disk *d;
	struct vol_member detected_member[MAX_NB_TRY_BLKDEVS];
	struct shfs_hdr_common *hdr_common;
	unsigned int i, j, m;
	unsigned int nb_detected_members;
	uint64_t min_member_size;
	void *chk0;

	dprintf(D_L0, "Detecting SHFS volume...\n");
	if (count > MAX_NB_TRY_BLKDEVS)
		dief("More devices passed than supported by a single SHFS volume");

	chk0 = malloc(4096);
	if (!chk0)
		die();

	/* Iterate over disks and try to find those with a valid SHFS disk label */
	nb_detected_members = 0;
	for (i = 0; i < count; i++) {
		d = checkopen_disk(path[i], chk0);
		dprintf(D_L0, "SHFSv1 label on %s detected\n", path[i]);

		/* chk0 contains the first chunk read from disk */
		hdr_common = (void *)((uint8_t *) chk0 + BOOT_AREA_LENGTH);
		detected_member[nb_detected_members].d = d;
		uuid_copy(detected_member[nb_detected_members].uuid, hdr_common->member_uuid);
		nb_detected_members++;
	}
	if (nb_detected_members == 0)
		dief("No SHFS disk found");

	/* Load label from first detected member */
	/* read first chunk (considered as 4K) */
	if (lseek(detected_member[0].d->fd, 0, SEEK_SET) < 0)
		dief("Could not seek on %s: %s\n", detected_member[0].d->path, strerror(errno));
	if (read(detected_member[0].d->fd, chk0, 4096) < 0)
		dief("Could not read from %s: %s\n", detected_member[0].d->path, strerror(errno));

	hdr_common = (void *)((uint8_t *) chk0 + BOOT_AREA_LENGTH);
	memcpy(shfs_vol.uuid, hdr_common->vol_uuid, 16);
	memcpy(shfs_vol.volname, hdr_common->vol_name, 16);
	shfs_vol.volname[17] = '\0'; /* ensure nullterminated volume name */
	shfs_vol.stripesize = hdr_common->member_stripesize;
	shfs_vol.chunksize = SHFS_CHUNKSIZE(hdr_common);
	shfs_vol.volsize = hdr_common->vol_size;

	/* Find and add members to the volume */
	shfs_vol.nb_members = 0;
	for (i = 0; i < hdr_common->member_count; i++) {
		for (m = 0; m < nb_detected_members; ++m) {
			if (uuid_compare(hdr_common->member[i].uuid, detected_member[m].uuid) == 0) {
				/* found device but was this member already added (malformed label)? */
				for (j = 0; j < shfs_vol.nb_members; ++j) {
					if (uuid_compare(shfs_vol.member[j].uuid,
					                 hdr_common->member[i].uuid) == 0)
						dief("A member is specified for multiple times for volume '%s'\n",
						     shfs_vol.volname);
				}
				shfs_vol.member[shfs_vol.nb_members].d = detected_member[m].d;
				uuid_copy(shfs_vol.member[shfs_vol.nb_members].uuid, detected_member[m].uuid);
				shfs_vol.nb_members++;
				continue;
			}
		}

	}
	if (shfs_vol.nb_members != count)
		dief("More members specified than actually required for volume '%s'\n", shfs_vol.volname);
	if (shfs_vol.nb_members != hdr_common->member_count)
		dief("Could not establish member mapping for volume '%s'\n", shfs_vol.volname);

	/* chunk and stripe size -> retrieve a device sector factor for each device */
	if (shfs_vol.stripesize < 4096 || !POWER_OF_2(shfs_vol.stripesize))
		dief("Stripe size invalid on volume '%s'\n", shfs_vol.volname);

	/* calculate and check volume size */
	min_member_size = (shfs_vol.volsize / shfs_vol.nb_members) * (uint64_t) shfs_vol.chunksize;
	for (i = 0; i < shfs_vol.nb_members; ++i) {
		if (shfs_vol.member[i].d->size < min_member_size)
			dief("Member %u of volume '%s' is too small\n", i, shfs_vol.volname);
	}

	free(chk0);
}

/**
 * This function loads the hash and allocator configuration from chunk 1
 * (as defined in SHFS)
 * This function can only be called, after load_vol_cconf
 * established successfully the low-level setup of a volume
 * (required for chunk I/O)
 */
static void load_vol_hconf(void)
{
	struct shfs_hdr_config *hdr_config;
	void *chk1;
	int ret;

	chk1 = malloc(shfs_vol.chunksize);
	if (!chk1)
		die();

	dprintf(D_L0, "Load SHFS configuration chunk\n");
	ret = shfs_sync_read_chunk(1, 1, chk1);
	if (ret < 0)
		die();

	hdr_config = chk1;
	shfs_vol.htable_ref                   = hdr_config->htable_ref;
	shfs_vol.htable_bak_ref               = hdr_config->htable_bak_ref;
	shfs_vol.htable_nb_buckets            = hdr_config->htable_bucket_count;
	shfs_vol.htable_nb_entries_per_bucket = hdr_config->htable_entries_per_bucket;
	shfs_vol.htable_nb_entries            = SHFS_HTABLE_NB_ENTRIES(hdr_config);
	shfs_vol.htable_nb_entries_per_chunk  = SHFS_HENTRIES_PER_CHUNK(shfs_vol.chunksize);
	shfs_vol.htable_len                   = SHFS_HTABLE_SIZE_CHUNKS(hdr_config, shfs_vol.chunksize);
	shfs_vol.hlen                         = hdr_config->hlen;
	shfs_vol.allocator                    = hdr_config->allocator;

	free(chk1);
}

/**
 * This function loads the hash table from the block device into memory
 * Note: load_vol_hconf() and local_vol_cconf() has to called before
 */
static void load_vol_htable(void)
{
	struct shfs_hentry *hentry;
	struct shfs_bentry *bentry;
	void *chk_buf;
	chk_t cur_chk, cur_htchk;
	unsigned int i;
	int ret;

	/* allocate bucket table */
	dprintf(D_L0, "Allocating btable...\n");
	shfs_vol.bt = shfs_alloc_btable(shfs_vol.htable_nb_buckets,
	                                shfs_vol.htable_nb_entries_per_bucket,
	                                shfs_vol.hlen);
	if (!shfs_vol.bt)
		die();

	/* allocate chunk cache reference table */
	dprintf(D_L0, "Allocating chunk cache reference table...\n");
	shfs_vol.htable_chunk_cache_state = malloc(sizeof(int) * shfs_vol.htable_len);
	shfs_vol.htable_chunk_cache       = calloc(1, sizeof(void *) * shfs_vol.htable_len);
	if (!shfs_vol.htable_chunk_cache_state || !shfs_vol.htable_chunk_cache)
		die();

	/* load hash table chunk-wise and fill-out btable metadata */
	dprintf(D_L0, "Reading hash table...\n");
	chk_buf = NULL;
	cur_chk = 0;
	for (i = 0; i < shfs_vol.htable_nb_entries; ++i) {
		cur_htchk = SHFS_HTABLE_CHUNK_NO(i, shfs_vol.htable_nb_entries_per_chunk);
		if (cur_chk != cur_htchk || !chk_buf) {
			chk_buf = malloc(shfs_vol.chunksize);
			if (!chk_buf)
				die();
			ret = shfs_sync_read_chunk(cur_htchk + shfs_vol.htable_ref, 1, chk_buf);
			if (ret < 0)
				dief("An error occured while reading the hash table from the volume\n");
			cur_chk = cur_htchk;

			/* register buffer to htable chunk cache */
			shfs_vol.htable_chunk_cache[cur_htchk]       = chk_buf;
			shfs_vol.htable_chunk_cache_state[cur_htchk] = 0x0;
		}

		bentry = shfs_btable_pick(shfs_vol.bt, i);
		bentry->hentry_htchunk  = cur_htchk;
		bentry->hentry_htoffset = SHFS_HTABLE_ENTRY_OFFSET(i, shfs_vol.htable_nb_entries_per_chunk);
		hentry = (struct shfs_hentry *)((uint8_t *) chk_buf + bentry->hentry_htoffset);
		hash_copy(bentry->hash, hentry->hash, shfs_vol.hlen);
	}
}

/**
 * Initialize allocator
 */
static void load_vol_alist(void)
{
	struct shfs_bentry *bentry;
	struct shfs_hentry *hentry;
	unsigned int i;
	int ret;

	dprintf(D_L0, "Initializing volume allocator...\n");
	shfs_vol.al = shfs_alloc_alist(shfs_vol.volsize, shfs_vol.allocator);
	if (!shfs_vol.al)
		dief("Could not initialize volume allocator: %s\n", strerror(errno));

	dprintf(D_L0, "Registering volume label region to allocator...\n");
	ret = shfs_alist_register(shfs_vol.al, 0, 2);
	if (ret < 0)
		dief("Could not register an allocator entry for boot chunk: %s\n", strerror(errno));
	dprintf(D_L0, "Registering hash table regions to allocator...\n");
	ret = shfs_alist_register(shfs_vol.al, shfs_vol.htable_ref, shfs_vol.htable_len);
	if (ret < 0)
		dief("Could not register an allocator entry for hash table: %s\n", strerror(errno));
	if (shfs_vol.htable_bak_ref) {
		ret = shfs_alist_register(shfs_vol.al, shfs_vol.htable_bak_ref, shfs_vol.htable_len);
		if (ret < 0)
			dief("Could not register an allocator entry for backup hash table: %s\n", strerror(errno));
	}

	dprintf(D_L0, "Registering containers to allocator...\n");
	for (i = 0; i < shfs_vol.htable_nb_entries; ++i) {
		bentry = shfs_btable_pick(shfs_vol.bt, i);

		if (!hash_is_zero(bentry->hash, shfs_vol.hlen)) {
			hentry = (struct shfs_hentry *)
				((uint8_t *) shfs_vol.htable_chunk_cache[bentry->hentry_htchunk]
				 + bentry->hentry_htoffset);
			shfs_alist_register(shfs_vol.al,
			                    hentry->chunk,
			                    DIV_ROUND_UP(hentry->offset + hentry->len,
			                                 shfs_vol.chunksize));
		}
	}
}


/**
 * Mount a SHFS volume
 * The volume is searched on the given list of VBD
 */
void mount_shfs(char *path[], unsigned int count)
{
	if (count == 0)
		dief("No devices passed\n");

	/* load common volume information and open devices */
	load_vol_cconf(path, count);

	/* load hash conf (uses shfs_sync_read_chunk) */
	load_vol_hconf();

	/* load htable (uses shfs_sync_read_chunk) */
	load_vol_htable();

	/* load and initialize allocator */
	load_vol_alist();
}

/**
 * Unmounts a previously mounted SHFS volume
 */
void umount_shfs(void) {
  unsigned int i;
  int ret;

  shfs_free_alist(shfs_vol.al);
  for(i = 0; i < shfs_vol.htable_len; ++i) {
    if (shfs_vol.htable_chunk_cache_state[i] & CCS_MODIFIED) {
      /* write buffer back to disk since it has been modified */
      ret = shfs_sync_write_chunk(shfs_vol.htable_ref + i,
                                  1,
                                  shfs_vol.htable_chunk_cache[i]);
      if (ret < 0)
	dief("An error occured while writing back the hash table to the volume!\n"
	     "The filesystem might be in a corrupted state right now\n");
      if (shfs_vol.htable_bak_ref) {
	ret = shfs_sync_write_chunk(shfs_vol.htable_bak_ref + i,
	                            1,
	                            shfs_vol.htable_chunk_cache[i]);
	if (ret < 0)
	  dief("An error occured while writing back the hash table to the volume!\n"
	       "The filesystem might be in a corrupted state right now\n");
      }
    }
    /* free buffer */
    free(shfs_vol.htable_chunk_cache[i]);
  }
  free(shfs_vol.htable_chunk_cache);
  free(shfs_vol.htable_chunk_cache_state);
  shfs_free_btable(shfs_vol.bt);
  for(i = 0; i < shfs_vol.nb_members; ++i)
    close_disk(shfs_vol.member[i].d);
}


/******************************************************************************
 * ACTIONS                                                                    *
 ******************************************************************************/

static int actn_addfile(struct job *j)
{
	struct shfs_bentry *bentry;
	struct shfs_hentry *hentry;
	char str_hash[(shfs_vol.hlen * 2) + 1];
	void *tmp_chk;
	struct stat fd_stat;
	int fd;
	int ret;
	size_t fsize;
	size_t rlen;
	chk_t csize;
	hash512_t fhash;
	chk_t cchk;
	MHASH td;
	uint64_t c;

	dprintf(D_L0, "Opening %s...\n", j->path);
	fd = open(j->path, O_RDONLY);
	if (fd < 0) {
		eprintf("Could not open %s: %s\n", j->path, strerror(errno));
		ret = -1;
		goto err;
	}
	ret = fstat(fd, &fd_stat);
	if (ret < 0) {
		eprintf("Could not retrieve stats from %s: %s\n", j->path, strerror(errno));
		goto err_close_fd;
	}
	if (!S_ISREG(fd_stat.st_mode)) {
		eprintf("%s is not a regular file\n", j->path);
		ret = -1;
		goto err_close_fd;
	}

	/* find and alloc container */
	fsize = fd_stat.st_size;
	csize = DIV_ROUND_UP(fsize, shfs_vol.chunksize);
	dprintf(D_L0, "Searching for an appropriate container to store file contents (%lu chunks)...\n", csize);
	cchk = shfs_alist_find_free(shfs_vol.al, csize);
	if (cchk == 0 || cchk >= shfs_vol.volsize) {
		eprintf("Could not find appropriate volume area to store %s\n", j->path);
		ret = -1;
		goto err_close_fd;
	}
	dprintf(D_L1, "Found appropriate container at chunk %lu\n", cchk);
	dprintf(D_L1, "Reserving container...\n", cchk);
	shfs_alist_register(shfs_vol.al, cchk, csize);

	/* allocate temporary buffer used for I/O */
	tmp_chk = malloc(shfs_vol.chunksize);
	if (!tmp_chk) {
		fatal();
		ret = -1;
		goto err_release_container;
	}

	/* calculate checksum */
	dprintf(D_L0, "Calculating hash of file contents...\n", csize);
	td = mhash_init(MHASH_SHA256);
	if (td == MHASH_FAILED) {
		eprintf("Could not initialize hash algorithm\n");
		ret = -1;
		goto err_free_tmp_chk;
	}
	if (lseek(fd, 0, SEEK_SET) < 0) {
		eprintf("Could not seek on %s: %s\n", j->path, strerror(errno));
		ret = -1;
		goto err_free_tmp_chk;
	}
	for (c = 0; c < csize; c++) {
		if (c == (csize - 1))
			rlen = fsize % shfs_vol.chunksize;
		else
			rlen = shfs_vol.chunksize;

		ret = read(fd, tmp_chk, rlen);
		if (ret < 0) {
			eprintf("Could not read from %s: %s\n", j->path, strerror(errno));
			ret = -1;
			goto err_mhash_deinit;
		}
		if (cancel) {
			ret = -2;
			goto err_mhash_deinit;
		}
		mhash(td, tmp_chk, rlen); /* hash chunk */
	}
	mhash_deinit(td, &fhash);
	if (verbosity >= D_L0) {
		str_hash[(shfs_vol.hlen * 2)] = '\0';
		hash_unparse(fhash, shfs_vol.hlen, str_hash);
		printf("Hash of %s is: %s\n",
		       j->path,
		       str_hash);
	}


	/* find place in hash list and add entry
	 * (still in-memory, will be written to device on umount) */
	dprintf(D_L0, "Trying to add a hash table entry...\n", csize);
	bentry = shfs_btable_lookup(shfs_vol.bt, fhash);
	if (bentry) {
		eprintf("An entry with the same hash already exists\n");
		ret = -1;
		goto err_free_tmp_chk;
	}
	bentry = shfs_btable_getfreeb(shfs_vol.bt, fhash);
	if (!bentry) {
		eprintf("Target bucket of hash table is full\n");
		ret = -1;
		goto err_free_tmp_chk;
	}
	hentry = (struct shfs_hentry *)
		((uint8_t *) shfs_vol.htable_chunk_cache[bentry->hentry_htchunk]
		 + bentry->hentry_htoffset);
	hash_copy(bentry->hash, fhash, shfs_vol.hlen);
	hash_copy(hentry->hash, fhash, shfs_vol.hlen);
	hentry->chunk = cchk;
	hentry->offset = 0;
	hentry->access_count = 0;
	hentry->len = (uint64_t) fsize;
	if (j->optstr0) { /* mime */
		memset(hentry->mime, 0, sizeof(hentry->mime));
		strncpy(hentry->mime, j->optstr0, sizeof(hentry->mime));
	}
	if (j->optstr1) { /* filename */
		memset(hentry->name, 0, sizeof(hentry->name));
		strncpy(hentry->name, j->optstr1, sizeof(hentry->name));
	}
	shfs_vol.htable_chunk_cache_state[bentry->hentry_htchunk] |= CCS_MODIFIED;

	/* copy file */
	dprintf(D_L0, "Copying file contents...\n", csize);
	if (lseek(fd, 0, SEEK_SET) < 0) {
		eprintf("Could not seek on %s: %s\n", j->path, strerror(errno));
		ret = -1;
		goto err_free_tmp_chk;
	}
	for (c = 0; c < csize; c++) {
		if (c == (csize - 1)) {
			rlen = fsize % shfs_vol.chunksize;
			memset(tmp_chk, 0, shfs_vol.chunksize);
		} else {
			rlen = shfs_vol.chunksize;
		}

		ret = read(fd, tmp_chk, rlen);
		if (ret < 0) {
			eprintf("Could not read from %s: %s\n", j->path, strerror(errno));
			ret = -1;
			goto err_free_tmp_chk;
		}

		ret = shfs_sync_write_chunk(cchk + c, 1, tmp_chk);
		if (ret < 0) {
			eprintf("Could not write to volume '%s': %s\n", shfs_vol.volname, strerror(errno));
			ret = -1;
			goto err_free_tmp_chk;
		}

		if (cancel) {
			ret = -2;
			goto err_free_tmp_chk;
		}
	}

	free(tmp_chk);
	close(fd);

	return 0;

 err_mhash_deinit:
	mhash_deinit(td, NULL);
 err_free_tmp_chk:
	free(tmp_chk);
 err_release_container:
	dprintf(D_L1, "Discard container reservation...\n", cchk);
	shfs_alist_unregister(shfs_vol.al, cchk, csize);
 err_close_fd:
	close(fd);
 err:
	return ret;
}

static int actn_rmfile(struct job *job)
{
	struct shfs_bentry *bentry;
	struct shfs_hentry *hentry;
	hash512_t h;
	int ret = 0;

	/* parse hash string */
	dprintf(D_L0, "Finding hash table entry of file %s...\n", job->path);
	ret = hash_parse(job->path, h, shfs_vol.hlen);
	if (ret < 0) {
		eprintf("Could not parse hash value\n");
		ret = -1;
		goto out;
	}
	/* find htable entry */
	bentry = shfs_btable_lookup(shfs_vol.bt, h);
	if (!bentry) {
		eprintf("No such entry found\n");
		ret = -1;
		goto out;
	}
	hentry = (struct shfs_hentry *)
		((uint8_t *) shfs_vol.htable_chunk_cache[bentry->hentry_htchunk]
		 + bentry->hentry_htoffset);

	/* release container */
	dprintf(D_L0, "Releasing container...\n");
	ret = shfs_alist_unregister(shfs_vol.al, hentry->chunk,
	                            DIV_ROUND_UP(hentry->len + hentry->offset,
	                                         shfs_vol.chunksize));
	if (ret < 0) {
		eprintf("Could not release container\n");
		ret = -1;
		goto out;
	}

	/* clear htable entry */
	dprintf(D_L0, "Clearing hash table entry...\n");
	hash_clear(hentry->hash, shfs_vol.hlen);
	hash_clear(bentry->hash, shfs_vol.hlen);
	shfs_vol.htable_chunk_cache_state[bentry->hentry_htchunk] |= CCS_MODIFIED;

 out:
	return ret;
}

static int actn_ls(struct job *job)
{
	struct shfs_bentry *bentry;
	struct shfs_hentry *hentry;
	char str_hash[(shfs_vol.hlen * 2) + 1];
	char str_mime[sizeof(hentry->mime) + 1];
	char str_name[sizeof(hentry->name) + 1];
	unsigned int i;

	str_hash[(shfs_vol.hlen * 2)] = '\0';
	str_name[sizeof(hentry->name)] = '\0';

	if (shfs_vol.hlen <= 32)
		printf("%-64s %12s %12s %-16s %s\n",
		       "Hash",
		       "At (chk)",
		       "Size (chk)",
		       "MIME",
		       "Name");
	else
		printf("%-128s %12s %12s %-16s %s\n",
		       "Hash",
		       "At (chk)",
		       "Size (chk)",
		       "MIME",
		       "Name");
	for (i = 0; i < shfs_vol.htable_nb_entries; ++i) {
		bentry = shfs_btable_pick(shfs_vol.bt, i);
		hentry = (struct shfs_hentry *)
			((uint8_t *) shfs_vol.htable_chunk_cache[bentry->hentry_htchunk]
			 + bentry->hentry_htoffset);
		if (!hash_is_zero(bentry->hash, shfs_vol.hlen)) {
			hash_unparse(bentry->hash, shfs_vol.hlen, str_hash);
			strncpy(str_name, hentry->name, sizeof(hentry->name));
			strncpy(str_mime, hentry->mime, sizeof(hentry->mime));
			if (shfs_vol.hlen <= 32)
				printf("%-64s %12lu %12lu %-16s %s\n",
				       str_hash,
				       hentry->chunk,
				       DIV_ROUND_UP(hentry->len + hentry->offset, shfs_vol.chunksize),
				       str_mime,
				       str_name);
			else
				printf("%-128s %12lu %12lu %-16s %s\n",
				       str_hash,
				       hentry->chunk,
				       DIV_ROUND_UP(hentry->len + hentry->offset, shfs_vol.chunksize),
				       str_mime,
				       str_name);
		}
	}

	return 0;
}

static int actn_info(struct job *job)
{
	void *chk0;
	void *chk1;
	struct shfs_hdr_common *hdr_common;
	struct shfs_hdr_config *hdr_config;
	int ret = 0;

	chk0 = malloc(4096);
	if (!hdr_config) {
		fatal();
		ret = -1;
		goto out;
	}
	chk1 = malloc(shfs_vol.chunksize);
	if (!hdr_config) {
		fatal();
		ret = -1;
		goto out_free_chk0;
	}

	/* read first chunk from first member (considered as 4K) */
	if (lseek(shfs_vol.member[0].d->fd, 0, SEEK_SET) < 0) {
		eprintf("Could not seek on %s: %s\n", shfs_vol.member[0].d->path, strerror(errno));
		ret = -1;
		goto out_free_chk1;
	}
	if (read(shfs_vol.member[0].d->fd, chk0, 4096) < 0) {
		eprintf("Could not read from %s: %s\n", shfs_vol.member[0].d->path, strerror(errno));
		ret = -1;
		goto out_free_chk1;
	}

	/* read second chunk */
	dprintf(D_L0, "Load SHFS configuration chunk\n");
	ret = shfs_sync_read_chunk(1, 1, chk1);
	if (ret < 0) {
		fatal();
		ret = -1;
		goto out_free_chk1;
	}

	hdr_common = (void *)((uint8_t *) chk0 + BOOT_AREA_LENGTH);
	hdr_config = chk1;
	print_shfs_hdr_summary(hdr_common, hdr_config);

 out_free_chk1:
	free(chk1);
 out_free_chk0:
	free(chk0);
 out:
	return ret;
}


/******************************************************************************
 * MAIN                                                                       *
 ******************************************************************************/
int main(int argc, char **argv)
{
	struct args args;
	struct job *cjob;
	unsigned int i;
	unsigned int failed;
	int ret;

	signal(SIGINT,  sigint_handler);
	signal(SIGTERM, sigint_handler);
	signal(SIGQUIT, sigint_handler);

	/*
	 * ARGUMENT PARSING
	 */
	memset(&args, 0, sizeof(args));
	if (parse_args(argc, argv, &args) < 0)
		exit(EXIT_FAILURE);
	if (verbosity > 0)
		eprintf("Verbosity increased to level %d.\n", verbosity);

	/*
	 * MAIN
	 */
	if (cancel)
		exit(-2);
	mount_shfs(args.devpath, args.nb_devs);
	failed = 0;
	i = 0;
	for (cjob = args.jobs; cjob != NULL; cjob = cjob->next) {
		if (cancel)
			break;

		switch (cjob->action) {
		case ADDFILE:
			dprintf(D_L0, "*** Job %u: add-file\n", i);
			printf("Adding %s...\n", cjob->path);
			ret = actn_addfile(cjob);
			break;
		case RMFILE:
			dprintf(D_L0, "*** Job %u: rm-file\n", i);
			printf("Deleting %s...\n", cjob->path);
			ret = actn_rmfile(cjob);
			break;
		case LSFILES:
			dprintf(D_L0, "*** Job %u: ls\n", i);
			ret = actn_ls(cjob);
			break;
		case SHOWINFO:
			dprintf(D_L0, "*** Job %u: info\n", i);
			ret = actn_info(cjob);
			break;
		default:
			ret = 0;
			break; /* unsupported job but should never happen */
		}

		if (ret < 0) {
			eprintf("Error: %d\n", i, ret);
			failed++;
		}
		i++;
	}
	dprintf(D_L1, "*** %u jobs executed on volume '%s'\n", i, shfs_vol.volname);
	umount_shfs();

	if (cancel)
		exit(-2);
	if (failed)
		eprintf("Some commands failed\n");
	/*
	 * EXIT
	 */
	release_args(&args);
	if (failed)
		exit(EXIT_FAILURE);
	exit(EXIT_SUCCESS);
}