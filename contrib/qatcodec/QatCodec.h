#pragma once

#include <memory>
#include <semaphore.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef __cplusplus
extern "C"{
#endif 

#include "cpa.h"
#include "cpa_dc.h"
#include "cpa_dc_dp.h"


#ifdef __cplusplus
}
#endif

typedef unsigned int uint32_t;
typedef pthread_t qatThread_t;
typedef void *(*thread_routin)(void *args);

#define INVALID_INSTANCE_INDEX 0xFFFF
#define MAX_BUFFER_SIZE_LEVEL  3

#define  DP_MODE
#ifndef DP_MODE
  #define  TL_MODE
#endif

struct VirBufferList
{
    Cpa8U *pBuffer;
    Cpa8U *pBufferNew;  //Allocate when the size of pSrcBuffer is not enough.
};

const uint32_t MAX_ASYNC_PROC_COUNT = 4;
struct OpMemRes
{
    Cpa16U                 list_count;
#ifdef DP_MODE
    Cpa16U                 req_count;
    CpaPhysBufferList      *p_src_phy_buf_list[MAX_ASYNC_PROC_COUNT];  //Need physical cont in memory and aligned in 8 bytes.
    CpaPhysBufferList      *p_dst_phy_buf_list[MAX_ASYNC_PROC_COUNT];
    VirBufferList          src_vir_buf_list[MAX_ASYNC_PROC_COUNT];
    VirBufferList          dst_vir_buf_list[MAX_ASYNC_PROC_COUNT];
    char                   *dest[MAX_ASYNC_PROC_COUNT];
    CpaDcDpOpData          *p_op_data[MAX_ASYNC_PROC_COUNT];
#else
    CpaBufferList          **buffer_list;
#endif
};

struct DcInstanceRes
{
    CpaInstanceHandle  m_dc_instance_handle;
    unsigned int       m_dc_instance_started;
    CpaDcSessionHandle m_session_handle;
    unsigned int       m_instance_lock;
#ifdef DP_MODE
    OpMemRes           m_op_mem_res;
    unsigned int       m_need_flush;
#else
    unsigned int       m_op_done;
    OpMemRes           m_src_buffers;
    OpMemRes           m_dst_buffers;
#endif
    unsigned int       m_mem_setup;
    CpaInstanceInfo2   m_instance_info;
};

class QatCodec
{
    public:
        QatCodec();
        ~QatCodec();
        uint32_t doCompressData(const char * source, uint32_t source_size, char * dest);
        int32_t doDecompressData(const char * source, uint32_t source_size, char * dest, uint32_t uncompressed_size);
        uint32_t getMaxCompressedDataSize(uint32_t uncompressed_size);
        int32_t  get_qat_status() {return m_qat_status;}

        //Async mode
        int32_t decompressReqSubmit(const char * source, uint32_t source_size, char * dest, uint32_t uncompressed_size, CpaBoolean doNow = CPA_TRUE);
        int32_t decompressReqFlush(void);
    private:
        static CpaStatus osMemAlloc(void **ppMemAddr, Cpa32U sizeBytes);
        static void osMemFree(void **ppMemAddr);
        static CpaStatus phyContigMemAlloc(void **ppMemAddr, Cpa32U sizeBytes, Cpa32U numaNode, Cpa32U alignment);
        static void phyContigMemFree(void **ppMemAddr);

        static CpaStatus threadSleep(Cpa32U ms);
    #ifdef DP_MODE
        static void dcCallback(CpaDcDpOpData *pOpData);
    #else
        static void dcCallback(void *pCallbackTag, CpaStatus status);
    #endif    

        static void EvaluateCnVFlag(const CpaDcInstanceCapabilities *const cap,
                                  CpaBoolean *cnv,
                                  CpaBoolean *cnvnr);
        static CpaStatus getDcCapabilities(CpaDcInstanceCapabilities *capabilities);
        static void getCnvFlagInternal(CpaBoolean *cnv, CpaBoolean *cnvnr);
        static CpaBoolean getCnVFlag(void);
        static CpaBoolean getCnVnRFlag(void);
        static CpaPhysicalAddr qatVirtToPhys(void *virtAddr);

        //InstancePool functions
        static CpaStatus setupInstancePool(void);
        static void destroyInstancePool(void);
        static CpaStatus getQatInstance(Cpa16U & dc_inst_index, Cpa16U start_index);
        static void ReleaseQatInstance(Cpa16U & inst_index);
        static Cpa16U getBuffSizeIdxByInstIdx(Cpa16U & inst_index);
        static Cpa16U getBuffSizeIdxByDataSize(Cpa32U uncompressed_size);
        Cpa16U getStartIndex(Cpa32U uncompressed_size);

        static CpaStatus qatInit(void);
        CpaStatus setupSession(Cpa32U uncompressed_size);
        CpaStatus setupSessMemRes();
        static void freeSessMemRes(Cpa16U  dc_inst_index);
        void queryInstanceStatics(void);
        void teardownSession();
        static void qatClose()__attribute__((destructor));

        static int32_t checkDataOffset(const char * source, uint32_t source_size);

    private:
        CpaDcSessionHandle m_session_handle = NULL;
        CpaInstanceHandle  m_dc_inst_handle = NULL;
        Cpa16U             m_dc_inst_index = INVALID_INSTANCE_INDEX;

        static Cpa32U      m_max_src_buffer_size[MAX_BUFFER_SIZE_LEVEL];
        static Cpa32U      m_max_dst_buffer_size[MAX_BUFFER_SIZE_LEVEL];

        static int32_t     m_qat_status;
        static Cpa16U      m_instance_num;
        static Cpa16U      m_instance_buff_level2_pos;
        static Cpa16U      m_instance_buff_level3_pos;
        /*
         * ----------------------------------7/8-------15/16-----
         *    src buffer size: 128K          |   640K   |    1M  |
        */
        static DcInstanceRes * m_dc_instance_res_pool;
        
        static pthread_mutex_t m_lock_mutex;
};

#if 0
std::shared_ptr<QatCodec> getQatCodecInstance();
#else
QatCodec * getQatCodecInstance();
#endif


