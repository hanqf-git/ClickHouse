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

namespace ErrorCodes
{
    extern const int CANNOT_COMPRESS;
    extern const int CANNOT_DECOMPRESS;
    extern const int ILLEGAL_SYNTAX_FOR_CODEC_TYPE;
    extern const int ILLEGAL_CODEC_PARAMETER;
}

class SessionPool
{
public:
    SessionPool();
    ~SessionPool();
    static SessionPool & instance();
    static constexpr auto sessionPoolSize = 1024;
    static QzSession_T * sessionPool[sessionPoolSize];
    static std::atomic_bool sessionLock[sessionPoolSize];

    QzSession_T *ALWAYS_INLINE acquireSession(uint32_t *index_p)
    {
        uint32_t retry = 0;
        auto index = random(sessionPoolSize);
        while (tryLockSession(index) == false)
        {
            index = random(sessionPoolSize);
            retry++;
            if(retry > sessionPoolSize)
            {
                return nullptr;               
            }
        }
        *index_p = index;
        return sessionPool[index];
    }
    QzSession_T * ALWAYS_INLINE releaseSession(uint32_t index)
    {
        ReleaseSessionObjectGuard _(index);
        return sessionPool[index];
    }

private:
    size_t ALWAYS_INLINE random(uint32_t pool_size)
    {
        size_t tsc = 0;
        unsigned lo, hi;
        __asm__ volatile("rdtsc" : "=a" (lo), "=d" (hi): : );
        tsc = (((static_cast<uint64_t>(hi)) << 32) | (static_cast<uint64_t>(lo)));
        return (static_cast<size_t>((tsc*44485709377909ULL)>>4)) % pool_size;
    }

    int32_t ALWAYS_INLINE init_sess_helper(QzSession_T * sess_ptr)
    {
        if (sess_ptr == nullptr)
        {
            return -1;
        }
        auto ret = qzSetupSession(sess_ptr, &m_params);
        if (ret != QZ_OK)
        {
            return -1;
        }
        return 0;
    }

    void ALWAYS_INLINE get_default_parameter()
    {
        qzGetDefaults(&m_params);
        m_params.data_fmt           = QZ_LZ4_FH;
        m_params.comp_lvl           = 1;
        m_params.comp_algorithm     = QZ_LZ4;
        m_params.sw_backup          = 1;
        m_params.hw_buff_sz         = QZ_HW_BUFF_SZ;
        m_params.strm_buff_sz       = QZ_HW_BUFF_SZ;
        m_params.input_sz_thrshold  = QZ_COMP_THRESHOLD_MINIMUM;
    }

    int32_t ALWAYS_INLINE initSessionPool()
    {
        static bool initialized = false;

        if (initialized == false)
        {
            const int32_t size = sizeof(QzSession_T);
            if (size < 0) return -1;
            for (int i = 0; i < sessionPoolSize; ++i)
            {
                sessionPool[i] = nullptr;
                QzSession_T *sess_ptr = reinterpret_cast<QzSession_T *>(new uint8_t [size]);
                if (init_sess_helper(sess_ptr) < 0) return -1;
                sessionPool[i] = sess_ptr;
                sessionLock[i].store(false);
            }
            initialized = true;
        }
        return 0;
    }

    bool ALWAYS_INLINE tryLockSession(size_t index)
    {
        bool expected = false;
        return sessionLock[index].compare_exchange_strong(expected, true);
    }

    void ALWAYS_INLINE destroySessionPool()
    {
        const uint32_t size = sizeof(QzSessionParams_T);
        for (uint32_t i = 0; i < sessionPoolSize && size > 0; ++i)
        {
            while (tryLockSession(i) == false) {}
            if (sessionPool[i])
            {
                qzTeardownSession(sessionPool[i]);
                delete[] sessionPool[i];
            }
            sessionPool[i] = nullptr;
            sessionLock[i].store(false);
        }
        LOG_TRACE(&Poco::Logger::get("CompressionCodecQatLZ4"), "destroySessionPool done.");
    }

    struct ReleaseSessionObjectGuard
    {
        uint32_t index;
        ReleaseSessionObjectGuard() = delete;
    public:
        ALWAYS_INLINE ReleaseSessionObjectGuard(const uint32_t i) : index(i) {}
        ALWAYS_INLINE ~ReleaseSessionObjectGuard() { sessionLock[index].store(false); }
    };

    private:
        QzSessionParams_T m_params;
        uint8_t m_sw_backup = 1; //sw backup diabled.
};

QzSession_T *SessionPool::sessionPool[SessionPool::sessionPoolSize];
std::atomic_bool SessionPool::sessionLock[SessionPool::sessionPoolSize];

SessionPool & SessionPool::instance()
{
    static SessionPool ret;
    return ret;
}

