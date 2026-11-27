/* Host tests for the portable layers of gm168sec_hal.h (no sensor needed).
 * Build: make -C host_test (host gcc, links system libssl/libcrypto).
 * The TLS test drives the HAL's own gm_tls server code against a real
 * OpenSSL PSK client over memory BIOs - the same handshake the sensor
 * performs as TLS client, with the same all-zero PSK.
 */
#define GM168SEC_HOST_TEST 1
#define GM168SEC_DATA_DIR "/tmp/gm168sec-test-data"
#include <stdio.h>
#include <assert.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "gm168sec_hal.h"

/* Matcher stubs: the host harness tests the transport/proto/TLS layers,
 * not Milan. The Android build links milan_matcher.c instead. */
int gm_matcher_check_setup(struct goodix *s, const uint16_t *setup)
{ (void)s; (void)setup; return 0; }
int gm_matcher_enroll_begin(struct goodix *s, const char *const *uids,
			    const uint8_t *const *tpls, const size_t *tpl_lens, int n)
{ (void)s; (void)uids; (void)tpls; (void)tpl_lens; (void)n; return GM168SEC_ERR_NO_MATCHER; }
int gm_matcher_enroll_sample(struct goodix *s, const uint16_t *setup,
			     const uint16_t *live, int *quality, int *coverage,
			     char *dup_uid)
{ (void)s; (void)setup; (void)live; (void)quality; (void)coverage; (void)dup_uid; return GM168SEC_ERR_NO_MATCHER; }
int gm_matcher_enroll_finish(struct goodix *s, uint8_t **tpl, size_t *tpl_len)
{ (void)s; (void)tpl; (void)tpl_len; return GM168SEC_ERR_NO_MATCHER; }
int gm_matcher_match(struct goodix *s, const uint16_t *setup,
		     const uint16_t *live, const char *const *uids,
		     const uint8_t *const *tpls, const size_t *tpl_lens, int n,
		     int *winner)
{ (void)s; (void)setup; (void)live; (void)uids; (void)tpls; (void)tpl_lens; (void)n; (void)winner; return GM168SEC_ERR_NO_MATCHER; }

static int fails = 0;
#define CHECK(cond, ...) do { \
		if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } \
	} while (0)

static void test_pack(void)
{
	uint8_t msg[] = { 0x20, 0x05, 0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0x11 };
	size_t flen = 0;
	uint8_t *framed = gm_frame_message(msg, sizeof(msg), &flen);
	CHECK(framed != NULL, "frame_message returned NULL");
	CHECK(flen == 64, "message pack must pad to 64 bytes, got %zu", flen);
	CHECK(framed[0] == 0xa0, "message pack flags 0xa0, got 0x%02x", framed[0]);
	CHECK(framed[1] == sizeof(msg) && framed[2] == 0, "pack length field");
	CHECK(framed[3] == (uint8_t)(framed[0] + framed[1] + framed[2]),
	      "pack header checksum");
	CHECK(memcmp(framed + 4, msg, sizeof(msg)) == 0, "pack payload");
	size_t plen = 0;
	CHECK(gm_pack_read_header(framed, &plen) && plen == sizeof(msg), "pack header parse");
	framed[3] ^= 0xFF;
	CHECK(!gm_pack_read_header(framed, &plen), "bad pack header checksum must reject");
	framed[3] ^= 0xFF;
	framed[0] = 0x50;
	CHECK(!gm_pack_read_header(framed, &plen), "unknown pack family must reject");
	free(framed);
	/* TLS pack. */
	uint8_t rec[] = { 0x16, 0x03, 0x03, 0x00, 0x05, 1, 2, 3, 4, 5 };
	uint8_t *tframed = gm_frame_pack(GM_PACK_TLS, rec, sizeof(rec), &flen);
	CHECK(tframed != NULL && tframed[0] == 0xb0, "TLS pack flags 0xb0");
	free(tframed);
	printf("pack: ok\n");
}

