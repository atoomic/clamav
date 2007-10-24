/*
 *  Copyright (C) 2007 Sourcefire Inc.
 *  Author: aCaB <acab@clamav.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#define printf(...)

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifndef O_BINARY
#define O_BINARY        0
#endif

#include "others.h"
#include "scanners.h"


/* FIXME: use unicode detection and normalization from edwin */
static void u2a(uint8_t *b, unsigned int s) {
  unsigned int i;
  if(s<2) return;
  if(b[1]!=0) return;
  for (i = 0 ; i < s; i+=2) b[i/2] = b[i];
  b[i/2]='\0';
}
    

/*********************
   MT realted stuff 
*********************/

struct MT {
  uint32_t mt[624];
  uint32_t items;
  uint32_t *next;
};

static uint8_t MT_getnext(struct MT *MT) {
  uint32_t r;

  if (!--MT->items) {
    uint32_t *mt = MT->mt;
    unsigned int i;

    MT->items = 624;
    MT->next = mt;

    for (i=0; i<227; i++)
      mt[i] = ((((mt[i] ^ mt[i+1])&0x7ffffffe)^mt[i])>>1)^((0-(mt[i+1]&1))&0x9908b0df)^mt[i+397];
    for (; i<623; i++)
      mt[i] = ((((mt[i] ^ mt[i+1])&0x7ffffffe)^mt[i])>>1)^((0-(mt[i+1]&1))&0x9908b0df)^mt[i-227];
    mt[623] = ((((mt[623] ^ mt[0])&0x7ffffffe)^mt[623])>>1)^((0-(mt[0]&1))&0x9908b0df)^mt[i-227];
  }

  r = *(MT->next++);
  r ^= (r >> 11);
  r ^= ((r & 0xff3a58ad) << 7);
  r ^= ((r & 0xffffdf8c) << 15);
  r ^= (r >> 18);
  return (uint8_t)(r >> 1);
}

static void MT_decrypt(uint8_t *buf, unsigned int size, uint32_t seed) {
  struct MT MT;
  unsigned int i;
  uint32_t *mt = MT.mt;

  *mt=seed;
  for(i=1; i<624; i++)
    mt[i] = i+0x6c078965*((mt[i-1]>>30)^mt[i-1]);
  MT.items = 1;

  while(size--)
    *buf++ ^= MT_getnext(&MT);
}


/*********************
     inflate stuff 
*********************/

struct UNP {
  uint8_t *outputbuf;
  uint8_t *inputbuf;
  uint32_t cur_output;
  uint32_t cur_input;
  uint32_t usize;
  uint32_t csize;
  uint32_t bits_avail;
  union {
    uint32_t full;
    struct {
#if WORDS_BIGENDIAN != 0
      uint16_t h; /* BE */
      uint16_t l;
#else
      uint16_t l; /* LE */
      uint16_t h;
#endif
    } half;
  } bitmap;
  uint32_t error;
};


static uint32_t getbits(struct UNP *UNP, uint32_t size) {
  UNP->bitmap.half.h = 0;
  if (size > UNP->bits_avail && ((size - UNP->bits_avail - 1)/16+1)*2 > UNP->csize - UNP->cur_input) {
    cli_dbgmsg("autoit: getbits() - not enough bits available");
    UNP->error = 1;
    return 0; /* won't infloop nor spam */
  }
  while (size) {
    if (!UNP->bits_avail) {
      UNP->bitmap.half.l |= UNP->inputbuf[UNP->cur_input++]<<8;
      UNP->bitmap.half.l |= UNP->inputbuf[UNP->cur_input++];
      UNP->bits_avail = 16;
    }
    UNP->bitmap.full<<=1;
    UNP->bits_avail--;
    size--;
  }
  return (uint32_t)UNP->bitmap.half.h;
}


/*********************
 autoit3 EA05 handler 
*********************/


