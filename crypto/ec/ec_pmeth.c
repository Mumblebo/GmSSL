/*
 * Written by Dr Stephen N Henson (steve@openssl.org) for the OpenSSL project
 * 2006.
 */
/* ====================================================================
 * Copyright (c) 2006 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit. (http://www.OpenSSL.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    licensing@OpenSSL.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *    nor may "OpenSSL" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.OpenSSL.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This product includes cryptographic software written by Eric Young
 * (eay@cryptsoft.com).  This product includes software written by Tim
 * Hudson (tjh@cryptsoft.com).
 *
 */

#include <stdio.h>
#include "cryptlib.h"
#include <openssl/asn1t.h>
#include <openssl/x509.h>
#include <openssl/ec.h>
#include "ec_lcl.h"
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include "evp_locl.h"
#include <openssl/sm2.h>
#include <openssl/ecies.h>

/* EC pkey context structure */

typedef struct {
    /* Key and paramgen group */
    EC_GROUP *gen_group;
    /* message digest */
    const EVP_MD *md;
    /* Duplicate key if custom cofactor needed */
    EC_KEY *co_key;
    /* Cofactor mode */
    signed char cofactor_mode;
    /* KDF (if any) to use for ECDH */
    char kdf_type;
    /* Message digest to use for key derivation */
    const EVP_MD *kdf_md;
    /* User key material */
    unsigned char *kdf_ukm;
    size_t kdf_ukmlen;
    /* KDF output length */
    size_t kdf_outlen;
} EC_PKEY_CTX;

static int pkey_ec_init(EVP_PKEY_CTX *ctx)
{
    EC_PKEY_CTX *dctx;
    dctx = OPENSSL_malloc(sizeof(EC_PKEY_CTX));
    if (!dctx)
        return 0;
    dctx->gen_group = NULL;
    dctx->md = NULL;

    dctx->cofactor_mode = -1;
    dctx->co_key = NULL;
    dctx->kdf_type = EVP_PKEY_ECDH_KDF_NONE;
    dctx->kdf_md = NULL;
    dctx->kdf_outlen = 0;
    dctx->kdf_ukm = NULL;
    dctx->kdf_ukmlen = 0;

    ctx->data = dctx;

    return 1;
}

static int pkey_ec_copy(EVP_PKEY_CTX *dst, EVP_PKEY_CTX *src)
{
    EC_PKEY_CTX *dctx, *sctx;
    if (!pkey_ec_init(dst))
        return 0;
    sctx = src->data;
    dctx = dst->data;
    if (sctx->gen_group) {
        dctx->gen_group = EC_GROUP_dup(sctx->gen_group);
        if (!dctx->gen_group)
            return 0;
    }
    dctx->md = sctx->md;

    if (sctx->co_key) {
        dctx->co_key = EC_KEY_dup(sctx->co_key);
        if (!dctx->co_key)
            return 0;
    }
    dctx->kdf_type = sctx->kdf_type;
    dctx->kdf_md = sctx->kdf_md;
    dctx->kdf_outlen = sctx->kdf_outlen;
    if (sctx->kdf_ukm) {
        dctx->kdf_ukm = BUF_memdup(sctx->kdf_ukm, sctx->kdf_ukmlen);
        if (!dctx->kdf_ukm)
            return 0;
    } else
        dctx->kdf_ukm = NULL;
    dctx->kdf_ukmlen = sctx->kdf_ukmlen;
    return 1;
}

static void pkey_ec_cleanup(EVP_PKEY_CTX *ctx)
{
    EC_PKEY_CTX *dctx = ctx->data;
    if (dctx) {
        if (dctx->gen_group)
            EC_GROUP_free(dctx->gen_group);
        if (dctx->co_key)
            EC_KEY_free(dctx->co_key);
        if (dctx->kdf_ukm)
            OPENSSL_free(dctx->kdf_ukm);
        OPENSSL_free(dctx);
    }
}

