/*
 * Copyright 2010-2012 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <fsl_validate.h>
#include <fsl_secboot_err.h>
#include <asm/fsl_sfp_snvs.h>
#include <command.h>
#include <common.h>
#include <malloc.h>
#include <rsa_sec.h>
#include <sha.h>
#include <jr.h>
#include <jobdesc.h>
#include <asm/fsl_pamu.h>

#define SHA256_BITS	256
#define SHA256_BYTES	(256/8)
#define SHA256_NIBBLES	(256/4)
#define NUM_HEX_CHARS	(sizeof(ulong) * 2)

extern struct jobring jr;

/* This array contains DER value for SHA-256 */
static const u8 hash_identifier[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60,
		0x86, 0x48, 0x01, 0x65,	0x03, 0x04, 0x02, 0x01, 0x05, 0x00,
		0x04, 0x20
		};

static u8 hash_val[SHA256_BYTES];
static const u8 barker_code[ESBC_BARKER_LEN] = { 0x68, 0x39, 0x27, 0x81 };

void branch_to_self(void) __attribute__ ((noreturn));

/*
 * This function will put core in infinite loop.
 * This will be called when the ESBC can not proceed further due
 * to some errors.
 */
void branch_to_self(void)
{
	printf("Core is in infinite loop due to errors.\n");
	while (1);
}

#ifdef CONFIG_KEY_REVOCATION
/* This function checks srk_table_flag in header and set/reset srk_flag.*/
static u32 check_srk(struct fsl_secboot_img_priv *img)
{
	if (img->hdr.len_kr.srk_table_flag & SRK_FLAG)
		return 1;

	return 0;
}

/* This function returns ospr's key_revoc values.*/
static u32 get_key_revoc(void)
{
	ccsr_sfp_regs_t *sfp_regs = (void *)(CONFIG_SYS_SFP_ADDR);
	return (in_be32(&sfp_regs->ospr) & OSPR_KEY_REVOC_MASK) >>
		OSPR_KEY_REVOC_SHIFT;
}

/* This function checks if selected key is revoked or not.*/
static u32 is_key_revoked(u32 keynum, u32 rev_flag)
{
	if (keynum == UNREVOCABLE_KEY)
		return 0;

	if ((u32)(1 << (ALIGN_REVOC_KEY - keynum)) & rev_flag)
		return 1;

	return 0;
}

/* It validates srk_table key lengths.*/
static u32 validate_srk_tbl(struct srk_table *tbl, u32 num_entries)
{
	int i = 0;
	for (i = 0; i < num_entries; i++) {
		if (!((tbl[i].key_len == 2 * KEY_SIZE_BYTES/4) ||
		      (tbl[i].key_len == 2 * KEY_SIZE_BYTES/2) ||
		      (tbl[i].key_len == 2 * KEY_SIZE_BYTES)))
			return ERROR_ESBC_CLIENT_HEADER_INV_SRK_ENTRY_KEYLEN;
	}
	return 0;
}
#endif

/* This function return length of public key.*/
static u32 get_key_len(struct fsl_secboot_img_priv *img)
{
	u32 key_len;
#ifdef CONFIG_KEY_REVOCATION
	if (check_srk(img))
		key_len = img->srk_tbl[img->hdr.len_kr.srk_sel - 1].key_len;
	else
		key_len = img->hdr.key_len;
#else
	key_len = img->hdr.key_len;
#endif
	return key_len;
}

/*
 * Handles the ESBC uboot client header verification failure.
 * This  function  handles all the errors which might occur in the
 * parsing and checking of ESBC uboot client header. It will also
 * set the error bits in the SNVS.
 */
static void fsl_secboot_header_verification_failure(void)
{
	ccsr_snvs_regs_t *snvs_regs = (void *)(CONFIG_SYS_SNVS_ADDR);
	ccsr_sfp_regs_t *sfp_regs = (void *)(CONFIG_SYS_SFP_ADDR);
	u32 sts = in_be32(&snvs_regs->hp_stat);

	/* 29th bit of OSPR is ITS */
	u32 its = in_be32(&sfp_regs->ospr) >> 2;

	/*
	 * Read the SNVS status register
	 * Read SSM_ST field
	 */
	sts = in_be32(&snvs_regs->hp_stat);
	if ((sts & HPSR_SSM_ST_MASK) == HPSR_SSM_ST_TRUST) {
		if (its == 1)
			change_sec_mon_state(HPSR_SSM_ST_TRUST,
				HPSR_SSM_ST_SOFT_FAIL);
		else
			change_sec_mon_state(HPSR_SSM_ST_TRUST,
				HPSR_SSM_ST_NON_SECURE);
	}

	generate_reset_req();
}