static int ea05(int desc, cli_ctx *ctx) {
  uint8_t b[24], comp;
  uint8_t *buf = b;
  uint32_t s, us, m4sum=0;
  int i;
  char *tempfile;
  struct UNP UNP;

  if (cli_readn(desc, buf, 24)!=24)
    return CL_CLEAN;

  for (i=0; i<16; i++)
    m4sum += *buf++;
  
  MT_decrypt(buf,4,0x16fa);
  if(cli_readint32(buf) != 0x454c4946) {
    cli_dbgmsg("autoit: no FILE magic found, giving up\n");
    return CL_CLEAN;
  }

  buf+=4;
  s = cli_readint32(buf) ^ 0x29bc;
  buf=b;
  if (s > 23) {
    cli_dbgmsg("autoit: magic string too long, giving up\n");
    return CL_CLEAN;
  }
  if(cli_debug_flag) {
    cli_dbgmsg("autoit: magic string size %d (expected values 23 or 15)\n", s);
    if (cli_readn(desc, buf, s)!=(int)s)
      return CL_CLEAN;
    buf[s]='\0';
    MT_decrypt(buf,s,s+0xa25e);
    cli_dbgmsg("autoit: magic string '%s'\n", buf);
  } else {
    lseek(desc, s, SEEK_CUR);
  }

  if (cli_readn(desc, buf, 4)!=4)
    return CL_CLEAN;
  s = cli_readint32(buf) ^ 0x29ac;
  if(cli_debug_flag && s<300) {
    uint8_t *n;
    if (!(n = cli_malloc(s+1)))
      return CL_EMEM;
    if (cli_readn(desc, n, s)!=(int)s) {
      free(n);
      return CL_CLEAN;
    }
    MT_decrypt(n,s,s+0xf25e);
    n[s]='\0';
    cli_dbgmsg("autoit: original filename '%s'\n", n);
    free(n);
  } else {
    lseek(desc, s, SEEK_CUR);
  }

  if (cli_readn(desc, buf, 13)!=13)
    return CL_CLEAN;
  comp = *buf;
  UNP.csize = cli_readint32(buf+1) ^ 0x45aa;
  cli_dbgmsg("autoit: compressed size: %x\n", UNP.csize);
  us = cli_readint32(buf+5) ^ 0x45aa;
  cli_dbgmsg("autoit: advertised uncompressed size %x\n", us);
  s = cli_readint32(buf+9) ^ 0xc3d2;
  cli_dbgmsg("autoit: ref chksum: %x\n", s);

  if(ctx->limits && ctx->limits->maxfilesize && UNP.csize > ctx->limits->maxfilesize) {
    cli_dbgmsg("autoit: sizes exceeded (%lu > %lu)\n", (unsigned long int)UNP.csize, ctx->limits->maxfilesize);
    return CL_CLEAN;
  }

  lseek(desc, 16, SEEK_CUR);
  if (!(buf = cli_malloc(UNP.csize)))
    return CL_EMEM;
  if (cli_readn(desc, buf, UNP.csize)!=(int)UNP.csize) {
    cli_dbgmsg("autoit: failed to read compressed stream. broken/truncated file?\n");
    free(buf);
    return CL_CLEAN;
  }
  MT_decrypt(buf,UNP.csize,0x22af+m4sum);

  if (comp == 1) {
    cli_dbgmsg("autoit: script is compressed\n");
    if (cli_readint32(buf)!=0x35304145) {
      cli_dbgmsg("autoit: bad magic or unsupported version\n");
      return CL_EFORMAT;
    }

    UNP.usize = ntohl(*(uint32_t *)(buf+4)); /* FIXME: portable? */
    if(ctx->limits && ctx->limits->maxfilesize && UNP.usize > ctx->limits->maxfilesize) {
      free(buf);
      return CL_CLEAN;
    }
    if (!(UNP.outputbuf = cli_malloc(UNP.usize))) {
      free(buf);
      return CL_EMEM;
    }
    cli_dbgmsg("autoit: uncompressed size again: %x\n", UNP.usize);

    UNP.inputbuf = buf;
    UNP.cur_output = 0;
    UNP.cur_input = 8;
    UNP.bitmap.full = 0;
    UNP.bits_avail = 0;
    UNP.error = 0;
  
    while (!UNP.error && UNP.cur_output < UNP.usize) {
      if (getbits(&UNP, 1)) {
	uint32_t bb, bs, addme=0;
	bb = getbits(&UNP, 15);
      
	if ((bs = getbits(&UNP, 2))==3) {
	  addme = 3;
	  if((bs = getbits(&UNP, 3))==7) {
	    addme = 10;
	    if((bs = getbits(&UNP, 5))==31) {
	      addme = 41;
	      if((bs = getbits(&UNP, 8))==255) {
		addme = 296;
		while((bs = getbits(&UNP, 8))==255) {
		  addme+=255;
		}
	      }
	    }
	  }
	}
	bs += 3+addme;

	if(!CLI_ISCONTAINED(UNP.outputbuf, UNP.usize, &UNP.outputbuf[UNP.cur_output], bs) ||
	   !CLI_ISCONTAINED(UNP.outputbuf, UNP.usize, &UNP.outputbuf[UNP.cur_output-bb], bs)) {
	  UNP.error = 1;
	  break;
	}
	while(bs--) {
	  UNP.outputbuf[UNP.cur_output]=UNP.outputbuf[UNP.cur_output-bb];
	  UNP.cur_output++;
	}
      } else {
	UNP.outputbuf[UNP.cur_output] = (uint8_t)getbits(&UNP, 8);
	UNP.cur_output++;
      }
    }

    free(buf);
    if (UNP.error) {
      cli_dbgmsg("autoit: decompression error\n");
      free(UNP.outputbuf);
      return CL_CLEAN;
    }
  } else {
    /* No clues how this would look like o.0 */
    cli_dbgmsg("autoit: script is not compressed\n");
    UNP.outputbuf = buf;
    UNP.usize = UNP.csize;
  }


  /* FIXME: TODO send to text notmalization */

  if(!(tempfile = cli_gentemp(NULL))) {
    free(UNP.outputbuf);
    return CL_EMEM;
  }
  if((i = open(tempfile, O_RDWR|O_CREAT|O_TRUNC|O_BINARY, S_IRWXU)) < 0) {
    cli_dbgmsg("autoit: Can't create file %s\n", tempfile);
    free(tempfile);
    free(UNP.outputbuf);
    return CL_EIO;
  }
  if(cli_writen(i, UNP.outputbuf, UNP.usize) != UNP.usize) {
    cli_dbgmsg("autoit: cannot write %d bytes\n", UNP.usize);
    close(i);
    free(tempfile);
    free(UNP.outputbuf);
    return CL_EIO;
  }
  free(UNP.outputbuf);
  if(cli_leavetemps_flag)
    cli_dbgmsg("autoit: script extracted to %s\n", tempfile);
  else 
    cli_dbgmsg("autoit: script successfully extracted\n");
  fsync(i);
  lseek(i, 0, SEEK_SET);
  if(cli_magic_scandesc(i, ctx) == CL_VIRUS) {
    close(i);
    if(!cli_leavetemps_flag) unlink(tempfile);
    free(tempfile);
    return CL_VIRUS;
  }
  close(i);
  return CL_CLEAN;
}


