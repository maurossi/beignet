#include "utests/utest_helper.hpp"
#include <sys/time.h>

#define BENCH_COPY_BUFFER(T, K, M) \
double benchmark_copy_buffer_ ##T(void) \
{ \
  struct timeval start,stop; \
 \
  const size_t w = 1920; \
  const size_t h = 1080; \
  const size_t sz = 4 * w * h; \
 \
  OCL_CREATE_BUFFER(buf[0], 0, sz * sizeof(M), NULL); \
  OCL_CREATE_BUFFER(buf[1], 0, sz * sizeof(M), NULL); \
 \
  OCL_CREATE_KERNEL_FROM_FILE("bench_copy_buffer",K); \
 \
  OCL_MAP_BUFFER(0); \
  for (size_t i = 0; i < sz; i ++) { \
    ((M *)(buf_data[0]))[i] = rand(); \
  } \
  OCL_UNMAP_BUFFER(0); \
 \
  OCL_SET_ARG(0, sizeof(cl_mem), &buf[0]); \
  OCL_SET_ARG(1, sizeof(cl_mem), &buf[1]); \
 \
  globals[0] = w; \
  globals[1] = h; \
  locals[0] = 16; \
  locals[1] = 4; \
 \
  gettimeofday(&start,0); \
  for (size_t i=0; i<100; i++) { \
    OCL_NDRANGE(2); \
  } \
  OCL_FINISH(); \
 \
  OCL_MAP_BUFFER(1); \
  OCL_UNMAP_BUFFER(1); \
  gettimeofday(&stop,0); \
 \
  clReleaseMemObject(buf[0]); \
  free(buf_data[0]); \
  buf_data[0] = NULL; \
 \
  double elapsed = time_subtract(&stop, &start, 0); \
 \
  return BANDWIDTH(sz * sizeof(M) * 2 * 100, elapsed); \
} \
 \
MAKE_BENCHMARK_FROM_FUNCTION_KEEP_PROGRAM(benchmark_copy_buffer_ ##T,true);

BENCH_COPY_BUFFER(uchar,"bench_copy_buffer_uchar",unsigned char)
BENCH_COPY_BUFFER(ushort,"bench_copy_buffer_ushort",unsigned short)
BENCH_COPY_BUFFER(uint,"bench_copy_buffer_uint",unsigned int)
