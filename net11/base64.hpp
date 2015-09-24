#ifndef __INCLUDED_NET11_BASE64_H__
#define __INCLUDED_NET11_BASE64_H__

#pragma once

namespace net11 {
	static const char base64chars[65]=
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	static const signed char base64lookup[256]={
		// 0
		-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
		-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,62,-1,-1,-1,63, 52,53,54,55,56,57,58,59, 60,61,-1,-1,-1,-1,-1,-1,
		// 64
		-1, 0, 1, 2, 3, 4, 5, 6,  7, 8, 9,10,11,12,13,14, 15,16,17,18,19,20,21,22, 23,24,25,-1,-1,-1,-1,-1,
		-1,26,27,28,29,30,31,32, 33,34,35,36,37,38,39,40, 41,42,43,44,45,46,47,48, 49,50,51,-1,-1,-1,-1,-1,
		// 128
		-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
		-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
		// 192
		-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
		-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1
	};

	class base64enc {
		int bits;
		int count;
		char tmp[4];
	public:
		base64enc():bits(0),count(0) {}
		char* encode(char c) {
			bits=(bits<<8)|(c&0xff);
			count+=8;
			int i=0;
			while (count>=6) {
				count-=6;
				tmp[i++]=base64chars[(bits>>count)&0x3f];
			}
			tmp[i]=0;
			return tmp;
		}
		char* end() {
			int align=count;
			int i=0;
			while(align) {
				if (count) {
					tmp[i++]=base64chars[(bits<<(6-count))&0x3f];
					count=0;
				} else {
					tmp[i++]='=';
				}
				align=(align-6)&7;
			}
			tmp[i]=0;
			return tmp;
		}
	};

	class base64dec {
		int bits;
		int count;
	public:
		base64dec():bits(0),count(0) {}
		int decode(char i) {
			int lu=base64lookup[i&0xff];
			if (lu<0)
				return -1;
			bits=(bits<<6)|lu;
			count+=6;
			if (count<8)
				return -1;
			count-=8;
			return (bits>>count)&0xff;
		}
	};
};

#endif // __INCLUDED_NET11_BASE64_H__