/*
 * Handles the ESBC uboot client image verification failure.
 * This  function  handles all the errors which might occur in the
 * public key hash comparison and signature verification of
 * ESBC uboot client image. It will also
 * set the error bits in the SNVS.
 */
static void fsl_secboot_image_verification_failure(void)
{
	ccsr_snvs_regs_t *snvs_regs = (void *)(CONFIG_SYS_SNVS_ADDR);
	ccsr_sfp_regs_t *sfp_regs = (void *)(CONFIG_SYS_SFP_ADDR);
	u32 sts = in_be32(&snvs_regs->hp_stat);

	u32 its = in_be32(&sfp_regs->ospr) & ITS_MASK >> ITS_BIT;

	/*
	 * Read the SNVS status register
	 * Read SSM_ST field
	 */
	sts = in_be32(&snvs_regs->hp_stat);
	if ((sts & HPSR_SSM_ST_MASK) == HPSR_SSM_ST_TRUST) {
		if (its == 1) {
			change_sec_mon_state(HPSR_SSM_ST_TRUST,
				HPSR_SSM_ST_SOFT_FAIL);

			generate_reset_req();
		} else {
			change_sec_mon_state(HPSR_SSM_ST_TRUST,
				HPSR_SSM_ST_NON_SECURE);
		}
	}
}

static void fsl_secboot_bootscript_parse_failure(void)
{
	fsl_secboot_header_verification_failure();
}

/*
 * Handles the errors in esbc boot.
 * This  function  handles all the errors which might occur in the
 * esbc boot phase. It will call the appropriate api to log the
 * errors and set the error bits in the SNVS.
 */
void fsl_secboot_handle_error(int error)
{
	const struct fsl_secboot_errcode *e;

	for (e = fsl_secboot_errcodes; e->errcode != ERROR_ESBC_CLIENT_MAX;
		e++) {
		if (e->errcode == error)
			printf("ERROR :: %x :: %s\n", error, e->name);
	}

	switch (error) {
	case ERROR_ESBC_CLIENT_HEADER_BARKER:
	case ERROR_ESBC_CLIENT_HEADER_IMG_SIZE:
	case ERROR_ESBC_CLIENT_HEADER_KEY_LEN:
	case ERROR_ESBC_CLIENT_HEADER_SIG_LEN:
	case ERROR_ESBC_CLIENT_HEADER_KEY_LEN_NOT_TWICE_SIG_LEN:
	case ERROR_ESBC_CLIENT_HEADER_KEY_MOD_1:
	case ERROR_ESBC_CLIENT_HEADER_KEY_MOD_2:
	case ERROR_ESBC_CLIENT_HEADER_SIG_KEY_MOD:
	case ERROR_ESBC_CLIENT_HEADER_SG_ESBC_EP:
	case ERROR_ESBC_CLIENT_HEADER_SG_ENTIRES_BAD:
#ifdef CONFIG_KEY_REVOCATION
	case ERROR_ESBC_CLIENT_HEADER_KEY_REVOKED:
	case ERROR_ESBC_CLIENT_HEADER_INVALID_SRK_NUM_ENTRY:
	case ERROR_ESBC_CLIENT_HEADER_INVALID_KEY_NUM:
	case ERROR_ESBC_CLIENT_HEADER_INV_SRK_ENTRY_KEYLEN:
#endif
		fsl_secboot_header_verification_failure();
		break;
	case ERROR_ESBC_SEC_RESET:
	case ERROR_ESBC_SEC_DEQ:
	case ERROR_ESBC_SEC_ENQ:
	case ERROR_ESBC_SEC_DEQ_TO:
	case ERROR_ESBC_SEC_JOBQ_STATUS:
	case ERROR_ESBC_CLIENT_HASH_COMPARE_KEY:
	case ERROR_ESBC_CLIENT_HASH_COMPARE_EM:
		fsl_secboot_image_verification_failure();
		break;
	case ERROR_ESBC_MISSING_BOOTM:
		fsl_secboot_bootscript_parse_failure();
		break;
	case ERROR_ESBC_WRONG_CMD:
	default:
		branch_to_self();
		break;
	}
}

