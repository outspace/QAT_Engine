/* ====================================================================
 *
 *
 *   BSD LICENSE
 *
 *   Copyright(c) 2016 Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * ====================================================================
 */

/*****************************************************************************
 * @file qat_ec.c
 *
 * This file provides support for ECDH & ECDSA
 *
 *****************************************************************************/

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#include <pthread.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <openssl/ecdh.h>
#include <openssl/async.h>
#include <openssl/err.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/bn.h>
#include <openssl/rand.h>

#include "cpa.h"
#include "cpa_types.h"
#include "cpa_cy_ec.h"
#include "cpa_cy_ecdsa.h"
#include "e_qat.h"
#include "qat_asym_common.h"
#ifdef USE_QAT_CONTIG_MEM
# include "qae_mem_utils.h"
#endif
#ifdef USE_QAE_MEM
# include "cmn_mem_drv_inf.h"
#endif
#include "e_qat_err.h"
#include "qat_utils.h"
#include "qat_ec.h"

#ifdef OPENSSL_ENABLE_QAT_ECDSA
# ifdef OPENSSL_DISABLE_QAT_ECDSA
#  undef OPENSSL_DISABLE_QAT_ECDSA
# endif
#endif

#ifdef OPENSSL_ENABLE_QAT_ECDH
# ifdef OPENSSL_DISABLE_QAT_ECDH
#  undef OPENSSL_DISABLE_QAT_ECDH
# endif
#endif


static int qat_ecdsa_sign(int type, const unsigned char *dgst, int dlen,
                          unsigned char *sig, unsigned int *siglen,
                          const BIGNUM *kinv, const BIGNUM *r, EC_KEY *eckey);

static ECDSA_SIG *qat_ecdsa_do_sign(const unsigned char *dgst, int dlen,
                                    const BIGNUM *in_kinv, const BIGNUM *in_r,
                                    EC_KEY *eckey);

static int qat_ecdsa_verify(int type, const unsigned char *dgst, int dgst_len,
                            const unsigned char *sigbuf, int sig_len, EC_KEY *eckey);

static int qat_ecdsa_do_verify(const unsigned char *dgst, int dgst_len,
                               const ECDSA_SIG *sig, EC_KEY *eckey);

/* Qat engine ECDH methods declaration */
static int qat_ecdh_compute_key(unsigned char **outX, size_t *outlenX,
                                unsigned char **outY, size_t *outlenY,
                                const EC_POINT *pub_key, const EC_KEY *ecdh);

static int qat_engine_ecdh_compute_key(unsigned char **out, size_t *outlen,
                                       const EC_POINT *pub_key, const EC_KEY *ecdh);

static int qat_ecdh_generate_key(EC_KEY *ecdh);

typedef int (*PFUNC_COMP_KEY)(unsigned char **,
                              size_t *,
                              const EC_POINT *,
                              const EC_KEY *);

typedef int (*PFUNC_GEN_KEY)(EC_KEY *);

typedef int (*PFUNC_SIGN)(int,
                          const unsigned char *,
                          int,
                          unsigned char *,
                          unsigned int *,
                          const BIGNUM *,
                          const BIGNUM *,
                          EC_KEY *);

typedef int (*PFUNC_SIGN_SETUP)(EC_KEY *,
                                BN_CTX *,
                                BIGNUM **,
                                BIGNUM **);

typedef ECDSA_SIG *(*PFUNC_SIGN_SIG)(const unsigned char *,
                                     int,
                                     const BIGNUM *,
                                     const BIGNUM *,
                                     EC_KEY *);

typedef int (*PFUNC_VERIFY)(int,
                            const unsigned char *,
                            int,
                            const unsigned char *,
                            int,
                            EC_KEY *);

typedef int (*PFUNC_VERIFY_SIG)(const unsigned char *,
                                int,
                                const ECDSA_SIG *,
                                EC_KEY *eckey);

static EC_KEY_METHOD *qat_ec_method = NULL;

EC_KEY_METHOD *qat_get_EC_methods(void)
{
    if (qat_ec_method != NULL)
        return qat_ec_method;

#if defined (OPENSSL_DISABLE_QAT_ECDSA) || defined (OPENSSL_DISABLE_QAT_ECDH)
    EC_KEY_METHOD *def_ec_meth = (EC_KEY_METHOD *)EC_KEY_get_default_method();
#endif
#ifdef OPENSSL_DISABLE_QAT_ECDSA
    PFUNC_SIGN sign_pfunc = NULL;
    PFUNC_SIGN_SETUP sign_setup_pfunc = NULL;
    PFUNC_SIGN_SIG sign_sig_pfunc = NULL;
    PFUNC_VERIFY verify_pfunc = NULL;
    PFUNC_VERIFY_SIG verify_sig_pfunc = NULL;
#endif
#ifdef OPENSSL_DISABLE_QAT_ECDH
    PFUNC_COMP_KEY comp_key_pfunc = NULL;
    PFUNC_GEN_KEY gen_key_pfunc = NULL;
#endif

    if ((qat_ec_method = EC_KEY_METHOD_new(qat_ec_method)) == NULL) {
        QATerr(QAT_F_QAT_GET_EC_METHODS, ERR_R_INTERNAL_ERROR);
        return NULL;
    }

#ifndef OPENSSL_DISABLE_QAT_ECDSA
    EC_KEY_METHOD_set_sign(qat_ec_method,
                           qat_ecdsa_sign,
                           NULL,
                           qat_ecdsa_do_sign);
    EC_KEY_METHOD_set_verify(qat_ec_method,
                             qat_ecdsa_verify,
                             qat_ecdsa_do_verify);
#else
    EC_KEY_METHOD_get_sign(def_ec_meth,
                           &sign_pfunc,
                           &sign_setup_pfunc,
                           &sign_sig_pfunc);
    EC_KEY_METHOD_set_sign(qat_ec_method,
                           sign_pfunc,
                           sign_setup_pfunc,
                           sign_sig_pfunc);
    EC_KEY_METHOD_get_verify(def_ec_meth,
                             &verify_pfunc,
                             &verify_sig_pfunc);
    EC_KEY_METHOD_set_verify(qat_ec_method,
                             verify_pfunc,
                             verify_sig_pfunc);
#endif

#ifndef OPENSSL_DISABLE_QAT_ECDH
    EC_KEY_METHOD_set_keygen(qat_ec_method, qat_ecdh_generate_key);
    EC_KEY_METHOD_set_compute_key(qat_ec_method, qat_engine_ecdh_compute_key);
#else
    EC_KEY_METHOD_get_keygen(def_ec_meth, &gen_key_pfunc);
    EC_KEY_METHOD_set_keygen(qat_ec_method, gen_key_pfunc);
    EC_KEY_METHOD_get_compute_key(def_ec_meth, &comp_key_pfunc);
    EC_KEY_METHOD_set_compute_key(qat_ec_method, comp_key_pfunc);
#endif

    return qat_ec_method;
}

