
#include <semaphore.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <immintrin.h>
#include <algorithm>
#include <cassert>
//measure cycles
#include <x86intrin.h>
#include <lz4.h>

#ifdef __cplusplus
extern "C"{
#endif

#include "cpa.h"
#include "cpa_dev.h"
#include "cpa_dc.h"
#include "cpa_dc_dp.h"

#include "qae_mem.h"
#include "icp_sal_user.h"
#include "icp_sal_poll.h"

#ifdef __cplusplus
}
#endif

#include "QatCodec.h"

//#define SVM_ENABLED
//#define OUTPUT_INST_INFO
//#define PRINT_COM_INPUT_DATA
//#define PRINT_DEC_INPUT_DATA
//#define PRINT_DEC_OUTPUT_DATA

//#define CHECK_DATA_OFFSET

//#define MEAS_TIME

const char *g_dev_tag = "SSL";

#define QAT_NOT_INIT   -1
#define QAT_NOT_AVAIL  0
#define QAT_AVAIL       1
#define MAX_INSTANCES   1024
#define TIMEOUT_MS              1000 /* 5 seconds */    
#define MAX_TRY_COUNT            3

#define SRC_BUFFER_SIZE_LEVEL1         (128*1024)
#define SRC_BUFFER_SIZE_LEVEL2         (640*1024)
#define SRC_BUFFER_SIZE_LEVEL3         (1024*1024)

#define DST_BUFFER_SIZE_LEVEL1         (133227) //cpaDcLZ4CompressBound(SRC_BUFFER_SIZE_LEVEL1)
#define DST_BUFFER_SIZE_LEVEL2         (662000)
#define DST_BUFFER_SIZE_LEVEL3         (1058570)

int32_t QatCodec::m_qat_status = QAT_NOT_INIT;
Cpa16U QatCodec::m_instance_num = 0;
Cpa16U QatCodec::m_instance_buff_level2_pos = 0;
Cpa16U QatCodec::m_instance_buff_level3_pos = 0;
DcInstanceRes * QatCodec::m_dc_instance_res_pool = (DcInstanceRes *)NULL;

Cpa32U  QatCodec::m_max_src_buffer_size[MAX_BUFFER_SIZE_LEVEL] = {SRC_BUFFER_SIZE_LEVEL1, SRC_BUFFER_SIZE_LEVEL2, SRC_BUFFER_SIZE_LEVEL3};
Cpa32U  QatCodec::m_max_dst_buffer_size[MAX_BUFFER_SIZE_LEVEL] = {DST_BUFFER_SIZE_LEVEL1, DST_BUFFER_SIZE_LEVEL2, DST_BUFFER_SIZE_LEVEL3};

pthread_mutex_t QatCodec::m_lock_mutex = PTHREAD_MUTEX_INITIALIZER;

using namespace std;
#define PRINT_LOG
#ifdef PRINT_LOG
  #define PRINT_DISABLE    0
  #define PRINT_ERR_INFO   1
  #define PRINT_DEBUG_INFO 2

  //static int gDebugParam = PRINT_DEBUG_INFO;
  static int gDebugParam = PRINT_ERR_INFO;

  #define PRINT_DBG(args...)                                                     \
        do                                                                         \
        {                                                                          \
            if (gDebugParam>= PRINT_DEBUG_INFO )                               \
            {                                                                      \
                printf("%s(): ", __func__);                                        \
                printf(args);                                                      \
                fflush(stdout);                                                    \
            }                                                                      \
        } while (0)

    /**< Prints the arguments */
  #ifndef PRINT
  #define PRINT(args...)                                                         \
        do                                                                         \
        {                                                                          \
            printf(args);                                                          \
        } while (0)
  #endif

    /**< Prints the name of the function and the arguments */
  #ifndef PRINT_ERR
  #define PRINT_ERR(args...)                                                     \
        do                                                                         \
        {                                                                          \
            if (gDebugParam>= PRINT_ERR_INFO)                                  \
            {                                                                      \
                printf("%s(): error -- ", __func__);                                \
                printf(args);                                                      \
            }                                                                      \
        } while (0)
  #endif
#else
  #define PRINT_DBG(args...)
  #define PRINT(args...)
  #define PRINT_ERR(args...)
#endif

/*----------------Function of memory allocate------------------------------------*/
__inline CpaStatus QatCodec::osMemAlloc(void **ppMemAddr, Cpa32U sizeBytes)
{
    *ppMemAddr = malloc(sizeBytes);
    if (NULL == *ppMemAddr)
    {
        return CPA_STATUS_RESOURCE;
    }
    return CPA_STATUS_SUCCESS;
}

__inline void QatCodec::osMemFree(void **ppMemAddr)
{
    if (NULL != *ppMemAddr)
    {
        free(*ppMemAddr);
        *ppMemAddr = NULL;
    }
}

//Memory allocated by this function is guaranteed to be physically contiguous
__inline CpaStatus QatCodec::phyContigMemAlloc(void **ppMemAddr,
                                           Cpa32U sizeBytes,
                                           Cpa32U numaNode,
                                           Cpa32U alignment)
{
    *ppMemAddr = qaeMemAllocNUMA(sizeBytes, numaNode, alignment);
    if (NULL == *ppMemAddr)
    {
        return CPA_STATUS_RESOURCE;
    }
    return CPA_STATUS_SUCCESS;
}

//The memory must have been allocated by the function Mem_Alloc_Contig().
__inline void QatCodec::phyContigMemFree(void **ppMemAddr)
{
    if (NULL != *ppMemAddr)
    {
        qaeMemFreeNUMA(ppMemAddr);
        *ppMemAddr = NULL;
    }
}

#define OS_MALLOC(ppMemAddr, sizeBytes)  QatCodec::osMemAlloc((void **)(ppMemAddr), (sizeBytes))

#define OS_FREE(pMemAddr) QatCodec::osMemFree((void **)&pMemAddr)

#define PHY_CON_MEM_ALLOC(ppMemAddr, sizeBytes, numaNode)  QatCodec::phyContigMemAlloc((void **)(ppMemAddr), (sizeBytes),  (numaNode), 1)

#define PHYS_CONTIG_FREE(pMemAddr) QatCodec::phyContigMemFree((void **)&pMemAddr)

#define PHY_CON_MEM_ALLOC_ALIGNED(ppMemAddr, sizeBytes, numaNode, alignment)             \
    QatCodec::phyContigMemAlloc((void **)(ppMemAddr), (sizeBytes), (numaNode), (alignment))

/*-------------------Macros & functions to construct (De)Compression request parameters.------------------------------*/
#define CNV(x) (x)->compressAndVerify
#define SET_CNV(x, v) (CNV(x) = (v))
#define CNV_RECOVERY(x) (x)->compressAndVerifyAndRecover
#define SET_CNV_RECOVERY(x, v) (CNV_RECOVERY(x) = v)

#define SET_DEFAULT_OP_PARAM(x, flag)                                       \
    do                                                                         \
    {                                                                          \
        (x)->flushFlag = (flag);                                               \
        (x)->compressAndVerify = CPA_FALSE;                                  \
        (x)->compressAndVerifyAndRecover = CPA_FALSE;                        \
        (x)->inputSkipData.skipMode = CPA_DC_SKIP_DISABLED;                    \
        (x)->outputSkipData.skipMode = CPA_DC_SKIP_DISABLED;                   \
    } while (0)

void QatCodec::EvaluateCnVFlag(const CpaDcInstanceCapabilities *const cap,
                                  CpaBoolean *cnv,
                                  CpaBoolean *cnvnr)
{
    CpaBoolean fw_cnv_capable = CPA_FALSE;
    CpaBoolean cnv_loose_mode = CPA_FALSE;
    CpaBoolean cnvOpFlag = CPA_FALSE;
    CpaBoolean cnvnrOpFlag = CPA_FALSE;

    /* When capabilities are known, fill in the queried values */
    if (cap != NULL)
    {
        fw_cnv_capable = CNV(cap);
        cnv_loose_mode =
            (cap->compressAndVerifyStrict != CPA_TRUE) ? CPA_TRUE : CPA_FALSE;

        cnvnrOpFlag = CNV_RECOVERY(cap);
    }
    /* Determine the value of CompressAndVerify flag used by DP and
     * Traditional API depending on the FW CNV capability and CNV mode
     * of operation. The API will accept the submission of payload only
     * if this flag value is correct for the combination.
     * FW-CNV-CAPABLE MODE PERMITTED-OPERATION CNVFLAG
     *    Y            S    CompressWithVerify  CPA_TRUE
     *    Y            L    Compress only       CPA_FALSE
     *    N            S    NONE                 NA
     *    N            L    Compress only       CPA_FALSE
     */
    if (fw_cnv_capable == CPA_TRUE)
    {
        cnvOpFlag = (cnv_loose_mode == CPA_FALSE) ? CPA_TRUE : CPA_FALSE;
    }
    else
    {
        cnvOpFlag = CPA_FALSE;
    }

    /* CNV Recovery only possible when
     * CNV is enabled/present.
     */
    if (cnvOpFlag == CPA_FALSE)
    {
        cnvnrOpFlag = CPA_FALSE;
    }

    if (cnv != NULL)
        *cnv = cnvOpFlag;

    if (cnvnr != NULL)
        *cnvnr = cnvnrOpFlag;

    return;
}


/*----------------------------Polling Thread related functions-----------------------------*/
__inline CpaStatus QatCodec::threadSleep(Cpa32U ms)
{
    int ret = 0;
    struct timespec resTime, remTime;
    resTime.tv_sec = ms / 1000;
    resTime.tv_nsec = (ms % 1000) * 1000000;
    do
    {
        ret = nanosleep(&resTime, &remTime);
        resTime = remTime;
    } while ((ret != 0) && (errno == EINTR));

    if (ret != 0)
    {
        PRINT_ERR("nanoSleep failed with code %d\n", ret);
        return CPA_STATUS_FAIL;
    }
    else
    {
        return CPA_STATUS_SUCCESS;
    }

}
#ifdef  DP_MODE
void QatCodec::dcCallback(CpaDcDpOpData *pOpData)
{
    assert(pOpData != NULL);

    uint64_t callback_tag = (unsigned long) pOpData->pCallbackTag;

    PRINT_DBG("Callback called with responseStatus = %d, pCallbackTag(0x%lx).\n", \
                     pOpData->responseStatus, callback_tag);

    /* indicate that the function has been called */
    uint16_t inst_index = (uint16_t)(callback_tag >> 16);
    if (inst_index >= m_instance_num)
    {
        PRINT_ERR("inst_index (%d) is wrong", inst_index);
        assert(inst_index < m_instance_num);
    }

    uint16_t req_index = (uint16_t)(callback_tag);
    assert(req_index < MAX_ASYNC_PROC_COUNT);

    m_dc_instance_res_pool[inst_index].m_op_mem_res.p_op_data[req_index]   = pOpData;

    PRINT_DBG("Set process res flag done, instance index=%d, req_index=%d, pOpData=0x%lx \n", inst_index, req_index, (uint64_t)pOpData);
}
#else
void QatCodec::dcCallback(void *pCallbackTag, CpaStatus status)
{
    PRINT_DBG("Callback called with status = %d, pCallbackTag(0x%lx).\n", status, (uint64_t)pCallbackTag);

    if (NULL != pCallbackTag)
    {
        /* indicate that the function has been called */
        auto inst_index = *((Cpa16U *)pCallbackTag);
        if (inst_index>=m_instance_num)
        {
            PRINT_ERR("inst_index (%d) is wrong", inst_index);
        }
        assert(inst_index < m_instance_num);
        m_dc_instance_res_pool[inst_index].m_op_done = 1;
        PRINT_DBG("Set process res flag done, instance index =%d \n", inst_index);
    }
}
#endif



