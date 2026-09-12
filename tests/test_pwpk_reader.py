import subprocess, shutil, struct, zlib
from pathlib import Path
import pytest
ROOT=Path(__file__).parents[1]

@pytest.fixture(scope='session')
def c_reader(tmp_path_factory):
    cc=shutil.which('cc') or shutil.which('gcc')
    if not cc: pytest.skip('no C compiler')
    inc=tmp_path_factory.mktemp('shim')
    (inc/'esp_err.h').write_text('''typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_INVALID_SIZE 3
#define ESP_ERR_INVALID_VERSION 4
#define ESP_ERR_INVALID_CRC 5
#define ESP_ERR_NOT_FOUND 6
''')
    (inc/'esp_rom_crc.h').write_text('''#include <stdint.h>
static inline uint32_t esp_rom_crc32_le(uint32_t c,const uint8_t*p,unsigned n){c=~c;while(n--){c^=*p++;for(int i=0;i<8;i++)c=(c&1)?0xedb88320u^(c>>1):c>>1;}return ~c;}\n''')
    out=inc/'reader';cmd=[cc,'-std=c99','-I',str(inc),'-I',str(ROOT/'firmware/main'),str(ROOT/'firmware/main/pwpk_reader.c'),str(ROOT/'tests/c_pwpk_reader_harness.c'),'-o',str(out)]
    subprocess.run(cmd,check=True,capture_output=True);return out

def test_c_reader_real_pack_and_trusted(tmp_path,c_reader):
    import sys;sys.path.insert(0,str(ROOT/'tools'));from pwpk import Article,build_pack
    p=tmp_path/'x.pwpk';p.write_bytes(build_pack([Article(9,'x',b'hello'*10),Article(2,'y',b'world'*20)],block_size=100))
    for mode in ('','trusted'):
        a=[str(c_reader),str(p),'9']+(['trusted'] if mode else [])
        got=subprocess.run(a,capture_output=True);assert got.returncode==0,got.stderr

def test_c_reader_rejects_refreshed_bad_offset(tmp_path,c_reader):
    import sys;sys.path.insert(0,str(ROOT/'tools'));from pwpk import Article,build_pack,HEADER_SIZE
    b=bytearray(build_pack([Article(2,'x',b'hello')]))
    struct.pack_into('<I',b,HEADER_SIZE+20,0xffffffff)
    struct.pack_into('<I',b,88,zlib.crc32(b[HEADER_SIZE:])&0xffffffff)
    p=tmp_path/'bad.pwpk';p.write_bytes(b)
    assert subprocess.run([str(c_reader),str(p),'2']).returncode!=0
