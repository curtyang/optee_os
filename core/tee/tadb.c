// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2017, Linaro Limited
 */

#include <bitstring.h>
#include <crypto/crypto.h>
#include <kernel/msg_param.h>
#include <kernel/mutex.h>
#include <kernel/refcount.h>
#include <kernel/thread.h>
#include <optee_msg_supplicant.h>
#include <string.h>
#include <tee_api_defines_extensions.h>
#include <tee/tadb.h>
#include <tee/tee_fs.h>
#include <tee/tee_fs_rpc.h>
#include <tee/tee_pobj.h>
#include <tee/tee_svc_storage.h>
#include <utee_defines.h>

#define TADB_MAX_BUFFER_SIZE	(64U * 1024)

#define TADB_AUTH_ENC_ALG	TEE_ALG_AES_GCM
#define TADB_IV_SIZE		TEE_AES_BLOCK_SIZE
#define TADB_TAG_SIZE		TEE_AES_BLOCK_SIZE
#define TADB_KEY_SIZE		TEE_AES_MAX_KEY_SIZE

struct tee_tadb_dir {
	const struct tee_file_operations *ops;
	struct tee_file_handle *fh;
	int nbits;
	bitstr_t *files;
};

struct tadb_entry {
	struct tee_tadb_property prop;
	uint32_t file_number;
	uint8_t iv[TADB_IV_SIZE];
	uint8_t tag[TADB_TAG_SIZE];
	uint8_t key[TADB_KEY_SIZE];
};

struct tadb_header {
	uint32_t opaque_len;
	uint8_t opaque[];
};

struct tee_tadb_ta_write {
	struct tee_tadb_dir *db;
	int fd;
	struct tadb_entry entry;
	size_t pos;
	void *ctx;
};

struct tee_tadb_ta_read {
	struct tee_tadb_dir *db;
	int fd;
	struct tadb_entry entry;
	size_t pos;
	void *ctx;
	uint64_t ta_cookie;
	struct mobj *ta_mobj;
	uint8_t *ta_buf;
};

static const char tadb_obj_id[] = "ta.db";
static struct tee_tadb_dir *tadb_db;
static struct refcount tadb_db_refc;
static struct mutex tadb_mutex = MUTEX_INITIALIZER;

static void file_num_to_str(char *buf, size_t blen, uint32_t file_number)
{
	snprintf(buf, blen, "%" PRIu32 ".ta", file_number);
}

static bool is_null_uuid(const TEE_UUID *uuid)
{
	const TEE_UUID null_uuid = { 0 };

	return !memcmp(uuid, &null_uuid, sizeof(*uuid));
}

static TEE_Result ta_operation_open(unsigned int cmd, uint32_t file_number,
				    int *fd)
{
	struct mobj *mobj;
	TEE_Result res;
	void *va;
	uint64_t cookie;
	struct optee_msg_param params[] = {
		[0] = { .attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT,
			.u.value.a = cmd },
		[2] = { .attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT }
	};

	va = tee_fs_rpc_cache_alloc(TEE_FS_NAME_MAX, &mobj, &cookie);
	if (!va)
		return TEE_ERROR_OUT_OF_MEMORY;

	if (!msg_param_init_memparam(params + 1, mobj, 0, TEE_FS_NAME_MAX,
				     cookie, MSG_PARAM_MEM_DIR_IN))
		return TEE_ERROR_BAD_STATE;

	file_num_to_str(va, TEE_FS_NAME_MAX, file_number);

	res = thread_rpc_cmd(OPTEE_MSG_RPC_CMD_FS, ARRAY_SIZE(params), params);
	if (!res)
		*fd = params[2].u.value.a;

	return res;
}

static TEE_Result ta_operation_remove(uint32_t file_number)
{
	struct mobj *mobj;
	void *va;
	uint64_t cookie;
	struct optee_msg_param params[2] = {
		[0] = { .attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT,
			.u.value.a = OPTEE_MRF_REMOVE },
	};

	va = tee_fs_rpc_cache_alloc(TEE_FS_NAME_MAX, &mobj, &cookie);
	if (!va)
		return TEE_ERROR_OUT_OF_MEMORY;

	if (!msg_param_init_memparam(params + 1, mobj, 0, TEE_FS_NAME_MAX,
				     cookie, MSG_PARAM_MEM_DIR_IN))
		return TEE_ERROR_BAD_STATE;

	file_num_to_str(va, TEE_FS_NAME_MAX, file_number);

	return thread_rpc_cmd(OPTEE_MSG_RPC_CMD_FS, ARRAY_SIZE(params), params);
}