static int pkey_ec_sign(EVP_PKEY_CTX *ctx, unsigned char *sig, size_t *siglen,
                        const unsigned char *tbs, size_t tbslen)
{
    int ret, type;
    unsigned int sltmp;
    EC_PKEY_CTX *dctx = ctx->data;
    EC_KEY *ec = ctx->pkey->pkey.ec;

    if (!sig) {
        *siglen = ECDSA_size(ec);
        return 1;
    } else if (*siglen < (size_t)ECDSA_size(ec)) {
        ECerr(EC_F_PKEY_EC_SIGN, EC_R_BUFFER_TOO_SMALL);
        return 0;
    }

    if (dctx->md)
        type = EVP_MD_type(dctx->md);
    else
        type = NID_sha1;

    ret = ECDSA_sign(type, tbs, tbslen, sig, &sltmp, ec);

    if (ret <= 0)
        return ret;
    *siglen = (size_t)sltmp;
    return 1;
}

static int pkey_ec_verify(EVP_PKEY_CTX *ctx,
                          const unsigned char *sig, size_t siglen,
                          const unsigned char *tbs, size_t tbslen)
{
    int ret, type;
    EC_PKEY_CTX *dctx = ctx->data;
    EC_KEY *ec = ctx->pkey->pkey.ec;

    if (dctx->md)
        type = EVP_MD_type(dctx->md);
    else
        type = NID_sha1;

    ret = ECDSA_verify(type, tbs, tbslen, sig, siglen, ec);

    return ret;
}

#ifndef OPENSSL_NO_ECDH
static int pkey_ec_derive(EVP_PKEY_CTX *ctx, unsigned char *key,
                          size_t *keylen)
{
    int ret;
    size_t outlen;
    const EC_POINT *pubkey = NULL;
    EC_KEY *eckey;
    EC_PKEY_CTX *dctx = ctx->data;
    if (!ctx->pkey || !ctx->peerkey) {
        ECerr(EC_F_PKEY_EC_DERIVE, EC_R_KEYS_NOT_SET);
        return 0;
    }

    eckey = dctx->co_key ? dctx->co_key : ctx->pkey->pkey.ec;

    if (!key) {
        const EC_GROUP *group;
        group = EC_KEY_get0_group(eckey);
        *keylen = (EC_GROUP_get_degree(group) + 7) / 8;
        return 1;
    }
    pubkey = EC_KEY_get0_public_key(ctx->peerkey->pkey.ec);

    /*
     * NB: unlike PKCS#3 DH, if *outlen is less than maximum size this is not
     * an error, the result is truncated.
     */

    outlen = *keylen;

    ret = ECDH_compute_key(key, outlen, pubkey, eckey, 0);
    if (ret <= 0)
        return 0;
    *keylen = ret;
    return 1;
}

static int pkey_ec_kdf_derive(EVP_PKEY_CTX *ctx,
                              unsigned char *key, size_t *keylen)
{
    EC_PKEY_CTX *dctx = ctx->data;
    unsigned char *ktmp = NULL;
    size_t ktmplen;
    int rv = 0;
    if (dctx->kdf_type == EVP_PKEY_ECDH_KDF_NONE)
        return pkey_ec_derive(ctx, key, keylen);
    if (!key) {
        *keylen = dctx->kdf_outlen;
        return 1;
    }
    if (*keylen != dctx->kdf_outlen)
        return 0;
    if (!pkey_ec_derive(ctx, NULL, &ktmplen))
        return 0;
    ktmp = OPENSSL_malloc(ktmplen);
    if (!ktmp)
        return 0;
    if (!pkey_ec_derive(ctx, ktmp, &ktmplen))
        goto err;
    /* Do KDF stuff */
    if (!ECDH_KDF_X9_62(key, *keylen, ktmp, ktmplen,
                        dctx->kdf_ukm, dctx->kdf_ukmlen, dctx->kdf_md))
        goto err;
    rv = 1;

 err:
    if (ktmp) {
        OPENSSL_cleanse(ktmp, ktmplen);
        OPENSSL_free(ktmp);
    }
    return rv;
}
#endif