static void test_proto(void)
{
	size_t len = 0;
	uint8_t payload[] = { 0x01, 0x02, 0x03 };
	uint8_t *msg = gm_proto_build_message(0x02, 0x00, payload, sizeof(payload), true, &len);
	CHECK(msg != NULL, "proto build NULL");
	CHECK(len == 1 + 2 + 3 + 1, "proto total length %zu", len);
	CHECK(msg[0] == 0x20, "cmd byte (0x02<<4)|(0x00<<1) = 0x20, got 0x%02x", msg[0]);
	CHECK(msg[1] == 4 && msg[2] == 0, "size field = payload+1");
	CHECK(gm_proto_validate_checksum(msg, len), "proto checksum must validate");
	msg[len - 1] ^= 0x01;
	CHECK(!gm_proto_validate_checksum(msg, len), "corrupt checksum must fail");
	msg[len - 1] ^= 0x01;
	/* Handshake marker 0x88 always validates. */
	uint8_t *hs = gm_proto_build_message(0x00, 0x00, NULL, 0, false, &len);
	CHECK(hs != NULL && hs[len - 1] == 0x88, "handshake message ends 0x88");
	CHECK(gm_proto_validate_checksum(hs, len), "0x88 marker validates");
	free(msg);
	free(hs);
	/* Reassembly: feed a 200-byte message through cells. */
	uint8_t big[200];
	for (int i = 0; i < 200; i++)
		big[i] = (uint8_t)i;
	size_t mlen = 0;
	uint8_t *m = gm_proto_build_message(0x09, 0x00, big, sizeof(big), true, &mlen);
	struct gm_rx rx;
	gm_rx_reset(&rx);
	rx.buf = NULL;
	size_t off = mlen < 64 ? mlen : 64;
	CHECK(gm_rx_feed_cell(&rx, m, off), "first cell feed");
	while (off < mlen) {
		size_t n = mlen - off < 63 ? mlen - off : 63;
		uint8_t cell[64];
		cell[0] = (uint8_t)(m[0] | 1);
		memcpy(cell + 1, m + off, n);
		CHECK(gm_rx_feed_cell(&rx, cell, n + 1), "continuation cell feed");
		off += n;
	}
	CHECK(gm_rx_complete(&rx), "reassembly must complete");
	uint8_t cat, cmd;
	const uint8_t *pl;
	size_t pl_len;
	CHECK(gm_rx_parse(&rx, &cat, &cmd, &pl, &pl_len), "reassembly parse");
	CHECK(cat == 0x09 && cmd == 0x00, "parsed cat/cmd 0x09/0x00, got 0x%02x/0x%02x", cat, cmd);
	CHECK(pl_len == 200 && memcmp(pl, big, 200) == 0, "reassembled payload");
	free(rx.buf);
	free(m);
	printf("proto: ok\n");
}

