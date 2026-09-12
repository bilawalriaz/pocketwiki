#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "pwpk_reader.h"
int main(int argc,char **argv){ if(argc<3)return 2; FILE *f=fopen(argv[1],"rb"); if(!f)return 3; pwpk_reader_t r; esp_err_t e=argv[3]&&argv[3][0]=='t'?pwpk_reader_open_trusted(&r,f):pwpk_reader_open(&r,f); if(e!=ESP_OK)return 10+(int)e; pwpk_entry_t x; e=pwpk_reader_lookup(&r,strtoull(argv[2],0,10),&x); if(e!=ESP_OK)return 20+(int)e; printf("%llu %u %u %u\n",(unsigned long long)x.id,x.compressed_size,x.raw_size,x.unit_raw_length); return 0; }