void qat_free_EC_methods(void)
{
    if (NULL != qat_ec_method) {
        EC_KEY_METHOD_free(qat_ec_method);
        qat_ec_method = NULL;
    } else {
        QATerr(QAT_F_QAT_FREE_EC_METHODS, ERR_R_INTERNAL_ERROR);
    }
}

/* Callback to indicate QAT completion of EC point multiply */
void qat_ecCallbackFn(void *pCallbackTag, CpaStatus status, void *pOpData,
                      CpaBoolean multiplyStatus, CpaFlatBuffer * pXk,
                      CpaFlatBuffer * pYk)
{
    qat_crypto_callbackFn(pCallbackTag, status, CPA_CY_SYM_OP_CIPHER, pOpData,
                          NULL, multiplyStatus);
}

int qat_ecdh_compute_key(unsigned char **outX, size_t *outlenX,
                         unsigned char **outY, size_t *outlenY,
                         const EC_POINT *pub_key, const EC_KEY *ecdh)
{
    BN_CTX *ctx = NULL;
    BIGNUM *p = NULL, *a = NULL, *b = NULL;
    BIGNUM *xg = NULL, *yg = NULL;
    const BIGNUM *priv_key;
    const EC_GROUP *group;
    int ret = -1;
    size_t buflen;
    PFUNC_COMP_KEY comp_key_pfunc = NULL;

    CpaInstanceHandle instanceHandle;
    CpaCyEcPointMultiplyOpData *opData = NULL;
    CpaBoolean bEcStatus;
    CpaFlatBuffer *pResultX = NULL;
    CpaFlatBuffer *pResultY = NULL;
    int qatPerformOpRetries = 0;
    useconds_t ulPollInterval = getQatPollInterval();
    int iMsgRetry = getQatMsgRetryCount();
    CpaStatus status;
    struct op_done op_done;

    DEBUG("%s been called \n", __func__);

    if (ecdh == NULL || (priv_key = EC_KEY_get0_private_key(ecdh)) == NULL) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_PASSED_NULL_PARAMETER);
        return ret;
    }

    if ((group = EC_KEY_get0_group(ecdh)) == NULL) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_PASSED_NULL_PARAMETER);
        return ret;
    }

    /* Unsupported curve: X25519.
     * Detect and call it's software implementation.
     */
    if (EC_GROUP_get_curve_name(group) == NID_X25519) {
        EC_KEY_METHOD_get_compute_key((EC_KEY_METHOD *) EC_KEY_OpenSSL(), &comp_key_pfunc);
        if (comp_key_pfunc == NULL) {
            QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
            return ret;
        }
        return (*comp_key_pfunc)(outX, outlenX, pub_key, ecdh);
    }

    opData = (CpaCyEcPointMultiplyOpData *)
        OPENSSL_malloc(sizeof(CpaCyEcPointMultiplyOpData));
    if (opData == NULL) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_MALLOC_FAILURE);
        return ret;
    }

    opData->k.pData = NULL;
    opData->xg.pData = NULL;
    opData->yg.pData = NULL;
    opData->a.pData = NULL;
    opData->b.pData = NULL;
    opData->q.pData = NULL;

    /* To instruct the Quickassist API not to use co-factor */
    opData->h.pData = NULL;
    opData->h.dataLenInBytes = 0;

    /* Populate the parameters required for EC point multiply */
    if ((ctx = BN_CTX_new()) == NULL) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    BN_CTX_start(ctx);
    if ((p = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((a = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((b = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((xg = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((yg = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    buflen = (EC_GROUP_get_degree(group) + 7) / 8;
    pResultX = (CpaFlatBuffer *) OPENSSL_malloc(sizeof(CpaFlatBuffer));
    if (pResultX == NULL) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_MALLOC_FAILURE);
        goto err;
    }
    pResultX->pData = qaeCryptoMemAlloc(buflen, __FILE__, __LINE__);
    if (pResultX->pData == NULL) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_MALLOC_FAILURE);
        goto err;
    }
    pResultX->dataLenInBytes = (Cpa32U) buflen;
    pResultY = (CpaFlatBuffer *) OPENSSL_malloc(sizeof(CpaFlatBuffer));
    if (!pResultY) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_MALLOC_FAILURE);
        goto err;
    }
    pResultY->pData = qaeCryptoMemAlloc(buflen, __FILE__, __LINE__);
    if (pResultY->pData == NULL) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_MALLOC_FAILURE);
        goto err;
    }
    pResultY->dataLenInBytes = (Cpa32U) buflen;

    if ((qat_BN_to_FB(&(opData->k), (BIGNUM *)priv_key)) != 1) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if (EC_METHOD_get_field_type(EC_GROUP_method_of(group)) ==
        NID_X9_62_prime_field) {
        if (!EC_GROUP_get_curve_GFp(group, p, a, b, ctx)) {
            QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
            goto err;
        }

        if (!EC_POINT_get_affine_coordinates_GFp(group, pub_key, xg, yg, ctx)) {
            QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
            goto err;
        }
        opData->fieldType = CPA_CY_EC_FIELD_TYPE_PRIME;
    } else {
        if ((!EC_GROUP_get_curve_GF2m(group, p, a, b, ctx)) ||
            (!EC_POINT_get_affine_coordinates_GF2m(group, pub_key,
                                                   xg, yg, ctx))) {
            QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
            goto err;
        }
        opData->fieldType = CPA_CY_EC_FIELD_TYPE_BINARY;
    }
    if ((qat_BN_to_FB(&(opData->xg), xg) != 1) ||
        (qat_BN_to_FB(&(opData->yg), yg) != 1) ||
        (qat_BN_to_FB(&(opData->a), a) != 1)) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    /*
     * This is a special handling required for curves with 'a' co-efficient
     * of 0. The translation to a flatbuffer results in a zero sized field
     * but the Quickassist API expects a flatbuffer of size 1 with a value
     * of zero. As a special case we will create that manually.
     */
    if (opData->a.pData == NULL && opData->a.dataLenInBytes == 0) {
        opData->a.pData = qaeCryptoMemAlloc(1, __FILE__, __LINE__);
        if (opData->a.pData == NULL) {
            QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_MALLOC_FAILURE);
            goto err;
        }
        opData->a.dataLenInBytes = 1;
        if (opData->a.pData) {
            opData->a.pData[0] = 0;
        }
    }
    if ((qat_BN_to_FB(&(opData->b), b) != 1) ||
        (qat_BN_to_FB(&(opData->q), p) != 1)) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    initOpDone(&op_done);
    if (op_done.job) {
        if (qat_setup_async_event_notification(0) == 0) {
            QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
            cleanupOpDone(&op_done);
            goto err;
        }
    }
    CRYPTO_QAT_LOG("KX - %s\n", __func__);

    /* Invoke the crypto engine API for EC Point Multiply */
    do {
        if ((instanceHandle = get_next_inst()) == NULL) {
            QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
            cleanupOpDone(&op_done);
            goto err;
        }

        CRYPTO_QAT_LOG("KX - %s\n", __func__);
        status = cpaCyEcPointMultiply(instanceHandle,
                                      qat_ecCallbackFn,
                                      &op_done,
                                      opData,
                                      &bEcStatus, pResultX, pResultY);

        if (status == CPA_STATUS_RETRY) {
            if (op_done.job == NULL) {
                usleep(ulPollInterval +
                       (qatPerformOpRetries %
                        QAT_RETRY_BACKOFF_MODULO_DIVISOR));
                qatPerformOpRetries++;
                if (iMsgRetry != QAT_INFINITE_MAX_NUM_RETRIES) {
                    if (qatPerformOpRetries >= iMsgRetry) {
                        break;
                    }
                }
            } else {
                if ((qat_wake_job(op_done.job, 0) == 0) ||
                    (qat_pause_job(op_done.job, 0) == 0)) {
                    status = CPA_STATUS_FAIL;
                    break;
                }
            }
        }
    }
    while (status == CPA_STATUS_RETRY );

    if (status != CPA_STATUS_SUCCESS) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
        cleanupOpDone(&op_done);
        goto err;
    }

    do {
        if(op_done.job) {
            /* If we get a failure on qat_pause_job then we will
               not flag an error here and quit because we have
               an asynchronous request in flight.
               We don't want to start cleaning up data
               structures that are still being used. If
               qat_pause_job fails we will just yield and
               loop around and try again until the request
               completes and we can continue. */
            if (qat_pause_job(op_done.job, 0) == 0)
                pthread_yield();
        } else {
            pthread_yield();
        }
    }
    while (!op_done.flag);

    if (op_done.verifyResult != CPA_TRUE) {
        QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
        cleanupOpDone(&op_done);
        goto err;
    }

    cleanupOpDone(&op_done);

    /* KDF, is done in the caller now just copy out bytes */
    if (outX != NULL) {
        *outlenX = pResultX->dataLenInBytes;
        *outX = OPENSSL_zalloc(*outlenX);
        if (*outX == NULL) {
            QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_MALLOC_FAILURE);
            goto err;
        }
        memcpy(*outX, pResultX->pData, *outlenX);
    }

    if (outY != NULL) {
        if (*outlenY != pResultY->dataLenInBytes) {
            QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_INTERNAL_ERROR);
            goto err;
        }
        *outY = OPENSSL_zalloc(*outlenY);
        if (*outY == NULL) {
            QATerr(QAT_F_QAT_ECDH_COMPUTE_KEY, ERR_R_MALLOC_FAILURE);
            goto err;
        }
        memcpy(*outY, pResultY->pData, pResultY->dataLenInBytes);
    }
    ret = *outlenX;

 err:
    if (pResultX) {
        QAT_CHK_CLNSE_QMFREE_FLATBUFF(*pResultX);
        OPENSSL_free(pResultX);
    }
    if (pResultY) {
        QAT_CHK_CLNSE_QMFREE_FLATBUFF(*pResultY);
        OPENSSL_free(pResultY);
    }
    QAT_CHK_CLNSE_QMFREE_FLATBUFF(opData->k);
    QAT_CHK_QMFREE_FLATBUFF(opData->xg);
    QAT_CHK_QMFREE_FLATBUFF(opData->yg);
    QAT_CHK_QMFREE_FLATBUFF(opData->a);
    QAT_CHK_QMFREE_FLATBUFF(opData->b);
    QAT_CHK_QMFREE_FLATBUFF(opData->q);
    if (opData)
        OPENSSL_free(opData);
    if (ctx) {
        BN_CTX_end(ctx);
        BN_CTX_free(ctx);
    }
    return (ret);
}