/*********************
  LAME realted stuff 
*********************/

#define ROFL(a,b) (( a << (b % (sizeof(a)<<3) ))  |  (a >> (  (sizeof(a)<<3)  -  (b % (sizeof(a)<<3 )) ) ))

struct LAME {
  uint32_t c0;
  uint32_t c1;
  uint32_t grp1[17];
  uint32_t grp2[17];
  uint32_t grp3[17];
  uint32_t unk2;
  uint32_t unk3;
};


static double LAME_fpusht(struct LAME *l) {
  union {
    double as_double;
    struct { /* FIXME: SWAP IF FPU_BIGENDIAN */
      uint32_t lo;
      uint32_t hi;
    } as_uint;
  } ret;

  uint32_t rolled = ROFL(l->grp1[l->c0],9) +  ROFL(l->grp1[l->c1],13);

  l->grp1[l->c0] = rolled;

  if (!l->c0--) l->c0 = 16;
  if (!l->c1--) l->c1 = 16;

  if (l->grp1[l->c0] == l->grp2[0]) {
    if (!memcmp(l->grp1, (uint32_t *)l + 0x24 - l->c0, 0x44)) {
      printf("returned %e\n", 0);
      return 0.0;
    }
  }

  ret.as_uint.lo = rolled << 0x14;
  ret.as_uint.hi = 0x3ff00000 | (rolled >> 0xc);
  printf("returned %e\n", ret.as_double - 1.0);
  return ret.as_double - 1.0;
}


