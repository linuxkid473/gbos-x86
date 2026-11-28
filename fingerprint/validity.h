/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Blocking usbfs + custom TLS 1.2 backend for Validity/Synaptics 9x MOC sensors
 * (ThinkPad T470 138a:0097, plus 138a:0090, 138a:009d, 06cb:009a).
 * Protocol, TLS handshake, calibration, timeslot patching, and on-chip DB
 * commands are ported from python-validity (uunicorn/python-validity).
 *
 * Requires standard libc/bionic and BoringSSL/OpenSSL declarations before
 * inclusion, plus gbos_fp_policy.h (for fp_name_format / fp_name_parse).
 */
#ifndef GBOS_VALIDITY_H
#define GBOS_VALIDITY_H

#include "validity_tables.h"

#ifndef VALIDITY_ERRNO
#define VALIDITY_ERRNO errno
#endif
#ifndef VALIDITY_LOGI
#define VALIDITY_LOGI(...) ((void)0)
#define VALIDITY_LOGW(...) ((void)0)
#define VALIDITY_LOGE(...) ((void)0)
#endif
#ifndef VALIDITY_RANDOM_BUF
#define VALIDITY_RANDOM_BUF(buf, len) arc4random_buf((buf), (len))
#endif

/* Linux x86_64 usbdevfs ioctl constants (verified against linux/usbdevice_fs.h) */
#define VALIDITY_USB_BULK             0xc0185502U
#define VALIDITY_USB_SETCONFIGURATION 0x80045505U
#define VALIDITY_USB_SUBMITURB        0x8038550aU
#define VALIDITY_USB_DISCARDURB       0x0000550bU
#define VALIDITY_USB_REAPURBNDELAY    0x4008550dU
#define VALIDITY_USB_CLAIMINTERFACE   0x8004550fU
#define VALIDITY_USB_RELEASEINTERFACE 0x80045510U
#define VALIDITY_USB_RESET            0x00005514U
#define VALIDITY_URB_TYPE_INTERRUPT   1

#define VALIDITY_EP_OUT     0x01U
#define VALIDITY_EP_IN      0x81U
#define VALIDITY_EP_CAPTURE 0x82U
#define VALIDITY_EP_INT     0x83U
#define VALIDITY_USB_CHUNK  16384U

#define VALIDITY_NID_P256       415	/* NID_X9_62_prime256v1 in BoringSSL and OpenSSL */
#define VALIDITY_GBOS_SID_MAGIC 0x47424f53U	/* "GBOS" */
#define VALIDITY_ENROLL_STAGES  7
#define VALIDITY_MAX_TEMPLATES  32
#define VALIDITY_DMI_FILE       "/dev/gbos-fp-dmi"
#define VALIDITY_IO_BUF_MAX     (20 * 1024)
#define VALIDITY_CALIB_MAX      (144 * 152)	/* max 21888 bytes (0xdb: 144*152, 0x199: 112*120=13440) */
#define VALIDITY_TLS_CERT_MAX   1024
#define VALIDITY_PRIV_BLOB_MAX  256
#define VALIDITY_ECDH_BLOB_MAX  512
#define VALIDITY_FACTORY_MAX    512

enum {
	VALIDITY_OK = 0,
	VALIDITY_NOMATCH,
	VALIDITY_RETRY,
	VALIDITY_NORECORD,
	VALIDITY_FAILED,
	VALIDITY_DUPLICATE,
};

enum {
	VALIDITY_EV_CAPTURE,
	VALIDITY_EV_RETRY,
	VALIDITY_EV_PROGRESS,
};

enum {
	VALIDITY_MODE_CALIBRATE = 1,
	VALIDITY_MODE_IDENTIFY = 2,
	VALIDITY_MODE_ENROLL = 3,
};

struct validity_ev {
	void (*fn)(void *ctx, int kind, int value);
	void *ctx;
};

struct validity_usb_bulk {
	unsigned int ep, len, timeout;
	void *data;
};

/* Matches Linux x86_64 struct usbdevfs_urb (56 bytes) */
struct validity_usb_urb {
	unsigned char type;
	unsigned char endpoint;
	int status;
	unsigned int flags;
	void *buffer;
	int buffer_length;
	int actual_length;
	int start_frame;
	int number_of_packets;
	int error_count;
	unsigned int signr;
	void *usercontext;
};

struct validity_rom_info {
	uint32_t timestamp;
	uint32_t build;
	uint8_t major;
	uint8_t minor;
	uint8_t product;
	uint8_t u1;
};

struct validity_template {
	uint16_t user_dbid;
	char uid[FP_NAME_MAX + 1];
	size_t uid_len;
};

struct validity_user_info {
	uint16_t dbid;
	uint8_t identity[0x4c];
	size_t identity_len;
	uint16_t finger_dbids[16];
	uint16_t finger_subtypes[16];
	size_t finger_count;
};

struct validity {
	int fd;
	char path[32];
	char data_dir[128];
	char dmi_file[128];
	uint16_t vid;
	uint16_t pid;
	bool ready;
	bool cancel;

	/* DMI-derived pre-TLS keys */
	char dmi_product[128];
	char dmi_serial[128];
	uint8_t psk_enc[32];
	uint8_t psk_val[32];

	/* Parsed TLS flash state */
	uint8_t tls_cert[VALIDITY_TLS_CERT_MAX];
	size_t tls_cert_len;
	uint8_t priv_blob[VALIDITY_PRIV_BLOB_MAX];
	size_t priv_blob_len;
	uint8_t ecdh_blob[VALIDITY_ECDH_BLOB_MAX];
	size_t ecdh_blob_len;
	EC_KEY *priv_key;
	EC_POINT *ecdh_q;

	/* Active TLS 1.2 session state */
	bool secure_tx;
	bool secure_rx;
	uint8_t client_random[32];
	uint8_t server_random[32];
	uint8_t master_secret[48];
	uint8_t sign_key[32];
	uint8_t val_key[32];
	uint8_t enc_key[32];
	uint8_t dec_key[32];

	/* Sensor hardware + calibration state */
	struct validity_rom_info rom;
	const struct validity_sensor_type_info *type_info;
	uint16_t lines_per_frame;
	uint8_t factory_calib_values[VALIDITY_FACTORY_MAX];
	size_t factory_calib_values_len;
	uint8_t factory_calib_data[VALIDITY_FACTORY_MAX];
	size_t factory_calib_data_len;
	uint8_t calib_data[VALIDITY_CALIB_MAX];
	size_t calib_data_len;
	char fw_version[32];
};

/* ---- Endianness helpers ---- */

static inline uint16_t val_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static inline uint32_t val_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint16_t val_be16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline uint32_t val_be24(const uint8_t *p)
{
	return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static inline void val_put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void val_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline void val_put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline void val_put_be24(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 16);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)v;
}

static inline void val_put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline void val_reverse_copy(uint8_t *dst, const uint8_t *src, size_t n)
{
	for (size_t i = 0; i < n; i++)
		dst[i] = src[n - 1 - i];
}

static inline bool validity_is_supported_usbid(uint16_t vid, uint16_t pid)
{
	return (vid == 0x138a && (pid == 0x0090 || pid == 0x0097 || pid == 0x009d)) ||
	       (vid == 0x06cb && pid == 0x009a);
}

static inline bool validity_is_0090(const struct validity *s)
{
	return s->vid == 0x138a && s->pid == 0x0090;
}

/* ---- Lifecycle & cancel ---- */

static inline void validity_init(struct validity *s)
{
	memset(s, 0, sizeof(*s));
	s->fd = -1;
}

static inline void validity_request_cancel(struct validity *s)
{
	__atomic_store_n(&s->cancel, true, __ATOMIC_RELEASE);
}

static inline void validity_clear_cancel(struct validity *s)
{
	__atomic_store_n(&s->cancel, false, __ATOMIC_RELEASE);
}

static inline bool validity_cancel_requested(const struct validity *s)
{
	return __atomic_load_n(&s->cancel, __ATOMIC_ACQUIRE);
}

static inline void validity_emit(const struct validity_ev *ev, int kind, int value)
{
	if (ev && ev->fn)
		ev->fn(ev->ctx, kind, value);
}

static inline void validity_free_crypto(struct validity *s)
{
	if (s->priv_key) {
		EC_KEY_free(s->priv_key);
		s->priv_key = NULL;
	}
	if (s->ecdh_q) {
		EC_POINT_free(s->ecdh_q);
		s->ecdh_q = NULL;
	}
	s->secure_tx = false;
	s->secure_rx = false;
}

/* ---- Crypto helpers (BoringSSL + OpenSSL compatible) ---- */

static inline bool val_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
	unsigned int out_len = 0;

	return EVP_Digest(data, len, out, &out_len, EVP_sha256(), NULL) == 1 && out_len == 32;
}

static inline bool val_bn_to_be32(const BIGNUM *bn, uint8_t out[32])
{
	unsigned int nb = ((unsigned int)BN_num_bits(bn) + 7U) / 8U;

	if (nb > 32)
		return false;
	memset(out, 0, 32);
	if (nb > 0)
		BN_bn2bin(bn, out + (32 - nb));
	return true;
}

/* TLS 1.2 P_SHA256 PRF (matches tls.py:prf) */
static inline bool validity_prf(const uint8_t *secret, size_t secret_len,
				const uint8_t *seed, size_t seed_len,
				uint8_t *out, size_t out_len)
{
	uint8_t a[32], tmp[32 + 256], block[32];
	unsigned int hlen = 0;
	size_t off = 0;

	if (seed_len > 256)
		return false;
	if (!HMAC(EVP_sha256(), secret, secret_len, seed, seed_len, a, &hlen) || hlen != 32)
		return false;
	while (off < out_len) {
		size_t take = out_len - off;

		if (take > 32)
			take = 32;
		memcpy(tmp, a, 32);
		memcpy(tmp + 32, seed, seed_len);
		if (!HMAC(EVP_sha256(), secret, secret_len, tmp, 32 + seed_len, block, &hlen) || hlen != 32)
			return false;
		memcpy(out + off, block, take);
		off += take;
		if (off < out_len) {
			if (!HMAC(EVP_sha256(), secret, secret_len, a, 32, a, &hlen) || hlen != 32)
				return false;
		}
	}
	return true;
}

static inline bool val_aes256_cbc(bool encrypt, const uint8_t key[32], const uint8_t iv[16],
				  const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len)
{
	EVP_CIPHER_CTX *ctx;
	int len1 = 0, len2 = 0;
	bool ok = false;

	if (in_len == 0 || (in_len % 16) != 0)
		return false;
	ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return false;
	if (encrypt) {
		if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) == 1 &&
		    EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
		    EVP_EncryptUpdate(ctx, out, &len1, in, (int)in_len) == 1 &&
		    EVP_EncryptFinal_ex(ctx, out + len1, &len2) == 1)
			ok = true;
	} else {
		if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) == 1 &&
		    EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
		    EVP_DecryptUpdate(ctx, out, &len1, in, (int)in_len) == 1 &&
		    EVP_DecryptFinal_ex(ctx, out + len1, &len2) == 1)
			ok = true;
	}
	EVP_CIPHER_CTX_free(ctx);
	if (ok)
		*out_len = (size_t)(len1 + len2);
	return ok;
}

/* Derive EC_KEY from 32-byte big-endian private scalar on SECP256R1 */
static inline EC_KEY *val_ec_key_from_priv_be32(const uint8_t d_be[32])
{
	EC_KEY *k = EC_KEY_new_by_curve_name(VALIDITY_NID_P256);
	BIGNUM *d = NULL;
	EC_POINT *pub = NULL;
	const EC_GROUP *grp;

	if (!k)
		return NULL;
	grp = EC_KEY_get0_group(k);
	d = BN_bin2bn(d_be, 32, NULL);
	pub = EC_POINT_new(grp);
	if (!d || !pub ||
	    EC_KEY_set_private_key(k, d) != 1 ||
	    EC_POINT_mul(grp, pub, d, NULL, NULL, NULL) != 1 ||
	    EC_KEY_set_public_key(k, pub) != 1) {
		EC_POINT_free(pub);
		BN_free(d);
		EC_KEY_free(k);
		return NULL;
	}
	EC_POINT_free(pub);
	BN_free(d);
	return k;
}

static inline EC_POINT *val_ec_point_from_be32(const EC_GROUP *grp,
					       const uint8_t x_be[32], const uint8_t y_be[32])
{
	BIGNUM *bx = BN_bin2bn(x_be, 32, NULL);
	BIGNUM *by = BN_bin2bn(y_be, 32, NULL);
	EC_POINT *pt = EC_POINT_new(grp);

	if (!bx || !by || !pt ||
	    EC_POINT_set_affine_coordinates_GFp(grp, pt, bx, by, NULL) != 1) {
		EC_POINT_free(pt);
		BN_free(bx);
		BN_free(by);
		return NULL;
	}
	BN_free(bx);
	BN_free(by);
	return pt;
}

/* ---- DMI & pre-TLS key derivation (tls.py:set_hwkey) ---- */

static inline void val_strip_ws(char *s)
{
	size_t n = strlen(s);

	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = 0;
}

static inline bool val_read_trimmed_file(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	long n;

	if (fd < 0)
		return false;
	n = read(fd, buf, cap - 1);
	close(fd);
	if (n <= 0) {
		buf[0] = 0;
		return false;
	}
	buf[n] = 0;
	val_strip_ws(buf);
	return buf[0] != 0;
}

static inline bool validity_set_hwkey(struct validity *s, const char *product_name,
				      const char *serial_number)
{
	uint8_t seed[3 + 128 + 1 + 128 + 1];
	uint8_t sign_seed[8 + 32];
	size_t plen = strlen(product_name), slen = strlen(serial_number);
	size_t seed_len;

	if (plen >= sizeof(s->dmi_product) || slen >= sizeof(s->dmi_serial))
		return false;
	memcpy(s->dmi_product, product_name, plen + 1);
	memcpy(s->dmi_serial, serial_number, slen + 1);

	memcpy(seed, "GWK", 3);
	memcpy(seed + 3, product_name, plen + 1);
	memcpy(seed + 3 + plen + 1, serial_number, slen + 1);
	seed_len = 3 + plen + 1 + slen + 1;
	if (!validity_prf(validity_password_hardcoded, sizeof(validity_password_hardcoded),
			  seed, seed_len, s->psk_enc, 32))
		return false;
	memcpy(sign_seed, "GWK_SIGN", 8);
	memcpy(sign_seed + 8, validity_gwk_sign_hardcoded, 32);
	return validity_prf(s->psk_enc, 32, sign_seed, sizeof(sign_seed), s->psk_val, 32);
}

static inline void validity_load_dmi(struct validity *s)
{
	char buf[260], prod[128], ser[128];
	const char *dmi_path = s->dmi_file[0] ? s->dmi_file : VALIDITY_DMI_FILE;
	int fd = open(dmi_path, O_RDONLY | O_CLOEXEC);

	if (fd >= 0) {
		long n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n > 0) {
			char *nl;

			buf[n] = 0;
			nl = buf;
			while (*nl && *nl != '\n')
				nl++;
			if (*nl == '\n') {
				size_t pl = (size_t)(nl - buf);
				size_t sl;

				if (pl >= sizeof(prod))
					pl = sizeof(prod) - 1;
				memcpy(prod, buf, pl);
				prod[pl] = 0;
				val_strip_ws(prod);
				nl++;
				sl = strlen(nl);
				if (sl >= sizeof(ser))
					sl = sizeof(ser) - 1;
				memcpy(ser, nl, sl);
				ser[sl] = 0;
				val_strip_ws(ser);
				if (prod[0] && ser[0]) {
					validity_set_hwkey(s, prod, ser);
					return;
				}
			}
		}
	}
	if (val_read_trimmed_file("/sys/class/dmi/id/product_name", prod, sizeof(prod)) &&
	    val_read_trimmed_file("/sys/class/dmi/id/product_serial", ser, sizeof(ser))) {
		validity_set_hwkey(s, prod, ser);
		return;
	}
	VALIDITY_LOGW("DMI product_name/product_serial unreadable; falling back to VirtualBox/0");
	validity_set_hwkey(s, "VirtualBox", "0");
}

/* ---- USB transport over usbfs ---- */

static inline bool validity_probe_node(const char *path, uint16_t *vid, uint16_t *pid)
{
	uint8_t d[18];
	int fd, n;
	uint16_t v, p;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	n = (int)read(fd, d, sizeof(d));
	close(fd);
	if (n < (int)sizeof(d) || d[1] != 1)
		return false;
	v = val_le16(d + 8);
	p = val_le16(d + 10);
	if (!validity_is_supported_usbid(v, p))
		return false;
	*vid = v;
	*pid = p;
	return true;
}

static inline int validity_find(struct validity *s)
{
	if (s->path[0] && validity_probe_node(s->path, &s->vid, &s->pid))
		return 0;
	for (int bus = 1; bus <= 32; bus++) {
		for (int dev = 1; dev <= 127; dev++) {
			snprintf(s->path, sizeof(s->path), "/dev/bus/usb/%03d/%03d", bus, dev);
			if (validity_probe_node(s->path, &s->vid, &s->pid))
				return 0;
		}
	}
	s->path[0] = 0;
	return -ENODEV;
}

