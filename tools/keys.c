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

#include "brssl.h"
#include "bearssl.h"

static private_key *
decode_key(const unsigned char *buf, size_t len)
{
	br_skey_decoder_context dc;
	int err;
	private_key *sk;

	br_skey_decoder_init(&dc);
	br_skey_decoder_push(&dc, buf, len);
	err = br_skey_decoder_last_error(&dc);
	if (err != 0) {
		const char *errname, *errmsg;

		fprintf(stderr, "ERROR (decoding): err=%d\n", err);
		errname = find_error_name(err, &errmsg);
		if (errname != NULL) {
			fprintf(stderr, "  %s: %s\n", errname, errmsg);
		} else {
			fprintf(stderr, "  (unknown)\n");
		}
		return NULL;
	}
	switch (br_skey_decoder_key_type(&dc)) {
		const br_rsa_private_key *rk;
		const br_ec_private_key *ek;

	case BR_KEYTYPE_RSA:
		rk = br_skey_decoder_get_rsa(&dc);
		sk = xmalloc(sizeof *sk);
		sk->key_type = BR_KEYTYPE_RSA;
		sk->key.rsa.n_bitlen = rk->n_bitlen;
		sk->key.rsa.p = xblobdup(rk->p, rk->plen);
		sk->key.rsa.plen = rk->plen;
		sk->key.rsa.q = xblobdup(rk->q, rk->qlen);
		sk->key.rsa.qlen = rk->qlen;
		sk->key.rsa.dp = xblobdup(rk->dp, rk->dplen);
		sk->key.rsa.dplen = rk->dplen;
		sk->key.rsa.dq = xblobdup(rk->dq, rk->dqlen);
		sk->key.rsa.dqlen = rk->dqlen;
		sk->key.rsa.iq = xblobdup(rk->iq, rk->iqlen);
		sk->key.rsa.iqlen = rk->iqlen;
		break;

	case BR_KEYTYPE_EC:
		ek = br_skey_decoder_get_ec(&dc);
		sk = xmalloc(sizeof *sk);
		sk->key_type = BR_KEYTYPE_EC;
		sk->key.ec.curve = ek->curve;
		sk->key.ec.x = xblobdup(ek->x, ek->xlen);
		sk->key.ec.xlen = ek->xlen;
		break;

	default:
		fprintf(stderr, "Unknown key type: %d\n",
			br_skey_decoder_key_type(&dc));
		sk = NULL;
		break;
	}

	return sk;
}

/* see brssl.h */
private_key *
read_private_key(const char *fname)
{
	unsigned char *buf;
	size_t len;
	private_key *sk;
	pem_object *pos;
	size_t num, u;

	buf = NULL;
	pos = NULL;
	sk = NULL;
	buf = read_file(fname, &len);
	if (buf == NULL) {
		goto deckey_exit;
	}
	if (looks_like_DER(buf, len)) {
		sk = decode_key(buf, len);
		goto deckey_exit;
	} else {
		pos = decode_pem(buf, len, &num);
		if (pos == NULL) {
			goto deckey_exit;
		}
		for (u = 0; pos[u].name; u ++) {
			const char *name;

			name = pos[u].name;
			if (eqstr(name, "RSA PRIVATE KEY")
				|| eqstr(name, "EC PRIVATE KEY")
				|| eqstr(name, "PRIVATE KEY"))
			{
				sk = decode_key(pos[u].data, pos[u].data_len);
				goto deckey_exit;
			}
		}
		fprintf(stderr, "ERROR: no private key in file '%s'\n", fname);
		goto deckey_exit;
	}

deckey_exit:
	if (buf != NULL) {
		xfree(buf);
	}
	if (pos != NULL) {
		for (u = 0; pos[u].name; u ++) {
			free_pem_object_contents(&pos[u]);
		}
		xfree(pos);
	}
	return sk;
}

/* see brssl.h */
void
free_private_key(private_key *sk)
{
	if (sk == NULL) {
		return;
	}
	switch (sk->key_type) {
	case BR_KEYTYPE_RSA:
		xfree(sk->key.rsa.p);
		xfree(sk->key.rsa.q);
		xfree(sk->key.rsa.dp);
		xfree(sk->key.rsa.dq);
		xfree(sk->key.rsa.iq);
		break;
	case BR_KEYTYPE_EC:
		xfree(sk->key.ec.x);
		break;
	}
	xfree(sk);
}