static int pkey_ec_ctrl(EVP_PKEY_CTX *ctx, int type, int p1, void *p2)
{
    EC_PKEY_CTX *dctx = ctx->data;
    EC_GROUP *group;
    switch (type) {
    case EVP_PKEY_CTRL_EC_PARAMGEN_CURVE_NID:
        group = EC_GROUP_new_by_curve_name(p1);
        if (group == NULL) {
            ECerr(EC_F_PKEY_EC_CTRL, EC_R_INVALID_CURVE);
            return 0;
        }
        if (dctx->gen_group)
            EC_GROUP_free(dctx->gen_group);
        dctx->gen_group = group;
        return 1;

    case EVP_PKEY_CTRL_EC_PARAM_ENC:
        if (!dctx->gen_group) {
            ECerr(EC_F_PKEY_EC_CTRL, EC_R_NO_PARAMETERS_SET);
            return 0;
        }
        EC_GROUP_set_asn1_flag(dctx->gen_group, p1);
        return 1;

#ifndef OPENSSL_NO_ECDH
    case EVP_PKEY_CTRL_EC_ECDH_COFACTOR:
        if (p1 == -2) {
            if (dctx->cofactor_mode != -1)
                return dctx->cofactor_mode;
            else {
                EC_KEY *ec_key = ctx->pkey->pkey.ec;
                return EC_KEY_get_flags(ec_key) & EC_FLAG_COFACTOR_ECDH ? 1 :
                    0;
            }
        } else if (p1 < -1 || p1 > 1)
            return -2;
        dctx->cofactor_mode = p1;
        if (p1 != -1) {
            EC_KEY *ec_key = ctx->pkey->pkey.ec;
            if (!ec_key->group)
                return -2;
            /* If cofactor is 1 cofactor mode does nothing */
            if (BN_is_one(&ec_key->group->cofactor))
                return 1;
            if (!dctx->co_key) {
                dctx->co_key = EC_KEY_dup(ec_key);
                if (!dctx->co_key)
                    return 0;
            }
            if (p1)
                EC_KEY_set_flags(dctx->co_key, EC_FLAG_COFACTOR_ECDH);
            else
                EC_KEY_clear_flags(dctx->co_key, EC_FLAG_COFACTOR_ECDH);
        } else if (dctx->co_key) {
            EC_KEY_free(dctx->co_key);
            dctx->co_key = NULL;
        }
        return 1;
#endif

    case EVP_PKEY_CTRL_EC_KDF_TYPE:
        if (p1 == -2)
            return dctx->kdf_type;
        if (p1 != EVP_PKEY_ECDH_KDF_NONE && p1 != EVP_PKEY_ECDH_KDF_X9_62)
            return -2;
        dctx->kdf_type = p1;
        return 1;

    case EVP_PKEY_CTRL_EC_KDF_MD:
        dctx->kdf_md = p2;
        return 1;

    case EVP_PKEY_CTRL_GET_EC_KDF_MD:
        *(const EVP_MD **)p2 = dctx->kdf_md;
        return 1;

    case EVP_PKEY_CTRL_EC_KDF_OUTLEN:
        if (p1 <= 0)
            return -2;
        dctx->kdf_outlen = (size_t)p1;
        return 1;

    case EVP_PKEY_CTRL_GET_EC_KDF_OUTLEN:
        *(int *)p2 = dctx->kdf_outlen;
        return 1;

    case EVP_PKEY_CTRL_EC_KDF_UKM:
        if (dctx->kdf_ukm)
            OPENSSL_free(dctx->kdf_ukm);
        dctx->kdf_ukm = p2;
        if (p2)
            dctx->kdf_ukmlen = p1;
        else
            dctx->kdf_ukmlen = 0;
        return 1;

    case EVP_PKEY_CTRL_GET_EC_KDF_UKM:
        *(unsigned char **)p2 = dctx->kdf_ukm;
        return dctx->kdf_ukmlen;

    case EVP_PKEY_CTRL_MD:
        if (EVP_MD_type((const EVP_MD *)p2) != NID_sha1 &&
            EVP_MD_type((const EVP_MD *)p2) != NID_ecdsa_with_SHA1 &&
            EVP_MD_type((const EVP_MD *)p2) != NID_sha224 &&
            EVP_MD_type((const EVP_MD *)p2) != NID_sha256 &&
#ifndef OPENSSL_NO_GMSSL
            EVP_MD_type((const EVP_MD *)p2) != NID_sm3 &&
#endif
            EVP_MD_type((const EVP_MD *)p2) != NID_sha384 &&
            EVP_MD_type((const EVP_MD *)p2) != NID_sha512) {
            ECerr(EC_F_PKEY_EC_CTRL, EC_R_INVALID_DIGEST_TYPE);
            return 0;
        }
        dctx->md = p2;
        return 1;

    case EVP_PKEY_CTRL_GET_MD:
        *(const EVP_MD **)p2 = dctx->md;
        return 1;

    case EVP_PKEY_CTRL_PEER_KEY:
        /* Default behaviour is OK */
    case EVP_PKEY_CTRL_DIGESTINIT:
    case EVP_PKEY_CTRL_PKCS7_SIGN:
    case EVP_PKEY_CTRL_CMS_SIGN:
        return 1;

    default:
        return -2;

    }
}