static TEE_Result maybe_grow_files(struct tee_tadb_dir *db, int idx)
{
	void *p;

	if (idx < db->nbits)
		return TEE_SUCCESS;

	p = realloc(db->files, bitstr_size(idx + 1));
	if (!p)
		return TEE_ERROR_OUT_OF_MEMORY;
	db->files = p;

	bit_nclear(db->files, db->nbits, idx);
	db->nbits = idx + 1;

	return TEE_SUCCESS;
}

static TEE_Result set_file(struct tee_tadb_dir *db, int idx)
{
	TEE_Result res = maybe_grow_files(db, idx);

	if (!res)
		bit_set(db->files, idx);

	return res;
}

static void clear_file(struct tee_tadb_dir *db, int idx)
{
	if (idx < db->nbits)
		bit_clear(db->files, idx);
}

static bool test_file(struct tee_tadb_dir *db, int idx)
{
	if (idx < db->nbits)
		return bit_test(db->files, idx);

	return false;
}

static TEE_Result read_ent(struct tee_tadb_dir *db, size_t idx,
			   struct tadb_entry *entry)
{
	size_t l = sizeof(*entry);
	TEE_Result res = db->ops->read(db->fh, idx * l, entry, &l);

	if (!res && l != sizeof(*entry))
		return TEE_ERROR_ITEM_NOT_FOUND;

	return res;
}

static TEE_Result write_ent(struct tee_tadb_dir *db, size_t idx,
			    const struct tadb_entry *entry)
{
	const size_t l = sizeof(*entry);

	return db->ops->write(db->fh, idx * l, entry, l);
}

static TEE_Result tadb_open(struct tee_tadb_dir **db_ret)
{
	TEE_Result res;
	struct tee_tadb_dir *db = calloc(1, sizeof(*db));
	struct tee_pobj po = {
		.obj_id = (void *)tadb_obj_id,
		.obj_id_len = sizeof(tadb_obj_id)
	};

	if (!db)
		return TEE_ERROR_OUT_OF_MEMORY;

	db->ops = tee_svc_storage_file_ops(TEE_STORAGE_PRIVATE);

	res = db->ops->open(&po, NULL, &db->fh);
	if (res == TEE_ERROR_ITEM_NOT_FOUND)
		res = db->ops->create(&po, false, NULL, 0, NULL, 0, NULL, 0,
				      &db->fh);

	if (res)
		free(db);
	else
		*db_ret = db;

	return res;
}

static TEE_Result tee_tadb_open(struct tee_tadb_dir **db)
{
	if (!refcount_inc(&tadb_db_refc)) {
		TEE_Result res;

		mutex_lock(&tadb_mutex);
		res = tadb_open(&tadb_db);
		if (!res)
			refcount_set(&tadb_db_refc, 1);
		mutex_unlock(&tadb_mutex);
		if (res)
			return res;
	}

	*db = tadb_db;
	return TEE_SUCCESS;
}

static void tadb_put(struct tee_tadb_dir *db)
{
	if (refcount_dec(&tadb_db_refc)) {
		mutex_lock(&tadb_mutex);
		if (!refcount_val(&tadb_db_refc) && tadb_db) {
			db->ops->close(&db->fh);
			free(db->files);
			free(db);
			tadb_db = NULL;
		}
		mutex_unlock(&tadb_mutex);
	}
}

static void tee_tadb_close(struct tee_tadb_dir *db)
{
	tadb_put(db);
}

static TEE_Result tadb_authenc_init(TEE_OperationMode mode,
				    const struct tadb_entry *entry,
				    void **ctx_ret)
{
	TEE_Result res;
	void *ctx;
	const size_t enc_size = entry->prop.custom_size + entry->prop.bin_size;

	res = crypto_authenc_alloc_ctx(&ctx, TADB_AUTH_ENC_ALG);
	if (res)
		return res;

	res = crypto_authenc_init(ctx, TADB_AUTH_ENC_ALG, mode,
				  entry->key, sizeof(entry->key),
				  entry->iv, sizeof(entry->iv),
				  sizeof(entry->tag), 0, enc_size);
	if (res)
		crypto_authenc_free_ctx(ctx, TADB_AUTH_ENC_ALG);
	else
		*ctx_ret = ctx;

	return res;
}

static TEE_Result tadb_update_payload(void *ctx, TEE_OperationMode mode,
				      const void *src, size_t len, void *dst)
{
	TEE_Result res;
	size_t sz = len;

	res = crypto_authenc_update_payload(ctx, TADB_AUTH_ENC_ALG, mode,
					    (const uint8_t *)src, len, dst,
					    &sz);
	assert(res || sz == len);
	return res;
}