static void test_crc_sha_raw12(void)
{
	/* CRC-32/MPEG-2 published check values. */
	CHECK(gm_crc32_mpeg2((const uint8_t *)"", 0) == 0xFFFFFFFFu,
	      "crc32-mpeg2 empty = 0xFFFFFFFF, got 0x%08x", gm_crc32_mpeg2((const uint8_t *)"", 0));
	CHECK(gm_crc32_mpeg2((const uint8_t *)"123456789", 9) == 0x0376E6E7u,
	      "crc32-mpeg2 \"123456789\" = 0x0376E6E7, got 0x%08x",
	      gm_crc32_mpeg2((const uint8_t *)"123456789", 9));
	/* SHA-256("abc"). */
	uint8_t digest[32];
	gm_sha256((const uint8_t *)"abc", 3, digest);
	static const uint8_t want[32] = {
		0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
		0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
	CHECK(memcmp(digest, want, 32) == 0, "sha256(\"abc\")");
	/* raw12: 6 bytes -> 4 pixels, driver byte order. Decode a full frame
	 * into a full 9504-pixel buffer (the function's contract). */
	uint8_t raw[GM168SEC_RAW12_BYTES] = { 0 };
	raw[0] = 0x12; raw[1] = 0x34; raw[2] = 0x56;
	raw[3] = 0x78; raw[4] = 0x9A; raw[5] = 0xBC;
	uint16_t *px = (uint16_t *)calloc(GM168SEC_SENSOR_PIXELS, sizeof(uint16_t));
	CHECK(px != NULL, "px alloc");
	gm_decode_raw12(raw, px);
	/* p0 = ((0x12&0xF)<<8)|0x34 = 0x234; p1 = (0x78<<4)|(0x12>>4) = 0x781;
	 * p2 = ((0xBC&0xF)<<8)|0x56 = 0xC56; p3 = (0x9A<<4)|(0xBC>>4) = 0x9AB */
	CHECK(px[0] == 0x234 && px[1] == 0x781 && px[2] == 0xC56 && px[3] == 0x9AB,
	      "raw12 decode got %03x %03x %03x %03x", px[0], px[1], px[2], px[3]);
	free(px);
	/* Wrapped image validation: build a frame, stamp the CRC, check. */
	uint8_t *frame = (uint8_t *)calloc(1, GM168SEC_WRAPPED_IMAGE_LEN);
	for (int i = 0; i < GM168SEC_RAW12_BYTES; i++)
		frame[i] = (uint8_t)(i * 7);
	uint32_t crc = gm_crc32_mpeg2(frame, GM168SEC_RAW12_BYTES);
	/* gm_decode_u32 order: d[0]*0x100 + d[1] + d[2]*0x1000000 + d[3]*0x10000 */
	frame[GM168SEC_RAW12_BYTES + 0] = (uint8_t)((crc >> 8) & 0xFF);
	frame[GM168SEC_RAW12_BYTES + 1] = (uint8_t)(crc & 0xFF);
	frame[GM168SEC_RAW12_BYTES + 2] = (uint8_t)((crc >> 24) & 0xFF);
	frame[GM168SEC_RAW12_BYTES + 3] = (uint8_t)((crc >> 16) & 0xFF);
	CHECK(gm_wrapped_image_valid(frame, GM168SEC_WRAPPED_IMAGE_LEN), "valid wrapped image");
	frame[100] ^= 0x01;
	CHECK(!gm_wrapped_image_valid(frame, GM168SEC_WRAPPED_IMAGE_LEN), "corrupt image must fail CRC");
	free(frame);
	printf("crc/sha/raw12: ok\n");
}

static void test_config(void)
{
	uint8_t cfg[256];
	struct gm168sec_calib cal;
	uint8_t otp[32] = { 0 };
	otp[23] = 0x40;	/* tcode = 0x41 */
	otp[17] = 0x60; otp[22] = 0x11; otp[31] = 0x22;
	gm_parse_otp(otp, sizeof(otp), &cal);
	CHECK(cal.tcode == 0x41, "otp tcode 0x41, got 0x%x", cal.tcode);
	CHECK(cal.dac_from_otp, "otp supplies dac");
	memcpy(cfg, gm168sec_default_config, sizeof(cfg));
	uint8_t before[256];
	memcpy(before, cfg, sizeof(before));
	gm_patch_config(cfg, sizeof(cfg), &cal);
	CHECK(memcmp(cfg, before, sizeof(cfg)) != 0, "patch must change the config");
	/* Checksum: recompute independently. */
	uint32_t sum = 0xA5A5;
	for (int i = 0; i < 254; i += 2)
		sum = (sum + (uint16_t)(cfg[i] | (cfg[i+1] << 8))) & 0xFFFF;
	uint32_t expect = 0x10000 - sum;
	CHECK(cfg[254] == (expect & 0xFF) && cfg[255] == ((expect >> 8) & 0xFF),
	      "config checksum");
	printf("config: ok (tcode 0x%x dac_h 0x%x dac_l 0x%x delta_down 0x%x)\n",
	       cal.tcode, cal.dac_h, cal.dac_l, cal.delta_down);
}

static unsigned int test_psk_client_cb(SSL *ssl, const char *hint, char *identity,
				       unsigned int max_identity_len,
				       unsigned char *psk, unsigned int max_psk_len)
{
	(void)ssl; (void)hint;
	const char *id = GM_TLS_IDENTITY;
	if (strlen(id) + 1 > max_identity_len || 32 > max_psk_len)
		return 0;
	strcpy(identity, id);
	memset(psk, 0, 32);
	return 32;
}

static void test_tls_loopback(void)
{
	/* Server: the HAL's own gm_tls code (all-zero PSK). Client: plain
	 * OpenSSL, PSK client callback returning the same all-zero PSK. */
	uint8_t zero_psk[32] = { 0 };
	struct gm_tls *server = gm_tls_new(zero_psk, 32);
	CHECK(server != NULL, "gm_tls_new");
	SSL_CTX *cctx = SSL_CTX_new(TLS_client_method());
	CHECK(cctx != NULL, "client ctx");
	SSL_CTX_set_min_proto_version(cctx, TLS1_2_VERSION);
	SSL_CTX_set_max_proto_version(cctx, TLS1_2_VERSION);
	SSL_CTX_set_cipher_list(cctx, GM_TLS_CIPHER);
	SSL_CTX_set_psk_client_callback(cctx, test_psk_client_cb);
	SSL *client = SSL_new(cctx);
	BIO *cin = BIO_new(BIO_s_mem()), *cout = BIO_new(BIO_s_mem());
	BIO_set_mem_eof_return(cin, -1);
	SSL_set_bio(client, cin, cout);
	SSL_set_connect_state(client);
	/* Pump records both ways until both sides finish. */
	int server_done = 0, client_done = 0;
	for (int iter = 0; iter < 50 && !(server_done && client_done); iter++) {
		/* client -> server */
		char *cdata = NULL;
		long clen = BIO_get_mem_data(cout, &cdata);
		if (clen > 0) {
			uint8_t *out = NULL;
			size_t out_len = 0;
			int r = gm_tls_handshake_step(server, (uint8_t *)cdata, (size_t)clen, &out, &out_len);
			CHECK(r >= 0, "server handshake step failed (iter %d)", iter);
			if (r == 1)
				server_done = 1;
			(void)BIO_reset(cout);
			/* server -> client */
			if (out_len > 0) {
				int w = BIO_write(cin, out, (int)out_len);
				CHECK(w == (int)out_len, "client BIO_write");
			}
			free(out);
		}
		int cr = SSL_do_handshake(client);
		if (cr == 1)
			client_done = 1;
		else
			CHECK(SSL_get_error(client, cr) == SSL_ERROR_WANT_READ,
			      "client handshake error %d", SSL_get_error(client, cr));
	}
	CHECK(server_done && client_done, "TLS-PSK handshake must complete (server %d client %d)",
	      server_done, client_done);
	CHECK(gm_tls_established(server), "server reports established");
	/* Application data: client encrypts a fake image, server decrypts. */
	uint8_t fake[GM168SEC_WRAPPED_IMAGE_LEN];
	for (size_t i = 0; i < sizeof(fake); i++)
		fake[i] = (uint8_t)i;
	/* Stamp a valid CRC so the image check passes after decrypt. */
	uint32_t crc = gm_crc32_mpeg2(fake, GM168SEC_RAW12_BYTES);
	fake[GM168SEC_RAW12_BYTES + 0] = (uint8_t)((crc >> 8) & 0xFF);
	fake[GM168SEC_RAW12_BYTES + 1] = (uint8_t)(crc & 0xFF);
	fake[GM168SEC_RAW12_BYTES + 2] = (uint8_t)((crc >> 24) & 0xFF);
	fake[GM168SEC_RAW12_BYTES + 3] = (uint8_t)((crc >> 16) & 0xFF);
	int w = SSL_write(client, fake, sizeof(fake));
	CHECK(w == (int)sizeof(fake), "client SSL_write %d", w);
	char *cdata = NULL;
	long clen = BIO_get_mem_data(cout, &cdata);
	CHECK(clen > 0, "client produced records");
	size_t plain_len = 0;
	uint8_t *plain = gm_tls_decrypt(server, (uint8_t *)cdata, (size_t)clen, &plain_len);
	CHECK(plain != NULL && plain_len == sizeof(fake), "server decrypts %zu bytes", plain_len);
	if (plain)
		CHECK(memcmp(plain, fake, sizeof(fake)) == 0, "decrypted image matches");
	CHECK(plain && gm_wrapped_image_valid(plain, plain_len), "decrypted image passes CRC check");
	free(plain);
	SSL_free(client);
	SSL_CTX_free(cctx);
	gm_tls_free(server);
	printf("tls loopback: ok\n");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	test_pack();
	test_proto();
	test_crc_sha_raw12();
	test_config();
	test_tls_loopback();
	if (fails) {
		printf("%d FAILURES\n", fails);
		return 1;
	}
	printf("ALL HOST TESTS PASSED\n");
	return 0;
}
