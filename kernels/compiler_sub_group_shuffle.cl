__kernel void compiler_sub_group_shuffle(global int *dst, int c)
{
  int i = get_global_id(0);
  if (i == 0)
    dst[0] = get_max_sub_group_size();
  dst++;

  int from = i;
  int j = get_max_sub_group_size() - get_sub_group_local_id() - 1;
  int o0 = get_sub_group_local_id();
  int o1 = intel_sub_group_shuffle(from, c);
  int o2 = intel_sub_group_shuffle(from, 5);
  int o3 = intel_sub_group_shuffle(from, j);
  dst[i*4] = o0;
  dst[i*4+1] = o1;
  dst[i*4+2] = o2;
  dst[i*4+3] = o3;
}
