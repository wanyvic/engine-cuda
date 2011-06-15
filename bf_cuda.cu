// vim:foldenable:foldmethod=marker:foldmarker=[[,]]
/**
 * @version 0.1.3 (2011)
 * @author Johannes Gilger <heipei@hackvalue.de>
 * 
 * Copyright 2011 Johannes Gilger
 *
 * This file is part of engine-cuda
 *
 * engine-cuda is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License or
 * any later version.
 * 
 * engine-cuda is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with engine-cuda. If not, see <http://www.gnu.org/licenses/>.
 *
 */
#ifndef __DEVICE_EMULATION__

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <openssl/blowfish.h>
#include <openssl/evp.h>
#include <cuda_runtime_api.h>
#include "cuda_common.h"
#include "common.h"

__device__ uint32_t bf_global_schedule[1042];
__shared__ uint32_t bf_schedule[1042];
__constant__ uint64_t d_iv;

__device__ uint64_t *bf_device_data;
uint8_t  *bf_host_data;

float bf_elapsed;
cudaEvent_t bf_start,bf_stop;

#define BF_M  0x3fc
#define BF_0  22
#define BF_1  14
#define BF_2   6
#define BF_3  2
#define BF_ENC(LL,R,S,P) ( \
	LL^=P, \
	LL^= (((*(BF_LONG *)((unsigned char *)&(S[  0])+((R>>BF_0)&BF_M))+ \
		*(BF_LONG *)((unsigned char *)&(S[256])+((R>>BF_1)&BF_M)))^ \
		*(BF_LONG *)((unsigned char *)&(S[512])+((R>>BF_2)&BF_M)))+ \
		*(BF_LONG *)((unsigned char *)&(S[768])+((R<<BF_3)&BF_M))) \
	)

__global__ void BFencKernel(uint64_t *data) {
	register uint32_t l, r;
	register uint64_t block = data[TX];

	bf_schedule[threadIdx.x] = bf_global_schedule[threadIdx.x];
	bf_schedule[threadIdx.x+256] = bf_global_schedule[threadIdx.x+256];
	bf_schedule[threadIdx.x+512] = bf_global_schedule[threadIdx.x+512];
	bf_schedule[threadIdx.x+768] = bf_global_schedule[threadIdx.x+768];

	#if MAX_THREAD == 128
		bf_schedule[threadIdx.x+128] = bf_global_schedule[threadIdx.x+128];
		bf_schedule[threadIdx.x+384] = bf_global_schedule[threadIdx.x+384];
		bf_schedule[threadIdx.x+640] = bf_global_schedule[threadIdx.x+640];
		bf_schedule[threadIdx.x+896] = bf_global_schedule[threadIdx.x+896];
	#endif

	if(threadIdx.x < 18)
		bf_schedule[threadIdx.x+1024] = bf_global_schedule[threadIdx.x+1024];

	__syncthreads();

	register uint32_t *p,*s;

	p=bf_schedule;
	s=bf_schedule+18;

	nl2i(block, l, r);

	l^=p[0];
	BF_ENC(r,l,s,p[ 1]);
	BF_ENC(l,r,s,p[ 2]);
	BF_ENC(r,l,s,p[ 3]);
	BF_ENC(l,r,s,p[ 4]);
	BF_ENC(r,l,s,p[ 5]);
	BF_ENC(l,r,s,p[ 6]);
	BF_ENC(r,l,s,p[ 7]);
	BF_ENC(l,r,s,p[ 8]);
	BF_ENC(r,l,s,p[ 9]);
	BF_ENC(l,r,s,p[10]);
	BF_ENC(r,l,s,p[11]);
	BF_ENC(l,r,s,p[12]);
	BF_ENC(r,l,s,p[13]);
	BF_ENC(l,r,s,p[14]);
	BF_ENC(r,l,s,p[15]);
	BF_ENC(l,r,s,p[16]);
	r^=p[BF_ROUNDS+1];

	block = ((uint64_t)r) << 32 | l;
	flip64(block);
	data[TX] = block;

}

__global__ void BFdecKernel(uint64_t *data) {
	register uint32_t l, r;
	register uint64_t block = data[TX];

	bf_schedule[threadIdx.x] = bf_global_schedule[threadIdx.x];
	bf_schedule[threadIdx.x+256] = bf_global_schedule[threadIdx.x+256];
	bf_schedule[threadIdx.x+512] = bf_global_schedule[threadIdx.x+512];
	bf_schedule[threadIdx.x+768] = bf_global_schedule[threadIdx.x+768];

	#if MAX_THREAD == 128
		bf_schedule[threadIdx.x+128] = bf_global_schedule[threadIdx.x+128];
		bf_schedule[threadIdx.x+384] = bf_global_schedule[threadIdx.x+384];
		bf_schedule[threadIdx.x+640] = bf_global_schedule[threadIdx.x+640];
		bf_schedule[threadIdx.x+896] = bf_global_schedule[threadIdx.x+896];
	#endif

	if(threadIdx.x < 18)
		bf_schedule[threadIdx.x+1024] = bf_global_schedule[threadIdx.x+1024];

	__syncthreads();

	register uint32_t *p,*s;

	p=bf_schedule;
	s=bf_schedule+18;

	nl2i(block, l, r);

	l^=p[BF_ROUNDS+1];
	BF_ENC(r,l,s,p[16]);
	BF_ENC(l,r,s,p[15]);
	BF_ENC(r,l,s,p[14]);
	BF_ENC(l,r,s,p[13]);
	BF_ENC(r,l,s,p[12]);
	BF_ENC(l,r,s,p[11]);
	BF_ENC(r,l,s,p[10]);
	BF_ENC(l,r,s,p[ 9]);
	BF_ENC(r,l,s,p[ 8]);
	BF_ENC(l,r,s,p[ 7]);
	BF_ENC(r,l,s,p[ 6]);
	BF_ENC(l,r,s,p[ 5]);
	BF_ENC(r,l,s,p[ 4]);
	BF_ENC(l,r,s,p[ 3]);
	BF_ENC(r,l,s,p[ 2]);
	BF_ENC(l,r,s,p[ 1]);
	r^=p[0];

	block = ((uint64_t)r) << 32 | l;
	flip64(block);
	data[TX] = block;

	
}