static inline int validity_usb_bulk_write(struct validity *s, unsigned int ep,
					  const uint8_t *data, size_t len, unsigned int timeout_ms)
{
	size_t off = 0;

	while (off < len) {
		unsigned int chunk = (unsigned int)(len - off);
		struct validity_usb_bulk bulk;
		int n;

		if (chunk > VALIDITY_USB_CHUNK)
			chunk = VALIDITY_USB_CHUNK;
		bulk.ep = ep;
		bulk.len = chunk;
		bulk.timeout = timeout_ms;
		bulk.data = (void *)(uintptr_t)(data + off);
		n = ioctl(s->fd, (int)VALIDITY_USB_BULK, &bulk);
		if (n < 0)
			return -VALIDITY_ERRNO;
		if ((unsigned int)n != chunk)
			return -EIO;
		off += (size_t)n;
	}
	return 0;
}

static inline int validity_usb_bulk_read(struct validity *s, unsigned int ep,
					 uint8_t *out, size_t max_len, size_t *out_len,
					 unsigned int timeout_ms)
{
	size_t total = 0;

	while (total < max_len) {
		unsigned int chunk = (unsigned int)(max_len - total);
		struct validity_usb_bulk bulk;
		int n;

		if (chunk > VALIDITY_USB_CHUNK)
			chunk = VALIDITY_USB_CHUNK;
		bulk.ep = ep;
		bulk.len = chunk;
		bulk.timeout = timeout_ms;
		bulk.data = out + total;
		n = ioctl(s->fd, (int)VALIDITY_USB_BULK, &bulk);
		if (n < 0) {
			if (total > 0 && VALIDITY_ERRNO == ETIMEDOUT)
				break;
			return -VALIDITY_ERRNO;
		}
		total += (size_t)n;
		if ((unsigned int)n < chunk)
			break;
	}
	*out_len = total;
	return 0;
}

static inline int validity_usb_cmd(struct validity *s, const uint8_t *tx, size_t tx_len,
				   uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
	int r;

	if (!tx_len) {
		*rx_len = 0;
		return 0;
	}
	r = validity_usb_bulk_write(s, VALIDITY_EP_OUT, tx, tx_len, 15000);
	if (r < 0) {
		VALIDITY_LOGE("Validity USB bulk write EP 0x01 failed: %d (tx_len=%zu)", r, tx_len);
		return r;
	}
	r = validity_usb_bulk_read(s, VALIDITY_EP_IN, rx, rx_cap, rx_len, 15000);
	if (r < 0)
		VALIDITY_LOGE("Validity USB bulk read EP 0x81 failed: %d (tx_len=%zu rx_cap=%zu)", r, tx_len, rx_cap);
	return r;
}

/* Interrupt transfer on EP 0x83 via USBDEVFS_SUBMITURB + poll + REAPURBNDELAY */
static inline int validity_usb_wait_int(struct validity *s, uint8_t *buf, size_t buf_len,
					size_t *out_len, bool cancellable, unsigned int timeout_ms)
{
	struct validity_usb_urb urb;
	struct validity_usb_urb *reaped = NULL;
	unsigned int elapsed = 0;
	int r;

	if (cancellable && validity_cancel_requested(s))
		return -ECANCELED;
	memset(&urb, 0, sizeof(urb));
	urb.type = VALIDITY_URB_TYPE_INTERRUPT;
	urb.endpoint = VALIDITY_EP_INT;
	urb.buffer = buf;
	urb.buffer_length = (int)buf_len;
	urb.usercontext = &urb;
	if (ioctl(s->fd, (int)VALIDITY_USB_SUBMITURB, &urb) < 0)
		return -VALIDITY_ERRNO;

	for (;;) {
		struct pollfd pfd = { s->fd, POLLOUT, 0 };

		if (cancellable && validity_cancel_requested(s)) {
			r = -ECANCELED;
			goto discard;
		}
		if (!cancellable && elapsed >= timeout_ms) {
			r = -ETIMEDOUT;
			goto discard;
		}
		r = poll(&pfd, 1, 100);
		if (r < 0) {
			if (VALIDITY_ERRNO == EINTR)
				continue;
			r = -VALIDITY_ERRNO;
			goto discard;
		}
		if (r == 0) {
			elapsed += 100;
			continue;
		}
		if (ioctl(s->fd, (int)VALIDITY_USB_REAPURBNDELAY, &reaped) == 0) {
			if (reaped->status != 0)
				return reaped->status < 0 ? reaped->status : -EIO;
			*out_len = (size_t)reaped->actual_length;
			return 0;
		}
		if (VALIDITY_ERRNO == EAGAIN)
			continue;
		return -VALIDITY_ERRNO;
	}

discard:
	(void)ioctl(s->fd, (int)VALIDITY_USB_DISCARDURB, &urb);
	for (int tries = 0; tries < 20; tries++) {
		if (ioctl(s->fd, (int)VALIDITY_USB_REAPURBNDELAY, &reaped) == 0)
			break;
		if (VALIDITY_ERRNO != EAGAIN)
			break;
		usleep(5000);
	}
	return r;
}

static inline int validity_check_status(const uint8_t *rsp, size_t len, uint16_t *err_out)
{
	uint16_t st;

	if (len < 2) {
		if (err_out)
			*err_out = 0xffff;
		return -EPROTO;
	}
	st = val_le16(rsp);
	if (err_out)
		*err_out = st;
	return st == 0 ? 0 : -EIO;
}

/* ---- Custom TLS 1.2 over USB (matches tls.py) ---- */

static inline int validity_tls_handle_priv(struct validity *s, const uint8_t *body, size_t len)
{
	uint8_t mac[32], plain[256], d_be[32];
	unsigned int mac_len = 0;
	size_t cipher_len, plain_len = 0;
	uint8_t pad_len;

	if (len > sizeof(s->priv_blob) || len < 1 + 16 + 16 + 32 || body[0] != 0x02)
		return -EPROTO;
	memcpy(s->priv_blob, body, len);
	s->priv_blob_len = len;

	cipher_len = len - 1 - 32;
	if (!HMAC(EVP_sha256(), s->psk_val, 32, body + 1, cipher_len, mac, &mac_len) || mac_len != 32)
		return -EIO;
	if (CRYPTO_memcmp(mac, body + 1 + cipher_len, 32) != 0) {
		VALIDITY_LOGE("Validity TLS private key HMAC failed (wrong DMI product/serial or paired with another PC)");
		return -EPROTO;
	}
	if (cipher_len - 16 > sizeof(plain))
		return -EPROTO;
	if (!val_aes256_cbc(false, s->psk_enc, body + 1, body + 1 + 16, cipher_len - 16, plain, &plain_len))
		return -EPROTO;
	if (plain_len == 0)
		return -EPROTO;
	pad_len = plain[plain_len - 1];
	if (pad_len == 0 || pad_len > plain_len || plain_len - pad_len < 96)
		return -EPROTO;
	/* x = plain[0..31], y = plain[32..63], d = plain[64..95] (little-endian) */
	val_reverse_copy(d_be, plain + 64, 32);
	if (s->priv_key)
		EC_KEY_free(s->priv_key);
	s->priv_key = val_ec_key_from_priv_be32(d_be);
	memset(plain, 0, sizeof(plain));
	memset(d_be, 0, sizeof(d_be));
	return s->priv_key ? 0 : -EPROTO;
}

static inline int validity_tls_handle_ecdh(struct validity *s, const uint8_t *body, size_t len)
{
	uint8_t x_be[32], y_be[32], digest[32];
	uint32_t sig_len;
	EC_KEY *fw_key;
	EC_POINT *fw_pt;
	const EC_GROUP *grp;
	int vr;

	if (len > sizeof(s->ecdh_blob) || len < 0x94)
		return -EPROTO;
	memcpy(s->ecdh_blob, body, len);
	s->ecdh_blob_len = len;

	sig_len = val_le32(body + 0x90);
	if (0x94 + sig_len > len)
		return -EPROTO;
	for (size_t i = 0x94 + sig_len; i < len; i++) {
		if (body[i] != 0)
			return -EPROTO;
	}
	if (!val_sha256(body, 0x90, digest))
		return -EIO;
	fw_key = EC_KEY_new_by_curve_name(VALIDITY_NID_P256);
	if (!fw_key)
		return -EIO;
	grp = EC_KEY_get0_group(fw_key);
	fw_pt = val_ec_point_from_be32(grp, validity_fwpub_x, validity_fwpub_y);
	if (!fw_pt || EC_KEY_set_public_key(fw_key, fw_pt) != 1) {
		EC_POINT_free(fw_pt);
		EC_KEY_free(fw_key);
		return -EIO;
	}
	vr = ECDSA_verify(0, digest, 32, body + 0x94, sig_len, fw_key);
	EC_POINT_free(fw_pt);
	if (vr != 1) {
		EC_KEY_free(fw_key);
		VALIDITY_LOGE("Validity ECDH key signature verification failed");
		return -EPROTO;
	}

	val_reverse_copy(x_be, body + 0x08, 32);
	val_reverse_copy(y_be, body + 0x4c, 32);
	if (s->ecdh_q)
		EC_POINT_free(s->ecdh_q);
	s->ecdh_q = val_ec_point_from_be32(grp, x_be, y_be);
	EC_KEY_free(fw_key);
	return s->ecdh_q ? 0 : -EPROTO;
}

static inline int validity_tls_parse_flash(struct validity *s, const uint8_t *data, size_t len)
{
	size_t off = 0;

	while (off + 4 + 32 <= len) {
		uint16_t id = val_le16(data + off);
		uint16_t sz = val_le16(data + off + 2);
		const uint8_t *hs = data + off + 4;
		const uint8_t *body = data + off + 36;
		uint8_t calc[32];
		int r;

		if (id == 0xffff)
			break;
		if (off + 36 + sz > len)
			return -EPROTO;
		if (!val_sha256(body, sz, calc) || CRYPTO_memcmp(calc, hs, 32) != 0) {
			VALIDITY_LOGE("Validity TLS flash block 0x%04x hash mismatch", id);
			return -EPROTO;
		}
		if (id == 3) {
			if (sz > sizeof(s->tls_cert))
				return -EPROTO;
			memcpy(s->tls_cert, body, sz);
			s->tls_cert_len = sz;
		} else if (id == 4) {
			if ((r = validity_tls_handle_priv(s, body, sz)) < 0)
				return r;
		} else if (id == 6) {
			if ((r = validity_tls_handle_ecdh(s, body, sz)) < 0)
				return r;
		} else if (id == 0 || id == 1 || id == 2) {
			for (size_t i = 0; i < sz; i++) {
				if (body[i] != 0)
					return -EPROTO;
			}
		}
		off += 36 + (size_t)sz;
	}
	if (!s->tls_cert_len || !s->priv_key || !s->ecdh_q) {
		VALIDITY_LOGE("Validity TLS flash incomplete (cert=%zu priv=%d ecdh=%d)",
			      s->tls_cert_len, s->priv_key != NULL, s->ecdh_q != NULL);
		return -EPROTO;
	}
	return 0;
}

/* TLS record sign + custom pad + AES-256-CBC encrypt (matches tls.py:sign + encrypt) */
static inline int validity_tls_wrap_record(const struct validity *s, uint8_t rec_type,
					   const uint8_t *plain, size_t plain_len, bool encrypt,
					   uint8_t *out, size_t out_cap, size_t *out_len)
{
	if (!encrypt) {
		if (5 + plain_len > out_cap)
			return -EINVAL;
		out[0] = rec_type;
		out[1] = 3;
		out[2] = 3;
		val_put_be16(out + 3, (uint16_t)plain_len);
		memcpy(out + 5, plain, plain_len);
		*out_len = 5 + plain_len;
		return 0;
	}
	{
		size_t signed_len = plain_len + 32;
		size_t pad_len = 16 - (signed_len % 16);
		size_t padded_len = signed_len + pad_len;
		uint8_t *buf;
		uint8_t hdr[5], iv[16];
		unsigned int hlen = 0;
		size_t enc_len = 0;

		if (5 + 16 + padded_len > out_cap)
			return -EINVAL;
		buf = malloc(5 + padded_len);
		if (!buf)
			return -EIO;
		hdr[0] = rec_type;
		hdr[1] = 3;
		hdr[2] = 3;
		val_put_be16(hdr + 3, (uint16_t)plain_len);
		memcpy(buf, hdr, 5);
		memcpy(buf + 5, plain, plain_len);
		if (!HMAC(EVP_sha256(), s->sign_key, 32, buf, 5 + plain_len, buf + 5 + plain_len, &hlen) ||
		    hlen != 32) {
			free(buf);
			return -EIO;
		}
		memset(buf + 5 + signed_len, (uint8_t)(pad_len - 1), pad_len);
		VALIDITY_RANDOM_BUF(iv, 16);
		out[0] = rec_type;
		out[1] = 3;
		out[2] = 3;
		val_put_be16(out + 3, (uint16_t)(16 + padded_len));
		memcpy(out + 5, iv, 16);
		if (!val_aes256_cbc(true, s->enc_key, iv, buf + 5, padded_len, out + 5 + 16, &enc_len) ||
		    enc_len != padded_len) {
			free(buf);
			return -EIO;
		}
		free(buf);
		*out_len = 5 + 16 + enc_len;
		return 0;
	}
}

/* Decrypt + custom unpad + HMAC validate a single TLS record payload (matches tls.py:decrypt + validate) */
static inline int validity_tls_unwrap_payload(const struct validity *s, uint8_t rec_type,
					      const uint8_t *enc, size_t enc_len,
					      uint8_t *plain_out, size_t out_cap, size_t *plain_len)
{
	uint8_t *buf;
	size_t dec_len = 0, unpadded, msg_len;
	uint8_t pad_val, mac[32];
	unsigned int mac_len = 0;

	if (enc_len < 32 || ((enc_len - 16) % 16) != 0)
		return -EPROTO;
	buf = malloc(5 + enc_len - 16);
	if (!buf)
		return -EIO;
	if (!val_aes256_cbc(false, s->dec_key, enc, enc + 16, enc_len - 16, buf + 5, &dec_len)) {
		free(buf);
		return -EPROTO;
	}
	if (dec_len == 0) {
		free(buf);
		return -EPROTO;
	}
	pad_val = buf[5 + dec_len - 1];
	if ((size_t)pad_val + 1 > dec_len) {
		free(buf);
		return -EPROTO;
	}
	unpadded = dec_len - ((size_t)pad_val + 1);
	if (unpadded < 32) {
		free(buf);
		return -EPROTO;
	}
	msg_len = unpadded - 32;
	if (msg_len > out_cap) {
		free(buf);
		return -EINVAL;
	}
	buf[0] = rec_type;
	buf[1] = 3;
	buf[2] = 3;
	val_put_be16(buf + 3, (uint16_t)msg_len);
	if (!HMAC(EVP_sha256(), s->val_key, 32, buf, 5 + msg_len, mac, &mac_len) || mac_len != 32 ||
	    CRYPTO_memcmp(mac, buf + 5 + msg_len, 32) != 0) {
		free(buf);
		VALIDITY_LOGE("Validity TLS record HMAC validation failed");
		return -EPROTO;
	}
	memcpy(plain_out, buf + 5, msg_len);
	*plain_len = msg_len;
	free(buf);
	return 0;
}