static void fsl_secblk_handle_error(int error)
{
	switch (error) {
	case ERROR_ESBC_SEC_ENQ:
		fsl_secboot_handle_error(ERROR_ESBC_SEC_ENQ);
		break;
	case ERROR_ESBC_SEC_DEQ:
		fsl_secboot_handle_error(ERROR_ESBC_SEC_DEQ);
		break;
	case ERROR_ESBC_SEC_DEQ_TO:
		fsl_secboot_handle_error(ERROR_ESBC_SEC_DEQ_TO);
		break;
	default:
		printf("Job Queue Output status %x\n", error);
		fsl_secboot_handle_error(ERROR_ESBC_SEC_JOBQ_STATUS);
		break;
	}
}

/*
 * Calculate hash of key obtained via offset present in ESBC uboot
 * client hdr. This function calculates the hash of key which is obtained
 * through offset present in ESBC uboot client header.
 */
static int calc_img_key_hash(struct sha_ctx *ctx,
				struct fsl_secboot_img_priv *img)
{
	int i;
	int ret = 0;

	/* calc hash of the esbc key */
	sha_init(ctx, &jr);
#ifdef CONFIG_KEY_REVOCATION
	if (check_srk(img))
		sha_update(ctx, (u8 *)(img->ehdrloc + img->hdr.srk_tbl_off),
			   img->hdr.len_kr.num_srk * sizeof(struct srk_table));
	else
		sha_update(ctx, img->img_key, img->hdr.key_len);
#else
	sha_update(ctx, img->img_key, img->hdr.key_len);
#endif
	ret = sha_final(ctx);
	if (ret)
		return ret;

	ret = sha_digest(ctx, hash_val);
	if (ret)
		return ret;

	for (i = 0; i < SHA256_BYTES; i++)
		img->img_key_hash[i] = hash_val[i];

	return 0;
}

/*
 * Calculate hash of ESBC hdr and ESBC. This function calculates the
 * single hash of ESBC header and ESBC image. If SG flag is on, all
 * SG entries are also hashed alongwith the complete SG table.
 */
static int calc_esbchdr_esbc_hash(struct sha_ctx *ctx,
	struct fsl_secboot_img_priv *img)
{
	int i = 0;
	int ret = 0;

	/* calculate the hash of the CSF header */
	sha_init(ctx, &jr);
	sha_update(ctx, (u8 *) &img->hdr,
		sizeof(struct fsl_secboot_img_hdr));
#ifdef CONFIG_KEY_REVOCATION
	if (check_srk(img))
		sha_update(ctx, (u8 *)(img->ehdrloc + img->hdr.srk_tbl_off),
			   img->hdr.len_kr.num_srk * sizeof(struct srk_table));
	else
		sha_update(ctx, img->img_key, img->hdr.key_len);
#else
	sha_update(ctx, img->img_key, img->hdr.key_len);
#endif

	if (img->hdr.sg_flag) {
		/* calculate hash of the SG table */
		sha_update(ctx, (u8 *) &img->sgtbl,
			img->hdr.sg_entries *
			sizeof(struct fsl_secboot_sg_table));

		/* calculate the hash of each entry in the table */
		for (i = 0; i < img->hdr.sg_entries; i++)
			sha_update(ctx, img->sgtbl[i].pdata,
				img->sgtbl[i].len);
	} else {
		/* contiguous ESBC */
		sha_update(ctx, (u8 *) img->hdr.pimg,
			img->hdr.img_size);
	}

	ret = sha_final(ctx);
	if (ret)
		return ret;

	ret = sha_digest(ctx, hash_val);
	if (ret)
		return ret;

	return 0;
}

/*
 * Construct encoded hash EM' wrt PKCSv1.5. This function calculates the
 * pointers for padding, DER value and hash. And finally, constructs EM'
 * which includes hash of complete CSF header and ESBC image. If SG flag
 * is on, hash of SG table and entries is also included.
 */
