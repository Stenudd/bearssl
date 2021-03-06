/*
 * Copyright (c) 2016 Thomas Pornin <pornin@bolet.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining 
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be 
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND 
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/poll.h>

#include "brssl.h"
#include "bearssl.h"

static int
host_bind(const char *host, const char *port, int verbose)
{
	struct addrinfo hints, *si, *p;
	int fd;
	int err;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	err = getaddrinfo(host, port, &hints, &si);
	if (err != 0) {
		fprintf(stderr, "ERROR: getaddrinfo(): %s\n",
			gai_strerror(err));
		return -1;
	}
	fd = -1;
	for (p = si; p != NULL; p = p->ai_next) {
		struct sockaddr *sa;
		struct sockaddr_in sa4;
		struct sockaddr_in6 sa6;
		size_t sa_len;
		void *addr;
		char tmp[INET6_ADDRSTRLEN + 50];
		int opt;

		sa = (struct sockaddr *)p->ai_addr;
		if (sa->sa_family == AF_INET) {
			sa4 = *(struct sockaddr_in *)sa;
			sa = (struct sockaddr *)&sa4;
			sa_len = sizeof sa4;
			addr = &sa4.sin_addr;
			if (host == NULL) {
				sa4.sin_addr.s_addr = INADDR_ANY;
			}
		} else if (sa->sa_family == AF_INET6) {
			sa6 = *(struct sockaddr_in6 *)sa;
			sa = (struct sockaddr *)&sa6;
			sa_len = sizeof sa6;
			addr = &sa6.sin6_addr;
			if (host == NULL) {
				sa6.sin6_addr = in6addr_any;
			}
		} else {
			addr = NULL;
			sa_len = p->ai_addrlen;
		}
		if (addr != NULL) {
			inet_ntop(p->ai_family, addr, tmp, sizeof tmp);
		} else {
			snprintf(tmp, INET6_ADDRSTRLEN + 50,
				 "<unknown family: %d>",
				 (int)sa->sa_family);
		}
		if (verbose) {
			fprintf(stderr, "binding to: %s\n", tmp);
		}
		fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (fd < 0) {
			if (verbose) {
				perror("socket()");
			}
			continue;
		}
		opt = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
		opt = 0;
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof opt);
		if (bind(fd, sa, sa_len) < 0) {
			if (verbose) {
				perror("bind()");
			}
			close(fd);
			continue;
		}
		break;
	}
	if (p == NULL) {
		freeaddrinfo(si);
		fprintf(stderr, "ERROR: failed to bind\n");
		return -1;
	}
	freeaddrinfo(si);
	if (listen(fd, 5) < 0) {
		if (verbose) {
			perror("listen()");
		}
		close(fd);
		return -1;
	}
	if (verbose) {
		fprintf(stderr, "bound.\n");
	}
	return fd;
}

static int
accept_client(int server_fd, int verbose)
{
	int fd;
	struct sockaddr sa;
	socklen_t sa_len;

	sa_len = sizeof sa;
	fd = accept(server_fd, &sa, &sa_len);
	if (fd < 0) {
		if (verbose) {
			perror("accept()");
		}
		return -1;
	}
	if (verbose) {
		char tmp[INET6_ADDRSTRLEN + 50];
		const char *name;

		name = NULL;
		switch (sa.sa_family) {
		case AF_INET:
			name = inet_ntop(AF_INET,
				&((struct sockaddr_in *)&sa)->sin_addr,
				tmp, sizeof tmp);
			break;
		case AF_INET6:
			name = inet_ntop(AF_INET,
				&((struct sockaddr_in *)&sa)->sin_addr,
				tmp, sizeof tmp);
			break;
		}
		if (name == NULL) {
			snprintf(tmp, INET6_ADDRSTRLEN + 50,
				 "<unknown: %lu>",
				 (unsigned long)sa.sa_family);
			name = tmp;
		}
		fprintf(stderr, "accepting connection from: %s\n", name);
	}

	/*
	 * We make the socket non-blocking, since we are going to use
	 * poll() to organise I/O.
	 */
	fcntl(fd, F_SETFL, O_NONBLOCK);
	return fd;
}

