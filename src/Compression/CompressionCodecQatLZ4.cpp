#include <lz4.h>
#include <lz4hc.h>

#include <qatzip.h>
#include <qz_utils.h>
#include <Poco/Logger.h>
#include <base/logger_useful.h>

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

QzSession_T m_sess;  // TODO: make a session pool to use

class CompressionCodecQatLZ4 : public  ICompressionCodec  // TODO: can be derived from CompressionCodecLZ4 to only replace compression or decompression, to let QAT only do com or dec task.
{
public:
    explicit CompressionCodecQatLZ4();

    uint8_t getMethodByte() const override;

    unsigned int getAdditionalSizeAtTheEndOfBuffer() const override { return LZ4::ADDITIONAL_BYTES_AT_END_OF_BUFFER; }  //todo: can be removed?

    void updateHash(SipHash & hash) const override;
    unsigned int getMaxCompressedDataSize(unsigned int uncompressed_size) const override;

protected:
    unsigned int doCompressData(const char * source, unsigned int source_size, char * dest) const override;
    void doDecompressData(const char * source, unsigned int source_size, char * dest, unsigned int uncompressed_size) const override;

    bool isCompression() const override { return true; }
    bool isGenericCompression() const override { return true; }

private:
    QzSessionParams_T m_params;
    uint8_t m_sw_backup = 1; //sw backup enabled.
    Poco::Logger * log = &Poco::Logger::get("CompressionCodecQatLZ4");
};

namespace ErrorCodes
{
    extern const int CANNOT_COMPRESS;
    extern const int CANNOT_DECOMPRESS;
    extern const int ILLEGAL_SYNTAX_FOR_CODEC_TYPE;
    extern const int ILLEGAL_CODEC_PARAMETER;
}

CompressionCodecQatLZ4::CompressionCodecQatLZ4()
{
    setCodecDescription("QATLZ4");

    int ret = qzInit(&m_sess, m_sw_backup);
    if (QZ_PARAMS == ret || QZ_NOSW_NO_HW == ret || QZ_FAIL == ret) 
    {
        throw Exception("QatLZ4 init failed", ErrorCodes::ILLEGAL_CODEC_PARAMETER);
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
        throw Exception("QatLZ4 setup session failed", ErrorCodes::ILLEGAL_CODEC_PARAMETER);
    }
    LOG_WARNING(log, "CompressionCodecQatLZ4() called.");
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

unsigned int CompressionCodecQatLZ4::getMaxCompressedDataSize(unsigned int uncompressed_size) const
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

    return (unsigned int)outputSizeLong;
}

unsigned int CompressionCodecQatLZ4::doCompressData(const char * source, unsigned int source_size, char * dest) const
{
    Int32  ret = QZ_OK;
    unsigned int dest_size = getMaxCompressedDataSize(source_size);
    unsigned int last_flag = 1;
    LOG_WARNING(log, "doCompressData called.");
    ret = qzCompress(&m_sess, reinterpret_cast<const unsigned char *>(source), &source_size, reinterpret_cast<unsigned char *>(dest), &dest_size, last_flag);
    if (ret != QZ_OK)
    {
        throw Exception("Cannot compress", ErrorCodes::CANNOT_COMPRESS);
    }
    LOG_WARNING(log, "doCompressData called done.");
// TODO: check whether the compressed  result of LZ4 has header and footer .
    return dest_size;
}

void CompressionCodecQatLZ4::doDecompressData(const char * source, unsigned int source_size, char * dest, unsigned int uncompressed_size) const
{
    Int32  ret = QZ_OK;
    LOG_WARNING(log, "doDecompressData called.");
    ret = qzDecompress(&m_sess, reinterpret_cast<const unsigned char *>(source), &source_size, reinterpret_cast<unsigned char *>(dest), &uncompressed_size);
    if (ret != QZ_OK)
    {
        throw Exception("Cannot decompress", ErrorCodes::CANNOT_DECOMPRESS);
    }
    LOG_WARNING(log, "doDecompressData called done.");
}

void registerCodecQatLZ4(CompressionCodecFactory & factory)
{
    LOG_WARNING(&Poco::Logger::get("CompressionCodecQatLZ4"), "registerCodecQatLZ4 called.");
    factory.registerSimpleCompressionCodec("QATLZ4", static_cast<UInt8>(CompressionMethodByte::QATLZ4), [&] ()
    {
        return std::make_shared<CompressionCodecQatLZ4>();
    });
}

}