static int pkey_ec_ctrl_str(EVP_PKEY_CTX *ctx,
                            const char *type, const char *value)
{
    if (!strcmp(type, "ec_paramgen_curve")) {
        int nid;
        nid = EC_curve_nist2nid(value);
        if (nid == NID_undef)
            nid = OBJ_sn2nid(value);
        if (nid == NID_undef)
            nid = OBJ_ln2nid(value);
        if (nid == NID_undef) {
            ECerr(EC_F_PKEY_EC_CTRL_STR, EC_R_INVALID_CURVE);
            return 0;
        }
        return EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, nid);
    } else if (!strcmp(type, "ec_param_enc")) {
        int param_enc;
        if (!strcmp(value, "explicit"))
            param_enc = 0;
        else if (!strcmp(value, "named_curve"))
            param_enc = OPENSSL_EC_NAMED_CURVE;
        else
            return -2;
        return EVP_PKEY_CTX_set_ec_param_enc(ctx, param_enc);
    } else if (!strcmp(type, "ecdh_kdf_md")) {
        const EVP_MD *md;
        if (!(md = EVP_get_digestbyname(value))) {
            ECerr(EC_F_PKEY_EC_CTRL_STR, EC_R_INVALID_DIGEST);
            return 0;
        }
        return EVP_PKEY_CTX_set_ecdh_kdf_md(ctx, md);
    } else if (!strcmp(type, "ecdh_cofactor_mode")) {
        int co_mode;
        co_mode = atoi(value);
        return EVP_PKEY_CTX_set_ecdh_cofactor_mode(ctx, co_mode);
    }

    return -2;
}

static int pkey_ec_paramgen(EVP_PKEY_CTX *ctx, EVP_PKEY *pkey)
{
    EC_KEY *ec = NULL;
    EC_PKEY_CTX *dctx = ctx->data;
    int ret = 0;
    if (dctx->gen_group == NULL) {
        ECerr(EC_F_PKEY_EC_PARAMGEN, EC_R_NO_PARAMETERS_SET);
        return 0;
    }
    ec = EC_KEY_new();
    if (!ec)
        return 0;
    ret = EC_KEY_set_group(ec, dctx->gen_group);
    if (ret)
        EVP_PKEY_assign_EC_KEY(pkey, ec);
    else
        EC_KEY_free(ec);
    return ret;
}

static int pkey_ec_keygen(EVP_PKEY_CTX *ctx, EVP_PKEY *pkey)
{
    EC_KEY *ec = NULL;
    EC_PKEY_CTX *dctx = ctx->data;
    if (ctx->pkey == NULL && dctx->gen_group == NULL) {
        ECerr(EC_F_PKEY_EC_KEYGEN, EC_R_NO_PARAMETERS_SET);
        return 0;
    }
    ec = EC_KEY_new();
    if (!ec)
        return 0;
    EVP_PKEY_assign_EC_KEY(pkey, ec);
    if (ctx->pkey) {
        /* Note: if error return, pkey is freed by parent routine */
        if (!EVP_PKEY_copy_parameters(pkey, ctx->pkey))
            return 0;
    } else {
        if (!EC_KEY_set_group(ec, dctx->gen_group))
            return 0;
    }
    return EC_KEY_generate_key(pkey->pkey.ec);
}

#ifndef OPENSSL_NO_ECIES
static int pkey_ec_encrypt(EVP_PKEY_CTX *ctx, unsigned char *out, size_t *outlen,
	const unsigned char *in, size_t inlen)
{
	EC_KEY *ec_key = ctx->pkey->pkey.ec;
	ECIES_PARAMS *param = ECIES_get_parameters(ec_key);
	OPENSSL_assert(param);
	return ECIES_encrypt(out, outlen, param, in, inlen, ec_key);
}

