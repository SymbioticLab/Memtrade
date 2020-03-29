#ifndef _UTILS_H
#define _UTILS_H

#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>

#define ASSERT(x)    assert((x))
#define ASSERTZ(x)   ASSERT(!(x))

#define LAT_SIZE     10000

//#define DEBUG

#define COORDINATOR_IP "192.168.0.1"
#define COORDINATOR_PORT 9400
#define RDMA_PRODUCER_IP "192.168.0.2"
#define RDMA_PRODUCER_PORT 9402
#define RDMA_PRODUCER_NSLABS     524288 // 819200 //1219200
#define RDMA_PRODUCER_SLAB_SIZE  131072 //(1ul << CLUSTERSHIFT) // 131072

#define PIN_EVICTION_HANDLER_CORE    8
#define PIN_PRODUCER_CORE              7
#define PIN_PRODUCER_POLLER_CORE       6
#define PIN_COORDINATOR_CORE         4
#define PIN_CC_POLLER_CORE   3

/********************************************
************************************************/

#define TSC_BEGIN(hi, lo)    \
     do { \
       __asm__ volatile ("cpuid\n\t"   \
      "rdtsc\n\t"   \
      "mov %%edx, %0\n\t"  \
      "mov %%eax, %1\n\t"  \
      : "=r" ((hi)), "=r" ((lo))  \
      :: "%rax", "%rbx", "%rcx", "%rdx", "memory");   \
    } while (0)

#define TSC_END(hi, lo)     \
    do { \
      __asm__ volatile ("rdtscp\n\t"  \
      "mov %%edx, %0\n\t"  \
      "mov %%eax, %1\n\t"  \
      "cpuid\n\t" \
      : "=r" ((hi)), "=r" ((lo)) \
      :: "%rax", "%rbx", "%rcx", "%rdx", "memory"); \
    } while (0)

#define TSC_ASSIGN(x, hi, lo)  \
  do { \
    (x) = (((unsigned long long)(hi) << 32) | (lo) ); \
  } while (0)
   
#define TSC_PRINT(start, end) \
  do { \
    printf("============== READ: start %llu end %llu final %llu, (%llu)\n", \
    (start), (end), (end)-(start), ((end)-(start))/2400); \
  } while (0)

#define TSC_COMPUTE(lat, start, end) \
  do { \
    unsigned long long x = ((end) - (start)) / 2400; \
    ASSERT(x >= 0); \
    /*ASSERT(x < LAT_SIZE); */\
    if (x >= LAT_SIZE) { \
      /*printf("%s:high latency! %llu\n", #lat, x);*/ \
      x = LAT_SIZE - 1; \
    } \
    lat[x]++; \
  } while (0)

#define LAT_BEGIN()      \
      { \
        unsigned int _lo0_, _lo1_, _hi0_, _hi1_; \
        unsigned long long _start_, _end_;  \
        TSC_BEGIN(_hi0_, _lo0_);   

#define LAT_END(lat)        \
        TSC_END(_hi1_, _lo1_); \
        TSC_ASSIGN(_start_, _hi0_, _lo0_); \
        TSC_ASSIGN(_end_, _hi1_, _lo1_); \
        TSC_COMPUTE((lat), _start_, _end_); \
       }

#define LAT_PRINT(lat)      \
      do { \
        printf("\n\n=== %s %p ===\n", #lat, lat); \
        for (int i = LAT_SIZE - 1; i >= 0; --i) { \
          if ((lat)[i] > 0) printf("lat %d num %lu\n", i, lat[i]); \
        } \
      } while (0)

static inline unsigned long long start_timer() {
  unsigned int _lo0_, _hi0_;
  unsigned long long _start_;
  TSC_BEGIN(_hi0_, _lo0_);   
  TSC_ASSIGN(_start_, _hi0_, _lo0_);
  return _start_;
}

static inline unsigned long long stop_timer() {
  unsigned int _lo1_, _hi1_;
  unsigned long long _end_;
  TSC_END(_hi1_, _lo1_); \
  TSC_ASSIGN(_end_, _hi1_, _lo1_); \
  return _end_;
}

static inline void compute_timer(unsigned long *lat, unsigned long long start, unsigned long long end) {
  //printf("timer %llu %llu\n", start, end);
  TSC_COMPUTE(lat, start, end);
}

#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define KBLU  "\x1B[34m"
#define KMAG  "\x1B[35m"
#define KCYN  "\x1B[36m"
#define KWHT  "\x1B[37m"
#define RESET "\x1B[0m"

#define pr_error(eno, func) \
    do { errno = eno; perror(KRED func); printf(RESET);} while (0)


void dump_stack(void);

#define PAGE_SHIFT	(12)
#define PAGE_SIZE 	(1ull << PAGE_SHIFT)
#define PAGE_MASK	(~(PAGE_SIZE - 1))
#define CLUSTER_SHIFT (17)
#define CLUSTER_SIZE (1ull << CLUSTER_SHIFT)
#define CLUSTER_MASK (~((CLUSTER_SIZE) - 1))

#ifdef DEBUG
#define pr_debug(fmt, ...) 					\
	do {							\
		fprintf(stderr, "%s : %d : " fmt "\n", 		\
			__func__, __LINE__, ##__VA_ARGS__ ); 	\
	} while (0)
#else
#define pr_debug(fmt, ...)					
#endif

#define pr_info(fmt, ...)						\
	do {								\
		printf(KGRN "++ " fmt "\n" RESET, ##__VA_ARGS__);	\
        printf(RESET); \
	} while (0)

#define pr_debug_err(fmt, ...)						\
	do {								\
		pr_debug(KRED fmt " : %s", ##__VA_ARGS__, strerror(errno)); 	\
        fprintf(stderr, RESET);    \
	} while (0)

#define pr_err(fmt, ...)						\
	do {								\
		printf(KRED "++ " fmt "\n" RESET, ##__VA_ARGS__);	\
        printf(RESET); \
	} while (0)

#define pr_err_syscall(fmt, ...)					\
	do {								\
		pr_debug_err(fmt, ##__VA_ARGS__);			\
		pr_err(fmt, ##__VA_ARGS__);				\
	} while (0)

#define pr_warn(fmt, ...)						\
	do {								\
		printf(KYEL "++ " fmt "\n" RESET, ##__VA_ARGS__);	\
        printf(RESET); \
	} while (0)


#define BUG(c)								\
	do {								\
		__builtin_unreachable();				\
		pr_err("FATAL BUG on %s line %d", __func__, __LINE__);	\
		dump_stack();						\
		abort();						\
	} while (0)

#define BUG_ON(c)							\
	do {								\
		if (c)							\
			BUG(0);						\
	} while (0)	

static inline unsigned long align_up(unsigned long v, unsigned long a) {
	return (v + a - 1) & ~(a - 1);
}

static inline unsigned int uint_min(unsigned int a, unsigned int b) {
	return (a < b) ? a : b;
}

#endif