static inline int validity_tls_parse_response(struct validity *s, EVP_MD_CTX *hs_hash,
					      const uint8_t *rsp, size_t rsp_len,
					      uint8_t *app_out, size_t app_cap, size_t *app_len)
{
	size_t off = 0, app_total = 0;

	while (off < rsp_len) {
		uint8_t rec_type, mj, mn;
		uint16_t sz;
		const uint8_t *pkt;

		if (off + 5 > rsp_len)
			return -EPROTO;
		rec_type = rsp[off];
		mj = rsp[off + 1];
		mn = rsp[off + 2];
		sz = val_be16(rsp + off + 3);
		off += 5;
		if (mj != 3 || mn != 3 || off + sz > rsp_len)
			return -EPROTO;
		pkt = rsp + off;
		off += sz;

		if (rec_type == 0x14) {
			if (sz != 1 || pkt[0] != 1)
				return -EPROTO;
			s->secure_rx = true;
		} else if (rec_type == 0x17) {
			size_t dec_len = 0;
			int r;

			if (!s->secure_rx || !app_out)
				return -EPROTO;
			r = validity_tls_unwrap_payload(s, 0x17, pkt, sz,
							app_out + app_total, app_cap - app_total, &dec_len);
			if (r < 0)
				return r;
			app_total += dec_len;
		} else if (rec_type == 0x16) {
			uint8_t hs_buf[1024];
			const uint8_t *hs = pkt;
			size_t hs_len = sz, hoff = 0;

			if (!hs_hash)
				return -EPROTO;
			if (s->secure_rx) {
				int r = validity_tls_unwrap_payload(s, 0x16, pkt, sz,
								    hs_buf, sizeof(hs_buf), &hs_len);
				if (r < 0)
					return r;
				hs = hs_buf;
			}
			while (hoff < hs_len) {
				uint8_t htype;
				uint32_t hlen;
				const uint8_t *hp;

				if (hoff + 4 > hs_len)
					return -EPROTO;
				htype = hs[hoff];
				hlen = val_be24(hs + hoff + 1);
				if (hoff + 4 + hlen > hs_len)
					return -EPROTO;
				hp = hs + hoff + 4;

				if (htype == 0x02) {	/* ServerHello */
					uint8_t sid_len;

					if (hlen < 2 + 32 + 1)
						return -EPROTO;
					if (hp[0] != 3 || hp[1] != 3)
						return -EPROTO;
					memcpy(s->server_random, hp + 2, 32);
					sid_len = hp[34];
					if (35 + (size_t)sid_len + 3 != hlen)
						return -EPROTO;
					if (val_be16(hp + 35 + sid_len) != 0xc005)
						return -EPROTO;
					if (hp[35 + sid_len + 2] != 0)
						return -EPROTO;
				} else if (htype == 0x0d) {	/* CertificateRequest */
					if (hlen != 4 || val_be16(hp) != 0x0140 || val_be16(hp + 2) != 0)
						return -EPROTO;
				} else if (htype == 0x0e) {	/* ServerHelloDone */
					if (hlen != 0)
						return -EPROTO;
				} else if (htype == 0x14) {	/* ServerFinished */
					uint8_t cur_hash[32], seed[15 + 32], expect[12];
					unsigned int dlen = 0;
					EVP_MD_CTX *copy = EVP_MD_CTX_new();

					if (hlen != 12 || !copy) {
						EVP_MD_CTX_free(copy);
						return -EPROTO;
					}
					if (EVP_MD_CTX_copy_ex(copy, hs_hash) != 1 ||
					    EVP_DigestFinal_ex(copy, cur_hash, &dlen) != 1 || dlen != 32) {
						EVP_MD_CTX_free(copy);
						return -EIO;
					}
					EVP_MD_CTX_free(copy);
					memcpy(seed, "server finished", 15);
					memcpy(seed + 15, cur_hash, 32);
					if (!validity_prf(s->master_secret, 48, seed, sizeof(seed), expect, 12) ||
					    CRYPTO_memcmp(expect, hp, 12) != 0) {
						VALIDITY_LOGE("Validity TLS server finished check failed");
						return -EPROTO;
					}
				} else {
					return -EPROTO;
				}
				if (EVP_DigestUpdate(hs_hash, hs + hoff, 4 + hlen) != 1)
					return -EIO;
				hoff += 4 + hlen;
			}
		} else {
			return -EPROTO;
		}
	}
	if (app_len)
		*app_len = app_total;
	return 0;
}

static inline int validity_tls_open(struct validity *s)
{
	EVP_MD_CTX *hs_hash = NULL, *copy = NULL;
	EC_KEY *eph = NULL;
	BIGNUM *bx = NULL, *by = NULL;
	uint8_t tx[2048], rx[2048];
	uint8_t ch[80], hs2[1536], fin[16], pms[32], seed[64], key_block[0x120];
	uint8_t cur_hash[32], sig[128], fin_seed[15 + 32], verify_data[12];
	size_t tx_len = 0, rx_len = 0, ch_len = 0, hs2_len = 0, rec_len = 0;
	unsigned int sig_len = sizeof(sig), dlen = 0;
	int r = -EIO;

	s->secure_tx = false;
	s->secure_rx = false;
	if (!s->priv_key || !s->ecdh_q || !s->tls_cert_len)
		return -EINVAL;

	hs_hash = EVP_MD_CTX_new();
	if (!hs_hash || EVP_DigestInit_ex(hs_hash, EVP_sha256(), NULL) != 1)
		goto out;

	/* ClientHello (matches tls.py:make_client_hello, including the -2 extensions length quirk) */
	VALIDITY_RANDOM_BUF(s->client_random, 32);
	{
		size_t p = 4;

		ch[p++] = 3; ch[p++] = 3;			/* TLS 1.2 */
		memcpy(ch + p, s->client_random, 32);
		p += 32;
		ch[p++] = 7;					/* session ID len 7 */
		memset(ch + p, 0, 7);
		p += 7;
		ch[p++] = 0x00; ch[p++] = 0x06;			/* cipher suites len 6 */
		ch[p++] = 0xc0; ch[p++] = 0x05;
		ch[p++] = 0x00; ch[p++] = 0x3d;
		ch[p++] = 0x00; ch[p++] = 0x8d;
		ch[p++] = 0x00;					/* compression methods: len 0 (matches tls.py) */
		/* extensions: 12 bytes, declared as 12 - 2 = 10 (matches tls.py:404) */
		ch[p++] = 0x00; ch[p++] = 0x0a;
		ch[p++] = 0x00; ch[p++] = 0x04; ch[p++] = 0x00; ch[p++] = 0x02; ch[p++] = 0x00; ch[p++] = 0x17;
		ch[p++] = 0x00; ch[p++] = 0x0b; ch[p++] = 0x00; ch[p++] = 0x02; ch[p++] = 0x01; ch[p++] = 0x00;

		ch[0] = 0x01;
		val_put_be24(ch + 1, (uint32_t)(p - 4));
		ch_len = p;
	}
	if (EVP_DigestUpdate(hs_hash, ch, ch_len) != 1)
		goto out;

	tx[0] = 0x44; tx[1] = 0x00; tx[2] = 0x00; tx[3] = 0x00;
	if ((r = validity_tls_wrap_record(s, 0x16, ch, ch_len, false, tx + 4, sizeof(tx) - 4, &rec_len)) < 0)
		goto out;
	tx_len = 4 + rec_len;
	if ((r = validity_usb_cmd(s, tx, tx_len, rx, sizeof(rx), &rx_len)) < 0)
		goto out;
	if ((r = validity_tls_parse_response(s, hs_hash, rx, rx_len, NULL, 0, NULL)) < 0)
		goto out;

	/* Ephemeral ECDH keypair + key derivation (matches tls.py:make_keys) */
	eph = EC_KEY_new_by_curve_name(VALIDITY_NID_P256);
	if (!eph || EC_KEY_generate_key(eph) != 1) {
		r = -EIO;
		goto out;
	}
	if (ECDH_compute_key(pms, 32, s->ecdh_q, eph, NULL) != 32) {
		r = -EIO;
		goto out;
	}
	memcpy(seed, s->client_random, 32);
	memcpy(seed + 32, s->server_random, 32);
	{
		uint8_t ms_seed[13 + 64], ke_seed[13 + 64];

		memcpy(ms_seed, "master secret", 13);
		memcpy(ms_seed + 13, seed, 64);
		if (!validity_prf(pms, 32, ms_seed, sizeof(ms_seed), s->master_secret, 48)) {
			r = -EIO;
			goto out;
		}
		memcpy(ke_seed, "key expansion", 13);
		memcpy(ke_seed + 13, seed, 64);
		if (!validity_prf(s->master_secret, 48, ke_seed, sizeof(ke_seed), key_block, sizeof(key_block))) {
			r = -EIO;
			goto out;
		}
		memcpy(s->sign_key, key_block + 0x00, 32);
		memcpy(s->val_key, key_block + 0x20, 32);
		memcpy(s->enc_key, key_block + 0x40, 32);
		memcpy(s->dec_key, key_block + 0x60, 32);
	}

	/* Certificate (0x0b) */
	{
		size_t body_len = 3 + 3 + 2 + s->tls_cert_len;

		if (hs2_len + 4 + body_len > sizeof(hs2)) {
			r = -EINVAL;
			goto out;
		}
		hs2[hs2_len] = 0x0b;
		val_put_be24(hs2 + hs2_len + 1, (uint32_t)body_len);
		val_put_be24(hs2 + hs2_len + 4, (uint32_t)s->tls_cert_len);
		val_put_be24(hs2 + hs2_len + 7, (uint32_t)s->tls_cert_len);
		hs2[hs2_len + 10] = 0xac;
		hs2[hs2_len + 11] = 0x16;
		memcpy(hs2 + hs2_len + 12, s->tls_cert, s->tls_cert_len);
		if (EVP_DigestUpdate(hs_hash, hs2 + hs2_len, 4 + body_len) != 1) {
			r = -EIO;
			goto out;
		}
		hs2_len += 4 + body_len;
	}

	/* ClientKeyExchange (0x10): 0x04 || X_be32 || Y_be32 */
	{
		uint8_t x_be[32], y_be[32];

		bx = BN_new();
		by = BN_new();
		if (!bx || !by ||
		    EC_POINT_get_affine_coordinates_GFp(EC_KEY_get0_group(eph), EC_KEY_get0_public_key(eph),
							bx, by, NULL) != 1 ||
		    !val_bn_to_be32(bx, x_be) || !val_bn_to_be32(by, y_be)) {
			r = -EIO;
			goto out;
		}
		hs2[hs2_len] = 0x10;
		val_put_be24(hs2 + hs2_len + 1, 65);
		hs2[hs2_len + 4] = 0x04;
		memcpy(hs2 + hs2_len + 5, x_be, 32);
		memcpy(hs2 + hs2_len + 37, y_be, 32);
		if (EVP_DigestUpdate(hs_hash, hs2 + hs2_len, 4 + 65) != 1) {
			r = -EIO;
			goto out;
		}
		hs2_len += 4 + 65;
	}

	/* CertificateVerify (0x0f): ECDSA-SHA256(handshake_hash) raw DER */
	copy = EVP_MD_CTX_new();
	if (!copy || EVP_MD_CTX_copy_ex(copy, hs_hash) != 1 ||
	    EVP_DigestFinal_ex(copy, cur_hash, &dlen) != 1 || dlen != 32) {
		r = -EIO;
		goto out;
	}
	if (ECDSA_sign(0, cur_hash, 32, sig, &sig_len, s->priv_key) != 1) {
		r = -EIO;
		goto out;
	}
	hs2[hs2_len] = 0x0f;
	val_put_be24(hs2 + hs2_len + 1, sig_len);
	memcpy(hs2 + hs2_len + 4, sig, sig_len);
	if (EVP_DigestUpdate(hs_hash, hs2 + hs2_len, 4 + sig_len) != 1) {
		r = -EIO;
		goto out;
	}
	hs2_len += 4 + sig_len;

	/* ClientFinished (0x14), encrypted after ChangeCipherSpec */
	if (EVP_MD_CTX_copy_ex(copy, hs_hash) != 1 ||
	    EVP_DigestFinal_ex(copy, cur_hash, &dlen) != 1 || dlen != 32) {
		r = -EIO;
		goto out;
	}
	memcpy(fin_seed, "client finished", 15);
	memcpy(fin_seed + 15, cur_hash, 32);
	if (!validity_prf(s->master_secret, 48, fin_seed, sizeof(fin_seed), verify_data, 12)) {
		r = -EIO;
		goto out;
	}
	fin[0] = 0x14;
	val_put_be24(fin + 1, 12);
	memcpy(fin + 4, verify_data, 12);
	/* Quirk in Validity 9x firmware (matches tls.py:make_finish): ClientFinished is NOT
	 * fed into handshake_hash before verifying ServerFinished. */

	/* Assemble 44000000 || Handshake(hs2) || ChangeCipherSpec || EncryptedHandshake(fin) */
	tx[0] = 0x44; tx[1] = 0x00; tx[2] = 0x00; tx[3] = 0x00;
	tx_len = 4;
	if ((r = validity_tls_wrap_record(s, 0x16, hs2, hs2_len, false, tx + tx_len, sizeof(tx) - tx_len, &rec_len)) < 0)
		goto out;
	tx_len += rec_len;
	{
		static const uint8_t ccs[6] = { 0x14, 0x03, 0x03, 0x00, 0x01, 0x01 };

		memcpy(tx + tx_len, ccs, sizeof(ccs));
		tx_len += sizeof(ccs);
	}
	s->secure_tx = true;
	if ((r = validity_tls_wrap_record(s, 0x16, fin, 16, true, tx + tx_len, sizeof(tx) - tx_len, &rec_len)) < 0)
		goto out;
	tx_len += rec_len;

	if ((r = validity_usb_cmd(s, tx, tx_len, rx, sizeof(rx), &rx_len)) < 0)
		goto out;
	r = validity_tls_parse_response(s, hs_hash, rx, rx_len, NULL, 0, NULL);
out:
	BN_free(bx);
	BN_free(by);
	EC_KEY_free(eph);
	EVP_MD_CTX_free(copy);
	EVP_MD_CTX_free(hs_hash);
	if (r < 0) {
		s->secure_tx = false;
		s->secure_rx = false;
	}
	return r;
}

static inline int validity_tls_cmd(struct validity *s, const uint8_t *tx, size_t tx_len,
				   uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
	uint8_t *enc_tx, *enc_rx;
	size_t enc_tx_len = 0, enc_rx_len = 0;
	int r;

	if (!(s->secure_tx && s->secure_rx))
		return validity_usb_cmd(s, tx, tx_len, rx, rx_cap, rx_len);

	size_t enc_rx_cap = rx_cap + 256;
	if (enc_rx_cap < 32768)
		enc_rx_cap = 32768;
	enc_tx = malloc(tx_len + 128);
	enc_rx = malloc(enc_rx_cap);
	if (!enc_tx || !enc_rx) {
		free(enc_tx);
		free(enc_rx);
		return -EIO;
	}
	r = validity_tls_wrap_record(s, 0x17, tx, tx_len, true, enc_tx, tx_len + 128, &enc_tx_len);
	if (r == 0)
		r = validity_usb_cmd(s, enc_tx, enc_tx_len, enc_rx, enc_rx_cap, &enc_rx_len);
	if (r == 0) {
		r = validity_tls_parse_response(s, NULL, enc_rx, enc_rx_len, rx, rx_cap, rx_len);
		if (r < 0) {
			char hx[64];
			size_t n = enc_rx_len < 16 ? enc_rx_len : 16, hp = 0;
			for (size_t i = 0; i < n && hp + 3 < sizeof(hx); i++)
				hp += (size_t)snprintf(hx + hp, sizeof(hx) - hp, "%02x ", enc_rx[i]);
			hx[hp] = 0;
			VALIDITY_LOGE("Validity TLS parse failed: r=%d tx_cmd=0x%02x tx_len=%zu enc_rx_len=%zu head=[%s]",
				      r, tx_len ? tx[0] : 0, tx_len, enc_rx_len, hx);
		}
	}
	free(enc_tx);
	free(enc_rx);
	return r;
}

/* ---- Flash & low-level sensor commands (flash.py) ---- */

static inline int validity_cmd_status(struct validity *s, const uint8_t *tx, size_t tx_len,
				      uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
	uint16_t err = 0;
	int r = validity_tls_cmd(s, tx, tx_len, rx, rx_cap, rx_len);

	if (r < 0)
		return r;
	r = validity_check_status(rx, *rx_len, &err);
	if (r < 0)
		VALIDITY_LOGE("Validity cmd 0x%02x (tx_len=%zu) status=0x%04x (rx_len=%zu)",
			      tx_len ? tx[0] : 0, tx_len, err, *rx_len);
	return r;
}

static inline int validity_write_enable(struct validity *s)
{
	const uint8_t *blob = validity_is_0090(s) ? validity_db_write_90 : validity_db_write_97;
	size_t blen = validity_is_0090(s) ? sizeof(validity_db_write_90) : sizeof(validity_db_write_97);
	uint8_t rx[64];
	size_t rx_len = 0;

	return validity_cmd_status(s, blob, blen, rx, sizeof(rx), &rx_len);
}

static inline int validity_call_cleanups(struct validity *s)
{
	static const uint8_t cmd = 0x1a;
	uint8_t rx[64];
	size_t rx_len = 0;
	uint16_t err = 0;
	int r = validity_tls_cmd(s, &cmd, 1, rx, sizeof(rx), &rx_len);

	if (r < 0)
		return r;
	if (validity_check_status(rx, rx_len, &err) < 0 && err != 0x0491)
		return -EIO;
	return 0;
}

static inline int validity_reboot(struct validity *s)
{
	static const uint8_t cmd[3] = { 0x05, 0x02, 0x00 };
	uint8_t rx[64];
	size_t rx_len = 0;

	return validity_cmd_status(s, cmd, sizeof(cmd), rx, sizeof(rx), &rx_len);
}

static inline int validity_read_flash(struct validity *s, uint8_t partition, uint32_t addr,
				      uint32_t size, uint8_t *out, size_t *out_len)
{
	uint8_t cmd[13];
	uint8_t *rx;
	size_t rx_len = 0;
	uint32_t sz;
	int r;

	cmd[0] = 0x40;
	cmd[1] = partition;
	cmd[2] = 1;
	val_put_le16(cmd + 3, 0);
	val_put_le32(cmd + 5, addr);
	val_put_le32(cmd + 9, size);

	rx = malloc(size + 64);
	if (!rx)
		return -EIO;
	r = validity_cmd_status(s, cmd, sizeof(cmd), rx, size + 64, &rx_len);
	if (r == 0) {
		if (rx_len < 8) {
			r = -EPROTO;
		} else {
			sz = val_le32(rx + 2);
			if (8 + (size_t)sz > rx_len || sz > size)
				r = -EPROTO;
			else {
				memcpy(out, rx + 8, sz);
				*out_len = sz;
			}
		}
	}
	free(rx);
	return r;
}

static inline int validity_read_flash_all(struct validity *s, uint8_t partition, uint32_t start,
					  size_t size, uint8_t *out)
{
	size_t off = 0;

	while (off < size) {
		size_t got = 0;
		uint8_t blk[0x1000];
		size_t take = size - off;
		int r = validity_read_flash(s, partition, start + (uint32_t)off, 0x1000, blk, &got);

		if (r < 0)
			return r;
		if (take > 0x1000)
			take = 0x1000;
		if (got < take)
			return -EPROTO;
		memcpy(out + off, blk, take);
		off += take;
	}
	return 0;
}

