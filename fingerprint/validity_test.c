/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host unit tests for Validity 9x backend (validity.h):
 *  - Exact SHA-256 vector cross-checks against python-validity (PRF/DMI hwkey,
 *    bitpack, calibration average/process, and build_cmd_02 for 0x0199 & 0x00db)
 *  - SID <-> gbOS template-name round-trip & dual-boot safety (Windows/Linux SIDs)
 *  - Full simulated Validity 138a:0097 sensor: TLS 1.2 handshake (including every
 *    Synaptics wire quirk), encrypted commands, calibration + clean-slate caching,
 *    interrupt URB capture, enroll, identify, template policy, delete, and cancel.
 */
#define _GNU_SOURCE
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/ecdh.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

static void val_test_log(const char *fmt, ...);
#define GOODIX_LOGI(...) ((void)0)
#define GOODIX_LOGW(...) ((void)0)
#define GOODIX_LOGE(...) ((void)0)
#define FP_LOGI(...) val_test_log(__VA_ARGS__)
#define FP_LOGW(...) val_test_log(__VA_ARGS__)
#define VALIDITY_LOGI(...) val_test_log(__VA_ARGS__)
#define VALIDITY_LOGW(...) val_test_log(__VA_ARGS__)
#define VALIDITY_LOGE(...) val_test_log(__VA_ARGS__)

static int sim_ioctl(int fd, unsigned long request, ...);
static int sim_poll(struct pollfd *fds, unsigned int nfds, int timeout);
#define ioctl sim_ioctl
#define poll sim_poll
#include "goodix.h"
#include "gbos_fp_policy.h"
#include "validity.h"
#undef ioctl
#undef poll

static char test_log_last[256];
static void val_test_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(test_log_last, sizeof(test_log_last), fmt, ap);
	va_end(ap);
}

static void assert_sha256(const uint8_t *data, size_t len, const char *expected_hex)
{
	uint8_t digest[32];
	char hex[65];

	assert(val_sha256(data, len, digest));
	for (int i = 0; i < 32; i++)
		snprintf(hex + i * 2, 3, "%02x", digest[i]);
	if (strcmp(hex, expected_hex) != 0) {
		fprintf(stderr, "SHA256 mismatch:\n  got:  %s\n  want: %s\n", hex, expected_hex);
		abort();
	}
}

static void assert_hex(const uint8_t *data, size_t len, const char *expected_hex)
{
	char hex[256];

	assert(len * 2 < sizeof(hex));
	for (size_t i = 0; i < len; i++)
		snprintf(hex + i * 2, 3, "%02x", data[i]);
	if (strcmp(hex, expected_hex) != 0) {
		fprintf(stderr, "HEX mismatch:\n  got:  %s\n  want: %s\n", hex, expected_hex);
		abort();
	}
}

/* ---- 1. Exact python-validity reference vector tests ---- */

static void test_python_validity_vectors(void)
{
	struct validity s;
	uint8_t fvals[112], fvals_db[144], fdata_db[144];
	uint8_t u = 0, mn = 0, bp[128];
	size_t bp_len = 0, out_len = 0;
	uint8_t *cmd = malloc(VALIDITY_IO_BUF_MAX);
	uint8_t *raw = malloc(3 * 224 * 120);
	uint8_t *avg = malloc(VALIDITY_CALIB_MAX);

	assert(cmd && raw && avg);
	validity_init(&s);
	assert(validity_set_hwkey(&s, "20HD000MUS", "PF0ABCDE"));
	assert_hex(s.psk_enc, 32, "064299d86203ab145b8fa4d6fd0d7694bdef5965fd49225388b562df141f5b88");
	assert_hex(s.psk_val, 32, "5d51715ce3c62803ee0f1a65ac6b4e1db7f025bbf6cf6269371c4b515e57b5bb");

	for (size_t i = 0; i < 112; i++)
		fvals[i] = (uint8_t)(0x20 + (i * 7) % 19);
	val_bitpack(fvals, sizeof(fvals), &u, &mn, bp, &bp_len);
	assert(u == 5 && mn == 32 && bp_len == 70);
	assert_sha256(bp, bp_len, "e36fcaab1cbd2f65d283207e54a830272af9889ca1783f79723057dd9e574ed7");

	/* Type 0x0199 (ThinkPad T470 138a:0097) */
	s.rom.major = 6; s.rom.minor = 7; s.rom.build = 123; s.rom.product = 0x30;
	s.type_info = &validity_sensor_types[0];
	assert(s.type_info->sensor_type == 0x0199);
	s.lines_per_frame = 0x70 * 2;
	memcpy(s.factory_calib_values, fvals, sizeof(fvals));
	s.factory_calib_values_len = sizeof(fvals);
	s.calib_data_len = 0;

	assert(validity_build_cmd_02(&s, VALIDITY_MODE_CALIBRATE, cmd, VALIDITY_IO_BUF_MAX, &out_len) == 0);
	assert(out_len == 885);
	assert_sha256(cmd, out_len, "e9a4ed32084fb91097a8750221a1f5fbe2d775ec7b58b3aef895fb14cc7574c4");

	for (size_t i = 0; i < 3 * 224 * 120; i++)
		raw[i] = (uint8_t)((i * 13 + 0x40) & 0xff);
	assert(validity_average_calib(&s, raw, 3 * 224 * 120, avg, &out_len) == 0);
	assert(out_len == 13440);
	assert_sha256(avg, out_len, "71c298642568b602d5974e02f857e5bcac431580dd200e1d01986c86741a3f5a");

	validity_process_calib(&s, avg, out_len);
	assert(s.calib_data_len == 13440);
	assert_sha256(s.calib_data, s.calib_data_len, "2c612afbea8c5a253a3cd0db8fd62fdf3a714a90ffa02757caf8ba56cca3414c");

	validity_process_calib(&s, avg, out_len);
	assert_sha256(s.calib_data, s.calib_data_len, "a8d3f36b375a3844c6427845500dbe126cdc95d8cfd22af80e0bd6adbe916bd9");

	assert(validity_build_cmd_02(&s, VALIDITY_MODE_IDENTIFY, cmd, VALIDITY_IO_BUF_MAX, &out_len) == 0);
	assert(out_len == 13729);
	assert_sha256(cmd, out_len, "b84856333d1cb80edda07b0f536f397a7767c698b8b7c3a2ad88d691b66f66b5");

	assert(validity_build_cmd_02(&s, VALIDITY_MODE_ENROLL, cmd, VALIDITY_IO_BUF_MAX, &out_len) == 0);
	assert(out_len == 13729);
	assert_sha256(cmd, out_len, "8c04424e5d4fbc4eaa7ebee8a8b95e86e4aa6535e0bb0c99decb6f5e23a1ea8b");

	/* Type 0x00db (ThinkPad 138a:0090) */
	s.type_info = &validity_sensor_types[1];
	assert(s.type_info->sensor_type == 0x00db);
	s.lines_per_frame = 0x90;
	s.calib_data_len = 0;
	for (size_t i = 0; i < 144; i++) {
		fvals_db[i] = (uint8_t)(0x10 + i);
		fdata_db[i] = (uint8_t)(0x30 + i);
	}
	memcpy(s.factory_calib_values, fvals_db, 144);
	s.factory_calib_values_len = 144;
	memcpy(s.factory_calib_data, fdata_db, 144);
	s.factory_calib_data_len = 144;

	assert(validity_build_cmd_02(&s, VALIDITY_MODE_CALIBRATE, cmd, VALIDITY_IO_BUF_MAX, &out_len) == 0);
	assert(out_len == 1517);
	assert_sha256(cmd, out_len, "4b924c58fa534fbe531c19c10b5dd483c38e1de95ef2c501d29b73f33686a738");

	assert(validity_build_cmd_02(&s, VALIDITY_MODE_IDENTIFY, cmd, VALIDITY_IO_BUF_MAX, &out_len) == 0);
	assert(out_len == 1593);
	assert_sha256(cmd, out_len, "3bf7786bae96999a6130ec08ea9decdc18bae1f71d9ec8d1a0c2c627cd1ff8e2");

	assert(validity_build_cmd_02(&s, VALIDITY_MODE_ENROLL, cmd, VALIDITY_IO_BUF_MAX, &out_len) == 0);
	assert(out_len == 1593);
	assert_sha256(cmd, out_len, "c7b2c9d14c6651e15aa0ee7d6f1268854740d328c8f1d73cb5d802791b27dde9");

	free(cmd);
	free(raw);
	free(avg);
}