QatCodec::QatCodec()
{
    CpaStatus stat = CPA_STATUS_SUCCESS;

    if (m_qat_status != QAT_AVAIL)
    {
        stat = qatInit();
        if (stat != CPA_STATUS_SUCCESS)
        {
            PRINT_ERR("qatInit failed. (stat = %d)\n", stat);
        }
    }
}

QatCodec::~QatCodec()
{
}

CpaStatus QatCodec::getDcCapabilities(CpaDcInstanceCapabilities *capabilities)
{
    CpaStatus status;
    CpaInstanceHandle instHandle;
    Cpa16U numInstances = 0;

    /* Get the number of instances */
    status = cpaDcGetNumInstances(&numInstances);
    if (CPA_STATUS_SUCCESS != status)
        return CPA_STATUS_FAIL;

    if (numInstances == 0)
        return CPA_STATUS_FAIL;

    status = cpaDcGetInstances(1, &instHandle);
    if (status != CPA_STATUS_SUCCESS)
        return CPA_STATUS_FAIL;

    status = cpaDcQueryCapabilities(instHandle, capabilities);
    if (CPA_STATUS_SUCCESS != status)
        return CPA_STATUS_FAIL;

    return CPA_STATUS_SUCCESS;
}

void QatCodec::getCnvFlagInternal(CpaBoolean *cnv, CpaBoolean *cnvnr)
{
    CpaDcInstanceCapabilities cap;
    if (getDcCapabilities(&cap) != CPA_STATUS_SUCCESS)
    {
        return EvaluateCnVFlag(NULL, cnv, cnvnr);
    }

    return EvaluateCnVFlag(&cap, cnv, cnvnr);
}

CpaBoolean QatCodec::getCnVFlag(void)
{
    static CpaBoolean cnvOpFlag;
    static CpaBoolean initialised = CPA_FALSE;

    if (initialised == CPA_FALSE)
    {
        getCnvFlagInternal(&cnvOpFlag, (CpaBoolean *)NULL);
        initialised = CPA_TRUE;
    }

    return cnvOpFlag;
}

CpaBoolean QatCodec::getCnVnRFlag(void)
{
    static CpaBoolean cnvnrOpFlag;
    static CpaBoolean initialised = CPA_FALSE;

    if (initialised == CPA_FALSE)
    {
        getCnvFlagInternal((CpaBoolean *)NULL, &cnvnrOpFlag);
        initialised = CPA_TRUE;
    }

    return cnvnrOpFlag;
}

/*--------------------Private functions-------------------------------------------*/
CpaStatus QatCodec::getQatInstance(Cpa16U & dc_inst_index, Cpa16U start_index)
{
    Cpa16U end_num = m_instance_num + start_index;
    for (Cpa16U inst_i=start_index; inst_i<end_num; inst_i++)
    {
        auto index = (inst_i % m_instance_num);
        if (0 ==  __sync_lock_test_and_set(&m_dc_instance_res_pool[index].m_instance_lock, 1))
        {
            PRINT_DBG("Qat instance ( index =%d, addr=0x%lx ) selected \n", index, (uint64_t)m_dc_instance_res_pool[index].m_dc_instance_handle);
            dc_inst_index = index;
            return  CPA_STATUS_SUCCESS;
        }
    }

    PRINT_ERR("Qat get instance failed, all instances used now \n");

    return  CPA_STATUS_RESOURCE;
}

void QatCodec::ReleaseQatInstance(Cpa16U & inst_index)
{
    if (inst_index != INVALID_INSTANCE_INDEX)
    {
        __sync_lock_release(&m_dc_instance_res_pool[inst_index].m_instance_lock);
        PRINT_DBG("Qat instance ( index =%d, addr=0x%lx ) released \n", inst_index, (uint64_t)m_dc_instance_res_pool[inst_index].m_dc_instance_handle);
        inst_index = INVALID_INSTANCE_INDEX;
    }
}


__inline CpaPhysicalAddr QatCodec::qatVirtToPhys(void *virtAddr)
{
    return (CpaPhysicalAddr)qaeVirtToPhysNUMA(virtAddr);
}
//#define OUTPUT_INST_INFO
#define REORG_INSTACE_POOL
CpaStatus QatCodec::setupInstancePool(void)
{
    CpaStatus stat = CPA_STATUS_SUCCESS;

    stat = cpaDcGetNumInstances(&m_instance_num);
    if (CPA_STATUS_SUCCESS != stat) {
        PRINT_ERR("Failed to get instance number, stat(%d)\n", stat);
        return stat;
    }

    if (0 == m_instance_num)
    {
        PRINT_ERR("No instances found for %s\n", g_dev_tag);
        PRINT_ERR("Please check your section names");
        PRINT_ERR(" in the config file.\n");
        PRINT_ERR("Also make sure to use config file version 2.\n");
        return CPA_STATUS_RESOURCE;
    }

    if (m_instance_num >= MAX_INSTANCES)
    {
        m_instance_num = MAX_INSTANCES;
    }

    m_instance_buff_level2_pos = m_instance_num - m_instance_num /4;  //start from dev2
    m_instance_buff_level3_pos = m_instance_num - m_instance_num /8; //start from dev3

    PRINT_DBG("Number of instance: %u\n", m_instance_num);

    CpaInstanceHandle * dc_instance_handle_list = (CpaInstanceHandle *)malloc(m_instance_num * sizeof(CpaInstanceHandle));
    if ( NULL == dc_instance_handle_list)
    {
        PRINT_ERR("Allocate memory for instance handle list failed \n");
        return CPA_STATUS_FATAL;
    }

    m_dc_instance_res_pool = (DcInstanceRes *)malloc(m_instance_num * sizeof(DcInstanceRes));
    if ( NULL == m_dc_instance_res_pool)
    {
        free(dc_instance_handle_list);
        PRINT_ERR("Allocate memory for m_dc_instance_res_pool failed \n");
        return CPA_STATUS_FATAL;
    }

    stat = cpaDcGetInstances(m_instance_num, dc_instance_handle_list);
    if (CPA_STATUS_SUCCESS != stat)
    {
        free(m_dc_instance_res_pool);
        free(dc_instance_handle_list);
        PRINT_ERR("Error in cpaDcGetInstances status = %d\n", stat);
        return stat;
    }

#ifdef OUTPUT_INST_INFO
    //print all instance info 
    for (int inst_index=0; inst_index<m_instance_num; inst_index++)
    {
        CpaInstanceInfo2 Info2;
        cpaDcInstanceGetInfo2(dc_instance_handle_list[inst_index], &Info2);
        PRINT_DBG("accelerationServiceType: %d, vendorName:%s, partName: %s, swVersion:%s, instName:%s, instID:%s, physInstId:%d %d %d, nodeAffinity:%d, operState:%d, requiresPhysicallyContiguousMemory:%d, isPolled:%d, isOffloaded:%d \n", \
                 Info2.accelerationServiceType, Info2.vendorName, Info2.partName, Info2.swVersion, Info2.instName, Info2.instID,\
                 Info2.physInstId.packageId, Info2.physInstId.acceleratorId, Info2.physInstId.executionEngineId, Info2.nodeAffinity, Info2.operState, Info2.requiresPhysicallyContiguousMemory, Info2.isPolled, Info2.isOffloaded);
    }
#endif

#ifdef REORG_INSTACE_POOL
    CpaInstanceHandle * dc_instance_handle_list2 = (CpaInstanceHandle *)malloc(m_instance_num * sizeof(CpaInstanceHandle));
    if ( NULL == dc_instance_handle_list2)
    {
        PRINT_ERR("Allocate memory for instance handle list 2 failed \n");
        return CPA_STATUS_FATAL;
    }

    //Reorg the instance list
    uint32_t  qat_dev_count = 8;
    uint32_t  instance_per_dev = m_instance_num/8;
    if (instance_per_dev <1) instance_per_dev = 1;

    for (Cpa16U i=0; i<m_instance_num; i++)
    {
        Cpa16U new_index = i/instance_per_dev + (i%instance_per_dev)*qat_dev_count;
        //PRINT_DBG("Instance index=%u, new index=%u, qat_dev_count=%u, inst per dev=%u \n", i, new_index, qat_dev_count, instance_per_dev);
        dc_instance_handle_list2[new_index] = dc_instance_handle_list[i];
    }
    memcpy(dc_instance_handle_list, dc_instance_handle_list2, m_instance_num * sizeof(CpaInstanceHandle));
    free(dc_instance_handle_list2);
#endif

    int pool_index =0;
    for (int inst_index=0; inst_index<m_instance_num; inst_index++)
    {
        CpaDcInstanceCapabilities cap;
        stat = cpaDcQueryCapabilities(dc_instance_handle_list[inst_index], &cap);
        if (stat != CPA_STATUS_SUCCESS)
        {
            PRINT_DBG("Query DC capabilities failed, inst handle =0x%lx\n", (uint64_t)dc_instance_handle_list[inst_index]);
            continue;
        }

        if (!cap.statelessLZ4Compression ||
            !cap.statelessLZ4Decompression || !cap.checksumXXHash32)
        {
            PRINT_DBG("Error: Unsupported LZ4 functionality,  inst handle =0x%lx\n", (uint64_t)dc_instance_handle_list[inst_index]);
            continue;
        }
#if 0
        CpaInstanceInfo2   instance_info;
        cpaDcInstanceGetInfo2(dc_instance_handle_list2[inst_index], &instance_info);

        if (instance_info.nodeAffinity != 0)
        {
            continue;
        }
#endif
        m_dc_instance_res_pool[pool_index].m_dc_instance_handle   = dc_instance_handle_list[inst_index];
        m_dc_instance_res_pool[pool_index].m_dc_instance_started  = 0;
        m_dc_instance_res_pool[pool_index].m_instance_lock        = 0;
        m_dc_instance_res_pool[pool_index].m_session_handle       = (CpaDcSessionHandle)NULL;

    #ifdef DP_MODE
        for (unsigned int i=0; i<MAX_ASYNC_PROC_COUNT;i++)
        {
            m_dc_instance_res_pool[pool_index].m_op_mem_res.p_op_data[i]   = NULL;
        }
    #else
        m_dc_instance_res_pool[pool_index].m_op_done              = 0;
    #endif
        m_dc_instance_res_pool[pool_index].m_mem_setup            = 0;

        cpaDcInstanceGetInfo2(dc_instance_handle_list[inst_index], &m_dc_instance_res_pool[pool_index].m_instance_info);
        //m_dc_instance_res_pool[pool_index].m_instance_info.requiresPhysicallyContiguousMemory = CPA_FALSE;

        pool_index++;
    }
    free(dc_instance_handle_list);

    PRINT_DBG("QAT Instance pool setup sucessfully \n");

    return stat;
}

