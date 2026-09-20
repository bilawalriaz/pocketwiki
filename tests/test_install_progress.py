"""Exercise streaming UI and backpressure without an ESP32 or browser network."""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_web_install_stream(tmp_path):
    node = shutil.which('node')
    if not node:
        pytest.skip('node is required for the browser protocol test')
    source = (ROOT / 'assets/manager.js').read_text()
    function = source[source.index('async function installFromDevice('):source.index('function renderCatalog')]
    script = tmp_path / 'test.cjs'
    script.write_text('''const assert=require('node:assert/strict');
const transferText=(r,t)=>`${r}/${t}`;
''' + function + '''
(async()=>{
 const status={textContent:''};
 const p={hidden:true,max:100,value:0,removeAttribute(){this.value=null}};
 const encoder=new TextEncoder();
 let step=0;
 global.fetch=async()=>({ok:true,body:{getReader(){return {async read(){
   switch(step++){
    case 0:return {value:encoder.encode('P:0:100\\nP:2')};
    case 1:assert.equal(p.value,0);return {value:encoder.encode('5:100\\n')};
    case 2:assert.equal(p.value,25);assert.equal(p.hidden,false);return {value:encoder.encode('P:100:100\\nOK:42\\n')};
    default:return {done:true};
   }
 }}}}});
 assert.equal(await installFromDevice('https://example/pack','pack',status,p),true);
 assert.equal(status.textContent,'Pack installed — 42 articles.');
 assert.equal(p.hidden,true);
 global.fetch=async()=>({ok:false,status:503,json:async()=>({error:'station not connected'})});
 assert.equal(await installFromDevice('u','n',status,p),false);
 assert.equal(status.textContent,'station not connected');
 for(const text of ['P:30:100\\nERR:checksum mismatch\\n','P:30:100\\n']){
  let sent=false;
  global.fetch=async()=>({ok:true,body:{getReader(){return {async read(){if(sent)return {done:true};sent=true;return {value:encoder.encode(text)}}}}}});
  assert.equal(await installFromDevice('u','n',status,p),false);
  assert.match(status.textContent,text.includes('ERR:')?/checksum mismatch/:/before PocketWiki confirmed the install/);
 }
})().catch(e=>{console.error(e);process.exit(1)});
''')
    subprocess.run([node, str(script)], check=True)


def test_web_batch_install_is_ordered_and_stops_at_failure(tmp_path):
    node = shutil.which('node')
    if not node:
        pytest.skip('node is required for the browser protocol test')
    source = (ROOT / 'assets/manager.js').read_text()
    function = source[source.index('async function installSelected('):
                      source.index('catalogInstall.onclick')]
    script = tmp_path / 'batch.cjs'
    script.write_text('''const assert=require('node:assert/strict');
const chosen=new Map(), rendered=[], catalogProgress={};
const catalogStatus={textContent:''}, catalogClear={disabled:false};
let installing=false, calls=[], failOn=null, reloaded=false;
const location={reload(){reloaded=true}};
const setTimeout=fn=>fn();
function syncSelection(){}
function showStatus(element,message){element.textContent=message}
async function installFromDevice(url,name,status,progress,label){
  calls.push({name,label});
  return name!==failOn;
}
''' + function + '''
(async()=>{
 const a={id:'a',name:'A',bytes:10,url:'u-a'}, b={id:'b',name:'B',bytes:20,url:'u-b'}, c={id:'c',name:'C',bytes:30,url:'u-c'};
 rendered.push(a,b,c);
 /* Ticked out of order: a batch installs in catalogue order, and the label
  * says where each pack is in it. */
 chosen.set('c',c); chosen.set('a',a);
 await installSelected();
 assert.deepEqual(calls.map(call=>call.name),['a','c']);
 assert.deepEqual(calls.map(call=>call.label),['Pack 1 of 2 · ','Pack 2 of 2 · ']);
 assert.equal(chosen.size,0);
 assert.equal(reloaded,true);
 /* A rejection ends the batch: the packs after it are not attempted, and the
  * selection survives so the user can retry what is left. */
 calls=[]; reloaded=false; failOn='b';
 rendered.push(b); chosen.set('b',b); chosen.set('c',c);
 await installSelected();
 assert.deepEqual(calls.map(call=>call.name),['b']);
 assert.equal(reloaded,false);
 assert.equal(chosen.size,2);
 assert.equal(catalogClear.disabled,false);
})().catch(error=>{console.error(error);process.exit(1)});
''')
    subprocess.run([node, str(script)], check=True)


