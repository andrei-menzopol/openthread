/*
 *  Copyright (c) 2018, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *   This file implements ECDSA signing.
 */

#include "ecdsa.hpp"

#if OPENTHREAD_CONFIG_ECDSA_ENABLE

#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/pk.h>
#include <mbedtls/version.h>

#ifdef MBEDTLS_USE_TINYCRYPT
#include <tinycrypt/ecc.h>
#include <tinycrypt/ecc_dh.h>
#include <tinycrypt/ecc_dsa.h>
#endif

#include "common/code_utils.hpp"
#include "common/debug.hpp"
#include "common/random.hpp"
#include "crypto/mbedtls.hpp"

namespace ot {
namespace Crypto {
namespace Ecdsa {

Error P256::KeyPair::Generate(void)
{
    mbedtls_pk_context pk;
    int                ret;
    mbedtls_pk_init(&pk);

    ret = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    VerifyOrExit(ret == 0);

#ifdef MBEDTLS_USE_TINYCRYPT
    {
        mbedtls_uecc_keypair *keypair = mbedtls_pk_uecc(pk);

        ret = uECC_make_key(keypair->public_key, keypair->private_key);
        VerifyOrExit(ret == UECC_SUCCESS);
    }
#else
    ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), MbedTls::CryptoSecurePrng, nullptr);
    VerifyOrExit(ret == 0);
#endif

    ret = mbedtls_pk_write_key_der(&pk, mDerBytes, sizeof(mDerBytes));
    VerifyOrExit(ret > 0);

    mDerLength = static_cast<uint8_t>(ret);

    memmove(mDerBytes, mDerBytes + sizeof(mDerBytes) - mDerLength, mDerLength);

exit:
    mbedtls_pk_free(&pk);

    return (ret >= 0) ? kErrorNone : MbedTls::MapError(ret);
}

Error P256::KeyPair::Parse(void *aContext) const
{
    Error               error = kErrorNone;
    mbedtls_pk_context *pk    = reinterpret_cast<mbedtls_pk_context *>(aContext);

    mbedtls_pk_init(pk);

    VerifyOrExit(mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0, error = kErrorFailed);
#if (MBEDTLS_VERSION_NUMBER >= 0x03000000)
    VerifyOrExit(mbedtls_pk_parse_key(pk, mDerBytes, mDerLength, nullptr, 0, MbedTls::CryptoSecurePrng, nullptr) == 0,
                 error = kErrorParse);
#else
    VerifyOrExit(mbedtls_pk_parse_key(pk, mDerBytes, mDerLength, nullptr, 0) == 0, error = kErrorParse);
#endif

exit:
    return error;
}

Error P256::KeyPair::GetPublicKey(PublicKey &aPublicKey) const
{
    Error              error;
    mbedtls_pk_context pk;
    int                ret;

    SuccessOrExit(error = Parse(&pk));

#ifdef MBEDTLS_USE_TINYCRYPT
    {
        mbedtls_uecc_keypair *keyPair = mbedtls_pk_uecc(pk);

        memcpy(aPublicKey.mData, keyPair->public_key, kMpiSize);
        memcpy(aPublicKey.mData + kMpiSize, keyPair->public_key + kMpiSize, kMpiSize);
    }
#else
    {
        mbedtls_ecp_keypair *keyPair = mbedtls_pk_ec(pk);

        ret = mbedtls_mpi_write_binary(&keyPair->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X), aPublicKey.mData, kMpiSize);
        VerifyOrExit(ret == 0, error = MbedTls::MapError(ret));

        ret = mbedtls_mpi_write_binary(&keyPair->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y), aPublicKey.mData + kMpiSize,
                                       kMpiSize);
        VerifyOrExit(ret == 0, error = MbedTls::MapError(ret));
    }
#endif

exit:
    mbedtls_pk_free(&pk);
    return error;
}