/* ---- 2. SID <-> gbOS name mapping & dual-boot safety ---- */

static void test_sid_mapping(void)
{
	uint8_t id[0x4c];
	char name[FP_NAME_MAX + 1];

	assert(validity_name_to_identity("GBOS-1234abcd-u0-7fffffff", id));
	assert(validity_identity_to_name(id, sizeof(id), name));
	assert(strcmp(name, "GBOS-1234abcd-u0-7fffffff") == 0);

	assert(validity_name_to_identity("GBOS-L-0123abcd-u10-00000042", id));
	assert(validity_identity_to_name(id, sizeof(id), name));
	assert(strcmp(name, "GBOS-L-0123abcd-u10-00000042") == 0);

	/* Windows Hello SID: S-1-5-21-394619333-3876782012-1672975908-1001 */
	memset(id, 0, sizeof(id));
	val_put_le32(id + 0, 3);
	val_put_le32(id + 4, 28);
	id[8] = 1; id[9] = 5; id[15] = 5;
	val_put_le32(id + 16, 21);
	val_put_le32(id + 20, 394619333U);
	val_put_le32(id + 24, 3876782012U);
	val_put_le32(id + 28, 1672975908U);
	val_put_le32(id + 32, 1001U);
	assert(!validity_identity_to_name(id, sizeof(id), name));
	assert(name[0] == 0);

	/* Linux python-validity SID: S-1-5-21-111111111-1111111111-1111111111-1000 */
	val_put_le32(id + 20, 111111111U);
	val_put_le32(id + 24, 1111111111U);
	val_put_le32(id + 28, 1111111111U);
	val_put_le32(id + 32, 1000U);
	assert(!validity_identity_to_name(id, sizeof(id), name));
	assert(name[0] == 0);
}

/* ---- 3. Full simulated Validity 138a:0097 sensor with real TLS 1.2 ---- */

struct sim_user {
	bool used;
	uint16_t dbid;
	uint8_t identity[0x4c];
	uint16_t finger_dbid;
	int finger_id;
};

static struct {
	struct validity *dev;
	EC_KEY *srv_ecdh;
	EC_KEY *cli_key;
	uint8_t cli_cert[64];
	uint8_t tls_flash[0x1000];
	uint8_t part6[32768];
	size_t part6_len;

	/* Server-side TLS state */
	EVP_MD_CTX *hs_hash;
	bool tls_established;
	uint8_t client_random[32];
	uint8_t server_random[32];
	uint8_t master_secret[48];
	uint8_t cli_sign_key[32];
	uint8_t srv_sign_key[32];
	uint8_t cli_enc_key[32];
	uint8_t srv_enc_key[32];

	/* Queued USB responses */
	uint8_t ep81_buf[VALIDITY_IO_BUF_MAX];
	size_t ep81_len;
	size_t ep81_pos;

	uint8_t *ep82_buf;
	size_t ep82_len;
	size_t ep82_pos;

	uint8_t int_q[16][16];
	size_t int_len[16];
	int int_n;
	struct validity_usb_urb *pending_urb;
	bool cancel_on_poll;
	int discard_count;

	/* Simulated on-chip DB & capture state */
	struct sim_user users[16];
	uint16_t next_dbid;
	int calib_captures;
	int enroll_scans;
	int inject_51_err;
	int finger_present;		/* which physical finger is placed on sensor */
} vsim;

static void vsim_push_int(const uint8_t *b, size_t n)
{
	assert(vsim.int_n < 16 && n <= 16);
	memcpy(vsim.int_q[vsim.int_n], b, n);
	vsim.int_len[vsim.int_n] = n;
	vsim.int_n++;
}

static void vsim_set_ep81(const uint8_t *b, size_t n)
{
	assert(n <= sizeof(vsim.ep81_buf));
	memcpy(vsim.ep81_buf, b, n);
	vsim.ep81_len = n;
	vsim.ep81_pos = 0;
}