static inline int validity_erase_flash(struct validity *s, uint8_t partition)
{
	uint8_t cmd[2] = { 0x3f, partition }, rx[64];
	size_t rx_len = 0;
	int r;

	if ((r = validity_write_enable(s)) < 0)
		return r;
	r = validity_cmd_status(s, cmd, sizeof(cmd), rx, sizeof(rx), &rx_len);
	(void)validity_call_cleanups(s);
	return r;
}

static inline int validity_write_flash(struct validity *s, uint8_t partition, uint32_t addr,
				       const uint8_t *buf, size_t len)
{
	uint8_t *cmd;
	uint8_t rx[64];
	size_t rx_len = 0;
	int r;

	(void)validity_write_enable(s);
	cmd = malloc(13 + len);
	if (!cmd)
		return -EIO;
	cmd[0] = 0x41;
	cmd[1] = partition;
	cmd[2] = 1;
	val_put_le16(cmd + 3, 0);
	val_put_le32(cmd + 5, addr);
	val_put_le32(cmd + 9, (uint32_t)len);
	memcpy(cmd + 13, buf, len);
	r = validity_cmd_status(s, cmd, 13 + len, rx, sizeof(rx), &rx_len);
	free(cmd);
	(void)validity_call_cleanups(s);
	return r;
}

static inline int validity_write_flash_all(struct validity *s, uint8_t partition, uint32_t addr,
					   const uint8_t *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		size_t chunk = len - off;
		int r;

		if (chunk > 0x1000)
			chunk = 0x1000;
		r = validity_write_flash(s, partition, addr + (uint32_t)off, buf + off, chunk);
		if (r < 0)
			return r;
		off += chunk;
	}
	return 0;
}

static inline int validity_get_fw_info(struct validity *s, uint8_t partition, bool *present,
				       uint16_t *major_out, uint16_t *minor_out)
{
	uint8_t cmd[2] = { 0x43, partition }, rx[512];
	size_t rx_len = 0;
	uint16_t err = 0;
	int r = validity_tls_cmd(s, cmd, sizeof(cmd), rx, sizeof(rx), &rx_len);

	if (r < 0)
		return r;
	if (rx_len == 2 && rx[0] == 0xb0 && rx[1] == 0x04) {
		*present = false;
		return 0;
	}
	if (validity_check_status(rx, rx_len, &err) < 0 || rx_len < 12)
		return -EPROTO;
	*present = true;
	if (major_out)
		*major_out = val_le16(rx + 2);
	if (minor_out)
		*minor_out = val_le16(rx + 4);
	return 0;
}

static inline int validity_send_init(struct validity *s)
{
	static const uint8_t cmd01 = 0x01, cmd19 = 0x19, cmd4302[2] = { 0x43, 0x02 };
	const uint8_t *init_blob = validity_is_0090(s) ? validity_init_90 : validity_init_97;
	size_t init_len = validity_is_0090(s) ? sizeof(validity_init_90) : sizeof(validity_init_97);
	uint8_t rx[512];
	size_t rx_len = 0;
	uint16_t fw_err = 0;
	int r;

	if ((r = validity_cmd_status(s, &cmd01, 1, rx, sizeof(rx), &rx_len)) < 0)
		return r;
	if ((r = validity_cmd_status(s, &cmd19, 1, rx, sizeof(rx), &rx_len)) < 0)
		return r;
	if ((r = validity_usb_cmd(s, cmd4302, 2, rx, sizeof(rx), &rx_len)) < 0)
		return r;
	(void)validity_check_status(rx, rx_len, &fw_err);
	if ((r = validity_cmd_status(s, init_blob, init_len, rx, sizeof(rx), &rx_len)) < 0)
		return r;
	if (fw_err != 0 && !validity_is_0090(s)) {
		VALIDITY_LOGI("Validity fwext not yet loaded; sending clean-slate init");
		if ((r = validity_usb_cmd(s, validity_clean_97, sizeof(validity_clean_97), rx, sizeof(rx), &rx_len)) < 0)
			return r;
	}
	return 0;
}

/* ---- Optional flash initialization & fwext upload (init_flash.py, upload_fwext.py) ---- */

static inline bool val_make_tls_flash(const struct validity *s, uint8_t out[0x1000])
{
	size_t off = 0;
	uint8_t zero1 = 0;
	uint8_t zeroes256[256];
	struct { uint16_t id; const uint8_t *data; size_t len; } blocks[7];

	memset(zeroes256, 0, sizeof(zeroes256));
	blocks[0].id = 0; blocks[0].data = &zero1;                 blocks[0].len = 1;
	blocks[1].id = 4; blocks[1].data = s->priv_blob;           blocks[1].len = s->priv_blob_len;
	blocks[2].id = 3; blocks[2].data = s->tls_cert;            blocks[2].len = s->tls_cert_len;
	blocks[3].id = 5; blocks[3].data = validity_crt_hardcoded; blocks[3].len = sizeof(validity_crt_hardcoded);
	blocks[4].id = 1; blocks[4].data = zeroes256;              blocks[4].len = 256;
	blocks[5].id = 2; blocks[5].data = zeroes256;              blocks[5].len = 256;
	blocks[6].id = 6; blocks[6].data = s->ecdh_blob;           blocks[6].len = s->ecdh_blob_len;

	for (int i = 0; i < 7; i++) {
		if (off + 36 + blocks[i].len > 0x1000)
			return false;
		val_put_le16(out + off, blocks[i].id);
		val_put_le16(out + off + 2, (uint16_t)blocks[i].len);
		if (!val_sha256(blocks[i].data, blocks[i].len, out + off + 4))
			return false;
		memcpy(out + off + 36, blocks[i].data, blocks[i].len);
		off += 36 + blocks[i].len;
	}
	memset(out + off, 0xff, 0x1000 - off);
	return true;
}

static inline int validity_init_flash_if_needed(struct validity *s, bool *did_reboot)
{
	static const uint8_t cmd3e = 0x3e, cmd01 = 0x01, cmd50 = 0x50;
	uint8_t rx[1024];
	size_t rx_len = 0;
	uint16_t jid0, jid1, blocks, blocksize, pcnt;
	const struct validity_flash_ic *ic = NULL;
	EC_KEY *skey = NULL, *hs_ec = NULL;
	BIGNUM *bx = NULL, *by = NULL;
	uint8_t x_be[32], y_be[32], d_be[32];
	uint8_t cmd[2048], flash_buf[0x1000];
	size_t off = 0;
	int r = -EIO;

	*did_reboot = false;
	if ((r = validity_cmd_status(s, &cmd3e, 1, rx, sizeof(rx), &rx_len)) < 0)
		return r;
	if (rx_len < 16)
		return -EPROTO;
	jid0 = val_le16(rx + 2);
	jid1 = val_le16(rx + 4);
	blocks = val_le16(rx + 6);
	blocksize = val_le16(rx + 10);
	pcnt = val_le16(rx + 14);
	if (pcnt > 0)
		return 0;

	VALIDITY_LOGI("Validity flash has 0 partitions; initializing flash");
	for (size_t i = 0; i < sizeof(validity_flash_ic_table) / sizeof(validity_flash_ic_table[0]); i++) {
		if (validity_flash_ic_table[i].jid0 == jid0 &&
		    validity_flash_ic_table[i].jid1 == jid1 &&
		    validity_flash_ic_table[i].size == (uint32_t)blocks * (uint32_t)blocksize) {
			ic = &validity_flash_ic_table[i];
			break;
		}
	}
	if (!ic) {
		VALIDITY_LOGE("Validity unknown flash IC %x:%x size %u", jid0, jid1,
			      (unsigned int)blocks * (unsigned int)blocksize);
		return -ENODEV;
	}

	/* reset_blob */
	{
		const uint8_t *rb = validity_is_0090(s) ? validity_reset_90 : validity_reset_97;
		size_t rblen = validity_is_0090(s) ? sizeof(validity_reset_90) : sizeof(validity_reset_97);

		if ((r = validity_cmd_status(s, rb, rblen, rx, sizeof(rx), &rx_len)) < 0)
			return r;
	}

	skey = EC_KEY_new_by_curve_name(VALIDITY_NID_P256);
	bx = BN_new();
	by = BN_new();
	if (!skey || !bx || !by || EC_KEY_generate_key(skey) != 1 ||
	    EC_POINT_get_affine_coordinates_GFp(EC_KEY_get0_group(skey), EC_KEY_get0_public_key(skey),
						bx, by, NULL) != 1 ||
	    !val_bn_to_be32(bx, x_be) || !val_bn_to_be32(by, y_be) ||
	    !val_bn_to_be32(EC_KEY_get0_private_key(skey), d_be)) {
		r = -EIO;
		goto out;
	}

	/* Build partition_flash command (0x4f) */
	cmd[0] = 0x4f; cmd[1] = 0; cmd[2] = 0; cmd[3] = 0; cmd[4] = 0;
	off = 5;
	/* chunk 0: flash params (12 bytes) */
	val_put_le16(cmd + off, 0); val_put_le16(cmd + off + 2, 12); off += 4;
	val_put_le32(cmd + off, ic->size);
	val_put_le32(cmd + off + 4, ic->sector_size);
	cmd[off + 8] = 0; cmd[off + 9] = 0;
	cmd[off + 10] = ic->sector_erase_cmd;
	cmd[off + 11] = 0;
	off += 12;

	/* chunk 1: 5 partitions (each 12 + 4 + 32 = 48 bytes) + 256-byte signature */
	{
		static const struct { uint8_t id, type; uint16_t acc; uint32_t off, sz; } parts_97[5] = {
			{ 1, 4, 7, 0x00001000, 0x00001000 },
			{ 2, 1, 2, 0x00002000, 0x0003e000 },
			{ 5, 5, 3, 0x00040000, 0x00008000 },
			{ 6, 6, 3, 0x00048000, 0x00008000 },
			{ 4, 3, 5, 0x00050000, 0x00080000 },
		};
		static const struct { uint8_t id, type; uint16_t acc; uint32_t off, sz; } parts_90[5] = {
			{ 1, 4, 7, 0x00001000, 0x00001000 },
			{ 2, 1, 2, 0x00002000, 0x0003e000 },
			{ 5, 5, 3, 0x00040000, 0x00008000 },
			{ 6, 6, 3, 0x00048000, 0x00008000 },
			{ 4, 3, 5, 0x00050000, 0x00030000 },
		};
		const uint8_t *psig = validity_is_0090(s) ? validity_part_sig_90 : validity_part_sig_97;

		val_put_le16(cmd + off, 1); val_put_le16(cmd + off + 2, 5 * 48 + 256); off += 4;
		for (int i = 0; i < 5; i++) {
			uint8_t pbuf[12];
			const uint8_t id = validity_is_0090(s) ? parts_90[i].id : parts_97[i].id;
			const uint8_t tp = validity_is_0090(s) ? parts_90[i].type : parts_97[i].type;
			const uint16_t ac = validity_is_0090(s) ? parts_90[i].acc : parts_97[i].acc;
			const uint32_t po = validity_is_0090(s) ? parts_90[i].off : parts_97[i].off;
			const uint32_t ps = validity_is_0090(s) ? parts_90[i].sz : parts_97[i].sz;

			pbuf[0] = id; pbuf[1] = tp;
			val_put_le16(pbuf + 2, ac);
			val_put_le32(pbuf + 4, po);
			val_put_le32(pbuf + 8, ps);
			memcpy(cmd + off, pbuf, 12);
			memset(cmd + off + 12, 0, 4);
			if (!val_sha256(pbuf, 12, cmd + off + 16)) {
				r = -EIO;
				goto out;
			}
			off += 48;
		}
		memcpy(cmd + off, psig, 256);
		off += 256;
	}

	/* chunk 5: client cert signed by hs_key() (444 bytes) */
	{
		uint8_t hs_seed[15 + 18], hs_d_le[32], hs_d_be[32];
		uint8_t msg[444], digest[32], sig[128];
		unsigned int sig_len = sizeof(sig);

		memcpy(hs_seed, "HS_KEY_PAIR_GEN", 15);
		memcpy(hs_seed + 15, validity_password_hardcoded + 16, 16);
		hs_seed[31] = 0xaa; hs_seed[32] = 0xaa;
		if (!validity_prf(validity_password_hardcoded, 16, hs_seed, sizeof(hs_seed), hs_d_le, 32)) {
			r = -EIO;
			goto out;
		}
		val_reverse_copy(hs_d_be, hs_d_le, 32);
		hs_ec = val_ec_key_from_priv_be32(hs_d_be);
		if (!hs_ec) {
			r = -EIO;
			goto out;
		}
		memset(msg, 0, sizeof(msg));
		val_put_le32(msg, 0x17);
		val_put_le32(msg + 4, 0x20);
		val_reverse_copy(msg + 8, x_be, 32);
		val_reverse_copy(msg + 8 + 32 + 0x24, y_be, 32);
		/* msg header before signature is 8 + 32 + 0x24 + 32 + 0x4c = 0xbc (188 bytes) */
		if (!val_sha256(msg, 0xbc, digest) ||
		    ECDSA_sign(0, digest, 32, sig, &sig_len, hs_ec) != 1 ||
		    0xbc + 4 + sig_len > sizeof(msg)) {
			r = -EIO;
			goto out;
		}
		val_put_le32(msg + 0xbc, sig_len);
		memcpy(msg + 0xc0, sig, sig_len);
		val_put_le16(cmd + off, 5); val_put_le16(cmd + off + 2, 444); off += 4;
		memcpy(cmd + off, msg, 444);
		off += 444;
	}

	/* chunk 3: crt_hardcoded */
	val_put_le16(cmd + off, 3); val_put_le16(cmd + off + 2, sizeof(validity_crt_hardcoded)); off += 4;
	memcpy(cmd + off, validity_crt_hardcoded, sizeof(validity_crt_hardcoded));
	off += sizeof(validity_crt_hardcoded);

	if ((r = validity_cmd_status(s, cmd, off, rx, sizeof(rx), &rx_len)) < 0)
		goto out;
	if (rx_len < 6) {
		r = -EPROTO;
		goto out;
	}
	{
		uint32_t crt_len = val_le32(rx + 2);

		if (6 + crt_len > rx_len || crt_len > sizeof(s->tls_cert)) {
			r = -EPROTO;
			goto out;
		}
		memcpy(s->tls_cert, rx + 6, crt_len);
		s->tls_cert_len = crt_len;
	}

	if ((r = validity_cmd_status(s, &cmd01, 1, rx, sizeof(rx), &rx_len)) < 0)
		goto out;
	r = validity_cmd_status(s, &cmd50, 1, rx, sizeof(rx), &rx_len);
	(void)validity_call_cleanups(s);
	if (r < 0)
		goto out;
	if (rx_len < 2 + 4 + 400) {
		r = -EPROTO;
		goto out;
	}
	if ((r = validity_tls_handle_ecdh(s, rx + rx_len - 400, 400)) < 0)
		goto out;

	/* Encrypt client private key into priv_blob (init_flash.py:encrypt_key) */
	{
		uint8_t plain[112], iv[16], cbuf[1 + 16 + 112 + 32];
		size_t enc_len = 0;
		unsigned int hlen = 0;

		val_reverse_copy(plain + 0, x_be, 32);
		val_reverse_copy(plain + 32, y_be, 32);
		val_reverse_copy(plain + 64, d_be, 32);
		memset(plain + 96, 16, 16);	/* PKCS#7 pad */
		VALIDITY_RANDOM_BUF(iv, 16);
		cbuf[0] = 0x02;
		memcpy(cbuf + 1, iv, 16);
		if (!val_aes256_cbc(true, s->psk_enc, iv, plain, sizeof(plain), cbuf + 17, &enc_len) ||
		    enc_len != sizeof(plain) ||
		    !HMAC(EVP_sha256(), s->psk_val, 32, cbuf + 1, 16 + enc_len, cbuf + 17 + enc_len, &hlen) ||
		    hlen != 32) {
			r = -EIO;
			goto out;
		}
		if ((r = validity_tls_handle_priv(s, cbuf, sizeof(cbuf))) < 0)
			goto out;
	}

	if ((r = validity_tls_open(s)) < 0)
		goto out;
	if ((r = validity_erase_flash(s, 1)) < 0 ||
	    (r = validity_erase_flash(s, 2)) < 0 ||
	    (r = validity_erase_flash(s, 5)) < 0 ||
	    (r = validity_erase_flash(s, 6)) < 0 ||
	    (r = validity_erase_flash(s, 4)) < 0)
		goto out;
	if (!val_make_tls_flash(s, flash_buf)) {
		r = -EIO;
		goto out;
	}
	if ((r = validity_write_flash(s, 1, 0, flash_buf, sizeof(flash_buf))) < 0)
		goto out;
	(void)validity_reboot(s);
	*did_reboot = true;
	r = 0;
out:
	BN_free(bx);
	BN_free(by);
	EC_KEY_free(skey);
	EC_KEY_free(hs_ec);
	return r;
}