static void
usage_server(void)
{
	fprintf(stderr,
"usage: brssl server [ options ]\n");
	fprintf(stderr,
"options:\n");
	fprintf(stderr,
"   -q              suppress verbose messages\n");
	fprintf(stderr,
"   -trace          activate extra debug messages (dump of all packets)\n");
	fprintf(stderr,
"   -b name         bind to a specific address or host name\n");
	fprintf(stderr,
"   -p port         bind to a specific port (default: 4433)\n");
	fprintf(stderr,
"   -mono           use monodirectional buffering\n");
	fprintf(stderr,
"   -buf length     set the I/O buffer length (in bytes)\n");
	fprintf(stderr,
"   -cache length   set the session cache storage length (in bytes)\n");
	fprintf(stderr,
"   -cert fname     read certificate chain from file 'fname'\n");
	fprintf(stderr,
"   -key fname      read private key from file 'fname'\n");
	fprintf(stderr,
"   -list           list supported names (protocols, algorithms...)\n");
	fprintf(stderr,
"   -vmin name      set minimum supported version (default: TLS-1.0)\n");
	fprintf(stderr,
"   -vmax name      set maximum supported version (default: TLS-1.2)\n");
	fprintf(stderr,
"   -cs names       set list of supported cipher suites (comma-separated)\n");
	fprintf(stderr,
"   -hf names       add support for some hash functions (comma-separated)\n");
	fprintf(stderr,
"   -serverpref     enforce server's preferences for cipher suites\n");
	exit(EXIT_FAILURE);
}

typedef struct {
	const br_ssl_server_policy_class *vtable;
	int verbose;
	br_x509_certificate *chain;
	size_t chain_len;
	int cert_signer_algo;
	private_key *sk;
} policy_context;

static int
get_cert_signer_algo(br_x509_certificate *xc)
{
	br_x509_decoder_context dc;
	int err;

	br_x509_decoder_init(&dc, 0, 0);
	br_x509_decoder_push(&dc, xc->data, xc->data_len);
	err = br_x509_decoder_last_error(&dc);
	if (err != 0) {
		return -err;
	} else {
		return br_x509_decoder_get_signer_key_type(&dc);
	}
}