static void vsim_build_tls_flash(const uint8_t psk_enc[32], const uint8_t psk_val[32])
{
	uint8_t x_be[32], y_be[32], d_be[32];
	uint8_t plain[112], iv[16], priv_blob[1 + 16 + 112 + 32];
	uint8_t ecdh_blob[0x94 + 80];
	BIGNUM *bx = BN_new(), *by = BN_new();
	size_t enc_len = 0, off = 0;
	unsigned int hlen = 0;

	vsim.srv_ecdh = EC_KEY_new_by_curve_name(VALIDITY_NID_P256);
	vsim.cli_key = EC_KEY_new_by_curve_name(VALIDITY_NID_P256);
	assert(vsim.srv_ecdh && vsim.cli_key);
	assert(EC_KEY_generate_key(vsim.srv_ecdh) == 1);
	assert(EC_KEY_generate_key(vsim.cli_key) == 1);
	memset(vsim.cli_cert, 0x42, sizeof(vsim.cli_cert));

	/* Build priv_blob (id 4) */
	assert(EC_POINT_get_affine_coordinates_GFp(EC_KEY_get0_group(vsim.cli_key),
						   EC_KEY_get0_public_key(vsim.cli_key), bx, by, NULL) == 1);
	assert(val_bn_to_be32(bx, x_be) && val_bn_to_be32(by, y_be) &&
	       val_bn_to_be32(EC_KEY_get0_private_key(vsim.cli_key), d_be));
	val_reverse_copy(plain + 0, x_be, 32);
	val_reverse_copy(plain + 32, y_be, 32);
	val_reverse_copy(plain + 64, d_be, 32);
	memset(plain + 96, 16, 16);
	memset(iv, 0x11, 16);
	priv_blob[0] = 0x02;
	memcpy(priv_blob + 1, iv, 16);
	assert(val_aes256_cbc(true, psk_enc, iv, plain, sizeof(plain), priv_blob + 17, &enc_len) && enc_len == 112);
	assert(HMAC(EVP_sha256(), psk_val, 32, priv_blob + 1, 16 + 112, priv_blob + 17 + 112, &hlen) && hlen == 32);

	/* Build ecdh_blob (id 6): we override handle_ecdh's fwpub? No, in sim we test real handle_priv + TLS
	 * handshake directly; for handle_ecdh we verify the key coords directly and test TLS flash block framing. */
	memset(ecdh_blob, 0, sizeof(ecdh_blob));
	assert(EC_POINT_get_affine_coordinates_GFp(EC_KEY_get0_group(vsim.srv_ecdh),
						   EC_KEY_get0_public_key(vsim.srv_ecdh), bx, by, NULL) == 1);
	assert(val_bn_to_be32(bx, x_be) && val_bn_to_be32(by, y_be));
	val_reverse_copy(ecdh_blob + 0x08, x_be, 32);
	val_reverse_copy(ecdh_blob + 0x4c, y_be, 32);

	/* Pack blocks 3 and 4 into tls_flash */
	val_put_le16(vsim.tls_flash + off, 3);
	val_put_le16(vsim.tls_flash + off + 2, sizeof(vsim.cli_cert));
	assert(val_sha256(vsim.cli_cert, sizeof(vsim.cli_cert), vsim.tls_flash + off + 4));
	memcpy(vsim.tls_flash + off + 36, vsim.cli_cert, sizeof(vsim.cli_cert));
	off += 36 + sizeof(vsim.cli_cert);

	val_put_le16(vsim.tls_flash + off, 4);
	val_put_le16(vsim.tls_flash + off + 2, sizeof(priv_blob));
	assert(val_sha256(priv_blob, sizeof(priv_blob), vsim.tls_flash + off + 4));
	memcpy(vsim.tls_flash + off + 36, priv_blob, sizeof(priv_blob));
	off += 36 + sizeof(priv_blob);

	memset(vsim.tls_flash + off, 0xff, sizeof(vsim.tls_flash) - off);
	BN_free(bx);
	BN_free(by);
}

static void vsim_tls_encrypt_send(uint8_t rec_type, const uint8_t *plain, size_t plain_len,
				  uint8_t *out, size_t *out_len)
{
	size_t signed_len = plain_len + 32;
	size_t pad_len = 16 - (signed_len % 16);
	size_t padded_len = signed_len + pad_len;
	uint8_t *buf = malloc(5 + padded_len);
	uint8_t iv[16];
	unsigned int hlen = 0;
	size_t enc_len = 0;

	assert(buf);
	buf[0] = rec_type; buf[1] = 3; buf[2] = 3;
	val_put_be16(buf + 3, (uint16_t)plain_len);
	memcpy(buf + 5, plain, plain_len);
	assert(HMAC(EVP_sha256(), vsim.srv_sign_key, 32, buf, 5 + plain_len, buf + 5 + plain_len, &hlen) && hlen == 32);
	memset(buf + 5 + signed_len, (uint8_t)(pad_len - 1), pad_len);
	memset(iv, 0x33, 16);
	out[0] = rec_type; out[1] = 3; out[2] = 3;
	val_put_be16(out + 3, (uint16_t)(16 + padded_len));
	memcpy(out + 5, iv, 16);
	assert(val_aes256_cbc(true, vsim.srv_enc_key, iv, buf + 5, padded_len, out + 5 + 16, &enc_len) &&
	       enc_len == padded_len);
	*out_len = 5 + 16 + enc_len;
	free(buf);
}

static size_t vsim_tls_decrypt(uint8_t rec_type, const uint8_t *enc, size_t enc_len, uint8_t *plain_out)
{
	size_t dec_len = 0, unpadded, msg_len;
	uint8_t *buf = malloc(5 + enc_len);
	uint8_t pad_val, mac[32];
	unsigned int hlen = 0;

	assert(buf && enc_len >= 32);
	assert(val_aes256_cbc(false, vsim.cli_enc_key, enc, enc + 16, enc_len - 16, buf + 5, &dec_len));
	pad_val = buf[5 + dec_len - 1];
	assert((size_t)pad_val + 1 <= dec_len);
	unpadded = dec_len - ((size_t)pad_val + 1);
	assert(unpadded >= 32);
	msg_len = unpadded - 32;
	buf[0] = rec_type; buf[1] = 3; buf[2] = 3;
	val_put_be16(buf + 3, (uint16_t)msg_len);
	assert(HMAC(EVP_sha256(), vsim.cli_sign_key, 32, buf, 5 + msg_len, mac, &hlen) && hlen == 32);
	assert(memcmp(mac, buf + 5 + msg_len, 32) == 0);
	memcpy(plain_out, buf + 5, msg_len);
	free(buf);
	return msg_len;
}

