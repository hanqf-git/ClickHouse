#include <cassert>
#include <thread>
#include <stdexcept>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <memory>
#include <vector>
#include <chrono>
#include <iostream>
#include <x86intrin.h>

#include "QatCodec.h"
#include <lz4.h>
#include "LZ4_decompress_faster.h"

namespace A
{
    constexpr const uint32_t SIZE_NUM = 4;
    constexpr const uint32_t source_size[SIZE_NUM] = {64*1024, 128*1024 , 640*1024, 1024*1024};
    constexpr const uint32_t nThreads = 16;//16;
    constexpr const uint32_t nJobs = 2;//16;
    constexpr const uint32_t nLoop = 2; //8;
    typedef std::vector<char> VectorType;
    void test()
    {
        uint32_t num = nThreads*nJobs;
        std::vector<VectorType> allSrc(num), allDst_compressed(num), allDst_decompressed(num);
        std::vector<uint32_t> compressed_size(num, 0);
        std::vector<int32_t> compressed_job_indexes(num, 0);
        std::vector<std::thread> threads(num);
        uint64_t start,end;
        int32_t ret = 0;
        uint32_t total_bytes = 0;

        //std::shared_ptr<QatCodec> qat_codec_ptr = getQatCodecInstance();
        QatCodec * qat_codec_ptr = getQatCodecInstance();

        for (uint32_t t= 0; t < num; ++t)
        {
            threads[t] = std::thread([&](uint32_t i)
                        {
                            allSrc[i].resize(source_size[i%SIZE_NUM], 5);
                            allDst_compressed[i].resize(source_size[i%SIZE_NUM] / 2, 4);
                            allDst_decompressed[i].resize(source_size[i%SIZE_NUM], 7);
                        }, t);
        }
        for (auto & t : threads) t.join();

        for (uint32_t i = 0; i < num; ++i)
        {
            compressed_size[i] = qat_codec_ptr->doCompressData(reinterpret_cast<const char *>(&(allSrc[i][0])),
                                                               source_size[i%SIZE_NUM],
                                                               reinterpret_cast<char *>(&(allDst_compressed[i][0])));
            total_bytes += source_size[i%SIZE_NUM];
        }
        delete qat_codec_ptr;

#if 0
        //compress data
        start = _rdtsc();
        for (uint32_t inter = 0; inter < nLoop; ++inter)
        {
            for (uint32_t i = 0; i < num; ++i)
            {
                compressed_size[i] = qat_codec_ptr->doCompressData(reinterpret_cast<const char *>(&(allSrc[i][0])), source_size, reinterpret_cast<char *>(&(allDst_compressed[i][0])));
            }
        }
        end = _rdtsc();
        printf("Sync compression time: %f cycles per loop per byte. nJobs:%d,nLoop:%d\n", (end-start)/float(num*nLoop*source_size),num,nLoop);

        qat_codec_ptr->doDecompressData(reinterpret_cast<const char *>(&(allDst_compressed[0][0])), compressed_size[0], reinterpret_cast<char *>(&(allDst_decompressed[0][0])), source_size);

        start = _rdtsc();
        for (uint32_t inter = 0; inter < nLoop; ++inter)
        {
            for(uint32_t i = 0; i < num; ++i)
            {
                ret = qat_codec_ptr->doDecompressData(reinterpret_cast<const char *>(&(allDst_compressed[i][0])), compressed_size[i], reinterpret_cast<char *>(&(allDst_decompressed[i][0])), source_size);
                assert(ret ==0);
            }

        }
        end = _rdtsc();
        printf("SYNC decompression time: %f cycles per loop per byte. nLoops:%d, nJobs:%d, compress_size:%d, Decompress_size:%d\n", (end-start)/float(nLoop*num*source_size),nLoop,num, compressed_size[0],source_size);
#endif


#if 1
        start = _rdtsc();
        QatCodec * qat_codec_ptr_t[nThreads];
        for (uint32_t inter = 0; inter < nLoop; ++inter)
        {
            for (uint32_t t = 0; t < nThreads; ++t) threads[t] = std::thread([&](int32_t i) {
                qat_codec_ptr_t[i] = getQatCodecInstance();
                for (uint32_t j = 0; j < nJobs; ++j)
                {
                    ret = qat_codec_ptr_t[i]->doDecompressData(reinterpret_cast<const char *>(&(allDst_compressed[i*nJobs+j][0])),
                                                               compressed_size[i*nJobs+j],
                                                               reinterpret_cast<char *>(&(allDst_decompressed[i*nJobs+j][0])),
                                                               source_size[(i*nJobs+j)%SIZE_NUM]);
                    assert(ret ==0);
                }
                delete qat_codec_ptr_t[i];
            }, t);
            for (auto & t : threads)
                if(t.joinable())
                    t.join();
        }

        end = _rdtsc();

        printf("SYNC_THREAD decompression time: %f cycles per loop per byte. nLoops:%d, nJobs:%d,nThreads:%d compress_size:%d, %d, %d, %d, Decompress_size:%d, %d, %d, %d\n",\
                                        (end-start)/float(nLoop* total_bytes),nLoop,nJobs,nThreads,\
                                        compressed_size[0],compressed_size[1], compressed_size[2],compressed_size[3],\
                                        source_size[0], source_size[1], source_size[2], source_size[3]);
#endif

        // Compare reference functions
        for (uint32_t i = 0; i < num; i++)
        {
            for (uint32_t j = 0; j < source_size[num%SIZE_NUM]; j++)
            {
                if (allSrc[i][j] != allDst_decompressed[i][j])
                {
                    throw std::runtime_error("Content wasn't successfully compressed and decompressed.");
                }
            }
        }

        std::cout << "Content was successfully compressed and decompressed." << std::endl;
        std::cout << "Compressed size: " << compressed_size[0] <<", " << compressed_size[1] <<", "<<compressed_size[2] <<", "<<compressed_size[3] <<", "<< std::endl;

    }
}