int qat_engine_ecdh_compute_key(unsigned char **out,
                                size_t *outlen,
                                const EC_POINT *pub_key,
                                const EC_KEY *ecdh)
{
    return qat_ecdh_compute_key(out, outlen, NULL, 0, pub_key, ecdh);
}

int qat_ecdh_generate_key(EC_KEY *ecdh)
{
    int ok = 0;
    int alloc_priv = 0, alloc_pub = 0;
    int field_size = 0;
    BN_CTX *ctx = NULL;
    BIGNUM *priv_key = NULL, *order = NULL, *x_bn = NULL, *y_bn =
        NULL, *tx_bn = NULL, *ty_bn = NULL;
    EC_POINT *pub_key = NULL;
    const EC_POINT *gen;
    const EC_GROUP *group;
    unsigned char *temp_xbuf = NULL;
    unsigned char *temp_ybuf = NULL;
    size_t temp_xfield_size = 0;
    size_t temp_yfield_size = 0;
    PFUNC_GEN_KEY gen_key_pfunc = NULL;

# ifdef OPENSSL_FIPS
    if (FIPS_mode())
        return FIPS_ec_key_generate_key(ecdh);
# endif

    if (ecdh == NULL || ((group = EC_KEY_get0_group(ecdh)) == NULL)) {
        QATerr(QAT_F_QAT_ECDH_GENERATE_KEY, ERR_R_PASSED_NULL_PARAMETER);
        return 0;
    }

    /* Unsupported curve: X25519.
     * Detect and call it's software implementation.
     */
    if (EC_GROUP_get_curve_name(group) == NID_X25519) {
        EC_KEY_METHOD_get_keygen((EC_KEY_METHOD *) EC_KEY_OpenSSL(), &gen_key_pfunc);
        if (gen_key_pfunc == NULL) {
            QATerr(QAT_F_QAT_ECDH_GENERATE_KEY, ERR_R_INTERNAL_ERROR);
            return 0;
        }
        return (*gen_key_pfunc)(ecdh);
    }

    if (((order = BN_new()) == NULL) || ((ctx = BN_CTX_new()) == NULL)) {
        QATerr(QAT_F_QAT_ECDH_GENERATE_KEY, ERR_R_MALLOC_FAILURE);
        goto err;
    }
    if ((priv_key = (BIGNUM *)EC_KEY_get0_private_key(ecdh)) == NULL) {
        priv_key = BN_new();
        if (priv_key == NULL) {
            QATerr(QAT_F_QAT_ECDH_GENERATE_KEY, ERR_R_MALLOC_FAILURE);
            goto err;
        }
        alloc_priv = 1;
    }

    if (!EC_GROUP_get_order(group, order, ctx)) {
        QATerr(QAT_F_QAT_ECDH_GENERATE_KEY, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    do
        if (!BN_rand_range(priv_key, order)) {
            QATerr(QAT_F_QAT_ECDH_GENERATE_KEY, ERR_R_INTERNAL_ERROR);
            goto err;
        }
    while (BN_is_zero(priv_key)) ;

    if (alloc_priv) {
        if (!EC_KEY_set_private_key(ecdh, priv_key)) {
            QATerr(QAT_F_QAT_ECDH_GENERATE_KEY, ERR_R_INTERNAL_ERROR);
            goto err;
        }
    }

    if ((pub_key = (EC_POINT *)EC_KEY_get0_public_key(ecdh)) == NULL) {
        pub_key = EC_POINT_new(group);
        if (pub_key == NULL) {
            QATerr(QAT_F_QAT_ECDH_GENERATE_KEY, QAT_R_MEM_ALLOC_FAILED);
            goto err;
        }
        alloc_pub = 1;
    }

    field_size = EC_GROUP_get_degree(group);
    if (field_size <= 0) {
        QATerr(QAT_F_QAT_ECDH_GENERATE_KEY, QAT_R_FIELD_SIZE_ERROR);
        goto err;
    }
    gen = EC_GROUP_get0_generator(group);
    temp_xfield_size = temp_yfield_size = (field_size + 7) / 8;

    if (qat_ecdh_compute_key(&temp_xbuf,
                              &temp_xfield_size,
                              &temp_ybuf,
                              &temp_yfield_size,
                              gen, ecdh) <= 0) {
        /*
         * No QATerr is raised here because errors are already handled in
         * qat_ecdh_compute_key()
         */
        goto err;
    }

    if (((x_bn = BN_new()) == NULL) ||
        ((y_bn = BN_new()) == NULL) ||
        ((tx_bn = BN_new()) == NULL) || ((ty_bn = BN_new()) == NULL)) {
        QATerr(QAT_F_QAT_ECDH_GENERATE_KEY, QAT_R_MEM_ALLOC_FAILED);
        goto err;
    }

    x_bn = BN_bin2bn(temp_xbuf, temp_xfield_size, x_bn);
    y_bn = BN_bin2bn(temp_ybuf, temp_yfield_size, y_bn);
    if (EC_METHOD_get_field_type(EC_GROUP_method_of(group)) ==
        NID_X9_62_prime_field) {
        if (!EC_POINT_set_affine_coordinates_GFp
            (group, pub_key, x_bn, y_bn, ctx)) {
            QATerr(QAT_F_QAT_ECDH_GENERATE_KEY,
                   QAT_R_ECDH_SET_AFFINE_COORD_FAILED);
            goto err;
        }
        if (!EC_POINT_get_affine_coordinates_GFp
            (group, pub_key, tx_bn, ty_bn, ctx)) {
            QATerr(QAT_F_QAT_ECDH_GENERATE_KEY,
                   QAT_R_ECDH_GET_AFFINE_COORD_FAILED);
            goto err;
        }

        /*
         * Check if retrieved coordinates match originals: if not values are
         * out of range.
         */
        if (BN_cmp(x_bn, tx_bn) || BN_cmp(y_bn, ty_bn)) {
            QATerr(QAT_F_QAT_ECDH_GENERATE_KEY, ERR_R_INTERNAL_ERROR);
            goto err;
        }
        if (!EC_KEY_set_public_key(ecdh, pub_key)) {
            QATerr(QAT_F_QAT_ECDH_GENERATE_KEY, ERR_R_INTERNAL_ERROR);
            goto err;
        }
    } else {
        if (EC_METHOD_get_field_type(EC_GROUP_method_of(group)) ==
            NID_X9_62_characteristic_two_field) {
            if (!EC_POINT_set_affine_coordinates_GF2m
                (group, pub_key, x_bn, y_bn, ctx)) {
                QATerr(QAT_F_QAT_ECDH_GENERATE_KEY,
                       QAT_R_ECDH_SET_AFFINE_COORD_FAILED);
                goto err;
            }
            if (!EC_POINT_get_affine_coordinates_GF2m
                (group, pub_key, tx_bn, ty_bn, ctx)) {
                QATerr(QAT_F_QAT_ECDH_GENERATE_KEY,
                       QAT_R_ECDH_GET_AFFINE_COORD_FAILED);
                goto err;
            }
            if (BN_cmp(x_bn, tx_bn) || BN_cmp(y_bn, ty_bn)) {
                QATerr(QAT_F_QAT_ECDH_GENERATE_KEY,
                       ERR_R_INTERNAL_ERROR);
                goto err;
            }
            if (!EC_KEY_set_public_key(ecdh, pub_key)) {
                QATerr(QAT_F_QAT_ECDH_GENERATE_KEY,
                       ERR_R_INTERNAL_ERROR);
                goto err;
            }
        } else {
            QATerr(QAT_F_QAT_ECDH_GENERATE_KEY,
                   QAT_R_ECDH_UNKNOWN_FIELD_TYPE);
            goto err;
        }
    }
    ok = 1;

 err:
    if (order)
        BN_free(order);
    if (alloc_pub)
        EC_POINT_free(pub_key);
    if (alloc_priv)
        BN_clear_free(priv_key);
    if (ctx != NULL)
        BN_CTX_free(ctx);
    if (temp_xbuf != NULL)
        OPENSSL_free(temp_xbuf);
    if (temp_ybuf != NULL)
        OPENSSL_free(temp_ybuf);
    if (x_bn != NULL)
        BN_free(x_bn);
    if (y_bn != NULL)
        BN_free(y_bn);
    if (tx_bn != NULL)
        BN_free(tx_bn);
    if (ty_bn != NULL)
        BN_free(ty_bn);
    return (ok);
}

/* Callback to indicate QAT completion of ECDSA Sign */
void qat_ecdsaSignCallbackFn(void *pCallbackTag, CpaStatus status,
                             void *pOpData, CpaBoolean bEcdsaSignStatus,
                             CpaFlatBuffer * pResultR,
                             CpaFlatBuffer * pResultS)
{
    qat_crypto_callbackFn(pCallbackTag, status, CPA_CY_SYM_OP_CIPHER, pOpData,
                          NULL, bEcdsaSignStatus);
}

/* Callback to indicate QAT completion of ECDSA Verify */
void qat_ecdsaVerifyCallbackFn(void *pCallbackTag, CpaStatus status,
                               void *pOpData, CpaBoolean bEcdsaVerifyStatus)
{
    qat_crypto_callbackFn(pCallbackTag, status, CPA_CY_SYM_OP_CIPHER, pOpData,
                          NULL, bEcdsaVerifyStatus);
}


int qat_ecdsa_sign(int type, const unsigned char *dgst, int dlen,
                          unsigned char *sig, unsigned int *siglen,
                          const BIGNUM *kinv, const BIGNUM *r, EC_KEY *eckey)
{
    ECDSA_SIG *s;
    RAND_seed(dgst, dlen);
    s = qat_ecdsa_do_sign(dgst, dlen, kinv, r, eckey);
    if (s == NULL) {
        *siglen = 0;
        return 0;
    }
    *siglen = i2d_ECDSA_SIG(s, &sig);
    ECDSA_SIG_free(s);
    return 1;
}


ECDSA_SIG *qat_ecdsa_do_sign(const unsigned char *dgst, int dgst_len,
                                    const BIGNUM *in_kinv, const BIGNUM *in_r,
                                    EC_KEY *eckey)
{
    int ok = 0, i;
    BIGNUM *m = NULL, *order = NULL;
    BN_CTX *ctx = NULL;
    const EC_GROUP *group;
    ECDSA_SIG *ret = NULL;
    BIGNUM *ecdsa_sig_r = NULL, *ecdsa_sig_s = NULL;
    const BIGNUM *priv_key;
    BIGNUM *p = NULL, *a = NULL, *b = NULL, *k = NULL, *r = NULL;
    BIGNUM *xg = NULL, *yg = NULL;
    const EC_POINT *pub_key = NULL;

    CpaFlatBuffer *pResultR = NULL;
    CpaFlatBuffer *pResultS = NULL;
    CpaInstanceHandle instanceHandle;
    CpaCyEcdsaSignRSOpData *opData = NULL;
    CpaBoolean bEcdsaSignStatus;
    CpaStatus status;
    size_t buflen;
    struct op_done op_done;
    int qatPerformOpRetries = 0;
    useconds_t ulPollInterval = getQatPollInterval();
    int iMsgRetry = getQatMsgRetryCount();
    const EC_POINT *ec_point = NULL;

    DEBUG("[%s] --- called.\n", __func__);

    group = EC_KEY_get0_group(eckey);
    priv_key = EC_KEY_get0_private_key(eckey);
    pub_key = EC_KEY_get0_public_key(eckey);

    if (group == NULL || priv_key == NULL || pub_key == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_PASSED_NULL_PARAMETER);
        return ret;
    }

    if ((ec_point = EC_GROUP_get0_generator(group)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_EC_LIB);
        return ret;
    }

    opData = (CpaCyEcdsaSignRSOpData *)
        OPENSSL_malloc(sizeof(CpaCyEcdsaSignRSOpData));
    if (opData == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_MALLOC_FAILURE);
        return ret;
    }

    memset(opData, 0, sizeof(CpaCyEcdsaSignRSOpData));

    if ((ret = ECDSA_SIG_new()) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_MALLOC_FAILURE);
        goto err;
    }

    ecdsa_sig_r = BN_new();
    ecdsa_sig_s = BN_new();
    /* NULL checking of ecdsa_sig_r & ecdsa_sig_s done in ECDSA_SIG_set0() */
    if (ECDSA_SIG_set0(ret, ecdsa_sig_r, ecdsa_sig_s) == 0) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_MALLOC_FAILURE);
        goto err;
    }

    if ((ctx = BN_CTX_new()) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_MALLOC_FAILURE);
        goto err;
    }

    BN_CTX_start(ctx);

    if ((p = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((a = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((b = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((xg = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((yg = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((m = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((k = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((r = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((order = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if ((qat_BN_to_FB(&(opData->d), (BIGNUM *)priv_key)) != 1) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if (!EC_GROUP_get_order(group, order, ctx)) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_EC_LIB);
        goto err;
    }
    i = BN_num_bits(order);

    /*
     * Need to truncate digest if it is too long: first truncate whole bytes.
     */
    if (8 * dgst_len > i)
        dgst_len = (i + 7) / 8;

    if (!BN_bin2bn(dgst, dgst_len, m)) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_BN_LIB);
        goto err;
    }

    /* If still too long truncate remaining bits with a shift */
    if ((8 * dgst_len > i) && !BN_rshift(m, m, 8 - (i & 0x7))) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_BN_LIB);
        goto err;
    }

    if ((qat_BN_to_FB(&(opData->m), (BIGNUM *)m)) != 1) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    do
        if (!BN_rand_range(k, order)) {
            QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
            goto err;
        }
    while (BN_is_zero(k)) ;

    if (EC_METHOD_get_field_type(EC_GROUP_method_of(group))
        == NID_X9_62_prime_field) {
        if ((!EC_GROUP_get_curve_GFp(group, p, a, b, ctx)) ||
            (!EC_POINT_get_affine_coordinates_GFp(group, ec_point,
                                                  xg, yg, ctx))) {
            QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
            goto err;
        }
        opData->fieldType = CPA_CY_EC_FIELD_TYPE_PRIME;
    } else {
        if ((!EC_GROUP_get_curve_GF2m(group, p, a, b, ctx)) ||
            (!EC_POINT_get_affine_coordinates_GF2m(group, ec_point,
                                                   xg, yg, ctx))) {
            QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
            goto err;
        }
        opData->fieldType = CPA_CY_EC_FIELD_TYPE_BINARY;
    }

    if ((qat_BN_to_FB(&(opData->xg), xg) != 1) ||
        (qat_BN_to_FB(&(opData->yg), yg) != 1) ||
        (qat_BN_to_FB(&(opData->a), a) != 1)) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    /*
     * This is a special handling required for curves with 'a' co-efficient
     * of 0. The translation to a flatbuffer results in a zero sized field
     * but the Quickassist API expects a flatbuffer of size 1 with a value
     * of zero. As a special case we will create that manually.
     */
    if (opData->a.pData == NULL && opData->a.dataLenInBytes == 0) {
        opData->a.pData = qaeCryptoMemAlloc(1, __FILE__, __LINE__);
        opData->a.dataLenInBytes = 1;
        if (opData->a.pData) {
            opData->a.pData[0] = 0;
        }
    }

    if ((qat_BN_to_FB(&(opData->b), b) != 1) ||
        (qat_BN_to_FB(&(opData->q), p) != 1)) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if (in_kinv == NULL || in_r == NULL) {
        if ((qat_BN_to_FB(&(opData->k), (BIGNUM *)k)) != 1) {
            QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
            goto err;
        }

        if ((qat_BN_to_FB(&(opData->n), (BIGNUM *)order)) != 1) {
            QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
            goto err;
        }

    } else {
        if ((qat_BN_to_FB(&(opData->k), (BIGNUM *)in_kinv)) != 1) {
            QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
            goto err;
        }

        if ((qat_BN_to_FB(&(opData->n), (BIGNUM *)in_r)) != 1) {
            QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
            goto err;
        }

    }

    buflen = EC_GROUP_get_degree(group);
    pResultR = (CpaFlatBuffer *) OPENSSL_malloc(sizeof(CpaFlatBuffer));
    if (pResultR == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_MALLOC_FAILURE);
        goto err;
    }
    pResultR->pData = qaeCryptoMemAlloc(buflen, __FILE__, __LINE__);
    if (pResultR->pData == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_MALLOC_FAILURE);
        goto err;
    }
    pResultR->dataLenInBytes = (Cpa32U) buflen;
    pResultS = (CpaFlatBuffer *) OPENSSL_malloc(sizeof(CpaFlatBuffer));
    if (pResultS == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_MALLOC_FAILURE);
        goto err;
    }
    pResultS->pData = qaeCryptoMemAlloc(buflen, __FILE__, __LINE__);
    if (pResultS->pData == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_MALLOC_FAILURE);
        goto err;
    }
    pResultS->dataLenInBytes = (Cpa32U) buflen;

    /* perform ECDSA sign */
    initOpDone(&op_done);
    if (op_done.job) {
        if (qat_setup_async_event_notification(0) == 0) {
            QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
            cleanupOpDone(&op_done);
            goto err;
        }
    }

    CRYPTO_QAT_LOG("AU - %s\n", __func__);
    do {
        if ((instanceHandle = get_next_inst()) == NULL) {
            QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
            cleanupOpDone(&op_done);
            goto err;
        }

        CRYPTO_QAT_LOG("AU - %s\n", __func__);
        status = cpaCyEcdsaSignRS(instanceHandle,
                qat_ecdsaSignCallbackFn,
                &op_done,
                opData,
                &bEcdsaSignStatus, pResultR, pResultS);

        if (status == CPA_STATUS_RETRY) {
            if (op_done.job == NULL) {
                usleep(ulPollInterval +
                        (qatPerformOpRetries %
                         QAT_RETRY_BACKOFF_MODULO_DIVISOR));
                qatPerformOpRetries++;
                if (iMsgRetry != QAT_INFINITE_MAX_NUM_RETRIES) {
                    if (qatPerformOpRetries >= iMsgRetry) {
                        break;
                    }
                }
            } else {
                if ((qat_wake_job(op_done.job, 0) == 0) ||
                    (qat_pause_job(op_done.job, 0) == 0)) {
                    status = CPA_STATUS_FAIL;
                    break;
                }
            }
        }
    }
    while (status == CPA_STATUS_RETRY);

    if (status != CPA_STATUS_SUCCESS) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
        cleanupOpDone(&op_done);
        goto err;
    }

    do {
        if(op_done.job) {
            /* If we get a failure on qat_pause_job then we will
               not flag an error here and quit because we have
               an asynchronous request in flight.
               We don't want to start cleaning up data
               structures that are still being used. If
               qat_pause_job fails we will just yield and
               loop around and try again until the request
               completes and we can continue. */
            if (qat_pause_job(op_done.job, 0) == 0)
                pthread_yield();
        } else {
            pthread_yield();
        }
    }
    while (!op_done.flag);

    cleanupOpDone(&op_done);

    if (op_done.verifyResult != CPA_TRUE) {
        QATerr(QAT_F_QAT_ECDSA_DO_SIGN, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    /* Convert the flatbuffer results back to a BN */
    BN_bin2bn(pResultR->pData, pResultR->dataLenInBytes, ecdsa_sig_r);
    BN_bin2bn(pResultS->pData, pResultS->dataLenInBytes, ecdsa_sig_s);

    ok = 1;

 err:
    if (!ok) {
        ECDSA_SIG_free(ret);
        ret = NULL;
    }

    if (pResultR) {
        QAT_CHK_QMFREE_FLATBUFF(*pResultR);
        OPENSSL_free(pResultR);
    }
    if (pResultS) {
        QAT_CHK_QMFREE_FLATBUFF(*pResultS);
        OPENSSL_free(pResultS);
    }

    if (opData) {
        QAT_CHK_QMFREE_FLATBUFF(opData->n);
        QAT_CHK_QMFREE_FLATBUFF(opData->m);
        QAT_CHK_QMFREE_FLATBUFF(opData->xg);
        QAT_CHK_QMFREE_FLATBUFF(opData->yg);
        QAT_CHK_QMFREE_FLATBUFF(opData->a);
        QAT_CHK_QMFREE_FLATBUFF(opData->b);
        QAT_CHK_QMFREE_FLATBUFF(opData->q);
        QAT_CHK_CLNSE_QMFREE_FLATBUFF(opData->k);
        QAT_CHK_CLNSE_QMFREE_FLATBUFF(opData->d);
        OPENSSL_free(opData);
    }

    if (ctx) {
        BN_CTX_end(ctx);
        BN_CTX_free(ctx);
    }
    return ret;
}


/*-
 * returns
 *      1: correct signature
 *      0: incorrect signature
 *     -1: error
 */
int qat_ecdsa_verify(int type, const unsigned char *dgst, int dgst_len,
                            const unsigned char *sigbuf, int sig_len, EC_KEY *eckey)
{
    ECDSA_SIG *s;
    const unsigned char *p = sigbuf;
    unsigned char *der = NULL;
    int derlen = -1;
    int ret = -1;

    s = ECDSA_SIG_new();
    if (s == NULL)
        return (ret);
    if (d2i_ECDSA_SIG(&s, &p, sig_len) == NULL)
        goto err;
    /* Ensure signature uses DER and doesn't have trailing garbage */
    derlen = i2d_ECDSA_SIG(s, &der);
    if (derlen != sig_len || memcmp(sigbuf, der, derlen) != 0)
        goto err;
    ret = qat_ecdsa_do_verify(dgst, dgst_len, s, eckey);
 err:
    OPENSSL_clear_free(der, derlen);
    ECDSA_SIG_free(s);
    return ret;
}


int qat_ecdsa_do_verify(const unsigned char *dgst, int dgst_len,
                        const ECDSA_SIG *sig, EC_KEY *eckey)
{
    int ret = -1, i;
    BN_CTX *ctx = NULL;
    BIGNUM *order = NULL, *m = NULL;
    const EC_GROUP *group;
    const EC_POINT *pub_key;
    BIGNUM *p = NULL, *a = NULL, *b = NULL;
    BIGNUM *xg = NULL, *yg = NULL, *xp = NULL, *yp = NULL;
    const EC_POINT *ec_point;
    const BIGNUM *sig_r = NULL, *sig_s = NULL;

    CpaInstanceHandle instanceHandle;
    CpaCyEcdsaVerifyOpData *opData = NULL;
    CpaBoolean bEcdsaVerifyStatus;
    CpaStatus status;
    struct op_done op_done;
    int qatPerformOpRetries = 0;
    useconds_t ulPollInterval = getQatPollInterval();
    int iMsgRetry = getQatMsgRetryCount();

    DEBUG("%s been called \n", __func__);

    /* check input values */
    if (eckey == NULL || (group = EC_KEY_get0_group(eckey)) == NULL ||
        (pub_key = EC_KEY_get0_public_key(eckey)) == NULL || sig == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        return ret;
    }

    if ((ec_point = EC_GROUP_get0_generator(group)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_EC_LIB);
        return ret;
    }

    opData = (CpaCyEcdsaVerifyOpData *)
        OPENSSL_malloc(sizeof(CpaCyEcdsaVerifyOpData));
    if (opData == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_MALLOC_FAILURE);
        return ret;
    }

    memset(opData, 0, sizeof(CpaCyEcdsaVerifyOpData));

    if ((ctx = BN_CTX_new()) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_MALLOC_FAILURE);
        goto err;
    }

    BN_CTX_start(ctx);

    if ((p = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((a = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((b = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((xg = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((yg = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((xp = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((yp = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((m = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    if ((order = BN_CTX_get(ctx)) == NULL) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if (!EC_GROUP_get_order(group, order, ctx)) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_EC_LIB);
        goto err;
    }

    ECDSA_SIG_get0((ECDSA_SIG *)sig, &sig_r, &sig_s);
    if (BN_is_zero(sig_r) || BN_is_negative(sig_r) ||
        BN_ucmp(sig_r, order) >= 0 || BN_is_zero(sig_s) ||
        BN_is_negative(sig_s) || BN_ucmp(sig_s, order) >= 0) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        ret = 0;                /* signature is invalid */
        goto err;
    }
    /* digest -> m */
    i = BN_num_bits(order);
    /*
     * Need to truncate digest if it is too long: first truncate whole bytes.
     */
    if (8 * dgst_len > i)
        dgst_len = (i + 7) / 8;

    if (!BN_bin2bn(dgst, dgst_len, m)) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_BN_LIB);
        goto err;
    }
    /* If still too long truncate remaining bits with a shift */
    if ((8 * dgst_len > i) && !BN_rshift(m, m, 8 - (i & 0x7))) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_BN_LIB);
        goto err;
    }

    if ((qat_BN_to_FB(&(opData->m), (BIGNUM *)m)) != 1) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if (EC_METHOD_get_field_type(EC_GROUP_method_of(group))
        == NID_X9_62_prime_field) {
        if ((!EC_GROUP_get_curve_GFp(group, p, a, b, ctx)) ||
            (!EC_POINT_get_affine_coordinates_GFp(group, ec_point,
                                                  xg, yg, ctx)) ||
            (!EC_POINT_get_affine_coordinates_GFp(group, pub_key,
                                                  xp, yp, ctx))) {
            QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
            goto err;
        }
        opData->fieldType = CPA_CY_EC_FIELD_TYPE_PRIME;
    } else {
        if ((!EC_GROUP_get_curve_GF2m(group, p, a, b, ctx)) ||
            (!EC_POINT_get_affine_coordinates_GF2m(group, ec_point,
                                                   xg, yg, ctx)) ||
            (!EC_POINT_get_affine_coordinates_GF2m(group, pub_key,
                                                   xp, yp, ctx))) {
            QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
            goto err;
        }
        opData->fieldType = CPA_CY_EC_FIELD_TYPE_BINARY;
    }

    if ((qat_BN_to_FB(&(opData->xg), xg) != 1) ||
        (qat_BN_to_FB(&(opData->yg), yg) != 1) ||
        (qat_BN_to_FB(&(opData->a), a) != 1)) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    /*
     * This is a special handling required for curves with 'a' co-efficient
     * of 0. The translation to a flatbuffer results in a zero sized field
     * but the Quickassist API expects a flatbuffer of size 1 with a value
     * of zero. As a special case we will create that manually.
     */

    if (opData->a.pData == NULL && opData->a.dataLenInBytes == 0) {
        opData->a.pData = qaeCryptoMemAlloc(1, __FILE__, __LINE__);
        opData->a.dataLenInBytes = 1;
        if (opData->a.pData) {
            opData->a.pData[0] = 0;
        }
    }

    if ((qat_BN_to_FB(&(opData->b), b) != 1) ||
        (qat_BN_to_FB(&(opData->q), p) != 1)) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if ((qat_BN_to_FB(&(opData->n), (BIGNUM *)order)) != 1) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    ECDSA_SIG_get0((ECDSA_SIG *)sig, &sig_r, &sig_s);

    if ((qat_BN_to_FB(&(opData->r), (BIGNUM *)sig_r)) != 1) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if ((qat_BN_to_FB(&(opData->s), (BIGNUM *)sig_s)) != 1) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if ((qat_BN_to_FB(&(opData->xp), (BIGNUM *)xp)) != 1) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if ((qat_BN_to_FB(&(opData->yp), (BIGNUM *)yp)) != 1) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    /* perform ECDSA verify */
    initOpDone(&op_done);
    if (op_done.job) {
        if (qat_setup_async_event_notification(0) == 0) {
            QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
            cleanupOpDone(&op_done);
            goto err;
        }
    }

    CRYPTO_QAT_LOG("AU - %s\n", __func__);
    do {
        if ((instanceHandle = get_next_inst()) == NULL) {
            QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
            cleanupOpDone(&op_done);
            goto err;
        }

        CRYPTO_QAT_LOG("AU - %s\n", __func__);
        status = cpaCyEcdsaVerify(instanceHandle,
                                  qat_ecdsaVerifyCallbackFn,
                                  &op_done, opData, &bEcdsaVerifyStatus);

        if (status == CPA_STATUS_RETRY) {
            if (op_done.job == NULL) {
                usleep(ulPollInterval +
                       (qatPerformOpRetries %
                        QAT_RETRY_BACKOFF_MODULO_DIVISOR));
                qatPerformOpRetries++;
                if (iMsgRetry != QAT_INFINITE_MAX_NUM_RETRIES) {
                    if (qatPerformOpRetries >= iMsgRetry) {
                        break;
                    }
                }
            } else {
                if ((qat_wake_job(op_done.job, 0) == 0) ||
                    (qat_pause_job(op_done.job, 0) == 0)) {
                    status = CPA_STATUS_FAIL;
                    break;
                }
            }
        }
    }
    while (status == CPA_STATUS_RETRY);

    if (status != CPA_STATUS_SUCCESS) {
        QATerr(QAT_F_QAT_ECDSA_DO_VERIFY, ERR_R_INTERNAL_ERROR);
        cleanupOpDone(&op_done);
        goto err;
    }

    do {
        if(op_done.job) {
            /* If we get a failure on qat_pause_job then we will
               not flag an error here and quit because we have
               an asynchronous request in flight.
               We don't want to start cleaning up data
               structures that are still being used. If
               qat_pause_job fails we will just yield and
               loop around and try again until the request
               completes and we can continue. */
            if (qat_pause_job(op_done.job, 0) == 0)
                pthread_yield();
        } else {
            pthread_yield();
        }
    }
    while (!op_done.flag);

    cleanupOpDone(&op_done);

    if (op_done.verifyResult == CPA_TRUE)
        ret = 1;

 err:
    if (opData) {
        QAT_CHK_QMFREE_FLATBUFF(opData->r);
        QAT_CHK_QMFREE_FLATBUFF(opData->s);
        QAT_CHK_QMFREE_FLATBUFF(opData->n);
        QAT_CHK_QMFREE_FLATBUFF(opData->m);
        QAT_CHK_QMFREE_FLATBUFF(opData->xg);
        QAT_CHK_QMFREE_FLATBUFF(opData->yg);
        QAT_CHK_QMFREE_FLATBUFF(opData->a);
        QAT_CHK_QMFREE_FLATBUFF(opData->b);
        QAT_CHK_QMFREE_FLATBUFF(opData->q);
        QAT_CHK_QMFREE_FLATBUFF(opData->xp);
        QAT_CHK_QMFREE_FLATBUFF(opData->yp);
        OPENSSL_free(opData);
    }

    if (ctx) {
        BN_CTX_end(ctx);
        BN_CTX_free(ctx);
    }
    return ret;
}

