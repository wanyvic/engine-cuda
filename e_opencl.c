// vim:ft=opencl
#include <CL/opencl.h>
#include <sys/time.h>
#include <string.h>

#include "aes_cuda.h"
#include "bf_cuda.h"
#include "cast_cuda.h"
#include "cmll_cuda.h"
#include "des_cuda.h"
#include "idea_cuda.h"

#include "bf_opencl.h"
#include "des_opencl.h"
#include "common.h"
#include "opencl_common.h"

#define DYNAMIC_ENGINE
#define OPENCL_ENGINE_ID	"opencl"
#define OPENCL_ENGINE_NAME	"OpenSSL engine for AES, DES, IDEA, Blowfish, Camellia and CAST5 with OpenCL acceleration"
#define CMD_SO_PATH		ENGINE_CMD_BASE
#define CMD_VERBOSE		(ENGINE_CMD_BASE+1)
#define CMD_QUIET		(ENGINE_CMD_BASE+2)
#define CMD_BUFFER_SIZE		(ENGINE_CMD_BASE+3)

static int opencl_ciphers (ENGINE *e, const EVP_CIPHER **cipher, const int **nids, int nid);
static int opencl_crypt(EVP_CIPHER_CTX *ctx, unsigned char *out_arg, const unsigned char *in_arg, size_t nbytes);
void (*opencl_device_crypt) (const unsigned char *in, unsigned char *out, size_t nbytes, int enc, cl_mem *device_buffer, cl_mem *device_schedule, cl_command_queue queue, cl_kernel device_kernel, cl_context context);

int buffer_size = 0;
int verbose = 0;
int quiet = 0;
int initialized = 0;
char *library_path=NULL;
int maxbytes = 8388608;

static cl_context context;
static cl_command_queue queue;
static cl_mem device_buffer;
static cl_mem device_schedule;
static cl_device_id device;
static cl_int error = 0;
static cl_program device_program;
static cl_kernel device_kernel;
unsigned char *host_data = NULL;
FILE *device_kernels = NULL;
size_t BF_source_length;
char *kernels_file = "/home/jojo/git/engine-cuda/opencl_kernels.cl";

int set_buffer_size(const char *buffer_size_string) {
	buffer_size=atoi(buffer_size_string)*1024;	// The size is in kilobytes
	return 1;
}

int inc_quiet(void) {
	quiet++;
	return 1;
}

int inc_verbose(void) {
	verbose++;
	return 1;
}

int opencl_finish(ENGINE * engine) {
	clReleaseKernel(device_kernel);
	clReleaseProgram(device_program);
	clUnloadCompiler();

	clReleaseCommandQueue(queue);
	clReleaseContext(context);
	clReleaseMemObject(device_buffer);
	clReleaseMemObject(device_schedule);

	return 1;
}