static void construct_img_encoded_hash_second(struct fsl_secboot_img_priv *img)
{
	/*
	 * RSA PKCSv1.5 encoding format for encoded message is below
	 * EM = 0x0 || 0x1 || PS || 0x0 || DER || Hash
	 * PS is Padding String
	 * DER is DER value for SHA-256
	 * Hash is SHA-256 hash
	 * *********************************************************
	 * representative points to first byte of EM initially and is
	 * filled with 0x0
	 * representative is incremented by 1 and second byte is filled
	 * with 0x1
	 * padding points to third byte of EM
	 * digest points to full length of EM - 32 bytes
	 * hash_id (DER value) points to 19 bytes before pDigest
	 * separator is one byte which separates padding and DER
	 */

	size_t len;
	u8 *representative;
	u8 *padding, *digest;
	u8 *hash_id, *separator;
	int i;

	len = (get_key_len(img) / 2) - 1;
	representative = img->img_encoded_hash_second;
	representative[0] = 0;
	representative[1] = 1;  /* block type 1 */

	padding = &representative[2];
	digest = &representative[1] + len - 32;
	hash_id = digest - sizeof(hash_identifier);
	separator = hash_id - 1;

	/* fill padding area pointed by padding with 0xff */
	memset(padding, 0xff, separator - padding);

	/* fill byte pointed by separator */
	*separator = 0;

	/* fill SHA-256 DER value  pointed by HashId */
	memcpy(hash_id, hash_identifier, sizeof(hash_identifier));

	/* fill hash pointed by Digest */
	for (i = 0; i < SHA256_BYTES; i++)
		digest[i] = hash_val[i];
}

/*
 * Reads and validates the ESBC client header.
 * This function reads key and signature from the ESBC client header.
 * If Scatter/Gather flag is on, lengths and offsets of images
 * present as SG entries are also read. This function also checks
 * whether the header is valid or not.
 */
static int read_validate_esbc_client_header(struct fsl_secboot_img_priv *img)
{
	char buf[20];
	struct fsl_secboot_img_hdr *hdr = &img->hdr;
	void *esbc = (u8 *) img->ehdrloc;
	u8 *k, *s;
#ifdef CONFIG_KEY_REVOCATION
	u32 ret;
	u32 key_num, key_revoc_flag, size;
#endif

	/* check barker code */
	if (memcmp(hdr->barker, barker_code, ESBC_BARKER_LEN))
		return ERROR_ESBC_CLIENT_HEADER_BARKER;

	sprintf(buf, "%p", hdr->pimg);
	setenv("img_addr", buf);

	if (!hdr->img_size)
		return ERROR_ESBC_CLIENT_HEADER_IMG_SIZE;

	/* Key checking*/
#ifdef CONFIG_KEY_REVOCATION
	if (check_srk(img)) {
		if ((hdr->len_kr.num_srk == 0) ||
		    (hdr->len_kr.num_srk > MAX_KEY_ENTRIES))
			return ERROR_ESBC_CLIENT_HEADER_INVALID_SRK_NUM_ENTRY;

		key_num = hdr->len_kr.srk_sel;
		if (key_num == 0 || key_num > hdr->len_kr.num_srk)
			return ERROR_ESBC_CLIENT_HEADER_INVALID_KEY_NUM;

		/* Get revoc key from sfp */
		key_revoc_flag = get_key_revoc();
		ret = is_key_revoked(key_num, key_revoc_flag);
		if (ret)
			return ERROR_ESBC_CLIENT_HEADER_KEY_REVOKED;

		size = hdr->len_kr.num_srk * sizeof(struct srk_table);

		memcpy(&img->srk_tbl, esbc + hdr->srk_tbl_off, size);
		memcpy(&img->img_key, &(img->srk_tbl[key_num - 1].pkey),
		       img->srk_tbl[key_num - 1].key_len);

		ret = validate_srk_tbl(img->srk_tbl, hdr->len_kr.num_srk);

		if (ret != 0)
			return ret;

	} else
#endif
	{
		/* check key length */
		if (!((hdr->key_len == 2 * KEY_SIZE_BYTES / 4) ||
			(hdr->key_len == 2 * KEY_SIZE_BYTES / 2) ||
			(hdr->key_len == 2 * KEY_SIZE_BYTES)))
			return ERROR_ESBC_CLIENT_HEADER_KEY_LEN;

		memcpy(&img->img_key, esbc + hdr->pkey, hdr->key_len);
	}

	/* check signaure */
	if (get_key_len(img) == 2 * hdr->sign_len) {
		/* check signature length */
		if (!((hdr->sign_len == KEY_SIZE_BYTES / 4) ||
			(hdr->sign_len == KEY_SIZE_BYTES / 2) ||
			(hdr->sign_len == KEY_SIZE_BYTES)))
			return ERROR_ESBC_CLIENT_HEADER_SIG_LEN;
	} else {
		return ERROR_ESBC_CLIENT_HEADER_KEY_LEN_NOT_TWICE_SIG_LEN;
	}

	memcpy(&img->img_sign, esbc + hdr->psign, hdr->sign_len);

	/* No SG support */
	if (hdr->sg_flag)
		return ERROR_ESBC_CLIENT_HEADER_SG;

	/* modulus most significant bit should be set */
	k = (u8 *) &img->img_key;

	if ((k[0] & 0x80) == 0)
		return ERROR_ESBC_CLIENT_HEADER_KEY_MOD_1;

	/* modulus value should be odd */
	if ((k[get_key_len(img) / 2 - 1] & 0x1) == 0)
		return ERROR_ESBC_CLIENT_HEADER_KEY_MOD_2;

	/* Check signature value < modulus value */
	s = (u8 *) &img->img_sign;

	if (!(memcmp(s, k, hdr->sign_len) < 0))
		return ERROR_ESBC_CLIENT_HEADER_SIG_KEY_MOD;

	return ESBC_VALID_HDR;
}

