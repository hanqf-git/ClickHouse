
#include "QatCodec.h"
#include <cassert>

#ifdef __cplusplus
extern "C"{
#endif 

void * getQatCodecInst()
{
    auto inst_p = new QatCodec;
    return (void *)inst_p;
}

uint32_t qatCompress(void* inst, const char * source, uint32_t source_size, char * dest)
{
    assert(inst != NULL);
    QatCodec * QatCodecInst = (QatCodec *)inst;
    return QatCodecInst->doCompressData(source,source_size,dest);
}

int32_t qatDecompress(void* inst, const char * source, uint32_t source_size, char * dest, uint32_t uncompressed_size)
{
    assert(inst != NULL);
    QatCodec * QatCodecInst = (QatCodec *)inst;
    return QatCodecInst->doDecompressData(source, source_size, dest, uncompressed_size);
}

uint32_t qatGetMaxCoDataSize(void* inst, uint32_t uncompressed_size)
{
    assert(inst != NULL);
    QatCodec * QatCodecInst = (QatCodec *)inst;
    return QatCodecInst->getMaxCompressedDataSize(uncompressed_size);
}

void relQatCodecInst(void* inst)
{
    delete (QatCodec *)inst;
}

#ifdef DP_MODE
//Async API
int32_t qatDecompressReq(void* inst, const char * source, uint32_t source_size, char * dest, uint32_t uncompressed_size)
{
    assert(inst != NULL);
    QatCodec * QatCodecInst = (QatCodec *)inst;
    return QatCodecInst->decompressReqSubmit(source, source_size, dest, uncompressed_size);
}

int32_t qatdecompressReqFlush(void* inst)
{
    assert(inst != NULL);
    QatCodec * QatCodecInst = (QatCodec *)inst;
    return  QatCodecInst->decompressReqFlush();
}
#endif
#ifdef __cplusplus
}
#endif