static TEE_Result populate_files(struct tee_tadb_dir *db)
{
	TEE_Result res;
	size_t idx;

	/*
	 * If db->files isn't NULL the bitfield is already populated and
	 * there's nothing left to do here for now.
	 */
	if (db->files)
		return TEE_SUCCESS;

	/*
	 * Iterate over the TA database and set the bits in the bit field
	 * for used file numbers. Note that set_file() will allocate and
	 * grow the bitfield as needed.
	 *
	 * At the same time clean out duplicate file numbers, the first
	 * entry with the file number has precedence. Duplicate entries is
	 * not supposed to be able to happen, but if it still does better
	 * to clean it out here instead of letting the error spread with
	 * unexpected side effects.
	 */
	for (idx = 0;; idx++) {
		struct tadb_entry entry;

		res = read_ent(db, idx, &entry);
		if (res) {
			if (res == TEE_ERROR_ITEM_NOT_FOUND)
				return TEE_SUCCESS;
			goto err;
		}

		if (is_null_uuid(&entry.prop.uuid))
			continue;

		if (test_file(db, entry.file_number)) {
			IMSG("Clearing duplicate file number %" PRIu32,
			     entry.file_number);
			memset(&entry, 0, sizeof(entry));
			res = write_ent(db, idx, &entry);
			if (res)
				goto err;
			continue;
		}

		res = set_file(db, entry.file_number);
		if (res)
			goto err;
	}

err:
	free(db->files);
	db->files = NULL;
	db->nbits = 0;

	return res;
}

TEE_Result tee_tadb_ta_create(const struct tee_tadb_property *property,
			      struct tee_tadb_ta_write **ta_ret)
{
	TEE_Result res;
	struct tee_tadb_ta_write *ta;
	int i = 0;

	if (is_null_uuid(&property->uuid))
		return TEE_ERROR_GENERIC;

	ta = calloc(1, sizeof(*ta));
	if (!ta)
		return TEE_ERROR_OUT_OF_MEMORY;

	res = tee_tadb_open(&ta->db);
	if (res)
		goto err;

	mutex_lock(&tadb_mutex);

	/*
	 * Since we're going to search for next free file number below we
	 * need to populate the bitfield holding used file numbers.
	 */
	res = populate_files(ta->db);
	if (res)
		goto err_mutex;

	if (ta->db->files) {
		bit_ffc(ta->db->files, ta->db->nbits, &i);
		if (i == -1)
			i = ta->db->nbits;
	}

	res = set_file(ta->db, i);
	if (res)
		goto err_mutex;

	mutex_unlock(&tadb_mutex);

	ta->entry.file_number = i;
	ta->entry.prop = *property;

	res = crypto_rng_read(ta->entry.iv, sizeof(ta->entry.iv));
	if (res)
		goto err;

	res = crypto_rng_read(ta->entry.key, sizeof(ta->entry.key));
	if (res)
		goto err;

	res = ta_operation_open(OPTEE_MRF_CREATE, ta->entry.file_number,
				&ta->fd);
	if (res)
		goto err;

	res = tadb_authenc_init(TEE_MODE_ENCRYPT, &ta->entry, &ta->ctx);
	if (res)
		goto err;

	*ta_ret = ta;

	return TEE_SUCCESS;

err_mutex:
	mutex_unlock(&tadb_mutex);
err:
	tadb_put(ta->db);
	free(ta);

	return res;
}

TEE_Result tee_tadb_ta_write(struct tee_tadb_ta_write *ta, const void *buf,
			     size_t len)
{
	TEE_Result res;
	const uint8_t *rb = buf;
	size_t rl = len;
	struct tee_fs_rpc_operation op;

	while (rl) {
		size_t wl = MIN(rl, TADB_MAX_BUFFER_SIZE);
		void *wb;

		res = tee_fs_rpc_write_init(&op, OPTEE_MSG_RPC_CMD_FS, ta->fd,
					    ta->pos, wl, &wb);
		if (res)
			return res;

		res = tadb_update_payload(ta->ctx, TEE_MODE_ENCRYPT,
					  rb, wl, wb);
		if (res)
			return res;

		res = tee_fs_rpc_write_final(&op);
		if (res)
			return res;

		rl -= wl;
		rb += wl;
		ta->pos += wl;
	}

	return TEE_SUCCESS;
}