static int
sp_choose(const br_ssl_server_policy_class **pctx,
	const br_ssl_server_context *cc,
	br_ssl_server_choices *choices)
{
	policy_context *pc;
	const br_suite_translated *st;
	size_t u, st_num;
	unsigned chashes;
	int hash_id;

	pc = (policy_context *)pctx;
	st = br_ssl_server_get_client_suites(cc, &st_num);
	chashes = br_ssl_server_get_client_hashes(cc);
	for (hash_id = 6; hash_id >= 2; hash_id --) {
		if ((chashes >> hash_id) & 1) {
			break;
		}
	}
	if (pc->verbose) {
		fprintf(stderr, "Client parameters:\n");
		fprintf(stderr, "   Maximum version:      ");
		switch (cc->client_max_version) {
		case BR_SSL30:
			fprintf(stderr, "SSL 3.0");
			break;
		case BR_TLS10:
			fprintf(stderr, "TLS 1.0");
			break;
		case BR_TLS11:
			fprintf(stderr, "TLS 1.1");
			break;
		case BR_TLS12:
			fprintf(stderr, "TLS 1.2");
			break;
		default:
			fprintf(stderr, "unknown (0x%04X)",
				(unsigned)cc->client_max_version);
			break;
		}
		fprintf(stderr, "\n");
		fprintf(stderr, "   Compatible cipher suites:\n");
		for (u = 0; u < st_num; u ++) {
			char csn[80];

			get_suite_name_ext(st[u][0], csn, sizeof csn);
			fprintf(stderr, "      %s\n", csn);
		}
		fprintf(stderr, "   Common hash functions:");
		for (u = 2; u <= 6; u ++) {
			if ((chashes >> u) & 1) {
				int z;

				switch (u) {
				case 3: z = 224; break;
				case 4: z = 256; break;
				case 5: z = 384; break;
				case 6: z = 512; break;
				default:
					z = 1;
					break;
				}
				fprintf(stderr, " sha%d", z);
			}
		}
		fprintf(stderr, "\n");
	}
	for (u = 0; u < st_num; u ++) {
		unsigned tt;

		tt = st[u][1];
		switch (tt >> 12) {
		case BR_SSLKEYX_RSA:
			if (pc->sk->key_type == BR_KEYTYPE_RSA) {
				choices->cipher_suite = st[u][0];
				goto choose_ok;
			}
			break;
		case BR_SSLKEYX_ECDHE_RSA:
			if (pc->sk->key_type == BR_KEYTYPE_RSA) {
				choices->cipher_suite = st[u][0];
				choices->hash_id = hash_id;
				goto choose_ok;
			}
			break;
		case BR_SSLKEYX_ECDHE_ECDSA:
			if (pc->sk->key_type == BR_KEYTYPE_EC) {
				choices->cipher_suite = st[u][0];
				choices->hash_id = hash_id;
				goto choose_ok;
			}
			break;
		case BR_SSLKEYX_ECDH_RSA:
			if (pc->sk->key_type == BR_KEYTYPE_EC
				&& pc->cert_signer_algo == BR_KEYTYPE_RSA)
			{
				choices->cipher_suite = st[u][0];
				goto choose_ok;
			}
			break;
		case BR_SSLKEYX_ECDH_ECDSA:
			if (pc->sk->key_type == BR_KEYTYPE_EC
				&& pc->cert_signer_algo == BR_KEYTYPE_EC)
			{
				choices->cipher_suite = st[u][0];
				goto choose_ok;
			}
			break;
		}
	}
	return 0;

choose_ok:
	choices->chain = pc->chain;
	choices->chain_len = pc->chain_len;
	if (pc->verbose) {
		char csn[80];

		get_suite_name_ext(choices->cipher_suite, csn, sizeof csn);
		fprintf(stderr, "Using: %s\n", csn);
	}
	return 1;
}

static uint32_t
sp_do_keyx(const br_ssl_server_policy_class **pctx,
	unsigned char *data, size_t len)
{
	policy_context *pc;

	pc = (policy_context *)pctx;
	switch (pc->sk->key_type) {
	case BR_KEYTYPE_RSA:
		return br_rsa_ssl_decrypt(
			&br_rsa_i31_private, &pc->sk->key.rsa,
			data, len);
	case BR_KEYTYPE_EC:
		return br_ec_prime_i31.mul(data, len, pc->sk->key.ec.x,
			pc->sk->key.ec.xlen, pc->sk->key.ec.curve);
	default:
		fprintf(stderr, "ERROR: unknown private key type (%d)\n",
			(int)pc->sk->key_type);
		return 0;
	}
}

/*
 * OID for hash functions in RSA signatures.
 */
static const unsigned char HASH_OID_SHA1[] = {
	0x05, 0x2B, 0x0E, 0x03, 0x02, 0x1A
};

static const unsigned char HASH_OID_SHA224[] = {
	0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x04
};

static const unsigned char HASH_OID_SHA256[] = {
	0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01
};

static const unsigned char HASH_OID_SHA384[] = {
	0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02
};

static const unsigned char HASH_OID_SHA512[] = {
	0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03
};

static const unsigned char *HASH_OID[] = {
	HASH_OID_SHA1,
	HASH_OID_SHA224,
	HASH_OID_SHA256,
	HASH_OID_SHA384,
	HASH_OID_SHA512
};