int opencl_init(ENGINE * engine) {
	if (!quiet && verbose) fprintf(stdout, "initializing engine\n");
	int verbosity=OUTPUT_NORMAL;
	if (quiet==1)
		verbosity=OUTPUT_QUIET;
	if(verbose) 
		verbosity=OUTPUT_VERBOSE;

	struct timeval starttime,curtime,difference;
	gettimeofday(&starttime, NULL);

	// Get all OpenCL platform IDs
	cl_platform_id cl_platform;
	CL_WRAPPER(clGetPlatformIDs(1, &cl_platform, NULL));

	// Get a GPU device
	CL_WRAPPER(clGetDeviceIDs(cl_platform, CL_DEVICE_TYPE_GPU, 1, &device, NULL));

	char cBuffer[1024];
	CL_WRAPPER(clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(cBuffer), &cBuffer, NULL));
	printf("CL_DEVICE_NAME:    %s\n", cBuffer);
	CL_WRAPPER(clGetDeviceInfo(device, CL_DRIVER_VERSION, sizeof(cBuffer), &cBuffer, NULL));
	printf("CL_DRIVER_VERSION: %s\n\n", cBuffer);

	CL_ASSIGN(context = clCreateContext(NULL, 1, &device, NULL, NULL, &error));
	CL_ASSIGN(queue = clCreateCommandQueue(context, device, 0, &error));
	CL_ASSIGN(device_buffer = clCreateBuffer(context, CL_MEM_READ_WRITE|CL_MEM_ALLOC_HOST_PTR, maxbytes, NULL, &error));
	CL_ASSIGN(host_data = (unsigned char *)clEnqueueMapBuffer(queue,device_buffer,CL_TRUE,CL_MAP_WRITE|CL_MAP_READ,0,maxbytes,0,NULL,NULL,&error));

	size_t kernels_source_length;
	device_kernels = fopen(kernels_file, "rb");
	fseek(device_kernels, 0, SEEK_END);
	kernels_source_length = ftell(device_kernels);
	fseek(device_kernels, 0, SEEK_SET);

	char *kernels_source = (char *)malloc(kernels_source_length + 1);
	error = fread(kernels_source, kernels_source_length, 1, device_kernels);
	fclose(device_kernels);
	
	if(verbose && !quiet)
		fprintf(stdout, "Opening file %s of %zu lines length\n", kernels_file, kernels_source_length);
	CL_ASSIGN(device_program = clCreateProgramWithSource(context,1,(const char **)&kernels_source,&kernels_source_length,&error));
	free(kernels_source);

	gettimeofday(&starttime, NULL);
	
	cl_int error;
	error = clBuildProgram(device_program, 1, &device, "-w -cl-nv-verbose", NULL, NULL);

	if(verbose && !quiet) {
		char build_info[600001];
		CL_WRAPPER(clGetProgramBuildInfo(device_program,device,CL_PROGRAM_BUILD_LOG,60000,build_info,NULL));
		fprintf(stdout, "Build log: %s\n", build_info);
	}

	CL_ASSIGN(device_kernel = clCreateKernel(device_program, "DESencKernel", &error));

	gettimeofday(&curtime, NULL);
	timeval_subtract(&difference,&curtime,&starttime);

	if(verbose && !quiet)
		fprintf(stdout, "opencl_init %d.%06d usecs\n", (int)difference.tv_sec, (int)difference.tv_usec);

	initialized=1;
	return 1;
}

static const ENGINE_CMD_DEFN opencl_cmd_defns[] = {
	{CMD_SO_PATH, "SO_PATH", "Specifies the path to the 'opencl-engine' shared library", ENGINE_CMD_FLAG_STRING},
	{CMD_VERBOSE, "VERBOSE", "Print additional details", ENGINE_CMD_FLAG_NO_INPUT},
	{CMD_QUIET, "QUIET", "Remove additional details", ENGINE_CMD_FLAG_NO_INPUT},
	{CMD_BUFFER_SIZE, "BUFFER_SIZE", "Specifies the size of the buffer between central memory and GPU memory in kilobytes (default: 8MB)", ENGINE_CMD_FLAG_STRING},
	{0, NULL, NULL, 0}
};

static int opencl_engine_ctrl(ENGINE * e, int cmd, long i, void *p, void (*f) ()) {
	switch (cmd) {
	case CMD_SO_PATH:
		if (p!=NULL && library_path==NULL) {
			library_path=strdup((const char*)p);
			return 1;
		} else return 0;
	case CMD_QUIET:
		if (initialized) { 
			if (!quiet) fprintf(stderr,"Error: you cannot set command %d when the engine is already initialized.",cmd);
			return 0;
			} else return inc_quiet();
	case CMD_VERBOSE:
		if (initialized) {
			if (!quiet) fprintf(stderr,"Error: you cannot set command %d when the engine is already initialized.",cmd);
			return 0;
			} else return inc_verbose();
	case CMD_BUFFER_SIZE:
		if (initialized) {
			if (!quiet) fprintf(stderr,"Error: you cannot set command %d when the engine is already initialized.",cmd);
			return 0;
			} else return set_buffer_size((const char *)p);
	default:
		break;
	}
	if (!quiet) fprintf(stderr,"Command not implemented: %d - %s",cmd,(char*)p);
	return 0;
}

static int opencl_bind_helper(ENGINE * e) {
	if (!ENGINE_set_id(e, OPENCL_ENGINE_ID) ||
	    !ENGINE_set_init_function(e, opencl_init) ||
	    !ENGINE_set_finish_function(e, opencl_finish) ||
	    !ENGINE_set_ctrl_function(e, opencl_engine_ctrl) ||
	    !ENGINE_set_cmd_defns(e, opencl_cmd_defns) ||
	    !ENGINE_set_name(e, OPENCL_ENGINE_NAME) ||
	    !ENGINE_set_ciphers (e, opencl_ciphers)) {
		return 0;
	} else {
		return 1;
	}
}