static inline int validity_upload_fwext_if_needed(struct validity *s, bool *did_reboot)
{
	bool present = false;
	uint16_t maj = 0, min = 0;
	const char *fwname = validity_is_0090(s) ? "6_07f_Lenovo.xpfwext" : "6_07f_lenovo_mis_qm.xpfwext";
	char fwpath[200];
	uint8_t *buf = NULL;
	uint8_t cmd[10], rx[64];
	size_t rx_len = 0, flen = 0, body_len;
	int fd, r;

	*did_reboot = false;
	if ((r = validity_get_fw_info(s, 2, &present, &maj, &min)) < 0)
		return r;
	if (present) {
		VALIDITY_LOGI("Validity fwext %u.%u present on sensor", maj, min);
		return 0;
	}
	snprintf(fwpath, sizeof(fwpath), "%s/%s", s->data_dir, fwname);
	fd = open(fwpath, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		VALIDITY_LOGE("Validity sensor has no firmware (fwext); pair once in Windows/Linux or place %s in %s",
			      fwname, s->data_dir);
		return -ENOENT;
	}
	buf = malloc(512 * 1024);
	if (!buf) {
		close(fd);
		return -EIO;
	}
	for (;;) {
		long n = read(fd, buf + flen, 512 * 1024 - flen);

		if (n < 0) {
			close(fd);
			free(buf);
			return -VALIDITY_ERRNO;
		}
		if (n == 0)
			break;
		flen += (size_t)n;
		if (flen == 512 * 1024)
			break;
	}
	close(fd);

	/* upload_fwext.py: write_hw_reg32(0x8000205c, 7), check reg 0x80002080 in [2,3] */
	cmd[0] = 8; val_put_le32(cmd + 1, 0x8000205cU); val_put_le32(cmd + 5, 7U); cmd[9] = 4;
	if ((r = validity_cmd_status(s, cmd, 10, rx, sizeof(rx), &rx_len)) < 0) {
		free(buf);
		return r;
	}
	cmd[0] = 7; val_put_le32(cmd + 1, 0x80002080U); cmd[5] = 4;
	if ((r = validity_cmd_status(s, cmd, 6, rx, sizeof(rx), &rx_len)) < 0 || rx_len < 6) {
		free(buf);
		return r < 0 ? r : -EPROTO;
	}
	{
		uint32_t rv = val_le32(rx + 2);

		if (rv != 2 && rv != 3) {
			free(buf);
			return -EPROTO;
		}
	}

	/* Strip up to first 0x1a byte, trailing 0x100 bytes are signature */
	{
		size_t hdr_end = 0;

		while (hdr_end < flen && buf[hdr_end] != 0x1a)
			hdr_end++;
		if (hdr_end >= flen || flen - (hdr_end + 1) <= 0x100) {
			free(buf);
			return -EINVAL;
		}
		body_len = flen - (hdr_end + 1) - 0x100;
		VALIDITY_LOGI("Uploading %s (%zu bytes) to Validity sensor", fwname, body_len);
		if ((r = validity_write_flash_all(s, 2, 0, buf + hdr_end + 1, body_len)) < 0) {
			free(buf);
			return r;
		}
		{
			uint8_t sig_cmd[5 + 0x100];

			sig_cmd[0] = 0x42;
			sig_cmd[1] = 2;
			sig_cmd[2] = 0;
			val_put_le16(sig_cmd + 3, 0x100);
			memcpy(sig_cmd + 5, buf + hdr_end + 1 + body_len, 0x100);
			r = validity_cmd_status(s, sig_cmd, sizeof(sig_cmd), rx, sizeof(rx), &rx_len);
		}
	}
	free(buf);
	if (r < 0)
		return r;
	if ((r = validity_get_fw_info(s, 2, &present, &maj, &min)) < 0 || !present)
		return r < 0 ? r : -EIO;
	VALIDITY_LOGI("Uploaded Validity fwext %u.%u; rebooting sensor", maj, min);
	(void)validity_reboot(s);
	*did_reboot = true;
	return 0;
}

/* ---- Timeslot instruction decoder & cmd_02 builder (timeslot.py + sensor.py) ---- */

struct val_insn {
	int opcode;
	size_t len;
	uint32_t op0;
	uint32_t op1;
};

static inline int val_decode_insn(const uint8_t *b, size_t avail, struct val_insn *out)
{
	uint8_t c;

	if (avail == 0)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	c = b[0];
	if (c <= 4) {
		out->opcode = c;
		out->len = 1;
	} else if (c == 5 || c == 6) {
		if (avail < 2)
			return -EINVAL;
		out->opcode = c;
		out->len = 2;
		out->op0 = b[1];
	} else if (c == 7) {
		if (avail < 2)
			return -EINVAL;
		out->opcode = 7;
		out->len = 2;
		out->op0 = b[1] == 0 ? 0x100U : b[1];
	} else if ((c & 0xfe) == 0x08 || (c & 0xfe) == 0x0a) {
		if (avail < 2)
			return -EINVAL;
		out->opcode = (c & 0xfe) == 0x08 ? 8 : 9;
		out->len = 2;
		out->op0 = ((uint32_t)(c & 1) << 8) | b[1];
	} else if ((c & 0xfc) == 0x0c) {
		out->opcode = 10;
		out->len = 1;
		out->op0 = c & 3;
	} else if ((c & 0xf8) == 0x10) {
		if (avail < 3)
			return -EINVAL;
		out->opcode = 11;
		out->len = 3;
		out->op0 = c & 7;
		out->op1 = (uint32_t)b[1] << 2;
	} else if ((c & 0xe0) == 0x20) {
		out->opcode = 12;
		out->len = 1;
		out->op0 = c & 0x1f;
	} else if ((c & 0xc0) == 0x40) {
		if (avail < 3)
			return -EINVAL;
		out->opcode = 13;
		out->len = 3;
		out->op0 = (uint32_t)(c & 0x3f) * 4U + 0x80002000U;
		out->op1 = (uint32_t)b[1] | ((uint32_t)b[2] << 8);
	} else if ((c & 0xc0) == 0x80) {
		out->opcode = 14;
		out->len = 1;
		out->op0 = (c & 0x38) >> 3;
		out->op1 = c & 7;
	} else {
		if (avail < 2)
			return -EINVAL;
		out->opcode = 15;
		out->len = 2;
		out->op0 = (c & 0x38) >> 3;
		out->op1 = c & 7;
	}
	return 0;
}

static inline int val_find_nth_insn(const uint8_t *b, size_t len, int opcode, int n, size_t *pc_out)
{
	size_t pc = 0;

	while (pc < len) {
		struct val_insn insn;

		if (val_decode_insn(b + pc, len - pc, &insn) < 0)
			return -EINVAL;
		if (insn.opcode == opcode && --n == 0) {
			*pc_out = pc;
			return 0;
		}
		pc += insn.len;
	}
	return -ENOENT;
}

static inline int val_find_nth_regwrite(const uint8_t *b, size_t len, uint32_t reg_addr, int n,
					size_t *pc_out)
{
	size_t pc = 0;

	while (pc < len) {
		struct val_insn insn;

		if (val_decode_insn(b + pc, len - pc, &insn) < 0)
			return -EINVAL;
		if (insn.opcode == 13 && insn.op0 == reg_addr && --n == 0) {
			*pc_out = pc;
			return 0;
		}
		pc += insn.len;
	}
	return -ENOENT;
}

static inline void val_patch_timeslot_table(uint8_t *b, size_t len, bool inc_addr, uint8_t mult)
{
	size_t i = 0;

	while (i + 3 < len) {
		if ((b[i] & 0xf8) == 0x10) {
			if (b[i + 2] > 1) {
				b[i + 2] = (uint8_t)(b[i + 2] * mult);
				if (inc_addr)
					b[i + 1] = (uint8_t)(b[i + 1] + 1);
			}
			i += 3;
		} else if (b[i] == 0) {
			i += 1;
		} else if (b[i] == 7) {
			i += 2;
		} else {
			break;
		}
	}
}

static inline void val_patch_timeslot_again(const struct validity *s, uint8_t *b, size_t len)
{
	size_t pc = 0;
	int match = -1;

	while (pc < len) {
		struct val_insn insn;

		if (val_decode_insn(b + pc, len - pc, &insn) < 0)
			return;
		if (insn.opcode == 1 || insn.opcode == 2 || insn.opcode == 4)
			break;
		if (insn.opcode == 11)
			match = (int)insn.op1;
		pc += insn.len;
	}
	if (match < 0 || (size_t)match >= len)
		return;
	pc = (size_t)match;
	match = -1;
	while (pc < len) {
		struct val_insn insn;

		if (val_decode_insn(b + pc, len - pc, &insn) < 0)
			return;
		if (insn.opcode == 1 || insn.opcode == 2 || insn.opcode == 4)
			break;
		if (insn.opcode == 13 && insn.op0 == 0x8000203cU)
			match = (int)pc;
		pc += insn.len;
	}
	if (match >= 0 && (size_t)match + 1 < len &&
	    s->type_info->key_calib_line < s->factory_calib_values_len)
		b[match + 1] = s->factory_calib_values[s->type_info->key_calib_line];
}

/* Matches sensor.py:bitpack */
static inline void val_bitpack(const uint8_t *in, size_t len, uint8_t *u_out, uint8_t *min_out,
			       uint8_t *out, size_t *out_len)
{
	uint8_t mn = 255, mx = 0, u = 0;
	unsigned int delta;
	size_t bytes;

	for (size_t i = 0; i < len; i++) {
		if (in[i] < mn)
			mn = in[i];
		if (in[i] > mx)
			mx = in[i];
	}
	delta = (unsigned int)(mx - mn);
	while (delta > 0) {
		delta >>= 1;
		u++;
	}
	if (u == 0)
		u = 1;
	bytes = ((size_t)u * len + 7) / 8;
	memset(out, 0, bytes);
	for (size_t i = 0; i < len; i++) {
		unsigned int v = (unsigned int)(in[i] - mn);

		for (unsigned int bit = 0; bit < u; bit++) {
			size_t pos = i * (size_t)u + bit;

			if ((v >> bit) & 1U)
				out[pos >> 3] |= (uint8_t)(1U << (pos & 7U));
		}
	}
	*u_out = u;
	*min_out = mn;
	*out_len = bytes;
}

static inline int val_append_chunk(uint8_t *out, size_t cap, size_t *off,
				   uint16_t key, const uint8_t *val, size_t len)
{
	if (*off + 4 + len > cap)
		return -EINVAL;
	val_put_le16(out + *off, key);
	val_put_le16(out + *off + 2, (uint16_t)len);
	if (len)
		memcpy(out + *off + 4, val, len);
	*off += 4 + len;
	return 0;
}