def test_web_selection_bar_budgets_the_whole_selection(tmp_path):
    node = shutil.which('node')
    if not node:
        pytest.skip('node is required for the browser protocol test')
    source = (ROOT / 'assets/manager.js').read_text()
    function = source[source.index('function syncSelection('):
                      source.index('async function installSelected(')]
    script = tmp_path / 'selection.cjs'
    script.write_text('''const assert=require('node:assert/strict');
const chosen=new Map();
const installable=40;
let installing=false, short=false, deviceUplink=null;
const sizeText=bytes=>bytes+'B';
const catalogActions={hidden:false}, catalogCount={textContent:''}, catalogRoom={textContent:''};
const catalogRoomClass={toggle(name,on){if(name==='short')short=on}};
const catalogInstall={disabled:false,textContent:''};
''' + function.replace('catalogRoom.classList', 'catalogRoomClass') + '''
const pack=(id,bytes)=>({id,bytes});
/* Nothing chosen: the bar is out of the way. */
syncSelection();
assert.equal(catalogActions.hidden,true);
/* One pack that fits: the bar offers the install and says what is left. */
chosen.set('a',pack('a',30)); syncSelection();
assert.equal(catalogActions.hidden,false);
assert.equal(catalogInstall.disabled,false);
assert.equal(catalogInstall.textContent,'Install');
assert.equal(catalogRoom.textContent,'40B available');
assert.equal(short,false);
/* Two packs that fit individually but not together: blocked, with the shortfall. */
chosen.set('b',pack('b',20)); syncSelection();
assert.equal(catalogInstall.disabled,true);
assert.equal(catalogInstall.textContent,'Install 2');
assert.equal(catalogRoom.textContent,'10B more than the 40B available');
assert.equal(short,true);
/* A running batch owns the bar, so the selection cannot be changed under it. */
installing=true; syncSelection();
assert.equal(catalogActions.hidden,true);
''')
    subprocess.run([node, str(script)], check=True)