static int opencl_bind_fn(ENGINE *e, const char *id) {
	if (id && (strcmp(id, OPENCL_ENGINE_ID) != 0)) {
		if (!quiet) fprintf(stderr, "bad engine id\n");
		return 0;
	}
	if (!opencl_bind_helper(e))  {
		if (!quiet) fprintf(stderr, "bind failed\n");
		return 0;
	}

	return 1;
}

IMPLEMENT_DYNAMIC_CHECK_FN()
IMPLEMENT_DYNAMIC_BIND_FN(opencl_bind_fn)

static int opencl_cipher_nids[] = {
	NID_bf_ecb,
	NID_camellia_128_ecb,
	NID_cast5_ecb,
	NID_des_ecb,
	NID_idea_ecb
};

static int opencl_cipher_nids_num = (sizeof(opencl_cipher_nids)/sizeof(opencl_cipher_nids[0]));

static int opencl_init_key(EVP_CIPHER_CTX *ctx, const unsigned char *key, const unsigned char *iv, int enc) {
	switch ((ctx->cipher)->nid) {
	  case NID_des_ecb:
	  case NID_des_cbc:
	    if (!quiet && verbose) fprintf(stdout,"Start calculating DES key schedule...");
	    DES_key_schedule des_key_schedule;
	    DES_set_key((const_DES_cblock *)key,&des_key_schedule);
	    device_schedule = clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(DES_key_schedule), &des_key_schedule, &error);
	    DES_opencl_transfer_key_schedule(&des_key_schedule,&device_schedule,queue);
	    break;
	  case NID_bf_ecb:
	  case NID_bf_cbc:
	    if (!quiet && verbose) fprintf(stdout,"Start calculating Blowfish key schedule...\n");
	    BF_KEY key_schedule;
	    BF_set_key(&key_schedule,ctx->key_len,key);
	    device_schedule = clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(BF_KEY), &key_schedule, &error);
	    check_opencl_error(error);
	    BF_opencl_transfer_key_schedule(&key_schedule,&device_schedule,queue);
	    break;
	  case NID_cast5_ecb:
	    if (!quiet && verbose) fprintf(stdout,"Start calculating CAST5 key schedule...\n");
	    CAST_KEY cast_key_schedule;
	    CAST_set_key(&cast_key_schedule,ctx->key_len*8,key);
	    //CAST_opencl_transfer_key_schedule(&cast_key_schedule);
	    break;
	  case NID_camellia_128_ecb:
	  case NID_camellia_128_cbc:
	    if (!quiet && verbose) fprintf(stdout,"Start calculating Camellia key schedule...\n");
	    CAMELLIA_KEY cmll_key_schedule;
	    Camellia_set_key(key,ctx->key_len*8,&cmll_key_schedule);
	    //CMLL_opencl_transfer_key_schedule(&cmll_key_schedule);
	    break;
	  case NID_idea_ecb:
	  case NID_idea_cbc:
	    if (!quiet && verbose) fprintf(stdout,"Start calculating IDEA key schedule...\n");
	    IDEA_KEY_SCHEDULE idea_key_schedule;
	    idea_set_encrypt_key(key,&idea_key_schedule);
	    //IDEA_opencl_transfer_key_schedule(&idea_key_schedule);
	    break;
	  default:
	    return 0;
	}
	if (!quiet && verbose) fprintf(stdout,"DONE!\n");
	return 1;
}


