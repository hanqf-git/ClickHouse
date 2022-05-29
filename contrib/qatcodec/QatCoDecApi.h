#pragma once

typedef unsigned int uint32_t;
typedef signed int int32_t;

#ifdef __cplusplus
extern "C"{
#endif 

void * getQatCodecInst();
uint32_t qatCompress(void* inst, const char * source, uint32_t source_size, char * dest);
int32_t qatDecompress(void* inst, const char * source, uint32_t source_size, char * dest, uint32_t uncompressed_size);
uint32_t qatGetMaxCoDataSize(void* inst, uint32_t uncompressed_size);
void relQatCodecInst(void* inst);

#ifdef DP_MODE
//Async API
int32_t qatDecompressReq(void* inst, const char * source, uint32_t source_size, char * dest, uint32_t uncompressed_size);
int32_t qatdecompressReqFlush(void* inst);
#endif

#ifdef __cplusplus
}
#endif