void QatCodec::destroyInstancePool(void)
{
    for (Cpa16U i=0; i<m_instance_num; i++)
    {
        if (1 == m_dc_instance_res_pool[i].m_instance_lock)
        {
            PRINT_ERR("Instance (%d) didn't released before\n", i);
        }

        if (m_dc_instance_res_pool[i].m_session_handle != NULL)
        {
            auto instance_handle = m_dc_instance_res_pool[i].m_dc_instance_handle;
            auto session_handle  = m_dc_instance_res_pool[i].m_session_handle;
#ifdef DP_MODE
            auto status = cpaDcDpRemoveSession(instance_handle, session_handle);
#else
            auto status = cpaDcRemoveSession(instance_handle, session_handle);
#endif
            if (status == CPA_STATUS_SUCCESS)
            {
                PRINT_DBG("cpaDcRemoveSession(0x%lx) successfully\n", (uint64_t)session_handle);
            }
            else
            {
                PRINT_DBG("cpaDcRemoveSession(0x%lx) failed with status of %d\n", (uint64_t)session_handle, status);
            }

            /* Free session Context */
            PHYS_CONTIG_FREE(session_handle);
        }

        if (1 == m_dc_instance_res_pool[i].m_mem_setup)
        {
            freeSessMemRes(i);
        }

        if ( 1 == m_dc_instance_res_pool[i].m_dc_instance_started )
        {
            auto instance_handle = m_dc_instance_res_pool[i].m_dc_instance_handle;

            auto status = cpaDcStopInstance(instance_handle);
            if (CPA_STATUS_SUCCESS != status)
            {
                PRINT_ERR("Stop instance(index=%d, addr=0x%lx) failed, status=%d\n", \
                        i, (uint64_t)instance_handle, status);
            }
            else
            {
                PRINT_DBG("Stop instance(index=%d, addr=0x%lx) sucessfully, status=%d\n",\
                        i, (uint64_t)instance_handle, status);
            }
        }
    }

    free(m_dc_instance_res_pool);
    m_instance_num = 0;

    PRINT_DBG("QAT Instance pool destroyed sucessfully \n");
}

/*---------------------------Public compression & decompression functions--------------------------*/
CpaStatus QatCodec::qatInit()
{
    CpaStatus stat = CPA_STATUS_SUCCESS;

    pthread_mutex_lock(&m_lock_mutex);

    if (m_qat_status != QAT_NOT_INIT)
    {
        PRINT_DBG("Qat init duplicated, qat_status=%d \n", m_qat_status);
        pthread_mutex_unlock(&m_lock_mutex);
        stat = (QAT_AVAIL == m_qat_status) ? CPA_STATUS_SUCCESS: CPA_STATUS_FAIL;
        return stat;
    }

    stat = icp_sal_userStart(g_dev_tag);
    if (CPA_STATUS_SUCCESS != stat)
    {
        PRINT_ERR("Failed to start user process %s with status (%d) \n", g_dev_tag,stat);
        pthread_mutex_unlock(&m_lock_mutex);
        return stat;
    }

    //setup QatInstance pool
    stat = setupInstancePool();
    if ( CPA_STATUS_SUCCESS != stat )
    {
        PRINT_DBG("QAT Instance pool setup failed with status = %d \n", stat);
        m_qat_status = QAT_NOT_AVAIL;
        pthread_mutex_unlock(&m_lock_mutex);
        return stat;
    }

    m_qat_status = QAT_AVAIL;
    pthread_mutex_unlock(&m_lock_mutex);

    PRINT_DBG("qatInit successfully\n");

    return stat;
}

void QatCodec::qatClose()
{
    destroyInstancePool();
    icp_sal_userStop();
    m_qat_status = QAT_NOT_INIT;

    PRINT_DBG("qatClose successfully\n");
}
inline Cpa16U QatCodec::getBuffSizeIdxByDataSize(Cpa32U uncompressed_size)
{
    Cpa16U buffer_size_index;

    if (uncompressed_size > SRC_BUFFER_SIZE_LEVEL2)
    {
         buffer_size_index = 2;
    }
    else if (uncompressed_size > SRC_BUFFER_SIZE_LEVEL1)
    {
        buffer_size_index = 1;
    }
    else
    {
        buffer_size_index = 0;
    }

    return buffer_size_index;
}

inline Cpa16U QatCodec::getBuffSizeIdxByInstIdx(Cpa16U & inst_index)
{
    Cpa16U buffer_size_index;

    if (inst_index < m_instance_buff_level2_pos)
    {
         buffer_size_index = 0;
    }
    else if (inst_index < m_instance_buff_level3_pos)
    {
        buffer_size_index = 1;
    }
    else
    {
        buffer_size_index = 2;
    }

    return buffer_size_index;
}


inline Cpa16U QatCodec::getStartIndex(Cpa32U uncompressed_size)
{

    Cpa16U start_search_index;

    if (uncompressed_size <= SRC_BUFFER_SIZE_LEVEL1)
    {
        start_search_index = 0;
    }
    else if (uncompressed_size <= SRC_BUFFER_SIZE_LEVEL2)
    {
        start_search_index = m_instance_buff_level2_pos;
    }
    else if (uncompressed_size <= SRC_BUFFER_SIZE_LEVEL3)
    {
        start_search_index = m_instance_buff_level3_pos;
    }
    else
    {
        start_search_index = 0;
    }

    return start_search_index;
}