static void vsim_handle_app_cmd(const uint8_t *cmd, size_t len, uint8_t *rsp, size_t *rsp_len)
{
	uint8_t op = cmd[0];

	*rsp_len = 2;
	rsp[0] = 0; rsp[1] = 0;
	switch (op) {
	case 0x01: /* RomInfo */
		memset(rsp, 0, 18);
		val_put_le32(rsp + 6, 42);
		rsp[10] = 6; rsp[11] = 7; rsp[13] = 0x30;
		*rsp_len = 18;
		break;
	case 0x02: { /* capture / calibrate */
		uint16_t req_lines = val_le16(cmd + 3);
		if (req_lines > 0) {
			vsim.calib_captures++;
			vsim.ep82_len = (size_t)req_lines * 120;
			vsim.ep82_pos = 0;
			for (size_t i = 0; i < vsim.ep82_len; i++)
				vsim.ep82_buf[i] = (uint8_t)((i * 7 + 0x55) & 0xff);
		} else {
			static const uint8_t i0[4] = { 0, 0, 0, 0 };
			static const uint8_t i2[4] = { 2, 0, 0, 0 };
			static const uint8_t i3[4] = { 3, 0, 4, 0 };
			vsim.int_n = 0;
			vsim_push_int(i0, 4);
			vsim_push_int(i2, 4);
			vsim_push_int(i3, 4);
		}
		memset(rsp, 0, 2400);
		*rsp_len = 2400;
		break;
	}
	case 0x39:
		assert(len == 125);
		break;
	case 0x04: case 0x05: case 0x06: case 0x19: case 0x1a: case 0x62: case 0x69:
		break;
	case 0x3e: /* get_flash_info: 5 partitions already initialized */
		memset(rsp, 0, 16 + 5 * 12);
		val_put_le16(rsp + 14, 5);
		*rsp_len = 16 + 5 * 12;
		break;
	case 0x3f: /* erase_flash */
		if (cmd[1] == 6) {
			memset(vsim.part6, 0xff, sizeof(vsim.part6));
			vsim.part6_len = 0;
		}
		break;
	case 0x40: { /* read_flash */
		uint8_t part = cmd[1];
		uint32_t addr = val_le32(cmd + 5);
		uint32_t sz = val_le32(cmd + 9);
		const uint8_t *src = part == 1 ? vsim.tls_flash : vsim.part6;
		size_t cap = part == 1 ? sizeof(vsim.tls_flash) : sizeof(vsim.part6);
		assert(addr + sz <= cap);
		val_put_le32(rsp + 2, sz);
		val_put_le16(rsp + 6, 0);
		memcpy(rsp + 8, src + addr, sz);
		*rsp_len = 8 + sz;
		break;
	}
	case 0x41: { /* write_flash */
		uint8_t part = cmd[1];
		uint32_t addr = val_le32(cmd + 5);
		uint32_t sz = val_le32(cmd + 9);
		assert(len == 13 + sz);
		if (part == 6) {
			assert(addr + sz <= sizeof(vsim.part6));
			memcpy(vsim.part6 + addr, cmd + 13, sz);
			if (addr + sz > vsim.part6_len)
				vsim.part6_len = addr + sz;
		}
		break;
	}
	case 0x43: /* get_fw_info(2): present v1.1 */
		memset(rsp, 0, 12);
		val_put_le16(rsp + 2, 1);
		val_put_le16(rsp + 4, 1);
		*rsp_len = 12;
		break;
	case 0x45: /* db_info */
		memset(rsp, 0, 2 + 0x18);
		*rsp_len = 2 + 0x18;
		break;
	case 0x47: { /* new_record */
		uint16_t parent = val_le16(cmd + 1);
		uint16_t type = val_le16(cmd + 3);
		uint16_t dlen = val_le16(cmd + 7);
		uint16_t id = vsim.next_dbid++;
		if (type == 5) {
			for (int i = 0; i < 16; i++) {
				if (!vsim.users[i].used) {
					vsim.users[i].used = true;
					vsim.users[i].dbid = id;
					assert(dlen == 0x4c);
					memcpy(vsim.users[i].identity, cmd + 9, 0x4c);
					break;
				}
			}
		} else if (type == 0x0b) {
			static const uint8_t done_int[4] = { 3, 0, 0, 0 };
			for (int i = 0; i < 16; i++) {
				if (vsim.users[i].used && vsim.users[i].dbid == parent) {
					vsim.users[i].finger_dbid = id;
					vsim.users[i].finger_id = vsim.finger_present;
					break;
				}
			}
			vsim_push_int(done_int, 4);
		}
		val_put_le16(rsp + 2, id);
		*rsp_len = 4;
		break;
	}
	case 0x48: { /* del_record */
		uint16_t id = val_le16(cmd + 1);
		for (int i = 0; i < 16; i++) {
			if (!vsim.users[i].used)
				continue;
			if (vsim.users[i].dbid == id)
				vsim.users[i].used = false;
			else if (vsim.users[i].finger_dbid == id)
				vsim.users[i].finger_dbid = 0;
		}
		break;
	}
	case 0x4a: { /* get_user / lookup_user */
		uint16_t dbid = val_le16(cmd + 1);
		uint16_t idsz = val_le16(cmd + 5);
		int found = -1;
		for (int i = 0; i < 16; i++) {
			if (!vsim.users[i].used)
				continue;
			if (dbid != 0 && vsim.users[i].dbid == dbid) {
				found = i;
				break;
			}
			if (dbid == 0 && idsz == 0x4c && memcmp(vsim.users[i].identity, cmd + 7, 0x4c) == 0) {
				found = i;
				break;
			}
		}
		if (found < 0) {
			val_put_le16(rsp, 0x04b3);
			*rsp_len = 2;
		} else {
			uint16_t fcnt = vsim.users[found].finger_dbid ? 1 : 0;
			size_t off = 10;
			val_put_le16(rsp + 2, vsim.users[found].dbid);
			val_put_le16(rsp + 4, fcnt);
			val_put_le16(rsp + 6, 0);
			val_put_le16(rsp + 8, 0x4c);
			if (fcnt) {
				val_put_le16(rsp + off, vsim.users[found].finger_dbid);
				val_put_le16(rsp + off + 2, 0xf5);
				val_put_le16(rsp + off + 4, 3);
				val_put_le16(rsp + off + 6, 16);
				off += 8;
			}
			memcpy(rsp + off, vsim.users[found].identity, 0x4c);
			*rsp_len = off + 0x4c;
		}
		break;
	}
	case 0x4b: { /* get_user_storage(StgWindsor) */
		uint16_t cnt = 0;
		size_t off = 10;
		val_put_le16(rsp + 2, 3);
		for (int i = 0; i < 16; i++) {
			if (vsim.users[i].used) {
				val_put_le16(rsp + off, vsim.users[i].dbid);
				val_put_le16(rsp + off + 2, 0x4c);
				off += 4;
				cnt++;
			}
		}
		val_put_le16(rsp + 4, cnt);
		val_put_le16(rsp + 6, 11);
		val_put_le16(rsp + 8, 0);
		memcpy(rsp + off, "StgWindsor", 11);
		*rsp_len = off + 11;
		break;
	}
	case 0x51: /* get_prg_status2 */
		memset(rsp, 0, 18);
		if (vsim.inject_51_err > 0) {
			vsim.inject_51_err--;
			val_put_le16(rsp, 0x0007);
		}
		val_put_le32(rsp + 2, 12);
		*rsp_len = 18;
		break;
	case 0x5e: { /* match_finger */
		int found = -1;
		for (int i = 0; i < 16; i++) {
			if (vsim.users[i].used && vsim.users[i].finger_dbid &&
			    vsim.users[i].finger_id == vsim.finger_present) {
				found = i;
				break;
			}
		}
		uint8_t ev[4] = { (uint8_t)(found >= 0 ? 3 : 1), 0, 0, 0 };
		vsim_push_int(ev, 4);
		break;
	}
	case 0x60: { /* match results */
		int found = -1;
		for (int i = 0; i < 16; i++) {
			if (vsim.users[i].used && vsim.users[i].finger_dbid &&
			    vsim.users[i].finger_id == vsim.finger_present) {
				found = i;
				break;
			}
		}
		assert(found >= 0);
		/* dict: tag 1 (4B usrid), tag 3 (2B subtype), tag 4 (32B hash) */
		size_t pos = 4;
		val_put_le16(rsp + pos, 1); val_put_le16(rsp + pos + 2, 4);
		val_put_le32(rsp + pos + 4, vsim.users[found].dbid);
		pos += 8;
		val_put_le16(rsp + pos, 3); val_put_le16(rsp + pos + 2, 2);
		val_put_le16(rsp + pos + 4, 0xf5);
		pos += 6;
		val_put_le16(rsp + pos, 4); val_put_le16(rsp + pos + 2, 32);
		memset(rsp + pos + 4, 0xaa, 32);
		pos += 36;
		val_put_le16(rsp + 2, (uint16_t)(pos - 4));
		*rsp_len = pos;
		break;
	}
	case 0x68: { /* enrollment_update_start */
		static const uint8_t done_int[4] = { 3, 0, 0, 0 };
		val_put_le32(rsp + 2, 1234);
		*rsp_len = 6;
		vsim_push_int(done_int, 4);
		break;
	}
	case 0x6b: { /* enrollment_update (called twice per stage) */
		static int toggle = 0;
		if ((toggle++ & 1) == 0) {
			static const uint8_t done_int[4] = { 3, 0, 0, 0 };
			vsim_push_int(done_int, 4);
			*rsp_len = 2;
		} else {
			size_t pos = 4;
			vsim.enroll_scans++;
			/* tag 0: template (0x38 header + 16B body) */
			memset(rsp + pos, 0, 0x38 + 16);
			val_put_le16(rsp + pos, 0);
			val_put_le16(rsp + pos + 2, 16);
			pos += 0x38 + 16;
			/* On 3rd scan, also emit tag 3 (tid) to finish enrollment */
			if (vsim.enroll_scans >= 3) {
				memset(rsp + pos, 0, 0x38 + 16);
				val_put_le16(rsp + pos, 3);
				val_put_le16(rsp + pos + 2, 16);
				memset(rsp + pos + 0x38, 0x99, 16);
				pos += 0x38 + 16;
			}
			val_put_le16(rsp + 2, (uint16_t)(pos - 4));
			*rsp_len = pos;
		}
		break;
	}
	case 0x6f: { /* get_factory_bits(0x0e00) */
		size_t off = 2;
		val_put_le32(rsp + off, 0); off += 4;
		val_put_le32(rsp + off, 1); off += 4;		/* 1 entry: subtag 3 */
		val_put_le32(rsp + off, 0);
		val_put_le16(rsp + off + 4, 4 + 112);		/* len */
		val_put_le16(rsp + off + 6, 0x0e00);
		val_put_le16(rsp + off + 8, 3);			/* subtag 3 */
		val_put_le16(rsp + off + 10, 0x8000);
		off += 12;
		memset(rsp + off, 0, 4);
		for (int i = 0; i < 112; i++)
			rsp[off + 4 + i] = (uint8_t)(0x20 + (i % 15));
		off += 4 + 112;
		*rsp_len = off;
		break;
	}
	case 0x75: /* identify_sensor: major=0x0190, minor=0x4a -> type 0x0199 (57K0) */
		val_put_le32(rsp + 2, 0);
		val_put_le16(rsp + 6, 0x004a);
		val_put_le16(rsp + 8, 0x0190);
		*rsp_len = 10;
		break;
	default:
		fprintf(stderr, "sim: unhandled Validity cmd 0x%02x\n", op);
		abort();
	}
}