static inline int str2long(const char *p, ulong *num)
{
	char *endptr;

	if (!p)
		return 0;
	else
		*num = simple_strtoul(p, &endptr, 16);

	return *p != '\0' && *endptr == '\0';
}

void done(uint32_t desc, uint32_t status, void *arg)
{
	struct result *x = arg;
	x->status = status;
	x->err = caam_jr_strstatus(x->outstr, status);
	x->done = 1;
}

int fsl_secboot_blob_decap(cmd_tbl_t *cmdtp, int flag, int argc,
		char * const argv[])
{
	ulong enc_addr = simple_strtoul(argv[1], NULL, 16);
	ulong blob_decap_addr = simple_strtoul(argv[2], NULL, 16);
	ulong out_sz = simple_strtoul(argv[3], NULL, 16);

	int ret;
	uint32_t desc[64];
	struct result op;
	unsigned long long timeval;
	unsigned long long timeout;
	char key_str[NUM_HEX_CHARS + 1];
	ulong key_id[16/sizeof(ulong)];

#ifdef CONFIG_FSL_CORENET
	pamu_enable();
#endif
	memset(&op, 0, sizeof(struct result));

	if (argc == 5) {
		char *cp = argv[4];
		int i = 0;

		if (*cp == '0' && *(cp + 1) == 'x')
			cp += 2;

		/* The input string expected is in hex, where
		 * each 4 bits would be represented by a hex
		 * sha256 hash is 256 bits long, which would mean
		 * num of characters = 256 / 4
		 */
		if (strlen(cp) != 32) {
			printf("%s is not a 16 byte hex string as expected\n",
				argv[4]);
			return -1;
		}

		for (i = 0; i < sizeof(key_id)/sizeof(ulong); i++) {
			strncpy(key_str, cp + (i * NUM_HEX_CHARS),
				NUM_HEX_CHARS);
			key_str[NUM_HEX_CHARS] = '\0';
			if (!str2long(key_str, &key_id[i])) {
				printf("%s is not a 128 bit hex string\n",
					argv[4]);
				return -1;
			}
		}
	}

#ifdef DEBUG
	printf("enc text at addr %lx" , enc_addr);
	printf("blob_decap text at addr %lx" , blob_decap_addr);
	printf("size %lx" , out_sz);
#endif
	printf("Decapsulating\n");

	inline_cnstr_jobdesc_blob_decap(desc, (uint8_t *)key_id,
		(uint8_t *)enc_addr, (uint8_t *)blob_decap_addr, out_sz);

	ret = jr_enqueue(&jr, desc, done, &op);
	if (ret) {
		printf("enq failed with ret %d\n", ret);
		fsl_secblk_handle_error(ret);
		goto out;
	}

#ifdef DEBUG
	printf("Descriptor\n");
	for (i = 0; i < 16; i++)
		printf("0x%x\n", desc[i]);
#endif

	timeval = get_ticks();
	timeout = usec2ticks(CONFIG_SEC_DEQ_TIMEOUT);

	while (op.done != 1) {
		if (jr_dequeue(&jr)) {
			printf("Deq error ERROR_ESBC_SEC_DEQ\n");
			fsl_secblk_handle_error(ERROR_ESBC_SEC_DEQ);
			goto out;
		}

		if ((get_ticks() - timeval) > timeout) {
			printf("SEC Dequeue timed out\n");
			fsl_secblk_handle_error(ERROR_ESBC_SEC_DEQ_TO);
			goto out;
		}
	}

	if (op.err < 0) {
		fsl_secblk_handle_error(op.status);
		goto out;
	}

	printf("Decapsulation successful\n");

out:
#ifdef CONFIG_FSL_CORENET
	pamu_disable();
#endif
	if (jr_reset(&jr) < 0) {
		fsl_secboot_handle_error(ERROR_ESBC_SEC_RESET);
		return 0;
	}
	return 0;
}