static void LAME_srand(struct LAME *l, uint32_t seed) {
  unsigned int i;

  for (i=0; i<17; i++) {
    seed *= 0x53A9B4FB; /*1403630843*/
    seed = 1 - seed;
    l->grp1[i] = seed;
  }

  l->c0 = 0;
  l->c1 = 10;
  l->unk2 = 0;

  memcpy(l->grp2, l->grp1, sizeof(l->grp1));
  memcpy(l->grp3, l->grp1, sizeof(l->grp1));

  for (i = 0; i < 9; i++)
    LAME_fpusht(l);
}

static uint8_t LAME_getnext(struct LAME *l) {
  double x;
  uint8_t ret;
  LAME_fpusht(l);
  x = LAME_fpusht(l) * 256.0;
  if ((int32_t)x < 256) ret = (uint8_t)x;
  else ret=0xff;
  printf("next: %02x\n", ret);
  return ret;
}

static void LAME_decrypt (uint8_t *cypher, uint32_t size, uint16_t seed) {
  struct LAME lame;
  /* mt_srand_timewrap(struct srand_struc bufDC); */
  lame.unk2 = 0;

  LAME_srand(&lame, (uint32_t)seed);
  while(size--)
    *cypher++^=LAME_getnext(&lame);
}



/*********************
 autoit3 EA06 handler 
*********************/


