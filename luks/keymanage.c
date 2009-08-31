/*
 * LUKS - Linux Unified Key Setup 
 *
 * Copyright (C) 2004-2006, Clemens Fruhwirth <clemens@endorphin.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <sys/ioctl.h>
#include <linux/fs.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "luks.h"
#include "af.h"
#include "pbkdf.h"
#include "random.h"
#include <uuid/uuid.h>
#include <../lib/internal.h>

#define div_round_up(a,b) ({           \
	typeof(a) __a = (a);          \
	typeof(b) __b = (b);          \
	(__a - 1) / __b + 1;        \
})

static inline int round_up_modulo(int x, int m) {
	return div_round_up(x, m) * m;
}

struct luks_masterkey *LUKS_alloc_masterkey(int keylength, const char *key)
{ 
	struct luks_masterkey *mk=malloc(sizeof(*mk) + keylength);
	if(NULL == mk) return NULL;
	mk->keyLength=keylength;
	if (key)
		memcpy(&mk->key, key, keylength);
	return mk;
}

void LUKS_dealloc_masterkey(struct luks_masterkey *mk)
{
	if(NULL != mk) {
		memset(mk->key,0,mk->keyLength);
		mk->keyLength=0;
		free(mk);
	}
}

struct luks_masterkey *LUKS_generate_masterkey(int keylength)
{
	struct luks_masterkey *mk=LUKS_alloc_masterkey(keylength, NULL);
	if(NULL == mk) return NULL;

	int r = getRandom(mk->key,keylength);
	if(r < 0) {
		LUKS_dealloc_masterkey(mk);
		return NULL;
	}
	return mk;
}

int LUKS_read_phdr(const char *device,
		   struct luks_phdr *hdr,
		   int require_luks_device,
		   struct crypt_device *ctx)
{
	int devfd = 0, r = 0;
	unsigned int i;
	uint64_t size;
	char luksMagic[] = LUKS_MAGIC;

	log_dbg("Reading LUKS header of size %d from device %s",
		sizeof(struct luks_phdr), device);

	devfd = open(device,O_RDONLY | O_DIRECT | O_SYNC);
	if(-1 == devfd) {
		log_err(ctx, _("Cannot open device %s.\n"), device);
		return -EINVAL; 
	}

	if(read_blockwise(devfd, hdr, sizeof(struct luks_phdr)) < sizeof(struct luks_phdr)) {
		r = -EIO;
	} else if(memcmp(hdr->magic, luksMagic, LUKS_MAGIC_L)) { /* Check magic */
		log_dbg("LUKS header not detected.");
		if (require_luks_device)
			log_err(ctx, _("%s is not a LUKS device.\n"), device);
		else
			set_error(_("%s is not a LUKS device."), device);
		r = -EINVAL;
	} else if((hdr->version = ntohs(hdr->version)) != 1) {	/* Convert every uint16/32_t item from network byte order */
		log_err(ctx, _("Unsupported LUKS version %d.\n"), hdr->version);
		r = -EINVAL;
	} else if (PBKDF2_HMAC_ready(hdr->hashSpec) < 0) {
		log_err(ctx, _("Requested LUKS hash %s is not supported.\n"), hdr->hashSpec);
		r = -EINVAL;
	} else {
		hdr->payloadOffset      = ntohl(hdr->payloadOffset);
		hdr->keyBytes           = ntohl(hdr->keyBytes);
		hdr->mkDigestIterations = ntohl(hdr->mkDigestIterations);

		for(i = 0; i < LUKS_NUMKEYS; ++i) {
			hdr->keyblock[i].active             = ntohl(hdr->keyblock[i].active);
			hdr->keyblock[i].passwordIterations = ntohl(hdr->keyblock[i].passwordIterations);
			hdr->keyblock[i].keyMaterialOffset  = ntohl(hdr->keyblock[i].keyMaterialOffset);
			hdr->keyblock[i].stripes            = ntohl(hdr->keyblock[i].stripes);
		}
	}

#ifdef BLKGETSIZE64
	if (r == 0 && (ioctl(devfd, BLKGETSIZE64, &size) < 0 ||
	    size < (uint64_t)hdr->payloadOffset)) {
		log_err(ctx, _("LUKS header detected but device %s is too small.\n"), device);
		r = -EINVAL;
	}
#endif
	close(devfd);

	return r;
}

int LUKS_write_phdr(const char *device,
		    struct luks_phdr *hdr,
		    struct crypt_device *ctx)
{
	int devfd = 0; 
	unsigned int i; 
	struct luks_phdr convHdr;
	int r;

