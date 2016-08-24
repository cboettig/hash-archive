// Copyright 2016 Ben Trask
// MIT licensed (see LICENSE for details)

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <async/async.h>
#include <async/http/HTTP.h>
#include "util/hash.h"
#include "util/strext.h"
#include "util/url.h"
#include "db.h"
#include "page.h"

int url_fetch(strarg_t const URL, strarg_t const client, int *const outstatus, HTTPHeadersRef *const outheaders, uint64_t *const outlength, hasher_t **const outhasher);


static uint64_t current_id = 0;
static async_mutex_t id_lock[1];

static uint64_t latest_time = 0;
static uint64_t latest_id = 0;
static async_mutex_t latest_lock[1];
static async_cond_t latest_cond[1];


// TODO: Define static async_x_t initializers
void queue_init(void) {
	async_mutex_init(id_lock, 0);
	async_mutex_init(latest_lock, 0);
	async_cond_init(latest_cond, 0);
}


static int queue_peek(uint64_t *const outtime, uint64_t *const outid, char *const outURL, size_t const urlmax, char *const outclient, size_t const clientmax) {
	assert(outtime);
	assert(outid);
	assert(outURL);
	assert(urlmax > 0);
	assert(outclient);
	assert(clientmax > 0);

	DB_env *db = NULL;
	DB_txn *txn = NULL;
	DB_cursor *cursor = NULL;
	DB_range range[1];
	DB_val key[1];
	strarg_t URL, client;
	int rc = 0;

	rc = hx_db_open(&db);
	if(rc < 0) goto cleanup;
	rc = db_txn_begin(db, NULL, DB_RDONLY, &txn);
	if(rc < 0) goto cleanup;
	rc = db_txn_cursor(txn, &cursor);
	if(rc < 0) goto cleanup;
	HXTimeIDQueuedURLAndClientRange0(range);
	DB_VAL_STORAGE(key, DB_VARINT_MAX*3)
	db_bind_uint64(key, HXTimeIDQueuedURLAndClient);
	db_bind_uint64(key, latest_time);
	db_bind_uint64(key, latest_id+1);
	DB_VAL_STORAGE_VERIFY(key);
	rc = db_cursor_seekr(cursor, range, key, NULL, +1);
	if(rc < 0) goto cleanup;

	HXTimeIDQueuedURLAndClientKeyUnpack(key, txn, outtime, outid, &URL, &client);
	strlcpy(outURL, URL ? URL : "", urlmax);
	strlcpy(outclient, client ? client : "", clientmax);
	latest_time = *outtime;
	latest_id = *outid;
cleanup:
	cursor = NULL;
	db_txn_abort(txn); txn = NULL;
	hx_db_close(&db);
	return rc;
}
static int queue_remove(DB_txn *const txn, uint64_t const time, uint64_t const id, strarg_t const URL, strarg_t const client) {
	assert(time);
	assert(id);
	assert(URL);
	assert(client);
	DB_val key[1];
	HXTimeIDQueuedURLAndClientKeyPack(key, txn, time, id, URL, client);
	return db_del(txn, key, 0); // DB_NOOVERWRITE_FAST
}


int response_add(DB_txn *const txn, uint64_t const time, uint64_t const id, strarg_t const URL, int const status, strarg_t const type, uint64_t const length, hasher_t *const hasher) {
	char URL_surt[URI_MAX];
	int rc = url_normalize_surt(URL, URL_surt, sizeof(URL_surt));
	if(rc < 0) return rc;

	// TODO: Signed varints would be more efficient.
	int64_t sstatus = 0xffff + status;
	db_assert(sstatus >= 0);

	DB_val res_key[1], res_val[1];
	HXTimeIDToResponseKeyPack(res_key, time, id);
	DB_VAL_STORAGE(res_val,
		DB_INLINE_MAX +
		DB_VARINT_MAX +
		DB_INLINE_MAX +
		DB_VARINT_MAX *
		DB_BLOB_MAX(HASH_DIGEST_MAX)*HASH_ALGO_MAX)
	db_bind_string(res_val, URL, txn);
	db_bind_uint64(res_val, (uint64_t)sstatus);
	db_bind_string(res_val, type, txn);
	db_bind_uint64(res_val, length);
	for(size_t i = 0; i < HASH_ALGO_MAX; i++) {
		size_t const len = hash_algo_digest_len(i);
		db_bind_uint64(res_val, len);
		db_bind_blob(res_val, hasher_get(hasher, i), len);
	}
	DB_VAL_STORAGE_VERIFY(res_val);
	rc = db_put(txn, res_key, res_val, DB_NOOVERWRITE_FAST);
	if(rc < 0) return rc;

	DB_val url_key[1];
	HXURLSurtAndTimeIDKeyPack(url_key, txn, URL_surt, time, id);
	rc = db_put(txn, url_key, NULL, DB_NOOVERWRITE_FAST);
	if(rc < 0) return rc;

	DB_val hash_key[1];
	for(size_t i = 0; i < HASH_ALGO_MAX; i++) {
		uint8_t const *const hash = hasher_get(hasher, i);
		if(!hash) continue;
		HXAlgoHashAndTimeIDKeyPack(hash_key, i, hash, time, id);
		rc = db_put(txn, hash_key, NULL, DB_NOOVERWRITE_FAST);
		if(rc < 0) return rc;
	}

alogf("Added response %s %d %s %llu (%s)\n", URL, status, type, (unsigned long long)length, URL_surt);

	return 0;
}