void tee_tadb_ta_close_and_delete(struct tee_tadb_ta_write *ta)
{
	crypto_authenc_final(ta->ctx, TADB_AUTH_ENC_ALG);
	crypto_authenc_free_ctx(ta->ctx, TADB_AUTH_ENC_ALG);
	tee_fs_rpc_close(OPTEE_MSG_RPC_CMD_FS, ta->fd);
	ta_operation_remove(ta->entry.file_number);

	mutex_lock(&tadb_mutex);
	clear_file(ta->db, ta->entry.file_number);
	mutex_unlock(&tadb_mutex);

	tadb_put(ta->db);
	free(ta);
}

static TEE_Result find_ent(struct tee_tadb_dir *db, const TEE_UUID *uuid,
			   size_t *idx_ret, struct tadb_entry *entry_ret)
{
	TEE_Result res;
	size_t idx;

	/*
	 * Search for the provided uuid, if it's found return the index it
	 * has together with TEE_SUCCESS.
	 *
	 * If the uuid can't be found return the number indexes together
	 * with TEE_ERROR_ITEM_NOT_FOUND.
	 */
	for (idx = 0;; idx++) {
		struct tadb_entry entry;

		res = read_ent(db, idx, &entry);
		if (res) {
			if (res == TEE_ERROR_ITEM_NOT_FOUND)
				break;
			return res;
		}

		if (!memcmp(&entry.prop.uuid, uuid, sizeof(*uuid))) {
			if (entry_ret)
				*entry_ret = entry;
			break;
		}
	}

	*idx_ret = idx;
	return res;
}

static TEE_Result find_free_ent_idx(struct tee_tadb_dir *db, size_t *idx)
{
	const TEE_UUID null_uuid = { 0 };
	TEE_Result res = find_ent(db, &null_uuid, idx, NULL);

	/*
	 * Note that *idx is set to the number of entries on
	 * TEE_ERROR_ITEM_NOT_FOUND.
	 */
	if (res == TEE_ERROR_ITEM_NOT_FOUND)
		return TEE_SUCCESS;
	return res;
}

TEE_Result tee_tadb_ta_close_and_commit(struct tee_tadb_ta_write *ta)
{
	TEE_Result res;
	size_t dsz = 0;
	size_t sz = sizeof(ta->entry.tag);
	size_t idx;
	struct tadb_entry old_ent;
	bool have_old_ent = false;

	res = crypto_authenc_enc_final(ta->ctx, TADB_AUTH_ENC_ALG,
				       NULL, 0, NULL, &dsz,
				       ta->entry.tag, &sz);
	if (res)
		goto err;

	tee_fs_rpc_close(OPTEE_MSG_RPC_CMD_FS, ta->fd);

	mutex_lock(&tadb_mutex);
	/*
	 * First try to find an existing TA to replace. If there's one
	 * we'll use the entry, but we should also remove the old encrypted
	 * file.
	 *
	 * If there isn't an existing TA to replace, grab a new entry.
	 */
	res = find_ent(ta->db, &ta->entry.prop.uuid, &idx, &old_ent);
	if (!res) {
		have_old_ent = true;
	} else {
		res = find_free_ent_idx(ta->db, &idx);
		if (res)
			goto err_mutex;
	}
	res = write_ent(ta->db, idx, &ta->entry);
	if (res)
		goto err_mutex;
	if (have_old_ent)
		clear_file(ta->db, old_ent.file_number);
	mutex_unlock(&tadb_mutex);

	crypto_authenc_final(ta->ctx, TADB_AUTH_ENC_ALG);
	crypto_authenc_free_ctx(ta->ctx, TADB_AUTH_ENC_ALG);
	tadb_put(ta->db);
	free(ta);
	if (have_old_ent)
		ta_operation_remove(old_ent.file_number);
	return TEE_SUCCESS;

err_mutex:
	mutex_unlock(&tadb_mutex);
err:
	tee_tadb_ta_close_and_delete(ta);
	return res;
}

TEE_Result tee_tadb_ta_delete(const TEE_UUID *uuid)
{
	const struct tadb_entry null_entry = { { { 0 } } };
	struct tee_tadb_dir *db;
	struct tadb_entry entry;
	size_t idx;
	TEE_Result res;

	if (is_null_uuid(uuid))
		return TEE_ERROR_GENERIC;

	res = tee_tadb_open(&db);
	if (res)
		return res;

	mutex_lock(&tadb_mutex);
	res = find_ent(db, uuid, &idx, &entry);
	if (res) {
		mutex_unlock(&tadb_mutex);
		tee_tadb_close(db);
		return res;
	}

	clear_file(db, entry.file_number);
	res = write_ent(db, idx, &null_entry);
	mutex_unlock(&tadb_mutex);

	tee_tadb_close(db);
	if (res)
		return res;

	ta_operation_remove(entry.file_number);
	return TEE_SUCCESS;
}