static const br_hash_class *
get_hash_impl(int hash_id)
{
	size_t u;

	for (u = 0; hash_functions[u].name; u ++) {
		const br_hash_class *hc;
		int id;

		hc = hash_functions[u].hclass;
		id = (hc->desc >> BR_HASHDESC_ID_OFF) & BR_HASHDESC_ID_MASK;
		if (id == hash_id) {
			return hc;
		}
	}
	return NULL;
}

static size_t
sp_do_sign(const br_ssl_server_policy_class **pctx,
	int hash_id, size_t hv_len, unsigned char *data, size_t len)
{
	policy_context *pc;
	unsigned char hv[64];

	pc = (policy_context *)pctx;
	memcpy(hv, data, hv_len);
	switch (pc->sk->key_type) {
		size_t sig_len;
		uint32_t x;
		const unsigned char *hash_oid;
		const br_hash_class *hc;

	case BR_KEYTYPE_RSA:
		if (hash_id == 0) {
			hash_oid = NULL;
		} else if (hash_id >= 2 && hash_id <= 6) {
			hash_oid = HASH_OID[hash_id - 2];
		} else {
			if (pc->verbose) {
				fprintf(stderr, "ERROR: cannot RSA-sign with"
					" unknown hash function: %d\n",
					hash_id);
			}
			return 0;
		}
		sig_len = (pc->sk->key.rsa.n_bitlen + 7) >> 3;
		if (len < sig_len) {
			if (pc->verbose) {
				fprintf(stderr, "ERROR: cannot RSA-sign,"
					" buffer is too small"
					" (sig=%lu, buf=%lu)\n",
					(unsigned long)sig_len,
					(unsigned long)len);
			}
			return 0;
		}
		x = br_rsa_i31_pkcs1_sign(hash_oid, hv, hv_len,
			&pc->sk->key.rsa, data);
		if (!x) {
			if (pc->verbose) {
				fprintf(stderr, "ERROR: RSA-sign failure\n");
			}
			return 0;
		}
		return sig_len;

	case BR_KEYTYPE_EC:
		hc = get_hash_impl(hash_id);
		if (hc == NULL) {
			if (pc->verbose) {
				fprintf(stderr, "ERROR: cannot RSA-sign with"
					" unknown hash function: %d\n",
					hash_id);
			}
			return 0;
		}
		if (len < 139) {
			if (pc->verbose) {
				fprintf(stderr, "ERROR: cannot ECDSA-sign"
					" (output buffer = %lu)\n",
					(unsigned long)len);
			}
			return 0;
		}
		sig_len = br_ecdsa_i31_sign_asn1(&br_ec_prime_i31, 
			hc, hv, &pc->sk->key.ec, data);
		if (sig_len == 0) {
			if (pc->verbose) {
				fprintf(stderr, "ERROR: ECDSA-sign failure\n");
			}
			return 0;
		}
		return sig_len;

	default:
		return 0;
	}
}

static const br_ssl_server_policy_class policy_vtable = {
	sizeof(policy_context),
	sp_choose,
	sp_do_keyx,
	sp_do_sign
};