int queue_add(uint64_t const time, strarg_t const URL, strarg_t const client) {
	assert(time);
	assert(URL);
	assert(client);
	DB_env *db = NULL;
	DB_txn *txn = NULL;
	DB_val key[1];

	async_mutex_lock(id_lock);
	uint64_t const id = ++current_id;
	async_mutex_unlock(id_lock);

	int rc = hx_db_open(&db);
	if(rc < 0) goto cleanup;
	rc = db_txn_begin(db, NULL, DB_RDWR, &txn);
	if(rc < 0) goto cleanup;

	// TODO: Verify that this URL isn't already queued,
	// and hasn't been fetched in the past 24 hours.

	HXTimeIDQueuedURLAndClientKeyPack(key, txn, time, id, URL, client);
	rc = db_put(txn, key, NULL, 0); // DB_NOOVERWRITE_FAST
	if(rc < 0) goto cleanup;
	rc = db_txn_commit(txn); txn = NULL;
	if(rc < 0) goto cleanup;
	hx_db_close(&db);
	alogf("enqueued %s (%s)\n", URL, hx_strerror(rc));
	async_cond_broadcast(latest_cond);
cleanup:
	db_txn_abort(txn); txn = NULL;
	hx_db_close(&db);
	return rc;
}

static void queue_work(void) {
	uint64_t then;
	uint64_t old_id;
	char URL[URI_MAX];
	char client[255+1];

	uint64_t now;
	uint64_t new_id;
	int status;
	HTTPHeadersRef headers = NULL;
	strarg_t type;
	uint64_t length;
	hasher_t *hasher = NULL;

	DB_env *db = NULL;
	DB_txn *txn = NULL;
	int rc = 0;

	async_mutex_lock(latest_lock);
	for(;;) {
		rc = queue_peek(&then, &old_id, URL, sizeof(URL), client, sizeof(client));
		if(DB_NOTFOUND != rc) break;
		rc = async_cond_wait(latest_cond, latest_lock);
		if(rc < 0) break;
	}
	async_mutex_unlock(latest_lock);
	if(rc < 0) goto cleanup;

	alogf("fetching %s\n", URL);

	async_mutex_lock(id_lock);
	new_id = current_id++;
	async_mutex_unlock(id_lock);
	now = time(NULL);
	rc = url_fetch(URL, client, &status, &headers, &length, &hasher);
	if(rc < 0) goto cleanup;
	// TODO: Fetch errors should be recorded, not discarded.

	type = HTTPHeadersGet(headers, "Content-Type");

	rc = hx_db_open(&db);
	if(rc < 0) goto cleanup;
	rc = db_txn_begin(db, NULL, DB_RDWR, &txn);
	if(rc < 0) goto cleanup;
	rc = queue_remove(txn, then, old_id, URL, client);
	if(rc < 0) goto cleanup;
	rc = response_add(txn, now, new_id, URL, status, type, length, hasher);
	if(rc < 0) goto cleanup;
	rc = db_txn_commit(txn); txn = NULL;
	if(rc < 0) goto cleanup;

cleanup:
	db_txn_abort(txn); txn = NULL;
	hx_db_close(&db);
	HTTPHeadersFree(&headers);
	hasher_free(&hasher);

	if(rc >= 0) return;

	alogf("Worker error: %s\n", hx_strerror(rc));
	async_sleep(1000*5);
}
void queue_work_loop(void *ignored) {
	for(;;) queue_work();
}