static int pkey_ec_decrypt(EVP_PKEY_CTX *ctx, unsigned char *out, size_t *outlen,
	const unsigned char *in, size_t inlen)
{
	EC_KEY *ec_key = ctx->pkey->pkey.ec;
	ECIES_PARAMS *param = ECIES_get_parameters(ec_key);
	OPENSSL_assert(param);
	return ECIES_decrypt(out, outlen, param, in, inlen, ec_key);
}
#endif

const EVP_PKEY_METHOD ec_pkey_meth = {
    EVP_PKEY_EC,
    0,
    pkey_ec_init,
    pkey_ec_copy,
    pkey_ec_cleanup,

    0,
    pkey_ec_paramgen,

    0,
    pkey_ec_keygen,

    0,
    pkey_ec_sign,

    0,
    pkey_ec_verify,

    0, 0,

    0, 0, 0, 0,

    0,
#ifndef OPENSSL_NO_ECIES
    pkey_ec_encrypt,
#else
    0,
#endif

    0,
#ifndef OPENSSL_NO_ECIES
    pkey_ec_decrypt,
#else
    0,
#endif

    0,
#ifndef OPENSSL_NO_ECDH
    pkey_ec_kdf_derive,
#else
    0,
#endif

    pkey_ec_ctrl,
    pkey_ec_ctrl_str
};

#ifndef OPENSSL_NO_SM2

static int pkey_sm2_init(EVP_PKEY_CTX *ctx)
{
    EC_PKEY_CTX *dctx;
    dctx = OPENSSL_malloc(sizeof(EC_PKEY_CTX));
    if (!dctx)
        return 0;
    dctx->gen_group = EC_GROUP_new_by_curve_name(NID_sm2p256v1);
    if (dctx->gen_group == NULL) {
        return 0;
    }
    dctx->md = NULL; //FIXME: sm3

    dctx->cofactor_mode = -1;
    dctx->co_key = NULL;
    dctx->kdf_type = EVP_PKEY_ECDH_KDF_NONE;
    dctx->kdf_md = NULL;
    dctx->kdf_outlen = 0;
    dctx->kdf_ukm = NULL;
    dctx->kdf_ukmlen = 0;

    ctx->data = dctx;

    return 1;
}

static int pkey_sm2_keygen(EVP_PKEY_CTX *ctx, EVP_PKEY *pkey)
{
    EC_KEY *ec = NULL;
    EC_PKEY_CTX *dctx = ctx->data;
   
     if (ctx->pkey == NULL && dctx->gen_group == NULL) {
        ECerr(EC_F_PKEY_EC_KEYGEN, EC_R_NO_PARAMETERS_SET);
        return 0;
    }
    ec = EC_KEY_new();
    if (!ec)
        return 0;
    EVP_PKEY_assign_SM2(pkey, ec);
    if (ctx->pkey) {
        /* Note: if error return, pkey is freed by parent routine */
        if (!EVP_PKEY_copy_parameters(pkey, ctx->pkey))
            return 0;
    } else {
        if (!EC_KEY_set_group(ec, dctx->gen_group))
            return 0;
    }
    return EC_KEY_generate_key(pkey->pkey.ec);
}


static int pkey_sm2_sign(EVP_PKEY_CTX *ctx, unsigned char *sig, size_t *siglen,
	const unsigned char *dgst, size_t dgstlen)
{
	int ret;
	EC_PKEY_CTX *ec_ctx = ctx->data;
	EC_KEY *ec_key = ctx->pkey->pkey.ec;
	int type = NID_sm3;
	size_t len;

	if (!sig) {
		*siglen = SM2_signature_size(ec_key);
		return 1;
	}
	if (*siglen < (size_t)SM2_signature_size(ec_key)) {
		ECerr(EC_F_PKEY_SM2_SIGN, EC_R_BUFFER_TOO_SMALL);
		return 0;
	}

	if ((ret = SM2_sign(type, dgst, dgstlen, sig, &len, ec_key)) <= 0) {
		return ret;
	}

	*siglen = len;
	return 1;
}