TEE_Result tee_tadb_ta_open(const TEE_UUID *uuid,
			    struct tee_tadb_ta_read **ta_ret)
{
	TEE_Result res;
	size_t idx;
	struct tee_tadb_ta_read *ta;
	static struct tadb_entry last_entry;

	if (is_null_uuid(uuid))
		return TEE_ERROR_GENERIC;

	ta = calloc(1, sizeof(*ta));
	if (!ta)
		return TEE_ERROR_OUT_OF_MEMORY;

	if (!memcmp(uuid, &last_entry.prop.uuid, sizeof(*uuid))) {
		ta->entry = last_entry;
	} else {
		res = tee_tadb_open(&ta->db);
		if (res)
			goto err_free; /* Mustn't all tadb_put() */

		mutex_read_lock(&tadb_mutex);
		res = find_ent(ta->db, uuid, &idx, &ta->entry);
		mutex_read_unlock(&tadb_mutex);
		if (res)
			goto err;
	}

	res = ta_operation_open(OPTEE_MRF_OPEN, ta->entry.file_number, &ta->fd);
	if (res)
		goto err;

	res = tadb_authenc_init(TEE_MODE_DECRYPT, &ta->entry, &ta->ctx);
	if (res)
		goto err;

	*ta_ret = ta;

	return TEE_SUCCESS;
err:
	tadb_put(ta->db);
err_free:
	free(ta);
	return res;
}

const struct tee_tadb_property *
tee_tadb_ta_get_property(struct tee_tadb_ta_read *ta)
{
	return &ta->entry.prop;
}

static TEE_Result ta_load(struct tee_tadb_ta_read *ta)
{
	TEE_Result res;
	const size_t sz = ta->entry.prop.custom_size + ta->entry.prop.bin_size;
	struct optee_msg_param params[2] = {
		[0] = { .attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT,
			.u.value.a = OPTEE_MRF_READ,
			.u.value.b = ta->fd,
			.u.value.c = 0 },
	};

	if (ta->ta_mobj)
		return TEE_SUCCESS;

	ta->ta_mobj = thread_rpc_alloc_payload(sz, &ta->ta_cookie);
	if (!ta->ta_mobj)
		return TEE_ERROR_OUT_OF_MEMORY;

	ta->ta_buf = mobj_get_va(ta->ta_mobj, 0);
	assert(ta->ta_buf);

	if (!msg_param_init_memparam(params + 1, ta->ta_mobj, 0, sz,
				     ta->ta_cookie, MSG_PARAM_MEM_DIR_OUT))
		return TEE_ERROR_BAD_STATE;

	res = thread_rpc_cmd(OPTEE_MSG_RPC_CMD_FS, ARRAY_SIZE(params), params);
	if (res) {
		thread_rpc_free_payload(ta->ta_cookie, ta->ta_mobj);
		ta->ta_mobj = NULL;
	}
	return res;
}

TEE_Result tee_tadb_ta_read(struct tee_tadb_ta_read *ta, void *buf, size_t *len)
{
	TEE_Result res;
	const size_t sz = ta->entry.prop.custom_size + ta->entry.prop.bin_size;
	size_t l = MIN(*len, sz - ta->pos);

	res = ta_load(ta);
	if (res)
		return res;

	if (buf) {
		res = tadb_update_payload(ta->ctx, TEE_MODE_DECRYPT,
					  ta->ta_buf + ta->pos, l, buf);
		if (res)
			return res;
	} else {
		size_t num_bytes = 0;
		size_t b_size = MIN(SIZE_4K, l);
		uint8_t *b = malloc(b_size);

		if (!b)
			return TEE_ERROR_OUT_OF_MEMORY;

		while (num_bytes < l) {
			size_t n = MIN(b_size, l - num_bytes);

			res = tadb_update_payload(ta->ctx, TEE_MODE_DECRYPT,
						  ta->ta_buf + ta->pos, n, b);
			if (res)
				break;
			num_bytes += n;
		}

		free(b);
		if (res)
			return res;
	}

	ta->pos += l;
	*len = l;
	return TEE_SUCCESS;
}

void tee_tadb_ta_close(struct tee_tadb_ta_read *ta)
{
	crypto_authenc_final(ta->ctx, TADB_AUTH_ENC_ALG);
	crypto_authenc_free_ctx(ta->ctx, TADB_AUTH_ENC_ALG);
	if (ta->ta_mobj)
		thread_rpc_free_payload(ta->ta_cookie, ta->ta_mobj);
	tee_fs_rpc_close(OPTEE_MSG_RPC_CMD_FS, ta->fd);
	tadb_put(ta->db);
	free(ta);
}