int fsl_secboot_blob_encap(cmd_tbl_t *cmdtp, int flag, int argc,
		char * const argv[])
{
	ulong plain_addr = simple_strtoul(argv[1], NULL, 16);
	ulong enc_addr = simple_strtoul(argv[2], NULL, 16);
	ulong in_sz = simple_strtoul(argv[3], NULL, 16);
	int ret;
	uint32_t desc[64];
	struct result op;
	unsigned long long timeval;
	unsigned long long timeout;
	int i = 0;
#ifdef CONFIG_FSL_CORENET
	pamu_enable();
#endif
	char key_str[NUM_HEX_CHARS + 1];
	ulong key_id[KEY_IDNFR_SZ_BYTES/sizeof(ulong)];

	memset(&op, 0, sizeof(struct result));

	if (argc == 5) {
		char *cp = argv[4];
		int i = 0;

		if (*cp == '0' && *(cp + 1) == 'x')
			cp += 2;

		/* The input string expected is in hex, where
		 * each 4 bits would be represented by a hex
		 * sha256 hash is 256 bits long, which would mean
		 * num of characters = 256 / 4
		 */
		if (strlen(cp) != 32) {
			printf("%s is not a 16 byte hex string as expected\n",
				argv[4]);
			return -1;
		}

		for (i = 0; i < sizeof(key_id)/sizeof(ulong); i++) {
			strncpy(key_str, cp + (i * NUM_HEX_CHARS),
				NUM_HEX_CHARS);
			key_str[NUM_HEX_CHARS] = '\0';
			if (!str2long(key_str, &key_id[i])) {
				printf("%s is not a 128 bit hex string\n"
					, argv[4]);
				return -1;
			}
		}
	}

	printf("\nEncapsulating data\n");

#ifdef DEBUG
	printf("plain text at addr %lx\n" , plain_addr);
	printf("enc text at addr %lx\n" , enc_addr);
	printf("size %lx\n" , in_sz);
	for (i = 0; i < 4; i++)
		printf("%x\t", key_id[i]);
#endif

	inline_cnstr_jobdesc_blob_encap(desc, (uint8_t *)key_id,
		(uint8_t *)plain_addr, (uint8_t *)enc_addr, in_sz);
	ret = jr_enqueue(&jr, desc, done, &op);
	if (ret) {
		printf("enq failed with ret %d\n", ret);
		fsl_secblk_handle_error(ret);
		goto out;
	}

#ifdef DEBUG
#endif

	timeval = get_ticks();
	timeout = usec2ticks(CONFIG_SEC_DEQ_TIMEOUT);

	while (op.done != 1) {
		if (jr_dequeue(&jr)) {
			printf("deq error ERROR_ESBC_SEC_DEQ\n");
			fsl_secblk_handle_error(ERROR_ESBC_SEC_DEQ);
			goto out;
		}

		if ((get_ticks() - timeval) > timeout) {
			printf("SEC Dequeue timed out\n");
			fsl_secblk_handle_error(ERROR_ESBC_SEC_DEQ_TO);
			goto out;
		}
	}

	if (op.err < 0) {
		fsl_secblk_handle_error(op.status);
		printf("Descriptor\n");
		for (i = 0; i < 16; i++)
			printf("0x%x\n", desc[i]);
		goto out;
	}

	printf("Encapsulation succesful\n");

out:
#ifdef CONFIG_FSL_CORENET
	pamu_disable();
#endif
	if (jr_reset(&jr) < 0) {
		fsl_secboot_handle_error(ERROR_ESBC_SEC_RESET);
		return 0;
	}
	return 0;

}