static void vsim_handle_tls_handshake(const uint8_t *tx, size_t len)
{
	/* Both handshake flights start with 44 00 00 00 16 03 03 ... */
	assert(len > 9 && tx[0] == 0x44 && tx[4] == 0x16 && tx[5] == 3 && tx[6] == 3);
	uint16_t rlen = val_be16(tx + 7);
	const uint8_t *hs = tx + 9;

	if (hs[0] == 0x01) {
		/* Flight 1: ClientHello -> verify quirks (-2 ext len, 00 compression) */
		uint8_t rsp[256], body[128];
		size_t bp = 0, rp = 0;

		assert(hs[4] == 3 && hs[5] == 3);
		memcpy(vsim.client_random, hs + 6, 32);
		assert(hs[38] == 7);			/* 7-byte session id */
		assert(hs[46] == 0 && hs[47] == 6);	/* cipher suites len 6 */
		assert(hs[54] == 0);			/* compression len 0 */
		assert(hs[55] == 0 && hs[56] == 10);	/* 12 - 2 = 10 quirk */

		EVP_MD_CTX_free(vsim.hs_hash);
		vsim.hs_hash = EVP_MD_CTX_new();
		assert(EVP_DigestInit_ex(vsim.hs_hash, EVP_sha256(), NULL) == 1);
		assert(EVP_DigestUpdate(vsim.hs_hash, hs, rlen) == 1);

		/* ServerHello (02) */
		memset(vsim.server_random, 0x77, 32);
		body[bp] = 0x02; val_put_be24(body + bp + 1, 2 + 32 + 1 + 2 + 1); bp += 4;
		body[bp++] = 3; body[bp++] = 3;
		memcpy(body + bp, vsim.server_random, 32); bp += 32;
		body[bp++] = 0;				/* sessid len 0 */
		body[bp++] = 0xc0; body[bp++] = 0x05;	/* suite */
		body[bp++] = 0;				/* no compression */
		/* CertificateRequest (0d) */
		body[bp] = 0x0d; val_put_be24(body + bp + 1, 4); bp += 4;
		body[bp++] = 0x01; body[bp++] = 0x40; body[bp++] = 0x00; body[bp++] = 0x00;
		/* ServerHelloDone (0e) */
		body[bp] = 0x0e; val_put_be24(body + bp + 1, 0); bp += 4;

		assert(EVP_DigestUpdate(vsim.hs_hash, body, bp) == 1);
		rsp[rp++] = 0x16; rsp[rp++] = 3; rsp[rp++] = 3;
		val_put_be16(rsp + rp, (uint16_t)bp); rp += 2;
		memcpy(rsp + rp, body, bp); rp += bp;
		vsim_set_ep81(rsp, rp);
	} else {
		/* Flight 2: Certificate + ClientKeyExchange + CertificateVerify + CCS + EncryptedFinished */
		size_t hoff = 0, off = 9 + rlen;
		uint8_t pms[32], seed[64], ms_seed[13 + 64], ke_seed[13 + 64], kb[0x120];
		uint8_t cfin[64], sfin[16], cur_hash[32], fseed[15 + 32], expect[12];
		unsigned int dlen = 0;
		EVP_MD_CTX *copy = EVP_MD_CTX_new();

		while (hoff < rlen) {
			uint8_t ht = hs[hoff];
			uint32_t hl = val_be24(hs + hoff + 1);
			const uint8_t *hp = hs + hoff + 4;

			if (ht == 0x0b) {
				/* Verify Synaptics ac16 + dual length cert header */
				assert(val_be24(hp) == sizeof(vsim.cli_cert));
				assert(val_be24(hp + 3) == sizeof(vsim.cli_cert));
				assert(hp[6] == 0xac && hp[7] == 0x16);
				assert(memcmp(hp + 8, vsim.cli_cert, sizeof(vsim.cli_cert)) == 0);
			} else if (ht == 0x10) {
				/* ClientKeyExchange: compute ECDH with client's ephemeral pubkey */
				EC_POINT *cli_eph;
				assert(hl == 65 && hp[0] == 0x04);
				cli_eph = val_ec_point_from_be32(EC_KEY_get0_group(vsim.srv_ecdh), hp + 1, hp + 33);
				assert(cli_eph && ECDH_compute_key(pms, 32, cli_eph, vsim.srv_ecdh, NULL) == 32);
				EC_POINT_free(cli_eph);
				memcpy(seed, vsim.client_random, 32);
				memcpy(seed + 32, vsim.server_random, 32);
				memcpy(ms_seed, "master secret", 13); memcpy(ms_seed + 13, seed, 64);
				assert(validity_prf(pms, 32, ms_seed, sizeof(ms_seed), vsim.master_secret, 48));
				memcpy(ke_seed, "key expansion", 13); memcpy(ke_seed + 13, seed, 64);
				assert(validity_prf(vsim.master_secret, 48, ke_seed, sizeof(ke_seed), kb, sizeof(kb)));
				memcpy(vsim.cli_sign_key, kb + 0x00, 32);
				memcpy(vsim.srv_sign_key, kb + 0x20, 32);
				memcpy(vsim.cli_enc_key,  kb + 0x40, 32);
				memcpy(vsim.srv_enc_key,  kb + 0x60, 32);
			} else if (ht == 0x0f) {
				/* CertificateVerify: verify client's ECDSA signature over handshake hash so far */
				assert(EVP_MD_CTX_copy_ex(copy, vsim.hs_hash) == 1);
				assert(EVP_DigestFinal_ex(copy, cur_hash, &dlen) == 1 && dlen == 32);
				assert(ECDSA_verify(0, cur_hash, 32, hp, hl, vsim.cli_key) == 1);
			}
			assert(EVP_DigestUpdate(vsim.hs_hash, hs + hoff, 4 + hl) == 1);
			hoff += 4 + hl;
		}
		/* Expect ChangeCipherSpec + Encrypted ClientFinished */
		assert(off + 6 < len && tx[off] == 0x14 && tx[off + 5] == 0x01);
		off += 6;
		assert(off + 5 <= len && tx[off] == 0x16);
		uint16_t flen = val_be16(tx + off + 3);
		size_t cfin_len = vsim_tls_decrypt(0x16, tx + off + 5, flen, cfin);
		assert(cfin_len == 16 && cfin[0] == 0x14 && val_be24(cfin + 1) == 12);

		assert(EVP_MD_CTX_copy_ex(copy, vsim.hs_hash) == 1);
		assert(EVP_DigestFinal_ex(copy, cur_hash, &dlen) == 1 && dlen == 32);
		memcpy(fseed, "client finished", 15); memcpy(fseed + 15, cur_hash, 32);
		assert(validity_prf(vsim.master_secret, 48, fseed, sizeof(fseed), expect, 12));
		assert(memcmp(expect, cfin + 4, 12) == 0);
		/* Real Validity 9x firmware does not hash ClientFinished before ServerFinished. */

		/* Build Server CCS + Encrypted ServerFinished */
		assert(EVP_MD_CTX_copy_ex(copy, vsim.hs_hash) == 1);
		assert(EVP_DigestFinal_ex(copy, cur_hash, &dlen) == 1 && dlen == 32);
		memcpy(fseed, "server finished", 15); memcpy(fseed + 15, cur_hash, 32);
		assert(validity_prf(vsim.master_secret, 48, fseed, sizeof(fseed), sfin + 4, 12));
		sfin[0] = 0x14; val_put_be24(sfin + 1, 12);
		{
			uint8_t rsp[256];
			size_t elen = 0;
			static const uint8_t ccs[6] = { 0x14, 0x03, 0x03, 0x00, 0x01, 0x01 };
			memcpy(rsp, ccs, 6);
			vsim_tls_encrypt_send(0x16, sfin, 16, rsp + 6, &elen);
			vsim_set_ep81(rsp, 6 + elen);
		}
		vsim.tls_established = true;
		EVP_MD_CTX_free(copy);
	}
}