Error P256::KeyPair::Sign(const Sha256::Hash &aHash, Signature &aSignature) const
{
    Error              error;
    mbedtls_pk_context pk;
    int                ret;

    SuccessOrExit(error = Parse(&pk));

#ifdef MBEDTLS_USE_TINYCRYPT
    {
        mbedtls_uecc_keypair *keypair;
        uint8_t               sig[2 * kMpiSize];

        keypair = mbedtls_pk_uecc(pk);

        ret = uECC_sign(keypair->private_key, aHash.GetBytes(), Sha256::Hash::kSize, sig);
        VerifyOrExit(ret == UECC_SUCCESS, error = MbedTls::MapError(ret));

        memcpy(aSignature.mShared.mMpis.mR, sig, kMpiSize);
        memcpy(aSignature.mShared.mMpis.mS, sig + kMpiSize, kMpiSize);
    }
#else
    {
        mbedtls_ecp_keypair * keypair;
        mbedtls_ecdsa_context ecdsa;
        mbedtls_mpi           r;
        mbedtls_mpi           s;

        mbedtls_ecdsa_init(&ecdsa);
        mbedtls_mpi_init(&r);
        mbedtls_mpi_init(&s);

        keypair = mbedtls_pk_ec(pk);

        ret = mbedtls_ecdsa_from_keypair(&ecdsa, keypair);
        VerifyOrExit(ret == 0, error = MbedTls::MapError(ret));

#if (MBEDTLS_VERSION_NUMBER >= 0x02130000)
        ret =
            mbedtls_ecdsa_sign_det_ext(&ecdsa.MBEDTLS_PRIVATE(grp), &r, &s, &ecdsa.MBEDTLS_PRIVATE(d), aHash.GetBytes(),
                                       Sha256::Hash::kSize, MBEDTLS_MD_SHA256, MbedTls::CryptoSecurePrng, nullptr);
#else
        ret = mbedtls_ecdsa_sign_det(&ecdsa.MBEDTLS_PRIVATE(grp), &r, &s, &ecdsa.MBEDTLS_PRIVATE(d), aHash.GetBytes(),
                                     Sha256::Hash::kSize, MBEDTLS_MD_SHA256);
#endif
        VerifyOrExit(ret == 0, error = MbedTls::MapError(ret));

        OT_ASSERT(mbedtls_mpi_size(&r) <= kMpiSize);

        ret = mbedtls_mpi_write_binary(&r, aSignature.mShared.mMpis.mR, kMpiSize);
        VerifyOrExit(ret == 0, error = MbedTls::MapError(ret));

        ret = mbedtls_mpi_write_binary(&s, aSignature.mShared.mMpis.mS, kMpiSize);
        VerifyOrExit(ret == 0, error = MbedTls::MapError(ret));
    }
#endif

exit:
    mbedtls_pk_free(&pk);
#ifndef MBEDTLS_USE_TINYCRYPT
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecdsa_free(&ecdsa);
#endif

    return error;
}

Error P256::PublicKey::Verify(const Sha256::Hash &aHash, const Signature &aSignature) const
{
    Error error = kErrorNone;
    int   ret;

#ifdef MBEDTLS_USE_TINYCRYPT
    {
        uint8_t public_key[2 * kMpiSize];
        uint8_t sig[2 * kMpiSize];

        memcpy(public_key, GetBytes(), 2 * kMpiSize);

        memcpy(sig, aSignature.mShared.mMpis.mR, kMpiSize);
        memcpy(sig + kMpiSize, aSignature.mShared.mMpis.mS, kMpiSize);

        ret = uECC_verify(public_key, aHash.GetBytes(), Sha256::Hash::kSize, sig);
        VerifyOrExit(ret == UECC_SUCCESS, error = kErrorSecurity);
    }
#else
    {
        mbedtls_ecdsa_context ecdsa;
        mbedtls_mpi           r;
        mbedtls_mpi           s;

        mbedtls_ecdsa_init(&ecdsa);
        mbedtls_mpi_init(&r);
        mbedtls_mpi_init(&s);

        ret = mbedtls_ecp_group_load(&ecdsa.MBEDTLS_PRIVATE(grp), MBEDTLS_ECP_DP_SECP256R1);
        VerifyOrExit(ret == 0, error = MbedTls::MapError(ret));

        ret = mbedtls_mpi_read_binary(&ecdsa.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X), GetBytes(), kMpiSize);
        VerifyOrExit(ret == 0, error = MbedTls::MapError(ret));
        ret = mbedtls_mpi_read_binary(&ecdsa.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y), GetBytes() + kMpiSize, kMpiSize);
        VerifyOrExit(ret == 0, error = MbedTls::MapError(ret));
        ret = mbedtls_mpi_lset(&ecdsa.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Z), 1);
        VerifyOrExit(ret == 0, error = MbedTls::MapError(ret));

        ret = mbedtls_mpi_read_binary(&r, aSignature.mShared.mMpis.mR, kMpiSize);
        VerifyOrExit(ret == 0, error = MbedTls::MapError(ret));

        ret = mbedtls_mpi_read_binary(&s, aSignature.mShared.mMpis.mS, kMpiSize);
        VerifyOrExit(ret == 0, error = MbedTls::MapError(ret));

        ret = mbedtls_ecdsa_verify(&ecdsa.MBEDTLS_PRIVATE(grp), aHash.GetBytes(), Sha256::Hash::kSize,
                                   &ecdsa.MBEDTLS_PRIVATE(Q), &r, &s);
        VerifyOrExit(ret == 0, error = kErrorSecurity);
    }
