/***************************************************************************
 *
 *   BSD LICENSE
 *
 *   Copyright(c) 2021 Intel Corporation. All rights reserved.
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
 ***************************************************************************/

/**
 *****************************************************************************
 * @file qatzipChain.h
 *
 * @defgroup qatZipChain QAT Chaining API
 *
 * @description
 *      The definitions and functions identified here
 *      specify the API for chaining APIs.
 *
 * @remarks
 *
 *
 *****************************************************************************/

#ifndef _QATZIPCHAIN_H
#define _QATZIPCHAIN_H

#ifdef __cplusplus
extern"C" {
#endif

#include <stdint.h>
#include "qatzip.h"


/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      Supported size for caller fields
 *
 * @description
 *       This value defines the maximum size in bytes for caller
 *       specified fields.  Caller specified fields differ from tags in that
 *       tags are used in callback functions primarily to identify specific
 *       response messages and correlate them to request messages.
 *
 *       Caller specified fields can be used for any other purpose, including
 *       identifying a flow or session associated with the data.
 *
 *       This value applies to caller specified fields that are part of the
 *       context structures for both Stor1 and Stor2 operations.
 *
 *
 *****************************************************************************/
#define QzStorageCallerFieldSz ((uint8_t)8)

/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      Supported size for IV for aes-gcm
 *
 * @description
 *       This value defines the in bytes for the initialization vector
 *       for AES-GCM operations
 *
 *
 *****************************************************************************/
#define QzGCMIvSz ((uint8_t)12)
/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      Definition of callback function invoked after Stor1
 *      requests.
 *
 * @description
 *      This is the prototype for the qzStor1 and qzStor2 callback functions.
 *      The callback function is registered by the application
 *      by adding it to a Stor1 context.
 *
 * @context
 *      This callback function can be executed in a context that DOES NOT
 *      permit sleeping to occur.
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @reentrant
 *      No
 * @threadSafe
 *      Yes
 *
 * @param[in]       callbackTag   User-supplied value to help identify request.
 * @param[in]       rc            Status of the operation.
 *
 * @retval
 *      None
 * @pre
 *      Component has been initialized.
 * @post
 *      None
 * @note
 *      None
 * @see
 *      None
 *
 *****************************************************************************/
typedef void (*qzStorageCallbackFn)(
    void                 *callbackTag,
    uint64_t             rc);

/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      Supported AES key size
 *
 * @description
 *      This enumerated list identifies lists the supported key size
 *      Note: AES-XTS requires two different keys of equal size.  The size
 *      list here is the size of one key.
 *
 *****************************************************************************/
typedef enum QzAESKeySz_E {
    QZ_AES_KS_128 = (128 / 8),
    QZ_AES_KS_192 = (192 / 8),
    QZ_AES_KS_256 = (256 / 8)
} QzAESKeySz_T;


/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      QATzip AES key structure
 *
 * @description
 *      This structure contains keys for encryption and/or decryption requests
 *      Three sizes are supported, and for sizes valid for XTS
 *      there is space allocated for
 *      two different keys. The first is the cipher key and the second
 *      is the initial tweak value.
 *
 *****************************************************************************/
typedef struct QzAESKeys_S {
    QzAESKeySz_T    size;
    union {
        uint8_t e_keys128[2 * QZ_AES_KS_128];
        uint8_t e_keys192[QZ_AES_KS_192];
        uint8_t e_keys256[2 * QZ_AES_KS_256];
    } enc_keys;
    union {
        uint8_t d_keys128[2 * QZ_AES_KS_128];
        uint8_t d_keys192[QZ_AES_KS_192];
        uint8_t d_keys256[2 * QZ_AES_KS_256];
    } dec_keys;
} QzAESKeys_T;

/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      Supported AES algorithms
 *
 * @description
 *      This enumerated list identifies lists the supported AES
 *      cipher algorithms. AES GCM is support with and without AAD
 *
 *****************************************************************************/
typedef enum QzAESCipher_E {
    QZ_AES_XTS = 1,
    QZ_AES_GCM = 2
} QzAESCipher_T;


/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      QATzip stor1 operation context definition
 *
 * @description
 *      This structure contains data for Qzstor1 contexts
 * @Open
 *   check on padding - (Not on XTS because S is for stealing)
 *
 *****************************************************************************/
typedef struct QzStor1Context_S {

    QzAESCipher_T cipher;
    QzAESKeys_T keys;
    uint16_t    qzDUSz;     // size of block to be encrypted with the
    // same tweak value if XTS.
    uint16_t    pre_skip;   // in output buffer, number of bytes to
    // skip before writing data
    uint16_t    post_skip;  // in output buffer, number of bytes to
    // skip after writing data
    uint8_t     verify;     // 0 = don't verify  anything else = verify
    uint8_t     caller[QzStorageCallerFieldSz];     // caller defined

} QzStor1Context_T;


/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      QATzip stor1 performnOp definition
 *
 *
 * @description
 *      This structure contains data for Qzstor1 perform Op
 *
 *****************************************************************************/
typedef struct QzStor1PerformOp_S {
    uint8_t     direction;
    /**< 0 for encrypt, 1 for decrypt */
    uint8_t     *src;
    /**< pointer to a data buffer to be encrypted or decrypted */
    uint32_t    src_len;
    /**< length of data in source buffer */
    uint8_t     *dst;
    /**< pointer to destination buffer */
    uint32_t    dst_len;
    /**< length of data in source buffer */
    uint8_t     *intermediate;
    /**< pointer to intermediate buffer */
    uint32_t    intermediate_len;
    /**< available length of space in destination buffer */
    /**< intermediate buffer should be at least as large as the */
    /**< source and destination buffer */
} QzStor1PerformOp_T;

/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      QATzip Storage return structure
 *
 * @description
 *      This structure contains return values for Qz storage contexts
 *
 *****************************************************************************/
typedef struct QzStor1Response_S {
    int32_t    enc_ret;      // return from encrypt operation.
    uint64_t   i_crc;        // input crc64 of encrypt operation.
    int32_t    dec_ret;      // return code from decrypt operation.
    uint64_t   o_crc;        // output crc64 of decrypted data.
} QzStor1Response_T;

/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      Submits a request to be the Stor1 chained operation.
 *
 * @description
 *      This function performs an stor1 encryprpyion or decryption
 *      on a data set.
 *      The input data is processed in units of a data unit size as
 *      defined in the context.  For the second and subsequent data
 *      unit, the tweak value is incremented.
 *
 * @context
 *      This function shall not be called in an interrupt context.
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @blocking
 *      Yes
 * @reentrant
 *      No
 * @threadSafe
 *      Yes
 *
 * @param[in]       sess        Session pointer
 * @param[in]       ctx         Context pointer
 * @param[in]       performOp   Pointer to perform op structure
 * @param[in]       results     Pointer to results structure
 * @param[in]       tag         caller supplied tag for callback
 *
 * @retval QZ_OK          Function executed successfully
 * @retval QZ_FAIL        Function did not succeed
 * @retval QZ_PARAMS      *ctx is NULL or member of params is invalid
 * @retval QZ_INTEG       Integrity check failed.
 *
 * @pre
 *      None
 * @post
 *      None
 * @note
 *      This API does not compare the crc64 values.  This is left to the
 *      application.
 *      If qzStor1Init has not been called, then the operations
 *      will be processed synchronously,
 *
 * @see
 *      None
 * @Note
 *      None
 * @Flow
 *    If direction == 0                // encrypt
 *       calculate crc64 of src for s_len bytes  -> store in i_crc
 *       encrypt with XTS
 *          Initial tweak = 1/2 of ctx->keys
 *          For each ctx->qzDUSz
 *             encrypt with tweak & other 1/2 of ctx->keys
 *          src, s_len encrypted to dst, d_len
 *     If verify != 0 or direction != 0   // decrypt operation
 *        (for verify, encrypted data is in dest, d_len
 *                     decrypted data will go to NULL
 *         for decryption, encrypted data is in src, s_len
 *                         decrypted data is in dest, d_len)
 *       decrypt with XTS
 *          output goes to UCS slice for crc64
 *          initial tweak = 1/2 of cts->keys
 *          For each ctx->qzDUSz
 *             decrypt with tweak & other 1/2 of ctx->keys
 *          src, s_len encrypted to dst, d_len
 *          or dest, d_len  encrypted to NULL
 *          record crc64 of decrypted data in o_crc
 *
 *
 *****************************************************************************/
int16_t qzStor1Perform(QzSession_T *sess, QzStor1Context_T *ctx,
                       QzStor1PerformOp_T *performOp,
                       QzStor1Response_T *results, void *tag);


/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      Supported SHA sizes
 *
 * @description
 *      This enumerated list identifies lists the supported SHA hash size`
 *      list here is the size of one key.
 *
 *****************************************************************************/
typedef enum Qz_SHASz_E {
    QZ_SHA1_128 = 128,
    QZ_SHA2_256 = 256,
    QZ_SHA2_512 = 512
} QzSHASz_T;

#define QZ_KDF_INPUT_SZ ((unsigned int)128)   // bytes

typedef struct QzKDFIn_S {
    uint8_t derive;   // 0 = don't derive the key else = derive
    // if derive == 0, cipher key is
    // in KeyContext[0..keyinLen], which must match the
    // required key length for the cipher
    // if derive == 1, then a single pass of NIST
    // SP 800.108 will take place.
    uint8_t KeyContext[(QZ_SHA2_256 / 8) + QZ_KDF_INPUT_SZ];
    /**< concatenation of keyin and (n,label,0x00,context,hash_sz) */
    /**< hash_sz represents the number of bytes produced with the */
    /**< hmac hash operation.  Currently, only QZ_SHA2_256 is supported */
    uint8_t keyinLen;
    /**< length of keyin  octets */
    uint8_t concatLen;
    /**< length of ((n,label,0x00,context,hmac_sz) */
    /**< Note.  concateLen does not include the length of the input key */
} QzKDFIn_T;

/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      QATzip Storage 2 key parameters
 *
 * @description
 *      This structure contains data for Qz storage key.
 *      Because a crc64 will be calculated across the contents of
 *      this structure, all contents are directly included in the
 *      structure, and no pointers to are used as structure elements.
 *
 *****************************************************************************/

typedef struct QzStor2KM_S {
    QzKDFIn_T p_km1;                // Key material
    QzKDFIn_T p_km2;                // not used if verify = 0 or rederive = 0
    uint8_t   p_iv[QzGCMIvSz];      // IV
    uint16_t  p_poly;               // polynomial index for crc64
    uint16_t  p_polyreflect;        // 0 no reflect,  1 = refect in,
    // 2 = reflect put, 3 = reflect in and out
    uint64_t p_xorFinal;             // XOR for final crc64 value
} QzStor2KM_T;


/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      QATzip Storage operation parameters
 *
 * @description
 *      This structure contains data for Qz storage contexts.
 *      Because a crc64 will be calculated across the contents of this
 *      this structure, all contents are directly included in the
 *      structure, and no pointers to are used as structure elements.
 *
 *****************************************************************************/
typedef struct QzStor2Context_S {
    uint16_t  p_size;          // size of p_KM
    QzStor2KM_T p_KM;          // keymaterial or key.

    QzAESCipher_T cipher;      // 1 = XTS, 2 = GCM slice needs this
    QzAESKeySz_T size;

    uint8_t   verify;          // 0 = don't verify anything else = verify
    uint8_t   append_crc;      // 0 = don't append anything else =
    //   append to compressed data
    uint8_t   caller[QzStorageCallerFieldSz];
    // Customer specific data
} QzStor2Context_T;

/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      QATzip stor2 performnOp definition
 *
 *
 * @description
 *      This structure contains data for Qzstor2 perform Op
 *
 *****************************************************************************/
typedef struct QzStor2PerformOp_S {
    uint8_t     direction;
    /**< 0 for encrypt, 1 for decrypt */
    uint8_t     *aad_ptr;
    /**< point to addition authentication data, or NULL */
    uint16_t    aad_len;
    /**< length of addition authentication data */
    uint8_t     *src;
    /**< pointer to source buffer */
    uint32_t    src_len;
    /**< length of data in source buffer */
    uint8_t     *dst;
    /**< pointer to destination buffer */
    uint32_t    dst_len;
    /**< available length of space in destination buffer */
    uint8_t     *intermediate;
    /**< pointer to intermediate buffer */
    uint32_t    intermediate_len;
    /**< available length of space in destination buffer */
    /**< intermediate buffer should be at least as large as the */
    /**< larger of the source and destination buffers */
    uint8_t   input_crc64_added;
    /**< 0 = no input crc64, 1 = crc64 included */
    uint64_t  input_crc64;
    /**< input crc64 based on provided polynomial and reflect instructions. */
} QzStor2PerformOp_T;

/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      QATzip Storage return structure
 *
 * @description
 *      This structure contains return values for Qz storage contexts
 *
 *****************************************************************************/
typedef struct QzStor2Response_S {
    uint64_t   ctx_crc;  // crc64 of context structure
    int32_t    ccret;    // return from comp operation.
    uint32_t   ccrc32;   // crc32 compress operation
    uint32_t   cAdler32; // Adler32 compress operation
    uint32_t   clen;     // Length of compressed data
    uint32_t   elen;     // Length of encrypted data
    uint64_t   i_ccrc;   // input crc of comp operation.
    uint64_t   o_ccrc;   // output crc of comp operation.
    int32_t    dcret;    // return from decomp operation.
    int32_t    dcrc32;   // crc32 decomp operation
    uint32_t   dAdler32; // Adler32 compress operation
    uint64_t   i_dcrc;   // input crc of decomp operation
    uint64_t   o_dcrc;   // output crc of decomp operation
    uint32_t   dlen;     // Length of decompressed data
    int32_t    eret;     // return code from encrypt operation.
    uint64_t   ecrc;     // crc64 of encrypted data.
    int32_t    dret;     // return code from decrypt operation
    uint64_t   dcrc;     // crc64 of decrypted data.
} QzStor2Response_T;

/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      Submits a request to QAT for storage
 *
 * @description
 *      This function performs chained key derivation, compress-encrypt
 *      with an optional verify step on a data set.
 *      This function allow performs the inverse operation.
 *      Return data includes the crc64 from the input and output of the
 *      compress or decompress operation.
 *
 * @context
 *      This function shall not be called in an interrupt context.
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @blocking
 *      Yes
 * @reentrant
 *      No
 * @threadSafe
 *      Yes
 *
 * @param[in]       sess        Session pointer
 * @param[in]       ctx_len     Size of context structure in bytes
 * @param[in]       ctx         Context pointer
 * @param[out]      res         pointer to crc64 and rc structure
 * @param[in]       tag         call supplied tag for callback
 *
 * @retval QZ_OK          Function executed successfully
 * @retval QZ_FAIL        Function did not succeed
 * @retval QZ_PARAMS      *res or *ctx is NULL or member of params is invalid
 * @retval QZ_INTEG       Integrity check failed.
 *
 * @pre
 *      None
 * @post
 *      None
 * @note
 *      If qzStor2Init has not been called with a callback function,
 *      then the operations will be processed syncrhonously.  If no callback
 *      function has been specified in qzStor2Init, or if qzStor2Init was
 *      not called, then the operatins will be processed syncronously.
 *
 *      By default, this perform function creates deflate blocks
 *      with no gzip nor zlib headers.
 *
 * @see
 *      None
 * @open
 *      Can we avoid key gen except for the first time
 *
 * @flow
 *      Calculate crc64 of context and store in res->ctx_crc.
 *      if direction ==  0
 *          enc_key = key_derive using cts->km1 structure
 *          compress and encrypt:
 *             compress based on QzSession (ex: dynamic, no gzip headers,
 *                deflate, level 4 or Level 9
 *                src, src_len to dest, dest_len
 *             res->ccret = return code
 *             res->crc32 = crc32
 *             res->i_crc = icrc64
 *             res->o_crc = ocrc64
 *             if  ctx->append_crc != 0
 *                append crc32 after last byte of compress data
 *                increase dest_len by 4 bytes to include crc32
 *             encrypt gcm
 *                 if ctx->useAAD then include aad_ptr with aad_sz
 *                 dest, dest_len -> dest, dest_len (in-place)
 *                 record return code in res->eret
 *      if validate or direction != 0
 *          if ctx->rederive != 0
 *             dec_key = key_drive using cts->km2 structure
 *          else
 *             dec_key = enc_key  // (key modification?)
 *             decrypt and decompress
 *                  (if verify, then input to decrypt is dest, dest_len
 *                    output of decomp is NULL.
 *                   if direction !=0 input to decrypt is in src, src_len
 *                     output of decomp is dest, dest_len )
 *             decrypt gcm
 *                if ctx->useAAD then verify AAD MAC
 *                   update return code if no match
 *                record return code in res->dret
 *             decompress
 *                record crc32 in res->dcrc32
 *                record crc64 in res->i_dcrc32 and res->o_icrc32
 *                record return code of decomp operation in res->cdret
 *
 *
 *****************************************************************************/

int16_t qzStor2Perform(QzSession_T *sess,
                       uint16_t ctx_sz,
                       QzStor2Context_T *ctx,
                       QzStor2PerformOp_T *performOp,
                       QzStor2Response_T *res, void *tag);

/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      Perform initialization of a chaining session for storage
 *
 * @description
 *    Sets up session for storage chaining requests
 *
 * @context
 *      This function shall not be called in an interrupt context.
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @blocking
 *      Yes
 * @reentrant
 *      No
 * @threadSafe
 *      Yes
 *
 * @param[in]       sess              Session pointer
 *                                    of data populated in buffer
 * @param[in]       callbackfn        Optional pointer to a callback
 *                                    function to support asynchronous
 *                                    operations.
 *
 * @retval QZ_OK          Function executed successfully
 * @retval QZ_PARAMS      *sess is NULL or member of params is invalid
 *
 * @pre
 *      None
 * @post
 *      None
 * @note
 *      Only a synchronous version of this function is provided.
 *
 * @see
 *      None
 *
 *
 *****************************************************************************/
int16_t qzStor1Init(QzSession_T *sess, qzStorageCallbackFn callbackfn);

/**
 *****************************************************************************
 * @ingroup qatZipChain
 *      Perform initialization of a chaining session for XTS
 *
 * @description
 *    Sets up session for storage chaining requests
 *
 * @context
 *      This function shall not be called in an interrupt context.
 * @assumptions
 *      None
 * @sideEffects
 *      None
 * @blocking
 *      Yes
 * @reentrant
 *      No
 * @threadSafe
 *      Yes
 *
 * @param[in]       sess              Session pointer
 *                                    of data populated in buffer
 * @param[in]       callbackfn        Optional pointer to a callback
 *                                    function to support asynchronous
 *                                    operations.
 *
 * @retval QZ_OK          Function executed successfully
 * @retval QZ_PARAMS      *sess is NULL or member of params is invalid
 *
 * @pre
 *      None
 * @post
 *      None
 * @note
 *      Only a synchronous version of this function is provided.
 *
 * @see
 *      None
 *
 *
 *****************************************************************************/
int16_t qzStor2Init(QzSession_T *sess, qzStorageCallbackFn callbackfn);
#ifdef __cplusplus
}
#endif

#endif