static int opencl_crypt(EVP_CIPHER_CTX *ctx, unsigned char *out_arg, const unsigned char *in_arg, size_t nbytes) {
	assert(in_arg && out_arg && ctx && nbytes);
	size_t current=0;
	int chunk;

	switch(EVP_CIPHER_CTX_nid(ctx)) {
	  case NID_des_ecb:
	    opencl_device_crypt = DES_opencl_crypt;
	    break;
	  //case NID_bf_cbc:
	  case NID_bf_ecb:
	    opencl_device_crypt = BF_opencl_crypt;
	    break;
	  case NID_cast5_ecb:
	    //opencl_device_crypt = CAST_opencl_crypt;
	    break;
	  //case NID_camellia_128_cbc:
	  case NID_camellia_128_ecb:
	    //opencl_device_crypt = CMLL_opencl_crypt;
	    break;
	  //case NID_idea_cbc:
	  case NID_idea_ecb:
	    //opencl_device_crypt = IDEA_opencl_crypt;
	    break;
	  default:
	    return 0;
	}

	while (nbytes!=current) {
		chunk=(nbytes-current)/maxbytes;
		if(chunk>=1) {
			memcpy(host_data,in_arg+current,maxbytes);
			opencl_device_crypt(host_data,host_data,maxbytes,ctx->encrypt,&device_buffer,&device_schedule,queue,device_kernel, context);
			memcpy(out_arg+current,host_data,maxbytes);
			current+=maxbytes;  
		} else {
			memcpy(host_data,in_arg+current,nbytes-current);
			opencl_device_crypt(host_data,host_data,(nbytes-current),ctx->encrypt,&device_buffer,&device_schedule,queue,device_kernel, context);
			memcpy(out_arg+current,host_data,nbytes-current);
			current+=(nbytes-current);
		}
	}

	return 1;
}

#define DECLARE_EVP(lciph,uciph,ksize,lmode,umode)            \
static const EVP_CIPHER opencl_##lciph##_##ksize##_##lmode = {\
        NID_##lciph##_##ksize##_##lmode,                      \
        uciph##_BLOCK_SIZE,                                   \
        uciph##_KEY_SIZE_##ksize,                             \
        uciph##_BLOCK_SIZE,                                   \
        0 | EVP_CIPH_##umode##_MODE,                          \
        opencl_init_key,                                      \
        opencl_crypt,                                         \
        NULL,                                                 \
        sizeof(AES_KEY) + 16,                                 \
        EVP_CIPHER_set_asn1_iv,                               \
        EVP_CIPHER_get_asn1_iv,                               \
        NULL,                                                 \
        NULL                                                  \
}

#define opencl_des_64_ecb opencl_des_ecb
#define opencl_des_64_cbc opencl_des_cbc
#define NID_des_64_ecb NID_des_ecb
#define NID_des_64_cbc NID_des_cbc

#define opencl_cast5_ecb opencl_cast_ecb
#define opencl_cast_128_ecb opencl_cast_ecb
#define NID_cast_128_ecb NID_cast5_ecb

#define opencl_bf_128_ecb opencl_bf_ecb
#define opencl_bf_128_cbc opencl_bf_cbc
#define NID_bf_128_ecb NID_bf_ecb
#define NID_bf_128_cbc NID_bf_cbc

#define opencl_idea_64_ecb opencl_idea_ecb
#define opencl_idea_64_cbc opencl_idea_cbc
#define NID_idea_64_ecb NID_idea_ecb
#define NID_idea_64_cbc NID_idea_cbc

DECLARE_EVP(bf,BF,128,ecb,ECB);
DECLARE_EVP(camellia,CAMELLIA,128,ecb,ECB);
DECLARE_EVP(cast,CAST,128,ecb,ECB);
DECLARE_EVP(des,DES,64,ecb,ECB);
DECLARE_EVP(idea,IDEA,64,ecb,ECB);

static int opencl_ciphers(ENGINE *e, const EVP_CIPHER **cipher, const int **nids, int nid) {
	if (!cipher) {
		*nids = opencl_cipher_nids;
		return opencl_cipher_nids_num;
	}
	switch (nid) {
	  case NID_bf_ecb:
	    *cipher = &opencl_bf_ecb;
	    break;
	  case NID_camellia_128_ecb:
	    *cipher = &opencl_camellia_128_ecb;
	    break;
	  case NID_cast5_ecb:
	    *cipher = &opencl_cast5_ecb;
	    break;
	  case NID_des_ecb:
	    *cipher = &opencl_des_ecb;
	    break;
	  case NID_idea_ecb:
	    *cipher = &opencl_idea_ecb;
	    break;
	  default:
	    *cipher = NULL;
	    return 0;
	}
	return 1;
}