#endif

exit:
#ifndef MBEDTLS_USE_TINYCRYPT
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecdsa_free(&ecdsa);
#endif
    return error;
}

Error Sign(uint8_t *      aOutput,
           uint16_t &     aOutputLength,
           const uint8_t *aInputHash,
           uint16_t       aInputHashLength,
           const uint8_t *aPrivateKey,
           uint16_t       aPrivateKeyLength)
{
    Error              error = kErrorNone;
    mbedtls_pk_context pkCtx;

    mbedtls_pk_init(&pkCtx);

    // Parse a private key in PEM format.
#if (MBEDTLS_VERSION_NUMBER >= 0x03000000)
    VerifyOrExit(mbedtls_pk_parse_key(&pkCtx, aPrivateKey, aPrivateKeyLength, nullptr, 0, MbedTls::CryptoSecurePrng,
                                      nullptr) == 0,
                 error = kErrorInvalidArgs);
#else
    VerifyOrExit(mbedtls_pk_parse_key(&pkCtx, aPrivateKey, aPrivateKeyLength, nullptr, 0) == 0,
                 error = kErrorInvalidArgs);
#endif
    VerifyOrExit(mbedtls_pk_get_type(&pkCtx) == MBEDTLS_PK_ECKEY, error = kErrorInvalidArgs);

#ifdef MBEDTLS_USE_TINYCRYPT
    {
        uint8_t               sig[2 * NUM_ECC_BYTES];
        mbedtls_uecc_keypair *keypair = mbedtls_pk_uecc(pkCtx);
        OT_ASSERT(keypair != nullptr);

        // Sign using ECDSA.
        VerifyOrExit(uECC_sign(keypair->private_key, aInputHash, aInputHashLength, sig) == UECC_SUCCESS,
                     error = kErrorFailed);
        VerifyOrExit(2 * NUM_ECC_BYTES <= aOutputLength, error = kErrorNoBufs);

        // Concatenate the two octet sequences in the order R and then S.
        memcpy(aOutput, sig, 2 * NUM_ECC_BYTES);
        aOutputLength = 2 * NUM_ECC_BYTES;
    }
#else
    {
        mbedtls_ecdsa_context ctx;
        mbedtls_ecp_keypair * keypair;
        mbedtls_mpi           rMpi;
        mbedtls_mpi           sMpi;

        mbedtls_pk_init(&pkCtx);
        mbedtls_ecdsa_init(&ctx);
        mbedtls_mpi_init(&rMpi);
        mbedtls_mpi_init(&sMpi);

        keypair = mbedtls_pk_ec(pkCtx);
        OT_ASSERT(keypair != nullptr);

        VerifyOrExit(mbedtls_ecdsa_from_keypair(&ctx, keypair) == 0, error = kErrorFailed);

        // Sign using ECDSA.
        VerifyOrExit(mbedtls_ecdsa_sign(&ctx.MBEDTLS_PRIVATE(grp), &rMpi, &sMpi, &ctx.MBEDTLS_PRIVATE(d), aInputHash,
                                        aInputHashLength, MbedTls::CryptoSecurePrng, nullptr) == 0,
                     error = kErrorFailed);
        VerifyOrExit(mbedtls_mpi_size(&rMpi) + mbedtls_mpi_size(&sMpi) <= aOutputLength, error = kErrorNoBufs);

        // Concatenate the two octet sequences in the order R and then S.
        VerifyOrExit(mbedtls_mpi_write_binary(&rMpi, aOutput, mbedtls_mpi_size(&rMpi)) == 0, error = kErrorFailed);
        aOutputLength = static_cast<uint16_t>(mbedtls_mpi_size(&rMpi));

        VerifyOrExit(mbedtls_mpi_write_binary(&sMpi, aOutput + aOutputLength, mbedtls_mpi_size(&sMpi)) == 0,
                     error = kErrorFailed);
        aOutputLength += mbedtls_mpi_size(&sMpi);
    }
#endif

exit:
    mbedtls_pk_free(&pkCtx);
#ifndef MBEDTLS_USE_TINYCRYPT
    mbedtls_mpi_free(&rMpi);
    mbedtls_mpi_free(&sMpi);
    mbedtls_ecdsa_free(&ctx);
#endif

    return error;
}

} // namespace Ecdsa
} // namespace Crypto
} // namespace ot

#endif // OPENTHREAD_CONFIG_ECDSA_ENABLE
