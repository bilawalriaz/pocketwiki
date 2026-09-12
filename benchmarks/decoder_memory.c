/* Host allocator instrumentation, not an ESP measurement.
 * cc -O2 -I/opt/homebrew/include benchmarks/decoder_memory.c \
 *   -L/opt/homebrew/lib -lzstd -lbrotlidec -llzma -lz -o build/decoder_memory
 * build/decoder_memory zstd frame.bin [dictionary.bin]
 */
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <brotli/decode.h>
#include <lzma.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

typedef union { struct { size_t n; } s; max_align_t align; } Header;
static size_t live_bytes, peak_bytes, allocations;
static void *allocate(void *opaque, size_t n) {
    (void)opaque;
    if (n>SIZE_MAX-sizeof(Header)) return NULL;
    Header *h=malloc(sizeof(Header)+n); if (!h) return NULL;
    h->s.n=n;live_bytes+=n;if(live_bytes>peak_bytes)peak_bytes=live_bytes;allocations++;
    return h+1;
}
static void release(void *opaque, void *p) {
    (void)opaque;if(!p)return;Header *h=(Header *)p-1;live_bytes-=h->s.n;free(h);
}
static void *allocate_array(void *opaque, size_t n, size_t size) {
    if(size && n>SIZE_MAX/size)return NULL;return allocate(opaque,n*size);
}
static voidpf zallocate(voidpf opaque,uInt n,uInt size){return allocate_array(opaque,n,size);}
static void zrelease(voidpf opaque,voidpf p){release(opaque,p);}
static unsigned char *read_file(const char *path,size_t *len){
    FILE *f=fopen(path,"rb");if(!f)return NULL;
    if(fseek(f,0,SEEK_END)||ftell(f)<0){fclose(f);return NULL;}
    *len=(size_t)ftell(f);rewind(f);unsigned char *p=malloc(*len?*len:1);
    if(!p||fread(p,1,*len,f)!=*len){free(p);fclose(f);return NULL;}fclose(f);return p;
}
int main(int argc,char **argv){
    if(argc<3)return 2;size_t input_size=0,dict_size=0;
    unsigned char *input=read_file(argv[2],&input_size),*dict=NULL;
    if(!input)return 2;if(argc>3){dict=read_file(argv[3],&dict_size);if(!dict)return 2;}
    unsigned char out[4096];size_t decoded=0;int ok=0;clock_t start=clock();
    if(!strcmp(argv[1],"zstd")){
        ZSTD_customMem mem={allocate,release,NULL};ZSTD_DCtx *ctx=ZSTD_createDCtx_advanced(mem);
        if(!ctx)return 3;
        if(dict && ZSTD_isError(ZSTD_DCtx_loadDictionary(ctx,dict,dict_size)))return 3;
        ZSTD_inBuffer ib={input,input_size,0};size_t r=1;
        while(r){ZSTD_outBuffer ob={out,sizeof(out),0};size_t before=ib.pos;
            r=ZSTD_decompressStream(ctx,&ob,&ib);decoded+=ob.pos;
            if(ZSTD_isError(r)||(before==ib.pos&&!ob.pos))break;
        }
        ok=r==0 && ib.pos==ib.size;ZSTD_freeDCtx(ctx);
    }else if(!strcmp(argv[1],"brotli")){
        BrotliDecoderState *ctx=BrotliDecoderCreateInstance(allocate,release,NULL);if(!ctx)return 3;
        size_t avail=input_size;const uint8_t *in=input;BrotliDecoderResult r=BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT;
        do{size_t size=sizeof(out);uint8_t *p=out;r=BrotliDecoderDecompressStream(ctx,&avail,&in,&size,&p,NULL);decoded+=sizeof(out)-size;}
        while(r==BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT);
        ok=r==BROTLI_DECODER_RESULT_SUCCESS && avail==0;BrotliDecoderDestroyInstance(ctx);
    }else if(!strcmp(argv[1],"xz")){
        lzma_allocator allocator={allocate_array,release,NULL};lzma_stream s=LZMA_STREAM_INIT;s.allocator=&allocator;
        if(lzma_stream_decoder(&s,128*1024*1024,0)!=LZMA_OK)return 3;
        s.next_in=input;s.avail_in=input_size;lzma_ret r;
        do{s.next_out=out;s.avail_out=sizeof(out);r=lzma_code(&s,LZMA_FINISH);decoded+=sizeof(out)-s.avail_out;}while(r==LZMA_OK);
        ok=r==LZMA_STREAM_END && s.avail_in==0;lzma_end(&s);
    }else if(!strcmp(argv[1],"gzip")){
        z_stream s={0};s.zalloc=zallocate;s.zfree=zrelease;if(inflateInit2(&s,31)!=Z_OK)return 3;
        s.next_in=input;s.avail_in=(uInt)input_size;int r;
        do{s.next_out=out;s.avail_out=sizeof(out);r=inflate(&s,Z_NO_FLUSH);decoded+=sizeof(out)-s.avail_out;}while(r==Z_OK);
        ok=r==Z_STREAM_END && s.avail_in==0;inflateEnd(&s);
    }
    printf("{\"codec\":\"%s\",\"ok\":%s,\"decoder_peak_allocated_bytes\":%zu,\"allocations\":%zu,\"live_after_free\":%zu,\"output_buffer_bytes\":%zu,\"external_dictionary_bytes\":%zu,\"decoded_bytes\":%zu,\"cpu_seconds\":%.9f}\n",argv[1],ok?"true":"false",peak_bytes,allocations,live_bytes,sizeof(out),dict_size,decoded,(double)(clock()-start)/CLOCKS_PER_SEC);
    free(input);free(dict);return ok?0:1;
}