	log_dbg("Updating LUKS header of size %d on device %s",
		sizeof(struct luks_phdr), device);

	devfd = open(device,O_RDWR | O_DIRECT | O_SYNC);
	if(-1 == devfd) { 
		log_err(ctx, _("Cannot open device %s.\n"), device);
		return -EINVAL;
	}

	memcpy(&convHdr, hdr, sizeof(struct luks_phdr));
	memset(&convHdr._padding, 0, sizeof(convHdr._padding));

	/* Convert every uint16/32_t item to network byte order */
	convHdr.version            = htons(hdr->version);
	convHdr.payloadOffset      = htonl(hdr->payloadOffset);
	convHdr.keyBytes           = htonl(hdr->keyBytes);
	convHdr.mkDigestIterations = htonl(hdr->mkDigestIterations);
	for(i = 0; i < LUKS_NUMKEYS; ++i) {
		convHdr.keyblock[i].active             = htonl(hdr->keyblock[i].active);
		convHdr.keyblock[i].passwordIterations = htonl(hdr->keyblock[i].passwordIterations);
		convHdr.keyblock[i].keyMaterialOffset  = htonl(hdr->keyblock[i].keyMaterialOffset);
		convHdr.keyblock[i].stripes            = htonl(hdr->keyblock[i].stripes);
	}

	r = write_blockwise(devfd, &convHdr, sizeof(struct luks_phdr)) < sizeof(struct luks_phdr) ? -EIO : 0;
	if (r)
		log_err(ctx, _("Error during update of LUKS header on device %s.\n"), device);
	close(devfd);

	/* Re-read header from disk to be sure that in-memory and on-disk data are the same. */
	if (!r) {
		r = LUKS_read_phdr(device, hdr, 1, ctx);
		if (r)
			log_err(ctx, _("Error re-reading LUKS header after update on device %s.\n"), device);
	}

	return r;
}

int LUKS_generate_phdr(struct luks_phdr *header,
		       const struct luks_masterkey *mk,
		       const char *cipherName, const char *cipherMode, const char *hashSpec,
		       const char *uuid, unsigned int stripes,
		       unsigned int alignPayload,
		       struct crypt_device *ctx)
{
	unsigned int i=0;
	unsigned int blocksPerStripeSet = div_round_up(mk->keyLength*stripes,SECTOR_SIZE);
	int r;
	char luksMagic[] = LUKS_MAGIC;
	uuid_t partitionUuid;
	int currentSector;
	int alignSectors = 4096/SECTOR_SIZE;
	if (alignPayload == 0)
		alignPayload = alignSectors;

	memset(header,0,sizeof(struct luks_phdr));

	/* Set Magic */
	memcpy(header->magic,luksMagic,LUKS_MAGIC_L);
	header->version=1;
	strncpy(header->cipherName,cipherName,LUKS_CIPHERNAME_L);
	strncpy(header->cipherMode,cipherMode,LUKS_CIPHERMODE_L);
	strncpy(header->hashSpec,hashSpec,LUKS_HASHSPEC_L);

	header->keyBytes=mk->keyLength;

	log_dbg("Generating LUKS header version %d using hash %s, %s, %s, MK %d bytes",
		header->version, header->hashSpec ,header->cipherName, header->cipherMode,
		header->keyBytes);

	r = getRandom(header->mkDigestSalt,LUKS_SALTSIZE);
	if(r < 0) {
		log_err(ctx,  _("Cannot create LUKS header: reading random salt failed.\n"));
		return r;
	}

	/* Compute master key digest */
	header->mkDigestIterations = LUKS_MKD_ITER;
	r = PBKDF2_HMAC(header->hashSpec,mk->key,mk->keyLength,
			header->mkDigestSalt,LUKS_SALTSIZE,
			header->mkDigestIterations,
			header->mkDigest,LUKS_DIGESTSIZE);
	if(r < 0) {
		log_err(ctx,  _("Cannot create LUKS header: header digest failed (using hash %s).\n"),
			header->hashSpec);
		return r;
	}

	currentSector = round_up_modulo(LUKS_PHDR_SIZE, alignSectors);
	for(i = 0; i < LUKS_NUMKEYS; ++i) {
		header->keyblock[i].active = LUKS_KEY_DISABLED;
		header->keyblock[i].keyMaterialOffset = currentSector;
		header->keyblock[i].stripes = stripes;
		currentSector = round_up_modulo(currentSector + blocksPerStripeSet, alignSectors);
	}
	currentSector = round_up_modulo(currentSector, alignPayload);

	header->payloadOffset=currentSector;