CpaStatus QatCodec::setupSession(Cpa32U uncompressed_size)
{
    CpaStatus status = CPA_STATUS_SUCCESS;

    Cpa16U start_index = getStartIndex(uncompressed_size);

    status = getQatInstance(m_dc_inst_index, start_index);

    if (CPA_STATUS_SUCCESS != status)
    {
        PRINT_ERR("Set up session failed because of no QAT instance available \n");
        return status;
    }
    assert(m_dc_inst_index < m_instance_num);
    m_dc_inst_handle = m_dc_instance_res_pool[m_dc_inst_index].m_dc_instance_handle;
    PRINT_DBG("Get dc inst handle = 0x%lx, inst index=%d \n", (uint64_t)m_dc_inst_handle, m_dc_inst_index);

    if (CPA_STATUS_SUCCESS == status)
    {
        if (0 == m_dc_instance_res_pool[m_dc_inst_index].m_dc_instance_started)
        {
            status = cpaDcSetAddressTranslation(m_dc_inst_handle, qatVirtToPhys);
            if (CPA_STATUS_SUCCESS == status)
            {
                /* Start DataCompression component */
                PRINT_DBG("cpaDcStartInstance\n");
                status = cpaDcStartInstance(m_dc_inst_handle, 0, NULL);
                if (CPA_STATUS_SUCCESS == status)
                {
                    //Record instance started or not, qat close use this flag to decide which instances are started and close them.
                    m_dc_instance_res_pool[m_dc_inst_index].m_dc_instance_started = 1;
                #ifdef DP_MODE    
                    status = cpaDcDpRegCbFunc(m_dc_inst_handle, dcCallback);
                #endif
                }
            }
        }
        else
        {
            PRINT_DBG("Dc instance (%d) is already started before.\n", m_dc_inst_index);
        }
    }

    if (CPA_STATUS_SUCCESS == status)
    {
        if ( NULL == m_dc_instance_res_pool[m_dc_inst_index].m_session_handle)
        {
            Cpa32U sess_size = 0;
            Cpa32U ctx_size = 0;
            CpaDcSessionSetupData sd;
            memset((void*)&sd, 0, sizeof(CpaDcSessionSetupData));

            sd.compLevel       = CPA_DC_L6;
            sd.compType        = CPA_DC_LZ4;
            sd.sessDirection   = CPA_DC_DIR_COMBINED;
            sd.sessState       = CPA_DC_STATELESS;
            sd.windowSize      = CPA_DC_WINSIZE_32K;
            sd.minMatch        = CPA_DC_MIN_4_BYTE_MATCH;
            sd.lz4BlockMaxSize = CPA_DC_LZ4_MAX_BLOCK_SIZE_1M;
            sd.checksum        = CPA_DC_XXHASH32;

            /* Determine size of session context to allocate */
            PRINT_DBG("cpaDcGetSessionSize with compType=%d, compLevel=%d, windowSize=%d, minMatch=%d, lz4BlockMaxSize=%d, checksum=%d, sessDirection=%d, sessState=%d\n",\
                       sd.compType, sd.compLevel, sd.windowSize, sd.minMatch, sd.lz4BlockMaxSize, sd.checksum, sd.sessDirection, sd.sessState);
            status = cpaDcGetSessionSize(m_dc_inst_handle, &sd, &sess_size, &ctx_size);
            if (CPA_STATUS_SUCCESS == status)
            {
                /* Allocate session memory */
                Cpa32U  numaNode = m_dc_instance_res_pool[m_dc_inst_index].m_instance_info.nodeAffinity;
                assert(numaNode<2);
                status = PHY_CON_MEM_ALLOC(&m_session_handle, sess_size, numaNode);
            }

            /* Initialize the Stateless session */
            if (CPA_STATUS_SUCCESS == status)
            {
                PRINT_DBG("Call cpaDcInitSession with inst handle=0x%lx, session handle=0x%lx \n", (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle);
#ifdef DP_MODE
                status = cpaDcDpInitSession(m_dc_inst_handle,
                            m_session_handle, /* session memory */
                            &sd);       /* session setup data */  
#else
                status = cpaDcInitSession(
                    m_dc_inst_handle,
                    m_session_handle, /* session memory */
                    &sd,        /* session setup data */
                    NULL, /* pContexBuffer not required for stateless operations */
                    dcCallback); /* callback function */
#endif
            }

            if (CPA_STATUS_SUCCESS == status)
            {
                m_dc_instance_res_pool[m_dc_inst_index].m_session_handle = m_session_handle;
                PRINT_DBG("setupSession finished, dc inst handle = 0x%lx, session handle=0x%lx \n",\
                                                 (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle);
            }
        }
        else
        {
            m_session_handle = m_dc_instance_res_pool[m_dc_inst_index].m_session_handle;
            PRINT_DBG("Session already setup, inst index=%d, dc inst handle = 0x%lx, session handle=0x%lx\n",\
                                    m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle);
        }
    }

    if (CPA_STATUS_SUCCESS == status)
    {
        if (0 == m_dc_instance_res_pool[m_dc_inst_index].m_mem_setup)
        {
            setupSessMemRes();
        }
        else
        {
            PRINT_DBG("Memory already setup ,inst index=%d, dc inst handle=0x%lx, session handle=0x%lx\n",\
                                    m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle);
        }
    }

    return status;
}

#ifdef DP_MODE
inline CpaStatus QatCodec::setupSessMemRes()
{
    /*
    struct OpMemRes
    {
        Cpa16U                 list_count;

        Cpa16U                 req_count;
        CpaPhysBufferList      *p_src_phy_buf_list[MAX_ASYNC_PROC_COUNT];  //Need physical cont in memory and aligned in 8 bytes.
        CpaPhysBufferList      *p_dst_pyh_buf_list[MAX_ASYNC_PROC_COUNT];
        VirBufferList          src_vir_buf_list[MAX_ASYNC_PROC_COUNT];
        VirBufferList          dst_vir_buf_list[MAX_ASYNC_PROC_COUNT];
        unsigned int           m_op_done[MAX_ASYNC_PROC_COUNT];
        CpaDcDpOpData          *p_op_data[MAX_ASYNC_PROC_COUNT];
    };


    typedef struct _CpaPhysBufferList {
        Cpa64U reserved0;
        Cpa32U numBuffers;
        Cpa32U reserved1;
        CpaPhysFlatBuffer flatBuffers[];
    } CpaPhysBufferList;

    typedef struct _CpaPhysFlatBuffer {
        Cpa32U dataLenInBytes;
        Cpa32U reserved;
        CpaPhysicalAddr bufferPhysAddr;
    } CpaPhysFlatBuffer;

    */

    assert(m_dc_inst_index < m_instance_num);
    CpaBoolean  requires_cont_mem  =  m_dc_instance_res_pool[m_dc_inst_index].m_instance_info.requiresPhysicallyContiguousMemory;
    OpMemRes  & op_mem_res          = m_dc_instance_res_pool[m_dc_inst_index].m_op_mem_res;
    Cpa32U      numaNode            = m_dc_instance_res_pool[m_dc_inst_index].m_instance_info.nodeAffinity;
    assert(numaNode<2);
    op_mem_res.req_count            = 0;
    op_mem_res.list_count           = MAX_ASYNC_PROC_COUNT;

    Cpa16U buff_size_index = getBuffSizeIdxByInstIdx(m_dc_inst_index);
    assert(buff_size_index < 3);

    uint32_t    numFlatBuffers  = 1;
    Cpa32U      bufferListMemSize = sizeof(CpaPhysBufferList) + (numFlatBuffers * sizeof(CpaPhysFlatBuffer));

    for (int i=0; i<op_mem_res.list_count; i++)
    {
        //Source CpaPhysBufferList
        PHY_CON_MEM_ALLOC_ALIGNED(&op_mem_res.p_src_phy_buf_list[i], bufferListMemSize, numaNode, 8);
        assert(op_mem_res.p_src_phy_buf_list[i] != NULL);

        op_mem_res.p_src_phy_buf_list[i]->numBuffers = numFlatBuffers;

        if (CPA_TRUE == requires_cont_mem)
        {
            PHY_CON_MEM_ALLOC_ALIGNED(&(op_mem_res.src_vir_buf_list[i].pBuffer), m_max_src_buffer_size[buff_size_index], numaNode, 8);
            assert(op_mem_res.src_vir_buf_list[i].pBuffer != NULL);
            op_mem_res.src_vir_buf_list[i].pBufferNew = NULL;
            //CpaPhysFlatBuffer
            op_mem_res.p_src_phy_buf_list[i]->flatBuffers[0].dataLenInBytes = m_max_src_buffer_size[buff_size_index];  // Will be replaced by compression request data size
            op_mem_res.p_src_phy_buf_list[i]->flatBuffers[0].bufferPhysAddr = qatVirtToPhys(op_mem_res.src_vir_buf_list[i].pBuffer);
        }

        //Dest CpaPhysBufferList
        PHY_CON_MEM_ALLOC_ALIGNED(&op_mem_res.p_dst_phy_buf_list[i], bufferListMemSize, numaNode, 8);
        assert(op_mem_res.p_dst_phy_buf_list[i] != NULL);

        op_mem_res.p_dst_phy_buf_list[i]->numBuffers = numFlatBuffers;

        if (CPA_TRUE == requires_cont_mem)
        {
            PHY_CON_MEM_ALLOC_ALIGNED(&(op_mem_res.dst_vir_buf_list[i].pBuffer), m_max_dst_buffer_size[buff_size_index], numaNode, 8);
            assert(op_mem_res.dst_vir_buf_list[i].pBuffer != NULL);
            op_mem_res.dst_vir_buf_list[i].pBufferNew = NULL;
            //CpaPhysFlatBuffer
            op_mem_res.p_dst_phy_buf_list[i]->flatBuffers[0].dataLenInBytes = m_max_dst_buffer_size[buff_size_index];
            op_mem_res.p_dst_phy_buf_list[i]->flatBuffers[0].bufferPhysAddr = qatVirtToPhys(op_mem_res.dst_vir_buf_list[i].pBuffer);
        }
    }

    m_dc_instance_res_pool[m_dc_inst_index].m_mem_setup = 1;
    PRINT_DBG("Memory setup done,inst index=%d, dc inst handle=0x%lx, session handle=0x%lx, requires_cont_mem=%d, src buffer size=%d, dec buffer size=%d\n",\
                            m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle, requires_cont_mem, m_max_src_buffer_size[buff_size_index], m_max_dst_buffer_size[buff_size_index]);

    return CPA_STATUS_SUCCESS;
}

inline void QatCodec::freeSessMemRes(Cpa16U  dc_inst_index)
{
    assert(dc_inst_index < m_instance_num);

    OpMemRes & op_mem_res         = m_dc_instance_res_pool[dc_inst_index].m_op_mem_res;
    for (int k=0; k<op_mem_res.list_count; k++)
    {
        PHYS_CONTIG_FREE(op_mem_res.src_vir_buf_list[k].pBuffer);
        PHYS_CONTIG_FREE(op_mem_res.p_src_phy_buf_list[k]);

        PHYS_CONTIG_FREE(op_mem_res.dst_vir_buf_list[k].pBuffer);
        PHYS_CONTIG_FREE(op_mem_res.p_dst_phy_buf_list[k]);        
    }

    PRINT_DBG("All memory for instance(index=%d) released sucessfully\n", dc_inst_index);
}

#else
inline CpaStatus QatCodec::setupSessMemRes()
{
     Cpa16U      list_count = 1;
     uint32_t    numFlatBuffers = 1;
     Cpa32U      bufferMetaSize;
     CpaBoolean  requires_cont_mem =  m_dc_instance_res_pool[m_dc_inst_index].m_instance_info.requiresPhysicallyContiguousMemory;
     assert(m_dc_inst_index < m_instance_num);
     Cpa32U      numaNode           = m_dc_instance_res_pool[m_dc_inst_index].m_instance_info.nodeAffinity;
     assert(numaNode<2);

     (void)cpaDcBufferListGetMetaSize(m_dc_inst_handle, numFlatBuffers, &bufferMetaSize);
    
     OpMemRes & src_buffer     = m_dc_instance_res_pool[m_dc_inst_index].m_src_buffers;
     src_buffer.list_count          = list_count;
     OS_MALLOC(&src_buffer.buffer_list, list_count * sizeof(CpaBufferList *));
    
     OpMemRes & dst_buffer     = m_dc_instance_res_pool[m_dc_inst_index].m_dst_buffers;
     dst_buffer.list_count          = list_count;
     OS_MALLOC(&dst_buffer.buffer_list, list_count * sizeof(CpaBufferList *));
    
     Cpa16U buff_size_index = getBuffSizeIdxByInstIdx(m_dc_inst_index);
     assert(buff_size_index < 3);
    
     for (int i=0; i<list_count; i++)
     {
         //CpaBufferList
         OS_MALLOC(&src_buffer.buffer_list[i], sizeof(CpaBufferList));
         assert(src_buffer.buffer_list[i]!=NULL);
         src_buffer.buffer_list[i]->numBuffers = numFlatBuffers;
         OS_MALLOC(&src_buffer.buffer_list[i]->pBuffers, numFlatBuffers * sizeof(CpaFlatBuffer));
         assert(src_buffer.buffer_list[i]->pBuffers!=NULL);
         PHY_CON_MEM_ALLOC(&(src_buffer.buffer_list[i]->pPrivateMetaData), bufferMetaSize, numaNode);
         assert(src_buffer.buffer_list[i]->pPrivateMetaData!=NULL);
    
         if (CPA_TRUE == requires_cont_mem)
         {
             //CpaFlatBuffer
             src_buffer.buffer_list[i]->pBuffers->dataLenInBytes = m_max_src_buffer_size[buff_size_index];  // Will be replaced by compression request data size
             PHY_CON_MEM_ALLOC(&(src_buffer.buffer_list[i]->pBuffers->pData), m_max_src_buffer_size[buff_size_index], numaNode);
             assert(src_buffer.buffer_list[i]->pBuffers->pData!=NULL);
         }
    
         //CpaBufferList
         OS_MALLOC(&dst_buffer.buffer_list[i], sizeof(CpaBufferList));
         assert(dst_buffer.buffer_list[i]!=NULL);
         dst_buffer.buffer_list[i]->numBuffers = numFlatBuffers;
         OS_MALLOC(&dst_buffer.buffer_list[i]->pBuffers, numFlatBuffers * sizeof(CpaFlatBuffer));
         assert(dst_buffer.buffer_list[i]->pBuffers!=NULL);
         PHY_CON_MEM_ALLOC(&(dst_buffer.buffer_list[i]->pPrivateMetaData), bufferMetaSize, numaNode);
         assert(dst_buffer.buffer_list[i]->pPrivateMetaData!=NULL);
    
         if (CPA_TRUE == requires_cont_mem)
         {
             //CpaFlatBuffer
             dst_buffer.buffer_list[i]->pBuffers->dataLenInBytes = m_max_dst_buffer_size[buff_size_index];
             PHY_CON_MEM_ALLOC(&(dst_buffer.buffer_list[i]->pBuffers->pData), m_max_dst_buffer_size[buff_size_index], numaNode);
             assert(dst_buffer.buffer_list[i]->pBuffers->pData!=NULL);
         }
     }
    
     m_dc_instance_res_pool[m_dc_inst_index].m_mem_setup = 1;
     PRINT_DBG("Memory setup done,inst index=%d, dc inst handle=0x%lx, session handle=0x%lx, requires_cont_mem=%d, src buffer size=%d, dec buffer size=%d\n",\
                             m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle, requires_cont_mem, m_max_src_buffer_size[buff_size_index], m_max_dst_buffer_size[buff_size_index]);

    return CPA_STATUS_SUCCESS;
}

inline void QatCodec::freeSessMemRes(Cpa16U  dc_inst_index)
{
    assert(dc_inst_index < m_instance_num);

    OpMemRes src_buffer  = m_dc_instance_res_pool[dc_inst_index].m_src_buffers;
    for (int k=0; k<src_buffer.list_count; k++)
    {
        PHYS_CONTIG_FREE(src_buffer.buffer_list[k]->pBuffers->pData);
        PHYS_CONTIG_FREE(src_buffer.buffer_list[k]->pPrivateMetaData);
        OS_FREE(src_buffer.buffer_list[k]->pBuffers);
        OS_FREE(src_buffer.buffer_list[k]);
    }
    
    OpMemRes dst_buffer  = m_dc_instance_res_pool[dc_inst_index].m_dst_buffers;
    for (int k=0; k<dst_buffer.list_count; k++)
    {
        PHYS_CONTIG_FREE(dst_buffer.buffer_list[k]->pBuffers->pData);
        PHYS_CONTIG_FREE(dst_buffer.buffer_list[k]->pPrivateMetaData);
        OS_FREE(dst_buffer.buffer_list[k]->pBuffers);
        OS_FREE(dst_buffer.buffer_list[k]);
    }
    PRINT_DBG("All memory for instance(index=%d) released sucessfully\n", dc_inst_index);

}

#endif

void QatCodec::teardownSession()
{
    ReleaseQatInstance(m_dc_inst_index);
    m_session_handle = NULL;
    m_dc_inst_handle = NULL;
    PRINT_DBG("teardownSession successfully\n");
}

//Need to limit window size (LZ4_DISTANCE_MAX) to 32KB in LZ4 lib as QAT HW only support 32KB window size.
uint32_t QatCodec::doCompressData(const char * source, uint32_t source_size, char * dest)
{
    uint32_t dest_size = LZ4_compress_default(source, dest, source_size, LZ4_COMPRESSBOUND(source_size));
    PRINT_DBG("doCompressDataSw request with source_size=%u, output size=%u \n", source_size, dest_size);

#ifdef PRINT_COM_INPUT_DATA
    uint32_t print_size = source_size;
    printf("Print source data to be compressed (source_size(%d)):\n", source_size);
    for (uint32_t i=0; i<print_size; i++)
    {
        if((i%32)==0) printf("\n");
        printf("0x%02x,",(unsigned char)source[i]);
    }
    printf("\n");
    threadSleep(10);
#endif
    return dest_size;
}

inline int32_t QatCodec::checkDataOffset(const char * source, uint32_t source_size)
{
    uint32_t literal_length;
    uint16_t match_offset;
    uint32_t match_length;
    uint8_t * source_ptr = (uint8_t *)source;
    uint32_t max_match_offset = 0;

    while(1)
    {
        literal_length = ((*source_ptr)>>4) & 0x0F;  //Read the Literal lenth from the Token.
        match_length   = ((*source_ptr)) & 0x0F;     //Read the Match lenth from the Token.

        if (literal_length == 0x0F)  //read other Literal length if literal length value from token is F.
        {
            do
            {
                source_ptr ++;
                literal_length += *source_ptr;
            }while(*source_ptr == 0xFF);
        }

        source_ptr ++;  //move to point to the Literals

        source_ptr += literal_length; //move over the literals to point the match offset

        if ((source_ptr - (uint8_t *)source) >= source_size)  //All bytes read, return.
        {
            PRINT_DBG("** All data processed, processed length=%lu, total length=%u \n",\
                                            (source_ptr - (uint8_t *)source), source_size);
            return 0;
        }

        match_offset = *(uint16_t *)source_ptr;  //Get the 2 Bytes match offset.
        max_match_offset= (max_match_offset> match_offset )?max_match_offset: match_offset;
        PRINT_DBG("** processed length=%lu, total length=%u, match_offset=%u, max_match_offset=%u \n",\
                         (source_ptr - (uint8_t *)source), source_size, match_offset, max_match_offset);
        if (match_offset > 32767)  //if the match offset >32k, return -1 as QAT only support max 32K window size.
        {
            return -1;
        }

        source_ptr +=1; //move to point the previous byte of the match length or the next Token.

        if (match_length == 0x0F)
        {
            do
            {
                source_ptr ++;  //move to point the optinal match length
                match_length += *source_ptr;
            }while(*source_ptr == 0xFF);
        }

        source_ptr +=1; //move to point to the next Token.
    }

    return 0;
}

#ifdef DP_MODE
int32_t QatCodec::doDecompressData(const char * source, uint32_t source_size, char * dest, uint32_t uncompressed_size)
{
    CpaStatus status = CPA_STATUS_SUCCESS;
    CpaPhysBufferList *pPhyBufferListSrc = NULL;
    CpaPhysBufferList *pPhyBufferListDst = NULL;

    Cpa8U *pSrcBuffer = NULL;
    Cpa8U *pDstBuffer = NULL;

    Cpa32U src_data_size = source_size + 4; //add the compressed block size to the front of data as lz4 block header
    Cpa32U dstBufferSize = uncompressed_size;

    CpaDcDpOpData  *pOpData = NULL;
    CpaDcRqResults *pDcResults = NULL;

    /* The following variables are allocated on the stack because we block
     * until the callback comes back. If a non-blocking approach was to be
     * used then these variables should be dynamically allocated */

#ifdef MEAS_TIME
    uint64_t t_s, t_a,t_b, session_time=0, mem_time=0, polling_time=0, copy_dst_time=0, teardown_time=0;
#endif
    PRINT_DBG("Decompression request with source =0x%lx, source_size=%d, dest =0x%lx, uncompressed_size=%d\n", \
                        (long unsigned int)source, source_size, (long unsigned int)dest, uncompressed_size);
#ifdef CHECK_DATA_OFFSET
    int32_t check_result = checkDataOffset(source, source_size);
    if (check_result == -1)
    {
        PRINT_ERR(" checkDataOffset return code -1\n");
        status = CPA_STATUS_FATAL; //Input data can not be decompressed by QAT , set to CPA_STATUS_FATAL to avoid to try agian.
        return status;
    }

    PRINT_DBG(" checkDataOffset return code 0\n");
#endif
#ifdef MEAS_TIME
    t_s = _rdtsc();
    t_a = t_s;
#endif
    if (NULL == m_session_handle)
    {
        status = setupSession(uncompressed_size);
        if (status != CPA_STATUS_SUCCESS)
        {
            PRINT_ERR("setupSession failed. (status = %d)\n", status);
            return CPA_STATUS_FAIL;
        }
    }
#ifdef MEAS_TIME
    t_b = _rdtsc();
    session_time = t_b - t_a;
#endif

    CpaBoolean      requires_cont_mem =  m_dc_instance_res_pool[m_dc_inst_index].m_instance_info.requiresPhysicallyContiguousMemory;
    VirBufferList & src_vir_buf        =  m_dc_instance_res_pool[m_dc_inst_index].m_op_mem_res.src_vir_buf_list[0];
    VirBufferList & dst_vir_buf        =  m_dc_instance_res_pool[m_dc_inst_index].m_op_mem_res.dst_vir_buf_list[0];
    Cpa32U          numaNode           =  m_dc_instance_res_pool[m_dc_inst_index].m_instance_info.nodeAffinity;
    assert(numaNode<2);

    if (CPA_STATUS_SUCCESS == status)
    {
        Cpa16U        req_index    = 0;
        unsigned long callback_tag = (m_dc_inst_index<<16) | req_index;

        pPhyBufferListSrc = m_dc_instance_res_pool[m_dc_inst_index].m_op_mem_res.p_src_phy_buf_list[0];
        pPhyBufferListDst = m_dc_instance_res_pool[m_dc_inst_index].m_op_mem_res.p_dst_phy_buf_list[0];

        if (requires_cont_mem)
        {
            Cpa16U   cur_buffer_size_index = getBuffSizeIdxByInstIdx(m_dc_inst_index);
            Cpa32U   cur_src_buffer_size = m_max_src_buffer_size[cur_buffer_size_index];
            Cpa32U   cur_dst_buffer_size = m_max_dst_buffer_size[cur_buffer_size_index];

            if (src_data_size <= cur_src_buffer_size)
            {
                pSrcBuffer = src_vir_buf.pBuffer;
            }
            else
            {
                status = PHY_CON_MEM_ALLOC(&pSrcBuffer, src_data_size, numaNode);
                assert(pSrcBuffer!=NULL);
                PRINT_DBG("Reallocate src buffer with size = %d \n", src_data_size);

                if (CPA_STATUS_SUCCESS == status)
                {
                    pPhyBufferListSrc->flatBuffers[0].bufferPhysAddr = qatVirtToPhys(pSrcBuffer);
                    src_vir_buf.pBufferNew  = pSrcBuffer;
                }
            }

            if (dstBufferSize <= cur_dst_buffer_size)
            {
                pDstBuffer = dst_vir_buf.pBuffer;
            }
            else
            {
                status = PHY_CON_MEM_ALLOC(&pDstBuffer, dstBufferSize, numaNode);
                assert(pDstBuffer!=NULL);
                PRINT_DBG("Reallocate dec buffer with size = %d \n", dstBufferSize);

                if (CPA_STATUS_SUCCESS == status)
                {
                    pPhyBufferListDst->flatBuffers[0].bufferPhysAddr = qatVirtToPhys(pDstBuffer);
                    dst_vir_buf.pBufferNew  = pDstBuffer;
                }
            }

            if (CPA_STATUS_SUCCESS == status)
            {
                memcpy(pSrcBuffer, (void *)(&source_size), 4); //add the compressed block size to the front of data as lz4 block header
                memcpy((pSrcBuffer+4), source, source_size);

                pPhyBufferListSrc->flatBuffers[0].dataLenInBytes = src_data_size;
                pPhyBufferListDst->flatBuffers[0].dataLenInBytes = dstBufferSize;
            }
        }
        else
        {
            pPhyBufferListSrc->flatBuffers[0].dataLenInBytes = source_size;
            pPhyBufferListDst->flatBuffers[0].dataLenInBytes = uncompressed_size;

            pPhyBufferListSrc->flatBuffers[0].bufferPhysAddr = (Cpa64U)source;  //For svm implementation, the lz4 block header is included in the source data.
            pPhyBufferListDst->flatBuffers[0].bufferPhysAddr = (Cpa64U)dest;
        }
#ifdef MEAS_TIME
        t_a = _rdtsc();
        mem_time = t_a -t_b;
#endif
        if (CPA_STATUS_SUCCESS == status)
        {
            status = PHY_CON_MEM_ALLOC_ALIGNED(&pOpData, sizeof(CpaDcDpOpData), numaNode, 8);
        }

        if (CPA_STATUS_SUCCESS == status)
        {
            memset(pOpData, 0, sizeof(CpaDcDpOpData));
            pOpData->bufferLenToCompress = src_data_size;
            //pOpData->bufferLenForData = dstBufferSize >= 1024 ? dstBufferSize : 1024;
			pOpData->bufferLenForData = dstBufferSize;
            pOpData->dcInstance = m_dc_inst_handle;
            pOpData->pSessionHandle = m_session_handle;
            pOpData->srcBuffer = qatVirtToPhys(pPhyBufferListSrc);
            pOpData->srcBufferLen = CPA_DP_BUFLIST;
            pOpData->destBuffer = qatVirtToPhys(pPhyBufferListDst);
            pOpData->destBufferLen = CPA_DP_BUFLIST;
            pOpData->sessDirection = CPA_DC_DIR_DECOMPRESS;
            pOpData->thisPhys = qatVirtToPhys(pOpData);
            pOpData->pCallbackTag = (void *)callback_tag;

            uint32_t try_count = 0;
            do
            {
                status = cpaDcDpEnqueueOp(pOpData, CPA_TRUE);

                PRINT_DBG("Call DC DP decompression op with inst index(%u), inst handle(0x%lx), session handle (0x%lx), SrcBufLen=%u, DstBuflen=%u \n", \
                            m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle,\
                            src_data_size, dstBufferSize);

                /*
                * We now wait until the m_dc_instance_res_pool[m_dc_inst_index].m_res be set to 1 by callback.
                */
                uint32_t polling_count = 0;
                if (CPA_STATUS_SUCCESS == status)
                {
                    PRINT_DBG("Waiting callback with instance index (%d), pCallbackTag(0x%lx). \n", m_dc_inst_index, callback_tag);
                    do
                    {
                        status = icp_sal_DcPollDpInstance(m_dc_inst_handle, 1);
                        polling_count++;
                        _mm_pause();
                    }while((status != CPA_STATUS_FAIL)&&(m_dc_instance_res_pool[m_dc_inst_index].m_op_mem_res.p_op_data[0] == NULL));

                    if (status == CPA_STATUS_FAIL)
                    {
                        try_count ++;
                        PRINT_ERR("timeout or interruption in DC DP decompression op with try_count=%u, polling_count=%u, inst index(%u), inst handle(0x%lx), session handle (0x%lx), m_dc_inst_index(%d)\n",\
                                   try_count, polling_count, m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle, m_dc_inst_index);
                    }
                    m_dc_instance_res_pool[m_dc_inst_index].m_op_mem_res.p_op_data[0] = NULL;
                }
                else
                {
                  assert(CPA_STATUS_RETRY == status);
                  try_count ++;
                  PRINT_ERR("DC DP decompression op failed. (status = %d, try_count = %u, inst index(%u), inst handle(0x%lx) session handle (0x%lx))\n", \
                               status, try_count, m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle);
                }
                /*
                 * We now check the results
                 */
                if (CPA_STATUS_SUCCESS == status)
                {
                    pDcResults = &pOpData->results;

                    if (pDcResults->status != CPA_DC_OK)
                    {
                        PRINT_ERR("Results status not as expected decomp (status = %d, try_count = %u, polling_count=%u, inst index(%u), inst handle(0x%lx), session handle (0x%lx) \n",
                                    pDcResults->status, try_count, polling_count, m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle);
                        status = CPA_STATUS_FATAL; //Input data can not be decompressed by QAT , set to CPA_STATUS_FATAL to avoid to try agian.
                    }
                    else
                    {
                    #ifdef MEAS_TIME
                        t_b =  _rdtsc();
                        polling_time = t_b - t_a;
                    #endif
                        uncompressed_size = pDcResults->produced;
                        if (requires_cont_mem)
                        {
                            memcpy(dest, pDstBuffer, pDcResults->produced);
                        }

                    #ifdef MEAS_TIME
                        t_a = _rdtsc();
                        copy_dst_time =  t_a - t_b;
                    #endif
                        PRINT_DBG("Decompression finished with inst index(%u), inst handle(0x%lx), session handle (0x%lx), try_count(%u),  polling_count(%u), Data consumed %d, produced %d, Adler checksum 0x%x\n", \
                                    m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle, try_count, polling_count, pDcResults->consumed, pDcResults->produced, pDcResults->checksum);
                    }
                }
            }while((status == CPA_STATUS_FAIL || status == CPA_STATUS_RETRY) && (try_count < MAX_TRY_COUNT));
        }
    }

#ifdef PRINT_DEC_INPUT_DATA
    if (pDcResults->status != CPA_DC_OK)  //Only print when decompression failed.
    {
        printf("Print source data to be decompressed (len(%d)):\n", source_size);
        for (uint32_t i=0; i<source_size; i++)
        {
            if((i%32)==0) printf("\n");
            printf("0x%02x,",(unsigned char)source[i]);
        }
        printf("\n");
        threadSleep(10);

        printf("Print QAT hw input data to be decompressed (len(%d)):\n",  src_data_size);
        for (uint32_t i=0; i<src_data_size; i++)
        {
            if((i%32)==0) printf("\n");
            printf("0x%02x,",(unsigned char)pSrcBuffer[i]);
        }
        printf("\n");
        threadSleep(10);
    }

#endif
#ifdef PRINT_DEC_OUTPUT_DATA
    if (pDcResults->status == CPA_DC_OK)
    {
        printf("Print decompressed data (len(%d)):\n", uncompressed_size);
        for (uint32_t i=0; i<uncompressed_size; i++)
        {
            if((i%32)==0) printf("\n");
            printf("0x%02x,",(unsigned char)dest[i]);
        }
        printf("\n");
    }
    threadSleep(10);
#endif

    /*
     * At this stage, the callback function has returned, so it is
     * sure that the structures won't be needed any more.  Free the
     * memory!
     */
    if (requires_cont_mem)
    {
        if (src_vir_buf.pBufferNew != NULL)
        {
            pPhyBufferListSrc->flatBuffers[0].bufferPhysAddr = qatVirtToPhys(src_vir_buf.pBuffer);
            PHYS_CONTIG_FREE(src_vir_buf.pBufferNew);
            src_vir_buf.pBufferNew = NULL;
            PRINT_DBG("Release src buffer Reallocated \n");
        }

        if (dst_vir_buf.pBufferNew  != NULL)
        {
            pPhyBufferListDst->flatBuffers[0].bufferPhysAddr = qatVirtToPhys(dst_vir_buf.pBuffer);
            PHYS_CONTIG_FREE(dst_vir_buf.pBufferNew);
            dst_vir_buf.pBufferNew = NULL;
            PRINT_DBG("Release dec buffer Reallocated \n");
        }
    }

    if (pOpData != NULL)
    {
        PHYS_CONTIG_FREE(pOpData);
    }

#ifdef MEAS_TIME
    t_a = _rdtsc();
#endif
    teardownSession();
#ifdef MEAS_TIME
    t_b =  _rdtsc();
    teardown_time = t_b - t_a;
    printf("total_time: %ld, session_time %ld, mem_time %ld, polling_time %ld, copy_dst_time %ld, teardown_time %ld \n",\
                (t_b-t_s), session_time, mem_time, polling_time, copy_dst_time, teardown_time);
#endif
    return status;
}


int32_t QatCodec::decompressReqSubmit(const char * source, uint32_t source_size, char * dest, uint32_t uncompressed_size, CpaBoolean doNow )
{
    CpaStatus status = CPA_STATUS_SUCCESS;
    CpaPhysBufferList *pPhyBufferListSrc = NULL;
    CpaPhysBufferList *pPhyBufferListDst = NULL;

    Cpa8U *pSrcBuffer = NULL;
    Cpa8U *pDstBuffer = NULL;

    Cpa32U inputDataSize = source_size + 4; //add the compressed block size to the front of data as lz4 block header
    Cpa32U dstBufferSize = uncompressed_size;

    CpaDcDpOpData  *pOpData = NULL;
    PRINT_DBG("Decompression request with source =0x%lx, source_size=%d, dest =0x%lx, uncompressed_size=%d\n", \
                        (long unsigned int)source, source_size, (long unsigned int)dest, uncompressed_size);
#ifdef CHECK_DATA_OFFSET
    int32_t check_result = checkDataOffset(source, source_size);
    if (check_result == -1)
    {
        PRINT_ERR(" checkDataOffset return code -1\n");
        status = CPA_STATUS_FATAL; //Input data can not be decompressed by QAT , set to CPA_STATUS_FATAL to avoid to try agian.
        return status;
    }

    PRINT_DBG(" checkDataOffset return code 0\n");
#endif

    if (NULL == m_session_handle)
    {
        status = setupSession(uncompressed_size);
        if (status != CPA_STATUS_SUCCESS)
        {
            PRINT_ERR("setupSession failed. (status = %d)\n", status);
            return CPA_STATUS_FAIL;
        }
    }

    assert(m_dc_inst_index<m_instance_num);

    OpMemRes      & op_mem_res         = m_dc_instance_res_pool[m_dc_inst_index].m_op_mem_res;
    Cpa16U        & req_index          = op_mem_res.req_count;
    assert(req_index < op_mem_res.list_count);//todo: need to re allocate mem after full.
    VirBufferList & src_vir_buf        =  op_mem_res.src_vir_buf_list[req_index];
    VirBufferList & dst_vir_buf        =  op_mem_res.dst_vir_buf_list[req_index];
    CpaBoolean      requires_cont_mem  =  m_dc_instance_res_pool[m_dc_inst_index].m_instance_info.requiresPhysicallyContiguousMemory;
    Cpa32U          numaNode           =  m_dc_instance_res_pool[m_dc_inst_index].m_instance_info.nodeAffinity;
    assert(numaNode<2);

    if (CPA_STATUS_SUCCESS == status)
    {
        unsigned long callback_tag = (m_dc_inst_index<<16) | req_index;

        pPhyBufferListSrc     = op_mem_res.p_src_phy_buf_list[req_index];
        pPhyBufferListDst     = op_mem_res.p_dst_phy_buf_list[req_index];
        op_mem_res.dest[req_index] = dest;

        if (requires_cont_mem)
        {
            Cpa16U   cur_buffer_size_index = getBuffSizeIdxByInstIdx(m_dc_inst_index);
            Cpa32U   cur_src_buffer_size   = m_max_src_buffer_size[cur_buffer_size_index];
            Cpa32U   cur_dst_buffer_size   = m_max_dst_buffer_size[cur_buffer_size_index];

            if (inputDataSize <= cur_src_buffer_size)
            {
                pSrcBuffer = src_vir_buf.pBuffer;
            }
            else
            {
                status = PHY_CON_MEM_ALLOC_ALIGNED(&pSrcBuffer, inputDataSize, numaNode, 8);
                assert( pSrcBuffer!=NULL && CPA_STATUS_SUCCESS == status);
                PRINT_DBG("Reallocate src buffer with size = %d \n", inputDataSize);

                if (CPA_STATUS_SUCCESS == status)
                {
                    pPhyBufferListSrc->flatBuffers[0].bufferPhysAddr = qatVirtToPhys((void *)pSrcBuffer);
                    src_vir_buf.pBufferNew = pSrcBuffer;
                }
            }

            if (dstBufferSize <= cur_dst_buffer_size)
            {
                pDstBuffer = dst_vir_buf.pBuffer;
            }
            else
            {
                status = PHY_CON_MEM_ALLOC_ALIGNED(&pDstBuffer, dstBufferSize, numaNode, 8);
                assert(pDstBuffer!=NULL && CPA_STATUS_SUCCESS == status);
                PRINT_DBG("Reallocate dec buffer with size = %d \n", dstBufferSize);

                if (CPA_STATUS_SUCCESS == status)
                {
                    pPhyBufferListDst->flatBuffers[0].bufferPhysAddr = qatVirtToPhys((void *)pDstBuffer);
                    dst_vir_buf.pBufferNew = pDstBuffer;
                }
            }

            if (CPA_STATUS_SUCCESS == status)
            {
                memcpy(pSrcBuffer, (void *)(&source_size), 4); //add the compressed block size to the front of data as lz4 block header
                memcpy((pSrcBuffer+4), source, source_size);

                pPhyBufferListSrc->flatBuffers[0].dataLenInBytes = inputDataSize;
                pPhyBufferListDst->flatBuffers[0].dataLenInBytes = dstBufferSize;
            }
        }
        else
        {
            pPhyBufferListSrc->flatBuffers[0].dataLenInBytes = source_size;
            pPhyBufferListDst->flatBuffers[0].dataLenInBytes = uncompressed_size;

            pPhyBufferListSrc->flatBuffers[0].bufferPhysAddr = (Cpa64U)source;  //For svm implementation, the lz4 block header is included in the source data.
            pPhyBufferListDst->flatBuffers[0].bufferPhysAddr = (Cpa64U)dest;
        }

        if (CPA_STATUS_SUCCESS == status)
        {
            status = PHY_CON_MEM_ALLOC_ALIGNED(&pOpData, sizeof(CpaDcDpOpData), numaNode, 8);
        }

        if (CPA_STATUS_SUCCESS == status)
        {
            memset(pOpData, 0, sizeof(CpaDcDpOpData));
            pOpData->bufferLenToCompress = inputDataSize;
            //pOpData->bufferLenForData = dstBufferSize >= 1024 ? dstBufferSize : 1024;
			pOpData->bufferLenForData = dstBufferSize;
            pOpData->dcInstance = m_dc_inst_handle;
            pOpData->pSessionHandle = m_session_handle;
            pOpData->srcBuffer = qatVirtToPhys(pPhyBufferListSrc);
            pOpData->srcBufferLen = CPA_DP_BUFLIST;
            pOpData->destBuffer = qatVirtToPhys(pPhyBufferListDst);
            pOpData->destBufferLen = CPA_DP_BUFLIST;
            pOpData->sessDirection = CPA_DC_DIR_DECOMPRESS;
            pOpData->thisPhys = qatVirtToPhys(pOpData);
            pOpData->pCallbackTag = (void *)(callback_tag);  //(m_dc_inst_index<<16) | list_used_cout

            uint32_t try_count = 0;
            do
            {
                status = cpaDcDpEnqueueOp(pOpData, doNow);
                try_count ++;
                PRINT_DBG("Call DC DP decompression op with inst index(%u), inst handle(0x%lx), session handle (0x%lx), pOpData(0x%lx), SrcBufLen=%u, DstBuflen=%u \n", \
                            m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle,\
                            (uint64_t)pOpData , inputDataSize, dstBufferSize);
                if (status != CPA_STATUS_SUCCESS)
                {
                    PRINT_ERR("DC DP decompression op failed. (status = %d, try_count = %u, inst index(%u), inst handle(0x%lx) session handle (0x%lx))\n", \
                               status, try_count, m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle);
                }

            }while ((status == CPA_STATUS_RETRY) && (try_count < MAX_TRY_COUNT));

            if (doNow == CPA_FALSE)
            {
                m_dc_instance_res_pool[m_dc_inst_index].m_need_flush = 1;
            }

            PRINT_DBG("Op memory resource info: inst index(%u), req_index=%u, list count=%u, dest addr=0x%lx, pDstBuffer=0x%lx, doNow=%u \n",\
                      m_dc_inst_index, req_index,  op_mem_res.list_count, (uint64_t)dest, (uint64_t)pDstBuffer, doNow);
            req_index++;
        }
    }

    return status;
}

int32_t QatCodec::decompressReqFlush(void)
{
    CpaStatus   status = CPA_STATUS_SUCCESS;

    if (m_dc_inst_index >= m_instance_num)
    {
        PRINT_DBG("Instance index(%u) is Invalid now, no flush needed \n", m_dc_inst_index);
        return status;
    }

    OpMemRes  * p_op_mem_res = &m_dc_instance_res_pool[m_dc_inst_index].m_op_mem_res;
    uint16_t  req_submited = p_op_mem_res->req_count;

    if (req_submited == 0)
    {
        PRINT_DBG("Instance index is %u, req submisted count is 0, no flush needed \n", m_dc_inst_index);
        return status;
    }

    assert(req_submited <=MAX_ASYNC_PROC_COUNT);
    assert(m_dc_inst_handle != NULL);

    if (m_dc_instance_res_pool[m_dc_inst_index].m_need_flush)
    {
        status = cpaDcDpPerformOpNow(m_dc_inst_handle);
    }

    uint32_t    req_completed = 0;
    uint32_t    polling_count = 0;

    if (CPA_STATUS_SUCCESS == status)
    {
        PRINT_DBG("Waiting callback of instance index (%d) \n", m_dc_inst_index);
        do
        {
            status = icp_sal_DcPollDpInstance(m_dc_inst_handle, 0);
            polling_count++;

            for (uint32_t i=0; i<req_submited; i++)
            {
                if (p_op_mem_res->p_op_data[i] != NULL)
                {
                    PRINT_DBG("Process callback of instance index (%u), req_index (%u), opDataPtr(0x%lx) \n",\
                                                                   m_dc_inst_index, i, (uint64_t)p_op_mem_res->p_op_data[i]);
                    //Process the decompressed data
                    CpaDcDpOpData  * p_op_data           = p_op_mem_res->p_op_data[i];
                    VirBufferList  * p_virSrcBufferList = &p_op_mem_res->src_vir_buf_list[i];
                    VirBufferList  * p_virDstBufferList = &p_op_mem_res->dst_vir_buf_list[i];
                    CpaDcRqResults * pDcResults          = &p_op_data->results;

                    if (pDcResults->status != CPA_DC_OK)
                    {
                        PRINT_ERR("Results status is not as expected (status = %d, polling_count=%u, inst index(%u), inst handle(0x%lx), session handle (0x%lx), pOpData(0x%lx)\n",\
                                   pDcResults->status, polling_count, m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle, (uint64_t)(p_op_data));
                        status = CPA_STATUS_FATAL;
                    }
                    else
                    {
                        CpaBoolean   requires_cont_mem =  m_dc_instance_res_pool[m_dc_inst_index].m_instance_info.requiresPhysicallyContiguousMemory;
                        Cpa8U      * pDstBuffer         = (p_virDstBufferList->pBufferNew != NULL) ? p_virDstBufferList->pBufferNew: p_virDstBufferList->pBuffer;
                        char       * dest                = p_op_mem_res->dest[i];

                        if (requires_cont_mem)
                        {
                            memcpy(dest, pDstBuffer, pDcResults->produced);
                            PRINT_DBG(" Copy to dest, instance index (%u), req_index (%u), opDataPtr(0x%lx), dest(0x%lx), pDstBuffer(0x%lx) \n",\
                                                       m_dc_inst_index, i, (uint64_t)p_op_data, (uint64_t)dest, (uint64_t)pDstBuffer );
                        }

                        PRINT_DBG("Decompression finished with inst index(%u), inst handle(0x%lx), session handle (0x%lx), pOpData(0x%lx), req index(%u), polling_count(%u), Data consumed %d, produced %d, Adler checksum 0x%x\n", \
                                    m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle, (uint64_t)(p_op_data),i, polling_count, pDcResults->consumed, pDcResults->produced, pDcResults->checksum);
                    }
                    //Process done
                    req_completed ++;
                    PHYS_CONTIG_FREE(p_op_data);
                    p_op_mem_res->p_op_data[i] = NULL;

                    if (p_virSrcBufferList->pBufferNew != NULL)
                    {
                        PHYS_CONTIG_FREE(p_virSrcBufferList->pBufferNew);
                        p_op_mem_res->p_src_phy_buf_list[i]->flatBuffers[0].bufferPhysAddr = qatVirtToPhys(p_virSrcBufferList->pBuffer);
                    }

                    if (p_virDstBufferList->pBufferNew != NULL)
                    {
                        PHYS_CONTIG_FREE(p_virDstBufferList->pBufferNew);
                        p_op_mem_res->p_dst_phy_buf_list[i]->flatBuffers[0].bufferPhysAddr = qatVirtToPhys(p_virDstBufferList->pBuffer);
                    }
                }
            }

            _mm_pause();
        }while(req_completed < req_submited);
    }
    else
    {
        PRINT_ERR("cpaDcDpPerformOpNow failed. (status = %d, inst index(%u), inst handle(0x%lx) session handle (0x%lx))\n", \
                   status, m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle);
    }
    p_op_mem_res->req_count = 0;

    teardownSession();

    return status;
}

#else
int32_t QatCodec::doDecompressData(const char * source, uint32_t source_size, char * dest, uint32_t uncompressed_size)
{
    CpaStatus status = CPA_STATUS_SUCCESS;
    CpaBufferList *pBufferListSrc = NULL;
    CpaBufferList *pBufferListDst = NULL;

    Cpa8U *pSrcBuffer = NULL;
    Cpa8U *pDstBuffer = NULL;

    Cpa32U src_data_size = source_size + 4; //add the compressed block size to the front of data as lz4 block header
    Cpa32U dstBufferSize = uncompressed_size;

    Cpa8U *pTempSrcBuffer = NULL;
    Cpa8U *pTempDstBuffer = NULL;

    /* The following variables are allocated on the stack because we block
     * until the callback comes back. If a non-blocking approach was to be
     * used then these variables should be dynamically allocated */
    CpaDcRqResults dcResults;
    CpaDcOpData opData = {};
    SET_DEFAULT_OP_PARAM(&opData, CPA_DC_FLUSH_FINAL);
#ifdef MEAS_TIME
    uint64_t t_s, t_a,t_b, session_time=0, mem_time=0, polling_time=0, copy_dst_time=0, teardown_time=0;
#endif
    PRINT_DBG("Decompression request with source =0x%lx, source_size=%d, dest =0x%lx, uncompressed_size=%d\n", \
                        (long unsigned int)source, source_size, (long unsigned int)dest, uncompressed_size);
#ifdef CHECK_DATA_OFFSET
    int32_t check_result = checkDataOffset(source, source_size);
    if (check_result == -1)
    {
        PRINT_ERR(" checkDataOffset return code -1\n");
        status = CPA_STATUS_FATAL; //Input data can not be decompressed by QAT , set to CPA_STATUS_FATAL to avoid to try agian.
        return status;
    }

    PRINT_DBG(" checkDataOffset return code 0\n");
#endif
#ifdef MEAS_TIME
    t_s = _rdtsc();
    t_a = t_s;
#endif
    if (NULL == m_session_handle)
    {
        status = setupSession(uncompressed_size);
        if (status != CPA_STATUS_SUCCESS)
        {
            PRINT_ERR("setupSession failed. (status = %d)\n", status);
            return CPA_STATUS_FAIL;
        }
    }
#ifdef MEAS_TIME
    t_b = _rdtsc();
    session_time = t_b - t_a;
#endif
    if (CPA_STATUS_SUCCESS == status)
    {
        pBufferListSrc = m_dc_instance_res_pool[m_dc_inst_index].m_src_buffers.buffer_list[0];
        pBufferListDst = m_dc_instance_res_pool[m_dc_inst_index].m_dst_buffers.buffer_list[0];
        CpaBoolean  requires_cont_mem =  m_dc_instance_res_pool[m_dc_inst_index].m_instance_info.requiresPhysicallyContiguousMemory;
        Cpa32U      numaNode           =  m_dc_instance_res_pool[m_dc_inst_index].m_instance_info.nodeAffinity;
        assert(numaNode<2);

        if (requires_cont_mem)
        {
            Cpa16U   cur_buffer_size_index = getBuffSizeIdxByInstIdx(m_dc_inst_index);
            Cpa32U   cur_src_buffer_size = m_max_src_buffer_size[cur_buffer_size_index];
            Cpa32U   cur_dst_buffer_size = m_max_dst_buffer_size[cur_buffer_size_index];

            if (src_data_size <= cur_src_buffer_size)
            {
                pSrcBuffer = pBufferListSrc->pBuffers->pData;
            }
            else
            {
                status = PHY_CON_MEM_ALLOC(&pSrcBuffer, src_data_size, numaNode);
                assert(pSrcBuffer!=NULL);
                PRINT_DBG("Reallocate src buffer with size = %d \n", src_data_size);

                if (CPA_STATUS_SUCCESS == status)
                {
                    pTempSrcBuffer = pBufferListSrc->pBuffers->pData;
                    pBufferListSrc->pBuffers->pData = pSrcBuffer;
                }
            }

            if (dstBufferSize <= cur_dst_buffer_size)
            {
                pDstBuffer = pBufferListDst->pBuffers->pData;
            }
            else
            {
                status = PHY_CON_MEM_ALLOC(&pDstBuffer, dstBufferSize, numaNode);
                assert(pDstBuffer!=NULL);
                PRINT_DBG("Reallocate dec buffer with size = %d \n", dstBufferSize);

                if (CPA_STATUS_SUCCESS == status)
                {
                    pTempDstBuffer = pBufferListDst->pBuffers->pData;
                    pBufferListDst->pBuffers->pData = pDstBuffer;
                }
            }

            if (CPA_STATUS_SUCCESS == status)
            {
                memcpy(pSrcBuffer, (void *)(&source_size), 4); //add the compressed block size to the front of data as lz4 block header
                memcpy((pSrcBuffer+4), source, source_size);

                pBufferListSrc->pBuffers->dataLenInBytes = src_data_size;
                pBufferListDst->pBuffers->dataLenInBytes = dstBufferSize;
            }
        }
        else
        {
            pBufferListSrc->pBuffers->pData = (Cpa8U *)source;  //For svm implementation, the lz4 block header is included in the source data.
            pBufferListDst->pBuffers->pData = (Cpa8U *)dest;
            pBufferListSrc->pBuffers->dataLenInBytes = source_size;
            pBufferListDst->pBuffers->dataLenInBytes = uncompressed_size;
        }
#ifdef MEAS_TIME
        t_a = _rdtsc();
        mem_time = t_a -t_b;
#endif
        if (CPA_STATUS_SUCCESS == status)
        {
            uint32_t try_count = 0;
            do
            {
                status = cpaDcDecompressData2(
                    m_dc_inst_handle,
                    m_session_handle,
                    pBufferListSrc,  /* source buffer list */
                    pBufferListDst, /* destination buffer list */
                    &opData,
                    &dcResults, /* results structure */
                    (void *)&m_dc_inst_index); /* data sent as is to the callback function*/

                PRINT_DBG("Call cpaDcDecompressData2 with inst index(%u), inst handle(0x%lx), session handle (0x%lx), SrcBufLen=%u, DstBuflen=%u \n", \
                            m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle,\
                            pBufferListSrc->pBuffers->dataLenInBytes, pBufferListDst->pBuffers->dataLenInBytes);

                /*
                * We now wait until the m_dc_instance_res_pool[m_dc_inst_index].m_res be set to 1 by callback.
                */
                uint32_t polling_count = 0;
                if (CPA_STATUS_SUCCESS == status)
                {
                    PRINT_DBG("Waiting callback with instance index (%d), pCallbackTag(0x%lx). \n", m_dc_inst_index, (uint64_t)&m_dc_inst_index);
                    do
                    {
                        status = icp_sal_DcPollInstance(m_dc_inst_handle, 0);
                        polling_count++;
                        _mm_pause();
                    }while((status != CPA_STATUS_FAIL)&&(m_dc_instance_res_pool[m_dc_inst_index].m_op_done != 1));

                    if (status == CPA_STATUS_FAIL)
                    {
                        try_count ++;
                        PRINT_ERR("timeout or interruption in cpaDcDecompressData2 with try_count=%u, polling_count=%u, inst index(%u), inst handle(0x%lx), session handle (0x%lx), m_dc_inst_index(%d)\n",\
                                   try_count, polling_count, m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle, m_dc_inst_index);
                    }
                    m_dc_instance_res_pool[m_dc_inst_index].m_op_done = 0; //reset to 0 for the next copression job.
                }
                else
                {
                  assert(CPA_STATUS_RETRY == status);
                  try_count ++;
                  PRINT_ERR("cpaDcDecompressData2 failed. (status = %d, try_count = %u, inst index(%u), inst handle(0x%lx) session handle (0x%lx))\n", \
                               status, try_count, m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle);
                }
                /*
                 * We now check the results
                 */
                if (CPA_STATUS_SUCCESS == status)
                {
                    if (dcResults.status != CPA_DC_OK)
                    {
                        PRINT_ERR("Results status not as expected decomp (status = %d, try_count = %u, polling_count=%u, inst index(%u), inst handle(0x%lx), session handle (0x%lx) \n",
                                    dcResults.status, try_count, polling_count, m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle);
                        status = CPA_STATUS_FATAL; //Input data can not be decompressed by QAT , set to CPA_STATUS_FATAL to avoid to try agian.
                    }
                    else
                    {
                    #ifdef MEAS_TIME
                        t_b =  _rdtsc();
                        polling_time = t_b - t_a;
                    #endif
                        uncompressed_size = dcResults.produced;
                        if (requires_cont_mem)
                        {
                            memcpy(dest, pDstBuffer, dcResults.produced);
                        }

                    #ifdef MEAS_TIME
                        t_a = _rdtsc();
                        copy_dst_time =  t_a - t_b;
                    #endif
                        PRINT_DBG("Decompression finished with inst index(%u), inst handle(0x%lx), session handle (0x%lx), try_count(%u),  polling_count(%u), Data consumed %d, produced %d, Adler checksum 0x%x\n", \
                                    m_dc_inst_index, (uint64_t)m_dc_inst_handle, (uint64_t)m_session_handle, try_count, polling_count, dcResults.consumed, dcResults.produced, dcResults.checksum);
                    }
                }
            }while((status == CPA_STATUS_FAIL || status == CPA_STATUS_RETRY) && (try_count < MAX_TRY_COUNT));
        }
    }

#ifdef PRINT_DEC_INPUT_DATA
    if (dcResults.status != CPA_DC_OK)  //Only print when decompression failed.
    {
        printf("Print source data to be decompressed (len(%d)):\n", source_size);
        for (uint32_t i=0; i<source_size; i++)
        {
            if((i%32)==0) printf("\n");
            printf("0x%02x,",(unsigned char)source[i]);
        }
        printf("\n");
        threadSleep(10);

        printf("Print QAT hw input data to be decompressed (len(%d)):\n",  src_data_size);
        for (uint32_t i=0; i<src_data_size; i++)
        {
            if((i%32)==0) printf("\n");
            printf("0x%02x,",(unsigned char)pSrcBuffer[i]);
        }
        printf("\n");
        threadSleep(10);
    }

#endif
#ifdef PRINT_DEC_OUTPUT_DATA
    if (dcResults.status == CPA_DC_OK)
    {
        printf("Print decompressed data (len(%d)):\n", uncompressed_size);
        for (uint32_t i=0; i<uncompressed_size; i++)
        {
            if((i%32)==0) printf("\n");
            printf("0x%02x,",(unsigned char)dest[i]);
        }
        printf("\n");
    }
    threadSleep(10);
#endif

    /*
     * At this stage, the callback function has returned, so it is
     * sure that the structures won't be needed any more.  Free the
     * memory!
     */
    if (pTempSrcBuffer != NULL)
    {
        pBufferListSrc->pBuffers->pData = pTempSrcBuffer;
        PHYS_CONTIG_FREE(pSrcBuffer);
        PRINT_DBG("Release src buffer Reallocated \n");
    }

    if (pTempDstBuffer != NULL)
    {
        pBufferListDst->pBuffers->pData = pTempDstBuffer;
        PHYS_CONTIG_FREE(pDstBuffer);
        PRINT_DBG("Release dec buffer Reallocated \n");
    }
#ifdef MEAS_TIME
    t_a = _rdtsc();
#endif
    teardownSession();
#ifdef MEAS_TIME
    t_b =  _rdtsc();
    teardown_time = t_b - t_a;
    printf("total_time: %ld, session_time %ld, mem_time %ld, polling_time %ld, copy_dst_time %ld, teardown_time %ld \n",\
                (t_b-t_s), session_time, mem_time, polling_time, copy_dst_time, teardown_time);
#endif
    return status;
}
#endif


void QatCodec::queryInstanceStatics(void)
{
    CpaStatus status = CPA_STATUS_SUCCESS;
    CpaDcStats dcStats = {0};

    /*
     * We can now query the statistics on the instance.
     *
     * Note that some implementations may also make the stats
     * available through other mechanisms, e.g. in the /proc
     * virtual filesystem.
     */
    status = cpaDcGetStats(m_dc_inst_handle, &dcStats);

    if (CPA_STATUS_SUCCESS != status)
    {
        PRINT_ERR("cpaDcGetStats failed, status = %d\n", status);
    }
    else
    {
        PRINT_DBG("Number of compression operations completed: %llu\n",
                  (unsigned long long)dcStats.numCompCompleted);
        PRINT_DBG("Number of decompression operations completed: %llu\n",
                  (unsigned long long)dcStats.numDecompCompleted);
    }
}

uint32_t QatCodec::getMaxCompressedDataSize(uint32_t uncompressed_size)
{
  CpaStatus stat = CPA_STATUS_SUCCESS;
  uint32_t maxCompressedDataSize;
  PRINT_DBG("dc inst handle = 0x%lx\n", (uint64_t)m_dc_inst_handle);

  stat = cpaDcLZ4CompressBound(m_dc_inst_handle, uncompressed_size, &maxCompressedDataSize);
  if (CPA_STATUS_SUCCESS != stat)
  {
      maxCompressedDataSize = uncompressed_size;
  }

  return maxCompressedDataSize;
}

QatCodec * getQatCodecInstance()
{
    PRINT_DBG("Call getQatCodecInstance\n");
    QatCodec * qatCodecPtr = new QatCodec;
    return qatCodecPtr;
}

