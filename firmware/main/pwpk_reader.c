#include "pwpk_reader.h"
#include <string.h>
#include "esp_rom_crc.h"

#define PWPK_MAGIC 0x4b505750u
#define PWPK_HEADER 96u
#define PWPK_ENTRY 48u

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) | ((uint64_t)u32(p+4)<<32); }
static bool range(uint32_t off, uint32_t len, uint32_t total) { return off <= total && len <= total-off; }
static bool to_u32(uint64_t x, uint32_t *out) { if (x > UINT32_MAX) return false; *out=(uint32_t)x; return true; }
static bool read_at(FILE *f, uint32_t off, void *buf, size_t n) {
    return fseek(f, (long)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

static esp_err_t pwpk_reader_open_impl(pwpk_reader_t *r, FILE *f, bool trusted)
{
    if (!r || !f) return ESP_ERR_INVALID_ARG;
    memset(r, 0, sizeof *r);
    uint8_t h[PWPK_HEADER];
    if (!read_at(f, 0, h, sizeof h) || u32(h) != PWPK_MAGIC || h[4] != 1 || u32(h+8) != PWPK_HEADER)
        return ESP_ERR_INVALID_VERSION;
    uint32_t count=u32(h+12), io=0, il=0, to=0, tl=0, d0=0, dl=0, po=0, pl=0, want=u32(h+88); uint64_t q;
    bool ok=true; q=u64(h+24); ok &= to_u32(q,&io); q=u64(h+32); ok &= to_u32(q,&il); q=u64(h+40); ok &= to_u32(q,&to); q=u64(h+48); ok &= to_u32(q,&tl); q=u64(h+56); ok &= to_u32(q,&d0); q=u64(h+64); ok &= to_u32(q,&dl); q=u64(h+72); ok &= to_u32(q,&po); q=u64(h+80); ok &= to_u32(q,&pl);
    if (!ok || h[92] || h[93] || h[94] || h[95] || h[6] || h[7] || h[5] < 1 || h[5] > 4 || io != PWPK_HEADER || count > UINT32_MAX/PWPK_ENTRY || il != count*PWPK_ENTRY ||
        to != io+il || (dl && d0 != to+tl) || (!dl && d0 != 0) ||
        po != to+tl+dl) return ESP_ERR_INVALID_SIZE;
    if (fseek(f, 0, SEEK_END) != 0) return ESP_ERR_INVALID_SIZE;
    long size=ftell(f);
    if (size < 0 || (uint64_t)po+pl != (uint64_t)size || !range(io,il,(uint32_t)size) ||
        !range(to,tl,(uint32_t)size) || !range(po,pl,(uint32_t)size)) return ESP_ERR_INVALID_SIZE;
    if (!trusted) { uint8_t buf[512]; uint32_t crc=0, off=PWPK_HEADER, left=(uint32_t)size-PWPK_HEADER;
      while (left) { size_t n=left>sizeof buf?sizeof buf:left; if(!read_at(f,off,buf,n)) return ESP_ERR_INVALID_SIZE; crc=esp_rom_crc32_le(crc,buf,n); off+=(uint32_t)n; left-=(uint32_t)n; }
      if (crc != want) return ESP_ERR_INVALID_CRC; }
    if (!trusted) { uint8_t eb[PWPK_ENTRY]; uint64_t prior=0; bool have=false;
      for(uint32_t i=0;i<count;i++) { if(!read_at(f,io+i*PWPK_ENTRY,eb,sizeof eb)) return ESP_ERR_INVALID_SIZE; uint64_t eid=u64(eb), uoq=u64(eb+8); uint32_t uo; if(!to_u32(uoq,&uo)) return ESP_ERR_INVALID_SIZE; uint32_t cs=u32(eb+16),ro=u32(eb+20),rs=u32(eb+24),tt=u32(eb+28); uint16_t ts=(uint16_t)(eb[32]|(eb[33]<<8)); uint32_t ur=u32(eb+40); if((have&&eid<=prior)||((uint16_t)(eb[34]|(eb[35]<<8)))||eb[44]||eb[45]||eb[46]||eb[47]||!ur||ro>ur||rs>ur-ro||uo>pl||cs>pl-uo||tt>tl||ts>tl-tt) return ESP_ERR_INVALID_SIZE; prior=eid;have=true; } }
    r->file=f; r->count=count; r->index_offset=io; r->index_length=il; r->titles_offset=to; r->titles_length=tl; r->pack_id=u64(h+16);
    r->dictionary_offset=d0; r->dictionary_length=dl; r->payload_offset=po; r->payload_length=pl; r->codec=h[5]; r->valid=true;
    return ESP_OK;
}

/* Catalogue-selected reopen validates header/section bounds and file size,
 * while skipping the one-time body CRC and full index scan. */
esp_err_t pwpk_reader_open_trusted(pwpk_reader_t *r, FILE *f)
{ return pwpk_reader_open_impl(r, f, true); }

esp_err_t pwpk_reader_open(pwpk_reader_t *r, FILE *f)
{ return pwpk_reader_open_impl(r, f, false); }

esp_err_t pwpk_reader_lookup(const pwpk_reader_t *r, uint64_t id, pwpk_entry_t *out)
{
    if (!r || !r->valid || !out) return ESP_ERR_INVALID_ARG;
    uint32_t lo=0, hi=r->count; uint8_t b[PWPK_ENTRY];
    while (lo<hi) { uint32_t m=lo+(hi-lo)/2; if(!read_at(r->file,r->index_offset+m*PWPK_ENTRY,b,sizeof b)) return ESP_ERR_INVALID_STATE; uint64_t got=u64(b); if(got<id)lo=m+1; else hi=m; }
    if (lo==r->count || !read_at(r->file,r->index_offset+lo*PWPK_ENTRY,b,sizeof b) || u64(b)!=id) return ESP_ERR_NOT_FOUND;
    out->id=id; out->unit_offset=u64(b+8); out->compressed_size=u32(b+16); out->raw_offset=u32(b+20); out->raw_size=u32(b+24); out->title_offset=u32(b+28); out->title_size=(uint16_t)(b[32]|(b[33]<<8)); out->crc32=u32(b+36); out->unit_raw_length=u32(b+40);
    if (u32(b+8+4) != (uint32_t)out->unit_offset || (uint16_t)(b[34]|(b[35]<<8)) || b[44]||b[45]||b[46]||b[47] || !out->unit_raw_length || out->raw_offset > out->unit_raw_length || out->raw_size > out->unit_raw_length-out->raw_offset || out->unit_offset > r->payload_length || out->compressed_size > r->payload_length-(uint32_t)out->unit_offset || out->title_offset > r->titles_length || out->title_size > r->titles_length-out->title_offset) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

esp_err_t pwpk_reader_read_payload(const pwpk_reader_t *r, uint64_t off, void *buf, size_t n)
{ if(!r||!r->valid||off>r->payload_length||n>r->payload_length-off) return ESP_ERR_INVALID_ARG; return read_at(r->file,r->payload_offset+(uint32_t)off,buf,n)?ESP_OK:ESP_ERR_INVALID_STATE; }
esp_err_t pwpk_reader_read_title(const pwpk_reader_t *r,const pwpk_entry_t *e,char *buf,size_t cap,size_t *len)
{ if(!r||!e||!buf||cap==0||e->title_size>=cap) return ESP_ERR_INVALID_ARG; if(!read_at(r->file,r->titles_offset+e->title_offset,buf,e->title_size)) return ESP_ERR_INVALID_STATE; buf[e->title_size]=0; if(len)*len=e->title_size; return ESP_OK; }