/* see brssl.h */
int
do_server(int argc, char *argv[])
{
	int retcode;
	int verbose;
	int trace;
	int i, bidi;
	const char *bind_name;
	const char *port;
	unsigned vmin, vmax;
	cipher_suite *suites;
	size_t num_suites;
	uint16_t *suite_ids;
	unsigned hfuns;
	br_x509_certificate *chain;
	size_t chain_len;
	int cert_signer_algo;
	private_key *sk;
	size_t u;
	br_ssl_server_context cc;
	policy_context pc;
	br_ssl_session_cache_lru lru;
	unsigned char *iobuf, *cache;
	size_t iobuf_len, cache_len;
	uint32_t flags;
	int server_fd, fd;

	retcode = 0;
	verbose = 1;
	trace = 0;
	bind_name = NULL;
	port = NULL;
	bidi = 1;
	vmin = 0;
	vmax = 0;
	suites = NULL;
	num_suites = 0;
	hfuns = 0;
	suite_ids = NULL;
	chain = NULL;
	chain_len = 0;
	sk = NULL;
	iobuf = NULL;
	iobuf_len = 0;
	cache = NULL;
	cache_len = (size_t)-1;
	flags = 0;
	server_fd = -1;
	fd = -1;
	for (i = 0; i < argc; i ++) {
		const char *arg;

		arg = argv[i];
		if (arg[0] != '-') {
			usage_server();
			goto server_exit_error;
		}
		if (eqstr(arg, "-v") || eqstr(arg, "-verbose")) {
			verbose = 1;
		} else if (eqstr(arg, "-q") || eqstr(arg, "-quiet")) {
			verbose = 0;
		} else if (eqstr(arg, "-trace")) {
			trace = 1;
		} else if (eqstr(arg, "-b")) {
			if (++ i >= argc) {
				fprintf(stderr,
					"ERROR: no argument for '-b'\n");
				usage_server();
				goto server_exit_error;
			}
			if (bind_name != NULL) {
				fprintf(stderr, "ERROR: duplicate bind host\n");
				usage_server();
				goto server_exit_error;
			}
			bind_name = argv[i];
		} else if (eqstr(arg, "-p")) {
			if (++ i >= argc) {
				fprintf(stderr,
					"ERROR: no argument for '-p'\n");
				usage_server();
				goto server_exit_error;
			}
			if (port != NULL) {
				fprintf(stderr, "ERROR: duplicate bind port\n");
				usage_server();
				goto server_exit_error;
			}
			port = argv[i];
		} else if (eqstr(arg, "-mono")) {
			bidi = 0;
		} else if (eqstr(arg, "-buf")) {
			if (++ i >= argc) {
				fprintf(stderr,
					"ERROR: no argument for '-buf'\n");
				usage_server();
				goto server_exit_error;
			}
			arg = argv[i];
			if (iobuf_len != 0) {
				fprintf(stderr,
					"ERROR: duplicate I/O buffer length\n");
				usage_server();
				goto server_exit_error;
			}
			iobuf_len = strtoul(arg, 0, 10);
		} else if (eqstr(arg, "-cache")) {
			if (++ i >= argc) {
				fprintf(stderr,
					"ERROR: no argument for '-cache'\n");
				usage_server();
				goto server_exit_error;
			}
			arg = argv[i];
			if (cache_len != (size_t)-1) {
				fprintf(stderr, "ERROR: duplicate session"
					" cache length\n");
				usage_server();
				goto server_exit_error;
			}
			cache_len = strtoul(arg, 0, 10);
		} else if (eqstr(arg, "-cert")) {
			if (++ i >= argc) {
				fprintf(stderr,
					"ERROR: no argument for '-cert'\n");
				usage_server();
				goto server_exit_error;
			}
			if (chain != NULL) {
				fprintf(stderr,
					"ERROR: duplicate certificate chain\n");
				usage_server();
				goto server_exit_error;
			}
			arg = argv[i];
			chain = read_certificates(arg, &chain_len);
			if (chain == NULL || chain_len == 0) {
				goto server_exit_error;
			}
		} else if (eqstr(arg, "-key")) {
			if (++ i >= argc) {
				fprintf(stderr,
					"ERROR: no argument for '-key'\n");
				usage_server();
				goto server_exit_error;
			}
			if (sk != NULL) {
				fprintf(stderr,
					"ERROR: duplicate private key\n");
				usage_server();
				goto server_exit_error;
			}
			arg = argv[i];
			sk = read_private_key(arg);
			if (sk == NULL) {
				goto server_exit_error;
			}
		} else if (eqstr(arg, "-list")) {
			list_names();
			goto server_exit;
		} else if (eqstr(arg, "-vmin")) {
			if (++ i >= argc) {
				fprintf(stderr,
					"ERROR: no argument for '-vmin'\n");
				usage_server();
				goto server_exit_error;
			}
			arg = argv[i];
			if (vmin != 0) {
				fprintf(stderr,
					"ERROR: duplicate minimum version\n");
				usage_server();
				goto server_exit_error;
			}
			vmin = parse_version(arg, strlen(arg));
			if (vmin == 0) {
				fprintf(stderr,
					"ERROR: unrecognised version '%s'\n",
					arg);
				usage_server();
				goto server_exit_error;
			}
		} else if (eqstr(arg, "-vmax")) {
			if (++ i >= argc) {
				fprintf(stderr,
					"ERROR: no argument for '-vmax'\n");
				usage_server();
				goto server_exit_error;
			}
			arg = argv[i];
			if (vmax != 0) {
				fprintf(stderr,
					"ERROR: duplicate maximum version\n");
				usage_server();
				goto server_exit_error;
			}
			vmax = parse_version(arg, strlen(arg));
			if (vmax == 0) {
				fprintf(stderr,
					"ERROR: unrecognised version '%s'\n",
					arg);
				usage_server();
				goto server_exit_error;
			}
		} else if (eqstr(arg, "-cs")) {
			if (++ i >= argc) {
				fprintf(stderr,
					"ERROR: no argument for '-cs'\n");
				usage_server();
				goto server_exit_error;
			}
			arg = argv[i];
			if (suites != NULL) {
				fprintf(stderr, "ERROR: duplicate list"
					" of cipher suites\n");
				usage_server();
				goto server_exit_error;
			}
			suites = parse_suites(arg, &num_suites);
			if (suites == NULL) {
				usage_server();
				goto server_exit_error;
			}
		} else if (eqstr(arg, "-hf")) {
			unsigned x;

			if (++ i >= argc) {
				fprintf(stderr,
					"ERROR: no argument for '-hf'\n");
				usage_server();
				goto server_exit_error;
			}
			arg = argv[i];
			x = parse_hash_functions(arg);
			if (x == 0) {
				usage_server();
				goto server_exit_error;
			}
			hfuns |= x;
		} else if (eqstr(arg, "-serverpref")) {
			flags |= BR_OPT_ENFORCE_SERVER_PREFERENCES;
		} else {
			fprintf(stderr, "ERROR: unknown option: '%s'\n", arg);
			usage_server();
			goto server_exit_error;
		}
	}
	if (port == NULL) {
		port = "4433";
	}
	if (vmin == 0) {
		vmin = BR_TLS10;
	}
	if (vmax == 0) {
		vmax = BR_TLS12;
	}
	if (vmax < vmin) {
		fprintf(stderr, "ERROR: impossible minimum/maximum protocol"
			" version combination\n");
		usage_server();
		goto server_exit_error;
	}
	if (suites == NULL) {
		num_suites = 0;

		for (u = 0; cipher_suites[u].name; u ++) {
			if ((cipher_suites[u].req & REQ_TLS12) == 0
				|| vmax >= BR_TLS12)
			{
				num_suites ++;
			}
		}
		suites = xmalloc(num_suites * sizeof *suites);
		num_suites = 0;
		for (u = 0; cipher_suites[u].name; u ++) {
			if ((cipher_suites[u].req & REQ_TLS12) == 0
				|| vmax >= BR_TLS12)
			{
				suites[num_suites ++] = cipher_suites[u];
			}
		}
	}
	if (hfuns == 0) {
		hfuns = (unsigned)-1;
	}
	if (chain == NULL || chain_len == 0) {
		fprintf(stderr, "ERROR: no certificate chain provided\n");
		goto server_exit_error;
	}
	if (sk == NULL) {
		fprintf(stderr, "ERROR: no private key provided\n");
		goto server_exit_error;
	}
	switch (sk->key_type) {
		int curve;
		uint32_t supp;

	case BR_KEYTYPE_RSA:
		break;
	case BR_KEYTYPE_EC:
		curve = sk->key.ec.curve;
		supp = br_ec_prime_i31.supported_curves;
		if (curve > 31 || !((supp >> curve) & 1)) {
			fprintf(stderr, "ERROR: private key curve (%d)"
				" is not supported\n", curve);
			goto server_exit_error;
		}
		break;
	default:
		fprintf(stderr, "ERROR: unsupported private key type (%d)\n",
			sk->key_type);
		break;
	}
	cert_signer_algo = get_cert_signer_algo(chain);
	if (cert_signer_algo < 0) {
		fprintf(stderr, "ERROR: server certificate cannot be"
			" decoded (err=%d)\n", -cert_signer_algo);
		goto server_exit_error;
	} else if (verbose) {
		const char *csas;

		switch (cert_signer_algo) {
		case BR_KEYTYPE_RSA: csas = "RSA"; break;
		case BR_KEYTYPE_EC:  csas = "EC"; break;
		default:
			csas = "unknown";
			break;
		}
		fprintf(stderr, "Issuing CA key type: %d (%s)\n",
			cert_signer_algo, csas);
	}
	if (iobuf_len == 0) {
		if (bidi) {
			iobuf_len = BR_SSL_BUFSIZE_BIDI;
		} else {
			iobuf_len = BR_SSL_BUFSIZE_MONO;
		}
	}
	iobuf = xmalloc(iobuf_len);
	if (cache_len == (size_t)-1) {
		cache_len = 5000;
	}
	cache = xmalloc(cache_len);

	/*
	 * Compute implementation requirements and inject implementations.
	 */
	suite_ids = xmalloc(num_suites * sizeof *suite_ids);
	br_ssl_server_zero(&cc);
	br_ssl_engine_set_versions(&cc.eng, vmin, vmax);
	br_ssl_server_set_all_flags(&cc, flags);
	if (vmin <= BR_TLS11) {
		if (!(hfuns & (1 << br_md5_ID))) {
			fprintf(stderr, "ERROR: TLS 1.0 and 1.1 need MD5\n");
			goto server_exit_error;
		}
		if (!(hfuns & (1 << br_sha1_ID))) {
			fprintf(stderr, "ERROR: TLS 1.0 and 1.1 need SHA-1\n");
			goto server_exit_error;
		}
	}
	for (u = 0; u < num_suites; u ++) {
		unsigned req;

		req = suites[u].req;
		suite_ids[u] = suites[u].suite;
		if ((req & REQ_TLS12) != 0 && vmax < BR_TLS12) {
			fprintf(stderr,
				"ERROR: cipher suite %s requires TLS 1.2\n",
				suites[u].name);
			goto server_exit_error;
		}
		if ((req & REQ_SHA1) != 0 && !(hfuns & (1 << br_sha1_ID))) {
			fprintf(stderr,
				"ERROR: cipher suite %s requires SHA-1\n",
				suites[u].name);
			goto server_exit_error;
		}
		if ((req & REQ_SHA256) != 0 && !(hfuns & (1 << br_sha256_ID))) {
			fprintf(stderr,
				"ERROR: cipher suite %s requires SHA-256\n",
				suites[u].name);
			goto server_exit_error;
		}
		if ((req & REQ_SHA384) != 0 && !(hfuns & (1 << br_sha384_ID))) {
			fprintf(stderr,
				"ERROR: cipher suite %s requires SHA-384\n",
				suites[u].name);
			goto server_exit_error;
		}
		/* TODO: algorithm implementation selection */
		if ((req & REQ_AESCBC) != 0) {
			br_ssl_engine_set_aes_cbc(&cc.eng,
				&br_aes_ct_cbcenc_vtable,
				&br_aes_ct_cbcdec_vtable);
			br_ssl_engine_set_cbc(&cc.eng,
				&br_sslrec_in_cbc_vtable,
				&br_sslrec_out_cbc_vtable);
		}
		if ((req & REQ_AESGCM) != 0) {
			br_ssl_engine_set_aes_ctr(&cc.eng,
				&br_aes_ct_ctr_vtable);
			br_ssl_engine_set_ghash(&cc.eng,
				&br_ghash_ctmul);
			br_ssl_engine_set_gcm(&cc.eng,
				&br_sslrec_in_gcm_vtable,
				&br_sslrec_out_gcm_vtable);
		}
		if ((req & REQ_3DESCBC) != 0) {
			br_ssl_engine_set_des_cbc(&cc.eng,
				&br_des_ct_cbcenc_vtable,
				&br_des_ct_cbcdec_vtable);
			br_ssl_engine_set_cbc(&cc.eng,
				&br_sslrec_in_cbc_vtable,
				&br_sslrec_out_cbc_vtable);
		}
		if ((req & (REQ_ECDHE_RSA | REQ_ECDHE_ECDSA)) != 0) {
			br_ssl_engine_set_ec(&cc.eng, &br_ec_prime_i31);
		}
	}
	br_ssl_engine_set_suites(&cc.eng, suite_ids, num_suites);

	for (u = 0; hash_functions[u].name; u ++) {
		const br_hash_class *hc;
		int id;

		hc = hash_functions[u].hclass;
		id = (hc->desc >> BR_HASHDESC_ID_OFF) & BR_HASHDESC_ID_MASK;
		if ((hfuns & ((unsigned)1 << id)) != 0) {
			br_ssl_engine_set_hash(&cc.eng, id, hc);
		}
	}
	if (vmin <= BR_TLS11) {
		br_ssl_engine_set_prf10(&cc.eng, &br_tls10_prf);
	}
	if (vmax >= BR_TLS12) {
		if ((hfuns & ((unsigned)1 << br_sha256_ID)) != 0) {
			br_ssl_engine_set_prf_sha256(&cc.eng,
				&br_tls12_sha256_prf);
		}
		if ((hfuns & ((unsigned)1 << br_sha384_ID)) != 0) {
			br_ssl_engine_set_prf_sha384(&cc.eng,
				&br_tls12_sha384_prf);
		}
	}

	br_ssl_session_cache_lru_init(&lru, cache, cache_len);
	br_ssl_server_set_cache(&cc, &lru.vtable);

	pc.vtable = &policy_vtable;
	pc.verbose = verbose;
	pc.chain = chain;
	pc.chain_len = chain_len;
	pc.cert_signer_algo = cert_signer_algo;
	pc.sk = sk;
	br_ssl_server_set_policy(&cc, &pc.vtable);

	br_ssl_engine_set_buffer(&cc.eng, iobuf, iobuf_len, bidi);

	/*
	 * Open the server socket.
	 */
	server_fd = host_bind(bind_name, port, verbose);
	if (server_fd < 0) {
		goto server_exit_error;
	}

	/*
	 * Process incoming clients, one at a time. Note that we do not
	 * accept any client until the previous connection has finished:
	 * this is voluntary, since the tool uses stdin/stdout for
	 * application data, and thus cannot really run two connections
	 * simultaneously.
	 */
	for (;;) {
		int x;

		fd = accept_client(server_fd, verbose);
		if (fd < 0) {
			goto server_exit_error;
		}
		br_ssl_server_reset(&cc);
		x = run_ssl_engine(&cc.eng, fd,
			(verbose ? RUN_ENGINE_VERBOSE : 0)
			| (trace ? RUN_ENGINE_TRACE : 0));
		close(fd);
		fd = -1;
		if (x < -1) {
			goto server_exit_error;
		}
	}

	/*
	 * Release allocated structures.
	 */
server_exit:
	xfree(suites);
	xfree(suite_ids);
	if (chain != NULL) {
		for (u = 0; u < chain_len; u ++) {
			xfree(chain[u].data);
		}
		xfree(chain);
	}
	if (sk != NULL) {
		free_private_key(sk);
	}
	xfree(iobuf);
	xfree(cache);
	if (fd >= 0) {
		close(fd);
	}
	return retcode;

server_exit_error:
	retcode = -1;
	goto server_exit;
}
