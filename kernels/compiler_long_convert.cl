#pragma OPENCL EXTENSION cl_khr_fp64 : enable
kernel void compiler_long_convert(global char *src1, global short *src2, global int *src3, global long *dst1, global long *dst2, global long *dst3) {
  int i = get_global_id(0);
  dst1[i] = src1[i];
  dst2[i] = src2[i];
  dst3[i] = src3[i];
}