static int ea06(int desc, cli_ctx *ctx) {
  uint8_t b[40], comp;
  uint8_t *buf = b;
  uint32_t s, us;
  int i;
  char *tempfile;
  const char *prefixes[] = { "", "", "@", "$", "", ".", "\"", "#" };
  const char *opers[] = { ",", "=", ">", "<", "<>", ">=", "<=", "(", ")", "+", "-", "/", "*", "&", "[", "]", "==", "^", "+=", "-=", "/=", "*=", "&=" };
  struct UNP UNP;

  if (cli_readn(desc, buf, 24)!=24)
    return CL_CLEAN;

  LAME_decrypt(buf, 0x10, 0x99f2); /* FIXME: Useless due to a bug in CRC calculation - LMAO!!1 */
  buf+=0x10;

  LAME_decrypt(buf, 4, 0x18ee);
  if(cli_readint32(buf) != 0x454c4946) {
    cli_dbgmsg("autoit: no FILE magic found, giving up\n");
    return CL_CLEAN;
  }

  buf+=4;
  s = cli_readint32(buf) ^ 0xadbc;
  buf=b;
  if (s > 19) {
    cli_dbgmsg("autoit: magic string too long, giving up\n");
    return CL_CLEAN;
  }
  if(cli_debug_flag) {
    cli_dbgmsg("autoit: magic string size %d (expected value 19)\n", s);
    if (cli_readn(desc, buf, s*2)!=(int)s*2)
      return CL_CLEAN;
    buf[s*2]='\0'; buf[s*2+1]='\0';
    LAME_decrypt(buf,s*2,s+0xb33f);
    u2a(buf, s*2); /* FIXME: GET RID OF THIS */
    cli_dbgmsg("autoit: magic string '%s'\n", buf);
  } else {
    lseek(desc, s*2, SEEK_CUR);
  }

  if (cli_readn(desc, buf, 4)!=4)
    return CL_CLEAN;
  s = cli_readint32(buf) ^ 0xf820;
  if(cli_debug_flag && s<300) {
    uint8_t *n;
    if (!(n = cli_malloc(s*2+2)))
      return CL_EMEM;
    if (cli_readn(desc, n, s*2)!=(int)s*2) {
      free(n);
      return CL_CLEAN;
    }
    LAME_decrypt(n,s*2,s+0xf479);
    n[s*2]='\0'; n[s*2+1]='\0';
    u2a(n,s*2); /* FIXME: GET RID OF THIS */
    cli_dbgmsg("autoit: original filename '%s'\n", n);
    free(n);
  } else {
    lseek(desc, s*2, SEEK_CUR);
  }

  if (cli_readn(desc, buf, 13)!=13)
    return CL_CLEAN;
  comp = *buf;
  UNP.csize = cli_readint32(buf+1) ^ 0x87bc;
  cli_dbgmsg("autoit: compressed size: %x\n", UNP.csize);
  us = cli_readint32(buf+5) ^ 0x87bc;
  cli_dbgmsg("autoit: advertised uncompressed size %x\n", us);
  s = cli_readint32(buf+9) ^ 0xa685;
  cli_dbgmsg("autoit: ref chksum: %x\n", s);

  if(ctx->limits && ctx->limits->maxfilesize && UNP.csize > ctx->limits->maxfilesize) {
    cli_dbgmsg("autoit: sizes exceeded (%lu > %lu)\n", (unsigned long int)UNP.csize, ctx->limits->maxfilesize);
    return CL_CLEAN;
  }

  lseek(desc, 16, SEEK_CUR);
  if (!(buf = cli_malloc(UNP.csize)))
    return CL_EMEM;
  if (cli_readn(desc, buf, UNP.csize)!=(int)UNP.csize) {
    cli_dbgmsg("autoit: failed to read compressed stream. broken/truncated file?\n");
    free(buf);
    return CL_CLEAN;
  }
  LAME_decrypt(buf,UNP.csize,0x2477 /* + m4sum (broken by design) */ );

  if (comp == 1) {
    cli_dbgmsg("autoit: script is compressed\n");
    if (cli_readint32(buf)!=0x36304145) {
      cli_dbgmsg("autoit: bad magic or unsupported version\n");
      return CL_EFORMAT;
    }

    UNP.usize = ntohl(*(uint32_t *)(buf+4)); /* FIXME: portable? */
    if(ctx->limits && ctx->limits->maxfilesize && UNP.usize > ctx->limits->maxfilesize) {
      free(buf);
      return CL_CLEAN;
    }
    if (!(UNP.outputbuf = cli_malloc(UNP.usize))) {
      free(buf);
      return CL_EMEM;
    }
    cli_dbgmsg("autoit: uncompressed size again: %x\n", UNP.usize);

    UNP.inputbuf = buf;
    UNP.cur_output = 0;
    UNP.cur_input = 8;
    UNP.bitmap.full = 0;
    UNP.bits_avail = 0;
    UNP.error = 0;
  
    while (!UNP.error && UNP.cur_output < UNP.usize) {
      if (!getbits(&UNP, 1)) {
	uint32_t bb, bs, addme=0;
	bb = getbits(&UNP, 15);
      
	if ((bs = getbits(&UNP, 2))==3) {
	  addme = 3;
	  if((bs = getbits(&UNP, 3))==7) {
	    addme = 10;
	    if((bs = getbits(&UNP, 5))==31) {
	      addme = 41;
	      if((bs = getbits(&UNP, 8))==255) {
		addme = 296;
		while((bs = getbits(&UNP, 8))==255) {
		  addme+=255;
		}
	      }
	    }
	  }
	}
	bs += 3+addme;

	if(!CLI_ISCONTAINED(UNP.outputbuf, UNP.usize, &UNP.outputbuf[UNP.cur_output], bs) ||
	   !CLI_ISCONTAINED(UNP.outputbuf, UNP.usize, &UNP.outputbuf[UNP.cur_output-bb], bs)) {
	  UNP.error = 1;
	  break;
	}
	while(bs--) {
	  UNP.outputbuf[UNP.cur_output]=UNP.outputbuf[UNP.cur_output-bb];
	  UNP.cur_output++;
	}
      } else {
	UNP.outputbuf[UNP.cur_output] = (uint8_t)getbits(&UNP, 8);
	UNP.cur_output++;
      }
    }

    free(buf);
    if (UNP.error) {
      cli_dbgmsg("autoit: decompression error\n");
      free(UNP.outputbuf);
      return CL_CLEAN;
    }
  } else {
    /* No clues how this would look like o.0 */
    cli_dbgmsg("autoit: script is not compressed\n");
    UNP.outputbuf = buf;
    UNP.usize = UNP.csize;
  }

  if (UNP.usize<4) {
    cli_dbgmsg("autoit: script is too short\n");
    free(UNP.outputbuf);
    return CL_CLEAN;
  }

  UNP.csize = UNP.usize;
  if (!(buf = cli_malloc(UNP.csize))) {
    free(UNP.outputbuf);
    return CL_EMEM;
  }
  UNP.cur_output = 0;
  UNP.cur_input = 4;
  UNP.bits_avail = cli_readint32(UNP.outputbuf);
  cli_dbgmsg("autoit: script has got %u lines\n", UNP.bits_avail);

  while (!UNP.error && UNP.bits_avail && UNP.cur_input < UNP.usize) {
    uint8_t op;

    switch((op = UNP.outputbuf[UNP.cur_input++])) {
    case 5: /* <INT> */
      if (UNP.cur_input >= UNP.usize-4) {
	UNP.error = 1;
	cli_dbgmsg("autoit: not enough space for an int\n");
	break;
      }
      if (UNP.cur_output+12 >= UNP.csize) {
	uint8_t *newout;
	UNP.csize += 512;
	if (!(newout = cli_realloc(buf, UNP.csize))) {
	  UNP.error = 1;
	  break;
	}
	buf = newout;
      }
      UNP.cur_output += sprintf(&buf[UNP.cur_output], "0x%x ", cli_readint32(&UNP.outputbuf[UNP.cur_input]));
      UNP.cur_input += 4;
      break;

    case 0x20: /* <DOUBLE> */
      if (UNP.usize < 8 || UNP.cur_input >= UNP.usize-8) {
	UNP.error = 1;
	cli_dbgmsg("autoit: not enough space for a double\n");
	break;
      }
      if (UNP.cur_output+40 >= UNP.csize) {
	uint8_t *newout;
	UNP.csize += 512;
	if (!(newout = cli_realloc(buf, UNP.csize))) {
	  UNP.error = 1;
	  break;
	}
	buf = newout;
      }
      snprintf(&buf[UNP.cur_output], 39, "%g ", *(double *)&UNP.outputbuf[UNP.cur_input]);
      buf[UNP.cur_output+38]=' ';
      buf[UNP.cur_output+39]='\0';
      UNP.cur_output += strlen(&buf[UNP.cur_output]);
      UNP.cur_input += 8;
      break;

    case 0x30: /* COSTRUCT */
    case 0x31: /* COMMAND */
    case 0x32: /* MACRO */
    case 0x33: /* VAR */
    case 0x34: /* FUNC */
    case 0x35: /* OBJECT */
    case 0x36: /* STRING */
    case 0x37: /* DIRECTIVE */
      {
	uint32_t chars, dchars, i;

	if (UNP.cur_input >= UNP.usize-4) {
	  UNP.error = 1;
	  cli_dbgmsg("autoit: not enough space for size\n");
	  break;
	}
	chars = cli_readint32(&UNP.outputbuf[UNP.cur_input]);
	dchars = chars*2;
	UNP.cur_input+=4;

	if (UNP.usize < dchars || UNP.cur_input >= UNP.usize-dchars) {
	  UNP.error = 1;
	  cli_dbgmsg("autoit: size too big - needed %d, total %d, avail %d\n", dchars, UNP.usize, UNP.usize - UNP.cur_input);
	  break;
	}
	if (UNP.cur_output+chars+3 >= UNP.csize) {
	  uint8_t *newout;
	  UNP.csize += chars + 3;
	  if (!(newout = cli_realloc(buf, UNP.csize))) {
	    UNP.error = 1;
	    break;
	  }
	  buf = newout;
	}

	UNP.cur_output += sprintf(&buf[UNP.cur_output], "%s", prefixes[op-0x30]);

	if (chars) {
	  for (i = 0; i<dchars; i+=2) {
	    UNP.outputbuf[UNP.cur_input+i] ^= (uint8_t)chars;
	    UNP.outputbuf[UNP.cur_input+i+1] ^= (uint8_t)(chars>>8);
	  }
	  u2a(&UNP.outputbuf[UNP.cur_input], dchars);
	  memcpy(&buf[UNP.cur_output], &UNP.outputbuf[UNP.cur_input], chars);
	  UNP.cur_output += chars;
	  UNP.cur_input += dchars;
	}
	if (op==0x36)
	  buf[UNP.cur_output++] = '"';
	if (op!=0x34)
	  buf[UNP.cur_output++] = ' ';
      }
      break;

    case 0x40: /* , */
    case 0x41: /* = */
    case 0x42: /* > */
    case 0x43: /* < */
    case 0x44: /* <> */
    case 0x45: /* >= */
    case 0x46: /* <= */
    case 0x47: /* ( */
    case 0x48: /* ) */
    case 0x49: /* + */
    case 0x4a: /* - */
    case 0x4b: /* / */
    case 0x4c: /* * */
    case 0x4d: /* & */
    case 0x4e: /* [ */
    case 0x4f: /* ] */
    case 0x50: /* == */
    case 0x51: /* ^ */
    case 0x52: /* += */
    case 0x53: /* -= */
    case 0x54: /* /= */
    case 0x55: /* *= */
    case 0x56: /* &= */
      if (UNP.cur_output+4 >= UNP.csize) {
	uint8_t *newout;
	UNP.csize += 512;
	if (!(newout = cli_realloc(buf, UNP.csize))) {
	  UNP.error = 1;
	  break;
	}
	buf = newout;
      }
      UNP.cur_output += sprintf(&buf[UNP.cur_output], "%s ", opers[op-0x40]);
      break;

    case 0x7f:
      UNP.bits_avail--;
      if (UNP.cur_output+1 >= UNP.csize) {
	uint8_t *newout;
	UNP.csize += 512;
	if (!(newout = cli_realloc(buf, UNP.csize))) {
	  UNP.error = 1;
	  break;
	}
	buf = newout;
      }
      buf[UNP.cur_output++]='\n';
      break;

    default:
      cli_dbgmsg("autoit: found unknown op (%x)\n", op);
      UNP.error = 1;
    }
  }

  if (UNP.error)
    cli_dbgmsg("autoit: decompilation aborted - partial script may exist\n");

  free(UNP.outputbuf);

  /* FIXME: TODO send to text notmalization */

  if(!(tempfile = cli_gentemp(NULL))) {
    free(UNP.outputbuf);
    return CL_EMEM;
  }
  if((i = open(tempfile, O_RDWR|O_CREAT|O_TRUNC|O_BINARY, S_IRWXU)) < 0) {
    cli_dbgmsg("autoit: Can't create file %s\n", tempfile);
    free(tempfile);
    free(buf);
    return CL_EIO;
  }
  if(cli_writen(i, buf, UNP.cur_output) != UNP.cur_output) {
    cli_dbgmsg("autoit: cannot write %d bytes\n", UNP.usize);
    close(i);
    free(tempfile);
    free(buf);
    return CL_EIO;
  }
  free(buf);
  if(cli_leavetemps_flag)
    cli_dbgmsg("autoit: script extracted to %s\n", tempfile);
  else 
    cli_dbgmsg("autoit: script successfully extracted\n");
  fsync(i);
  lseek(i, 0, SEEK_SET);
  if(cli_magic_scandesc(i, ctx) == CL_VIRUS) {
    close(i);
    if(!cli_leavetemps_flag) unlink(tempfile);
    free(tempfile);
    return CL_VIRUS;
  }
  close(i);
  return CL_CLEAN;
}


/*********************
   autoit3 wrapper 
*********************/

int cli_scanautoit(int desc, cli_ctx *ctx, off_t offset) {
  uint8_t version;
  int (*func)(int desc, cli_ctx *ctx);

  lseek(desc, offset, SEEK_SET);
  if (cli_readn(desc, &version, 1)!=1)
    return CL_EIO;

  switch(version) {
  case 0x35:
    func = ea05;
    break;
  case 0x36:
    func = ea06;
    break;
  default:
    /* NOT REACHED */
    cli_dbgmsg("autoit: unknown method\n");
  }

  return func(desc, ctx);
}
