#define __GNU_SOURCE

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#define __USE_GNU
#include <sched.h>
#include <pthread.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/wait.h> /* wait for children process termination */

#include <unistd.h>

#include <ck_ring.h>

/* Note: define the number of the cores here. The program measures
 * core-to-core communication overhead with pipe(IPI) when the
 * USE_PIPE is defined. When not defined, measure shared memory
 * overhead. For measuring in Marss, comment out the RDTSCLL as it may
 * cause weird problem. */

#define MAXCPU 2

#define USE_PIPE

#define RDTSCLL

#ifdef RDTSCLL
#define rdtscll(val) __asm__ __volatile__("rdtsc" : "=A" (val))
#else
#define rdtscll(val) (val) = 0;
#endif

#define MEG (1<<20)
#define CACHE_LINE 64

int pipeA[2], pipeB[2];

#define MEAS_ITER (1<<20)
struct rendezvous {
	volatile int r;
	volatile unsigned long long cost;
} __attribute__((aligned(CACHE_LINE))) mcsync;
volatile unsigned long bounce __attribute__((aligned(CACHE_LINE)));

#define RB_SZ 1024
struct ck_ring rbping, rbpong;
int *bufferping[RB_SZ], *bufferpong[RB_SZ];

void init(void)
{
	char cmd[128];
        sprintf(cmd, "renice -n -20 %d", getpid());
        system(cmd);
#ifdef USE_PIPE
	if (pipe(pipeA)) exit(-1);
	if (pipe(pipeB)) exit(-1);
#else
	ck_ring_init(&rbping, bufferping, RB_SZ);
	ck_ring_init(&rbpong, bufferpong, RB_SZ);
#endif
}

#define RB_ITER 1000000

void thd_set_affinity(pthread_t tid, int cpuid)
{
	cpu_set_t s;

	CPU_ZERO(&s);
	CPU_SET(cpuid, &s);
	pthread_setaffinity_np(tid, MAXCPU, &s);
}

void *meas_thd(void *d)
{
	unsigned long long s, e;
	unsigned long iter = 0;
	
	while (mcsync.r != -1) {
		mcsync.r = 0;
		while (mcsync.r == 0) ;
		if (mcsync.r == -1) break;

		rdtscll(s);
		/* let the other thread go first to avoid races */
		while (bounce == MEAS_ITER) ;
		while (bounce > 0) {
			unsigned long t;
			
			t = bounce-1;
			bounce = t; /* write to cache line */
			if (bounce == 0) break;
			while (bounce == t) ;
		}
		bounce = 0;
		rdtscll(e);
		//		assert(s < e);
		mcsync.cost = (e-s)/MEAS_ITER;
	}
	printf("running consumer\n"); fflush(stdout);
	//	rb_test_c();
	system("PCM/Intel/./main");

	return NULL;
}

struct orch_meas_args {
	int ncpus;
	pthread_t ts[2];
};

void *orchestrate_measurements(void *d)
{
	struct orch_meas_args *a = d;
	pthread_t *ts = &a->ts[0];
	int ncpus = a->ncpus;
	int i, j;

	for (i = 0 ; i < ncpus ; i++) {
		for (j = i+1 ; j < ncpus ; j++) {
//			int iter;
again:
			thd_set_affinity(ts[1], j);
			thd_set_affinity(ts[0], i);
			
			bounce = MEAS_ITER;
			while (mcsync.r != 0) ;
			mcsync.r = 1;
			bounce = MEAS_ITER;
			while (bounce > 0) {
				unsigned long t;

				t = bounce-1;
				bounce = t; /* write to cache line */
				if (bounce == 0) break;
				while (bounce == t) ;
			}
			assert(bounce == 0);
			while (mcsync.r != 0) ;

			if (mcsync.cost > 100000) goto again;
			printf("One-way cost is %d\n", mcsync.cost);
			fflush(stdout);
			
			//dist_mat[i][j] = dist_mat[j][i] = mcsync.cost;
		}
	}
	while (mcsync.r != 0) ;
	mcsync.r = -1;

	thd_set_affinity(ts[1], 1);
	thd_set_affinity(ts[0], 0);
	printf("running producer\n"); fflush(stdout);
	system("PCM/Intel/./main");
	//	rb_test_p();

	return NULL;
}

void meas_lat(int ncpus) 
{
	struct orch_meas_args a;

	mcsync.r = 1;
	a.ncpus = ncpus;
	a.ts[0] = a.ts[1] = 0;
	pthread_create(&a.ts[1], 0, meas_thd, (void*)1) < 0 ? exit(-1) : 0 ;
	pthread_create(&a.ts[0], 0, orchestrate_measurements, (void*)&a) < 0 ? exit(-1) : 0 ;

	pthread_join(a.ts[0], NULL);
	pthread_join(a.ts[1], NULL);
}

