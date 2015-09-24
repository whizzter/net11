#ifndef __INCLUDED_NET11_SHA1_HPP__
#define __INCLUDED_NET11_SHA1_HPP__

#pragma once

#include <stdint.h>

namespace net11 {
	
	// fast C++ sha1 class
	// most calculations is done with 32bit integers directly on the fly to
	// minimize the number of instructions and data copies.
	class sha1 {
		// accumulator used for odd bytes
		uint32_t acc;
		// input word storage
		uint32_t w[80];
		// hash state
		uint32_t h[5];
		// length of hash in BYTES
		uint64_t ml;

		// rotation utility routine.
		static inline uint32_t rol(uint32_t v,int sh) {
			return (v<<sh)|(v>>(32-sh));
		}

		// do a 512 byte block
		void block() {
			// stretch out our input words
			for (int i=16;i<80;i++) {
				w[i]=rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
			}
			// initialize local state for rounds
			uint32_t
				a=h[0],
				b=h[1],
				c=h[2],
				d=h[3],
				e=h[4];

			// setup macros to do 20 rounds at a time
#define NET11_SHA1_ITER(i,code) { \
	code; \
	uint32_t tmp=rol(a,5) + f + e + k + w[i]; \
	e = d; d = c; c = rol(b,30); b=a; a=tmp; }

#define NET11_SHA1_ITER4(i,c) \
	NET11_SHA1_ITER(i,c)   NET11_SHA1_ITER(i+1,c) \
	NET11_SHA1_ITER(i+2,c) NET11_SHA1_ITER(i+3,c)
#define NET11_SHA1_ITER20(i,c) \
	NET11_SHA1_ITER4(i,c) NET11_SHA1_ITER4(i+4,c) NET11_SHA1_ITER4(i+8,c) \
	NET11_SHA1_ITER4(i+12,c) NET11_SHA1_ITER4(i+16,c)

			// rounds 0-19 with their specific parameters
			NET11_SHA1_ITER20(0, uint32_t f=(b&c)^((~b)&d); uint32_t k=0x5a827999)
			// rounds 20-39 with their specific parameters
			NET11_SHA1_ITER20(20,uint32_t f=b^c^d; uint32_t k=0x6ed9eba1)
			// rounds 40-59 with their specific parameters
			NET11_SHA1_ITER20(40,uint32_t f=(b&c)^(b&d)^(c&d); uint32_t k=0x8f1bbcdc)
			// rounds 60-79 with their specific parameters
			NET11_SHA1_ITER20(60,uint32_t f=b^c^d; uint32_t k=0xca62c1d6)

#undef NET11_SHA1_ITER20
#undef NET11_SHA1_ITER4
#undef NET11_SHA1_ITER

			// update the main state with the local words
			h[0]+=a;
			h[1]+=b;
			h[2]+=c;
			h[3]+=d;
			h[4]+=e;
		}
	public:
		// initialize default parameters
		sha1() {
			reinit();
		}
		// utility constructors to hash some input data directly.
		template<typename T>
		sha1(const T d,int count) {
			reinit();
			addbytes(d,count);
		}
		template<typename T>
		sha1(T &in) {
			reinit();
			addbytes(in);
		}

		void reinit() {
			acc = 0;
			ml = 0;
			h[0]=0x67452301;
			h[1]=0xefcdab89;
			h[2]=0x98badcfe;
			h[3]=0x10325476;
			h[4]=0xc3d2e1f0;
		}

		// add a single byte to the hash calculations
		inline void addbyte(uint8_t b) {
			acc = (acc << 8) | (b & 0xff);
			// note ml preincrement below, correct placement needed for comparison and performance.
			if (0 == (++ml & 3)) {
				int wi = ((ml-1) >> 2) & 0xf;
				w[wi] = acc;
				if (0 == (ml & 0x3f)) {
					// 512 bits added, do block
					block();
				}
			}
		}
		// function to add a counted number of bytes quickly.
		template<typename T>
		void addbytes(const T b,int count) {
			//uint8_t *b=(uint8_t*)b;
			int i = 0;
			// pre-roll out any odd bytes
			while (i < count && (ml & 3))
				addbyte(b[i++]);
			// calculate number of fast iters
			int fc = count>>2;
			// now do the direct word iters
			while (fc--) {
				// set the word
				w[(ml >> 2) & 0xf] = (((uint8_t)b[i]) << 24) | (((uint8_t)b[i + 1]) << 16) | (((uint8_t)b[i + 2]) << 8) | ((uint8_t)b[i + 3]);
				// increment size and dest ptr
				ml += 4;
				i += 4;
				// if we've collected 64 bytes for a block, calculate it.
				if (0 == (ml & 0x3f))
					block();
			}
			// and finally the end roll for any trailing bytes
			while (i < count)
				addbyte(b[i++]);
		}
		// utility proxies to unpack bytes from a stl like container
		template<typename T>
		void addbytes(T &cnt) {
			addbytes(cnt.data(),cnt.size());
		}
		// outputs a digest
		void digest(char *o) {
			uint64_t oml = ml << 3; // this needs to be in bits.
			addbyte(0x80);
			while((ml&0x3f)!=56) {
				addbyte(0);
			}
			for (int i=56;i>=0;i-=8)
				addbyte((uint8_t)(oml>>i));
			for (int i=0;i<5;i++) {
				uint32_t t=h[i];
				o[(i<<2)+0]=(t>>24)&0xff;
				o[(i<<2)+1]=(t>>16)&0xff;
				o[(i<<2)+2]=(t>>8)&0xff;
				o[(i<<2)+3]=(t)&0xff;
			}
		}
	};
}

#endif // __INCLUDED_NET11_SHA1_HPP__