def test_progress_short_writes_preserve_http_frames(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('C compiler required')
    source = (ROOT / 'firmware/main/web_server.c').read_text()
    begin = source.index('typedef struct {', source.index('/* After httpd has sent'))
    end = source.index('/* Device-side catalogue install:', begin)
    harness = tmp_path / 'progress.c'
    harness.write_text('''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define MSG_DONTWAIT 1
typedef int httpd_req_t;
static int httpd_req_to_sockfd(httpd_req_t *r) { return *r; }
static char output[1024];
static size_t used;
static int blocked, short_write, broken;
static int send(int fd, const void *data, size_t len, int flags) {
 (void)fd; (void)flags;
 if(broken){errno=EPIPE;return -1;}
 if(blocked){errno=EAGAIN;return -1;}
 if(short_write && len>3){len=3;blocked=1;}
 memcpy(output+used,data,len);used+=len;return (int)len;
}
''' + source[begin:end] + '''
int main(void) {
 httpd_req_t req=1;
 install_progress_t p={0};
 short_write=1;
 install_progress_send(&req,&p,25,100);
 assert(p.offset==3 && p.length>3 && !p.failed);
 install_progress_send(&req,&p,50,100);
 assert(used==3); /* Must not replace a partially sent frame. */
 blocked=0;short_write=0;
 install_progress_flush(&req,&p,0);
 assert(strcmp(output,"9\\r\\nP:25:100\\n\\r\\n")==0);
 install_progress_send(&req,&p,100,100);
 assert(strcmp(output,"9\\r\\nP:25:100\\n\\r\\na\\r\\nP:100:100\\n\\r\\n")==0);
 broken=1;
 install_progress_send(&req,&p,100,100);
 assert(p.failed);
 return 0;
}
''')
    binary = tmp_path / 'progress'
    subprocess.run([cc, '-Wall', '-Wextra', '-Werror', str(harness), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_download_timeout_resume_and_truncation(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('C compiler required')
    source = (ROOT / 'firmware/main/web_server.c').read_text()
    begin = source.index('    for (;;) {', source.index('static esp_err_t handle_pack_install'))
    end = source.index('    /* Release the TLS client before checksum verification.', begin)
    harness = tmp_path / 'download.c'
    harness.write_text('''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#define ESP_ERR_HTTP_EAGAIN 7007
#define ESP_OK 0
#define PSA_SUCCESS 0
#define PSA_HASH_OPERATION_INIT 0
#define PSA_ALG_SHA_256 1
#define CONFIG_POCKETWIKI_PACK_MAX_UPLOAD_KB 1
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef int psa_hash_operation_t;
#define TAG "test"
static int events[16],event_index,status_code,resumes,hash_bytes;
static bool complete;
static char requested_range[64];
#undef strlcpy
#define strlcpy test_strlcpy
static size_t test_strlcpy(char *dst,const char *src,size_t size){snprintf(dst,size,"%s",src);return strlen(src);}
static int esp_http_client_read(int client,char *buf,int length){
 (void)client;(void)length;
 int n=events[event_index++];
 if(n>0)memset(buf,'a',n);
 if(n==0)complete=true;
 return n;
}
static bool esp_http_client_is_complete_data_received(int c){(void)c;return complete;}
static void esp_http_client_close(int c){(void)c;}
static void esp_http_client_set_header(int c,const char *key,const char *value){
 (void)c;assert(strcmp(key,"Range")==0);strlcpy(requested_range,value,sizeof requested_range);
}
static int esp_http_client_delete_header(int c,const char *key){
 (void)c;assert(strcmp(key,"Range")==0);return 0;
}
static int esp_http_client_open(int c,int n){(void)c;(void)n;resumes++;return 0;}
static int64_t esp_http_client_fetch_headers(int c){(void)c;return status_code==200?6:4;}
static int esp_http_client_get_status_code(int c){(void)c;return status_code;}
static int psa_hash_setup(int *h,int alg){(void)h;(void)alg;return 0;}
static void psa_hash_abort(int *h){(void)h;hash_bytes=0;}
static int psa_hash_update(int *h,const void *b,size_t n){(void)h;(void)b;hash_bytes+=(int)n;return 0;}
static int64_t esp_timer_get_time(void){return 1000000;}
static void install_progress_send(int r,int *p,int64_t received,int64_t total){(void)r;(void)p;(void)received;(void)total;}
static void oled_show_transfer(const char *n,size_t r,size_t t,size_t u,size_t f){(void)n;(void)r;(void)t;(void)u;(void)f;}
static bool run(const int *input,size_t count,int status,bool curated){
 memcpy(events,input,count*sizeof(int));event_index=0;resumes=0;hash_bytes=0;complete=false;status_code=status;
 int client=1,hash=0,progress=0,req=0;
 const char *sha_hex=curated?"trusted":"",*name="pack";
 bool hashing=curated;
 int64_t total=6,expected_bytes=curated?6:-1,received=0,last_progress_us=0;
 unsigned read_timeouts=0,reconnects=0;
 size_t installable=1024,flash_used=0,flash_total=1024;
 char fail[128]="";
 unsigned char buffer[4096];
 FILE *fh=tmpfile();assert(fh);
''' + source[begin:end] + '''
install_done:
 fclose(fh);
 return !fail[0];
}
int main(void){
 const int stalled[]={2,-ESP_ERR_HTTP_EAGAIN,4,0};
 assert(run(stalled,4,206,true));assert(resumes==0 && hash_bytes==6);
 const int interrupted[]={2,-1,4,0};
 assert(run(interrupted,4,206,true));assert(resumes==1 && hash_bytes==6);
 assert(strcmp(requested_range,"bytes=2-")==0);
 const int interrupted_full[]={2,-1,6,0};
 assert(run(interrupted_full,4,200,true));assert(resumes==1 && hash_bytes==6);
 const int interrupted_untrusted[]={2,-1,6,0};
 assert(run(interrupted_untrusted,4,200,false)); /* Safe full restart without a hash. */
 const int truncated[]={2,0};
 assert(!run(truncated,2,206,false));
 const int timeout[]={-ESP_ERR_HTTP_EAGAIN,-ESP_ERR_HTTP_EAGAIN,-ESP_ERR_HTTP_EAGAIN};
 assert(!run(timeout,3,206,true));assert(event_index==3);
 return 0;
}
''')
    binary = tmp_path / 'download'
    subprocess.run([cc, '-Wall', '-Wextra', '-Werror', str(harness), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_pack_tls_receive_capacity_matches_standard_records():
    defaults = (ROOT / 'firmware/sdkconfig.defaults').read_text()
    assert 'CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=8192' not in defaults
    assert 'CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384' in defaults

    source = (ROOT / 'firmware/main/web_server.c').read_text()
    install = source[source.index('static esp_err_t handle_pack_install'):
                     source.index('/* ---- /browse:', source.index('static esp_err_t handle_pack_install'))]
    assert '.buffer_size = 1024' in install
    assert '.buffer_size_tx = 512' in install
    assert 'buffer = malloc(1024)' in install
    assert 'esp_http_client_read(client, (char *)buffer, 1024)' in install
    assert 'ble_provisioning_suspend()' in install
    assert 'ble_provisioning_resume()' in install