/* Exact C port of sensor.py:build_cmd_02 for type 0x0199 (type_1) and 0x00db (type_2) */
static inline int validity_build_cmd_02(const struct validity *s, int mode,
					uint8_t *out, size_t cap, size_t *out_len)
{
	const struct validity_sensor_type_info *ti = s->type_info;
	uint8_t tst[1024];
	size_t tst_len = 0, off = 5, p = 0;
	uint16_t req_lines;
	int r;

	if (!ti || s->rom.product != 0x30 || cap < 5)
		return -EINVAL;

	req_lines = mode == VALIDITY_MODE_CALIBRATE
		? (uint16_t)(ti->calib_frames * s->lines_per_frame + 1)
		: 0;
	out[0] = 0x02;
	val_put_le16(out + 1, ti->bytes_per_line);
	val_put_le16(out + 3, req_lines);

	/* Copy chunks from capture_prog, patching chunk 0x34 in place */
	while (p + 4 <= ti->capture_prog_len) {
		uint16_t k = val_le16(ti->capture_prog + p);
		uint16_t sz = val_le16(ti->capture_prog + p + 2);
		const uint8_t *v = ti->capture_prog + p + 4;

		if (p + 4 + sz > ti->capture_prog_len)
			return -EPROTO;
		if (k == 0x34) {
			uint8_t patched[1024];

			if (sz > sizeof(tst) || sz < ti->line_width)
				return -EPROTO;
			memcpy(tst, v, sz);
			tst_len = sz;
			val_patch_timeslot_table(tst, tst_len, true, ti->repeat_multiplier);
			if (mode != VALIDITY_MODE_CALIBRATE)
				val_patch_timeslot_again(s, tst, tst_len);
			memcpy(patched, tst, tst_len);
			if (ti->sensor_type == 0x0199) {
				/* get_key_line() replaces first line_width bytes of chunk 0x34 */
				if (s->calib_data_len > 0) {
					size_t bpl = s->calib_data_len / ti->lines_per_calib;
					size_t koff = 8 + bpl * ti->key_calib_line;

					if (koff + ti->line_width > s->calib_data_len)
						return -EPROTO;
					for (size_t i = 0; i < ti->line_width; i++) {
						uint8_t b = s->calib_data[koff + i];
						patched[i] = b == 5 ? 4 : b;
					}
				} else {
					memset(patched, 0, ti->line_width);
				}
			}
			if ((r = val_append_chunk(out, cap, &off, 0x34, patched, tst_len)) < 0)
				return r;
		} else {
			if ((r = val_append_chunk(out, cap, &off, k, v, sz)) < 0)
				return r;
		}
		p += 4 + sz;
	}

	/* Reply Configuration (0x17, empty) */
	if ((r = val_append_chunk(out, cap, &off, 0x17, NULL, 0)) < 0)
		return r;

	if (ti->sensor_type == 0x0199) {
		/* line_update_type_1 */
		static const uint8_t id_4e[40] = {
			0xfb, 0xb2, 0x0f, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x30, 0x00, 0x00, 0x00, 0x87, 0x00, 0x02, 0x00,
			0x67, 0x00, 0x0a, 0x00, 0x01, 0x80, 0x00, 0x00, 0x0a, 0x02, 0x00, 0x00, 0x0b, 0x19, 0x00, 0x00,
			0x88, 0x13, 0xb8, 0x0b, 0x01, 0x09, 0x10, 0x00,
		};
		static const uint8_t id_2e[28] = {
			0x02, 0x00, 0x18, 0x00, 0x02, 0x00, 0x00, 0x00, 0x70, 0x00, 0x70, 0x00, 0x4d, 0x01, 0x00, 0x00,
			0xa0, 0x00, 0x8c, 0x00, 0x3c, 0x32, 0x32, 0x1e, 0x3c, 0x0a, 0x02, 0x02,
		};
		static const uint8_t en_26[40] = {
			0xfb, 0xb2, 0x0f, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x30, 0x00, 0x00, 0x00, 0x87, 0x00, 0x02, 0x00,
			0x67, 0x00, 0x0a, 0x00, 0x01, 0x80, 0x00, 0x00, 0x0a, 0x02, 0x00, 0x00, 0x0b, 0x19, 0x00, 0x00,
			0x50, 0xc3, 0x60, 0xea, 0x01, 0x09, 0x10, 0x00,
		};
		static const uint8_t en_2e[28] = {
			0x02, 0x00, 0x18, 0x00, 0x23, 0x00, 0x00, 0x00, 0x70, 0x00, 0x70, 0x00, 0x4d, 0x01, 0x00, 0x00,
			0xa0, 0x00, 0x8c, 0x00, 0x3c, 0x32, 0x32, 0x1e, 0x3c, 0x0a, 0x02, 0x02,
		};
		uint8_t interleave[4] = { 1, 0, 0, 0 };
		size_t pc_rx = 0, pc_rw = 0, bp_len = 0, bp_padded, cb_padded;
		uint8_t u = 0, mn = 0, bp_buf[VALIDITY_FACTORY_MAX + 4];
		uint32_t nlines = s->calib_data_len > 0 ? 30U : 2U;
		size_t lu_len, ut_len;

		if (mode == VALIDITY_MODE_IDENTIFY) {
			if ((r = val_append_chunk(out, cap, &off, 0x4e, id_4e, sizeof(id_4e))) < 0 ||
			    (r = val_append_chunk(out, cap, &off, 0x2e, id_2e, sizeof(id_2e))) < 0)
				return r;
		} else if (mode == VALIDITY_MODE_ENROLL) {
			if ((r = val_append_chunk(out, cap, &off, 0x26, en_26, sizeof(en_26))) < 0 ||
			    (r = val_append_chunk(out, cap, &off, 0x2e, en_2e, sizeof(en_2e))) < 0)
				return r;
		}
		if ((r = val_append_chunk(out, cap, &off, 0x44, interleave, 4)) < 0)
			return r;

		if (val_find_nth_insn(tst, tst_len, 6, 2, &pc_rx) < 0 ||
		    val_find_nth_regwrite(tst, tst_len, 0x8000203cU, 1, &pc_rw) < 0)
			return -EPROTO;

		val_bitpack(s->factory_calib_values, s->factory_calib_values_len, &u, &mn, bp_buf, &bp_len);
		bp_padded = (bp_len + 3) & ~(size_t)3;
		if (bp_padded > bp_len)
			memset(bp_buf + bp_len, 0, bp_padded - bp_len);
		cb_padded = (ti->calib_blob_len + 3) & ~(size_t)3;

		/* Chunk 0x30: Line Update */
		lu_len = 4 + (size_t)nlines * 8 + (s->calib_data_len > 0 ? 28U * 112U * 4U : 0U);
		if (off + 4 + lu_len > cap)
			return -EINVAL;
		val_put_le16(out + off, 0x30);
		val_put_le16(out + off + 2, (uint16_t)lu_len);
		off += 4;
		val_put_le32(out + off, nlines);
		off += 4;
		/* Line 0: Enable Rx */
		val_put_le32(out + off, 0xffU);
		val_put_le32(out + off + 4, (uint32_t)(pc_rx + 1) | (2U << 20) | 0x07000000U);
		off += 8;
		/* Line 1: Write Register 0x8000203c */
		val_put_le32(out + off, 0xffU);
		val_put_le32(out + off + 4, (uint32_t)(pc_rw + 1) | (3U << 20) | 0x07000000U);
		off += 8;
		if (s->calib_data_len > 0) {
			size_t bpl = s->calib_data_len / ti->lines_per_calib;

			for (uint32_t i = 0; i < 112; i += 4) {
				val_put_le32(out + off, 0xffffffffU);
				val_put_le32(out + off + 4, i | (0x85U << 24));
				off += 8;
			}
			for (size_t i = 0; i < 112; i += 4) {
				for (size_t j = 0; j < 112; j++) {
					size_t pos = 8 + j * bpl + i;

					if (pos + 4 > s->calib_data_len)
						return -EPROTO;
					memcpy(out + off, s->calib_data + pos, 4);
					off += 4;
				}
			}
		}

		/* Chunk 0x43: Line Update Transform (lines 0 and 1) */
		ut_len = 4 + cb_padded + 4 + bp_padded;
		if (off + 4 + ut_len > cap)
			return -EINVAL;
		val_put_le16(out + off, 0x43);
		val_put_le16(out + off + 2, (uint16_t)ut_len);
		off += 4;
		out[off++] = 0x0f; out[off++] = 0x00; out[off++] = 0x00; out[off++] = 0x00;
		memcpy(out + off, ti->calib_blob, ti->calib_blob_len);
		if (cb_padded > ti->calib_blob_len)
			memset(out + off + ti->calib_blob_len, 0, cb_padded - ti->calib_blob_len);
		off += cb_padded;
		out[off++] = (uint8_t)((u - 1) | 8); out[off++] = mn; out[off++] = 0x00; out[off++] = 0x00;
		memcpy(out + off, bp_buf, bp_padded);
		off += bp_padded;
	} else {
		/* line_update_type_2 (0x00db / 138a:0090) */
		static const uint8_t id_4e[40] = {
			0xfb, 0xb2, 0x0f, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x30, 0x00, 0x00, 0x00, 0x60, 0x01, 0x02, 0x00,
			0x40, 0x01, 0x0a, 0x00, 0x01, 0x80, 0x00, 0x00, 0x0a, 0x02, 0x00, 0x00, 0x0b, 0x19, 0x00, 0x00,
			0x88, 0x13, 0xb8, 0x0b, 0x01, 0x09, 0x10, 0x00,
		};
		static const uint8_t id_2e[28] = {
			0x02, 0x00, 0x18, 0x00, 0x02, 0x00, 0x00, 0x00, 0x90, 0x00, 0x90, 0x00, 0x4d, 0x01, 0x00, 0x00,
			0x90, 0x01, 0x7c, 0x01, 0x3c, 0x32, 0x32, 0x32, 0x64, 0x0a, 0x02, 0x01,
		};
		static const uint8_t en_26[40] = {
			0xfb, 0xb2, 0x0f, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x30, 0x00, 0x00, 0x00, 0x60, 0x01, 0x02, 0x00,
			0x40, 0x01, 0x0a, 0x00, 0x01, 0x80, 0x00, 0x00, 0x0a, 0x02, 0x00, 0x00, 0x0b, 0x19, 0x00, 0x00,
			0x50, 0xc3, 0x60, 0xea, 0x01, 0x09, 0x10, 0x00,
		};
		static const uint8_t en_2e[28] = {
			0x02, 0x00, 0x18, 0x00, 0x23, 0x00, 0x00, 0x00, 0x90, 0x00, 0x90, 0x00, 0x4d, 0x01, 0x00, 0x00,
			0x90, 0x01, 0x7c, 0x01, 0x3c, 0x32, 0x32, 0x32, 0x64, 0x0a, 0x02, 0x01,
		};
		size_t pc_rx = 0, pc_fc = 0, pc_3c = 0;
		size_t p0 = (ti->calib_blob_len + 3) & ~(size_t)3;
		size_t p1 = (s->factory_calib_data_len + 3) & ~(size_t)3;
		size_t p2 = (s->factory_calib_values_len + 3) & ~(size_t)3;
		size_t lu_len = 4 + 3 * 8 + p0 + p1 + p2;

		if (mode == VALIDITY_MODE_IDENTIFY) {
			if ((r = val_append_chunk(out, cap, &off, 0x4e, id_4e, sizeof(id_4e))) < 0 ||
			    (r = val_append_chunk(out, cap, &off, 0x2e, id_2e, sizeof(id_2e))) < 0)
				return r;
		} else if (mode == VALIDITY_MODE_ENROLL) {
			if ((r = val_append_chunk(out, cap, &off, 0x26, en_26, sizeof(en_26))) < 0 ||
			    (r = val_append_chunk(out, cap, &off, 0x2e, en_2e, sizeof(en_2e))) < 0)
				return r;
		}
		if (val_find_nth_insn(tst, tst_len, 6, 2, &pc_rx) < 0 ||
		    val_find_nth_regwrite(tst, tst_len, 0x800020fcU, 1, &pc_fc) < 0 ||
		    val_find_nth_regwrite(tst, tst_len, 0x8000203cU, 1, &pc_3c) < 0)
			return -EPROTO;
		if (off + 4 + lu_len > cap)
			return -EINVAL;
		val_put_le16(out + off, 0x30);
		val_put_le16(out + off + 2, (uint16_t)lu_len);
		off += 4;
		val_put_le32(out + off, 3U);
		off += 4;
		val_put_le32(out + off, 0xffU); val_put_le32(out + off + 4, (uint32_t)(pc_rx + 1) | 0x03000000U); off += 8;
		val_put_le32(out + off, 0xffU); val_put_le32(out + off + 4, (uint32_t)(pc_fc + 1) | 0x03000000U); off += 8;
		val_put_le32(out + off, 0xffU); val_put_le32(out + off + 4, (uint32_t)(pc_3c + 1) | 0x03000000U); off += 8;

		memcpy(out + off, ti->calib_blob, ti->calib_blob_len);
		memset(out + off + ti->calib_blob_len, 0, p0 - ti->calib_blob_len);
		off += p0;
		memcpy(out + off, s->factory_calib_data, s->factory_calib_data_len);
		memset(out + off + s->factory_calib_data_len, 0, p1 - s->factory_calib_data_len);
		off += p1;
		memcpy(out + off, s->factory_calib_values, s->factory_calib_values_len);
		memset(out + off + s->factory_calib_values_len, 0, p2 - s->factory_calib_values_len);
		off += p2;
	}

	*out_len = off;
	return 0;
}

/* ---- Calibration (sensor.py:average, process_calibration_results, calibrate) ---- */

static inline uint8_t val_clip(int x)
{
	if (x < -128)
		x = -128;
	if (x > 127)
		x = 127;
	return (uint8_t)(x & 0xff);
}

static inline uint8_t val_scale(uint8_t x)
{
	int v = ((int)x - 0x80) * 10 / 0x22;

	return val_clip(v);
}

static inline uint8_t val_add_signed(uint8_t l, uint8_t r)
{
	return val_clip((int)(int8_t)l + (int)(int8_t)r);
}

static inline int validity_average_calib(const struct validity *s, const uint8_t *raw, size_t raw_len,
					 uint8_t *out, size_t *out_len)
{
	const struct validity_sensor_type_info *ti = s->type_info;
	size_t frame_size = (size_t)s->lines_per_frame * ti->bytes_per_line;
	size_t interleave = (size_t)s->lines_per_frame / ti->lines_per_calib;
	size_t input_frames = ti->calib_frames;
	size_t base = 0;

	if (interleave > 1) {
		size_t out_lines = ti->lines_per_calib;

		if (input_frames > 1) {
			input_frames -= 1;
			base = frame_size;
		}
		if (base + frame_size > raw_len)
			return -EPROTO;
		for (size_t line = 0; line < out_lines; line++) {
			for (size_t col = 0; col < ti->bytes_per_line; col++) {
				unsigned int sum = 0;

				for (size_t k = 0; k < interleave; k++)
					sum += raw[base + (line * interleave + k) * ti->bytes_per_line + col];
				out[line * ti->bytes_per_line + col] = (uint8_t)(sum / interleave);
			}
		}
		*out_len = out_lines * ti->bytes_per_line;
		return 0;
	}
	if (input_frames > 1) {
		input_frames -= 2;
		base = frame_size * 2;
	}
	if (input_frames == 0 || base + frame_size * input_frames > raw_len)
		return -EPROTO;
	for (size_t idx = 0; idx < frame_size; idx++) {
		unsigned int sum = 0;

		for (size_t f = 0; f < input_frames; f++)
			sum += raw[base + f * frame_size + idx];
		out[idx] = (uint8_t)(sum / input_frames);
	}
	*out_len = frame_size;
	return 0;
}

static inline void validity_process_calib(struct validity *s, const uint8_t *cooked, size_t len)
{
	size_t bpl = s->type_info->bytes_per_line;
	size_t lines = len / bpl;

	if (s->calib_data_len == 0) {
		for (size_t l = 0; l < lines; l++) {
			size_t base = l * bpl;

			memcpy(s->calib_data + base, cooked + base, 8);
			for (size_t c = 8; c < bpl; c++)
				s->calib_data[base + c] = val_scale(cooked[base + c]);
		}
		s->calib_data_len = len;
	} else {
		for (size_t l = 0; l < lines; l++) {
			size_t base = l * bpl;

			for (size_t c = 8; c < bpl; c++)
				s->calib_data[base + c] = val_add_signed(s->calib_data[base + c],
									 val_scale(cooked[base + c]));
		}
	}
}

static inline bool validity_check_clean_slate(struct validity *s)
{
	uint8_t start[0x44], calc[32];
	uint8_t *img;
	size_t got = 0;
	uint16_t magic, len;

	if (validity_read_flash(s, 6, 0, sizeof(start), start, &got) < 0 || got != sizeof(start))
		return false;
	magic = val_le16(start);
	len = val_le16(start + 2);
	if (magic != 0x5002 || len == 0 || len > VALIDITY_CALIB_MAX + 16)
		return false;
	for (size_t i = 0x24; i < 0x44; i++) {
		if (start[i] != 0)
			return false;
	}
	img = malloc(len);
	if (!img)
		return false;
	if (validity_read_flash_all(s, 6, 0x44, len, img) < 0 ||
	    !val_sha256(img, len, calc) ||
	    CRYPTO_memcmp(calc, start + 4, 32) != 0) {
		free(img);
		return false;
	}
	free(img);
	return true;
}

static inline int validity_calibrate(struct validity *s)
{
	char cpath[160];
	int fd, r;
	size_t expected_calib = (size_t)s->type_info->lines_per_calib * s->type_info->bytes_per_line;
	size_t raw_cap = (size_t)(s->type_info->calib_frames * s->lines_per_frame + 1) * s->type_info->bytes_per_line;
	uint8_t *cmd = NULL, *raw = NULL, *cooked = NULL, *cs = NULL, *rx = NULL;
	const size_t rx_cap = 8192;
	size_t cmd_len = 0, rx_len = 0, raw_len = 0, cooked_len = 0;

	if (s->type_info->calib_iterations == 0) {
		s->calib_data_len = 0;
		if (validity_check_clean_slate(s)) {
			VALIDITY_LOGI("Validity calibration already valid on flash partition 6");
			return 0;
		}
	}
	snprintf(cpath, sizeof(cpath), "%s/validity-calib.bin", s->data_dir);
	fd = open(cpath, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		long n = read(fd, s->calib_data, sizeof(s->calib_data));
		close(fd);
		if (n == (long)expected_calib) {
			s->calib_data_len = (size_t)n;
			if (validity_check_clean_slate(s)) {
				VALIDITY_LOGI("Validity calibration loaded from %s", cpath);
				return 0;
			}
		}
	}

	VALIDITY_LOGI("Calibrating Validity sensor (type 0x%04x, %u iterations, lpf=%u, raw_cap=%zu)...",
		      s->type_info->sensor_type, s->type_info->calib_iterations,
		      s->lines_per_frame, raw_cap);
	s->calib_data_len = 0;
	cmd = malloc(VALIDITY_IO_BUF_MAX);
	raw = malloc(raw_cap + 4096);
	cooked = malloc(VALIDITY_CALIB_MAX);
	cs = malloc(0x44 + VALIDITY_CALIB_MAX + 4);
	rx = malloc(rx_cap);
	if (!cmd || !raw || !cooked || !cs || !rx) {
		r = -EIO;
		goto out;
	}

	for (unsigned int iter = 0; iter < s->type_info->calib_iterations; iter++) {
		if ((r = validity_build_cmd_02(s, VALIDITY_MODE_CALIBRATE, cmd, VALIDITY_IO_BUF_MAX, &cmd_len)) < 0)
			goto out;
		if ((r = validity_cmd_status(s, cmd, cmd_len, rx, rx_cap, &rx_len)) < 0)
			goto out;
		if ((r = validity_usb_bulk_read(s, VALIDITY_EP_CAPTURE, raw, raw_cap, &raw_len, 10000)) < 0)
			goto out;
		if ((r = validity_average_calib(s, raw, raw_len, cooked, &cooked_len)) < 0)
			goto out;
		validity_process_calib(s, cooked, cooked_len);
	}

	/* Blank clean-slate image */
	if ((r = validity_build_cmd_02(s, VALIDITY_MODE_CALIBRATE, cmd, VALIDITY_IO_BUF_MAX, &cmd_len)) < 0) {
		VALIDITY_LOGE("Validity calib build_cmd_02 failed: %d", r);
		goto out;
	}
	if ((r = validity_cmd_status(s, cmd, cmd_len, rx, rx_cap, &rx_len)) < 0) {
		VALIDITY_LOGE("Validity calib cmd_02 failed: %d (cmd_len=%zu rx_len=%zu)", r, cmd_len, rx_len);
		goto out;
	}
	if ((r = validity_usb_bulk_read(s, VALIDITY_EP_CAPTURE, raw, raw_cap, &raw_len, 10000)) < 0) {
		VALIDITY_LOGE("Validity calib EP 0x82 read failed: %d (raw_cap=%zu)", r, raw_cap);
		goto out;
	}
	if ((r = validity_average_calib(s, raw, raw_len, cooked, &cooked_len)) < 0) {
		VALIDITY_LOGE("Validity calib average failed: %d (raw_len=%zu raw_cap=%zu)", r, raw_len, raw_cap);
		goto out;
	}

	{
		size_t body_len = 2 + cooked_len + 2;
		uint8_t start[0x44];
		size_t got = 0;

		val_put_le16(cs, 0x5002);
		val_put_le16(cs + 2, (uint16_t)body_len);
		memset(cs + 0x24, 0, 0x20);
		val_put_le16(cs + 0x44, (uint16_t)cooked_len);
		memcpy(cs + 0x46, cooked, cooked_len);
		val_put_le16(cs + 0x46 + cooked_len, 0);
		if (!val_sha256(cs + 0x44, body_len, cs + 4)) {
			r = -EIO;
			goto out;
		}
		if (validity_read_flash(s, 6, 0, sizeof(start), start, &got) == 0 && got == sizeof(start)) {
			bool all_ff = true;

			for (size_t i = 0; i < sizeof(start); i++)
				if (start[i] != 0xff)
					all_ff = false;
			if (!all_ff && memcmp(start, cs, sizeof(start)) != 0) {
				if ((r = validity_erase_flash(s, 6)) < 0)
					goto out;
			}
			if (all_ff || memcmp(start, cs, sizeof(start)) != 0) {
				if ((r = validity_write_flash_all(s, 6, 0, cs, 0x44 + body_len)) < 0)
					goto out;
			}
		} else {
			if ((r = validity_erase_flash(s, 6)) < 0 ||
			    (r = validity_write_flash_all(s, 6, 0, cs, 0x44 + body_len)) < 0)
				goto out;
		}
	}

	/* Save calib-data.bin atomically */
	if (s->calib_data_len > 0) {
		char tmp[180];

		snprintf(tmp, sizeof(tmp), "%s.tmp", cpath);
		fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (fd >= 0) {
			bool ok = write(fd, s->calib_data, s->calib_data_len) == (long)s->calib_data_len &&
				  fsync(fd) == 0;
			close(fd);
			if (ok)
				(void)rename(tmp, cpath);
		}
	}
	r = 0;
out:
	free(cmd);
	free(raw);
	free(cooked);
	free(cs);
	free(rx);
	return r;
}