void measure_cpu_dist(int ncpus)
{
	meas_lat(ncpus);
}

/* int create_thd(struct tinfo *t) */
/* { */
/* 	pthread_t tid; */

/* 	pthread_create(&tid, 0, init, t) < 0 ? exit(-1) : 0 ; */
/* 	t->id = tid; */
/* 	assert(t->id); */

/* 	thd_set_affinity(tid, t->cpu); */

/* 	return 0; */
/* } */

struct timeval t1, t2;

void start_timer(void)
{
	gettimeofday(&t1, NULL);
}

void end_timer(void)
{
	long long secs, usecs;
	gettimeofday(&t2, NULL);
	secs = t2.tv_sec - t1.tv_sec;
	usecs = t2.tv_usec - t1.tv_usec;

	printf("%lld\n", secs*1000000 + usecs);
}

void set_curr_affinity(int cpu)
{
	cpu_set_t s;
	CPU_ZERO(&s);
	CPU_SET(cpu, &s);
	sched_setaffinity(0, 1, &s);
	return;
}

static inline void __p(void)
{
	int ret;
	unsigned long long t, t2;

//	printf("p\n");	
	rdtscll(t);
	ret = write(pipeA[1], &t, 8);
//	printf("p write ret %d\n", ret);
	ret = read(pipeB[0], &t2, 8);
//	printf("p read ret %d\n", ret);
}
unsigned long long meas[RB_ITER];
void producer(void)
{
	int i;
	unsigned long long start, end, sum = 0, avg, dev = 0;


	for (i = 0 ; i < RB_ITER ; i++) {
		rdtscll(start);
		__p();
		rdtscll(end);
		meas[i] = end - start;
		sum += meas[i];
	}
	avg = sum/RB_ITER;
	for (i = 0 ; i < RB_ITER ; i++) {
		unsigned long long diff = (meas[i] > avg) ? 
			meas[i] - avg : 
			avg - meas[i];
		dev += (diff*diff);
	}
	dev /= RB_ITER;
	printf("round trip deviation^2 = %llu\n", dev);

	printf("RPC pipe: Producer: %llu\n", avg);
}

unsigned long long sum = 0, c_meas[RB_ITER];
static inline unsigned long long __c(void)
{
//	printf("c"); fflush(stdout);
	int ret;
	unsigned long long t, t2;
//	printf("c\n");
	ret = read(pipeA[0], &t2, 8);
	rdtscll(t);
	sum += t - t2;
//	printf("c read ret %d\n", ret);
	ret = write(pipeB[1], &t, 8);
//	printf("c write ret %d\n", ret);
	return t - t2;
}
void consumer(void)
{
	int i;
	unsigned long long start, end, c_sum = 0, avg, dev = 0;

	rdtscll(start);
	for (i = 0 ; i < RB_ITER ; i++) {
		c_meas[i] = __c();
		c_sum += c_meas[i];
	}
	rdtscll(end);
	avg = sum/RB_ITER;
	for (i = 0 ; i < RB_ITER ; i++) {
		unsigned long long diff = (c_meas[i] > avg) ? 
			c_meas[i] - avg : 
			avg - c_meas[i];
		dev += (diff*diff);
//		printf("%llu, diff %llu\n", c_meas[i], diff);
	}
	dev /= RB_ITER;

	printf("one way trip deviation^2 = %llu\n", dev);

	printf("one way: %d\n", sum / RB_ITER);
//	printf("RPC pipe: Consumer: %lld\n", avg);
}

void rpc_pipe(int cpu1, int cpu2)
{
	int pid;

	set_curr_affinity(cpu1);

	pid = fork();
	if (pid > 0) {
		producer();
	} else {
		set_curr_affinity(cpu2);
		consumer();
	}

	if (pid > 0) {
		int child_status;
		while (wait(&child_status) > 0) ;
	} else {
		/* printf("Child %d back in cos_loader.\n", getpid()); */
		exit(getpid());
	}

}
int main()
{
	init();

//	measure_cpu_dist(MAXCPU);
	int i,j;
	for (i = 0; i < MAXCPU; i++) {
		for (j = i; j < MAXCPU + i; j++) {
			printf("\ncore %d -> %d\n", i, j % MAXCPU);
			rpc_pipe(i, j%MAXCPU);
		}
	}
//	assign_thd2cpus(NTHDS, NGRP, MAXCPU, CLOSE);

	/* start_timer(); */
	/* for (i = 0 ; i < NTHDS ; i++) create_thd(&t[i]); */
	/* for (i = 0 ; i < NTHDS ; i++) pthread_join(t[i].id, NULL); */
	/* end_timer(); */
	return 0;
}