//test different data size latency QAT vs LZ4
namespace B_QAT
{
    constexpr const uint32_t SIZE_NUM = 5;
    uint32_t source_size[SIZE_NUM];
    constexpr const uint32_t nThreads = 1;
    constexpr const uint32_t nJobs = 4;
    constexpr const uint32_t nLoop = 4;
    typedef std::vector<char> VectorType;

    void test()
    {
        uint32_t num = nThreads*nJobs;
        std::vector<VectorType> allSrc(num), allDst_compressed(num), allDst_decompressed(num);
        std::vector<uint32_t> compressed_size(num, 0);
        std::vector<int32_t> compressed_job_indexes(num, 0);
        std::vector<std::thread> threads(num);
        uint64_t start,end;
        int32_t ret = 0;

        printf("B_QAT test \n");
        for (uint32_t i=0; i<SIZE_NUM; i++)
        {
            source_size[i] = 64*(1<<i)*1024;
        }

        source_size[0] = 128*1024;
        source_size[1] = 64*1024;
        QatCodec * qat_codec_ptr = getQatCodecInstance();

        for (uint32_t size_i=0; size_i<SIZE_NUM; size_i++)
        {
            uint32_t data_size = source_size[size_i];

            for (uint32_t t= 0; t < num; ++t)
            {
                allSrc[t].resize(data_size, 5);
                allDst_compressed[t].resize(data_size/2, 4);
                allDst_decompressed[t].resize(data_size, 7);
            }

            for (uint32_t k=0; k<2; k++)
            {
                for (uint32_t i = 0; i < num; ++i)
                {
                    compressed_size[i] = qat_codec_ptr->doCompressData(reinterpret_cast<const char *>(&(allSrc[i][0])),
                                                                       data_size,
                                                                       reinterpret_cast<char *>(&(allDst_compressed[i][0])));
                }
            }


            //compress data
            start = _rdtsc();
            for (uint32_t inter = 0; inter < nLoop; ++inter)
            {
                for (uint32_t i = 0; i < num; ++i)
                {
                    compressed_size[i] = qat_codec_ptr->doCompressData(reinterpret_cast<const char *>(&(allSrc[i][0])),
                                                                      data_size,
                                                                      reinterpret_cast<char *>(&(allDst_compressed[i][0])));
                }
            }
            end = _rdtsc();
            printf("Sync compression time: (%f) cycles per loop, (%f) cycles per byte. nJobs:%d, nLoop:%d\n", \
                             (end-start)/float(num*nLoop), (end-start)/float(num*nLoop*data_size), num, nLoop);
            for (uint32_t k=0; k<2; k++)
            {
                for(uint32_t i = 0; i < num; ++i)
                {
                  ret = qat_codec_ptr->doDecompressData(reinterpret_cast<const char *>(&(allDst_compressed[i][0])),
                                                        compressed_size[i],
                                                       reinterpret_cast<char *>(&(allDst_decompressed[i][0])),
                                                       data_size);
                }
            }
            assert(ret == 0);


            //decompress data
            start = _rdtsc();
            for (uint32_t inter = 0; inter < nLoop; ++inter)
            {
                for(uint32_t i = 0; i < num; ++i)
                {
                    ret = qat_codec_ptr->doDecompressData(reinterpret_cast<const char *>(&(allDst_compressed[i][0])),
                                                         compressed_size[i],
                                                         reinterpret_cast<char *>(&(allDst_decompressed[i][0])),
                                                         data_size);
                }
            }
            end = _rdtsc();
            assert(ret == 0);
            printf("SYNC decompression time: %f cycles per loop, (%f) cycles per byte. nLoops:%d, nJobs:%d, compress_size:%d, Decompress_size:%d\n", \
                                (end-start)/float(num*nLoop), (end-start)/float(nLoop*num*data_size), nLoop,num, compressed_size[0], data_size);

#ifdef DP_MODE
            //decompress data
            start = _rdtsc();
            for (uint32_t inter = 0; inter < nLoop; ++inter)
            {
                for(uint32_t i = 0; i < num; )
                {
                    ret = qat_codec_ptr->decompressReqSubmit(reinterpret_cast<const char *>(&(allDst_compressed[i][0])),
                                                         compressed_size[i],
                                                         reinterpret_cast<char *>(&(allDst_decompressed[i][0])),
                                                         data_size, CPA_FALSE);
                   ++i;
                   if ((i%MAX_ASYNC_PROC_COUNT) == 0)
                   {
                       ret = qat_codec_ptr->decompressReqFlush();
                   }
                }
            }
            end = _rdtsc();
            assert(ret == 0);
            printf("SYNC decompression time: %f cycles per loop, (%f) cycles per byte. nLoops:%d, nJobs:%d, compress_size:%d, Decompress_size:%d\n", \
                                (end-start)/float(num*nLoop), (end-start)/float(nLoop*num*data_size), nLoop,num, compressed_size[0], data_size);
#endif
            // Compare reference functions
            for (uint32_t i = 0; i < num; i++)
            {
                for (uint32_t j = 0; j < data_size; j++)
                {
                    if (allSrc[i][j] != allDst_decompressed[i][j])
                    {
                        throw std::runtime_error("Content wasn't successfully compressed and decompressed.");
                    }
                }
            }

            std::cout << "Content was successfully compressed and decompressed." << std::endl;
            std::cout << "Compressed size: " << compressed_size[0] << std::endl;
        }
    }
}