__global__ void BFdecKernel_cbc(uint64_t *data) {
	register uint32_t l, r;
	register uint64_t block = data[TX];

	bf_schedule[threadIdx.x] = bf_global_schedule[threadIdx.x];
	bf_schedule[threadIdx.x+256] = bf_global_schedule[threadIdx.x+256];
	bf_schedule[threadIdx.x+512] = bf_global_schedule[threadIdx.x+512];
	bf_schedule[threadIdx.x+768] = bf_global_schedule[threadIdx.x+768];

	#if MAX_THREAD == 128
		bf_schedule[threadIdx.x+128] = bf_global_schedule[threadIdx.x+128];
		bf_schedule[threadIdx.x+384] = bf_global_schedule[threadIdx.x+384];
		bf_schedule[threadIdx.x+640] = bf_global_schedule[threadIdx.x+640];
		bf_schedule[threadIdx.x+896] = bf_global_schedule[threadIdx.x+896];
	#endif

	if(threadIdx.x < 18)
		bf_schedule[threadIdx.x+1024] = bf_global_schedule[threadIdx.x+1024];

	__syncthreads();

	register uint32_t *p,*s;

	p=bf_schedule;
	s=bf_schedule+18;

	nl2i(block, l, r);

	l^=p[BF_ROUNDS+1];
	BF_ENC(r,l,s,p[16]);
	BF_ENC(l,r,s,p[15]);
	BF_ENC(r,l,s,p[14]);
	BF_ENC(l,r,s,p[13]);
	BF_ENC(r,l,s,p[12]);
	BF_ENC(l,r,s,p[11]);
	BF_ENC(r,l,s,p[10]);
	BF_ENC(l,r,s,p[ 9]);
	BF_ENC(r,l,s,p[ 8]);
	BF_ENC(l,r,s,p[ 7]);
	BF_ENC(r,l,s,p[ 6]);
	BF_ENC(l,r,s,p[ 5]);
	BF_ENC(r,l,s,p[ 4]);
	BF_ENC(l,r,s,p[ 3]);
	BF_ENC(r,l,s,p[ 2]);
	BF_ENC(l,r,s,p[ 1]);
	r^=p[0];

	block = ((uint64_t)r) << 32 | l;
	flip64(block);

	if(blockIdx.x == 0 && threadIdx.x == 0) {
		block ^= d_iv;
	} else {
		block ^= data[TX-1];
	}
	__syncthreads();

	data[TX] = block;

	
}

extern "C" void BF_cuda_crypt(const unsigned char *in, unsigned char *out, size_t nbytes, EVP_CIPHER_CTX *ctx, uint8_t **host_data, uint64_t **device_data) {
	assert(in && out && nbytes);
	int gridSize;

	transferHostToDevice(&in, (uint32_t **)device_data, host_data, &nbytes);

	if ((nbytes%(MAX_THREAD*BF_BLOCK_SIZE))==0) {
		gridSize = nbytes/(MAX_THREAD*BF_BLOCK_SIZE);
	} else {
		gridSize = nbytes/(MAX_THREAD*BF_BLOCK_SIZE)+1;
	}

	if (output_verbosity==OUTPUT_VERBOSE)
		fprintf(stdout,"Starting BF kernel for %zu bytes with (%d, (%d))...\n", nbytes, gridSize, MAX_THREAD);

	CUDA_START_TIME

	if(ctx->encrypt == BF_ENCRYPT && EVP_CIPHER_CTX_mode(ctx) == EVP_CIPH_ECB_MODE) {
		BFencKernel<<<gridSize,MAX_THREAD>>>(*device_data);
	} else if (!ctx->encrypt && EVP_CIPHER_CTX_mode(ctx) == EVP_CIPH_ECB_MODE) {
		BFdecKernel<<<gridSize,MAX_THREAD>>>(*device_data);
	} else if (!ctx->encrypt && EVP_CIPHER_CTX_mode(ctx) == EVP_CIPH_CBC_MODE) {
		BFdecKernel_cbc<<<gridSize,MAX_THREAD>>>(*device_data);
	}

	CUDA_STOP_TIME("BF         ")

	transferDeviceToHost(&out, (uint32_t **)device_data, host_data, host_data, &nbytes);
}

extern "C" void BF_cuda_transfer_key_schedule(BF_KEY *ks) {
	assert(ks);
	cudaError_t cudaerrno;
	size_t ks_size = sizeof(BF_KEY);
	_CUDA(cudaMemcpyToSymbolAsync(bf_global_schedule,ks,ks_size,0,cudaMemcpyHostToDevice));
}

extern "C" void BF_cuda_transfer_iv(const unsigned char *iv) {
	cudaError_t cudaerrno;
	_CUDA(cudaMemcpyToSymbolAsync(d_iv,iv,sizeof(uint64_t),0,cudaMemcpyHostToDevice));
}

#else
#error "ERROR: DEVICE EMULATION is NOT supported."
#endif