static int pkey_sm2_verify(EVP_PKEY_CTX *ctx,
	const unsigned char *sig, size_t siglen,
	const unsigned char *dgst, size_t dgstlen)
{
	EC_PKEY_CTX *ec_ctx = ctx->data;
	EC_KEY *ec_key = ctx->pkey->pkey.ec;
	int type = ec_ctx->md ? EVP_MD_type(ec_ctx->md) : NID_sm3;

	return SM2_verify(type, dgst, dgstlen, sig, siglen, ec_key);
}

static int pkey_sm2_signctx_init(EVP_PKEY_CTX *ctx, EVP_MD_CTX *mctx)
{
	int ret = 0;
	EC_PKEY_CTX *ec_ctx = ctx->data;
	EC_KEY *ec_key = ctx->pkey->pkey.ec;
	const EVP_MD *md = EVP_sm3();
	unsigned char zid[EVP_MAX_MD_SIZE];
	unsigned int zidlen = sizeof(zid);

	if (!SM2_compute_id_digest(md, zid, &zidlen, ec_key)) {
        	ECerr(EC_F_PKEY_SM2_SIGNCTX_INIT, ERR_R_SM2_LIB);
		return 0;
	}
	if (!mctx->update(mctx, zid, zidlen)) {
        	ECerr(EC_F_PKEY_SM2_SIGNCTX_INIT, ERR_R_EVP_LIB);
		return 0;
	}

	return 1;
}

static int pkey_sm2_signctx(EVP_PKEY_CTX *ctx,
	unsigned char *sig, size_t *siglen, EVP_MD_CTX *mctx)
{
	EC_PKEY_CTX *ec_ctx = ctx->data;
	EC_KEY *ec_key = ctx->pkey->pkey.ec;
	unsigned char dgst[EVP_MAX_MD_SIZE];
	unsigned int dgstlen;
	int type = NID_undef;

	if (!sig) {
		*siglen = SM2_signature_size(ec_key);
		return 1;
	}

	if (*siglen < (size_t)SM2_signature_size(ec_key)) {
		ECerr(EC_F_PKEY_SM2_SIGNCTX, EC_R_BUFFER_TOO_SMALL);
		return 0;
	}

	if (!EVP_DigestFinal_ex(mctx, dgst, &dgstlen)) {
		ECerr(EC_F_PKEY_SM2_SIGNCTX, ERR_R_EVP_LIB);
		return 0;
	}

	return SM2_sign(type, dgst, dgstlen, sig, &siglen, ec_key);
}

static int pkey_sm2_verifyctx_init(EVP_PKEY_CTX *ctx, EVP_MD_CTX *mctx)
{
	int ret = 0;
	EC_PKEY_CTX *ec_ctx = ctx->data;
	EC_KEY *ec_key = ctx->pkey->pkey.ec;
	const EVP_MD *md = EVP_sm3(); // FIXME: we need to get md from somewhere
	unsigned char zid[EVP_MAX_MD_SIZE];
	unsigned int zidlen;

	zidlen = sizeof(zid);
	if (!SM2_compute_id_digest(md, zid, &zidlen, ec_key)) {
		goto end;
	}
	if (!mctx->update(mctx, zid, zidlen)) {
		goto end;
	}

	ret = 1;
end:
	return ret;
}

static int pkey_sm2_verifyctx(EVP_PKEY_CTX *ctx,
	const unsigned char *sig, int siglen, EVP_MD_CTX *mctx)
{
	unsigned char dgst[EVP_MAX_MD_SIZE];
	size_t dgstlen;
	EC_PKEY_CTX *ec_ctx = ctx->data;
	EC_KEY *ec_key = ctx->pkey->pkey.ec;
	int type = ec_ctx->md ? EVP_MD_type(ec_ctx->md) : NID_sm3;

	dgstlen = sizeof(dgst);
	if (!EVP_DigestFinal_ex(mctx, dgst, &dgstlen)) {
		return -1;
	}

	return SM2_verify(type, dgst, dgstlen, sig, siglen, ec_key);
}

static int pkey_sm2_encrypt(EVP_PKEY_CTX *ctx,
	unsigned char *out, size_t *outlen,
	const unsigned char *in, size_t inlen)
{
	EC_PKEY_CTX *ec_ctx = ctx->data;
	EC_KEY *ec_key = ctx->pkey->pkey.ec;
	const EVP_MD *kdf_md = ec_ctx->kdf_md;
	const EVP_MD *mac_md = ec_ctx->md;
	point_conversion_form_t point_form = SM2_DEFAULT_POINT_CONVERSION_FORM;

	//FIXME: the ec_ctx is not work, no one init it
	kdf_md = EVP_sm3();
	mac_md = EVP_sm3();
	

	//FIXME: where to put the parameters?
	return SM2_encrypt(in, inlen, out, outlen, ec_key);
}