//test different data size latency QAT vs LZ4
namespace B_LZ4
{
    constexpr const uint32_t SIZE_NUM = 5;
    uint32_t source_size[SIZE_NUM];
    constexpr const uint32_t nThreads = 1;
    constexpr const uint32_t nJobs = 16;
    constexpr const uint32_t nLoop = 1;
    typedef std::vector<char> VectorType;

    void test()
    {
        uint32_t num = nThreads*nJobs;
        std::vector<VectorType> allSrc(num), allDst_compressed(num), allDst_decompressed(num);
        std::vector<uint32_t> compressed_size(num, 0);
        std::vector<int32_t> compressed_job_indexes(num, 0);
        std::vector<std::thread> threads(num);
        uint64_t start,end;
        uint32_t total_bytes = 0;
        bool success;

        printf("B_LZ4 test \n");
        for (uint32_t i=0; i<SIZE_NUM; i++)
        {
            source_size[i] = 64*(1<<i)*1024;
        }

        for (uint32_t size_i=0; size_i<SIZE_NUM; size_i++)
        {
            uint32_t data_size = source_size[size_i];

            for (uint32_t t= 0; t < num; ++t)
            {
                allSrc[t].resize(data_size, 5);
                allDst_compressed[t].resize(data_size/2, 4);
                allDst_decompressed[t].resize(data_size, 7);
            }

            for (uint32_t i = 0; i < num; ++i)
            {
                LZ4_compress_default(reinterpret_cast<const char *>(&(allSrc[i][0])),
                                     reinterpret_cast<char *>(&(allDst_compressed[i][0])),
                                     data_size,
                                     LZ4_COMPRESSBOUND(data_size));

                total_bytes += data_size;
            }


            //compress data
            start = _rdtsc();
            for (uint32_t inter = 0; inter < nLoop; ++inter)
            {
                for (uint32_t i = 0; i < num; ++i)
                {
                    compressed_size[i] = LZ4_compress_default(reinterpret_cast<const char *>(&(allSrc[i][0])),
                                         reinterpret_cast<char *>(&(allDst_compressed[i][0])),
                                         data_size,
                                         LZ4_COMPRESSBOUND(data_size));
                }
            }
            end = _rdtsc();
            printf("Sync compression time: (%f) cycles per loop, (%f) cycles per byte. nJobs:%d, nLoop:%d\n", \
                             (end-start)/float(num*nLoop), (end-start)/float(num*nLoop*data_size), num, nLoop);

            for(uint32_t i = 0; i < num; ++i)
            {
                success = LZ4::decompress(reinterpret_cast<const char *>(&(allDst_compressed[i][0])),
                                               reinterpret_cast<char *>(&(allDst_decompressed[i][0])),
                                               compressed_size[i],
                                               data_size);
            }

            assert(success == true);
            //decompress data
            start = _rdtsc();
            for (uint32_t inter = 0; inter < nLoop; ++inter)
            {
                for(uint32_t i = 0; i < num; ++i)
                {
                    success = LZ4::decompress(reinterpret_cast<const char *>(&(allDst_compressed[i][0])),
                                                   reinterpret_cast<char *>(&(allDst_decompressed[i][0])),
                                                   compressed_size[i],
                                                   data_size);
                }
            }
            end = _rdtsc();
            assert(success == true);
            printf("SYNC decompression time: %f cycles per loop, (%f) cycles per byte. nLoops:%d, nJobs:%d, compress_size:%d, Decompress_size:%d\n", \
                                (end-start)/float(num*nLoop), (end-start)/float(nLoop*num*data_size), nLoop,num, compressed_size[0], data_size);


            // Compare reference functions
            for (uint32_t i = 0; i < num; i++)
            {
                for (uint32_t j = 0; j < data_size; j++)
                {
                    if (allSrc[i][j] != allDst_decompressed[i][j])
                    {
                        throw std::runtime_error("Content wasn't successfully compressed and decompressed.");
                    }
                }
            }

            std::cout << "Content was successfully compressed and decompressed." << std::endl;
            std::cout << "Compressed size: " << compressed_size[0] << std::endl;
        }
    }
}


int main(int argc, char *argv[])
{
    //A::testA();
    B_QAT::test();
    B_LZ4::test();
    return 0;
}


