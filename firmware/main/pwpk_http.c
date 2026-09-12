/* Experimental multi-pack service. Production PWKP archives remain separate. */
#include "pwpk_http.h"
#include "pwpk_reader.h"
#include "pwpk_service.h"
#include "pack_store.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "esp_log.h"
#include "zstd.h"
#include "cJSON.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>

#define MAX_PACKS 25
#define NAME_CAP 41
#define PATH_CAP 64
#define OUTPUT_BYTES 2048
#define MAX_DECODE_UNIT (256u * 1024u)
typedef struct { char name[NAME_CAP]; uint64_t id, first_id; uint32_t count, size; } catalogue_t;
static catalogue_t s_catalogue[MAX_PACKS];
static size_t s_count;
static uint32_t s_requests, s_failures;
static size_t s_last_decoder, s_last_before, s_last_after;
static int64_t s_last_decode_us, s_last_response_us;
static const char *TAG="pwpk_http";

static void path_for(char *out, const char *name) { snprintf(out,PATH_CAP,PW_PACK_DIR "/%s.pwpk",name); }
static esp_err_t json_reply(httpd_req_t *req,cJSON *json) {
    if (!json) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"JSON allocation failed");
    char *s=cJSON_PrintUnformatted(json); cJSON_Delete(json);
    if(!s) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"JSON allocation failed");
    httpd_resp_set_type(req,"application/json");esp_err_t err=httpd_resp_sendstr(req,s);free(s);return err;
}
static esp_err_t rescan(void) {
    s_count=0;DIR *d=opendir(PW_PACK_DIR);if(!d)return ESP_FAIL;struct dirent *de;
    while((de=readdir(d)) && s_count<MAX_PACKS) {
        size_t n=strlen(de->d_name);if(n<=5||strcmp(de->d_name+n-5,".pwpk")||n-5>=NAME_CAP)continue;
        char name[NAME_CAP],path[PATH_CAP];memcpy(name,de->d_name,n-5);name[n-5]=0;if(!pack_store_safe_name(name))continue;
        path_for(path,name);FILE *f=fopen(path,"rb");if(!f)continue;pwpk_reader_t r;
        if(pwpk_reader_open(&r,f)==ESP_OK) {
            catalogue_t *c=&s_catalogue[s_count++];strlcpy(c->name,name,sizeof c->name);c->id=r.pack_id;uint8_t first[8];fseek(f,r.index_offset,SEEK_SET);c->first_id=0;if(fread(first,1,8,f)==8){for(unsigned j=0;j<8;j++)c->first_id|=(uint64_t)first[j]<<(j*8);}
            c->count=r.count;c->size=r.payload_offset+r.payload_length;
        }
        fclose(f);
    }
    closedir(d);return ESP_OK;
}
static catalogue_t *find_pack(const char *name) { for(size_t i=0;i<s_count;i++)if(!strcmp(name,s_catalogue[i].name))return &s_catalogue[i];return NULL; }
static esp_err_t catalogue_reply(httpd_req_t *req) {
    cJSON *root=cJSON_CreateObject(),*packs=cJSON_AddArrayToObject(root,"packs");
    for(size_t i=0;i<s_count;i++) { catalogue_t *c=&s_catalogue[i];cJSON *p=cJSON_CreateObject();char id[24];snprintf(id,sizeof id,"%016" PRIx64,c->id);
        cJSON_AddStringToObject(p,"name",c->name);cJSON_AddNumberToObject(p,"first_article_id",c->first_id);cJSON_AddStringToObject(p,"pack_id",id);cJSON_AddNumberToObject(p,"articles",c->count);cJSON_AddNumberToObject(p,"bytes",c->size);cJSON_AddItemToArray(packs,p); }
    cJSON_AddNumberToObject(root,"catalogue_static_bytes",sizeof s_catalogue);cJSON_AddNumberToObject(root,"requests",s_requests);cJSON_AddNumberToObject(root,"failures",s_failures);
    cJSON_AddNumberToObject(root,"queue_depth",pwpk_service_queue_depth());cJSON_AddNumberToObject(root,"queue_peak",pwpk_service_queue_peak());
    cJSON_AddNumberToObject(root,"heap_free",esp_get_free_heap_size());cJSON_AddNumberToObject(root,"heap_min",esp_get_minimum_free_heap_size());cJSON_AddNumberToObject(root,"heap_largest",heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root,"last_decoder_bytes",s_last_decoder);cJSON_AddNumberToObject(root,"last_heap_before",s_last_before);cJSON_AddNumberToObject(root,"last_heap_after",s_last_after);
    cJSON_AddNumberToObject(root,"last_decode_us",s_last_decode_us);cJSON_AddNumberToObject(root,"last_response_us",s_last_response_us);return json_reply(req,root);
}
typedef struct { char path[PATH_CAP]; uint64_t pack_id, article_id; unsigned buffer; bool raw, dictionary; int64_t queued_at; } request_t;
static esp_err_t execute(httpd_req_t *req,void *opaque) {
    request_t *job=opaque;FILE *f=fopen(job->path,"rb");pwpk_reader_t r;pwpk_entry_t e;
    esp_err_t err=ESP_FAIL;uint8_t *in=NULL,*out=NULL,*dict=NULL;ZSTD_DCtx *ctx=NULL;bool sent=false;
    s_requests++;s_last_before=esp_get_free_heap_size();s_last_decoder=0;s_last_decode_us=0;
    if(!f||pwpk_reader_open_trusted(&r,f)!=ESP_OK||r.pack_id!=job->pack_id){ goto done; }
    if(!job->dictionary && pwpk_reader_lookup(&r,job->article_id,&e)!=ESP_OK){ goto done; }
    in=malloc(job->buffer);if(!in){ goto done; }
    if(job->raw || job->dictionary) {
        uint32_t pos=job->dictionary?r.dictionary_offset:r.payload_offset+(uint32_t)e.unit_offset;
        uint32_t left=job->dictionary?r.dictionary_length:e.compressed_size;
        httpd_resp_set_type(req,"application/octet-stream");
        while(left) { size_t n=left>job->buffer?job->buffer:left;if(fseek(f,pos,SEEK_SET)||fread(in,1,n,f)!=n){ goto done; }
            if(httpd_resp_send_chunk(req,(char *)in,n)!=ESP_OK){ goto done; }sent=true;left-=n;pos+=n; }
        err=httpd_resp_send_chunk(req,NULL,0);goto done;
    }
    if(r.codec!=1||e.unit_raw_length>MAX_DECODE_UNIT){ goto done; }
    ctx=ZSTD_createDCtx();out=malloc(OUTPUT_BYTES);if(!ctx||!out){ goto done; }
    if(ZSTD_isError(ZSTD_DCtx_setParameter(ctx,ZSTD_d_windowLogMax,18))){ goto done; }
    if(r.dictionary_length) {
        if(r.dictionary_length>65536){ goto done; }dict=malloc(r.dictionary_length);if(!dict){ goto done; }
        if(fseek(f,r.dictionary_offset,SEEK_SET)||fread(dict,1,r.dictionary_length,f)!=r.dictionary_length){ goto done; }
        if(ZSTD_isError(ZSTD_DCtx_loadDictionary(ctx,dict,r.dictionary_length))){ goto done; }
        free(dict);dict=NULL;
    }
    httpd_resp_set_type(req,"text/plain; charset=utf-8");
    uint32_t left=e.compressed_size,pos=(uint32_t)e.unit_offset,decoded=0,selected=0,crc=0;size_t rc=1;
    while(left) {
        size_t n=left>job->buffer?job->buffer:left;
        if(pwpk_reader_read_payload(&r,pos,in,n)!=ESP_OK){ goto done; }left-=n;pos+=n;ZSTD_inBuffer ib={in,n,0};bool drain=false;
        do { ZSTD_outBuffer ob={out,OUTPUT_BYTES,0};size_t before=ib.pos;int64_t start=esp_timer_get_time();
            rc=ZSTD_decompressStream(ctx,&ob,&ib);s_last_decode_us+=esp_timer_get_time()-start;s_last_decoder=ZSTD_sizeof_DCtx(ctx);
            if(ZSTD_isError(rc)||decoded+ob.pos>e.unit_raw_length){ goto done; }
            uint32_t lo=decoded>e.raw_offset?decoded:e.raw_offset,hi=decoded+ob.pos;
            if(hi>e.raw_offset+e.raw_size)hi=e.raw_offset+e.raw_size;
            if(hi>lo) { size_t count=hi-lo;crc=esp_rom_crc32_le(crc,out+lo-decoded,count);
                if(httpd_resp_send_chunk(req,(char *)out+lo-decoded,count)!=ESP_OK){ goto done; }sent=true;selected+=count; }
            decoded+=ob.pos;drain=ob.pos==OUTPUT_BYTES;if(!rc){if(left||ib.pos!=ib.size){ goto done; }break;}
            if(before==ib.pos&&!ob.pos) { if(ib.pos==ib.size)break;goto done; }
        }while(ib.pos<ib.size || drain);
    }
    if(rc||decoded!=e.unit_raw_length||selected!=e.raw_size||crc!=e.crc32){ goto done; }
    err=httpd_resp_send_chunk(req,NULL,0);
done:
    s_last_after=esp_get_free_heap_size();s_last_response_us=esp_timer_get_time()-job->queued_at;
    if(err!=ESP_OK) { s_failures++;if(!sent)httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"PWPK read/decode failed");else httpd_sess_trigger_close(req->handle,httpd_req_to_sockfd(req)); }
    ESP_LOGI(TAG,"request us=%lld decode=%lld heap=%u decoder=%u ok=%d",(long long)s_last_response_us,(long long)s_last_decode_us,(unsigned)s_last_after,(unsigned)s_last_decoder,err==ESP_OK);
    if(ctx) { ZSTD_freeDCtx(ctx); }free(dict);free(out);free(in);if(f)fclose(f);free(job);return err;
}
static esp_err_t handle_get(httpd_req_t *req) {
    if(!strcmp(req->uri,"/pwpk/packs"))return catalogue_reply(req);
    char name[NAME_CAP],action[16],tail[32];name[0]=action[0]=tail[0]=0;
    int fields=sscanf(req->uri,"/pwpk/%40[^/]/%15[^/?]/%31[^?]",name,action,tail);
    if(fields<2||!pack_store_safe_name(name))return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid PWPK route");
    catalogue_t *c=find_pack(name);if(!c)return httpd_resp_send_err(req,HTTPD_404_NOT_FOUND,"Unknown PWPK pack");
    char *end;uint64_t id=fields==3?strtoull(tail,&end,10):0;
    bool dictionary=!strcmp(action,"dictionary");
    if(!dictionary&&(fields!=3||!*tail||*end))return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid article ID");
    if(!strcmp(action,"meta")) {
        char path[PATH_CAP],title[512];path_for(path,name);FILE *f=fopen(path,"rb");pwpk_reader_t r;pwpk_entry_t e;
        if(!f)return httpd_resp_send_err(req,HTTPD_404_NOT_FOUND,"Missing pack");
        esp_err_t ok=pwpk_reader_open_trusted(&r,f);if(ok==ESP_OK)ok=pwpk_reader_lookup(&r,id,&e);if(ok==ESP_OK)ok=pwpk_reader_read_title(&r,&e,title,sizeof title,NULL);fclose(f);
        if(ok!=ESP_OK)return httpd_resp_send_err(req,HTTPD_404_NOT_FOUND,"Missing article");
        cJSON *o=cJSON_CreateObject();char pack_id[24];snprintf(pack_id,sizeof pack_id,"%016" PRIx64,r.pack_id);cJSON_AddStringToObject(o,"pack_id",pack_id);cJSON_AddNumberToObject(o,"crc32",e.crc32);cJSON_AddNumberToObject(o,"id",id);cJSON_AddStringToObject(o,"title",title);cJSON_AddNumberToObject(o,"raw_offset",e.raw_offset);cJSON_AddNumberToObject(o,"raw_bytes",e.raw_size);cJSON_AddNumberToObject(o,"unit_raw_bytes",e.unit_raw_length);cJSON_AddNumberToObject(o,"unit_offset",e.unit_offset);cJSON_AddNumberToObject(o,"compressed_bytes",e.compressed_size);cJSON_AddNumberToObject(o,"dictionary_bytes",r.dictionary_length);cJSON_AddNumberToObject(o,"codec",r.codec);return json_reply(req,o);
    }
    if(!dictionary&&strcmp(action,"raw")&&strcmp(action,"article"))return httpd_resp_send_err(req,HTTPD_404_NOT_FOUND,"Unknown action");
    request_t *job=calloc(1,sizeof *job);if(!job)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Out of memory");
    path_for(job->path,name);job->pack_id=c->id;job->article_id=id;job->dictionary=dictionary;job->raw=!strcmp(action,"raw");job->buffer=2048;job->queued_at=esp_timer_get_time();
    char query[64],value[16];if(httpd_req_get_url_query_str(req,query,sizeof query)==ESP_OK&&httpd_query_key_value(query,"buffer",value,sizeof value)==ESP_OK) {
        unsigned n=(unsigned)strtoul(value,NULL,10);if(n==1024||n==2048||n==4096||n==8192||n==16384)job->buffer=n;
    }
    esp_err_t err=pwpk_service_submit(req,execute,job);if(err!=ESP_OK){free(job);if(err!=ESP_ERR_TIMEOUT)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Cannot queue request");}return ESP_OK;
}
static esp_err_t handle_upload(httpd_req_t *req) {
    const char *name=req->uri+6;
    if(strchr(name,'/')||strchr(name,'?')||!pack_store_safe_name(name)||strlen(name)>=NAME_CAP)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid pack name");
    if(pwpk_service_busy())return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Wait for readers before installing");
    if(!find_pack(name)&&s_count==MAX_PACKS)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"25-pack prototype limit");
    if(req->content_len<96||req->content_len>pack_store_installable_bytes())return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Pack exceeds free space");
    char path[PATH_CAP];path_for(path,name);const char *temp=PW_PACK_DIR "/.pwpk-upload.tmp";FILE *f=fopen(temp,"w+b");if(!f)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Cannot stage pack");
    char buf[1024];size_t left=req->content_len;bool ok=true;
    while(left){int n=httpd_req_recv(req,buf,left>sizeof buf?sizeof buf:left);if(n<=0||fwrite(buf,1,n,f)!=(size_t)n){ok=false;break;}left-=n;}
    pwpk_reader_t r;if(fflush(f)||!ok||pwpk_reader_open(&r,f)!=ESP_OK)ok=false;fclose(f);
    if(ok&&rename(temp,path))ok=false;
    if(!ok){unlink(temp);return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"PWPK validation/install failed");}
    rescan();return httpd_resp_sendstr(req,"installed");
}
static esp_err_t handle_remove(httpd_req_t *req) {
    const char *name=req->uri+6;
    if(!pack_store_safe_name(name)||strlen(name)>=NAME_CAP)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid pack name");
    if(pwpk_service_busy())return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Wait for readers before removing");
    char path[PATH_CAP];path_for(path,name);
    if(unlink(path))return httpd_resp_send_err(req,HTTPD_404_NOT_FOUND,"Missing pack");
    rescan();return httpd_resp_sendstr(req,"removed");
}
esp_err_t pwpk_http_register(httpd_handle_t server) {
#if CONFIG_POCKETWIKI_PWPK_SERVICE
    extern esp_err_t pwpk_assets_register(httpd_handle_t);
    esp_err_t assets=pwpk_assets_register(server);if(assets!=ESP_OK)return assets;
#endif
    rescan();esp_err_t err=pwpk_service_init();if(err!=ESP_OK)return err;
    httpd_uri_t get={.uri="/pwpk/*",.method=HTTP_GET,.handler=handle_get},put={.uri="/pwpk/*",.method=HTTP_POST,.handler=handle_upload};
    httpd_uri_t remove={.uri="/pwpk/*",.method=HTTP_DELETE,.handler=handle_remove};
    err=httpd_register_uri_handler(server,&get);if(err!=ESP_OK)return err;
    err=httpd_register_uri_handler(server,&put);return err==ESP_OK?httpd_register_uri_handler(server,&remove):err;
}