SessionPool::SessionPool()
{
    QzSession_T tempSess;
    int ret = qzInit(&tempSess, m_sw_backup);
    if (QZ_PARAMS == ret || QZ_NOSW_NO_HW == ret || QZ_FAIL == ret) 
    {
        throw Exception("QatLZ4 init failed", ErrorCodes::ILLEGAL_CODEC_PARAMETER);
    }
    get_default_parameter();

    if( initSessionPool() < 0 )
    {
        throw Exception("SessionPool initializing fail!", ErrorCodes::ILLEGAL_CODEC_PARAMETER);
    }
}
SessionPool::~SessionPool()
{
    LOG_TRACE(&Poco::Logger::get("CompressionCodecQatLZ4"), "~SessionPool called.");
    QzSession_T tempSess;
    destroySessionPool();
    qzClose(&tempSess);
    LOG_TRACE(&Poco::Logger::get("CompressionCodecQatLZ4"), "~SessionPool done.");
}

class CompressionCodecQatLZ4 : public  ICompressionCodec  // TODO: can be derived from CompressionCodecLZ4 to only replace compression or decompression, to let QAT only do com or dec task.
{
public:
    explicit CompressionCodecQatLZ4();
    ~CompressionCodecQatLZ4() override;

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
    Poco::Logger * log = &Poco::Logger::get("CompressionCodecQatLZ4");
};

CompressionCodecQatLZ4::CompressionCodecQatLZ4()
{
    setCodecDescription("QATLZ4");
    LOG_TRACE(log, "CompressionCodecQatLZ4() called.");
}

CompressionCodecQatLZ4::~CompressionCodecQatLZ4()
{
    LOG_TRACE(log, "~CompressionCodecQatLZ4() called.");
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
    outputSizeLong += CEIL_DIV(inputSizeLong, 1520) * 13 ;
    outputSizeLong += 19 + 8; //lz4 header and footer

    LOG_TRACE(log, "QatLz4 getMaxCompressedDataSize ,uncompressed_size {}, max compressed size {} .", uncompressed_size, (unsigned int)outputSizeLong );

    return (unsigned int)outputSizeLong;
}

unsigned int CompressionCodecQatLZ4::doCompressData(const char * source, unsigned int source_size, char * dest) const
{
    LOG_TRACE(log, "doCompressData called.");
    uint32_t sessionIndex = 0;
    QzSession_T * sessPtr = SessionPool::instance().acquireSession(&sessionIndex);
    if(sessPtr == nullptr)
    {
        LOG_WARNING(log, "QATLZ4 compression Acquire session failed!");
        throw Exception("Cannot compress, session ptr null", ErrorCodes::CANNOT_COMPRESS);
    }

    Int32  ret = QZ_OK;
    unsigned int dest_size = getMaxCompressedDataSize(source_size);
    unsigned int last_flag = 1;
    ret = qzCompress(sessPtr, reinterpret_cast<const unsigned char *>(source), &source_size, reinterpret_cast<unsigned char *>(dest), &dest_size, last_flag);
    if (ret != QZ_OK)
    {
        SessionPool::instance().releaseSession(sessionIndex);
        LOG_WARNING(log, "QATLZ4 compress failed!");
        throw Exception("Cannot compress, compress return error", ErrorCodes::CANNOT_COMPRESS);
    }
    SessionPool::instance().releaseSession(sessionIndex);
    LOG_TRACE(log, "doCompressData called done.");
    return dest_size;
}

void CompressionCodecQatLZ4::doDecompressData(const char * source, unsigned int source_size, char * dest, unsigned int uncompressed_size) const
{
    LOG_TRACE(log, "doDecompressData called.");

    uint32_t sessionIndex = 0;
    QzSession_T * sessPtr = SessionPool::instance().acquireSession(&sessionIndex);
    if(sessPtr == nullptr)
    {
        LOG_WARNING(log, "QATLZ4 decompression acquire session failed!");
        throw Exception("Cannot decompress, session ptr null", ErrorCodes::CANNOT_DECOMPRESS);
    }

    Int32  ret = QZ_OK;
    ret = qzDecompress(sessPtr, reinterpret_cast<const unsigned char *>(source), &source_size, reinterpret_cast<unsigned char *>(dest), &uncompressed_size);
    if (ret != QZ_OK)
    {  
        SessionPool::instance().releaseSession(sessionIndex);
        LOG_WARNING(log, "Cannot decompress, decompress return error code {}!", ret);
        throw Exception("Cannot decompress, decompress return error", ErrorCodes::CANNOT_DECOMPRESS);
    }
    SessionPool::instance().releaseSession(sessionIndex);
    LOG_TRACE(log, "doDecompressData called done.");
}

void registerCodecQatLZ4(CompressionCodecFactory & factory)
{
    LOG_TRACE(&Poco::Logger::get("CompressionCodecQatLZ4"), "registerCodecQatLZ4 called.");
    factory.registerSimpleCompressionCodec("QATLZ4", static_cast<UInt8>(CompressionMethodByte::QATLZ4), [&] ()
    {
        return std::make_shared<CompressionCodecQatLZ4>();
    });
}

}