	if (uuid && !uuid_parse(uuid, partitionUuid)) {
		log_err(ctx, _("Wrong UUID format provided, generating new one.\n"));
		uuid = NULL;
	}
	if (!uuid)
		uuid_generate(partitionUuid);
        uuid_unparse(partitionUuid, header->uuid);

	log_dbg("Data offset %d, UUID %s", header->payloadOffset, header->uuid);

	return 0;
}

int LUKS_set_key(const char *device, unsigned int keyIndex,
		 const char *password, size_t passwordLen,
		 struct luks_phdr *hdr, struct luks_masterkey *mk,
		 uint32_t iteration_time_ms,
		 uint64_t *PBKDF2_per_sec,
		 struct crypt_device *ctx)
{
	char derivedKey[hdr->keyBytes];
	char *AfKey;
	unsigned int AFEKSize;
	int r;

	if(hdr->keyblock[keyIndex].active != LUKS_KEY_DISABLED) {
		log_err(ctx,  _("Key slot %d active, purge first.\n"), keyIndex);
		return -EINVAL;
	}

	if(hdr->keyblock[keyIndex].stripes < LUKS_STRIPES) {
	        log_err(ctx, _("Key slot %d material includes too few stripes. Header manipulation?\n"),
			keyIndex);
	         return -EINVAL;
	}

	log_dbg("Calculating data for key slot %d", keyIndex);

	if (!*PBKDF2_per_sec) {
		if (PBKDF2_performance_check(hdr->hashSpec, PBKDF2_per_sec) < 0) {
			log_err(ctx, _("Not compatible PBKDF2 options (using hash algorithm %s)."), hdr->hashSpec);
			return -EINVAL;
		}
		log_dbg("PBKDF2: %" PRIu64 " iterations per second using hash %s.", *PBKDF2_per_sec, hdr->hashSpec);
	}

	/* Avoid floating point operation - don't tell anyone that second have no 1024 miliseconds :-) */
	iteration_time_ms = at_least_one(iteration_time_ms / 1024);
	hdr->keyblock[keyIndex].passwordIterations = at_least_one((uint32_t)(*PBKDF2_per_sec/2) * iteration_time_ms);
	log_dbg("Key slot %d use %d password iterations.", keyIndex, hdr->keyblock[keyIndex].passwordIterations);

	r = getRandom(hdr->keyblock[keyIndex].passwordSalt, LUKS_SALTSIZE);
	if(r < 0) return r;

//	assert((mk->keyLength % TWOFISH_BLOCKSIZE) == 0); FIXME

	r = PBKDF2_HMAC(hdr->hashSpec, password,passwordLen,
			hdr->keyblock[keyIndex].passwordSalt,LUKS_SALTSIZE,
			hdr->keyblock[keyIndex].passwordIterations,
			derivedKey, hdr->keyBytes);
	if(r < 0) return r;

	/*
	 * AF splitting, the masterkey stored in mk->key is splitted to AfMK
	 */
	AFEKSize = hdr->keyblock[keyIndex].stripes*mk->keyLength;
	AfKey = (char *)malloc(AFEKSize);
	if(AfKey == NULL) return -ENOMEM;

	log_dbg("Using hash %s for AF in key slot %d, %d stripes",
		hdr->hashSpec, keyIndex, hdr->keyblock[keyIndex].stripes);
	r = AF_split(mk->key,AfKey,mk->keyLength,hdr->keyblock[keyIndex].stripes,hdr->hashSpec);
	if(r < 0) goto out;

	log_dbg("Updating key slot %d [0x%04x] area on device %s.", keyIndex,
		hdr->keyblock[keyIndex].keyMaterialOffset << 9, device);
	/* Encryption via dm */
	r = LUKS_encrypt_to_storage(AfKey,
				    AFEKSize,
				    hdr,
				    derivedKey,
				    hdr->keyBytes,
				    device,
				    hdr->keyblock[keyIndex].keyMaterialOffset,
				    ctx);
	if(r < 0) {
		if(!get_error())
			log_err(ctx, _("Failed to write to key storage.\n"));
		goto out;
	}

	/* Mark the key as active in phdr */
	r = LUKS_keyslot_set(hdr, (int)keyIndex, 1);
	if(r < 0) goto out;

	r = LUKS_write_phdr(device, hdr, ctx);
	if(r < 0) goto out;

	r = 0;
out:
	free(AfKey);
	return r;
}

