//#define DEBUG

#ifndef DEBUG
#undef dev_info
#undef pr_info

#define dev_info(...) dev_dbg(__VA_ARGS__)
#define pr_info(...) pr_debug(__VA_ARGS__)
#endif
