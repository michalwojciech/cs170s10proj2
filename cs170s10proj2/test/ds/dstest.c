#include "inc.h"

char *key_u32 = "test_u32";
char *key_str = "test_str";
char *key_mem = "test_mem";
char *key_map = "test_map";
char *key_label = "test_label";

/*===========================================================================*
 *				test_u32				     *
 *===========================================================================*/
void test_u32(void)
{
	int r;
	unsigned long value;

	/* Publish and retrieve. */
	r = ds_publish_u32(key_u32, 1234, 0);
	assert(r == OK);
	r = ds_retrieve_u32(key_u32, &value);
	assert(r == OK && value == 1234);

	/* If dstest deletes 'test_u32' immediately after publishing it,
	 * subs will catch the event, but it can't check it immediately.
	 * So dstest will sleep 2 seconds to wait for subs to complete. */
	sleep(2);

	/* Publish again without DSF_OVERWRITE. */
	r = ds_publish_u32(key_u32, 4321, 0);
	assert(r == EEXIST);

	/* Publish again with DSF_OVERWRITE to overwrite it. */
	r = ds_publish_u32(key_u32, 4321, DSF_OVERWRITE);
	assert(r == OK);
	r = ds_retrieve_u32(key_u32, &value);
	assert(r == OK && value == 4321);

	/* Delete. */
	r = ds_delete_u32(key_u32);
	assert(r == OK);
	r = ds_retrieve_u32(key_u32, &value);
	assert(r == ESRCH);

	printf("DSTEST: U32 test successful!\n");
}

/*===========================================================================*
 *				test_str				     *
 *===========================================================================*/
void test_str(void)
{
	int r;
	char *string = "little";
	char *longstring = "verylooooooongstring";
	char buf[17];

	r = ds_publish_str(key_str, string, 0);
	assert(r == OK);

	r = ds_retrieve_str(key_str, buf, 0);
	assert(r == OK && strcmp(string, buf) == 0);

	r = ds_delete_str(key_str);
	assert(r == OK);

	/* Publish a long string. */
	r = ds_publish_str(key_str, longstring, 0);
	assert(r == EINVAL);

	printf("DSTEST: STRING test successful!\n");
}

/*===========================================================================*
 *				test_mem				     *
 *===========================================================================*/
void test_mem(void)
{
	char *string1 = "ok, this is a string";
	char *string2 = "ok, this is a very looooong string";
	size_t len1 = strlen(string1) + 1;
	size_t len2 = strlen(string2) + 1;
	char buf[100];
	size_t get_len;
	int r;

	/* Publish and retrieve. */
	r = ds_publish_mem(key_mem, string1, len1, 0);
	assert(r == OK);
	get_len = 100;
	r = ds_retrieve_mem(key_mem, buf, &get_len);
	assert(r == OK && strcmp(string1, buf) == 0);
	assert(get_len == len1);

	/* Let get_len=8, which is less than strlen(string1). */
	get_len = 8;
	r = ds_retrieve_mem(key_mem, buf, &get_len);
	assert(r == OK && get_len == 8);

	/* Publish again to overwrite with a bigger memory range. */
	r = ds_publish_mem(key_mem, string2, len2, DSF_OVERWRITE);
	assert(r == OK);

	get_len = 100;
	r = ds_retrieve_mem(key_mem, buf, &get_len);
	assert(r == OK && strcmp(string2, buf) == 0);
	assert(get_len == len2);

	r = ds_delete_mem(key_mem);
	assert(r == OK);

	printf("DSTEST: MEM test successful!\n");
}

/*===========================================================================*
 *				test_label				     *
 *===========================================================================*/
void test_label(void)
{
	int r;
	char get_label[DS_MAX_KEYLEN];
	unsigned long num;

	/* Publish and retrieve. */
	r = ds_publish_label(key_label, 1234, 0);
	assert(r == OK);
	r = ds_retrieve_label_num(key_label, &num);
	assert(r == OK && num == 1234);

	/* Here are the differences w.r.t. U32. */
	r = ds_publish_label("hello", 1234, 0);
	assert(r == EEXIST);
	r = ds_retrieve_label_name(get_label, 1234);
	assert(r == OK && strcmp(key_label, get_label) == 0);

	r = ds_delete_label(key_label);
	assert(r == OK);

	printf("DSTEST: LABEL test successful!\n");
}

/*===========================================================================*
 *				test_map				     *
 *===========================================================================*/
void test_map(void)
{
	char buf_buf[CLICK_SIZE * 2];
	char buf_buf2[CLICK_SIZE * 2];
	char *buf, *buf2;
	char get_buf[CLICK_SIZE];
	int *p;
	volatile int *p2;
	int *get_p;
	size_t get_len;
	int is;
	int r;

	buf = (char*) CLICK_CEIL(buf_buf);
	buf2 = (char*) CLICK_CEIL(buf_buf2);

	p = (int *)buf;
	p2 = (int *)buf2;
	get_p = (int *)get_buf;

	*p = 1;
	r = ds_publish_map(key_map, buf, CLICK_SIZE, 0);
	assert(r == OK);

	r = ds_snapshot_map(key_map, &is);
	assert(r == OK);

	/* Copy the mapped memory range.
	 * Set *p=2, then the mapped memory range should change too
	 * and *get_p should be 2.
	 */
	*p = 2;
	get_len = CLICK_SIZE;
	r = ds_retrieve_map(key_map, get_buf, &get_len, 0, DSMF_COPY_MAPPED);
	assert(r == OK && get_len == CLICK_SIZE && *get_p == 2);

	/* Copy snapshot, where *get_p should still be 1. */
	get_len = CLICK_SIZE;
	r = ds_retrieve_map(key_map, get_buf, &get_len, is, DSMF_COPY_SNAPSHOT);
	assert(r == OK && get_len == CLICK_SIZE && *get_p == 1);

	/* Map the mapped memory range to @buf2, then set *p=3, which
	 * in turn should let *p2=3.
	 */
	get_len = CLICK_SIZE;
	r = ds_retrieve_map(key_map, buf2, &get_len, 0, DSMF_MAP_MAPPED);
	assert(r == OK && get_len == CLICK_SIZE);
	*p = 3;
	assert(*p2 == 3);

	r = ds_delete_map(key_map);
	assert(r == OK);

	printf("DSTEST: MAP test successful!\n");
}

/* SEF functions and variables. */
FORWARD _PROTOTYPE( void sef_local_startup, (void) );

/*===========================================================================*
 *				main					     *
 *===========================================================================*/
int main(void)
{
	/* SEF local startup. */
	sef_local_startup();

	/* Run all the tests. */
	test_u32();
	test_str();
	test_mem();
	test_map();
	test_label();

	return 0;
}


/*===========================================================================*
 *			       sef_local_startup			     *
 *===========================================================================*/
PRIVATE void sef_local_startup()
{
  /* Let SEF perform startup. */
  sef_startup();
}