static int sim_poll(struct pollfd *fds, unsigned int nfds, int timeout)
{
	(void)timeout;
	assert(nfds == 1);
	if (vsim.cancel_on_poll) {
		validity_request_cancel(vsim.dev);
		vsim.cancel_on_poll = false;
		fds[0].revents = 0;
		return 0;
	}
	if (vsim.pending_urb && vsim.int_n > 0) {
		fds[0].revents = POLLOUT;
		return 1;
	}
	fds[0].revents = 0;
	return 0;
}

static int sim_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;

	(void)fd;
	if (request == (unsigned long)(int)VALIDITY_USB_SETCONFIGURATION ||
	    request == (unsigned long)(int)VALIDITY_USB_CLAIMINTERFACE ||
	    request == (unsigned long)(int)VALIDITY_USB_RELEASEINTERFACE ||
	    request == (unsigned long)(int)VALIDITY_USB_RESET)
		return 0;

	if (request == (unsigned long)(int)VALIDITY_USB_SUBMITURB) {
		va_start(ap, request);
		vsim.pending_urb = va_arg(ap, struct validity_usb_urb *);
		va_end(ap);
		assert(vsim.pending_urb->type == VALIDITY_URB_TYPE_INTERRUPT);
		assert(vsim.pending_urb->endpoint == VALIDITY_EP_INT);
		return 0;
	}
	if (request == (unsigned long)(int)VALIDITY_USB_DISCARDURB) {
		vsim.discard_count++;
		return 0;
	}
	if (request == (unsigned long)(int)VALIDITY_USB_REAPURBNDELAY) {
		struct validity_usb_urb **out;
		va_start(ap, request);
		out = va_arg(ap, struct validity_usb_urb **);
		va_end(ap);
		assert(vsim.pending_urb != NULL);
		if (vsim.int_n > 0) {
			memcpy(vsim.pending_urb->buffer, vsim.int_q[0], vsim.int_len[0]);
			vsim.pending_urb->actual_length = (int)vsim.int_len[0];
			vsim.pending_urb->status = 0;
			memmove(vsim.int_q[0], vsim.int_q[1], sizeof(vsim.int_q[0]) * (size_t)(vsim.int_n - 1));
			memmove(&vsim.int_len[0], &vsim.int_len[1], sizeof(vsim.int_len[0]) * (size_t)(vsim.int_n - 1));
			vsim.int_n--;
		} else {
			vsim.pending_urb->actual_length = 0;
			vsim.pending_urb->status = -ENOENT;
		}
		*out = vsim.pending_urb;
		vsim.pending_urb = NULL;
		return 0;
	}

	assert(request == (unsigned long)(int)VALIDITY_USB_BULK);
	{
		struct validity_usb_bulk *bulk;
		va_start(ap, request);
		bulk = va_arg(ap, struct validity_usb_bulk *);
		va_end(ap);
		assert(bulk->len <= VALIDITY_USB_CHUNK);

		if (bulk->ep == VALIDITY_EP_OUT) {
			const uint8_t *tx = bulk->data;
			if (bulk->len >= 4 && tx[0] == 0x44 && tx[1] == 0 && tx[2] == 0 && tx[3] == 0) {
				vsim_handle_tls_handshake(tx, bulk->len);
			} else if (vsim.tls_established && bulk->len >= 5 && tx[0] == 0x17 && tx[1] == 3 && tx[2] == 3) {
				uint8_t *plain = malloc(VALIDITY_IO_BUF_MAX);
				uint8_t *rsp = malloc(VALIDITY_IO_BUF_MAX);
				uint8_t *enc = malloc(VALIDITY_IO_BUF_MAX);
				size_t plen, rlen = 0, elen = 0;

				assert(plain && rsp && enc);
				plen = vsim_tls_decrypt(0x17, tx + 5, val_be16(tx + 3), plain);
				vsim_handle_app_cmd(plain, plen, rsp, &rlen);
				vsim_tls_encrypt_send(0x17, rsp, rlen, enc, &elen);
				vsim_set_ep81(enc, elen);
				free(plain); free(rsp); free(enc);
			} else {
				uint8_t *rsp = malloc(VALIDITY_IO_BUF_MAX);
				size_t rlen = 0;

				assert(rsp);
				vsim_handle_app_cmd(tx, bulk->len, rsp, &rlen);
				vsim_set_ep81(rsp, rlen);
				free(rsp);
			}
			return (int)bulk->len;
		}
		if (bulk->ep == VALIDITY_EP_IN) {
			size_t avail = vsim.ep81_len - vsim.ep81_pos;
			size_t take = avail < bulk->len ? avail : bulk->len;
			if (take) {
				memcpy(bulk->data, vsim.ep81_buf + vsim.ep81_pos, take);
				vsim.ep81_pos += take;
			}
			return (int)take;
		}
		if (bulk->ep == VALIDITY_EP_CAPTURE) {
			size_t avail = vsim.ep82_len - vsim.ep82_pos;
			size_t take = avail < bulk->len ? avail : bulk->len;
			if (take) {
				memcpy(bulk->data, vsim.ep82_buf + vsim.ep82_pos, take);
				vsim.ep82_pos += take;
			}
			return (int)take;
		}
		abort();
	}
}

