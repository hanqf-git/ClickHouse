#include <lz4.h>
#include <lz4hc.h>

#include <qatzip.h>
#include <qz_utils.h>

#include <Compression/ICompressionCodec.h>
#include <Compression/CompressionInfo.h>
#include <Compression/CompressionFactory.h>
#include <Compression/LZ4_decompress_faster.h>
#include <Parsers/IAST.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <IO/WriteBuffer.h>
#include <IO/WriteHelpers.h>
#include <IO/BufferWithOwnMemory.h>

#pragma GCC diagnostic ignored "-Wold-style-cast"


namespace DB
{

class CompressionCodecQatLZ4 : public  ICompressionCodec  // TODO: can be derived from CompressionCodecLZ4 to only replace compression or decompression, to let QAT only do com or dec task.
{
public:
    explicit CompressionCodecLZ4();

    uint8_t getMethodByte() const override;

    UInt32 getAdditionalSizeAtTheEndOfBuffer() const override { return LZ4::ADDITIONAL_BYTES_AT_END_OF_BUFFER; }  //todo: can be removed?

    void updateHash(SipHash & hash) const override;
    UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const override;

protected:
    UInt32 doCompressData(const char * source, UInt32 source_size, char * dest) const override;
    void doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const override;

    bool isCompression() const override { return true; }
    bool isGenericCompression() const override { return true; }

private:
    QzSession_T m_sess;  // TODO: make a session pool to use
    QzSessionParams_T m_params;
    uint8_t m_sw_backup = 1; //sw backup enabled.
};

CompressionCodecQatLZ4::CompressionCodecQatLZ4()
{
    setCodecDescription("LZ4QAT");

    int ret = qzInit(&m_sess, m_sw_backup);
    if (QZ_PARAMS == ret || QZ_NOSW_NO_HW == ret || QZ_FAIL == ret) 
    {
        throw Exception("QatLZ4 init failed", ErrorCodes::CANNOT_COMPRESS);
    }

    qzGetDefaults(&m_params);
    m_params.data_fmt           = QZ_LZ4_FH;
    m_params.comp_lvl           = 1;
    m_params.comp_algorithm     = QZ_LZ4;
    m_params.sw_backup          = 0;
    m_params.hw_buff_sz         = QZ_HW_BUFF_SZ;
    m_params.strm_buff_sz       = QZ_HW_BUFF_SZ;
    m_params.input_sz_thrshold  = QZ_COMP_THRESHOLD_MINIMUM;

    ret = qzSetupSession(&m_sess, &m_params);
    if (ret != QZ_OK)
    {
        throw Exception("QatLZ4 setup session failed", ErrorCodes::CANNOT_COMPRESS);
    }
}

uint8_t CompressionCodecQatLZ4::getMethodByte() const
{
    return static_cast<uint8_t>(CompressionMethodByte::QATLZ4);
}

void CompressionCodecQatLZ4::updateHash(SipHash & hash) const
{
    getCodecDesc()->updateTreeHash(hash);
}
#define CEIL_DIV(x, y) (((x) + (y)-1) / (y))

UInt32 CompressionCodecQatLZ4::getMaxCompressedDataSize(UInt32 uncompressed_size) const
{
    //return LZ4_COMPRESSBOUND(uncompressed_size);
    /* Formula for QAT GEN4 LZ4:
    * sourceLen + Ceil(sourceLen/1520) * 13 + 1024 */
    UInt64 outputSizeLong;
    UInt64 inputSizeLong = (UInt64)uncompressed_size;

    /* Formula for GEN4 LZ4:
     * sourceLen + Ceil(sourceLen/1520) * 13 + 1024 */
    outputSizeLong = inputSizeLong + 1024;
    outputSizeLong += CEIL_DIV(inputSizeLong, 1520) * 13;
    outputSizeLong += 19 + 8; //lz4 header and footer

    return (UInt32)outputSizeLong;
}

UInt32 CompressionCodecQatLZ4::doCompressData(const char * source, UInt32 source_size, char * dest) const
{
    Int32  ret = QZ_OK;
    UInt32 dest_size = getMaxCompressedDataSize(source_size);
    UInt32 last_flag = 1;

    ret = qzCompress(&m_sess, source, &source_size, dest, &dest_size, last_flag);
    if (ret != QZ_OK)
    {
        throw Exception("Cannot compress", ErrorCodes::CANNOT_COMPRESS);
        return 0;
    }
// TODO: check whether the compressed  result of LZ4 has header and footer .
    return dest_size;
}

void CompressionCodecQatLZ4::doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const
{
    Int32  ret = QZ_OK;

    ret = qzDecompress(&m_sess, source, &source_size, dest, &uncompressed_size);
    if (ret != QZ_OK)
    {
        throw Exception("Cannot decompress", ErrorCodes::CANNOT_DECOMPRESS);
    }
}

void registerCodecLZ4Qat(CompressionCodecFactory & factory)
{
    factory.registerSimpleCompressionCodec("QATLZ4", static_cast<UInt8>(CompressionMethodByte::LZ4QAT), [&] ()
    {
        return std::make_shared<CompressionCodecQatLZ4>();
    });
}

}