/* Check whether a master key is invalid. */
int LUKS_verify_master_key(const struct luks_phdr *hdr,
			   const struct luks_masterkey *mk)
{
	char checkHashBuf[LUKS_DIGESTSIZE];

	if (PBKDF2_HMAC(hdr->hashSpec, mk->key, mk->keyLength,
			hdr->mkDigestSalt, LUKS_SALTSIZE,
			hdr->mkDigestIterations, checkHashBuf,
			LUKS_DIGESTSIZE) < 0)
		return -EINVAL;

	if (memcmp(checkHashBuf, hdr->mkDigest, LUKS_DIGESTSIZE))
		return -EPERM;

	return 0;
}

/* Try to open a particular key slot */
int LUKS_open_key(const char *device,
		  unsigned int keyIndex,
		  const char *password,
		  size_t passwordLen,
		  struct luks_phdr *hdr,
		  struct luks_masterkey *mk,
		  struct crypt_device *ctx)
{
	crypt_keyslot_info ki = LUKS_keyslot_info(hdr, keyIndex);
	char derivedKey[hdr->keyBytes];
	char *AfKey;
	size_t AFEKSize;
	int r;

	log_dbg("Trying to open key slot %d [%d].", keyIndex, (int)ki);

	if (ki < SLOT_ACTIVE)
		return -ENOENT;

	// assert((mk->keyLength % TWOFISH_BLOCKSIZE) == 0); FIXME

	AFEKSize = hdr->keyblock[keyIndex].stripes*mk->keyLength;
	AfKey = (char *)malloc(AFEKSize);
	if(AfKey == NULL) return -ENOMEM;

	r = PBKDF2_HMAC(hdr->hashSpec, password,passwordLen,
			hdr->keyblock[keyIndex].passwordSalt,LUKS_SALTSIZE,
			hdr->keyblock[keyIndex].passwordIterations,
			derivedKey, hdr->keyBytes);
	if(r < 0) goto out;

	log_dbg("Reading key slot %d area.", keyIndex);
	r = LUKS_decrypt_from_storage(AfKey,
				      AFEKSize,
				      hdr,
				      derivedKey,
				      hdr->keyBytes,
				      device,
				      hdr->keyblock[keyIndex].keyMaterialOffset,
				      ctx);
	if(r < 0) {
		log_err(ctx, _("Failed to read from key storage.\n"));
		goto out;
	}

	r = AF_merge(AfKey,mk->key,mk->keyLength,hdr->keyblock[keyIndex].stripes,hdr->hashSpec);
	if(r < 0) goto out;

	r = LUKS_verify_master_key(hdr, mk);
	if (r >= 0)
		log_std(ctx, _("Key slot %d unlocked.\n"), keyIndex);
out:
	free(AfKey);
	return r;
}

int LUKS_open_key_with_hdr(const char *device,
			   int keyIndex,
			   const char *password,
			   size_t passwordLen,
			   struct luks_phdr *hdr,
			   struct luks_masterkey **mk,
			   struct crypt_device *ctx)
{
	unsigned int i;
	int r;

	*mk = LUKS_alloc_masterkey(hdr->keyBytes, NULL);

	if (keyIndex >= 0)
		return LUKS_open_key(device, keyIndex, password, passwordLen, hdr, *mk, ctx);

	for(i = 0; i < LUKS_NUMKEYS; i++) {
		r = LUKS_open_key(device, i, password, passwordLen, hdr, *mk, ctx);
		if(r == 0)
			return i;

		/* Do not retry for errors that are no -EPERM or -ENOENT,
		   former meaning password wrong, latter key slot inactive */
		if ((r != -EPERM) && (r != -ENOENT)) 
			return r;
	}
	/* Warning, early returns above */
	log_err(ctx, _("No key available with this passphrase.\n"));
	return -EPERM;
}

/*
 * Wipe patterns according to Gutmann's Paper
 */

static void wipeSpecial(char *buffer, size_t buffer_size, unsigned int turn)
{
        unsigned int i;

        unsigned char write_modes[][3] = {
                {"\x55\x55\x55"}, {"\xaa\xaa\xaa"}, {"\x92\x49\x24"},
                {"\x49\x24\x92"}, {"\x24\x92\x49"}, {"\x00\x00\x00"},
                {"\x11\x11\x11"}, {"\x22\x22\x22"}, {"\x33\x33\x33"},
                {"\x44\x44\x44"}, {"\x55\x55\x55"}, {"\x66\x66\x66"},
                {"\x77\x77\x77"}, {"\x88\x88\x88"}, {"\x99\x99\x99"},
                {"\xaa\xaa\xaa"}, {"\xbb\xbb\xbb"}, {"\xcc\xcc\xcc"},
                {"\xdd\xdd\xdd"}, {"\xee\xee\xee"}, {"\xff\xff\xff"},
                {"\x92\x49\x24"}, {"\x49\x24\x92"}, {"\x24\x92\x49"},
                {"\x6d\xb6\xdb"}, {"\xb6\xdb\x6d"}, {"\xdb\x6d\xb6"}
        };

        for(i = 0; i < buffer_size / 3; ++i) {
                memcpy(buffer, write_modes[turn], 3);
                buffer += 3;
        }
}