static inline int validity_sensor_open(struct validity *s)
{
	static const uint8_t cmd75 = 0x75, cmd01 = 0x01;
	uint8_t cmd6f[9], rx[2048];
	size_t rx_len = 0, p = 0;
	uint16_t minor, major, stype = 0;
	bool found = false;
	int r;

	/* identify_sensor (0x75) */
	if ((r = validity_cmd_status(s, &cmd75, 1, rx, sizeof(rx), &rx_len)) < 0 || rx_len < 10)
		return r < 0 ? r : -EPROTO;
	if (val_le32(rx + 2) != 0)
		return -EPROTO;
	minor = val_le16(rx + 6);
	major = val_le16(rx + 8);
	for (size_t i = 0; i < sizeof(validity_dev_info_table) / sizeof(validity_dev_info_table[0]); i++) {
		const struct validity_dev_info *di = &validity_dev_info_table[i];
		uint8_t mver;

		if (di->major != major)
			continue;
		mver = (uint8_t)(di->version & di->version_mask);
		if (minor == 0 || mver == 0) {
			stype = di->type;
			found = true;
		} else if (minor == mver) {
			stype = di->type;
			found = true;
			break;
		}
	}
	if (!found) {
		VALIDITY_LOGE("Validity unknown sensor major=0x%04x minor=0x%04x", major, minor);
		return -ENODEV;
	}
	s->type_info = NULL;
	for (size_t i = 0; i < sizeof(validity_sensor_types) / sizeof(validity_sensor_types[0]); i++) {
		if (validity_sensor_types[i].sensor_type == stype) {
			s->type_info = &validity_sensor_types[i];
			break;
		}
	}
	if (!s->type_info) {
		VALIDITY_LOGE("Validity sensor type 0x%04x not supported", stype);
		return -ENODEV;
	}

	/* RomInfo (0x01) */
	if ((r = validity_cmd_status(s, &cmd01, 1, rx, sizeof(rx), &rx_len)) < 0 || rx_len < 18)
		return r < 0 ? r : -EPROTO;
	s->rom.timestamp = val_le32(rx + 2);
	s->rom.build = val_le32(rx + 6);
	s->rom.major = rx[10];
	s->rom.minor = rx[11];
	s->rom.product = rx[13];
	s->rom.u1 = rx[17];
	snprintf(s->fw_version, sizeof(s->fw_version), "rom%u.%u.%u", s->rom.major, s->rom.minor, s->rom.build);

	/* Extract lines_2d from chunk 0x2f in capture_prog */
	s->lines_per_frame = 0;
	while (p + 4 <= s->type_info->capture_prog_len) {
		uint16_t k = val_le16(s->type_info->capture_prog + p);
		uint16_t sz = val_le16(s->type_info->capture_prog + p + 2);

		if (k == 0x2f && sz >= 4 && p + 4 + sz <= s->type_info->capture_prog_len) {
			s->lines_per_frame = (uint16_t)(val_le32(s->type_info->capture_prog + p + 4) *
							s->type_info->repeat_multiplier);
			break;
		}
		p += 4 + sz;
	}
	if (!s->lines_per_frame)
		return -EPROTO;

	/* get_factory_bits(0x0e00) */
	cmd6f[0] = 0x6f;
	val_put_le16(cmd6f + 1, 0x0e00);
	val_put_le16(cmd6f + 3, 0);
	val_put_le32(cmd6f + 5, 0);
	if ((r = validity_cmd_status(s, cmd6f, sizeof(cmd6f), rx, sizeof(rx), &rx_len)) < 0 || rx_len < 10)
		return r < 0 ? r : -EPROTO;
	{
		uint32_t entries = val_le32(rx + 6);
		size_t off = 10;

		s->factory_calib_values_len = 0;
		s->factory_calib_data_len = 0;
		for (uint32_t i = 0; i < entries; i++) {
			uint16_t l, subtag;

			if (off + 12 > rx_len)
				return -EPROTO;
			l = val_le16(rx + off + 4);
			subtag = val_le16(rx + off + 8);
			off += 12;
			if (off + l > rx_len)
				return -EPROTO;
			if (subtag == 3 && l > 4 && (size_t)(l - 4) <= sizeof(s->factory_calib_values)) {
				memcpy(s->factory_calib_values, rx + off + 4, (size_t)(l - 4));
				s->factory_calib_values_len = (size_t)(l - 4);
			} else if (subtag == 7 && l > 4 && (size_t)(l - 4) <= sizeof(s->factory_calib_data)) {
				memcpy(s->factory_calib_data, rx + off + 4, (size_t)(l - 4));
				s->factory_calib_data_len = (size_t)(l - 4);
			}
			off += l;
		}
		if (s->factory_calib_values_len == 0)
			return -EPROTO;
	}

	return validity_calibrate(s);
}

/* ---- On-chip database & gbOS SID mapping (db.py + sid.py) ---- */

static inline bool validity_name_to_identity(const char *uid, uint8_t out[0x4c])
{
	struct fp_name name;
	uint32_t inst = 0, user_word;

	if (!fp_name_parse(uid, &name) || !fp_hex8(name.instance, &inst))
		return false;
	if (name.user < 0 || name.user > 0x00ffffff)
		return false;
	user_word = (name.live ? 0x80000000U : 0U) | (uint32_t)name.user;

	memset(out, 0, 0x4c);
	val_put_le32(out + 0, 3U);		/* identity type = SID */
	val_put_le32(out + 4, 28U);		/* 8-byte SID header + 5 * 4-byte subauth */
	out[8] = 1;				/* revision */
	out[9] = 5;				/* subauth count */
	out[10] = 0; out[11] = 0; out[12] = 0; out[13] = 0; out[14] = 0; out[15] = 5;	/* NT authority */
	val_put_le32(out + 16, 21U);
	val_put_le32(out + 20, VALIDITY_GBOS_SID_MAGIC);
	val_put_le32(out + 24, inst);
	val_put_le32(out + 28, user_word);
	val_put_le32(out + 32, name.id);
	return true;
}

static inline bool validity_identity_to_name(const uint8_t *id, size_t len, char out[FP_NAME_MAX + 1])
{
	uint32_t sid_len, inst, user_word, fid;
	char inst_hex[9];
	bool live;
	int32_t user;

	out[0] = 0;
	if (len < 36 || val_le32(id) != 3U)
		return false;
	sid_len = val_le32(id + 4);
	if (sid_len != 28U || 8 + sid_len > len)
		return false;
	if (id[8] != 1 || id[9] != 5 ||
	    id[10] != 0 || id[11] != 0 || id[12] != 0 || id[13] != 0 || id[14] != 0 || id[15] != 5)
		return false;
	if (val_le32(id + 16) != 21U || val_le32(id + 20) != VALIDITY_GBOS_SID_MAGIC)
		return false;
	inst = val_le32(id + 24);
	user_word = val_le32(id + 28);
	fid = val_le32(id + 32);
	live = (user_word & 0x80000000U) != 0;
	user = (int32_t)(user_word & 0x00ffffffU);
	snprintf(inst_hex, sizeof(inst_hex), "%08x", inst);
	return fp_name_format(out, live, inst_hex, user, fid) > 0;
}

static inline int validity_db_new_record(struct validity *s, uint16_t parent, uint16_t type,
					 uint16_t storage, const uint8_t *data, size_t len,
					 uint16_t *recid_out)
{
	static const uint8_t cmd45 = 0x45;
	uint8_t *cmd;
	uint8_t rx[256];
	size_t rx_len = 0;
	int r;

	/* db_info (0x45) + db_write_enable (matches db.py:new_record) */
	if ((r = validity_cmd_status(s, &cmd45, 1, rx, sizeof(rx), &rx_len)) < 0)
		return r;
	if ((r = validity_write_enable(s)) < 0)
		return r;
	cmd = malloc(9 + len);
	if (!cmd) {
		(void)validity_call_cleanups(s);
		return -EIO;
	}
	cmd[0] = 0x47;
	val_put_le16(cmd + 1, parent);
	val_put_le16(cmd + 3, type);
	val_put_le16(cmd + 5, storage);
	val_put_le16(cmd + 7, (uint16_t)len);
	if (len)
		memcpy(cmd + 9, data, len);
	r = validity_cmd_status(s, cmd, 9 + len, rx, sizeof(rx), &rx_len);
	free(cmd);
	(void)validity_call_cleanups(s);
	if (r < 0)
		return r;
	if (rx_len < 4)
		return -EPROTO;
	if (recid_out)
		*recid_out = val_le16(rx + 2);
	return 0;
}

static inline int validity_db_get_storage(struct validity *s, uint16_t *stg_id,
					  uint16_t *user_ids, size_t max_users, size_t *user_count)
{
	static const uint8_t cmd[16] = {
		0x4b, 0x00, 0x00, 0x0b, 0x00,
		'S', 't', 'g', 'W', 'i', 'n', 'd', 's', 'o', 'r', 0x00
	};
	uint8_t rx[1024];
	size_t rx_len = 0;
	uint16_t err = 0, ucnt;
	int r = validity_tls_cmd(s, cmd, sizeof(cmd), rx, sizeof(rx), &rx_len);

	if (r < 0)
		return r;
	if (validity_check_status(rx, rx_len, &err) < 0)
		return err == 0x04b3 ? -ENOENT : -EIO;
	if (rx_len < 10)
		return -EPROTO;
	*stg_id = val_le16(rx + 2);
	ucnt = val_le16(rx + 4);
	if (10 + (size_t)ucnt * 4 > rx_len)
		return -EPROTO;
	if (user_count) {
		size_t n = ucnt < max_users ? ucnt : max_users;

		for (size_t i = 0; i < n; i++)
			user_ids[i] = val_le16(rx + 10 + i * 4);
		*user_count = n;
	}
	return 0;
}

static inline int validity_init_db(struct validity *s)
{
	static const uint8_t name[11] = "StgWindsor";
	uint16_t stg_id = 0;
	int r = validity_db_get_storage(s, &stg_id, NULL, 0, NULL);

	if (r == 0)
		return 0;
	if (r != -ENOENT)
		return r;
	VALIDITY_LOGI("Creating Validity StgWindsor storage");
	return validity_db_new_record(s, 1, 4, 3, name, sizeof(name), NULL);
}

static inline int validity_db_parse_user(const uint8_t *rx, size_t rx_len,
					 struct validity_user_info *u)
{
	uint16_t fcnt, idsz;
	size_t off;

	if (rx_len < 10)
		return -EPROTO;
	memset(u, 0, sizeof(*u));
	u->dbid = val_le16(rx + 2);
	fcnt = val_le16(rx + 4);
	idsz = val_le16(rx + 8);
	off = 10 + (size_t)fcnt * 8;
	if (off + idsz > rx_len)
		return -EPROTO;
	u->finger_count = fcnt < 16 ? fcnt : 16;
	for (size_t i = 0; i < u->finger_count; i++) {
		u->finger_dbids[i] = val_le16(rx + 10 + i * 8);
		u->finger_subtypes[i] = val_le16(rx + 10 + i * 8 + 2);
	}
	u->identity_len = idsz < sizeof(u->identity) ? idsz : sizeof(u->identity);
	memcpy(u->identity, rx + off, u->identity_len);
	return 0;
}

static inline int validity_db_get_user(struct validity *s, uint16_t dbid,
				       struct validity_user_info *u)
{
	uint8_t cmd[7], rx[1024];
	size_t rx_len = 0;
	int r;

	cmd[0] = 0x4a;
	val_put_le16(cmd + 1, dbid);
	val_put_le16(cmd + 3, 0);
	val_put_le16(cmd + 5, 0);
	if ((r = validity_cmd_status(s, cmd, sizeof(cmd), rx, sizeof(rx), &rx_len)) < 0)
		return r;
	return validity_db_parse_user(rx, rx_len, u);
}

static inline int validity_db_lookup_user(struct validity *s, uint16_t stg_id,
					  const uint8_t identity[0x4c],
					  struct validity_user_info *u)
{
	uint8_t cmd[7 + 0x4c], rx[1024];
	size_t rx_len = 0;
	uint16_t err = 0;
	int r;

	cmd[0] = 0x4a;
	val_put_le16(cmd + 1, 0);
	val_put_le16(cmd + 3, stg_id);
	val_put_le16(cmd + 5, 0x4c);
	memcpy(cmd + 7, identity, 0x4c);
	if ((r = validity_tls_cmd(s, cmd, sizeof(cmd), rx, sizeof(rx), &rx_len)) < 0)
		return r;
	if (validity_check_status(rx, rx_len, &err) < 0)
		return err == 0x04b3 ? -ENOENT : -EIO;
	return validity_db_parse_user(rx, rx_len, u);
}

static inline int validity_db_del_record(struct validity *s, uint16_t dbid)
{
	uint8_t cmd[3], rx[64];
	size_t rx_len = 0;

	cmd[0] = 0x48;
	val_put_le16(cmd + 1, dbid);
	return validity_cmd_status(s, cmd, sizeof(cmd), rx, sizeof(rx), &rx_len);
}

/* ---- Open / Close ---- */

static inline void validity_close(struct validity *s, bool send_reboot)
{
	unsigned int iface = 0;

	if (s->fd >= 0) {
		if (send_reboot && s->ready)
			(void)validity_reboot(s);
		(void)ioctl(s->fd, (int)VALIDITY_USB_RELEASEINTERFACE, &iface);
		if (send_reboot)
			(void)ioctl(s->fd, (int)VALIDITY_USB_RESET);
		close(s->fd);
		s->fd = -1;
	}
	validity_free_crypto(s);
	s->ready = false;
	validity_clear_cancel(s);
}

static inline int validity_open(struct validity *s)
{
	unsigned int cfg = 1, iface = 0;
	uint8_t tls_flash[0x1000];
	size_t flen = 0;
	bool did_reboot = false;
	int r;

	if (s->fd >= 0 && s->ready)
		return 0;
	if (s->fd >= 0)
		validity_close(s, false);

	if ((r = validity_find(s)) < 0)
		return r;
	if (!s->dmi_product[0])
		validity_load_dmi(s);

	s->fd = open(s->path, O_RDWR | O_CLOEXEC);
	if (s->fd < 0)
		return -VALIDITY_ERRNO;
	(void)ioctl(s->fd, (int)VALIDITY_USB_SETCONFIGURATION, &cfg);
	if (ioctl(s->fd, (int)VALIDITY_USB_CLAIMINTERFACE, &iface) < 0) {
		r = -VALIDITY_ERRNO;
		goto fail;
	}

	if ((r = validity_init_flash_if_needed(s, &did_reboot)) < 0) {
		VALIDITY_LOGE("Validity init_flash_if_needed failed: %d", r);
		goto fail;
	}
	if (did_reboot) {
		r = -EAGAIN;
		goto fail;
	}
	if ((r = validity_send_init(s)) < 0) {
		VALIDITY_LOGE("Validity send_init failed: %d", r);
		goto fail;
	}
	if ((r = validity_read_flash(s, 1, 0, sizeof(tls_flash), tls_flash, &flen)) < 0)
		goto fail;
	if ((r = validity_tls_parse_flash(s, tls_flash, flen)) < 0)
		goto fail;
	if ((r = validity_tls_open(s)) < 0)
		goto fail;
	if ((r = validity_upload_fwext_if_needed(s, &did_reboot)) < 0)
		goto fail;
	if (did_reboot) {
		r = -EAGAIN;
		goto fail;
	}
	if ((r = validity_sensor_open(s)) < 0) {
		VALIDITY_LOGE("Validity sensor_open failed: %d", r);
		goto fail;
	}
	if ((r = validity_init_db(s)) < 0) {
		VALIDITY_LOGE("Validity init_db failed: %d", r);
		goto fail;
	}

	s->ready = true;
	VALIDITY_LOGI("Validity %04x:%04x (%s, prod=0x%02x, type 0x%04x, fval=%zu, fdat=%zu) ready",
		      s->vid, s->pid, s->fw_version, s->rom.product, s->type_info->sensor_type,
		      s->factory_calib_values_len, s->factory_calib_data_len);
	return 0;
fail:
	/* Reset USB on any init failure so a wedged sensor or stale TLS session starts fresh next retry. */
	if (r < 0 && r != -EAGAIN && s->fd >= 0) {
		(void)ioctl(s->fd, (int)VALIDITY_USB_RESET);
	}
	validity_close(s, false);
	return r;
}

/* ---- LED glow, capture, enroll, identify, list, delete ---- */

