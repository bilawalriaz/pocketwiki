#!/usr/bin/env python3
"""Generate opt-in gzip browser assets offline; no network build dependencies."""
import argparse,gzip,mimetypes
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args();base=ROOT/'assets/pwpk-ui'
    files=[('index.html',base/'device.html')]+[('vendor/'+str(f.relative_to(base/'vendor')),f) for f in sorted((base/'vendor').rglob('*')) if f.is_file()]
    lines=['#include "esp_http_server.h"','#include <string.h>','#include <stdint.h>']
    rows=[]
    for i,(name,path) in enumerate(files):
        blob=gzip.compress(path.read_bytes(),compresslevel=9,mtime=0);symbol=f'asset_{i}'
        lines.append(f'static const uint8_t {symbol}[]={{'+','.join(str(b) for b in blob)+'};')
        mime='application/wasm' if name.endswith('.wasm') else ('text/javascript' if name.endswith('.js') else ('text/html; charset=utf-8' if name.endswith('.html') else 'text/plain'))
        rows.append(f'{{"/pwpk-ui/{name}","{mime}",{symbol},sizeof {symbol}}}')
    lines+=['static const struct {const char *path,*type;const uint8_t *data;size_t size;} assets[]={'+','.join(rows)+'};',
'''static esp_err_t serve_asset(httpd_req_t *req) {
 const char *path=req->uri;if(!strcmp(path,"/pwpk-ui/"))path="/pwpk-ui/index.html";
 for(size_t i=0;i<sizeof assets/sizeof assets[0];i++)if(!strcmp(path,assets[i].path)) {
  httpd_resp_set_type(req,assets[i].type);httpd_resp_set_hdr(req,"Content-Encoding","gzip");
  for(size_t pos=0;pos<assets[i].size;pos+=2048){size_t n=assets[i].size-pos;if(n>2048)n=2048;esp_err_t e=httpd_resp_send_chunk(req,(const char *)assets[i].data+pos,n);if(e!=ESP_OK)return e;}
  return httpd_resp_send_chunk(req,NULL,0);
 }
 return httpd_resp_send_err(req,HTTPD_404_NOT_FOUND,"Asset missing");
}
esp_err_t pwpk_assets_register(httpd_handle_t server) {
 httpd_uri_t route={.uri="/pwpk-ui/*",.method=HTTP_GET,.handler=serve_asset};return httpd_register_uri_handler(server,&route);
}''']
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text('\n'.join(lines)+'\n')
if __name__=='__main__':main()