static int wipe(const char *device, unsigned int from, unsigned int to)
{
	int devfd;
	char *buffer;
	unsigned int i;
	unsigned int bufLen = (to - from) * SECTOR_SIZE;
	int r = 0;

	devfd = open(device, O_RDWR | O_DIRECT | O_SYNC);
	if(devfd == -1)
		return -EINVAL;

	buffer = (char *) malloc(bufLen);
	if(!buffer) return -ENOMEM;

	for(i = 0; i < 39; ++i) {
		if     (i >=  0 && i <  5) getRandom(buffer, bufLen);
		else if(i >=  5 && i < 32) wipeSpecial(buffer, bufLen, i - 5);
		else if(i >= 32 && i < 38) getRandom(buffer, bufLen);
		else if(i >= 38 && i < 39) memset(buffer, 0xFF, bufLen);

		if(write_lseek_blockwise(devfd, buffer, bufLen, from * SECTOR_SIZE) < 0) {
			r = -EIO;
			break;
		}
	}

	free(buffer);
	close(devfd);

	return r;
}

int LUKS_del_key(const char *device,
		 unsigned int keyIndex,
		 struct luks_phdr *hdr,
		 struct crypt_device *ctx)
{
	unsigned int startOffset, endOffset, stripesLen;
	int r;

	r = LUKS_read_phdr(device, hdr, 1, ctx);
	if (r)
		return r;

	r = LUKS_keyslot_set(hdr, keyIndex, 0);
	if (r) {
		log_err(ctx, _("Key slot %d is invalid, please select keyslot between 0 and %d.\n"),
			keyIndex, LUKS_NUMKEYS - 1);
		return r;
	}

	/* secure deletion of key material */
	startOffset = hdr->keyblock[keyIndex].keyMaterialOffset;
	stripesLen = hdr->keyBytes * hdr->keyblock[keyIndex].stripes;
	endOffset = startOffset + div_round_up(stripesLen, SECTOR_SIZE);

	r = wipe(device, startOffset, endOffset);
	if (r) {
		log_err(ctx, _("Cannot wipe device %s.\n"), device);
		return r;
	}

	r = LUKS_write_phdr(device, hdr, ctx);

	return r;
}

crypt_keyslot_info LUKS_keyslot_info(struct luks_phdr *hdr, int keyslot)
{
	int i;

	if(keyslot >= LUKS_NUMKEYS || keyslot < 0)
		return SLOT_INVALID;

	if (hdr->keyblock[keyslot].active == LUKS_KEY_DISABLED)
		return SLOT_INACTIVE;

	if (hdr->keyblock[keyslot].active != LUKS_KEY_ENABLED)
		return SLOT_INVALID;

	for(i = 0; i < LUKS_NUMKEYS; i++)
		if(i != keyslot && hdr->keyblock[i].active == LUKS_KEY_ENABLED)
			return SLOT_ACTIVE;

	return SLOT_ACTIVE_LAST;
}

int LUKS_keyslot_find_empty(struct luks_phdr *hdr)
{
	int i;

	for (i = 0; i < LUKS_NUMKEYS; i++)
		if(hdr->keyblock[i].active == LUKS_KEY_DISABLED)
			break;

	if (i == LUKS_NUMKEYS)
		return -EINVAL;

	return i;
}

int LUKS_keyslot_active_count(struct luks_phdr *hdr)
{
	int i, num = 0;

	for (i = 0; i < LUKS_NUMKEYS; i++)
		if(hdr->keyblock[i].active == LUKS_KEY_ENABLED)
			num++;

	return num;
}

int LUKS_keyslot_set(struct luks_phdr *hdr, int keyslot, int enable)
{
	crypt_keyslot_info ki = LUKS_keyslot_info(hdr, keyslot);

	if (ki == SLOT_INVALID)
		return -EINVAL;

	hdr->keyblock[keyslot].active = enable ? LUKS_KEY_ENABLED : LUKS_KEY_DISABLED;
	log_dbg("Key slot %d was %s in LUKS header.", keyslot, enable ? "enabled" : "disabled");
	return 0;
}
