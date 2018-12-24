#include "ks.h"
#include "tap.h"

int test1(void)
{
	ks_pool_t *pool;
	ks_hash_t *hash;
	int i, sum1 = 0, sum2 = 0;

	ks_pool_open(&pool);
	ks_hash_create(&hash, KS_HASH_MODE_DEFAULT, KS_HASH_FREE_BOTH | KS_HASH_FLAG_RWLOCK, pool);

	for (i = 1; i < 1001; i++) {
		char *key = ks_pprintf(pool, "KEY %d", i);
		char *val = ks_pprintf(pool, "%d", i);
		ks_hash_insert(hash, key, val);
		sum1 += i;
	}



	ks_hash_iterator_t *itt;

	ks_hash_write_lock(hash);
	for (itt = ks_hash_first(hash, KS_UNLOCKED); itt; itt = ks_hash_next(&itt)) {
		const void *key;
		void *val;

		ks_hash_this(itt, &key, NULL, &val);

		printf("%s=%s\n", (char *)key, (char *)val);
		sum2 += atoi(val);

		ks_hash_remove(hash, (char *)key);
	}
	ks_hash_write_unlock(hash);

	ks_hash_destroy(&hash);

	ks_pool_close(&pool);

	return (sum1 == sum2);
}

#define MAX 100

static void *test2_thread(ks_thread_t *thread, void *data)
{
	ks_hash_iterator_t *itt;
	ks_hash_t *hash = (ks_hash_t *) data;

	while(thread->running) {
		for (itt = ks_hash_first(hash, KS_READLOCKED); itt; itt = ks_hash_next(&itt)) {
			const void *key;
			void *val;

			ks_hash_this(itt, &key, NULL, &val);

			printf("%d ITT %s=%s\n", (int)ks_thread_self_id(), (char *)key, (char *)val);
		}
		ks_sleep(100000);
	}


	return NULL;
}

int test2(void)
{
	ks_thread_t *threads[MAX];
	int ttl = 5;
	int runs = 5;
	ks_pool_t *pool;
	ks_hash_t *hash;
	int i;
	ks_hash_iterator_t *itt;

	ks_pool_open(&pool);
	ks_hash_create(&hash, KS_HASH_MODE_DEFAULT, KS_HASH_FREE_BOTH | KS_HASH_FLAG_RWLOCK, pool);

	for (i = 0; i < ttl; i++) {
		ks_thread_create(&threads[i], test2_thread, hash, pool);
	}

	for(i = 0; i < runs; i++) {
		int x = rand() % 5;
		int j;

		for (j = 0; j < 100; j++) {
			char *key = ks_pprintf(pool, "KEY %d", j);
			char *val = ks_pprintf(pool, "%d", j);
			ks_hash_insert(hash, key, val);
		}

		ks_sleep(x * 1000000);

		ks_hash_write_lock(hash);
		for (itt = ks_hash_first(hash, KS_UNLOCKED); itt; itt = ks_hash_next(&itt)) {
			const void *key;
			void *val;

			ks_hash_this(itt, &key, NULL, &val);

			printf("DEL %s=%s\n", (char *)key, (char *)val);
			ks_hash_remove(hash, (char *)key);
		}
		ks_hash_write_unlock(hash);

	}

	for (i = 0; i < ttl; i++) {
		threads[i]->running = 0;
		ks_thread_join(threads[i]);
	}


	ks_hash_destroy(&hash);
	ks_pool_close(&pool);

	return 1;
}

//#include "sodium.h"
#define TEST3_SIZE 20
int test3(void)
{
	ks_pool_t *pool;
	ks_hash_t *hash;
	ks_byte_t data[TEST3_SIZE];
	ks_byte_t data2[TEST3_SIZE];
	ks_byte_t data3[TEST3_SIZE];
	char *A, *B, *C;

	ks_pool_open(&pool);
	ks_hash_create(&hash, KS_HASH_MODE_ARBITRARY, KS_HASH_FLAG_NOLOCK, pool);
	ks_hash_set_keysize(hash, TEST3_SIZE);

	ks_rng_get_data(data, sizeof(data));
	ks_rng_get_data(data2, sizeof(data));
	//randombytes_buf(data, sizeof(data));
	//randombytes_buf(data2, sizeof(data2));

	ks_hash_insert(hash, data, "FOO");
	ks_hash_insert(hash, data2, "BAR");
	ks_hash_insert(hash, data3, "BAZ");


	A = (char *)ks_hash_search(hash, data, KS_UNLOCKED);
	B = (char *)ks_hash_search(hash, data2, KS_UNLOCKED);
	C = (char *)ks_hash_search(hash, data3, KS_UNLOCKED);


	printf("RESULT [%s][%s][%s]\n", A, B, C);

	ks_hash_destroy(&hash);

	ks_pool_close(&pool);

	return !strcmp(A, "FOO") && !strcmp(B, "BAR") && !strcmp(C, "BAZ");

}


int main(int argc, char **argv)
{

	ks_init();

	plan(3);

	ok(test1());
	ok(test2());
	ok(test3());

	ks_shutdown();

	done_testing();
}