static void test_simulated_sensor_flow(void)
{
	char tmpdir[] = "/tmp/valsim.XXXXXX";
	char devpath[31], dmipath[64];
	uint8_t desc[18] = { 18, 1 };
	struct validity s;
	struct validity_template listed[VALIDITY_MAX_TEMPLATES];
	size_t count = 0;
	int fail = 0;
	char matched[FP_NAME_MAX + 1];

	assert(mkdtemp(tmpdir) != NULL);
	snprintf(devpath, sizeof(devpath), "%s/usbdev", tmpdir);
	snprintf(dmipath, sizeof(dmipath), "%s/dmi", tmpdir);

	/* Write fake USB descriptor for ThinkPad T470 Validity 138a:0097 */
	desc[8] = 0x8a; desc[9] = 0x13;
	desc[10] = 0x97; desc[11] = 0x00;
	{
		int fd = open(devpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		assert(fd >= 0 && write(fd, desc, sizeof(desc)) == (ssize_t)sizeof(desc));
		close(fd);
	}
	{
		int fd = open(dmipath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		const char *dmi = "20HD000MUS\nPF0ABCDE\n";
		assert(fd >= 0 && write(fd, dmi, strlen(dmi)) == (ssize_t)strlen(dmi));
		close(fd);
	}

	/* Probe checks */
	{
		uint16_t vid = 0, pid = 0;
		assert(validity_probe_node(devpath, &vid, &pid) && vid == 0x138a && pid == 0x0097);
		assert(validity_is_supported_usbid(0x138a, 0x0090));
		assert(validity_is_supported_usbid(0x138a, 0x0097));
		assert(validity_is_supported_usbid(0x138a, 0x009d));
		assert(validity_is_supported_usbid(0x06cb, 0x009a));
		assert(!validity_is_supported_usbid(0x138a, 0x0099));
		assert(!validity_is_supported_usbid(0x27c6, 0x6384));
	}

	memset(&vsim, 0, sizeof(vsim));
	vsim.next_dbid = 10;
	vsim.ep82_buf = malloc(128 * 1024);
	assert(vsim.ep82_buf);
	memset(vsim.part6, 0xff, sizeof(vsim.part6));

	/* Pre-populate a Windows Hello user (finger 99) and a stale gbOS live template (finger 88) */
	vsim.users[0].used = true;
	vsim.users[0].dbid = 8;
	vsim.users[0].finger_dbid = 9;
	vsim.users[0].finger_id = 99;
	val_put_le32(vsim.users[0].identity + 0, 3);
	val_put_le32(vsim.users[0].identity + 4, 28);
	vsim.users[0].identity[8] = 1; vsim.users[0].identity[9] = 5; vsim.users[0].identity[15] = 5;
	val_put_le32(vsim.users[0].identity + 16, 21);
	val_put_le32(vsim.users[0].identity + 20, 394619333U);
	val_put_le32(vsim.users[0].identity + 32, 1001U);

	vsim.users[1].used = true;
	vsim.users[1].dbid = 6;
	vsim.users[1].finger_dbid = 7;
	vsim.users[1].finger_id = 88;
	assert(validity_name_to_identity("GBOS-L-deadbeef-u0-00000001", vsim.users[1].identity));

	validity_init(&s);
	vsim.dev = &s;
	snprintf(s.path, sizeof(s.path), "%s", devpath);
	snprintf(s.data_dir, sizeof(s.data_dir), "%s", tmpdir);
	snprintf(s.dmi_file, sizeof(s.dmi_file), "%s", dmipath);
	validity_load_dmi(&s);
	vsim_build_tls_flash(s.psk_enc, s.psk_val);

	/* Open sensor manually through the exact validity_open sequence, injecting sim server's ecdh_q
	 * (since only Synaptics holds the private key matching fwpub). */
	s.fd = open(devpath, O_RDWR | O_CLOEXEC);
	assert(s.fd >= 0);
	s.vid = 0x138a; s.pid = 0x0097;
	assert(validity_send_init(&s) == 0);
	{
		uint8_t fl[0x1000];
		size_t flen = 0;
		assert(validity_read_flash(&s, 1, 0, sizeof(fl), fl, &flen) == 0 && flen == sizeof(fl));
		/* Parse blocks 3 & 4 via validity_tls_parse_flash (fails only because block 6 isn't signed by Synaptics) */
		assert(validity_tls_parse_flash(&s, fl, flen) == -EPROTO);
		assert(s.tls_cert_len == sizeof(vsim.cli_cert) && s.priv_key != NULL);
		s.ecdh_q = EC_POINT_dup(EC_KEY_get0_public_key(vsim.srv_ecdh), EC_KEY_get0_group(vsim.srv_ecdh));
		assert(s.ecdh_q != NULL);
	}
	assert(validity_tls_open(&s) == 0);
	assert(s.secure_tx && s.secure_rx && vsim.tls_established);
	{
		bool did_reboot = false;
		assert(validity_upload_fwext_if_needed(&s, &did_reboot) == 0 && !did_reboot);
	}
	/* First sensor_open runs 3 calibration iterations + 1 blank clean-slate = 4 captures */
	assert(validity_sensor_open(&s) == 0);
	assert(vsim.calib_captures == 4 && s.calib_data_len == 13440);
	assert(validity_init_db(&s) == 0);
	s.ready = true;

	/* Second sensor_open hits cached validity-calib.bin + flash clean-slate hash: 0 new captures! */
	assert(validity_sensor_open(&s) == 0);
	assert(vsim.calib_captures == 4);

	/* List templates & apply gbos_fp_policy: stale live template deleted, Windows Hello kept */
	assert(validity_list_templates(&s, listed, VALIDITY_MAX_TEMPLATES, &count) == VALIDITY_OK);
	assert(count == 2);
	{
		struct fp_policy pol = { "1234abcd", false, false, 0, NULL, 0, true, true };
		for (size_t i = 0; i < count; i++) {
			enum fp_verdict v = fp_classify(&pol, listed[i].uid);
			if (listed[i].user_dbid == 8)
				assert(v == FP_KEEP_NOT_GBOS);
			else if (listed[i].user_dbid == 6) {
				assert(v == FP_DELETE_OLD_LIVE);
				assert(validity_delete(&s, listed[i].uid) == VALIDITY_OK);
			}
		}
	}
	assert(validity_list_templates(&s, listed, VALIDITY_MAX_TEMPLATES, &count) == VALIDITY_OK && count == 1);
	assert(listed[0].user_dbid == 8 && listed[0].uid[0] == 0); /* Windows Hello intact */

	/* Enroll finger 7 as GBOS-1234abcd-u0-00000042 */
	vsim.finger_present = 7;
	vsim.enroll_scans = 0;
	vsim.inject_51_err = 1;
	assert(validity_enroll(&s, "GBOS-1234abcd-u0-00000042", NULL, &fail) == VALIDITY_OK);
	assert(vsim.enroll_scans == 3 && vsim.inject_51_err == 0);

	/* Identify: matching finger 7 succeeds */
	{
		const char *uids[1] = { "GBOS-1234abcd-u0-00000042" };
		matched[0] = 0;
		assert(validity_identify(&s, uids, 1, NULL, matched, &fail) == VALIDITY_OK);
		assert(strcmp(matched, "GBOS-1234abcd-u0-00000042") == 0);

		/* Unknown finger 55 -> NOMATCH */
		vsim.finger_present = 55;
		assert(validity_identify(&s, uids, 1, NULL, matched, &fail) == VALIDITY_NOMATCH);

		/* Windows Hello finger 99 -> matched on chip, rejected as NOMATCH for Android user! */
		vsim.finger_present = 99;
		assert(validity_identify(&s, uids, 1, NULL, matched, &fail) == VALIDITY_NOMATCH);

		/* Cancel mid-capture during wait_int -> discards URB and returns -ECANCELED */
		vsim.cancel_on_poll = true;
		vsim.discard_count = 0;
		assert(validity_identify(&s, uids, 1, NULL, matched, &fail) == -ECANCELED);
		assert(vsim.discard_count == 1);
		validity_clear_cancel(&s);
	}

	/* Delete enrolled template and verify Windows Hello still untouched */
	assert(validity_delete(&s, "GBOS-1234abcd-u0-00000042") == VALIDITY_OK);
	assert(validity_delete(&s, "GBOS-1234abcd-u0-00000042") == VALIDITY_NORECORD);
	assert(validity_list_templates(&s, listed, VALIDITY_MAX_TEMPLATES, &count) == VALIDITY_OK && count == 1);
	assert(listed[0].user_dbid == 8);

	validity_close(&s, true);
	EC_KEY_free(vsim.srv_ecdh);
	EC_KEY_free(vsim.cli_key);
	EVP_MD_CTX_free(vsim.hs_hash);
	free(vsim.ep82_buf);

	/* Cleanup temp files */
	{
		char cpath[160];
		snprintf(cpath, sizeof(cpath), "%s/validity-calib.bin", tmpdir);
		unlink(cpath);
		unlink(devpath);
		unlink(dmipath);
		rmdir(tmpdir);
	}
}

int main(void)
{
	test_python_validity_vectors();
	test_sid_mapping();
	test_simulated_sensor_flow();
	puts("VALIDITY_PROTOCOL_TEST_PASS");
	return 0;
}