static int pkey_sm2_decrypt(EVP_PKEY_CTX *ctx,
	unsigned char *out, size_t *outlen,
	const unsigned char *in, size_t inlen)
{
	EC_PKEY_CTX *ec_ctx = ctx->data;
	EC_KEY *ec_key = ctx->pkey->pkey.ec;
	const EVP_MD *kdf_md = ec_ctx->kdf_md;
	const EVP_MD *mac_md = ec_ctx->md;
	point_conversion_form_t point_form = SM2_DEFAULT_POINT_CONVERSION_FORM;


	return SM2_decrypt(in, inlen, out, outlen, ec_key);
}

static int pkey_sm2_ctrl_digestinit(EVP_PKEY_CTX *pk_ctx, EVP_MD_CTX *md_ctx)
{
	int ret = 0;
	EC_KEY *ec_key = pk_ctx->pkey->pkey.ec;
	const EVP_MD *md = EVP_MD_CTX_md(md_ctx);
	char *id;
	unsigned char zid[EVP_MAX_MD_SIZE];
	unsigned int zidlen = sizeof(zid);

	EVP_PKEY_CTX *pctx;

	fprintf(stderr, "%s() called\n", __FUNCTION__);

	/*
	if (!(id = SM2_get_id(ec_key))) {
		fprintf(stderr, "error: %s %d\n", __FILE__, __LINE__);
		id = "alice@pku.edu.cn";
		//return 0;
	}
	*/

	//FIXME: check this function
	if (!SM2_compute_id_digest(md, zid, &zidlen, ec_key)) {
		fprintf(stderr, "error: %s %d\n", __FILE__, __LINE__);
		return 0;
	}

	pctx = md_ctx->pctx;
	md_ctx->pctx = NULL;
	
	if (!EVP_DigestInit_ex(md_ctx, md, NULL)) {
		fprintf(stderr, "error: %s %d\n", __FILE__, __LINE__);
		goto end;
	}

	md_ctx->pctx = pctx;
		
	if (!EVP_DigestUpdate(md_ctx, zid, zidlen)) {
		fprintf(stderr, "error: %s %d\n", __FILE__, __LINE__);
		goto end;
	}

	EVP_MD_CTX_set_flags(md_ctx, EVP_MD_CTX_FLAG_NO_INIT);

	ret = 1;
end:
	return ret;
}

static int pkey_sm2_derive_init(EVP_PKEY_CTX *ctx)
{
	return 0;
}

static int pkey_sm2_derive(EVP_PKEY_CTX *ctx, unsigned char *key, size_t *keylen)
{
	return 0;
}

static int pkey_sm2_ctrl(EVP_PKEY_CTX *pk_ctx, int type, int p1, void *p2)
{
	switch (type) {
	case EVP_PKEY_CTRL_DIGESTINIT:
		return pkey_sm2_ctrl_digestinit(pk_ctx, (EVP_MD_CTX *)p2);
	case EVP_PKEY_CTRL_MD:
		return 1;
        }

	return pkey_ec_ctrl(pk_ctx, type, p1, p2);
}

const EVP_PKEY_METHOD sm2_pkey_meth = {
	EVP_PKEY_SM2,
	0,
	pkey_sm2_init,
	pkey_ec_copy,
	pkey_ec_cleanup,
	0,
	pkey_ec_paramgen,
	0,
	pkey_sm2_keygen,
	0,
	pkey_sm2_sign,
	0,
	pkey_sm2_verify,
	0,
	0,
	pkey_sm2_signctx_init,
	pkey_sm2_signctx,
	pkey_sm2_verifyctx_init,
	pkey_sm2_verifyctx,
	0,
	pkey_sm2_encrypt,
	0,
	pkey_sm2_decrypt,
	pkey_sm2_derive_init,
	pkey_sm2_derive,
	pkey_ec_ctrl,
	pkey_ec_ctrl_str
};
#endif