static inline void validity_glow(struct validity *s, bool on)
{
	static const uint8_t led_on[125] = {
		0x39, 0x20, 0xbf, 0x02, 0x00, 0xff, 0xff, 0x00, 0x00, 0x01, 0x99, 0x00, 0x20, 0x00, 0x00, 0x00,
		0x00, 0x99, 0x99, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00,
		0x00, 0x00, 0x99, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	static const uint8_t led_off[125] = {
		0x39, 0xf4, 0x01, 0x00, 0x00, 0xf4, 0x01, 0x00, 0x00, 0x01, 0xff, 0x00, 0x20, 0x00, 0x00, 0x00,
		0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf4, 0x01, 0x00,
		0x00, 0x00, 0xff, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	uint8_t rx[64];
	size_t rx_len = 0;

	(void)validity_tls_cmd(s, on ? led_on : led_off, sizeof(led_on), rx, sizeof(rx), &rx_len);
}

static inline int validity_capture(struct validity *s, int mode, const struct validity_ev *ev, int *fail)
{
	static const uint8_t cmd51[5] = { 0x51, 0x00, 0x20, 0x00, 0x00 };
	static const uint8_t cmd04 = 0x04;
	uint8_t *cmd, *rx;
	const size_t rx_cap = 8192;
	uint8_t ibuf[1024];
	size_t cmd_len = 0, rx_len = 0, ilen = 0;
	int r, result = VALIDITY_FAILED;

	cmd = malloc(VALIDITY_IO_BUF_MAX);
	rx = malloc(rx_cap);
	if (!cmd || !rx) {
		free(cmd);
		free(rx);
		return -EIO;
	}
	r = validity_build_cmd_02(s, mode, cmd, VALIDITY_IO_BUF_MAX, &cmd_len);
	if (r < 0) {
		VALIDITY_LOGE("Validity capture build_cmd_02(mode=%d) failed: %d", mode, r);
		free(cmd);
		free(rx);
		return r;
	}
	r = validity_cmd_status(s, cmd, cmd_len, rx, rx_cap, &rx_len);
	free(cmd);
	if (r < 0) {
		VALIDITY_LOGE("Validity capture cmd_02(mode=%d) failed: %d (rx_len=%zu)", mode, r, rx_len);
		free(rx);
		return r;
	}

	/* Wait for start interrupt (type 0) */
	if ((r = validity_usb_wait_int(s, ibuf, sizeof(ibuf), &ilen, true, 15000)) < 0) {
		VALIDITY_LOGE("Validity capture start wait_int failed: %d", r);
		result = r;
		goto stop;
	}
	if (ilen < 1 || ibuf[0] != 0) {
		result = VALIDITY_RETRY;
		goto stop;
	}

	/* Wait for finger interrupt (type 2) */
	for (;;) {
		if ((r = validity_usb_wait_int(s, ibuf, sizeof(ibuf), &ilen, true, 15000)) < 0) {
			result = r;
			goto stop;
		}
		if (ilen >= 1 && ibuf[0] == 2)
			break;
	}
	validity_emit(ev, VALIDITY_EV_CAPTURE, 0);

	/* Wait for capture complete (type 3, ibuf[2] & 4) */
	for (;;) {
		if ((r = validity_usb_wait_int(s, ibuf, sizeof(ibuf), &ilen, true, 15000)) < 0) {
			result = r;
			goto stop;
		}
		if (ilen < 3 || ibuf[0] != 3) {
			result = VALIDITY_RETRY;
			goto stop;
		}
		if (ibuf[2] & 4)
			break;
	}

	{
		uint16_t st51 = 0;

		if ((r = validity_tls_cmd(s, cmd51, sizeof(cmd51), rx, rx_cap, &rx_len)) < 0) {
			result = r;
			goto stop;
		}
		if (validity_check_status(rx, rx_len, &st51) < 0) {
			VALIDITY_LOGI("Validity capture cmd51 status=0x%04x, retrying", st51);
			if (fail)
				*fail = (int)st51;
			validity_emit(ev, VALIDITY_EV_RETRY, 0);
			result = VALIDITY_RETRY;
			goto stop;
		}
	}
	if (rx_len < 18) {
		result = VALIDITY_RETRY;
		goto stop;
	}
	{
		uint32_t l = val_le32(rx + 2);
		uint32_t scan_err = val_le32(rx + 14);

		if (6 + (size_t)l != rx_len) {
			result = VALIDITY_RETRY;
			goto stop;
		}
		if (scan_err != 0) {
			if (fail)
				*fail = (int)scan_err;
			validity_emit(ev, VALIDITY_EV_RETRY, 0);
			result = VALIDITY_RETRY;
			goto stop;
		}
	}
	result = VALIDITY_OK;
stop:
	(void)validity_tls_cmd(s, &cmd04, 1, rx, rx_cap, &rx_len);
	free(rx);
	return result;
}

static inline int validity_list_templates(struct validity *s, struct validity_template *out,
					  size_t max_out, size_t *count)
{
	uint16_t stg_id = 0, uids[VALIDITY_MAX_TEMPLATES];
	size_t ucnt = 0;
	int r;

	*count = 0;
	if ((r = validity_db_get_storage(s, &stg_id, uids, VALIDITY_MAX_TEMPLATES, &ucnt)) < 0) {
		if (r == -ENOENT)
			return VALIDITY_OK;
		return r;
	}
	for (size_t i = 0; i < ucnt && *count < max_out; i++) {
		struct validity_user_info u;

		if ((r = validity_db_get_user(s, uids[i], &u)) < 0)
			return r;
		out[*count].user_dbid = u.dbid;
		if (validity_identity_to_name(u.identity, u.identity_len, out[*count].uid))
			out[*count].uid_len = strlen(out[*count].uid);
		else {
			out[*count].uid[0] = 0;
			out[*count].uid_len = 0;
		}
		(*count)++;
	}
	return VALIDITY_OK;
}

static inline int validity_delete(struct validity *s, const char *uid)
{
	uint8_t identity[0x4c];
	uint16_t stg_id = 0;
	struct validity_user_info u;
	int r;

	if (!validity_name_to_identity(uid, identity))
		return -EINVAL;
	if ((r = validity_db_get_storage(s, &stg_id, NULL, 0, NULL)) < 0)
		return r == -ENOENT ? VALIDITY_NORECORD : r;
	if ((r = validity_db_lookup_user(s, stg_id, identity, &u)) < 0)
		return r == -ENOENT ? VALIDITY_NORECORD : r;
	for (size_t i = 0; i < u.finger_count; i++) {
		if ((r = validity_db_del_record(s, u.finger_dbids[i])) < 0)
			return r;
	}
	if ((r = validity_db_del_record(s, u.dbid)) < 0)
		return r;
	return VALIDITY_OK;
}

static inline int validity_enroll(struct validity *s, const char *uid,
				  const struct validity_ev *ev, int *fail)
{
	static const uint8_t cmd_create[5] = { 0x69, 1, 0, 0, 0 };
	static const uint8_t cmd_end[5] = { 0x69, 0, 0, 0, 0 };
	uint8_t identity[0x4c], rx[64], ibuf[1024];
	uint8_t *tmpl = NULL, *tid = NULL, *upd_cmd = NULL, *upd_rx = NULL;
	size_t tmpl_len = 0, tid_len = 0, rx_len = 0, ilen = 0;
	uint32_t key = 0;
	int stage = 0, r = 0, result = VALIDITY_FAILED;

	if (!validity_name_to_identity(uid, identity))
		return -EINVAL;
	tmpl = malloc(VALIDITY_IO_BUF_MAX);
	tid = malloc(256);
	upd_cmd = malloc(VALIDITY_IO_BUF_MAX);
	upd_rx = malloc(VALIDITY_IO_BUF_MAX);
	if (!tmpl || !tid || !upd_cmd || !upd_rx) {
		r = -EIO;
		goto out;
	}

	if ((r = validity_cmd_status(s, cmd_create, sizeof(cmd_create), rx, sizeof(rx), &rx_len)) < 0) {
		VALIDITY_LOGE("Validity enroll cmd_create(0x69) failed: %d (rx_len=%zu)", r, rx_len);
		goto out;
	}

	for (;;) {
		uint8_t cmd68[9];
		size_t upd_len = 0, pos = 0;
		uint16_t total_len;

		validity_glow(s, true);
		r = validity_capture(s, VALIDITY_MODE_ENROLL, ev, fail);
		if (r == VALIDITY_RETRY) {
			(void)validity_tls_cmd(s, cmd_end, sizeof(cmd_end), rx, sizeof(rx), &rx_len);
			continue;
		}
		if (r != VALIDITY_OK) {
			if (r != -EPROTO && r != -ETIMEDOUT) {
				(void)validity_tls_cmd(s, cmd_end, sizeof(cmd_end), rx, sizeof(rx), &rx_len);
				validity_glow(s, false);
			}
			result = r;
			goto out;
		}

		/* enrollment_update_start (0x68) */
		cmd68[0] = 0x68;
		val_put_le32(cmd68 + 1, key);
		val_put_le32(cmd68 + 5, 0);
		{
			uint16_t st68 = 0;

			if ((r = validity_tls_cmd(s, cmd68, sizeof(cmd68), rx, sizeof(rx), &rx_len)) < 0) {
				validity_glow(s, false);
				result = r;
				goto out;
			}
			if (validity_check_status(rx, rx_len, &st68) < 0 || rx_len < 6) {
				VALIDITY_LOGI("Validity enroll cmd68 status=0x%04x (rx_len=%zu), retrying", st68, rx_len);
				(void)validity_tls_cmd(s, cmd_end, sizeof(cmd_end), rx, sizeof(rx), &rx_len);
				validity_emit(ev, VALIDITY_EV_RETRY, stage);
				continue;
			}
		}
		key = val_le32(rx + 2);
		if ((r = validity_usb_wait_int(s, ibuf, sizeof(ibuf), &ilen, true, 15000)) < 0) {
			(void)validity_tls_cmd(s, cmd_end, sizeof(cmd_end), rx, sizeof(rx), &rx_len);
			validity_glow(s, false);
			result = r;
			goto out;
		}

		/* append_new_image: 0x6b twice with wait_int in between */
		upd_cmd[0] = 0x6b;
		if (tmpl_len)
			memcpy(upd_cmd + 1, tmpl, tmpl_len);
		if ((r = validity_write_enable(s)) < 0) {
			(void)validity_tls_cmd(s, cmd_end, sizeof(cmd_end), rx, sizeof(rx), &rx_len);
			validity_glow(s, false);
			result = r;
			goto out;
		}
		r = validity_cmd_status(s, upd_cmd, 1 + tmpl_len, upd_rx, VALIDITY_IO_BUF_MAX, &upd_len);
		(void)validity_call_cleanups(s);
		if (r == 0)
			r = validity_usb_wait_int(s, ibuf, sizeof(ibuf), &ilen, true, 15000);
		if (r == 0) {
			if ((r = validity_write_enable(s)) == 0) {
				r = validity_cmd_status(s, upd_cmd, 1 + tmpl_len, upd_rx, VALIDITY_IO_BUF_MAX, &upd_len);
				(void)validity_call_cleanups(s);
			}
		}
		(void)validity_tls_cmd(s, cmd_end, sizeof(cmd_end), rx, sizeof(rx), &rx_len);
		if (r < 0) {
			if (r == -ECANCELED) {
				validity_glow(s, false);
				result = r;
				goto out;
			}
			validity_emit(ev, VALIDITY_EV_RETRY, stage);
			continue;
		}
		if (upd_len < 4) {
			validity_emit(ev, VALIDITY_EV_RETRY, stage);
			continue;
		}
		total_len = val_le16(upd_rx + 2);
		if (4 + (size_t)total_len != upd_len) {
			validity_emit(ev, VALIDITY_EV_RETRY, stage);
			continue;
		}
		tid_len = 0;
		pos = 4;
		while (pos + 0x38 <= upd_len) {
			uint16_t tag = val_le16(upd_rx + pos);
			uint16_t l = val_le16(upd_rx + pos + 2);
			size_t entry_len = 0x38 + (size_t)l;

			if (pos + entry_len > upd_len)
				break;
			if (tag == 0 && entry_len <= VALIDITY_IO_BUF_MAX - 1) {
				memcpy(tmpl, upd_rx + pos, entry_len);
				tmpl_len = entry_len;
			} else if (tag == 3 && l <= 256) {
				memcpy(tid, upd_rx + pos + 0x38, l);
				tid_len = l;
			}
			pos += entry_len;
		}
		stage++;
		validity_emit(ev, VALIDITY_EV_PROGRESS, stage);
		if (tid_len > 0)
			break;
	}

	/* Final enrollment_update_end + save finger record to StgWindsor */
	(void)validity_tls_cmd(s, cmd_end, sizeof(cmd_end), rx, sizeof(rx), &rx_len);
	{
		uint16_t stg_id = 0, usr_dbid = 0;
		struct validity_user_info u;
		size_t tinfo_len = 8 + (4 + tmpl_len) + (4 + tid_len) + 0x20;
		uint8_t *tinfo;

		if ((r = validity_db_get_storage(s, &stg_id, NULL, 0, NULL)) < 0)
			goto out_glow;
		r = validity_db_lookup_user(s, stg_id, identity, &u);
		if (r == -ENOENT) {
			if ((r = validity_db_new_record(s, stg_id, 5, stg_id, identity, 0x4c, &usr_dbid)) < 0)
				goto out_glow;
		} else if (r == 0) {
			usr_dbid = u.dbid;
		} else {
			goto out_glow;
		}

		tinfo = malloc(tinfo_len);
		if (!tinfo) {
			r = -EIO;
			goto out_glow;
		}
		val_put_le16(tinfo + 0, 0x00f5);		/* WINBIO_FINGER_UNSPECIFIED_POS_01 */
		val_put_le16(tinfo + 2, 3);
		val_put_le16(tinfo + 4, (uint16_t)((4 + tmpl_len) + (4 + tid_len)));
		val_put_le16(tinfo + 6, 0x20);
		val_put_le16(tinfo + 8, 1);
		val_put_le16(tinfo + 10, (uint16_t)tmpl_len);
		memcpy(tinfo + 12, tmpl, tmpl_len);
		val_put_le16(tinfo + 12 + tmpl_len, 2);
		val_put_le16(tinfo + 14 + tmpl_len, (uint16_t)tid_len);
		memcpy(tinfo + 16 + tmpl_len, tid, tid_len);
		memset(tinfo + 16 + tmpl_len + tid_len, 0, 0x20);

		r = validity_db_new_record(s, usr_dbid, 0x0b, stg_id, tinfo, tinfo_len, NULL);
		free(tinfo);
		if (r == 0)
			r = validity_usb_wait_int(s, ibuf, sizeof(ibuf), &ilen, false, 15000);
		if (r == 0)
			result = VALIDITY_OK;
	}
out_glow:
	validity_glow(s, false);
out:
	free(tmpl);
	free(tid);
	free(upd_cmd);
	free(upd_rx);
	if (r < 0 && result == VALIDITY_FAILED)
		return r;
	return result;
}

static inline int validity_identify(struct validity *s, const char *const *uids, int count,
				    const struct validity_ev *ev, char *matched, int *fail)
{
	static const uint8_t cmd5e[13] = { 0x5e, 2, 0xff, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0 };
	static const uint8_t cmd60[5] = { 0x60, 0, 0, 0, 0 };
	static const uint8_t cmd62[5] = { 0x62, 0, 0, 0, 0 };
	uint8_t rx[2048], ibuf[1024];
	size_t rx_len = 0, ilen = 0;
	int r, result = VALIDITY_FAILED;

	if (count <= 0)
		return -EINVAL;
	validity_glow(s, true);
	r = validity_capture(s, VALIDITY_MODE_IDENTIFY, ev, fail);
	if (r != VALIDITY_OK) {
		validity_glow(s, false);
		return r;
	}

	if ((r = validity_cmd_status(s, cmd5e, sizeof(cmd5e), rx, sizeof(rx), &rx_len)) < 0)
		goto cleanup;
	if ((r = validity_usb_wait_int(s, ibuf, sizeof(ibuf), &ilen, false, 15000)) < 0)
		goto cleanup;
	if (ilen < 1 || ibuf[0] != 3) {
		VALIDITY_LOGI("Validity identify: finger not recognized (int 0x%02x)", ilen ? ibuf[0] : 0xff);
		result = VALIDITY_NOMATCH;
		goto cleanup;
	}
	if ((r = validity_cmd_status(s, cmd60, sizeof(cmd60), rx, sizeof(rx), &rx_len)) < 0 || rx_len < 4)
		goto cleanup;
	{
		uint16_t dlen = val_le16(rx + 2);
		size_t pos = 4;
		uint32_t usrid = 0;
		bool got_usr = false;

		if (4 + (size_t)dlen != rx_len) {
			result = VALIDITY_FAILED;
			goto cleanup;
		}
		while (pos + 4 <= rx_len) {
			uint16_t k = val_le16(rx + pos);
			uint16_t l = val_le16(rx + pos + 2);

			pos += 4;
			if (pos + l > rx_len)
				break;
			if (k == 1 && l == 4) {
				usrid = val_le32(rx + pos);
				got_usr = true;
			}
			pos += l;
		}
		if (!got_usr) {
			result = VALIDITY_NOMATCH;
			goto cleanup;
		}
		/* Must send cleanup 0x62 before querying DB */
		(void)validity_tls_cmd(s, cmd62, sizeof(cmd62), rx, sizeof(rx), &rx_len);
		validity_glow(s, false);
		{
			struct validity_user_info u;
			char name[FP_NAME_MAX + 1];

			if ((r = validity_db_get_user(s, (uint16_t)usrid, &u)) < 0)
				return r;
			if (validity_identity_to_name(u.identity, u.identity_len, name)) {
				for (int i = 0; i < count; i++) {
					if (strcmp(name, uids[i]) == 0) {
						memcpy(matched, name, strlen(name) + 1);
						VALIDITY_LOGI("Validity identify: matched %s", name);
						return VALIDITY_OK;
					}
				}
			}
			VALIDITY_LOGI("Validity identify: matched dbid %u (%s), not one of this user's",
				      (unsigned int)usrid, name[0] ? name : "non-gbOS");
			return VALIDITY_NOMATCH;
		}
	}

cleanup:
	(void)validity_tls_cmd(s, cmd62, sizeof(cmd62), rx, sizeof(rx), &rx_len);
	validity_glow(s, false);
	if (r < 0 && result == VALIDITY_FAILED)
		return r;
	return result;
}

#endif /* GBOS_VALIDITY_H */