int fsl_secboot_validate(cmd_tbl_t *cmdtp, int flag, int argc,
		char * const argv[])
{
	int hash_srk = 1;
	ccsr_sfp_regs_t *sfp_regs = (void *)(CONFIG_SYS_SFP_ADDR);
	ulong hash[SHA256_BYTES/sizeof(ulong)];
	char hash_str[NUM_HEX_CHARS + 1];
	struct sha_ctx ctx;
	struct rsa_context rsa_ctx;
	ulong addr = simple_strtoul(argv[1], NULL, 16);
	struct fsl_secboot_img_priv *img;
	struct fsl_secboot_img_hdr *hdr;
	void *esbc;
	int ret, i;
	u32 srk_hash[8];

	if (argc == 3) {
		char *cp = argv[2];
		int i = 0;

		if (*cp == '0' && *(cp + 1) == 'x')
			cp += 2;

		/* The input string expected is in hex, where
		 * each 4 bits would be represented by a hex
		 * sha256 hash is 256 bits long, which would mean
		 * num of characters = 256 / 4
		 */
		if (strlen(cp) != SHA256_NIBBLES) {
			printf("%s is not a 256 bits hex string as expected\n",
				argv[2]);
			return -1;
		}

		for (i = 0; i < sizeof(hash)/sizeof(ulong); i++) {
			strncpy(hash_str, cp + (i * NUM_HEX_CHARS),
				NUM_HEX_CHARS);
			hash_str[NUM_HEX_CHARS] = '\0';
			if (!str2long(hash_str, &hash[i])) {
				printf("%s is not a 256 bits hex string "
					"as expected\n", argv[2]);
				return -1;
			}
		}

		hash_srk = 0;
	}

#ifdef CONFIG_FSL_CORENET
	pamu_enable();
#endif
	img = malloc(sizeof(struct fsl_secboot_img_priv));

	if (!img)
		return -1;

	memset(img, 0, sizeof(struct fsl_secboot_img_priv));

	hdr = &img->hdr;
	img->ehdrloc = addr;
	esbc = (u8 *) img->ehdrloc;

	memcpy(hdr, esbc, sizeof(struct fsl_secboot_img_hdr));

	/* read and validate esbc header */
	ret = read_validate_esbc_client_header(img);

	if (ret != ESBC_VALID_HDR) {
		fsl_secboot_handle_error(ret);
		goto exit1;
	}

	/* SRKH present in SFP */
	for (i = 0; i < NUM_SRKH_REGS; i++)
		srk_hash[i] = in_be32(&sfp_regs->srk_hash[i]);

	/*
	 * Calculate hash of key obtained via offset present in
	 * ESBC uboot client hdr
	 */
	ret = calc_img_key_hash(&ctx, img);
	if (ret) {
		fsl_secblk_handle_error(ret);
		goto exit;
	}

	/* Compare hash obtained above with SRK hash present in SFP */
	if (hash_srk)
		ret = memcmp(srk_hash, img->img_key_hash, SHA256_BYTES);
	else
		ret = memcmp(&hash, &img->img_key_hash, SHA256_BYTES);

	if (ret != 0) {
		fsl_secboot_handle_error(ERROR_ESBC_CLIENT_HASH_COMPARE_KEY);
		goto exit;
	}

	ret = calc_esbchdr_esbc_hash(&ctx, img);
	if (ret) {
		fsl_secblk_handle_error(ret);
		goto exit;
	}

	/* Construct encoded hash EM' wrt PKCSv1.5 */
	construct_img_encoded_hash_second(img);

	ret = rsa_public_verif_sec(img->img_sign, img->img_encoded_hash,
		img->img_key, get_key_len(img) / 2, &rsa_ctx, &jr);
	if (ret) {
		fsl_secblk_handle_error(ret);
		goto exit;
	}

	/*
	 * compare the encoded messages EM' and EM wrt RSA PKCSv1.5
	 * memcmp returns zero on success
	 * memcmp returns non-zero on failure
	 */
	ret = memcmp(&img->img_encoded_hash_second, &img->img_encoded_hash,
		img->hdr.sign_len);

	if (ret) {
		fsl_secboot_handle_error(ERROR_ESBC_CLIENT_HASH_COMPARE_EM);
		goto exit;
	}

	printf("esbc_validate command successful\n");

exit:
	if (jr_reset(&jr) < 0) {
		fsl_secboot_handle_error(ERROR_ESBC_SEC_RESET);
		return 0;
	}
exit1:
#ifdef CONFIG_FSL_CORENET
	pamu_disable();
#endif
	return 0;
}
